/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "dataSinkInt.h"
#include "dataSinkMgt.h"
#include "executorimpl.h"
#include "planner.h"
#include "tcompression.h"
#include "tdatablock.h"
#include "tglobal.h"
#include "tqueue.h"

extern SDataSinkStat gDataSinkStat;

typedef struct SSubmitRes {
  int64_t     affectedRows;
  int32_t     code;
  SSubmitRsp *pRsp;
} SSubmitRes;

typedef struct SDataInserterHandle {
  SDataSinkHandle     sink;
  SDataSinkManager*   pManager;
  STSchema*           pSchema;
  SQueryInserterNode* pNode;
  SSubmitRes          submitRes;
  SInserterParam*     pParam;
  SArray*             pDataBlocks;
  int32_t             status;
  bool                queryEnd;
  uint64_t            useconds;
  uint64_t            cachedSize;
  TdThreadMutex       mutex;
  tsem_t              ready;  
} SDataInserterHandle;

typedef struct SSubmitRspParam {
  SDataInserterHandle* pInserter;
} SSubmitRspParam;

int32_t inserterCallback(void* param, SDataBuf* pMsg, int32_t code) {
  SSubmitRspParam* pParam = (SSubmitRspParam*)param;
  SDataInserterHandle* pInserter = pParam->pInserter;

  pInserter->submitRes.code = code;
  
  if (code == TSDB_CODE_SUCCESS) {
    pInserter->submitRes.pRsp = taosMemoryCalloc(1, sizeof(SSubmitRsp));
    SDecoder    coder = {0};
    tDecoderInit(&coder, pMsg->pData, pMsg->len);
    code = tDecodeSSubmitRsp(&coder, pInserter->submitRes.pRsp);
    if (code) {
      tFreeSSubmitRsp(pInserter->submitRes.pRsp);
      pInserter->submitRes.code = code;
      goto _return;
    }
    
    if (pInserter->submitRes.pRsp->nBlocks > 0) {
      for (int32_t i = 0; i < pInserter->submitRes.pRsp->nBlocks; ++i) {
        SSubmitBlkRsp *blk = pInserter->submitRes.pRsp->pBlocks + i;
        if (TSDB_CODE_SUCCESS != blk->code) {
          code = blk->code;
          tFreeSSubmitRsp(pInserter->submitRes.pRsp);
          pInserter->submitRes.code = code;
          goto _return;
        }
      }
    }
    
    pInserter->submitRes.affectedRows += pInserter->submitRes.pRsp->affectedRows;
    qDebug("submit rsp received, affectedRows:%d, total:%d", pInserter->submitRes.pRsp->affectedRows, pInserter->submitRes.affectedRows);

    tFreeSSubmitRsp(pInserter->submitRes.pRsp);
  }

_return:

  tsem_post(&pInserter->ready);

  taosMemoryFree(param);
  
  return TSDB_CODE_SUCCESS;
}


static int32_t sendSubmitRequest(SDataInserterHandle* pInserter, SSubmitReq* pMsg, void* pTransporter, SEpSet* pEpset) {
  // send the fetch remote task result reques
  SMsgSendInfo* pMsgSendInfo = taosMemoryCalloc(1, sizeof(SMsgSendInfo));
  if (NULL == pMsgSendInfo) {
    taosMemoryFreeClear(pMsg);
    terrno = TSDB_CODE_QRY_OUT_OF_MEMORY;
    return terrno;
  }

  SSubmitRspParam* pParam = taosMemoryCalloc(1, sizeof(SSubmitRspParam));
  pParam->pInserter = pInserter;

  pMsgSendInfo->param = pParam;
  pMsgSendInfo->msgInfo.pData = pMsg;
  pMsgSendInfo->msgInfo.len = sizeof(SSubmitReq);
  pMsgSendInfo->msgType = TDMT_VND_SUBMIT;
  pMsgSendInfo->fp = inserterCallback;

  int64_t transporterId = 0;
  return asyncSendMsgToServer(pTransporter, pEpset, &transporterId, pMsgSendInfo);
}


