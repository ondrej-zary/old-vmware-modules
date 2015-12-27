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

/* Implementation of the VMCI discovery service.
 *
 * In its current incarnation this is a simple list.
 */


#if defined(linux) && !defined(VMKERNEL)
#   include "driver-config.h"
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
#include "vmci_defs.h"
#include "vmci_infrastructure.h"
#include "vmciContext.h"
#ifndef VMX86_SERVER
#  include "vmciProcess.h"
#endif // !VMX86_SERVER
#include "vmciGroup.h"
#include "vmciDatagram.h"
#include "vmware.h"
#include "vmciDsInt.h"
#include "vmciDriver.h"
#include "vmciCommonInt.h"

#define LGPFX "VMCIDs: "


/* Local Types */

typedef struct DsListElement {
   char *name;
   VMCIHandle handle;
   VMCIId contextID;
} DsListElement;

typedef struct DsList {
   int size;
   int capacity;
   DsListElement *elements; 
   Bool isInitialized;
} DsList;


/* Forward declarations */

static int  DsHandleMessage(const int8 *inBuffer, int8 *outBuffer,int outSize, 
                            int *written, VMCIId contextID,
                            VMCIPrivilegeFlags privFlags);
static void DsSetReplyStatus(VMCIDsReplyHeader *reply, int code, const char *msg, 
                             int *written);
static void DsLookupAction(const char *name, VMCIDsReplyHeader *reply,
                           int *written);
static void DsRegisterAction(const char *name, VMCIHandle handle,
                             VMCIDsReplyHeader *reply, 
                             int *written, VMCIId contextID);
static void DsUnregisterAction(const char *name, VMCIDsReplyHeader *reply,
                               int *written, VMCIId contextID);
static Bool DsListInit(DsList **list, int capacity);
static void DsListDestroy(DsList *list);
static int  DsListLookup(const DsList *list, const char *name,
                         VMCIHandle *out);
static int  DsListInsert(DsList *list, const char *name, VMCIHandle handle,
                         VMCIId contextID);
static int  DsListRemove(DsList *list, const char *name,
                         VMCIHandle *handleOut, VMCIId contextID);
static int  DsListLookupIndex(const DsList *list, const char *name);
static int  DsRequestCb(void *notifyData, VMCIDatagram *msg);
static int  DsListRemoveResource(DsList *list, VMCIResource *resource);
static int  DsListRemoveElement(DsList *list, int index);
static void DsRemoveRegistrationsContext(VMCIId contextID);

/* Global variables */

/* Struct used to represent the cds implementation */
static struct dsAPI {
   DsList *registry;
   VMCIHandle handle;
   VMCIHandle groupHandle;
   Bool isInitialized;
} dsAPI;
static VMCILock lock;


/*
 *-------------------------------------------------------------------------
 *
 *  DsRequestCb --
 *
 *    Main entry point to the discovery service. Deserialize the requst,
 *    performs it, and returns the result in seralized form.
 *
 *  Result:
 *    Returns number of bytes sent on success, and error code on failure.
 *     
 *  Side effects:
 *    None.
 *
 *-------------------------------------------------------------------------
 */

static int
DsRequestCb(void *notifyData,  // IN: callback data
            VMCIDatagram *msg) // IN: datagram 
{
   /* FIXME: On-stack 300byte buffer is no-no.  Besides that it is ignored anyway. */
   char replyBuffer[VMCI_DS_MAX_MSG_SIZE + sizeof(VMCIDatagram)];
   VMCIDatagram *replyMsg = (VMCIDatagram *)replyBuffer;
   int written, retval;
   VMCIPrivilegeFlags srcPrivFlags;

   VMCI_DEBUG_LOG((LGPFX"Got request from context: %d\n", msg->src.context));

   if (VMCIDatagram_GetPrivFlags(msg->src, &srcPrivFlags) != VMCI_SUCCESS) {
      retval = VMCI_ERROR_INVALID_ARGS;
      goto done;
   }
   replyMsg->dst = msg->src;
   replyMsg->src = dsAPI.handle;
   DsHandleMessage(VMCI_DG_PAYLOAD(msg), VMCI_DG_PAYLOAD(replyMsg), 
                   VMCI_DS_MAX_MSG_SIZE, &written, msg->src.context,
                   srcPrivFlags);
   replyMsg->payloadSize = written;

   /* Send reply back to source handle. */
   retval = VMCIDatagramSendInt(replyMsg);
done:
   if (retval >= VMCI_SUCCESS) {
      VMCI_DEBUG_LOG((LGPFX"Successfully replied with %d bytes\n", written));
   } else {
      VMCILOG((LGPFX"Failed to reply to request: %d.\n", retval));
   }

   return retval;
}


