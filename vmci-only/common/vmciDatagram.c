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
 * vmciDatagram.c --
 *
 *    This file implements the VMCI Simple Datagram API on the host. 
 */

#if defined(linux) && !defined(VMKERNEL)
#   include "driver-config.h"
#   define EXPORT_SYMTAB
#   define __NO_VERSION__
#   include "compat_module.h"
#   include <linux/string.h> /* memset() in the kernel */
#elif defined(WINNT_DDK)
#   include <ntddk.h>
#   include <string.h>
#elif !defined (__APPLE__) && !defined(VMKERNEL)
#   error "Unknown platform"
#endif

/* Must precede all vmware headers. */
#include "vmci_kernel_if.h"

#include "vm_assert.h"
#ifndef VMKERNEL
#  include "vmciHostKernelAPI.h"
#endif // VMKERNEL
#include "vmci_defs.h"
#include "vmci_call_defs.h"
#include "vmciCommonInt.h"
#include "vmciContext.h"
#include "vmci_infrastructure.h"
#include "vmciResource.h"
#ifndef VMX86_SERVER
#  include "vmciProcess.h"
#endif // VMX86_SERVER
#include "vmciGroup.h"
#include "vmciDatagram.h"
#include "vmciDriver.h"
#include "vmciDsInt.h"
#include "vmciHashtable.h"
#include "vmciEvent.h"
#include "vmware.h"

#define LGPFX "VMCIDatagram: "


/*
 * DatagramEntry describes the datagram entity. It is used for datagram
 * entities created only on the host.
 */
typedef struct DatagramEntry {
   VMCIResource        resource;
   uint32              flags;
   VMCIDatagramRecvCB  recvCB;
   void                *clientData;
   VMCIEvent           destroyEvent;
   VMCIPrivilegeFlags  privFlags;
} DatagramEntry;


/* Mapping between wellknown resource and context. */
typedef struct DatagramWKMapping {
   VMCIHashEntry entry;
   VMCIId        contextID;
} DatagramWKMapping;

/* Wellknown mapping hashtable. */
static VMCIHashTable *wellKnownTable = NULL;

static int VMCIDatagramGetPrivFlagsInt(VMCIId contextID, VMCIHandle handle,
                                       VMCIPrivilegeFlags *privFlags);
static void DatagramFreeCB(void *resource);
static int DatagramReleaseCB(void *clientData);
static DatagramWKMapping *DatagramGetWellKnownMap(VMCIId wellKnownID);
static void DatagramReleaseWellKnownMap(DatagramWKMapping *wkMap);
				     
#ifndef VMX86_SERVER
static int DatagramProcessNotifyCB(void *clientData, VMCIDatagram *msg);
#endif // !VMX86_SERVER

/*------------------------------ Helper functions ----------------------------*/


/*
 *------------------------------------------------------------------------------
 *
 *  DatagramFreeCB --
 *     Callback to free datagram structure when resource is no longer used,
 *     ie. the reference count reached 0.
 * 
 *  Result:
 *     None.
 *     
 *------------------------------------------------------------------------------
 */

static void
DatagramFreeCB(void *clientData)
{
   DatagramEntry *entry = (DatagramEntry *)clientData;
   VMCIResource *entryResource;
   ASSERT(entry);
   entryResource = &entry->resource;
   if (entryResource->registrationCount) {
         /* Remove all discovery service registrations for this resource. */
      VMCIDs_UnregisterResource(entryResource);
   }
   ASSERT(!entryResource->registrationCount);
   VMCI_SignalEvent(&entry->destroyEvent);

   /* 
    * The entry is freed in VMCIDatagram_DestroyHnd, who is waiting for the 
    * above signal. 
    */
}


/*
 *------------------------------------------------------------------------------
 *
 *  DatagramReleaseCB --
 *
 *     Callback to release the resource reference. It is called by the 
 *     VMCI_WaitOnEvent function before it blocks.
 * 
 *  Result:
 *     None.
 *     
 *------------------------------------------------------------------------------
 */

static int
DatagramReleaseCB(void *clientData)
{
   DatagramEntry *entry = (DatagramEntry *)clientData;
   ASSERT(entry);
   VMCIResource_Release(&entry->resource);
   return 0;
}


/*
 *------------------------------------------------------------------------------
 *
 * DatagramCreateHnd --
 *
 *      Internal function to create a datagram entry given a handle.
 *
 * Results:
 *      VMCI_SUCCESS if created, negative errno value otherwise.
 *
 * Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------
 */

static int
DatagramCreateHnd(VMCIId resourceID,            // IN:
		  uint32 flags,                 // IN:
		  VMCIPrivilegeFlags privFlags, // IN:
		  VMCIDatagramRecvCB recvCB,    // IN:
		  void *clientData,             // IN:
		  VMCIHandle *outHandle)        // OUT:

