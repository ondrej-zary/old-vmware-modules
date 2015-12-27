/*********************************************************
 * Copyright (C) 2006 VMware, Inc. All rights reserved.
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
 * vmciContext.c --
 *
 *     Platform independent routines for VMCI calls.
 */

#if defined(linux) && !defined(VMKERNEL)
#   include "driver-config.h"
#   define EXPORT_SYMTAB
#   include "compat_module.h"
#   include <linux/string.h> /* memset() in the kernel */
#elif defined(WINNT_DDK)
#   include <ntddk.h>
#   include <string.h>
#elif !defined(__APPLE__) && !defined(VMKERNEL)
#   error "Unknown platform"
#endif

/* Must precede all vmware headers. */
#include "vmci_kernel_if.h"

#include "vm_assert.h"
#include "vmci_infrastructure.h"
#include "vmciHostKernelAPI.h"
#include "vmciContext.h"
#include "vmciDatagram.h"
#include "vmciDriver.h"
#include "vmciResource.h"
#include "vmciDsInt.h"
#include "vmciGroup.h"
#include "vmciCommonInt.h"
#ifndef VMKERNEL
#  include "vmciQueuePair.h"
#endif
#include "vmware.h"
#include "circList.h"

#define LGPFX "VMCIContext: "

static void VMCIContextFreeContext(VMCIContext *context);
static Bool VMCIContextExists(VMCIId cid);
static int VMCIContextFireNotification(VMCIId contextID,
                                       VMCIPrivilegeFlags privFlags,
                                       const char *domain);

/*
 * List of current VMCI contexts.
 */

static struct {
   ListItem *head;
   VMCILock lock;
   VMCILock firingLock;
} contextList;


/*
 *----------------------------------------------------------------------
 *
 * VMCIContextSignalNotify --
 *
 *      Sets the notify flag to TRUE.  Assumes that the context lock is
 *      held.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE void
VMCIContextSignalNotify(VMCIContext *context) // IN:
{
#ifndef VMX86_SERVER
   if (context->notify) {
      *context->notify = TRUE;
   }
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContextClearNotify --
 *
 *      Sets the notify flag to FALSE.  Assumes that the context lock is
 *      held.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE void
VMCIContextClearNotify(VMCIContext *context) // IN:
{
#ifndef VMX86_SERVER
   if (context->notify) {
      *context->notify = FALSE;
   }
#endif
}


#ifndef VMX86_SERVER
/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_CheckAndSignalNotify --
 *
 *      Sets the context's notify flag iff datagrams are pending for this
 *      context.  Called from VMCISetupNotify(). 
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
VMCIContext_CheckAndSignalNotify(VMCIContext *context) // IN:
{
   VMCILockFlags flags;

   ASSERT(context);
   VMCI_GrabLock(&contextList.lock, &flags);
   if (context->pendingDatagrams) {
      VMCIContextSignalNotify(context);
   }
   VMCI_ReleaseLock(&contextList.lock, flags);
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * VMCIContextGetDomainName --
 *
 *      Internal function for retrieving a context domain name, if
 *      supported by the platform. The returned pointer can only be
 *      assumed valid while a reference count is held on the given
 *      context.
 *
 * Results:
 *      Pointer to name if appropriate. NULL otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE char *
VMCIContextGetDomainName(VMCIContext *context) // IN
{
#ifdef VMKERNEL
   return context->domainName;
#else
   return NULL;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_Init --
 *
 *      Initializes the VMCI context module.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VMCIContext_Init(void)
{
   contextList.head = NULL;
   VMCI_InitLock(&contextList.lock, "VMCIContextListLock",
		 VMCI_LOCK_RANK_HIGHER);
   VMCI_InitLock(&contextList.firingLock, "VMCIContextFiringLock",
		 VMCI_LOCK_RANK_MIDDLE_LOW);

   return VMCI_SUCCESS;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_Exit --
 *
 *      Cleans up the contexts module.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
VMCIContext_Exit(void)
{
   VMCI_CleanupLock(&contextList.firingLock);
   VMCI_CleanupLock(&contextList.lock);
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_InitContext --
 *
 *      Allocates and initializes a VMCI context. 
 *
 * Results:
 *      Returns 0 on success, appropriate error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VMCIContext_InitContext(VMCIId cid,                   // IN
			VMCIPrivilegeFlags privFlags, // IN
                        uintptr_t eventHnd,           // IN
                        int userVersion,              // IN: User's vers no.
                        VMCIContext **outContext)     // OUT
{
   VMCILockFlags flags;
   VMCIContext *context;
   int result;

   if (privFlags & ~VMCI_PRIVILEGE_ALL_FLAGS) {
      VMCILOG((LGPFX"Invalid flag for VMCI context.\n"));
      return VMCI_ERROR_INVALID_ARGS;
   }

   if (userVersion == 0) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   context = VMCI_AllocKernelMem(sizeof *context, VMCI_MEMORY_NONPAGED);
   if (context == NULL) {
      VMCILOG((LGPFX"Failed to allocate memory for VMCI context.\n"));
      return VMCI_ERROR_NO_MEM;
   }
   context->wellKnownArray = NULL;
   context->groupArray = NULL;
   context->queuePairArray = NULL;
   context->notifierArray = NULL;
   context->datagramQueue = NULL;
   context->pendingDatagrams = 0;
   context->datagramQueueSize = 0;
   context->userVersion = userVersion;

   context->wellKnownArray = VMCIHandleArray_Create(0);
   if (context->wellKnownArray == NULL) {
      result = VMCI_ERROR_NO_MEM;
      goto error;
   }

   context->groupArray = VMCIHandleArray_Create(0);
   if (context->groupArray == NULL) {
      result = VMCI_ERROR_NO_MEM;
      goto error;
   }

   context->queuePairArray = VMCIHandleArray_Create(0);
   if (!context->queuePairArray) {
      result = VMCI_ERROR_NO_MEM;
      goto error;
   }

   context->notifierArray = VMCIHandleArray_Create(0);
   if (context->notifierArray == NULL) {
      result = VMCI_ERROR_NO_MEM;
      goto error;
   }

   VMCI_InitLock(&context->lock,
                 "VMCIContextLock",
                 VMCI_LOCK_RANK_HIGHER);
   Atomic_Write(&context->refCount, 1);
   
   /* Inititialize host-specific VMCI context. */
   VMCIHost_InitContext(&context->hostContext, eventHnd);
   
   context->privFlags = privFlags;

   /* 
    * If we collide with an existing context we generate a new and use it 
    * instead. The VMX will determine if regeneration is okay. Since there
    * isn't 4B - 16 VMs running on a given host, the below loop will terminate.
    */
   VMCI_GrabLock(&contextList.lock, &flags);
   ASSERT(cid != VMCI_INVALID_ID);
   while (VMCIContextExists(cid)) {

      /*
       * If the cid is below our limit and we collide we are creating duplicate
       * contexts internally so we want to assert fail in that case.
       */
      ASSERT(cid >= VMCI_RESERVED_CID_LIMIT);

      /* We reserve the lowest 16 ids for fixed contexts. */
      cid = MAX(cid, VMCI_RESERVED_CID_LIMIT-1) + 1;
      if (cid == VMCI_INVALID_ID) {
	 cid = VMCI_RESERVED_CID_LIMIT;
      }
   }
   ASSERT(!VMCIContextExists(cid));
   context->cid = cid;
   
   LIST_QUEUE(&context->listItem, &contextList.head);
   VMCI_ReleaseLock(&contextList.lock, flags);