/*
 *-------------------------------------------------------------------------
 *
 *  DsHandleMessage
 *
 *    Deserialize the request, performs it,  and constructs a
 *    reply.
 *
 *  Result:
 *     Error code. 0 if success.
 *     
 *  Side effects:
 *     None.
 *
 *-------------------------------------------------------------------------
 */

static int
DsHandleMessage(const int8 *inBuffer,         // IN:
                int8 *outBuffer,              // IN:
                int outSize,                  // IN:
                int *written,                 // IN:
                VMCIId contextID,             // IN:
                VMCIPrivilegeFlags privFlags) // IN:
{
   const VMCIDsRequestHeader *request = (const VMCIDsRequestHeader*)inBuffer;
   VMCIDsReplyHeader *reply = (VMCIDsReplyHeader*)outBuffer;
   
   
   /** Initialize reply */
   if (outSize < VMCI_DS_MAX_MSG_SIZE) {
      return VMCI_ERROR_GENERIC;
   }

   /* Initialize reply structure */
   reply->msgid  = request->msgid;
   reply->handle = VMCI_INVALID_HANDLE;

   /*
    * Disable registration/unregistration check for developer builds,
    * as this functionality is useful for testing.
    */

#ifndef VMX86_DEVEL
   if (request->action != VMCI_DS_ACTION_LOOKUP &&
       !(privFlags & VMCI_PRIVILEGE_FLAG_TRUSTED)) {
      /* 
       * Only trusted entities are allowed to perform operations other
       * than lookup.
       */
      
      DsSetReplyStatus(reply, VMCI_ERROR_NO_ACCESS, "access denied", written);      
      return VMCI_SUCCESS;
   }
#endif

   // Make sure reply is initialized
   DsSetReplyStatus(reply, VMCI_ERROR_GENERIC, "general failure", written);
   
   switch(request->action) {
   case VMCI_DS_ACTION_LOOKUP:
      DsLookupAction(request->name, reply, written);
      break;
      
   case VMCI_DS_ACTION_REGISTER:
      DsRegisterAction(request->name, request->handle, reply, written,
                       contextID);
      break;
      
   case VMCI_DS_ACTION_UNREGISTER:
      DsUnregisterAction(request->name, reply, written, contextID);
      break;
      
   default:
      DsSetReplyStatus(reply, VMCI_ERROR_GENERIC, "unknown action", written);
      break;
   }
   
   /* We successfully generated a reply, which contain the real error code */
   return VMCI_SUCCESS;   
}


/*
 *-------------------------------------------------------------------------
 *
 *  DsSetReplyStatus --
 *
 *    Inserts an error code and message into a reply buffer
 *
 *  Result:
 *     None
 *     
 *  Side effects:
 *     None.
 *
 *-------------------------------------------------------------------------
 */

static void 
DsSetReplyStatus(VMCIDsReplyHeader *reply, // IN
                 int code,                 // IN
                 const char *msg,          // IN
                 int *written)             // OUT: Length of reply
{
   int len = strlen(msg);

   reply->code = code;
   reply->msgLen = len;
   memcpy(reply->msg, msg, len + 1);

   *written = sizeof(VMCIDsReplyHeader) + len + 1;
}


/*
 *-------------------------------------------------------------------------
 *
 *  DsLookupAction --
 *
 *    Looks up a key in the registry
 *
 *  Result:
 *     None
 *     
 *  Side effects:
 *     None.
 *
 *-------------------------------------------------------------------------
 */

