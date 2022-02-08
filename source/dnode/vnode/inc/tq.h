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

#ifndef _TD_TQ_H_
#define _TD_TQ_H_

#include "common.h"
#include "executor.h"
#include "mallocator.h"
#include "meta.h"
#include "os.h"
#include "scheduler.h"
#include "taoserror.h"
#include "tlist.h"
#include "tmsg.h"
#include "trpc.h"
#include "ttimer.h"
#include "tutil.h"
#include "vnode.h"
#include "wal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TQ_BUFFER_SIZE 8

typedef struct STqRspHandle {
  void* handle;
  void* ahandle;
} STqRspHandle;

typedef enum { TQ_ITEM_READY, TQ_ITEM_PROCESS, TQ_ITEM_EMPTY } STqItemStatus;

typedef struct STqTaskItem {
  int8_t         status;
  int64_t        offset;
  void*          dst;
  qTaskInfo_t    task;
  STqReadHandle* pReadHandle;
  SSubQueryMsg*  pQueryMsg;
} STqTaskItem;

// new version
typedef struct STqBuffer {
  int64_t     firstOffset;
  int64_t     lastOffset;
  STqTaskItem output[TQ_BUFFER_SIZE];
} STqBuffer;

typedef struct STqTopicHandle {
  char            topicName[TSDB_TOPIC_FNAME_LEN];
  char*           sql;
  char*           logicalPlan;
  char*           physicalPlan;
  int64_t         committedOffset;
  int64_t         currentOffset;
  STqBuffer       buffer;
  SWalReadHandle* pReadhandle;
} STqTopicHandle;

typedef struct STqConsumerHandle {
  int64_t consumerId;
  int64_t epoch;
  char    cgroup[TSDB_TOPIC_FNAME_LEN];
  SArray* topics;  // SArray<STqClientTopic>
} STqConsumerHandle;

typedef struct STqMemRef {
  SMemAllocatorFactory* pAllocatorFactory;
  SMemAllocator*        pAllocator;
} STqMemRef;

typedef struct STqSerializedHead {
  int16_t ver;
  int16_t action;
  int32_t checksum;
  int64_t ssize;
  char    content[];
} STqSerializedHead;

typedef int (*FTqSerialize)(const void* pObj, STqSerializedHead** ppHead);
typedef const void* (*FTqDeserialize)(const STqSerializedHead* pHead, void** ppObj);
typedef void (*FTqDelete)(void*);

#define TQ_BUCKET_MASK 0xFF
#define TQ_BUCKET_SIZE 256

#define TQ_PAGE_SIZE 4096
// key + offset + size
#define TQ_IDX_SIZE 24
// 4096 / 24
#define TQ_MAX_IDX_ONE_PAGE 170
// 24 * 170
#define TQ_IDX_PAGE_BODY_SIZE 4080
// 4096 - 4080
#define TQ_IDX_PAGE_HEAD_SIZE 16

#define TQ_ACTION_CONST 0
#define TQ_ACTION_INUSE 1
#define TQ_ACTION_INUSE_CONT 2
#define TQ_ACTION_INTXN 3

#define TQ_SVER 0

// TODO: inplace mode is not implemented
#define TQ_UPDATE_INPLACE 0
#define TQ_UPDATE_APPEND 1

#define TQ_DUP_INTXN_REWRITE 0
#define TQ_DUP_INTXN_REJECT 2

static inline bool tqUpdateAppend(int32_t tqConfigFlag) { return tqConfigFlag & TQ_UPDATE_APPEND; }

static inline bool tqDupIntxnReject(int32_t tqConfigFlag) { return tqConfigFlag & TQ_DUP_INTXN_REJECT; }

static const int8_t TQ_CONST_DELETE = TQ_ACTION_CONST;

#define TQ_DELETE_TOKEN (void*)&TQ_CONST_DELETE

typedef struct STqMetaHandle {
  int64_t key;
  int64_t offset;
  int64_t serializedSize;
  void*   valueInUse;
  void*   valueInTxn;
} STqMetaHandle;

typedef struct STqMetaList {
  STqMetaHandle       handle;
  struct STqMetaList* next;
  // struct STqMetaList* inTxnPrev;
  // struct STqMetaList* inTxnNext;
  struct STqMetaList* unpersistPrev;
  struct STqMetaList* unpersistNext;
} STqMetaList;

typedef struct STqMetaStore {
  STqMetaList* bucket[TQ_BUCKET_SIZE];
  // a table head
  STqMetaList* unpersistHead;
  // topics that are not connectted
  STqMetaList* unconnectTopic;

  // TODO:temporaral use, to be replaced by unified tfile
  int fileFd;
  // TODO:temporaral use, to be replaced by unified tfile
  int idxFd;

  char*          dirPath;
  int32_t        tqConfigFlag;
  FTqSerialize   pSerializer;
  FTqDeserialize pDeserializer;
  FTqDelete      pDeleter;
} STqMetaStore;

typedef struct STQ {
  // the collection of groups
  // the handle of meta kvstore
  char*         path;
  STqCfg*       tqConfig;
  STqMemRef     tqMemRef;
  STqMetaStore* tqMeta;
  SWal*         pWal;
  SMeta*        pMeta;
} STQ;

typedef struct STqMgmt {
  int8_t inited;
  tmr_h  timer;
} STqMgmt;

static STqMgmt tqMgmt;

// init once
int  tqInit();
void tqCleanUp();

// open in each vnode
STQ* tqOpen(const char* path, SWal* pWal, SMeta* pMeta, STqCfg* tqConfig, SMemAllocatorFactory* allocFac);
void tqClose(STQ*);

// void* will be replace by a msg type
int tqPushMsg(STQ*, void* msg, int64_t version);
int tqCommit(STQ*);

int32_t tqProcessConsumeReq(STQ* pTq, SRpcMsg* pMsg);
int32_t tqProcessSetConnReq(STQ* pTq, char* msg);

#ifdef __cplusplus
}
#endif

#endif /*_TD_TQ_H_*/