SSubmitReq* dataBlockToSubmit(SDataInserterHandle* pInserter) {
  const SArray* pBlocks = pInserter->pDataBlocks;
  const STSchema* pTSchema = pInserter->pSchema; 
  int64_t uid = pInserter->pNode->tableId; 
  int64_t suid = pInserter->pNode->stableId; 
  int32_t vgId = pInserter->pNode->vgId;

  SSubmitReq* ret = NULL;
  int32_t sz = taosArrayGetSize(pBlocks);

  // cal size
  int32_t cap = sizeof(SSubmitReq);
  for (int32_t i = 0; i < sz; i++) {
    SSDataBlock* pDataBlock = taosArrayGet(pBlocks, i);
    int32_t      rows = pDataBlock->info.rows;
    // TODO min
    int32_t rowSize = pDataBlock->info.rowSize;
    int32_t maxLen = TD_ROW_MAX_BYTES_FROM_SCHEMA(pTSchema);

    cap += sizeof(SSubmitBlk) + rows * maxLen;
  }

  // assign data
  // TODO
  ret = rpcMallocCont(cap);
  ret->header.vgId = vgId;
  ret->version = htonl(1);
  ret->length = sizeof(SSubmitReq);
  ret->numOfBlocks = htonl(sz);

  SSubmitBlk* blkHead = POINTER_SHIFT(ret, sizeof(SSubmitReq));
  for (int32_t i = 0; i < sz; i++) {
    SSDataBlock* pDataBlock = taosArrayGet(pBlocks, i);

    blkHead->numOfRows = htons(pDataBlock->info.rows);
    blkHead->sversion = htonl(pTSchema->version);
    // TODO
    blkHead->suid = htobe64(suid);
    blkHead->uid = htobe64(uid);
    blkHead->schemaLen = htonl(0);

    int32_t rows = pDataBlock->info.rows;
    int32_t dataLen = 0;
    STSRow* rowData = POINTER_SHIFT(blkHead, sizeof(SSubmitBlk));
    for (int32_t j = 0; j < rows; j++) {
      SRowBuilder rb = {0};
      tdSRowInit(&rb, pTSchema->version);
      tdSRowSetTpInfo(&rb, pTSchema->numOfCols, pTSchema->flen);
      tdSRowResetBuf(&rb, rowData);

      for (int32_t k = 0; k < pTSchema->numOfCols; k++) {
        const STColumn*  pColumn = &pTSchema->columns[k];
        SColumnInfoData* pColData = taosArrayGet(pDataBlock->pDataBlock, k);
        if (colDataIsNull_s(pColData, j)) {
          tdAppendColValToRow(&rb, pColumn->colId, pColumn->type, TD_VTYPE_NONE, NULL, false, pColumn->offset, k);
        } else {
          void* data = colDataGetData(pColData, j);
          tdAppendColValToRow(&rb, pColumn->colId, pColumn->type, TD_VTYPE_NORM, data, true, pColumn->offset, k);
        }
      }
      int32_t rowLen = TD_ROW_LEN(rowData);
      rowData = POINTER_SHIFT(rowData, rowLen);
      dataLen += rowLen;
    }
    
    blkHead->dataLen = htonl(dataLen);

    ret->length += sizeof(SSubmitBlk) + dataLen;
    blkHead = POINTER_SHIFT(blkHead, sizeof(SSubmitBlk) + dataLen);
  }

  ret->length = htonl(ret->length);

  return ret;
}