static void 
DsLookupAction(const char *name,          // IN: Action name
               VMCIDsReplyHeader *reply,  // OUT: Answer
               int *written)              // OUT: Length of answer
{
   VMCIHandle handle = VMCI_INVALID_HANDLE;
   int errcode;
   VMCILockFlags flags;

   VMCI_GrabLock(&lock, &flags);
   errcode = DsListLookup(dsAPI.registry, name, &handle /* out */);
   VMCI_ReleaseLock(&lock, flags);

   reply->handle = handle;
   DsSetReplyStatus(reply, errcode, "", written);
}



/*
 *-----------------------------------------------------------------------------
 *
 * VMCIDs_Register --
 *
 *      Registers a (key, handle) in the discovery service.
 *
 * Results:
 *      VMCI_SUCCESS if successful, otherwise an error code is returned.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

int
VMCIDs_Register(const char* name,   // IN:
                VMCIHandle handle,  // IN:
                VMCIId contextID)   // IN:
{
   int errcode;
   VMCILockFlags flags;

   VMCI_GrabLock(&lock, &flags);
   errcode = DsListInsert(dsAPI.registry, name, handle, contextID);
   VMCI_ReleaseLock(&lock, flags);
   if (errcode == VMCI_SUCCESS) {
      VMCIResource *resource = VMCIResource_Get(handle, VMCI_RESOURCE_TYPE_ANY);
      if (resource) {
         VMCIResource_IncDsRegCount(resource);
         VMCIResource_Release(resource);
      }
   }
   return errcode;
}


/*
 *-------------------------------------------------------------------------
 *
 *  DsRegisterAction --
 *
 *    Registers a (key, handle) in the discovery service
 *
 *  Result:
 *     None
 *     
 *  Side effects:
 *     None.
 *
 *-------------------------------------------------------------------------
 */


static void 
DsRegisterAction(const char *name,          // IN: Action name
                 VMCIHandle handle,         // IN: Action handle
                 VMCIDsReplyHeader *reply,  // OUT: Reply
                 int *written,              // OUT: Reply length
                 VMCIId contextID)          // IN: Id of requesting context
{
   int errcode = VMCIDs_Register(name, handle, contextID);
   DsSetReplyStatus(reply, errcode, "", written);
}

/*
 *-------------------------------------------------------------------------
 *
 *  VMCIDs_UnregisterResource --
 *
 *    Unregisters a resource from the discovery service.
 *
 *  Result:
 *    Number of registrations removed, error code on failure.
 *     
 *  Side effects:
 *    None.
 *
 *-------------------------------------------------------------------------
 */

int
VMCIDs_UnregisterResource(VMCIResource *resource) // IN:
{
   int rv = 0;
   VMCILockFlags flags;

   if (!resource) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   VMCI_GrabLock(&lock, &flags);
   if (resource->registrationCount) {
      rv = DsListRemoveResource(dsAPI.registry, resource);
   }
   VMCI_ReleaseLock(&lock, flags);

   return rv;
}

/*
 *-------------------------------------------------------------------------
 *
 *  VMCIDs_Unregister --
 *
 *    Unregisters a key in the discovery service
 *
 *  Result:
 *    VMCI_SUCCESS if successful, otherwise an error code is returned.
 *     
 *  Side effects:
 *    None.
 *
 *-------------------------------------------------------------------------
 */

int
VMCIDs_Unregister(const char *name,  // IN:
                  VMCIId contextID)  // IN:
{
   int errcode;
   VMCILockFlags flags;
   VMCIHandle handle = VMCI_INVALID_HANDLE;

   VMCI_GrabLock(&lock, &flags);
   errcode = DsListRemove(dsAPI.registry, name, &handle, contextID);
   VMCI_ReleaseLock(&lock, flags);
   if (errcode == VMCI_SUCCESS) {
      VMCIResource *resource;
      ASSERT(!VMCI_HANDLE_EQUAL(handle, VMCI_INVALID_HANDLE));
      resource = VMCIResource_Get(handle, VMCI_RESOURCE_TYPE_ANY);
      if (resource) {
         VMCIResource_DecDsRegCount(resource);
         VMCIResource_Release(resource);
      }
   }
   return errcode;
}


