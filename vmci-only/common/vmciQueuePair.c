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
 * vmciQueuePair.c --
 *
 *    VMCI QueuePair API implementation in the host driver.
 */

/* Must come before any kernel header file. */
#if defined(__linux__) && !defined(VMKERNEL)
#  define EXPORT_SYMTAB
#  include "driver-config.h"
#  include "compat_module.h"
#endif

#include "vmci_kernel_if.h"
#include "vm_assert.h"
#include "vmci_handle_array.h"
#include "vmciQueuePair.h"
#if !defined(VMKERNEL)
#  include "vmci_queue_pair.h"
#  include "vmciResource.h"
#endif
#include "vmciDriver.h"
#include "vmciContext.h"
#include "vmciDatagram.h"
#include "vmciCommonInt.h"
#include "circList.h"

#define LGPFX "VMCIQueuePair: "

typedef struct QueueInfo {
   uint64 size;
#ifndef VMKERNEL
   char   pageFile[VMCI_PATH_MAX];
#endif // !VMKERNEL
} QueueInfo;


/*
 * The context that creates the QueuePair becomes producer of produce queue,
 * and consumer of consume queue. The context on other end for the QueuePair
 * has roles reversed for these two queues.
 */

typedef struct QueuePairEntry {
   VMCIHandle         handle;
   VMCIId             peer;
   uint32             flags;
   QueueInfo          produceInfo;
   QueueInfo          consumeInfo;
   VMCIId             createId;
   VMCIId             attachId;
   uint32             refCount;
   Bool               pageStoreSet;
   Bool               allowAttach;
   ListItem           listItem;
   Bool               requireTrustedAttach;
   Bool               createdByTrusted;
#ifdef VMKERNEL
   QueuePairPageStore store;
#elif defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
   /*
    * Always created but only used if a host endpoint attaches to this
    * queue.
    */

   VMCIQueue           *produceQ;
   VMCIQueue           *consumeQ;
   PageStoreAttachInfo *attachInfo;
#endif
} QueuePairEntry;

#ifdef VMKERNEL
typedef VMCILock VMCIQPLock;
# define VMCIQPLock_Init(_l, _r) VMCI_InitLock(_l, "VMCIQPLock",        \
                                               VMCI_LOCK_RANK_HIGH); \
                                 _r = VMCI_SUCCESS
# define VMCIQPLock_Destroy(_l)  VMCI_CleanupLock(_l)
# define VMCIQPLock_Acquire(_l)  VMCI_GrabLock(_l, NULL)
# define VMCIQPLock_Release(_l)  VMCI_ReleaseLock(_l, 0)
#else
typedef VMCIMutex VMCIQPLock;
# define VMCIQPLock_Init(_l, _r) _r = VMCIMutex_Init(_l)
# define VMCIQPLock_Destroy(_l)  VMCIMutex_Destroy(_l)
# define VMCIQPLock_Acquire(_l)  VMCIMutex_Acquire(_l)
# define VMCIQPLock_Release(_l)  VMCIMutex_Release(_l)
#endif

typedef struct QueuePairList {
   ListItem  *head;
   VMCIQPLock lock;
} QueuePairList;

static QueuePairList queuePairList;

static int QueuePairAllocHost(VMCIHandle handle, VMCIId peer,
                              uint32 flags, VMCIPrivilegeFlags privFlags,
                              uint64 produceSize,
                              uint64 consumeSize,
                              QueuePairPageStore *pageStore,
                              VMCIContext *context,
                              QueuePairEntry **ent);
static QueuePairEntry *QueuePairList_FindEntry(VMCIHandle handle);
static void QueuePairList_AddEntry(QueuePairEntry *entry);
static void QueuePairList_RemoveEntry(QueuePairEntry *entry);
static QueuePairEntry *QueuePairList_GetHead(void);
static int QueuePairNotifyPeer(Bool attach, VMCIHandle handle, VMCIId myId,
                               VMCIId peerId);


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairList_Init --
 *
 *      Initializes the list of QueuePairs.
 *
 * Results:
 *      Success or failure.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE int