static int32_t putDataBlock(SDataSinkHandle* pHandle, const SInputData* pInput, bool* pContinue) {
  SDataInserterHandle* pInserter = (SDataInserterHandle*)pHandle;
  taosArrayPush(pInserter->pDataBlocks, pInput->pData);
  SSubmitReq* pMsg = dataBlockToSubmit(pInserter);

  int32_t code = sendSubmitRequest(pInserter, pMsg, pInserter->pParam->readHandle->pMsgCb->clientRpc, &pInserter->pNode->epSet);
  if (code) {
    return code;
  }

  tsem_wait(&pInserter->ready);

  if (pInserter->submitRes.code) {
    return pInserter->submitRes.code;
  }

  *pContinue = true;
  
  return TSDB_CODE_SUCCESS;
}

static void endPut(struct SDataSinkHandle* pHandle, uint64_t useconds) {
  SDataInserterHandle* pInserter = (SDataInserterHandle*)pHandle;
  taosThreadMutexLock(&pInserter->mutex);
  pInserter->queryEnd = true;
  pInserter->useconds = useconds;
  taosThreadMutexUnlock(&pInserter->mutex);
}

static void getDataLength(SDataSinkHandle* pHandle, int64_t* pLen, bool* pQueryEnd) {
  SDataInserterHandle* pDispatcher = (SDataInserterHandle*)pHandle;
  *pLen = pDispatcher->submitRes.affectedRows;
  qDebug("got total affectedRows %" PRId64 , *pLen);
}


static int32_t destroyDataSinker(SDataSinkHandle* pHandle) {
  SDataInserterHandle* pInserter = (SDataInserterHandle*)pHandle;
  atomic_sub_fetch_64(&gDataSinkStat.cachedSize, pInserter->cachedSize);
  taosArrayDestroy(pInserter->pDataBlocks);
  taosMemoryFree(pInserter->pSchema);
  taosThreadMutexDestroy(&pInserter->mutex);
  return TSDB_CODE_SUCCESS;
}

static int32_t getCacheSize(struct SDataSinkHandle* pHandle, uint64_t* size) {
  SDataInserterHandle* pDispatcher = (SDataInserterHandle*)pHandle;

  *size = atomic_load_64(&pDispatcher->cachedSize);
  return TSDB_CODE_SUCCESS;
}

int32_t createDataInserter(SDataSinkManager* pManager, const SDataSinkNode* pDataSink, DataSinkHandle* pHandle, void *pParam) {
  SDataInserterHandle* inserter = taosMemoryCalloc(1, sizeof(SDataInserterHandle));
  if (NULL == inserter) {
    terrno = TSDB_CODE_QRY_OUT_OF_MEMORY;
    return TSDB_CODE_QRY_OUT_OF_MEMORY;
  }

  SQueryInserterNode* pInserterNode = (SQueryInserterNode *)pDataSink;
  inserter->sink.fPut = putDataBlock;
  inserter->sink.fEndPut = endPut;
  inserter->sink.fGetLen = getDataLength;
  inserter->sink.fGetData = NULL;
  inserter->sink.fDestroy = destroyDataSinker;
  inserter->sink.fGetCacheSize = getCacheSize;
  inserter->pManager = pManager;
  inserter->pNode = pInserterNode;
  inserter->pParam = pParam;
  inserter->status = DS_BUF_EMPTY;
  inserter->queryEnd = false;

  int64_t suid = 0;
  int32_t code = tsdbGetTableSchema(inserter->pParam->readHandle->vnode, pInserterNode->tableId, &inserter->pSchema, &suid);
  if (code) {
    return code;
  }

  if (pInserterNode->stableId != suid) {
    terrno = TSDB_CODE_TDB_INVALID_TABLE_ID;
    return terrno;
  }

  inserter->pDataBlocks = taosArrayInit(1, POINTER_BYTES);
  taosThreadMutexInit(&inserter->mutex, NULL);
  if (NULL == inserter->pDataBlocks) {
    terrno = TSDB_CODE_QRY_OUT_OF_MEMORY;
    return TSDB_CODE_QRY_OUT_OF_MEMORY;
  }

  tsem_init(&inserter->ready, 0, 0);
  
  *pHandle = inserter;
  return TSDB_CODE_SUCCESS;
}