/*
 *-------------------------------------------------------------------------
 *
 *  DsUnregisterAction --
 *
 *    Unregisters a key in the discovery service
 *
 *  Result:
 *     None
 *     
 *  Side effects:
 *     None.
 *
 *-------------------------------------------------------------------------
 */

static void 
DsUnregisterAction(const char *name,          // IN: Action name
                   VMCIDsReplyHeader *reply,  // OUT: Reply
                   int *written,              // OUT: Reply length
                   VMCIId contextID)          // IN: requesting context
{
   int errcode = VMCIDs_Unregister(name, contextID);
   DsSetReplyStatus(reply, errcode, "", written);
}


/*
 *-------------------------------------------------------------------------
 *
 *  VMCIDs_Init
 *
 *    Initializes the registry
 *
 *  Result:
 *     None
 *     
 *  Side effects:
 *     None.
 *
 *-------------------------------------------------------------------------
 */

Bool 
VMCIDs_Init(void)
{
   static VMCIResourcePrivilegeType priv[] = { VMCI_PRIV_DG_SEND };
   int result;

   /* Initialize internal datastructure */
   if (!DsListInit(&dsAPI.registry, 10)) {
      VMCILOG((LGPFX"registry initialization failed.\n"));
      return FALSE;
   }
   
   
   /* Setup server handle */
   if (VMCIDatagramCreateHndPriv(VMCI_DS_RESOURCE_ID, VMCI_FLAG_WELLKNOWN_DG_HND,
                                 VMCI_PRIVILEGE_FLAG_TRUSTED,
                                 DsRequestCb, NULL,
                                 &dsAPI.handle) < VMCI_SUCCESS) {
      VMCILOG((LGPFX"make handle failed.\n"));
      return FALSE;
   }
   
   if (!VMCI_HANDLE_EQUAL(dsAPI.handle, VMCI_DS_HANDLE)) {
      VMCILOG((LGPFX"handle inconsistency.\n"));
      return FALSE;
   } 
   
   /*
    * Create a vmcids group.By adding this group as a client to the 
    * datagramAPIResource with the VMCI_PRIV_DG_CREATE we can give contexts access
    * to the vmcids by making them members of this group.
    */
   dsAPI.groupHandle = VMCIGroup_Create();
   if (VMCI_HANDLE_EQUAL(dsAPI.groupHandle, VMCI_INVALID_HANDLE)) {
      VMCILOG((LGPFX"Failed creating Datagram API group.\n"));
      VMCIDatagramDestroyHndInt(dsAPI.handle);
      return FALSE;
   }

   /* Add group as client of vmcids API with the right privilege. */
   result = VMCIResource_AddClientPrivileges(dsAPI.handle,
                                             dsAPI.groupHandle,
                                             1, priv, 0, NULL);
   
   if (result != VMCI_SUCCESS) {
      VMCILOG((LGPFX"Failed to setup privileges: %d.\n", result));
      VMCIGroup_Destroy(dsAPI.groupHandle);
      VMCIDatagramDestroyHndInt(dsAPI.handle);
      return FALSE;
   }
   VMCI_InitLock(&lock,
                 "VMCIDsLock",
                 VMCI_LOCK_RANK_MIDDLE);
   
   dsAPI.isInitialized = TRUE;
   
   return TRUE;
}


/*
 *-------------------------------------------------------------------------
 *
 *  VMCIDs_Exit
 *
 *    Cleans up the CDS entries
 *
 *  Result:
 *     None
 *     
 *  Side effects:
 *     None.
 *
 *-------------------------------------------------------------------------
 */

void
VMCIDs_Exit(void)
{
   if (!dsAPI.isInitialized) {
      return;
   }   

   VMCIResource_RemoveAllClientPrivileges(dsAPI.handle,
                                          dsAPI.groupHandle);
   
   VMCIGroup_Destroy(dsAPI.groupHandle);
   VMCIDatagramDestroyHndInt(dsAPI.handle);

   DsListDestroy(dsAPI.registry);

   dsAPI.isInitialized = FALSE;
   VMCI_CleanupLock(&lock);
}