{
   int result;
   DatagramEntry *entry;
   VMCIResourcePrivilegeType validPriv = VMCI_PRIV_DG_SEND;
   VMCIHandle handle;

   ASSERT(recvCB != NULL);
   ASSERT(outHandle != NULL);
   ASSERT(!(privFlags & ~VMCI_PRIVILEGE_ALL_FLAGS));

   if ((flags & VMCI_FLAG_WELLKNOWN_DG_HND) != 0) {
      if (resourceID == VMCI_INVALID_ID) {
	 return VMCI_ERROR_INVALID_ARGS;
      }
      
      result = VMCIDatagramRequestWellKnownMap(resourceID,
					       VMCI_HOST_CONTEXT_ID,
					       privFlags);
      if (result < VMCI_SUCCESS) {
	 VMCILOG((LGPFX"Failed to reserve wellknown id %d, error %d.\n",
                  resourceID, result));
	 return result;
      }

      handle = VMCI_MAKE_HANDLE(VMCI_WELL_KNOWN_CONTEXT_ID, resourceID);
   } else {
      if (resourceID == VMCI_INVALID_ID) {
	 resourceID = VMCIResource_GetID();
      }
      handle = VMCI_MAKE_HANDLE(VMCI_HOST_CONTEXT_ID, resourceID);
   }

   entry = VMCI_AllocKernelMem(sizeof *entry, VMCI_MEMORY_NONPAGED);
   if (entry == NULL) {
      VMCILOG((LGPFX"Failed allocating memory for datagram entry.\n"));
      return VMCI_ERROR_NO_MEM;
   }

   entry->flags = flags;
   entry->recvCB = recvCB;
   entry->clientData = clientData;
   VMCI_CreateEvent(&entry->destroyEvent);
   entry->privFlags = privFlags;

   /* Make datagram resource live. */
   result = VMCIResource_Add(&entry->resource, VMCI_RESOURCE_TYPE_DATAGRAM,
                             handle, VMCI_MAKE_HANDLE(handle.context,
						      VMCI_CONTEXT_RESOURCE_ID),
                             1, &validPriv, DatagramFreeCB, entry);
   if (result != VMCI_SUCCESS) {
      VMCILOG((LGPFX"Failed to add new resource %d:%d.\n", 
               handle.context, handle.resource));
      VMCI_DestroyEvent(&entry->destroyEvent);
      VMCI_FreeKernelMem(entry, sizeof *entry);
      return result;
   }
   *outHandle = handle;

   return VMCI_SUCCESS;
}


/*------------------------ Userlevel Process functions -----------------------*/

#ifndef VMX86_SERVER
/*
 *------------------------------------------------------------------------------
 *
 *  DatagramProcessNotifyCB --
 *     Callback to send a datagram to a host vmci datagram process.
 * 
 *  Result:
 *     VMCI_SUCCESS on success, appropriate error code otherwise.
 *
 *  Side effects:
 *     Allocates memory.
 *     
 *------------------------------------------------------------------------------
 */