QueuePairList_Init(void)
{
   int ret;

   memset(&queuePairList, 0, sizeof queuePairList);
   VMCIQPLock_Init(&queuePairList.lock, ret);

   return ret;
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairList_Exit --
 *
 *      Destroy the list's lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
QueuePairList_Exit(void)
{
   VMCIQPLock_Destroy(&queuePairList.lock);
   memset(&queuePairList, 0, sizeof queuePairList);
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairList_Lock --
 *
 *      Acquires the lock protecting the QueuePair list.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
QueuePairList_Lock(void)
{
   VMCIQPLock_Acquire(&queuePairList.lock);
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairList_Unlock --
 *
 *      Releases the lock protecting the QueuePair list.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
QueuePairList_Unlock(void)
{
   VMCIQPLock_Release(&queuePairList.lock);
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairList_FindEntry --
 *
 *      Finds the entry in the list corresponding to a given handle. Assumes
 *      that the list is locked.
 *
 * Results:
 *      Pointer to entry.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static QueuePairEntry *
QueuePairList_FindEntry(VMCIHandle handle) // IN:
{
   ListItem *next;

   ASSERT(!VMCI_HANDLE_INVALID(handle));
   LIST_SCAN(next, queuePairList.head) {
      QueuePairEntry *entry = LIST_CONTAINER(next, QueuePairEntry, listItem);

      if (VMCI_HANDLE_EQUAL(entry->handle, handle)) {
         return entry;
      }
   }

   return NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairList_AddEntry --
 *
 *      Adds the given entry to the list. Assumes that the list is locked.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static void
QueuePairList_AddEntry(QueuePairEntry *entry) // IN:
{
   if (entry) {
      LIST_QUEUE(&entry->listItem, &queuePairList.head);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairList_RemoveEntry --
 *
 *      Removes the given entry from the list. Assumes that the list is locked.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static void
QueuePairList_RemoveEntry(QueuePairEntry *entry) // IN:
{
   if (entry) {
      LIST_DEL(&entry->listItem, &queuePairList.head);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairList_GetHead --
 *
 *      Returns the entry from the head of the list. Assumes that the list is
 *      locked.
 *
 * Results:
 *      Pointer to entry.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static QueuePairEntry *
QueuePairList_GetHead(void)
{
   ListItem *first = LIST_FIRST(queuePairList.head);

   if (first) {
      QueuePairEntry *entry = LIST_CONTAINER(first, QueuePairEntry, listItem);
      return entry;
   }

   return NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairDenyConnection --
 *
 *      On ESX we check if the domain names of the two contexts match.
 *      Otherwise we deny the connection.  We always allow the connection on
 *      hosted.
 *
 * Results:
 *      Boolean result.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE Bool
QueuePairDenyConnection(VMCIId contextId, // IN:  Unused on hosted
                        VMCIId peerId)    // IN:  Unused on hosted
{
#ifndef VMKERNEL
   return FALSE; /* Allow on hosted. */
#else
   char contextDomain[VMCI_DOMAIN_NAME_MAXLEN];
   char peerDomain[VMCI_DOMAIN_NAME_MAXLEN];

   ASSERT(contextId != VMCI_INVALID_ID);
   if (peerId == VMCI_INVALID_ID) {
      return FALSE; /* Allow. */
   }
   if (VMCIContext_GetDomainName(contextId, contextDomain,
                                 sizeof contextDomain) != VMCI_SUCCESS) {
      return TRUE; /* Deny. */
   }
   if (VMCIContext_GetDomainName(peerId, peerDomain, sizeof peerDomain) !=
       VMCI_SUCCESS) {
      return TRUE; /* Deny. */
   }
   return strcmp(contextDomain, peerDomain) ? TRUE : /* Deny. */
                                              FALSE; /* Allow. */
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePair_Init --
 *
 *      Initalizes QueuePair state in the host driver.
 *
 * Results:
 *      Success or failure.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

int
QueuePair_Init(void)
{
   return QueuePairList_Init();
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePair_Exit --
 *
 *      Destroys QueuePair state in the host driver.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
QueuePair_Exit(void)
{
   QueuePairEntry *entry;

   QueuePairList_Lock();

   while ((entry = QueuePairList_GetHead())) {
      QueuePairList_RemoveEntry(entry);
      VMCI_FreeKernelMem(entry, sizeof *entry);
   }
   
   QueuePairList_Unlock();
   QueuePairList_Exit();
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePair_Alloc --
 *
 *      Does all the work for the QueuePairAlloc host driver call. Allocates a
 *      QueuePair entry if one does not exist. Attaches to one if it exists,
 *      and retrieves the page files backing that QueuePair.  Assumes that the
 *      QP list lock is held.
 *
 * Results:
 *      Success or failure.
 *
 * Side effects:
 *      Memory may be allocated.
 *
 *-----------------------------------------------------------------------------
 */

int
QueuePair_Alloc(VMCIHandle handle,             // IN:
                VMCIId peer,                   // IN:
                uint32 flags,                  // IN:
                VMCIPrivilegeFlags privFlags,  // IN:
                uint64 produceSize,            // IN:
                uint64 consumeSize,            // IN:
                QueuePairPageStore *pageStore, // IN/OUT
                VMCIContext *context)          // IN: Caller
{
   return QueuePairAllocHost(handle, peer, flags, privFlags,
                             produceSize, consumeSize,
                             pageStore, context, NULL);
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairAllocHost --
 *
 *      QueuePair_Alloc for use when setting up queue pair endpoints
 *      on the host. Like QueuePair_Alloc, but returns a pointer to
 *      the QueuePairEntry on success.
 *
 * Results:
 *      Success or failure.
 *
 * Side effects:
 *      Memory may be allocated.
 *
 *-----------------------------------------------------------------------------
 */

static int
QueuePairAllocHost(VMCIHandle handle,             // IN:
                   VMCIId peer,                   // IN:
                   uint32 flags,                  // IN:
                   VMCIPrivilegeFlags privFlags,  // IN:
                   uint64 produceSize,            // IN:
                   uint64 consumeSize,            // IN:
                   QueuePairPageStore *pageStore, // IN/OUT
                   VMCIContext *context,          // IN: Caller
                   QueuePairEntry **ent)          // OUT:
{
   QueuePairEntry *entry = NULL;
   int result;
   const VMCIId contextId = VMCIContext_GetId(context);

   if (VMCI_HANDLE_INVALID(handle) ||
       (flags & ~VMCI_QP_ALL_FLAGS) ||
       (flags & VMCI_QPFLAG_LOCAL) ||
       !(produceSize || consumeSize) ||
       !context || contextId == VMCI_INVALID_ID ||
       handle.context == VMCI_INVALID_ID) {
      return VMCI_ERROR_INVALID_ARGS;
   }

#ifdef VMKERNEL
   if (!pageStore || !pageStore->shared) {
      return VMCI_ERROR_INVALID_ARGS;
   }
#else
   /*
    * On hosted, pageStore can be NULL if the caller doesn't want the
    * information
    */
   if (pageStore &&
       (!pageStore->producePageFile ||
	!pageStore->consumePageFile ||
	!pageStore->producePageFileSize ||
	!pageStore->consumePageFileSize)) {
      return VMCI_ERROR_INVALID_ARGS;
   }
#endif // VMKERNEL

   if (VMCIHandleArray_HasEntry(context->queuePairArray, handle)) {
      VMCILOG((LGPFX"Context %u already attached to queue pair 0x%x:0x%x.\n",
               contextId, handle.context, handle.resource));
      result = VMCI_ERROR_ALREADY_EXISTS;
      goto out;
   }

   entry = QueuePairList_FindEntry(handle);
   if (!entry) { /* Create case. */
      /*
       * Do not create if the caller asked not to.
       */

      if (flags & VMCI_QPFLAG_ATTACH_ONLY) {
         result = VMCI_ERROR_NOT_FOUND;
         goto out;
      }

      /*
       * Creator's context ID should match handle's context ID or the creator
       * must allow the context in handle's context ID as the "peer".
       */

      if (handle.context != contextId && handle.context != peer) {
         result = VMCI_ERROR_NO_ACCESS;
         goto out;
      }

      /*
       * Check if we should allow this QueuePair connection.
       */

      if (QueuePairDenyConnection(contextId, peer)) {
         result = VMCI_ERROR_NO_ACCESS;
         goto out;
      }

      entry = VMCI_AllocKernelMem(sizeof *entry, VMCI_MEMORY_ATOMIC);
      if (!entry) {
         result = VMCI_ERROR_NO_MEM;
         goto out;
      }

      memset(entry, 0, sizeof *entry);
      entry->handle = handle;
      entry->peer = peer;
      entry->flags = flags;
      entry->createId = contextId;
      entry->attachId = VMCI_INVALID_ID;
      entry->produceInfo.size = produceSize;
      entry->consumeInfo.size = consumeSize;
      entry->refCount = 1;
      entry->pageStoreSet = FALSE;
      entry->allowAttach = TRUE;
      entry->requireTrustedAttach =
         (context->privFlags & VMCI_PRIVILEGE_FLAG_RESTRICTED) ? TRUE : FALSE;
      entry->createdByTrusted =
         (privFlags & VMCI_PRIVILEGE_FLAG_TRUSTED) ? TRUE : FALSE;

#ifndef VMKERNEL
      {
         uint64 numProducePages;
         uint64 numConsumePages;

         entry->produceQ = VMCI_AllocKernelMem(sizeof *entry->produceQ,
                                               VMCI_MEMORY_NORMAL);
         if (entry->produceQ == NULL) {
            result = VMCI_ERROR_NO_MEM;
            goto errorDealloc;
         }
         memset(entry->produceQ, 0, sizeof *entry->produceQ);

         entry->consumeQ = VMCI_AllocKernelMem(sizeof *entry->consumeQ,
                                               VMCI_MEMORY_NORMAL);
         if (entry->consumeQ == NULL) {
            result = VMCI_ERROR_NO_MEM;
            goto errorDealloc;
         }
         memset(entry->consumeQ, 0, sizeof *entry->consumeQ);

         entry->attachInfo = VMCI_AllocKernelMem(sizeof *entry->attachInfo,
                                                 VMCI_MEMORY_NORMAL);
         if (entry->attachInfo == NULL) {
            result = VMCI_ERROR_NO_MEM;
            goto errorDealloc;
         }
         memset(entry->attachInfo, 0, sizeof *entry->attachInfo);

#if defined _WIN32
         entry->produceQ->mutex = &entry->produceQ->__mutex;
         entry->consumeQ->mutex = &entry->produceQ->__mutex;

         ExInitializeFastMutex(entry->produceQ->mutex);
#endif /* Windows Host */

         numProducePages = CEILING(produceSize, PAGE_SIZE) + 1;
         numConsumePages = CEILING(consumeSize, PAGE_SIZE) + 1;

         entry->attachInfo->numProducePages = numProducePages;
         entry->attachInfo->numConsumePages = numConsumePages;
      }
#endif /* !VMKERNEL */

#ifdef VMKERNEL
      ASSERT_NOT_IMPLEMENTED(pageStore->shared);
#endif
      INIT_LIST_ITEM(&entry->listItem);

      QueuePairList_AddEntry(entry);
      result = VMCI_SUCCESS_QUEUEPAIR_CREATE;
   } else { /* Attach case. */
      /* Check for failure conditions. */
      if (contextId == entry->createId || contextId == entry->attachId) {
         result = VMCI_ERROR_ALREADY_EXISTS;
         goto out;
      }

      /*
       * QueuePairs are create/destroy entities.  There's no notion of
       * disconnecting/re-attaching.
       */

      if (!entry->allowAttach) {
         result = VMCI_ERROR_UNAVAILABLE;
         goto out;
      }
      ASSERT(entry->refCount < 2);
      ASSERT(entry->attachId == VMCI_INVALID_ID);

      /*
       * If we are attaching from a restricted context then the queuepair
       * must have been created by a trusted endpoint.
       */

      if (context->privFlags & VMCI_PRIVILEGE_FLAG_RESTRICTED) {
         if (!entry->createdByTrusted) {
            result = VMCI_ERROR_NO_ACCESS;
            goto out;
         }
      }

      /*
       * If we are attaching to a queuepair that was created by a restricted
       * context then we must be trusted.
       */

      if (entry->requireTrustedAttach) {
         if (!(privFlags & VMCI_PRIVILEGE_FLAG_TRUSTED)) {
            result = VMCI_ERROR_NO_ACCESS;
            goto out;
         }
      }

      /*
       * If the creator specifies VMCI_INVALID_ID in "peer" field, access
       * control check is not performed.
       */

      if (entry->peer != VMCI_INVALID_ID && entry->peer != contextId) {
         result = VMCI_ERROR_NO_ACCESS;
         goto out;
      }

#ifndef VMKERNEL
      /*
       * VMKernel doesn't need to check the capabilities because the
       * whole system is installed as the kernel and matching VMX.
       */

      if (entry->createId == VMCI_HOST_CONTEXT_ID) {
         /*
          * Do not attach if the caller doesn't support Host Queue Pairs
          * and a host created this queue pair.
          */

         if (!VMCIContext_SupportsHostQP(context)) {
            result = VMCI_ERROR_INVALID_RESOURCE;
            goto out;
         }
      } else if (contextId == VMCI_HOST_CONTEXT_ID) {
         VMCIContext *createContext;
         Bool supportsHostQP;

         /*
          * Do not attach a host to a user created QP if that user
          * doesn't support Host QP end points.
          */

         createContext = VMCIContext_Get(entry->createId);
         supportsHostQP = VMCIContext_SupportsHostQP(createContext);
         VMCIContext_Release(createContext);

         if (!supportsHostQP) {
            result = VMCI_ERROR_INVALID_RESOURCE;
            goto out;
         }
      }
#endif // !VMKERNEL

      if (entry->produceInfo.size != consumeSize ||
          entry->consumeInfo.size != produceSize ||
          entry->flags != (flags & ~VMCI_QPFLAG_ATTACH_ONLY)) {
         result = VMCI_ERROR_QUEUEPAIR_MISMATCH;
         goto out;
      }

      /*
       * On VMKERNEL (e.g., ESX) we don't allow an attach until
       * the page store information has been set.
       *
       * However, on hosted products we support an attach to a
       * QueuePair that hasn't had its page store established yet.  In
       * fact, that's how a VMX guest will approach a host-created
       * QueuePair.  After the VMX guest does the attach, VMX will
       * receive the CREATE status code to indicate that it should
       * create the page files for the QueuePair contents.  It will
       * then issue a separate call down to set the page store.  That
       * will complete the attach case.
       */
      if (vmkernel && !entry->pageStoreSet) {
         result = VMCI_ERROR_QUEUEPAIR_NOTSET;
         goto out;
      }

      /*
       * Check if we should allow this QueuePair connection.
       */

      if (QueuePairDenyConnection(contextId, entry->createId)) {
         result = VMCI_ERROR_NO_ACCESS;
         goto out;
      }

#ifdef VMKERNEL
      ASSERT_NOT_IMPLEMENTED(entry->store.shared);
      pageStore->store.shmID = entry->store.store.shmID;
#else
      if (pageStore && entry->pageStoreSet) {
	 ASSERT(entry->produceInfo.pageFile[0] && entry->consumeInfo.pageFile[0]);
	 if (pageStore->producePageFileSize < sizeof entry->consumeInfo.pageFile) {
	    result = VMCI_ERROR_NO_MEM;
	    goto out;
	 }
	 if (pageStore->consumePageFileSize < sizeof entry->produceInfo.pageFile) {
	    result = VMCI_ERROR_NO_MEM;
	    goto out;
	 }

	 if (pageStore->user) {
	    if (VMCI_CopyToUser(pageStore->producePageFile,
				entry->consumeInfo.pageFile,
				sizeof entry->consumeInfo.pageFile)) {
	       result = VMCI_ERROR_GENERIC;
	       goto out;
	    }

	    if (VMCI_CopyToUser(pageStore->consumePageFile,
				entry->produceInfo.pageFile,
				sizeof entry->produceInfo.pageFile)) {
	       result = VMCI_ERROR_GENERIC;
	       goto out;
	    }
	 } else {
	    memcpy(VMCIVA64ToPtr(pageStore->producePageFile),
		   entry->consumeInfo.pageFile,
		   sizeof entry->consumeInfo.pageFile);
	    memcpy(VMCIVA64ToPtr(pageStore->consumePageFile),
		   entry->produceInfo.pageFile,
		   sizeof entry->produceInfo.pageFile);
	 }
      }
#endif // VMKERNEL

      /*
       * We only send notification if the other end of the QueuePair
       * is not the host (in hosted products).  In the case that a
       * host created the QueuePair, we'll send notification when the
       * guest issues the SetPageStore() (see next function).  The
       * reason is that the host can't use the QueuePair until the
       * SetPageStore() is complete.
       *
       * Note that in ESX we always send the notification now
       * because the host can begin to enqueue immediately.
       */

      if (vmkernel || entry->createId != VMCI_HOST_CONTEXT_ID) {
         result = QueuePairNotifyPeer(TRUE, handle, contextId, entry->createId);
         if (result < VMCI_SUCCESS) {
            goto out;
         }
      }

      entry->attachId = contextId;
      entry->refCount++;
      entry->allowAttach = FALSE;

      /*
       * Default response to an attach is _ATTACH.  However, if a host
       * created the QueuePair then we're a guest (because
       * host-to-host isn't supported).  And thus, the guest's VMX
       * needs to create the backing for the port.  So, we send up a
       * _CREATE response.
       */

      if (!vmkernel && entry->createId == VMCI_HOST_CONTEXT_ID) {
	 result = VMCI_SUCCESS_QUEUEPAIR_CREATE;
      } else {
         result = VMCI_SUCCESS_QUEUEPAIR_ATTACH;
      }
   }

#ifndef VMKERNEL
   goto out;

   /*
    * Cleanup is only necessary on hosted
    */

errorDealloc:
   if (entry->produceQ != NULL) {
      VMCI_FreeKernelMem(entry->produceQ, sizeof *entry->produceQ);
   }
   if (entry->consumeQ != NULL) {
      VMCI_FreeKernelMem(entry->consumeQ, sizeof *entry->consumeQ);
   }
   if (entry->attachInfo != NULL) {
      VMCI_FreeKernelMem(entry->attachInfo, sizeof *entry->attachInfo);
   }
   VMCI_FreeKernelMem(entry, sizeof *entry);
#endif // !VMKERNEL

out:
   if (result >= VMCI_SUCCESS) {
      ASSERT(entry);
      if (ent != NULL) {
         *ent = entry;
      }
      VMCIHandleArray_AppendEntry(&context->queuePairArray, handle);
   }
   return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePair_SetPageStore --
 *
 *      The creator of a QueuePair uses this to set the page file names for a
 *      given QueuePair.  Assumes that the QP list lock is held.
 *
 *      Note now that sometimes the client that attaches to a
 *      QueuePair will set the page file.  This happens on hosted
 *      products because the host doesn't have a mechanism for
 *      creating the backing memory for queue contents.  ESX does and
 *      so this is a moot point there.  For example, note that in
 *      QueuePairAllocHost() an attaching guest receives the _CREATE
 *      result code (instead of _ATTACH) on hosted products only, not
 *      on VMKERNEL.
 *
 *      As a result, this routine now always creates the host information even
 *      if the QueuePair is only used by guests.  At the time a guest creates
 *      a QueuePair it doesn't know if a host or guest will attach.  So, the
 *      host information always has to be created.
 *
 * Results:
 *      Success or failure.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

int
QueuePair_SetPageStore(VMCIHandle handle,             // IN:
		       QueuePairPageStore *pageStore, // IN:
		       VMCIContext *context)          // IN: Caller
{
   QueuePairEntry *entry;
   int result;
   const VMCIId contextId = VMCIContext_GetId(context);
#ifndef VMKERNEL
   QueuePairPageStore normalizedPageStore;
#endif

   if (VMCI_HANDLE_INVALID(handle) || !pageStore ||
#ifdef VMKERNEL
       (pageStore->shared && pageStore->store.shmID == SHM_INVALID_ID) ||
#else
       !pageStore->producePageFile || !pageStore->consumePageFile ||
       !pageStore->producePageFileSize || !pageStore->consumePageFile ||
#endif // VMKERNEL
       !context || contextId == VMCI_INVALID_ID) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   if (!VMCIHandleArray_HasEntry(context->queuePairArray, handle)) {
      VMCILOG((LGPFX"Context %u not attached to queue pair 0x%x:0x%x.\n",
               contextId, handle.context, handle.resource));
      result = VMCI_ERROR_NOT_FOUND;
      goto out;
   }

#ifndef VMKERNEL
   /*
    * If the client supports Host QueuePairs then it must provide the
    * UVA's of the mmap()'d files backing the QueuePairs.
    */

   if (VMCIContext_SupportsHostQP(context) &&
       (pageStore->producePageUVA == 0 ||
        pageStore->consumePageUVA == 0)) {
      return VMCI_ERROR_INVALID_ARGS;
   }
#endif // !VMKERNEL

   entry = QueuePairList_FindEntry(handle);
   if (!entry) {
      result = VMCI_ERROR_NOT_FOUND;
      goto out;
   }

   /*
    * If I'm the owner then I can set the page store.
    *
    * Or, if a host created the QueuePair and I'm the attached peer
    * then I can set the page store.
    */

   if (entry->createId != contextId &&
       (entry->createId != VMCI_HOST_CONTEXT_ID ||
	entry->attachId != contextId)) {
      result = VMCI_ERROR_QUEUEPAIR_NOTOWNER;
      goto out;
   }
   if (entry->pageStoreSet) {
      result = VMCI_ERROR_UNAVAILABLE;
      goto out;
   }
#ifdef VMKERNEL
   entry->store = *pageStore;
#else
   /*
    * Normalize the page store information from the point of view of
    * the VMX process with respect to the QueuePair.  If VMX has
    * attached to a host-created QueuePair and is passing down
    * PageStore information then we must switch the produce/consume
    * queue information before applying it to the QueuePair.
    *
    * In other words, the QueuePair structure (entry->state) is
    * oriented with respect to the host that created it.  However, VMX
    * is sending down information relative to its view of the world
    * which is opposite of the host's.
    */

   if (entry->createId == contextId) {
      normalizedPageStore.producePageFile = pageStore->producePageFile;
      normalizedPageStore.consumePageFile = pageStore->consumePageFile;
      normalizedPageStore.producePageFileSize = pageStore->producePageFileSize;
      normalizedPageStore.consumePageFileSize = pageStore->consumePageFileSize;
      normalizedPageStore.producePageUVA = pageStore->producePageUVA;
      normalizedPageStore.consumePageUVA = pageStore->consumePageUVA;
   } else {
      normalizedPageStore.producePageFile = pageStore->consumePageFile;
      normalizedPageStore.consumePageFile = pageStore->producePageFile;
      normalizedPageStore.producePageFileSize = pageStore->consumePageFileSize;
      normalizedPageStore.consumePageFileSize = pageStore->producePageFileSize;
      normalizedPageStore.producePageUVA = pageStore->consumePageUVA;
      normalizedPageStore.consumePageUVA = pageStore->producePageUVA;
   }

   if (normalizedPageStore.producePageFileSize > sizeof entry->produceInfo.pageFile) {
      result = VMCI_ERROR_NO_MEM;
       goto out;
   }
   if (normalizedPageStore.consumePageFileSize > sizeof entry->consumeInfo.pageFile) {
      result = VMCI_ERROR_NO_MEM;
      goto out;
   }
   if (pageStore->user) {
      if (VMCI_CopyFromUser(entry->produceInfo.pageFile,
                            normalizedPageStore.producePageFile,
                            (size_t)normalizedPageStore.producePageFileSize)) {
         result = VMCI_ERROR_GENERIC;
         goto out;
      }

      if (VMCI_CopyFromUser(entry->consumeInfo.pageFile,
                            normalizedPageStore.consumePageFile,
                            (size_t)normalizedPageStore.consumePageFileSize)) {
         result = VMCI_ERROR_GENERIC;
         goto out;
      }
   } else {
      memcpy(entry->consumeInfo.pageFile,
             VMCIVA64ToPtr(normalizedPageStore.consumePageFile),
             (size_t)normalizedPageStore.consumePageFileSize);
      memcpy(entry->produceInfo.pageFile,
             VMCIVA64ToPtr(normalizedPageStore.producePageFile),
             (size_t)normalizedPageStore.producePageFileSize);
   }

   /*
    * Copy the data into the attachInfo structure
    */

   memcpy(&entry->attachInfo->producePageFile[0],
          &entry->produceInfo.pageFile[0],
          (size_t)normalizedPageStore.producePageFileSize);
   memcpy(&entry->attachInfo->consumePageFile[0],
          &entry->consumeInfo.pageFile[0],
          (size_t)normalizedPageStore.consumePageFileSize);

   /*
    * NOTE: The UVAs that follow may be 0.  In this case an older VMX has
    * issued a SetPageFile call without mapping the backing files for the
    * queue contents.  The result of this is that the queue pair cannot
    * be connected by host.
    */

   entry->attachInfo->produceBuffer = normalizedPageStore.producePageUVA;
   entry->attachInfo->consumeBuffer = normalizedPageStore.consumePageUVA;

   if (VMCIContext_SupportsHostQP(context)) {
      result = VMCIHost_GetUserMemory(entry->attachInfo,
                                      entry->produceQ,
                                      entry->consumeQ);

      if (result < VMCI_SUCCESS) {
         goto out;
      }
   }
#endif // VMKERNEL

   /*
    * In the event that the QueuePair was created by a host in a
    * hosted kernel, then we send notification now that the QueuePair
    * contents backing files are attached to the Queues.  Note in
    * QueuePairAllocHost(), above, we skipped this step when the
    * creator was a host (on hosted).
    */

   if (!vmkernel && entry->createId == VMCI_HOST_CONTEXT_ID) {
      result = QueuePairNotifyPeer(TRUE, handle, contextId, entry->createId);
      if (result < VMCI_SUCCESS) {
         goto out;
      }
   }

   entry->pageStoreSet = TRUE;
   result = VMCI_SUCCESS;

out:
   return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePair_Detach --
 *
 *      Detach a context from a given QueuePair handle.  Assumes that the QP
 *      list lock is held.  If the "detach" input parameter is FALSE, the QP
 *      entry is not removed from the list of QPs, and the context is not
 *      detached from the given handle.  If "detach" is TRUE, the detach
 *      operation really happens.  With "detach" set to FALSE, the caller can
 *      query if the "actual" detach operation would succeed or not.  The
 *      return value from this function remains the same irrespective of the
 *      value of the boolean "detach".
 *
 *      Also note that the result code for a VM detaching from a
 *      VM-host QP is always VMCI_SUCCESS_LAST_DETACH.  This is so
 *      that VMX can unlink the backing files.  On the host side the
 *      files are either locked (Mac OS/Linux) or the contents are
 *      saved (Windows).
 *
 * Results:
 *      Success or failure.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

int
QueuePair_Detach(VMCIHandle  handle,   // IN:
                 VMCIContext *context, // IN:
                 Bool detach)          // IN: Really detach?
{
   QueuePairEntry *entry;
   int result;
   const VMCIId contextId = VMCIContext_GetId(context);
   VMCIId peerId;

   if (VMCI_HANDLE_INVALID(handle) ||
       !context || contextId == VMCI_INVALID_ID) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   if (!VMCIHandleArray_HasEntry(context->queuePairArray, handle)) {
      VMCILOG((LGPFX"Context %u not attached to queue pair 0x%x:0x%x.\n",
               contextId, handle.context, handle.resource));
      result = VMCI_ERROR_NOT_FOUND;
      goto out;
   }

   entry = QueuePairList_FindEntry(handle);
   if (!entry) {
      result = VMCI_ERROR_NOT_FOUND;
      goto out;
   }

   ASSERT(!(entry->flags & VMCI_QPFLAG_LOCAL));

   if (contextId != entry->createId && contextId != entry->attachId) {
      result = VMCI_ERROR_QUEUEPAIR_NOTATTACHED;
      goto out;
   }

   if (contextId == entry->createId) {
      peerId = entry->attachId;
   } else {
      peerId = entry->createId;
   }

   if (!detach) {
      /* Do not update the QP entry. */

      ASSERT(entry->refCount == 1 || entry->refCount == 2);

      if (entry->refCount == 1 || peerId == VMCI_HOST_CONTEXT_ID) {
         result = VMCI_SUCCESS_LAST_DETACH;
      } else {
         result = VMCI_SUCCESS;
      }

      goto out;
   }

   if (contextId == entry->createId) {
      entry->createId = VMCI_INVALID_ID;
   } else {
      entry->attachId = VMCI_INVALID_ID;
   }
   entry->refCount--;

#ifdef _WIN32
   /*
    * If the caller detaching is a usermode process (e.g., VMX), then
    * we must detach the mappings now.  On Windows.
    *
    * VMCIHost_SaveProduceQ() will save the guest's produceQ so that
    * the host can pick up the data after the guest is gone.
    *
    * We save the ProduceQ whenever the guest detaches (even if VMX
    * continues to run).  If we didn't do this, then we'd have the
    * problem of finding and releasing the memory when the client goes
    * away because we won't be able to find the client in the list of
    * QueuePair entries.  The detach code path (has already) set the
    * contextId for detached end-point to VMCI_INVALID_ID.  (See just
    * a few lines above where that happens.)  Sure, we could fix that,
    * and then we could look at all entries finding ones where the
    * contextId of either creator or attach matches the going away
    * context's Id.  But, if we just copy out the guest's produceQ
    * -always- then we reduce the logic changes elsewhere.
    */

   /*
    * Some example paths through this code:
    *
    * Guest-to-guest: the code will call ReleaseUserMemory() once when
    * the first guest detaches.  And then a second time when the
    * second guest detaches.  That's OK.  Nobody is using the user
    * memory (because there is no host attached) and
    * ReleaseUserMemory() tracks its resources.
    *
    * Host detaches first: the code will not call anything because
    * contextId == VMCI_HOST_CONTEXT_ID and because (in the second if
    * () clause below) refCount > 0.
    *
    * Guest detaches second: the first if clause, below, will not be
    * taken because refCount is already 0.  The second if () clause
    * (below) will be taken and it will simply call
    * ReleaseUserMemory().
    *
    * Guest detaches first: the code will call SaveProduceQ().
    *
    * Host detaches second: the code will call ReleaseUserMemory()
    * which will free the kernel allocated Q memory.
    */

   if (entry->pageStoreSet &&
       contextId != VMCI_HOST_CONTEXT_ID &&
       VMCIContext_SupportsHostQP(context) &&
       entry->refCount) {
      /*
       * It's important to pass down produceQ and consumeQ in the
       * correct order because the produceQ that is to be saved is the
       * guest's, so we have to be sure that the routine sees the
       * guest's produceQ as (in this case) the first Q parameter.
       */

      if (entry->attachId == VMCI_HOST_CONTEXT_ID) {
         VMCIHost_SaveProduceQ(entry->attachInfo,
                               entry->produceQ,
                               entry->consumeQ,
                               entry->produceInfo.size);
      } else if (entry->createId == VMCI_HOST_CONTEXT_ID) {
         VMCIHost_SaveProduceQ(entry->attachInfo,
                               entry->consumeQ,
                               entry->produceQ,
                               entry->consumeInfo.size);
      } else {
         VMCIHost_ReleaseUserMemory(entry->attachInfo,
                                    entry->produceQ,
                                    entry->consumeQ);
      }
   }
#endif // _WIN32

   if (!entry->refCount) {
      QueuePairList_RemoveEntry(entry);

#ifndef VMKERNEL
      if (entry->pageStoreSet &&
          VMCIContext_SupportsHostQP(context)) {
         VMCIHost_ReleaseUserMemory(entry->attachInfo,
                                    entry->produceQ,
                                    entry->consumeQ);
      }
      if (entry->attachInfo) {
         VMCI_FreeKernelMem(entry->attachInfo, sizeof *entry->attachInfo);
      }
      if (entry->produceQ) {
         VMCI_FreeKernelMem(entry->produceQ, sizeof *entry->produceQ);
      }
      if (entry->consumeQ) {
         VMCI_FreeKernelMem(entry->consumeQ, sizeof *entry->consumeQ);
      }
#endif // !VMKERNEL

      VMCI_FreeKernelMem(entry, sizeof *entry);
      result = VMCI_SUCCESS_LAST_DETACH;
   } else {
      /*
       * XXX: If we ever allow the creator to detach and attach again
       * to the same queue pair, we need to handle the mapping of the
       * shared memory region in vmkernel differently. Currently, we
       * assume that an attaching VM always needs to swap the two
       * queues.
       */

      ASSERT(peerId != VMCI_INVALID_ID);
      QueuePairNotifyPeer(FALSE, handle, contextId, peerId);
      if (peerId == VMCI_HOST_CONTEXT_ID) {
         result = VMCI_SUCCESS_LAST_DETACH;
      } else {
         result = VMCI_SUCCESS;
      }
   }

out:
   if (result >= VMCI_SUCCESS && detach) {
      VMCIHandleArray_RemoveEntry(context->queuePairArray, handle);
   }
   return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairNotifyPeer --
 *
 *      Enqueues an event datagram to notify the peer VM attached to the given
 *      QP handle about attach/detach event by the given VM.
 *
 * Results:
 *      Payload size of datagram enqueued on success, error code otherwise.
 *
 * Side effects:
 *      Memory is allocated.
 *
 *-----------------------------------------------------------------------------
 */

int
QueuePairNotifyPeer(Bool attach,       // IN: attach or detach?
                    VMCIHandle handle, // IN:
                    VMCIId myId,       // IN:
                    VMCIId peerId)     // IN: CID of VM to notify
{
   int rv;
   VMCIEventMsg *eMsg;
   VMCIEventPayload_QP *evPayload;
   char buf[sizeof *eMsg + sizeof *evPayload];

   if (VMCI_HANDLE_INVALID(handle) || myId == VMCI_INVALID_ID ||
       peerId == VMCI_INVALID_ID) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   /*
    * Notification message contains:  QP handle and attaching/detaching VM's
    * context id.
    */

   eMsg = (VMCIEventMsg *)buf;

   /*
    * In VMCIContext_EnqueueDatagram() we enforce the upper limit on number of
    * pending events from the hypervisor to a given VM otherwise a rogue VM
    * could do arbitrary number of attached and detaches causing memory
    * pressure in the host kernel.
   */

   /* Clear out any garbage. */
   memset(eMsg, 0, sizeof *eMsg + sizeof *evPayload);

   eMsg->hdr.dst = VMCI_MAKE_HANDLE(peerId, VMCI_EVENT_HANDLER);
   eMsg->hdr.src = VMCI_MAKE_HANDLE(VMCI_HYPERVISOR_CONTEXT_ID,
                                    VMCI_CONTEXT_RESOURCE_ID);
   eMsg->hdr.payloadSize = sizeof *eMsg + sizeof *evPayload - sizeof eMsg->hdr;
   eMsg->eventData.event = attach ? VMCI_EVENT_QP_PEER_ATTACH :
                                    VMCI_EVENT_QP_PEER_DETACH;
   evPayload = VMCIEventMsgPayload(eMsg);
   evPayload->handle = handle;
   evPayload->peerId = myId;

   rv = VMCIDatagram_Dispatch(VMCI_HYPERVISOR_CONTEXT_ID, (VMCIDatagram *)eMsg);
   if (rv < VMCI_SUCCESS) {
      VMCILOG((LGPFX"Failed to enqueue QueuePair %s event datagram for "
               "context %u.\n", attach ? "ATTACH" : "DETACH", peerId));
   }

   return rv;
}

#if !defined(VMKERNEL)


/*
 *----------------------------------------------------------------------
 *
 * VMCIQueuePair_Alloc --
 *
 *    This function implements the kernel API for allocating a queue
 *    pair.
 *
 * Results:
 *     VMCI_SUCCESS on succes and appropriate failure code otherwise.
 *
 * Side effects:
 *     May allocate memory.
 *
 *----------------------------------------------------------------------
 */

int
VMCIQueuePair_Alloc(VMCIHandle *handle,           // IN/OUT
                    VMCIQueue **produceQ,         // OUT
                    uint64 produceSize,           // IN
                    VMCIQueue **consumeQ,         // OUT
                    uint64 consumeSize,           // IN
                    VMCIId peer,                  // IN
                    uint32 flags)                 // IN
{
   return VMCIQueuePair_AllocPriv(handle,
                                  produceQ, produceSize,
                                  consumeQ, consumeSize,
                                  peer, flags,
                                  VMCI_NO_PRIVILEGE_FLAGS);
}

#ifdef __linux__
EXPORT_SYMBOL(VMCIQueuePair_Alloc);
#endif


/*
 *----------------------------------------------------------------------
 *
 * VMCIQueuePair_AllocPriv --
 *
 *    This function implements the kernel API for allocating a queue
 *    pair.
 *
 * Results:
 *     VMCI_SUCCESS on succes and appropriate failure code otherwise.
 *
 * Side effects:
 *     May allocate memory.
 *
 *----------------------------------------------------------------------
 */

int
VMCIQueuePair_AllocPriv(VMCIHandle *handle,           // IN/OUT
                        VMCIQueue **produceQ,         // OUT
                        uint64 produceSize,           // IN
                        VMCIQueue **consumeQ,         // OUT
                        uint64 consumeSize,           // IN
                        VMCIId peer,                  // IN
                        uint32 flags,                 // IN
                        VMCIPrivilegeFlags privFlags) // IN
{
   VMCIContext *context;
   int result;
   QueuePairEntry *entry;

#ifdef _WIN32
   return VMCI_ERROR_UNAVAILABLE;
#endif

   result = VMCI_SUCCESS;

   if (!handle || (produceSize == 0 && consumeSize == 0) ||
       produceQ == NULL || consumeQ == NULL) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   if (VMCI_HANDLE_EQUAL(*handle, VMCI_INVALID_HANDLE)) {
      VMCIId resourceID = VMCIResource_GetID();

      *handle = VMCI_MAKE_HANDLE(VMCI_HOST_CONTEXT_ID, resourceID);
   }

   context = VMCIContext_Get(VMCI_HOST_CONTEXT_ID);
   ASSERT(context);

   entry = NULL;
   QueuePairList_Lock();
   result = QueuePairAllocHost(*handle, peer,
                               flags, privFlags, produceSize,
                               consumeSize, NULL,
                               context, &entry);

   if (result >= VMCI_SUCCESS) {
      ASSERT(entry != NULL);

      if (entry->createId == VMCI_HOST_CONTEXT_ID) {
         *produceQ = entry->produceQ;
         *consumeQ = entry->consumeQ;
      } else {
         *produceQ = entry->consumeQ;
         *consumeQ = entry->produceQ;
      }

      result = VMCI_SUCCESS;
   } else {
      VMCILOG((LGPFX"QueuePairAllocHost() failed: %d.\n", result));
   }

   QueuePairList_Unlock();
   VMCIContext_Release(context);
   return result;
}

#ifdef __linux__
EXPORT_SYMBOL(VMCIQueuePair_AllocPriv);
#endif


/*
 *----------------------------------------------------------------------
 *
 * VMCIQueuePair_Detach --
 *
 *    This function implements the host kernel API for detaching from
 *    a queue pair.
 *
 * Results:
 *     VMCI_SUCCESS on success and appropriate failure code otherwise.
 *
 * Side effects:
 *     May deallocate memory.
 *
 *----------------------------------------------------------------------
 */

int
VMCIQueuePair_Detach(VMCIHandle handle) // IN
{
   int result;
   VMCIContext *context;

   context = VMCIContext_Get(VMCI_HOST_CONTEXT_ID);

   QueuePairList_Lock();
   result = QueuePair_Detach(handle, context, TRUE);
   QueuePairList_Unlock();

   VMCIContext_Release(context);
   return result;
}

#ifdef __linux__
EXPORT_SYMBOL(VMCIQueuePair_Detach);
#endif
#endif  /* !VMKERNEL */