/*
 *------------------------------------------------------------------------------
 *
 *  VMCIDs_AddContext --
 *
 *     Adds the context as a member of the CDS group. This makes it 
 *     possible for the context to use the CDS.
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
VMCIDs_AddContext(VMCIId contextID)  // IN:
{
   VMCIContext *context = VMCIContext_Get(contextID);
   if (context != NULL) {
      VMCILockFlags flags;
      VMCIGroup_AddMember(dsAPI.groupHandle,
                          VMCI_MAKE_HANDLE(contextID, VMCI_CONTEXT_RESOURCE_ID),
                          FALSE);
      VMCI_GrabLock(&context->lock, &flags);
      VMCIHandleArray_AppendEntry(&context->groupArray, dsAPI.groupHandle);
      VMCI_ReleaseLock(&context->lock, flags);
      VMCIContext_Release(context);
   }
}


/*
 *------------------------------------------------------------------------------
 *
 *  VMCIDs_RemoveContext --
 *
 *     Removes the context as a member of the VMCIDS, disallowing the 
 *     context access to the VMCIDS functions.
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
VMCIDs_RemoveContext(VMCIId contextID)  // IN:
{
   VMCIContext *context;

   if (!dsAPI.isInitialized) {
      return;
   }

   context = VMCIContext_Get(contextID);
   if (context != NULL) {
      VMCILockFlags flags;

      VMCI_GrabLock(&context->lock, &flags);
      VMCIHandleArray_RemoveEntry(context->groupArray, dsAPI.groupHandle);
      VMCI_ReleaseLock(&context->lock, flags);
      VMCIContext_Release(context);

      VMCIGroup_RemoveMember(dsAPI.groupHandle,
                             VMCI_MAKE_HANDLE(contextID,
                                              VMCI_CONTEXT_RESOURCE_ID));
      DsRemoveRegistrationsContext(contextID);
   }
}


/***********************************************************************/
/*                                                                     */
/* Implementation of a simple (name, VMCIHandle) lookup table          */
/*                                                                     */
/* It is simply implemented as an expandable vector                    */
/*                                                                     */
/***********************************************************************/


/*
 *-------------------------------------------------------------------------
 *
 *  DsListInit --
 *
 *    Initialize a DsList datastructure
 *
 *  Result:
 *     True if success
 *     
 *  Side effects:
 *     None.
 *
 *-------------------------------------------------------------------------
 */

