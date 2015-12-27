/*********************************************************
 * Copyright (C) 2007 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation version 2 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 *********************************************************/

/*
 * vmciQueuePair.h --
 *
 *    VMCI QueuePair API implementation in the host driver.
 */

#ifndef _VMCI_QUEUE_PAIR_H_
#define _VMCI_QUEUE_PAIR_H_

#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMMON
#define INCLUDE_ALLOW_VMCORE
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vmci_defs.h"
#ifndef VMKERNEL
#  include "circList.h"
#endif
#include "vmciContext.h"
#include "vmci_iocontrols.h"
#include "vmciHostKernelAPI.h"
#ifdef VMKERNEL
#include "vm_atomic.h"
#include "util_copy_dist.h"
#include "shm.h"

/*
 * In vmkernel, we are planning to provide two kinds of storage for
 * the queue pairs: one is shared memory and the other is using copies
 * of the queue pair stored in private guest memory. Currently, we
 * only support shared memory.
 */

typedef struct QueuePairPageStore {
   Bool shared; // Indicates whether the pages are stored in shared memory
   union {
      Shm_ID   shmID;
   } store;
} QueuePairPageStore;
#else
typedef struct QueuePairPageStore {
   Bool user;                  // Whether the page file strings are userspace pointers
   VA64 producePageFile;       // Name of the file
   VA64 consumePageFile;       // Name of the file
   uint64 producePageFileSize; // Size of the string
   uint64 consumePageFileSize; // Size of the string
   VA64 producePageUVA;        // User space VA of the mapped file in VMX
   VA64 consumePageUVA;        // User space VA of the mapped file in VMX
} QueuePairPageStore;
#endif // VMKERNEL

int QueuePair_Init(void);
void QueuePair_Exit(void);
void QueuePairList_Lock(void);
void QueuePairList_Unlock(void);
int QueuePair_Alloc(VMCIHandle handle, VMCIId peer, uint32 flags,
                    VMCIPrivilegeFlags privFlags,
                    uint64 produceSize, uint64 consumeSize,
		    QueuePairPageStore *pageStore,
                    VMCIContext *context);
int QueuePair_SetPageStore(VMCIHandle handle,
			   QueuePairPageStore *pageStore,
			   VMCIContext *context);
int QueuePair_Detach(VMCIHandle handle, VMCIContext *context, Bool detach);

#ifdef VMKERNEL
struct QueuePairEntry;


/* Non-public Queuepair API to be accessed from host context  */
int VMCIQueuePairAlloc(VMCIHandle *handle, VMCIQueue **produceQ,
                       uint64 produceSize, VMCIQueue **consumeQ,
                       uint64 consumeSize, VMCIId peer, uint32 flags);
int VMCIQueuePairAllocPriv(VMCIHandle *handle, VMCIQueue **produceQ,
                           uint64 produceSize, VMCIQueue **consumeQ,
                           uint64 consumeSize, VMCIId peer, uint32 flags,
                           VMCIPrivilegeFlags privFlags);
int VMCIQueuePairDetach(VMCIHandle handle);
int64 VMCIQueueFreeSpace(const VMCIQueue *produceQueue,
                         const VMCIQueue *consumeQueue,
                         const uint64 produceQSize);
int64 VMCIQueueBufReady(const VMCIQueue *consumeQueue,
                        const VMCIQueue *produceQueue,
                        const uint64 consumeQSize);
ssize_t VMCIQueueEnqueue(VMCIQueue *produceQueue, const VMCIQueue *consumeQueue,
                         const uint64 produceQSize, const void *buf,
                         size_t bufSize, Util_BufferType bufType);
ssize_t VMCIQueuePeek(VMCIQueue *produceQueue, const VMCIQueue *consumeQueue,
                      const uint64 consumeQSize, void *buf,
                      size_t bufSize, Util_BufferType bufType);
ssize_t VMCIQueuePeekV(VMCIQueue *produceQueue, const VMCIQueue *consumeQueue,
                       const uint64 consumeQSize, void *buf,
                       size_t bufSize, Util_BufferType bufType);
ssize_t VMCIQueueDiscard(VMCIQueue *produceQueue, const VMCIQueue *consumeQueue,
                         const uint64 consumeQSize, size_t bufSize);
ssize_t VMCIQueueDequeue(VMCIQueue *produceQueue, const VMCIQueue *consumeQueue,
                         const uint64 consumeQSize, void *buf,
                         size_t bufSize, Util_BufferType bufType);
ssize_t VMCIQueueEnqueueV(VMCIQueue *produceQueue, const VMCIQueue *consumeQueue,
                          const uint64 produceQSize, const void *buf,
                          size_t bufSize, Util_BufferType bufType);
ssize_t VMCIQueueDequeueV(VMCIQueue *produceQueue, const VMCIQueue *consumeQueue,
                          const uint64 consumeQSize, void *buf,
                          size_t bufSize, Util_BufferType bufType);

#endif	/* VMKERNEL  */

#if (defined(__linux__) || defined(_WIN32) || defined(__APPLE__)) && !defined(VMKERNEL)
struct VMCIQueue;

typedef struct PageStoreAttachInfo {
   char producePageFile[VMCI_PATH_MAX];
   char consumePageFile[VMCI_PATH_MAX];
   uint64 numProducePages;
   uint64 numConsumePages;

   /* User VAs in the VMX task */
   VA64   produceBuffer;
   VA64   consumeBuffer;

   /*
    * Platform-specific references to the physical pages backing the
    * queue. These include a page for the header.
    *
    * PR361589 tracks this, too.
    */

#if defined(__linux__)
# if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 0)
   struct kiobuf *produceIoBuf;
   struct kiobuf *consumeIoBuf;
# else
   struct page **producePages;
   struct page **consumePages;
# endif
#elif defined(_WIN32)
   void *kmallocPtr;
   size_t kmallocSize;
   PMDL produceMDL;
   PMDL consumeMDL;
#elif defined(__APPLE__)
   /*
    * All the Mac OS X fields are members of the VMCIQueue
    */
#endif
} PageStoreAttachInfo;

typedef enum VMCIDRequestStatus {
   VMCID_REQ_STATUS_NEW,        // Request is on the vmcidRequestQueue
   VMCID_REQ_STATUS_PENDING,    // Request is in userland and in vmcidPendingRequests
   VMCID_REQ_STATUS_HANDLED,    // Request has been fully processed
} VMCIDRequestStatus;

typedef struct VMCIDRequest {
   VMCIEvent    handledEvent;
   ListItem     listItem;

   int                  type;
   PageStoreAttachInfo *attachInfo;

   int                  status;
   int                  result;
} VMCIDRequest;

int VMCIHost_NewPageStore(struct PageStoreAttachInfo *attach);
int VMCIHost_FreePageStore(struct PageStoreAttachInfo *attach);
int VMCIHost_AttachToPageStore(struct PageStoreAttachInfo *attach,
                               struct VMCIQueue *produceQ,
			       struct VMCIQueue *consumeQ);
int VMCIHost_DetachFromPageStore(struct PageStoreAttachInfo *attach);

VMCIDRequest* VMCID_GetPendingRequest(uint64 reqid);

int VMCIHost_WaitForVMCIDRequest(VMCIDRpc *rpc);
int VMCIHost_HandleVMCIDResponse(VMCIDRpc *rpc);
#endif // (__linux__ || _WIN32 || __APPLE__) && !VMKERNEL

#endif /* !_VMCI_QUEUE_PAIR_H_ */