static int
DatagramProcessNotifyCB(void *clientData,  // IN:
                        VMCIDatagram *msg) // IN:
{
   VMCIDatagramProcess *dgmProc = (VMCIDatagramProcess *) clientData;
   size_t dgmSize;
   VMCIDatagram *dgm;
   DatagramQueueEntry *dqEntry;
   VMCILockFlags flags;
   
   ASSERT(dgmProc != NULL && msg != NULL);
   dgmSize = VMCI_DG_SIZE(msg);
   ASSERT(dgmSize <= VMCI_MAX_DG_SIZE);

   dgm = VMCI_AllocKernelMem(dgmSize, VMCI_MEMORY_NORMAL);
   if (!dgm) {
      VMCILOG((LGPFX"Failed to allocate datagram of size %"FMTSZ"d bytes.\n",
               dgmSize));
      return VMCI_ERROR_NO_MEM;
   }
   memcpy(dgm, msg, dgmSize);

   /* Allocate datagram queue entry and add it to the target fd's queue. */
   dqEntry = VMCI_AllocKernelMem(sizeof *dqEntry, VMCI_MEMORY_NONPAGED);
   if (dqEntry == NULL) {
      VMCILOG((LGPFX"Failed to allocate memory for process datagram.\n"));
      VMCI_FreeKernelMem(dgm, dgmSize);
      return VMCI_ERROR_NO_MEM;
   }
   dqEntry->dg = dgm;
   dqEntry->dgSize = dgmSize;

   VMCI_GrabLock(&dgmProc->lock, &flags);
   if (dgmProc->datagramQueueSize + dgmSize >= VMCI_MAX_DATAGRAM_QUEUE_SIZE) {
      VMCI_ReleaseLock(&dgmProc->lock, flags);
      VMCI_FreeKernelMem(dgm, dgmSize);
      VMCI_FreeKernelMem(dqEntry, sizeof *dqEntry);
      VMCILOGThrottled((LGPFX"Datagram process receive queue is full.\n"));
      return VMCI_ERROR_NO_RESOURCES;
   }

   LIST_QUEUE(&dqEntry->listItem, &dgmProc->datagramQueue);
   dgmProc->pendingDatagrams++;
   dgmProc->datagramQueueSize += dgmSize;
   VMCIHost_SignalCall(&dgmProc->host);
   VMCI_ReleaseLock(&dgmProc->lock, flags);

   VMCI_DEBUG_LOG((LGPFX"Sent datagram with resource id %d and size %"FMTSZ
                   "u.\n", msg->dst.resource, dgmSize));
   /* dqEntry and dgm are freed when user reads call.. */

   return VMCI_SUCCESS;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIDatagramProcess_Create --
 *
 *      Creates a new VMCIDatagramProcess object.
 *
 * Results:
 *      0 on success, appropriate error code otherwise.
 *
 * Side effects:
 *      Memory is allocated, deallocated in the destroy routine.
 *
 *----------------------------------------------------------------------
 */

int
VMCIDatagramProcess_Create(VMCIDatagramProcess **outDgmProc,    // OUT:
                           VMCIDatagramCreateInfo *createInfo,  // IN:
                           uintptr_t eventHnd)                  // IN:
{
   VMCIDatagramProcess *dgmProc;

   ASSERT(createInfo);
   ASSERT(outDgmProc);
   *outDgmProc = NULL;

   dgmProc = VMCI_AllocKernelMem(sizeof *dgmProc, VMCI_MEMORY_NONPAGED);
   if (!dgmProc) {
      VMCILOG((LGPFX"Failed to allocate memory for datagram fd.\n"));
      return VMCI_ERROR_NO_MEM;
   }

   /* Initialize state */
   VMCI_InitLock(&dgmProc->lock, "VMCIDatagramProcessLock", VMCI_LOCK_RANK_LOW);
   VMCIHost_InitContext(&dgmProc->host, eventHnd);
   dgmProc->pendingDatagrams = 0;
   dgmProc->datagramQueue = NULL;
   dgmProc->datagramQueueSize = 0;

   /*
    * We pass the result and corresponding handle to user level via the 
    * createInfo.
    */
   createInfo->result = DatagramCreateHnd(createInfo->resourceID,
					  createInfo->flags,
					  VMCI_DEFAULT_PROC_PRIVILEGE_FLAGS,
					  DatagramProcessNotifyCB,
					  (void *)dgmProc, &dgmProc->handle);
   if (createInfo->result < VMCI_SUCCESS) {
      VMCI_CleanupLock(&dgmProc->lock);
      VMCIHost_ReleaseContext(&dgmProc->host);
      VMCI_FreeKernelMem(dgmProc, sizeof *dgmProc);
      return createInfo->result;
   }
   createInfo->handle = dgmProc->handle;

   *outDgmProc = dgmProc;
   return VMCI_SUCCESS;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIDatagramProcess_Destroy --
 *
 *      Destroys a VMCIDatagramProcess object.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Memory allocated in create routine is deallocated here.
 *
 *----------------------------------------------------------------------
 */

void
VMCIDatagramProcess_Destroy(VMCIDatagramProcess *dgmProc) // IN:
{
   ListItem *curr, *next;
   DatagramQueueEntry *dqEntry;

   if (!dgmProc) {
      return;
   }

   if (!VMCI_HANDLE_EQUAL(dgmProc->handle, VMCI_INVALID_HANDLE)) {

      /* 
       * We block in destroy so we know that there can be no more 
       * callbacks to DatagramProcessNotifyCB when we return from
       * this call.
       */
      VMCIDatagramDestroyHndInt(dgmProc->handle);
      dgmProc->handle = VMCI_INVALID_HANDLE;
   }
   

   /*
    * Flush dgmProc's call queue. It is safe to deallocate the queue
    * as we are the last thread having a reference to the datagram
    * process.
    */

   LIST_SCAN_SAFE(curr, next, dgmProc->datagramQueue) {
      dqEntry = LIST_CONTAINER(curr, DatagramQueueEntry, listItem);
      LIST_DEL(curr, &dgmProc->datagramQueue);
      ASSERT(dqEntry && dqEntry->dg);
      ASSERT(dqEntry->dgSize == VMCI_DG_SIZE(dqEntry->dg));
      VMCI_FreeKernelMem(dqEntry->dg, dqEntry->dgSize);
      VMCI_FreeKernelMem(dqEntry, sizeof *dqEntry);
   }

   VMCI_CleanupLock(&dgmProc->lock);
   VMCIHost_ReleaseContext(&dgmProc->host);
   VMCI_FreeKernelMem(dgmProc, sizeof *dgmProc);
}

   
/*
 *----------------------------------------------------------------------
 *
 * VMCIDatagramProcess_ReadCall --
 *
 *      Dequeues the next guest call and returns it to caller if maxSize
 *      is not exceeded.
 *
 * Results:
 *      0 on success, appropriate error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VMCIDatagramProcess_ReadCall(VMCIDatagramProcess *dgmProc, // IN:
                             size_t maxSize,               // IN: max size handled by caller
                             VMCIDatagram **dg)            // OUT:
{
   DatagramQueueEntry *dqEntry;
   ListItem *listItem;
   VMCILockFlags flags;

   ASSERT(dgmProc);
   ASSERT(dg);

   /* Dequeue the next dgmProc datagram queue entry. */
   VMCI_GrabLock(&dgmProc->lock, &flags);

   /*
    * Currently, we do not support blocking read of datagrams on Mac and
    * Solaris. XXX: This will go away soon.
    */

#if defined(SOLARIS) || defined(__APPLE__)
   if (dgmProc->pendingDatagrams == 0) {
      VMCIHost_ClearCall(&dgmProc->host);
      VMCI_ReleaseLock(&dgmProc->lock, flags);
      VMCILOG((LGPFX"No datagrams pending.\n"));
      return VMCI_ERROR_NO_MORE_DATAGRAMS;
   }
#else
   while (dgmProc->pendingDatagrams == 0) {
      VMCIHost_ClearCall(&dgmProc->host);
      if (!VMCIHost_WaitForCallLocked(&dgmProc->host, &dgmProc->lock,
                                      &flags, FALSE)) {
         VMCI_ReleaseLock(&dgmProc->lock, flags);
         VMCILOG((LGPFX"Blocking read of datagram interrupted.\n"));
         return VMCI_ERROR_NO_MORE_DATAGRAMS;
      }
   }
#endif

   listItem = LIST_FIRST(dgmProc->datagramQueue);
   ASSERT (listItem != NULL);

   dqEntry = LIST_CONTAINER(listItem, DatagramQueueEntry, listItem);
   ASSERT(dqEntry->dg);

   /* Check the size of the userland buffer. */
   if (maxSize < dqEntry->dgSize) {
      VMCI_ReleaseLock(&dgmProc->lock, flags);
      VMCILOG((LGPFX"Caller's buffer is too small.\n"));
      return VMCI_ERROR_NO_MEM;
   }
   
   LIST_DEL(listItem, &dgmProc->datagramQueue);
   dgmProc->pendingDatagrams--;
   dgmProc->datagramQueueSize -= dqEntry->dgSize;
   if (dgmProc->pendingDatagrams == 0) {
      VMCIHost_ClearCall(&dgmProc->host);
   }
   VMCI_ReleaseLock(&dgmProc->lock, flags);

   ASSERT(dqEntry->dgSize == VMCI_DG_SIZE(dqEntry->dg));
   *dg = dqEntry->dg;
   VMCI_FreeKernelMem(dqEntry, sizeof *dqEntry);

   return VMCI_SUCCESS;
}
#endif // !VMX86_SERVER

/*------------------------------ Init functions ----------------------------*/

/*
 *------------------------------------------------------------------------------
 *
 *  VMCIDatagram_Init --
 *
 *     Initialize Datagram API, ie. register the API functions with their
 *     corresponding vectors.
 *
 *  Result:
 *     None.
 *     
 * Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------
 */

int
VMCIDatagram_Init(void)
{
   /* Create hash table for wellknown mappings. */
   wellKnownTable = VMCIHashTable_Create(32);
   if (wellKnownTable == NULL) {
      return VMCI_ERROR_NO_RESOURCES;
   }

   return VMCI_SUCCESS;
}


/*
 *------------------------------------------------------------------------------
 *
 *  VMCIDatagram_Exit --
 *
 *     Cleanup Datagram API.
 *
 *  Result:
 *     None.
 *     
 * Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------
 */

void
VMCIDatagram_Exit(void)
{
   if (wellKnownTable != NULL) {
      VMCIHashTable_Destroy(wellKnownTable);
      wellKnownTable = NULL;
   }
}


/*------------------------------ Public API functions ----------------------------*/

/*
 *------------------------------------------------------------------------------
 *
 * VMCIDatagramCreateHndInt --
 *
 *      Internal function to create a host context datagram endpoint and 
 *	returns a handle to it.
 *
 * Results:
 *      VMCI_SUCCESS if created, negative errno value otherwise.
 *
 * Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------
 */

int
VMCIDatagramCreateHndInt(VMCIId resourceID,         // IN: Optional, generated 
                                                    //     if VMCI_INVALID_ID
                         uint32 flags,              // IN:
                         VMCIDatagramRecvCB recvCB, // IN:
                         void *clientData,          // IN:
	                 VMCIHandle *outHandle)     // OUT:newly created handle
{
   if (outHandle == NULL) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   if (recvCB == NULL) {
      VMCILOG((LGPFX"Client callback needed when creating datagram.\n"));
      return VMCI_ERROR_INVALID_ARGS;
   }

   return DatagramCreateHnd(resourceID, flags, VMCI_DEFAULT_PROC_PRIVILEGE_FLAGS,
			    recvCB, clientData, outHandle);
}

#ifndef VMKERNEL
/*
 *------------------------------------------------------------------------------
 *
 * VMCIDatagram_CreateHnd --
 *
 *      Creates a host context datagram endpoint and returns a handle to it.
 *
 * Results:
 *      VMCI_SUCCESS if created, negative errno value otherwise.
 *
 * Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------
 */

#if defined(linux)
EXPORT_SYMBOL(VMCIDatagram_CreateHnd);
#endif

int
VMCIDatagram_CreateHnd(VMCIId resourceID,          // IN: Optional, generated 
                                                   //     if VMCI_INVALID_ID
                       uint32 flags,               // IN:
                       VMCIDatagramRecvCB recvCB,  // IN:
                       void *clientData,           // IN:
		       VMCIHandle *outHandle)      // OUT: newly created handle
{
   return VMCIDatagramCreateHndInt(resourceID, flags, recvCB, clientData,
                                   outHandle);
}
#endif	/* !VMKERNEL  */


/*
 *------------------------------------------------------------------------------
 *
 * VMCIDatagramCreateHndPriv --
 *
 *      Creates a host context datagram endpoint and returns a handle to it.
 *
 * Results:
 *      VMCI_SUCCESS if created, negative errno value otherwise.
 *
 * Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------
 */

int
VMCIDatagramCreateHndPriv(VMCIId resourceID,           // IN: Optional, generated
			                               //     if VMCI_INVALID_ID
			  uint32 flags,                // IN:
			  VMCIPrivilegeFlags privFlags,// IN:
			  VMCIDatagramRecvCB recvCB,   // IN:
			  void *clientData,            // IN:
			  VMCIHandle *outHandle)       // OUT: newly created handle
{
   if (outHandle == NULL) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   if (recvCB == NULL) {
      VMCILOG((LGPFX"Client callback needed when creating datagram.\n"));
      return VMCI_ERROR_INVALID_ARGS;
   }

   if (privFlags & ~VMCI_PRIVILEGE_ALL_FLAGS) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   return DatagramCreateHnd(resourceID, flags, privFlags, recvCB, clientData,
			    outHandle);
}


#ifndef VMKERNEL
/*
 *------------------------------------------------------------------------------
 *
 * VMCIDatagram_CreateHndPriv --
 *
 *      Creates a host context datagram endpoint and returns a handle to it.
 *
 * Results:
 *      VMCI_SUCCESS if created, negative errno value otherwise.
 *
 * Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------
 */

#if defined(linux)
EXPORT_SYMBOL(VMCIDatagram_CreateHndPriv);
#endif

int
VMCIDatagram_CreateHndPriv(VMCIId resourceID,           // IN: Optional, generated
			                                //     if VMCI_INVALID_ID
			   uint32 flags,                // IN:
			   VMCIPrivilegeFlags privFlags,// IN:
			   VMCIDatagramRecvCB recvCB,   // IN:
			   void *clientData,            // IN:
			   VMCIHandle *outHandle)       // OUT: newly created handle
{
   return VMCIDatagramCreateHndPriv(resourceID, flags, privFlags, recvCB,
                                    clientData, outHandle);
}
#endif	/* !VMKERNEL  */


/*
 *------------------------------------------------------------------------------
 *
 * VMCIDatagramDestroyHndInt --
 *
 *      Destroys a handle.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------
 */

int
VMCIDatagramDestroyHndInt(VMCIHandle handle)       // IN
{
   DatagramEntry *entry;
   VMCIResource *resource = VMCIResource_Get(handle, 
 					     VMCI_RESOURCE_TYPE_DATAGRAM);
   if (resource == NULL) {
      VMCILOG((LGPFX"Failed to destroy handle 0x%x:0x%x.\n",
               handle.context, handle.resource));
      return VMCI_ERROR_NOT_FOUND;
   }
   entry = RESOURCE_CONTAINER(resource, DatagramEntry, resource);
   
   VMCIResource_Remove(handle, VMCI_RESOURCE_TYPE_DATAGRAM);
   
   /*
    * We now wait on the destroyEvent and release the reference we got
    * above.
    */
   VMCI_WaitOnEvent(&entry->destroyEvent, DatagramReleaseCB, entry);
   
   if ((entry->flags & VMCI_FLAG_WELLKNOWN_DG_HND) != 0) {
      VMCIDatagramRemoveWellKnownMap(handle.resource, VMCI_HOST_CONTEXT_ID);
   }

   /* 
    * We know that we are now the only reference to the above entry so
     * can safely free it.
     */
   VMCI_DestroyEvent(&entry->destroyEvent);
   VMCI_FreeKernelMem(entry, sizeof *entry);
 
   return VMCI_SUCCESS;
}


#ifndef VMKERNEL
/*
 *------------------------------------------------------------------------------
 *
 * VMCIDatagram_DestroyHnd --
 *
 *      Destroys a handle.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------
 */

#if defined(linux)
EXPORT_SYMBOL(VMCIDatagram_DestroyHnd);
#endif

int
VMCIDatagram_DestroyHnd(VMCIHandle handle)       // IN
{
   return VMCIDatagramDestroyHndInt(handle);
}
#endif	/* !VMKERNEL  */


/*
 *------------------------------------------------------------------------------
 *
 *  VMCIDatagramGetPrivFlagsInt --
 *
 *     Internal utilility function with the same purpose as
 *     VMCIDatagram_GetPrivFlags that also takes a contextID.
 *
 *  Result:
 *     VMCI_SUCCESS on success, VMCI_ERROR_INVALID_ARGS if handle is invalid.
 *     
 *  Side effects:
 *     None.
 *     
 *------------------------------------------------------------------------------
 */

static int
VMCIDatagramGetPrivFlagsInt(VMCIId contextID,              // IN
                            VMCIHandle handle,             // IN
                            VMCIPrivilegeFlags *privFlags) // OUT
{
   ASSERT(privFlags);
   ASSERT(contextID != VMCI_INVALID_ID);

   if (contextID == VMCI_HOST_CONTEXT_ID) {
      DatagramEntry *srcEntry;
      VMCIResource *resource;

      resource = VMCIResource_Get(handle, VMCI_RESOURCE_TYPE_DATAGRAM);
      if (resource == NULL) {
	 return VMCI_ERROR_INVALID_ARGS;
      }
      srcEntry = RESOURCE_CONTAINER(resource, DatagramEntry, resource);
      *privFlags = srcEntry->privFlags;
      VMCIResource_Release(resource);
   } else if (contextID == VMCI_HYPERVISOR_CONTEXT_ID) {
      *privFlags = VMCI_MAX_PRIVILEGE_FLAGS;
   } else {
      *privFlags = VMCIContext_GetPrivFlagsInt(contextID);
   }

   return VMCI_SUCCESS;
}


/*
 *------------------------------------------------------------------------------
 *
 *  VMCIDatagram_GetPrivFlags --
 *
 *     Utilility function that retrieves the privilege flags
 *     associated with a given datagram handle. For hypervisor and
 *     guest endpoints, the privileges are determined by the context
 *     ID, but for host endpoints privileges are associated with the
 *     complete handle.
 *
 *  Result:
 *     VMCI_SUCCESS on success, VMCI_ERROR_INVALID_ARGS if handle is invalid.
 *     
 *  Side effects:
 *     None.
 *     
 *------------------------------------------------------------------------------
 */

int
VMCIDatagram_GetPrivFlags(VMCIHandle handle,             // IN
                          VMCIPrivilegeFlags *privFlags) // OUT
{
   if (privFlags == NULL || handle.context == VMCI_INVALID_ID) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   return VMCIDatagramGetPrivFlagsInt(handle.context, handle, privFlags);
}

/*
 *------------------------------------------------------------------------------
 *
 *  VMCIDatagram_Dispatch --
 *
 *     Dispatch datagram to host or other vm context. This function cannot
 *     dispatch to hypervisor context handlers. This should have been handled
 *     before we get here by VMCIDatagramDispatch.
 *
 *  Result:
 *     Number of bytes sent on success, appropriate error code otherwise.
 *     
 *  Side effects:
 *     None.
 *     
 *------------------------------------------------------------------------------
 */

int 
VMCIDatagram_Dispatch(VMCIId contextID,  // IN:
		      VMCIDatagram *dg)  // IN:
{
   int retval = 0;
   size_t dgSize;
   VMCIId dstContext;
   VMCIPrivilegeFlags srcPrivFlags;
   char srcDomain[VMCI_DOMAIN_NAME_MAXLEN]; /* Not used on hosted. */
   char dstDomain[VMCI_DOMAIN_NAME_MAXLEN]; /* Not used on hosted. */

   ASSERT(dg);
   ASSERT_ON_COMPILE(sizeof(VMCIDatagram) == 24);
   dgSize = VMCI_DG_SIZE(dg);

   if (dgSize > VMCI_MAX_DG_SIZE) {
      VMCILOG((LGPFX"Invalid args.\n"));
      return VMCI_ERROR_INVALID_ARGS;
   }

   if (contextID == VMCI_HOST_CONTEXT_ID &&
       dg->dst.context == VMCI_HYPERVISOR_CONTEXT_ID) {
      return VMCI_ERROR_DST_UNREACHABLE;
   }
   
   ASSERT(dg->dst.context != VMCI_HYPERVISOR_CONTEXT_ID);   

   VMCI_DEBUG_LOG((LGPFX"Sending from handle 0x%x:0x%x to handle 0x%x:0x%x, "
                   "datagram size %u.\n",
                   dg->src.context, dg->src.resource, 
                   dg->dst.context, dg->dst.resource, (uint32)dgSize));

   /* 
    * Check that source handle matches sending context.
    */
   if (dg->src.context != contextID) {
      if (dg->src.context == VMCI_WELL_KNOWN_CONTEXT_ID) {
	 /* Determine mapping. */
	 DatagramWKMapping *wkMap = DatagramGetWellKnownMap(dg->src.resource);
	 if (wkMap == NULL) {
	    VMCILOG((LGPFX"Sending from invalid well-known resource id "
                     "0x%x:0x%x.\n", dg->src.context, dg->src.resource));
	    return VMCI_ERROR_INVALID_RESOURCE;
	 }
	 if (wkMap->contextID != contextID) {
	    VMCILOG((LGPFX"Sender context 0x%x is not owner of well-known "
                     "src datagram entry with handle 0x%x:0x%x.\n", 
                     contextID, dg->src.context, dg->src.resource));
	    DatagramReleaseWellKnownMap(wkMap);
	    return VMCI_ERROR_NO_ACCESS;
	 }
	 DatagramReleaseWellKnownMap(wkMap);
      } else {
	 VMCILOG((LGPFX"Sender context 0x%x is not owner of src datagram "
                  "entry with handle 0x%x:0x%x.\n", 
                  contextID, dg->src.context, dg->src.resource));
	 return VMCI_ERROR_NO_ACCESS;
      }
   }
   
   if (dg->dst.context == VMCI_WELL_KNOWN_CONTEXT_ID) {
      /* Determine mapping. */
      DatagramWKMapping *wkMap = DatagramGetWellKnownMap(dg->dst.resource);
      if (wkMap == NULL) {
	 VMCILOG((LGPFX"Sending to invalid wellknown destination 0x%x:0x%x.\n",
                  dg->dst.context, dg->dst.resource));
	 return VMCI_ERROR_DST_UNREACHABLE;
      }
      dstContext = wkMap->contextID;
      DatagramReleaseWellKnownMap(wkMap);
   } else {
      dstContext = dg->dst.context;
   }


   /*
    * Get hold of privileges of sending endpoint.
    */
   
   retval = VMCIDatagramGetPrivFlagsInt(contextID, dg->src, &srcPrivFlags);
   if (retval != VMCI_SUCCESS) {
      VMCILOG((LGPFX"Couldn't get privileges for handle 0x%x:0x%x.\n",
               dg->src.context, dg->src.resource));
      return retval;
   }
   
#ifdef VMKERNEL
   /*
    * In the vmkernel, all communicating contexts except the
    * hypervisor context must belong to the same domain. If the
    * hypervisor is the source, the domain doesn't matter.
    */

   if (contextID != VMCI_HYPERVISOR_CONTEXT_ID) {
      retval = VMCIContext_GetDomainName(contextID, srcDomain,
                                         sizeof srcDomain);
      if (retval < VMCI_SUCCESS) {
         VMCILOG((LGPFX"Failed to get domain name for context %u.\n",
                  contextID));
         return retval;
      }
   }
#endif


   /* Determine if we should route to host or guest destination. */
   if (dstContext == VMCI_HOST_CONTEXT_ID) {
      /* Route to host datagram entry. */
      DatagramEntry *dstEntry;
      VMCIResource *resource;

      if (dg->src.context == VMCI_HYPERVISOR_CONTEXT_ID && 
          dg->dst.resource == VMCI_EVENT_HANDLER) {
         return VMCIEvent_Dispatch(dg);
      }

      resource = VMCIResource_Get(dg->dst, VMCI_RESOURCE_TYPE_DATAGRAM);
      if (resource == NULL) {
	 VMCILOG((LGPFX"Sending to invalid destination handle 0x%x:0x%x.\n",
                  dg->dst.context, dg->dst.resource));
	 return VMCI_ERROR_INVALID_ARGS;
      }
      dstEntry = RESOURCE_CONTAINER(resource, DatagramEntry, resource);
#ifdef VMKERNEL
      retval = VMCIContext_GetDomainName(VMCI_HOST_CONTEXT_ID, dstDomain,
                                         sizeof dstDomain);
      if (retval < VMCI_SUCCESS) {
         VMCILOG((LGPFX"Failed to get domain name for context %u.\n",
                  VMCI_HOST_CONTEXT_ID));
         VMCIResource_Release(resource);
         return retval;
      }
#endif
      if (VMCIDenyInteraction(srcPrivFlags, dstEntry->privFlags, srcDomain,
                              dstDomain)) {
	 VMCIResource_Release(resource);
	 return VMCI_ERROR_NO_ACCESS;
      }
      ASSERT(dstEntry->recvCB);
      retval = dstEntry->recvCB(dstEntry->clientData, dg);
      VMCIResource_Release(resource);
      if (retval < VMCI_SUCCESS) {
	 return retval;
      }
   } else {
      /* Route to destination VM context. */
      VMCIDatagram *newDG;

#ifdef VMKERNEL
      retval = VMCIContext_GetDomainName(dstContext, dstDomain,
                                         sizeof dstDomain);
      if (retval < VMCI_SUCCESS) {
         VMCILOG((LGPFX"Failed to get domain name for context %u.\n",
                  dstContext));
         return retval;
      }
#endif
      if (contextID != dstContext && 
         VMCIDenyInteraction(srcPrivFlags, VMCIContext_GetPrivFlagsInt(dstContext),
                             srcDomain, dstDomain)) {
	 return VMCI_ERROR_NO_ACCESS;
      }

      /* We make a copy to enqueue. */
#ifdef _WIN32
      newDG = VMCI_AllocKernelMem(dgSize, VMCI_MEMORY_NONPAGED);
#else // Linux, Mac OS, ESX cases below
      newDG = VMCI_AllocKernelMem(dgSize, VMCI_MEMORY_NORMAL);
#endif // _WIN32

      if (newDG == NULL) {
	 return VMCI_ERROR_NO_MEM;
      }
      memcpy(newDG, dg, dgSize);
      retval = 
	 VMCIContext_EnqueueDatagram(dstContext, newDG);
      if (retval < VMCI_SUCCESS) {
	 VMCI_FreeKernelMem(newDG, dgSize);
	 return retval;
      }
   }
   /* The datagram is freed when the context reads it. */
   VMCI_DEBUG_LOG((LGPFX"Sent datagram of size %u.\n", dgSize));

   /*
    * We currently truncate the size to signed 32 bits. This doesn't 
    * matter for this handler as it only support 4Kb messages. 
    */
   return (int)dgSize;
}


/*
 *------------------------------------------------------------------------------
 *
 * VMCIDatagramSendInt --
 *
 *      Sends the payload to the destination datagram handle. Sending
 *      datagrams to the hypervisor context is not supported for the
 *      host context.
 *
 * Results:
 *      Returns number of bytes sent if success, or error code if failure.
 *
 * Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------
 */

int
VMCIDatagramSendInt(VMCIDatagram *msg) // IN
{
   if (msg == NULL) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   /*
    * This function is part of the kernel API and only used by host
    * context endpoints. These endpoints should never send datagrams
    * to the hypervisor.
    */

   if (msg->dst.context == VMCI_HYPERVISOR_CONTEXT_ID) {
      return VMCI_ERROR_DST_UNREACHABLE;
   }

   return VMCIDatagram_Dispatch(VMCI_HOST_CONTEXT_ID, msg);
}


#ifndef VMKERNEL
/*
 *------------------------------------------------------------------------------
 *
 * VMCIDatagram_Send --
 *
 *      Sends the payload to the destination datagram handle. Sending
 *      datagrams to the hypervisor context is not supported for the
 *      host context.
 *
 * Results:
 *      Returns number of bytes sent if success, or error code if failure.
 *
 * Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------
 */

#if defined(linux)
EXPORT_SYMBOL(VMCIDatagram_Send);
#endif

int
VMCIDatagram_Send(VMCIDatagram *msg) // IN
{
   return VMCIDatagramSendInt(msg);
}
#endif	/* !VMKERNEL  */


/*
 *------------------------------------------------------------------------------
 *
 * DatagramGetWellKnownMap --
 *
 *      Gets a mapping between handle and wellknown resource.
 *
 * Results:
 *      DatagramWKMapping * if found, NULL if not.
 *
 * Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------
 */

static DatagramWKMapping *
DatagramGetWellKnownMap(VMCIId wellKnownID)  // IN:
{
   VMCIHashEntry *entry;
   DatagramWKMapping *wkMap = NULL;
   VMCIHandle wkHandle = VMCI_MAKE_HANDLE(VMCI_WELL_KNOWN_CONTEXT_ID,
					  wellKnownID);
   entry = VMCIHashTable_GetEntry(wellKnownTable, wkHandle);
   if (entry != NULL) {
      wkMap = RESOURCE_CONTAINER(entry, DatagramWKMapping, entry);
   }
   return wkMap;
}


/*
 *------------------------------------------------------------------------------
 *
 * DatagramReleaseWellKnownMap --
 *
 *      Releases a wellknown mapping.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------
 */

static void
DatagramReleaseWellKnownMap(DatagramWKMapping *wkMap)  // IN:
{
   if (VMCIHashTable_ReleaseEntry(wellKnownTable, &wkMap->entry) == 
       VMCI_SUCCESS_ENTRY_DEAD) {
      VMCI_FreeKernelMem(wkMap, sizeof *wkMap);
   }
}


/*
 *------------------------------------------------------------------------------
 *
 * VMCIDatagramRequestWellKnownMap --
 *
 *      Creates a mapping between handle and wellknown resource. If resource
 *      is already used we fail the request.
 *
 * Results:
 *      VMCI_SUCCESS if created, negative errno value otherwise.
 *
 * Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------
 */

int
VMCIDatagramRequestWellKnownMap(VMCIId wellKnownID,           // IN:
                                VMCIId contextID,             // IN:
                                VMCIPrivilegeFlags privFlags) // IN:
{
   int result;
   DatagramWKMapping *wkMap;
   VMCIHandle wkHandle = VMCI_MAKE_HANDLE(VMCI_WELL_KNOWN_CONTEXT_ID,
					  wellKnownID);

   if (privFlags & VMCI_PRIVILEGE_FLAG_RESTRICTED ||
       !VMCIWellKnownID_AllowMap(wellKnownID, privFlags)) {
      return VMCI_ERROR_NO_ACCESS;
   }       

   wkMap = VMCI_AllocKernelMem(sizeof *wkMap, VMCI_MEMORY_NONPAGED);
   if (wkMap == NULL) {
      return VMCI_ERROR_NO_MEM;
   }

   VMCIHashTable_InitEntry(&wkMap->entry, wkHandle);
   wkMap->contextID = contextID;

   /* Fails if wkHandle (wellKnownID) already exists. */
   result = VMCIHashTable_AddEntry(wellKnownTable, &wkMap->entry);
   if (result != VMCI_SUCCESS) {
      VMCI_FreeKernelMem(wkMap, sizeof *wkMap);
      return result;
   }
   result = VMCIContext_AddWellKnown(contextID, wellKnownID);
   if (UNLIKELY(result < VMCI_SUCCESS)) {
      VMCIHashTable_RemoveEntry(wellKnownTable, &wkMap->entry);
      VMCI_FreeKernelMem(wkMap, sizeof *wkMap);
   }
   return result;
}


/*
 *------------------------------------------------------------------------------
 *
 * VMCIDatagramRemoveWellKnownMap --
 *
 *      Removes a mapping between handle and wellknown resource. Checks if
 *      mapping belongs to calling context.
 *
 * Results:
 *      VMCI_SUCCESS if removed, negative errno value otherwise.
 *
 * Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------
 */

int
VMCIDatagramRemoveWellKnownMap(VMCIId wellKnownID,  // IN:
			       VMCIId contextID)    // IN:
{
   int result = VMCI_ERROR_NO_ACCESS;
   DatagramWKMapping *wkMap = DatagramGetWellKnownMap(wellKnownID);
   if (wkMap == NULL) {
      VMCILOG((LGPFX"Failed to remove well-known mapping between resource "
               "%d and context %d.\n", wellKnownID, contextID));
      return VMCI_ERROR_NOT_FOUND;
   }

   if (contextID == wkMap->contextID) {
      VMCIHashTable_RemoveEntry(wellKnownTable, &wkMap->entry);
      VMCIContext_RemoveWellKnown(contextID, wellKnownID);
      result = VMCI_SUCCESS;
   }
   DatagramReleaseWellKnownMap(wkMap);
   return result;   
}