static Bool
DsListInit(DsList **list, // OUT:
           int capacity)  // IN:
{
   DsList *l = VMCI_AllocKernelMem(sizeof(DsList),
                                   VMCI_MEMORY_NONPAGED | VMCI_MEMORY_ATOMIC);

   ASSERT(list);
   ASSERT(capacity >= 1);
   
   if (l == NULL) {
      return FALSE;
   }
   l->size = 0;
   l->capacity = capacity;
   l->elements = VMCI_AllocKernelMem(sizeof(DsListElement) * capacity,
                                     VMCI_MEMORY_NONPAGED | VMCI_MEMORY_ATOMIC);
   if (l->elements == NULL) {
      VMCI_FreeKernelMem(l, sizeof *l);
      return FALSE;
   }
   
   *list = l;
   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DsListDestroy --
 *
 *      Destroy a DsList data structure.
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
DsListDestroy(DsList *list)  // IN:
{
   if (list == NULL) {
      return;
   }
   if (list->elements != NULL) {
      VMCI_FreeKernelMem(list->elements, list->capacity * sizeof *list->elements);
      list->elements = NULL;
   }
   VMCI_FreeKernelMem(list, sizeof *list);
}


/*
 *-------------------------------------------------------------------------
 *
 *  DsListLookup --
 *
 *    Finds an element in a list
 *
 *  Result:
 *     A VMCI_DS_* status code
 *     
 *  Side effects:
 *     None.
 *
 *-------------------------------------------------------------------------
 */

static int
DsListLookup(const DsList *list, // IN:
             const char *name,   // IN:
             VMCIHandle *out)    // OUT:
{
   int idx;
   ASSERT(list);
   ASSERT(name);
   
   idx = DsListLookupIndex(list, name);
   if (idx < 0) {
      return VMCI_ERROR_NOT_FOUND;
   }
   
   if (out) {
      *out = list->elements[idx].handle;
   }
   return VMCI_SUCCESS;
}


/*
 *-------------------------------------------------------------------------
 *
 *  DsListInsert --
 *
 *    Inserts a new name into a list. Fails if the name is a duplicate
 *
 *  Result:
 *     A VMCI_ERROR_* status code
 *     
 *  Side effects:
 *     None.
 *
 *-------------------------------------------------------------------------
 */

static int
DsListInsert(DsList *list,       // IN:
             const char *name,   // IN:
             VMCIHandle handle,  // IN:
             VMCIId contextID)   // IN:
{
   int nameLen;
   char *nameMem;

   if (!list || !name || VMCI_HANDLE_EQUAL(handle, VMCI_INVALID_HANDLE) ||
       contextID == VMCI_INVALID_ID) {
      return VMCI_ERROR_INVALID_ARGS;
   }
   
   /* Check for duplicates */
   if (DsListLookupIndex(list, name) >= 0) {
      return VMCI_ERROR_ALREADY_EXISTS;
   }
   
   if (list->capacity == list->size) {
      /* We need to expand the list */
      int newCapacity = list->capacity * 2;
      DsListElement *elms = VMCI_AllocKernelMem(sizeof(DsListElement) * 
                                                newCapacity,
                                                VMCI_MEMORY_NONPAGED |
                                                VMCI_MEMORY_ATOMIC);
      if (elms == NULL) {
         return VMCI_ERROR_NO_MEM;
      }
      memcpy(elms, list->elements, sizeof(DsListElement) * list->capacity);
      VMCI_FreeKernelMem(list->elements,
                         sizeof *list->elements * list->capacity);
      list->elements = elms;
      list->capacity = newCapacity;
   }
   
   ASSERT(list->capacity > list->size);
   
   nameLen = strlen(name) + 1;
   nameMem = VMCI_AllocKernelMem(nameLen,
                                 VMCI_MEMORY_NONPAGED | VMCI_MEMORY_ATOMIC);
   if (nameMem == NULL) {
      return VMCI_ERROR_NO_MEM;
   }
   memcpy(nameMem, name, nameLen);
   
   list->elements[list->size].name   = nameMem;
   list->elements[list->size].handle = handle;
   list->elements[list->size].contextID = contextID;
   list->size = list->size + 1;

   return VMCI_SUCCESS;
}


/*
 *-------------------------------------------------------------------------
 *
 *  DsListRemove --
 *
 *    Removes a new name from the list
 *
 *  Result:
 *     A VMCI_ERROR_* status code
 *     
 *  Side effects:
 *     None.
 *
 *-------------------------------------------------------------------------
 */

static int
DsListRemove(DsList *list,           // IN:
             const char *name,       // IN: name registered under
             VMCIHandle *handleOut,  // OUT: handle removed from the list
             VMCIId contextID)       // IN: calling context's ID
{
   DsListElement *elems;
   int idx, rv;
   VMCIHandle handle;

   if (!list || !name || contextID == VMCI_INVALID_ID) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   idx = DsListLookupIndex(list, name);
   if (idx < 0) {
      return VMCI_ERROR_NOT_FOUND;
   }

   elems = list->elements;

   /* Allow to unregister if contextID's match or if host is the caller. */
   if (contextID != VMCI_HOST_CONTEXT_ID && elems[idx].contextID != contextID) {
      return VMCI_ERROR_NO_ACCESS;
   }
   
   /* Cache the handle to be removed. */
   handle = elems[idx].handle;

   rv = DsListRemoveElement(list, idx);
   if (rv == VMCI_SUCCESS && handleOut) {
      /* The handle removed is an OUT value. */
      *handleOut = handle;
   }

   return rv;
}



/*
 *-------------------------------------------------------------------------
 *
 *  DsListLookupIndex --
 *
 *    Searches the register for the index of a given key, or return
 *    -1 if not found
 *
 *  Result:
 *     See above
 *     
 *  Side effects:
 *     None.
 *
 *-------------------------------------------------------------------------
 */

static int 
DsListLookupIndex(const DsList *list, // IN: 
                  const char *name)   // IN:
{
   int i;

   ASSERT(list);
   ASSERT(name);
   
   for(i = 0; i < list->size; i++) {
      if (strcmp(list->elements[i].name, name) == 0) {
         return i;
      }
   }
   return -1;
}


/*
 *-------------------------------------------------------------------------
 *
 *  DsListRemoveResource --
 *
 *    Removes all registrations for a given resource. Returns the count of
 *    removed registrations (>= 0) on success, error code otherwise.
 *    Assumes that the lock is held.
 *
 *  Result:
 *    See above
 *     
 *  Side effects:
 *    Discovery service list is pruned.
 *
 *-------------------------------------------------------------------------
 */

static int 
DsListRemoveResource(DsList *list,           // IN:
                     VMCIResource *resource) // IN:
{
   VMCIHandle handle;
   int i, registrationCount;
   int count = 0;

   if (!list || !resource) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   handle = resource->handle;
   if (VMCI_HANDLE_EQUAL(handle, VMCI_INVALID_HANDLE)) {
      return VMCI_ERROR_NO_HANDLE;
   }

   registrationCount = resource->registrationCount;
   if (!registrationCount) {
      VMCILOG((LGPFX"%s called with registrationCount = 0.\n",
               __FUNCTION__));
   }
   
   for (i = 0; i < list->size;) {
      if (VMCI_HANDLE_EQUAL(list->elements[i].handle, handle)) {
         int removeElement = DsListRemoveElement(list, i);
         if (removeElement != VMCI_SUCCESS) {
            VMCILOG((LGPFX"Error: %s returned %d.\n",
                     __FUNCTION__, removeElement));
            break;
         }
         count++;
         VMCIResource_DecDsRegCount(resource);
      } else {
         /* Move to the next element. */
         i++;
      }
   }
   if (count != registrationCount) {
      VMCILOG((LGPFX"Error: %s: no. of removed registrations "
               "= %d, should be %d.\n", __FUNCTION__, count,
               registrationCount));
   }
   return count;
}


/*
 *-------------------------------------------------------------------------
 *
 *  DsListRemoveElement --
 *
 *    Removes an element from the list. Assumes locks are held.
 *
 *  Result:
 *    A VMCI_ERROR_* status code on error, VMCI_SUCCESS otherwise.
 *     
 *  Side effects:
 *     Memory is freed.
 *
 *-------------------------------------------------------------------------
 */

static int
DsListRemoveElement(DsList *list, // IN:
                    int index)    // IN: index of the element to remove
{
   if (!list || index < 0) {
      return VMCI_ERROR_INVALID_ARGS;
   }
   if (index > list->size - 1) {
      return VMCI_ERROR_NOT_FOUND;
   }

   /* Free name. */
   VMCI_FreeKernelMem(list->elements[index].name,
                      strlen(list->elements[index].name) + 1);
   /* Move elements one spot up. */
   memmove(&list->elements[index],
           &list->elements[index + 1],
           (list->size - index - 1) * sizeof list->elements[index]);
   list->size--;
   /* Zero out the last element. */
   memset(&list->elements[list->size], 0, sizeof list->elements[list->size]);

   return VMCI_SUCCESS;
}


/*
 *-------------------------------------------------------------------------
 *
 *  DsRemoveRegistrationsContext --
 *
 *    Removes all registrations for a given context.  Iterates through the
 *    list of registrations searching for matching context ID, and removes
 *    them.
 *
 *  Result:
 *    None.
 *     
 *  Side effects:
 *    Memory is freed.
 *
 *-------------------------------------------------------------------------
 */

static void
DsRemoveRegistrationsContext(VMCIId contextID) // IN:
{
   if (contextID != VMCI_INVALID_ID) {
      VMCILockFlags flags;

      VMCI_GrabLock(&lock, &flags);
      if (dsAPI.isInitialized) {
         DsList *list;
         int i;

         list = dsAPI.registry;
         ASSERT(list);
         /*
          * Traverse from end of the list since elements are moved up the list
          * to cover holes caused by elements being deleted.
          */
         for (i = list->size - 1; i >= 0; i--) {
            if (list->elements[i].handle.context == contextID) {
               ASSERT(list->elements[i].contextID == contextID);
               DsListRemoveElement(list, i);
            }
         }
      }
      VMCI_ReleaseLock(&lock, flags);
   }
}