#ifdef VMKERNEL
   /*
    * Set default domain name.
    */
   VMCIContext_SetDomainName(context, "");
#endif

#ifndef VMX86_SERVER
   context->notify = NULL;
#  ifdef __linux__
   context->notifyPage = NULL;
#  endif
#endif

   *outContext = context;
   return VMCI_SUCCESS;

error:
   if (context->notifierArray) {
      VMCIHandleArray_Destroy(context->notifierArray);
   }
   if (context->wellKnownArray) {
      VMCIHandleArray_Destroy(context->wellKnownArray);
   }
   if (context->groupArray) {
      VMCIHandleArray_Destroy(context->groupArray);
   }
   if (context->queuePairArray) {
      VMCIHandleArray_Destroy(context->queuePairArray);
   }
   VMCI_FreeKernelMem(context, sizeof *context);
   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_ReleaseContext --
 *
 *      Cleans up a VMCI context.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
VMCIContext_ReleaseContext(VMCIContext *context)   // IN
{
   VMCILockFlags flags;

   /* Dequeue VMCI context. */

   VMCI_GrabLock(&contextList.lock, &flags);
   LIST_DEL(&context->listItem, &contextList.head);
   VMCI_ReleaseLock(&contextList.lock, flags);

   VMCIContext_Release(context);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIContextFreeContext --
 *
 *      Deallocates all parts of a context datastructure. This
 *      functions doesn't lock the context, because it assumes that
 *      the caller is holding the last reference to context. As paged
 *      memory may be freed as part of the call, the function must be
 *      called without holding any spinlocks as this is not allowed on
 *      Windows.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Paged memory is freed.
 *
 *-----------------------------------------------------------------------------
 */

static void
VMCIContextFreeContext(VMCIContext *context)  // IN
{ 
   ListItem *curr;
   ListItem *next;
   DatagramQueueEntry *dqEntry;
   VMCIHandle tempHandle;

   /* Fire event to all contexts interested in knowing this context is dying. */
   VMCIContextFireNotification(context->cid, context->privFlags,
                               VMCIContextGetDomainName(context));

   /*
    * Cleanup all wellknown mappings owned by context. Ideally these would
    * be removed already but we maintain this list to make sure no resources
    * are leaked. It is updated by the VMCIDatagramAdd/RemoveWellKnownMap.
    */
   ASSERT(context->wellKnownArray);
   tempHandle = VMCIHandleArray_RemoveTail(context->wellKnownArray);
   while (!VMCI_HANDLE_EQUAL(tempHandle, VMCI_INVALID_HANDLE)) {
      VMCIDatagramRemoveWellKnownMap(tempHandle.resource, context->cid);
      tempHandle = VMCIHandleArray_RemoveTail(context->wellKnownArray);
   }

#ifndef VMKERNEL
   /*
    * Cleanup all queue pair resources attached to context.  If the VM dies
    * without cleaning up, this code will make sure that no resources are
    * leaked.
    */

   tempHandle = VMCIHandleArray_GetEntry(context->queuePairArray, 0);
   while (!VMCI_HANDLE_EQUAL(tempHandle, VMCI_INVALID_HANDLE)) {
      if (QueuePair_Detach(tempHandle, context, TRUE) < VMCI_SUCCESS) {
         /*
          * When QueuePair_Detach() succeeds it removes the handle from the
          * array.  If detach fails, we must remove the handle ourselves.
          */
         VMCIHandleArray_RemoveEntry(context->queuePairArray, tempHandle);
      }
      tempHandle = VMCIHandleArray_GetEntry(context->queuePairArray, 0);
   }
#else
   /*
    * On ESX, all entries in the queuePairArray have been cleaned up
    * either by the regular VMCI device destroy path or by the world
    * cleanup destroy path. We assert that no resources are leaked.
    */

   ASSERT(VMCI_HANDLE_EQUAL(VMCIHandleArray_GetEntry(context->queuePairArray, 0),
                            VMCI_INVALID_HANDLE));
#endif /* !VMKERNEL */

   /*
    * Check that the context has been removed from all the groups it was a
    * member of. If not, remove it from the group.
    */
   ASSERT(context->groupArray);
   tempHandle = VMCIHandleArray_RemoveTail(context->groupArray);
   while (!VMCI_HANDLE_EQUAL(tempHandle, VMCI_INVALID_HANDLE)) {
      VMCI_DEBUG_LOG((LGPFX"Removing context 0x%x from group 0x%"FMT64
                      "x during release.\n", context->cid, tempHandle));
      VMCIGroup_RemoveMember(tempHandle, VMCI_MAKE_HANDLE(context->cid,
                                                      VMCI_CONTEXT_RESOURCE_ID));
      tempHandle = VMCIHandleArray_RemoveTail(context->groupArray);
   }

  /*
    * It is fine to destroy this without locking the callQueue, as
    * this is the only thread having a reference to the context.
    */

   LIST_SCAN_SAFE(curr, next, context->datagramQueue) {
      dqEntry = LIST_CONTAINER(curr, DatagramQueueEntry, listItem);
      LIST_DEL(curr, &context->datagramQueue);
      ASSERT(dqEntry && dqEntry->dg);
      ASSERT(dqEntry->dgSize == VMCI_DG_SIZE(dqEntry->dg));
      VMCI_FreeKernelMem(dqEntry->dg, dqEntry->dgSize);
      VMCI_FreeKernelMem(dqEntry, sizeof *dqEntry);
   }

   VMCIHandleArray_Destroy(context->notifierArray);
   VMCIHandleArray_Destroy(context->wellKnownArray);
   VMCIHandleArray_Destroy(context->groupArray);
   VMCIHandleArray_Destroy(context->queuePairArray);
   VMCI_CleanupLock(&context->lock);
   VMCIHost_ReleaseContext(&context->hostContext);
#ifndef VMX86_SERVER
#  ifdef __linux__
   /* TODO Windows and Mac OS. */
   VMCIUnsetNotify(context);
#  endif
#endif
   VMCI_FreeKernelMem(context, sizeof *context);
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_PendingDatagrams --
 *
 *      Returns the current number of pending datagrams. The call may
 *      also serve as a synchronization point for the datagram queue,
 *      as no enqueue operations can occur concurrently.
 *
 * Results:
 *      Length of datagram queue for the given context.
 *
 * Side effects:
 *      Locks datagram queue.
 *
 *----------------------------------------------------------------------
 */

int
VMCIContext_PendingDatagrams(VMCIId cid,      // IN
			     uint32 *pending) // OUT
{
   VMCIContext *context;
   VMCILockFlags flags;

   context = VMCIContext_Get(cid);
   if (context == NULL) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   VMCI_GrabLock(&context->lock, &flags);
   if (pending) {
      *pending = context->pendingDatagrams;
   }
   VMCI_ReleaseLock(&context->lock, flags);
   VMCIContext_Release(context);

   return VMCI_SUCCESS;
}


/*
 * We allow at least 1024 more event datagrams from the hypervisor past the
 * normally allowed datagrams pending for a given context.  We define this
 * limit on event datagrams from the hypervisor to guard against DoS attack
 * from a malicious VM which could repeatedly attach to and detach from a queue
 * pair, causing events to be queued at the destination VM.  However, the rate
 * at which such events can be generated is small since it requires a VM exit
 * and handling of queue pair attach/detach call at the hypervisor.  Event
 * datagrams may be queued up at the destination VM if it has interrupts
 * disabled or if it is not draining events for some other reason.  1024
 * datagrams is a grossly conservative estimate of the time for which
 * interrupts may be disabled in the destination VM, but at the same time does
 * not exacerbate the memory pressure problem on the host by much (size of each
 * event datagram is small).
 */

#define VMCI_MAX_DATAGRAM_AND_EVENT_QUEUE_SIZE \
   (VMCI_MAX_DATAGRAM_QUEUE_SIZE + \
    1024 * (sizeof(VMCIDatagram) + sizeof(VMCIEventData_Max)))


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_EnqueueDatagram --
 *
 *      Queues a VMCI datagram for the appropriate target VM 
 *      context.
 *
 * Results:
 *      Size of enqueued data on success, appropriate error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VMCIContext_EnqueueDatagram(VMCIId cid,        // IN: Target VM
                            VMCIDatagram *dg)  // IN:
{
   DatagramQueueEntry *dqEntry;
   VMCIContext *context;
   VMCILockFlags flags;
   VMCIHandle dgSrc;
   size_t vmciDgSize;

   ASSERT(dg);
   vmciDgSize = VMCI_DG_SIZE(dg);
   ASSERT(vmciDgSize <= VMCI_MAX_DG_SIZE);

   /* Get the target VM's VMCI context. */
   context = VMCIContext_Get(cid);
   if (context == NULL) {
      VMCILOGThrottled((LGPFX"Invalid cid.\n"));
      return VMCI_ERROR_INVALID_ARGS;
   }

   /* Allocate guest call entry and add it to the target VM's queue. */
   dqEntry = VMCI_AllocKernelMem(sizeof *dqEntry, VMCI_MEMORY_NONPAGED);
   if (dqEntry == NULL) {
      VMCILOG((LGPFX"Failed to allocate memory for datagram.\n"));
      VMCIContext_Release(context);
      return VMCI_ERROR_NO_MEM;
   }
   dqEntry->dg = dg;
   dqEntry->dgSize = vmciDgSize;
   dgSrc = dg->src;

   VMCI_GrabLock(&context->lock, &flags);
   /*
    * We put a higher limit on datagrams from the hypervisor.  If the pending
    * datagram is not from hypervisor, then we check if enqueueing it would
    * exceed the VMCI_MAX_DATAGRAM_QUEUE_SIZE limit on the destination.  If the
    * pending datagram is from hypervisor, we allow it to be queued at the
    * destination side provided we don't reach the
    * VMCI_MAX_DATAGRAM_AND_EVENT_QUEUE_SIZE limit.
    */
   if (context->datagramQueueSize + vmciDgSize >=
         VMCI_MAX_DATAGRAM_QUEUE_SIZE &&
       (!VMCI_HANDLE_EQUAL(dgSrc,
                           VMCI_MAKE_HANDLE(VMCI_HYPERVISOR_CONTEXT_ID,
                                            VMCI_CONTEXT_RESOURCE_ID)) ||
        context->datagramQueueSize + vmciDgSize >=
         VMCI_MAX_DATAGRAM_AND_EVENT_QUEUE_SIZE)) {
      VMCI_ReleaseLock(&context->lock, flags);
      VMCIContext_Release(context);
      VMCI_FreeKernelMem(dqEntry, sizeof *dqEntry);
      VMCILOGThrottled((LGPFX"Context 0x%x receive queue is full.\n", cid));
      return VMCI_ERROR_NO_RESOURCES;
   }

   LIST_QUEUE(&dqEntry->listItem, &context->datagramQueue);
   context->pendingDatagrams++;
   context->datagramQueueSize += vmciDgSize;
   VMCIContextSignalNotify(context);
   VMCIHost_SignalCall(&context->hostContext);
   VMCI_ReleaseLock(&context->lock, flags);
   VMCIContext_Release(context);

   return vmciDgSize;
}

#undef VMCI_MAX_DATAGRAM_AND_EVENT_QUEUE_SIZE


/*
 *----------------------------------------------------------------------
 *
 * VMCIContextExists --
 *
 *      Internal helper to check if a context with the specified context
 *      ID exists. Assumes the contextList.lock is held.
 *
 * Results:
 *      TRUE if a context exists with the given cid.
 *      FALSE otherwise
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Bool
VMCIContextExists(VMCIId cid)    // IN
{
   VMCIContext *context;
   ListItem *next;
   Bool rv = FALSE;

   LIST_SCAN(next, contextList.head) {
      context = LIST_CONTAINER(next, VMCIContext, listItem);
      if (context->cid == cid) {
         rv = TRUE;
         break;
      }
   }
   return rv;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_Exists --
 *
 *      Verifies whether a context with the specified context ID exists.
 *
 * Results:
 *      TRUE if a context exists with the given cid.
 *      FALSE otherwise
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
VMCIContext_Exists(VMCIId cid)    // IN
{
   VMCILockFlags flags;
   Bool rv;

   VMCI_GrabLock(&contextList.lock, &flags);
   rv = VMCIContextExists(cid);
   VMCI_ReleaseLock(&contextList.lock, flags);
   return rv;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_Get --
 *
 *      Retrieves VMCI context corresponding to the given cid.
 *
 * Results:
 *      VMCI context on success, NULL otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

VMCIContext *
VMCIContext_Get(VMCIId cid)  // IN
{
   VMCIContext *context = NULL;  
   ListItem *next;
   VMCILockFlags flags;

   VMCI_GrabLock(&contextList.lock, &flags);
   if (LIST_EMPTY(contextList.head)) {
      goto out;
   }

   LIST_SCAN(next, contextList.head) {
      context = LIST_CONTAINER(next, VMCIContext, listItem);
      if (context->cid == cid) {
         /*
          * At this point, we are sure that the reference count is
          * larger already than zero. When starting the destruction of
          * a context, we always remove it from the context list
          * before decreasing the reference count. As we found the
          * context here, it hasn't been destroyed yet. This means
          * that we are not about to increase the reference count of
          * something that is in the process of being destroyed.
          */

         Atomic_Inc(&context->refCount);
         break;
      }
   }

out:
   VMCI_ReleaseLock(&contextList.lock, flags);
   return (context && context->cid == cid) ? context : NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_Release --
 *
 *      Releases the VMCI context. If this is the last reference to
 *      the context it will be deallocated. A context is created with
 *      a reference count of one, and on destroy, it is removed from
 *      the context list before its reference count is
 *      decremented. Thus, if we reach zero, we are sure that nobody
 *      else are about to increment it (they need the entry in the
 *      context list for that). This function musn't be called with a
 *      lock held.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Paged memory may be deallocated.
 *
 *----------------------------------------------------------------------
 */

void
VMCIContext_Release(VMCIContext *context)  // IN
{
   uint32 refCount;
   ASSERT(context);
   refCount = Atomic_FetchAndDec(&context->refCount);
   if (refCount == 1) {
      VMCIContextFreeContext(context);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_DequeueDatagram --
 *
 *      Dequeues the next datagram and returns it to caller.
 *      The caller passes in a pointer to the max size datagram
 *      it can handle and the datagram is only unqueued if the
 *      size is less than maxSize. If larger maxSize is set to
 *      the size of the datagram to give the caller a chance to
 *      set up a larger buffer for the guestcall.
 *
 * Results:
 *      On success:  0 if no more pending datagrams, otherwise the size of
 *                   the next pending datagram.
 *      On failure:  appropriate error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VMCIContext_DequeueDatagram(VMCIContext *context, // IN
			    size_t *maxSize,      // IN/OUT: max size of datagram caller can handle.
			    VMCIDatagram **dg)    // OUT:
{
   DatagramQueueEntry *dqEntry;
   ListItem *listItem;
   VMCILockFlags flags;
   int rv;

   ASSERT(context && dg);

   /* Dequeue the next datagram entry. */
   VMCI_GrabLock(&context->lock, &flags);
   if (context->pendingDatagrams == 0) {
      VMCIHost_ClearCall(&context->hostContext);
      VMCIContextClearNotify(context);
      VMCI_ReleaseLock(&context->lock, flags);
      VMCI_DEBUG_LOG((LGPFX"No datagrams pending.\n"));
      return VMCI_ERROR_NO_MORE_DATAGRAMS;
   }

   listItem = LIST_FIRST(context->datagramQueue);
   ASSERT (listItem != NULL);

   dqEntry = LIST_CONTAINER(listItem, DatagramQueueEntry, listItem);
   ASSERT(dqEntry->dg);

   /* Check size of caller's buffer. */
   if (*maxSize < dqEntry->dgSize) {
      *maxSize = dqEntry->dgSize;
      VMCI_ReleaseLock(&context->lock, flags);
      VMCILOG((LGPFX"Caller's buffer is too small. It must be at "
               "least %"FMTSZ"d bytes.\n", *maxSize));
      return VMCI_ERROR_NO_MEM;
   }
   
   LIST_DEL(listItem, &context->datagramQueue);
   context->pendingDatagrams--;
   context->datagramQueueSize -= dqEntry->dgSize;
   if (context->pendingDatagrams == 0) {
      VMCIHost_ClearCall(&context->hostContext);
      VMCIContextClearNotify(context);
      rv = VMCI_SUCCESS;
   } else {
      /*
       * Return the size of the next datagram.
       */
      DatagramQueueEntry *nextEntry;

      listItem = LIST_FIRST(context->datagramQueue);
      ASSERT(listItem);
      nextEntry = LIST_CONTAINER(listItem, DatagramQueueEntry, listItem);
      ASSERT(nextEntry && nextEntry->dg);
      /*
       * The following size_t -> int truncation is fine as the maximum size of
       * a (routable) datagram is 68KB.
       */
      rv = (int)nextEntry->dgSize;
   }
   VMCI_ReleaseLock(&context->lock, flags);

   /* Caller must free datagram. */
   ASSERT(dqEntry->dgSize == VMCI_DG_SIZE(dqEntry->dg));
   *dg = dqEntry->dg;
   dqEntry->dg = NULL;
   VMCI_FreeKernelMem(dqEntry, sizeof *dqEntry);

   return rv;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_GetId --
 *
 *      Retrieves cid of given VMCI context.
 *
 * Results:
 *      VMCIId of context on success, VMCI_INVALID_ID otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

VMCIId
VMCIContext_GetId(VMCIContext *context) // IN:
{
   if (!context) {
      return VMCI_INVALID_ID;
   }
   ASSERT(context->cid != VMCI_INVALID_ID);
   return context->cid;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_GetPrivFlagsInt --
 *
 *      Internal function that retrieves the privilege flags of the given
 *      VMCI context ID.
 *
 * Results:
 *     Context's privilege flags.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

VMCIPrivilegeFlags
VMCIContext_GetPrivFlagsInt(VMCIId contextID)  // IN
{
   VMCIPrivilegeFlags flags;
   VMCIContext *context;

   context = VMCIContext_Get(contextID);
   if (!context) {
      return VMCI_LEAST_PRIVILEGE_FLAGS;
   }
   flags = context->privFlags;
   VMCIContext_Release(context);
   return flags;
}


#ifndef VMKERNEL
/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_GetPrivFlags --
 *
 *      Retrieves the privilege flags of the given VMCI context ID.
 *
 * Results:
 *     Context's privilege flags.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

#if defined(linux)
EXPORT_SYMBOL(VMCIContext_GetPrivFlags);
#endif

VMCIPrivilegeFlags
VMCIContext_GetPrivFlags(VMCIId contextID)  // IN
{
   return VMCIContext_GetPrivFlagsInt(contextID);
}
#endif /* !VMKERNEL */


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_AddGroupEntry --
 *
 *      Wrapper to call VMCIHandleArray_AppendEntry().
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      As in VMCIHandleArray_AppendEntry().
 *
 *----------------------------------------------------------------------
 */

int
VMCIContext_AddGroupEntry(VMCIContext *context, // IN:
                          VMCIHandle entryHandle) // IN:
{
   VMCILockFlags flags;

   if (!context) {
      return VMCI_ERROR_INVALID_ARGS;
   }
   VMCI_GrabLock(&context->lock, &flags);
   VMCIHandleArray_AppendEntry(&context->groupArray, entryHandle);
   VMCI_ReleaseLock(&context->lock, flags);
   return VMCI_SUCCESS;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_RemoveGroupEntry --
 *
 *      Wrapper to call VMCIHandleArray_RemoveEntry().
 *
 * Results:
 *      Return value from VMCIHandleArray_RemoveEntry().
 *
 * Side effects:
 *      As in VMCIHandleArray_RemoveEntry().
 *
 *----------------------------------------------------------------------
 */

VMCIHandle
VMCIContext_RemoveGroupEntry(VMCIContext *context, // IN:
                             VMCIHandle entryHandle) // IN:
{
   VMCILockFlags flags;
   VMCIHandle handle;

   if (!context) {
      return VMCI_INVALID_HANDLE;
   }
   VMCI_GrabLock(&context->lock, &flags);
   handle = VMCIHandleArray_RemoveEntry(context->groupArray, entryHandle);
   VMCI_ReleaseLock(&context->lock, flags);

   return handle;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_AddWellKnown --
 *
 *      Wrapper to call VMCIHandleArray_AppendEntry().
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      As in VMCIHandleArray_AppendEntry().
 *
 *----------------------------------------------------------------------
 */

int
VMCIContext_AddWellKnown(VMCIId contextID,    // IN:
			 VMCIId wellKnownID)  // IN:
{ 
   VMCILockFlags flags;
   VMCIHandle wkHandle;
   VMCIContext *context = VMCIContext_Get(contextID);
   if (context == NULL) {
      return VMCI_ERROR_NOT_FOUND;
   }
   wkHandle = VMCI_MAKE_HANDLE(VMCI_WELL_KNOWN_CONTEXT_ID, wellKnownID);
   VMCI_GrabLock(&context->lock, &flags);
   VMCIHandleArray_AppendEntry(&context->wellKnownArray, wkHandle);
   VMCI_ReleaseLock(&context->lock, flags);
   VMCIContext_Release(context);

   return VMCI_SUCCESS;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_RemoveWellKnown --
 *
 *      Wrapper to call VMCIHandleArray_RemoveEntry().
 *
 * Results:
 *      VMCI_SUCCESS if removed, error code otherwise.
 *
 * Side effects:
 *      As in VMCIHandleArray_RemoveEntry().
 *
 *----------------------------------------------------------------------
 */

int
VMCIContext_RemoveWellKnown(VMCIId contextID,    // IN:
			    VMCIId wellKnownID)  // IN:
{
   VMCILockFlags flags;
   VMCIHandle wkHandle, tmpHandle;
   VMCIContext *context = VMCIContext_Get(contextID);
   if (context == NULL) {
      return VMCI_ERROR_NOT_FOUND;
   }
   wkHandle = VMCI_MAKE_HANDLE(VMCI_WELL_KNOWN_CONTEXT_ID, wellKnownID);
   VMCI_GrabLock(&context->lock, &flags);
   tmpHandle = VMCIHandleArray_RemoveEntry(context->wellKnownArray, wkHandle);
   VMCI_ReleaseLock(&context->lock, flags);
   VMCIContext_Release(context);
   
   if (VMCI_HANDLE_EQUAL(tmpHandle, VMCI_INVALID_HANDLE)) {
      return VMCI_ERROR_NOT_FOUND;
   }
   return VMCI_SUCCESS;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_AddNotification --
 *
 *      Add remoteCID to list of contexts current contexts wants 
 *      notifications from/about.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      As in VMCIHandleArray_AppendEntry().
 *
 *----------------------------------------------------------------------
 */

int
VMCIContext_AddNotification(VMCIId contextID,  // IN:
			    VMCIId remoteCID)  // IN:
{ 
   int result = VMCI_ERROR_ALREADY_EXISTS;
   VMCILockFlags flags; 
   VMCILockFlags firingFlags;
   VMCIHandle notifierHandle;
   VMCIContext *context = VMCIContext_Get(contextID);
   if (context == NULL) {
      return VMCI_ERROR_NOT_FOUND;
   }

   if (context->privFlags & VMCI_PRIVILEGE_FLAG_RESTRICTED) {
      result = VMCI_ERROR_NO_ACCESS;
      goto out;
   }

   notifierHandle = VMCI_MAKE_HANDLE(remoteCID, VMCI_EVENT_HANDLER);
   VMCI_GrabLock(&contextList.firingLock, &firingFlags);
   VMCI_GrabLock(&context->lock, &flags);
   if (!VMCIHandleArray_HasEntry(context->notifierArray, notifierHandle)) {
      VMCIHandleArray_AppendEntry(&context->notifierArray, notifierHandle);
      result = VMCI_SUCCESS;
   }
   VMCI_ReleaseLock(&context->lock, flags);
   VMCI_ReleaseLock(&contextList.firingLock, firingFlags);
out:
   VMCIContext_Release(context);
   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_RemoveNotification --
 *
 *      Remove remoteCID from current context's list of contexts it is 
 *      interested in getting notifications from/about.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VMCIContext_RemoveNotification(VMCIId contextID,  // IN:
			       VMCIId remoteCID)  // IN:
{ 
   VMCILockFlags flags;
   VMCILockFlags firingFlags;
   VMCIContext *context = VMCIContext_Get(contextID);
   VMCIHandle tmpHandle;
   if (context == NULL) {
      return VMCI_ERROR_NOT_FOUND;
   }
   VMCI_GrabLock(&contextList.firingLock, &firingFlags);
   VMCI_GrabLock(&context->lock, &flags);
   tmpHandle = 
      VMCIHandleArray_RemoveEntry(context->notifierArray, 
				  VMCI_MAKE_HANDLE(remoteCID,
						   VMCI_EVENT_HANDLER));
   VMCI_ReleaseLock(&context->lock, flags);
   VMCI_ReleaseLock(&contextList.firingLock, firingFlags);
   VMCIContext_Release(context);

   if (VMCI_HANDLE_EQUAL(tmpHandle, VMCI_INVALID_HANDLE)) {
      return VMCI_ERROR_NOT_FOUND;
   }
   return VMCI_SUCCESS;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContextFireNotification --
 *
 *      Fire notification for all contexts interested in given cid.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
VMCIContextFireNotification(VMCIId contextID,             // IN
                            VMCIPrivilegeFlags privFlags, // IN
                            const char *domain)           // IN
{
   uint32 i, arraySize;
   ListItem *next;
   VMCILockFlags flags;
   VMCILockFlags firingFlags;
   VMCIHandleArray *subscriberArray;
   VMCIHandle contextHandle = VMCI_MAKE_HANDLE(contextID, VMCI_EVENT_HANDLER);

   /*
    * We create an array to hold the subscribers we find when scanning through
    * all contexts.
    */
   subscriberArray = VMCIHandleArray_Create(0);
   if (subscriberArray == NULL) {
      return VMCI_ERROR_NO_MEM;
   }

   /* 
    * Scan all contexts to find who is interested in being notified about
    * given contextID. We have a special firingLock that we use to synchronize
    * across all notification operations. This avoids us having to take the
    * context lock for each HasEntry call and it solves a lock ranking issue.
    */
   VMCI_GrabLock(&contextList.firingLock, &firingFlags);
   VMCI_GrabLock(&contextList.lock, &flags);
   LIST_SCAN(next, contextList.head) {
      VMCIContext *subCtx = LIST_CONTAINER(next, VMCIContext, listItem);

      /*
       * We only deliver notifications of the removal of contexts, if
       * the two contexts are allowed to interact.
       */

      if (VMCIHandleArray_HasEntry(subCtx->notifierArray, contextHandle) &&
          !VMCIDenyInteraction(privFlags, subCtx->privFlags, domain,
                               VMCIContextGetDomainName(subCtx))) {
         VMCIHandleArray_AppendEntry(&subscriberArray,
                                     VMCI_MAKE_HANDLE(subCtx->cid,
                                                      VMCI_EVENT_HANDLER));
      }
   }
   VMCI_ReleaseLock(&contextList.lock, flags);
   VMCI_ReleaseLock(&contextList.firingLock, firingFlags);

   /* Fire event to all subscribers. */ 
   arraySize = VMCIHandleArray_GetSize(subscriberArray);
   for (i = 0; i < arraySize; i++) {
      int result;
      VMCIEventMsg *eMsg;
      VMCIEventPayload_Context *evPayload;
      char buf[sizeof *eMsg + sizeof *evPayload];

      eMsg = (VMCIEventMsg *)buf;

      /* Clear out any garbage. */
      memset(eMsg, 0, sizeof *eMsg + sizeof *evPayload);
      eMsg->hdr.dst = VMCIHandleArray_GetEntry(subscriberArray, i);
      eMsg->hdr.src = VMCI_MAKE_HANDLE(VMCI_HYPERVISOR_CONTEXT_ID,
                                       VMCI_CONTEXT_RESOURCE_ID);
      eMsg->hdr.payloadSize = sizeof *eMsg + sizeof *evPayload -
                              sizeof eMsg->hdr;
      eMsg->eventData.event = VMCI_EVENT_CTX_REMOVED;
      evPayload = VMCIEventMsgPayload(eMsg);
      evPayload->contextID = contextID;

      result = VMCIDatagram_Dispatch(VMCI_HYPERVISOR_CONTEXT_ID,
                                     (VMCIDatagram *)eMsg);
      if (result < VMCI_SUCCESS) {
         VMCILOG((LGPFX"Failed to enqueue event datagram %d for context %d.\n",
                  eMsg->eventData.event, eMsg->hdr.dst.context));
         /* We continue to enqueue on next subscriber. */
      }
   }
   VMCIHandleArray_Destroy(subscriberArray);

   return VMCI_SUCCESS;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_GetCheckpointState --
 *
 *      Get current context's checkpoint state of given type.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VMCIContext_GetCheckpointState(VMCIId contextID,    // IN:
			       uint32 cptType,      // IN:
			       uint32 *bufSize,     // IN/OUT:
			       char   **cptBufPtr)  // OUT:
{ 
   int i, result;
   VMCILockFlags flags;
   uint32 arraySize, cptDataSize;
   VMCIHandleArray *array;
   VMCIContext *context;
   char *cptBuf;
   Bool getContextID;
   
   ASSERT(bufSize && cptBufPtr);

   context = VMCIContext_Get(contextID);
   if (context == NULL) {
      return VMCI_ERROR_NOT_FOUND;
   }

   VMCI_GrabLock(&context->lock, &flags);
   if (cptType == VMCI_NOTIFICATION_CPT_STATE) {
      ASSERT(context->notifierArray);
      array = context->notifierArray;
      getContextID = TRUE;
   } else if (cptType == VMCI_WELLKNOWN_CPT_STATE) {
      ASSERT(context->wellKnownArray);
      array = context->wellKnownArray;
      getContextID = FALSE;
   } else {
      VMCILOG((LGPFX"Invalid cpt state type %d.\n", cptType));
      result = VMCI_ERROR_INVALID_ARGS;
      goto release;
   }

   arraySize = VMCIHandleArray_GetSize(array);
   if (arraySize > 0) {
      cptDataSize = arraySize * sizeof(VMCIId);
      if (*bufSize < cptDataSize) {
	 *bufSize = cptDataSize;
	 result = VMCI_ERROR_MORE_DATA;
	 goto release;
      }

      cptBuf = VMCI_AllocKernelMem(cptDataSize,
                                   VMCI_MEMORY_NONPAGED | VMCI_MEMORY_ATOMIC);
      if (cptBuf == NULL) {
	 result = VMCI_ERROR_NO_MEM;
	 goto release;
      }
      
      for (i = 0; i < arraySize; i++) {
	 VMCIHandle tmpHandle = VMCIHandleArray_GetEntry(array, i);
	 ((VMCIId *)cptBuf)[i] = 
	    getContextID ? tmpHandle.context : tmpHandle.resource;
      }
      *bufSize = cptDataSize;
      *cptBufPtr = cptBuf;
   } else {
      *bufSize = 0;
      *cptBufPtr = NULL;
   }
   result = VMCI_SUCCESS;
   
  release:
   VMCI_ReleaseLock(&context->lock, flags);
   VMCIContext_Release(context);

   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_SetCheckpointState --
 *
 *      Set current context's checkpoint state of given type.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VMCIContext_SetCheckpointState(VMCIId contextID, // IN:
			       uint32 cptType,   // IN:
			       uint32 bufSize,   // IN:
			       char   *cptBuf)   // IN:
{ 
   uint32 i;
   VMCIId currentID;
   int result = VMCI_SUCCESS;
   uint32 numIDs = bufSize / sizeof(VMCIId);
   ASSERT(cptBuf);

   if (cptType != VMCI_NOTIFICATION_CPT_STATE &&
       cptType != VMCI_WELLKNOWN_CPT_STATE) {
      VMCILOG((LGPFX"Invalid cpt state type %d.\n", cptType));
      return VMCI_ERROR_INVALID_ARGS;
   }

   for (i = 0; i < numIDs && result == VMCI_SUCCESS; i++) {
      currentID = ((VMCIId *)cptBuf)[i];
      if (cptType == VMCI_NOTIFICATION_CPT_STATE) {
	 result = VMCIContext_AddNotification(contextID, currentID);
      } else if (cptType == VMCI_WELLKNOWN_CPT_STATE) {
	 result = VMCIDatagramRequestWellKnownMap(currentID, contextID, 
						  VMCIContext_GetPrivFlagsInt(contextID));
      }
   }
   if (result != VMCI_SUCCESS) {
      VMCILOG((LGPFX"Failed to set cpt state type %d, error %d.\n", 
               cptType, result));
   }
   return result;
}


#ifdef VMKERNEL


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_SetDomainName --
 *
 *      Sets the domain name of the given context.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VMCIContext_SetDomainName(VMCIContext *context,   // IN;
                          const char *domainName) // IN:
{
   size_t domainNameLen;

   if (!context || !domainName) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   domainNameLen = strlen(domainName);
   if (domainNameLen >= sizeof context->domainName) {
      return VMCI_ERROR_NO_MEM;
   }

   memcpy(context->domainName, domainName, domainNameLen + 1);

   return VMCI_SUCCESS;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_GetDomainName --
 *
 *      Returns the domain name of the given context.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VMCIContext_GetDomainName(VMCIId contextID,         // IN:
                          char *domainName,         // OUT:
                          size_t domainNameBufSize) // IN:
{
   VMCIContext *context;
   int rv = VMCI_SUCCESS;
   size_t domainNameLen;

   if (contextID == VMCI_INVALID_ID || !domainName || !domainNameBufSize) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   context = VMCIContext_Get(contextID);
   if (!context) {
      return VMCI_ERROR_NOT_FOUND;
   }

   domainNameLen = strlen(context->domainName);
   if (domainNameLen >= domainNameBufSize) {
      rv = VMCI_ERROR_NO_MEM;
      goto out;
   }

   memcpy(domainName, context->domainName, domainNameLen + 1);

out:
   VMCIContext_Release(context);
   return rv;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIContextID2HostVmID --
 *
 *      Maps a context ID to the host specific (process/world) ID
 *      of the VM/VMX.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VMCIContextID2HostVmID(VMCIId contextID,    // IN
                       void *hostVmID,      // OUT
                       size_t hostVmIDLen)  // IN
{
   VMCIContext *context;
   VMCIHostVmID vmID;
   int result;

   context = VMCIContext_Get(contextID);
   if (!context) {
      return VMCI_ERROR_NOT_FOUND;
   }
   
   result = VMCIHost_ContextToHostVmID(&context->hostContext, &vmID);
   if (result == VMCI_SUCCESS) {
      if (sizeof vmID == hostVmIDLen) {
         memcpy(hostVmID, &vmID, hostVmIDLen);
      } else {
         result = VMCI_ERROR_INVALID_ARGS;
      }
   }

   VMCIContext_Release(context);

   return result;
}

#endif


/*
 *----------------------------------------------------------------------
 *
 * VMCIContext_SupportsHostQP --
 *
 *      Can host QPs be connected to this user process.  The answer is
 *      FALSE unless a sufficient version number has previously been set
 *      by this caller.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
VMCIContext_SupportsHostQP(VMCIContext *context)    // IN: Context structure
{
#ifdef VMKERNEL
   return TRUE;
#else
   if (!context || context->userVersion < VMCI_VERSION_HOSTQP) {
      return FALSE;
   }
   return TRUE;
#endif
}

