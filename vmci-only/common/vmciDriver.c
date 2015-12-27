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
 * vmciDriver.c --
 *
 *     VMCI initialization and ioctl handling.
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
#elif !defined(__APPLE__) && !defined(VMKERNEL)
#   error "Unknown platform"
#endif

/* Must precede all vmware headers. */
#include "vmci_kernel_if.h"

#include "vm_assert.h"
#ifdef VMKERNEL
#define LOGLEVEL_MODULE_LEN 0
#define LOGLEVEL_MODULE VMCIVMK
#include "log.h"
#endif // VMKERNEL
#include "vmciResource.h"
#include "vmciContext.h"
#ifndef VMX86_SERVER
#include "vmciProcess.h"
#endif //VMX86_SERVER
#include "vmciEvent.h"
#include "vmciDriver.h"
#include "vmciGroup.h"
#include "vmciDatagram.h"
#include "vmciDsInt.h"
#include "vmci_defs.h"
#include "vmci_call_defs.h"
#include "vmci_infrastructure.h"
#include "vmciQueuePair.h"
#include "vmciCommonInt.h"
#include "vmware.h"

#define LGPFX "VMCI: "

/* All contexts are members of this group handle. */
static VMCIHandle vmciPublicGroupHandle;

static VMCIContext *hostContext;

/*
 *----------------------------------------------------------------------
 *
 * VMCI_Init --
 *
 *      Initializes VMCI. This registers core hypercalls.
 *
 * Results:
 *      VMCI_SUCCESS if successful, appropriate error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VMCI_Init(void)
{
   int result;

   result = VMCIResource_Init();
   if (result < VMCI_SUCCESS) {
      Log(LGPFX"Failed to initialize VMCIResource: %d\n", result);
      goto errorExit;
   }

#ifndef VMX86_SERVER
   result = VMCIProcess_Init();
   if (result < VMCI_SUCCESS) {
      Log(LGPFX"Failed to initialize VMCIProcess: %d\n", result);
      goto resourceExit;
   }
#endif // !VMX86_SERVER

   result = VMCIContext_Init();
   if (result < VMCI_SUCCESS) {
      Log(LGPFX"Failed to initialize VMCIContext: %d\n", result);
      goto resourceExit;
   }

   result = VMCIDatagram_Init();
   if (result < VMCI_SUCCESS) {
      Log(LGPFX"Failed to initialize VMCIDatagram: %d\n", result);
      goto contextExit;
   }

   /*
    * In theory, it is unsafe to pass an eventHnd of -1 to platforms which use
    * it (VMKernel/Windows/Mac OS at the time of this writing). In practice we
    * are fine though, because the event is never used in the case of the host
    * context.
    */
   result = VMCIContext_InitContext(VMCI_HOST_CONTEXT_ID,
                                    VMCI_DEFAULT_PROC_PRIVILEGE_FLAGS,
                                    -1, VMCI_VERSION, &hostContext);
   if (result < VMCI_SUCCESS) {
      Log(LGPFX"Failed to initialize VMCIContext: %d\n", result);
      goto datagramExit;
   }

   VMCIEvent_Init();

   /* This needs to be after init. of the host context */
   if (!VMCIDs_Init()) {
      result = VMCI_ERROR_GENERIC;
      Log(LGPFX"Failed to initialize Discovery Service.\n");
      goto hostContextExit;
   }

   result = QueuePair_Init();
   if (result < VMCI_SUCCESS) {
      goto hostContextExit;
   }
 
   /* Give host context access to the DS API. */
   VMCIDs_AddContext(VMCI_HOST_CONTEXT_ID);

   /* Create the public group handle under a well known name. */
   vmciPublicGroupHandle = VMCIGroup_Create();
   VMCIDs_Register(VMCI_PUBLIC_GROUP_NAME, vmciPublicGroupHandle,
                   VMCI_HOST_CONTEXT_ID);
   VMCIPublicGroup_AddContext(VMCI_HOST_CONTEXT_ID);

   Log(LGPFX"Driver initialized.\n");
   return VMCI_SUCCESS;

  hostContextExit:
   VMCIEvent_Exit();
   VMCIContext_ReleaseContext(hostContext);
  datagramExit:
   VMCIDatagram_Exit();
  contextExit:
   VMCIContext_Exit();
  resourceExit:
   VMCIResource_Exit();
  errorExit:
   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCI_Cleanup --
 *
 *      Cleanup the  VMCI module. 
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
VMCI_Cleanup(void)
{
   VMCIPublicGroup_RemoveContext(hostContext->cid);
   /* Unregister & destroy the public group handle. */
   VMCIDs_Unregister(VMCI_PUBLIC_GROUP_NAME, VMCI_HOST_CONTEXT_ID);
   VMCIGroup_Destroy(vmciPublicGroupHandle);

   /* Revoke host context access to DS and datagram API. */
   VMCIDs_RemoveContext(hostContext->cid);

   VMCIDs_Exit();
   VMCIEvent_Exit();
   VMCIContext_ReleaseContext(hostContext);
   VMCIDatagram_Exit();
   VMCIContext_Exit();
   VMCIResource_Exit();
   QueuePair_Exit();
}


/*
 *----------------------------------------------------------------------------
 *
 * VMCI_GetContextID --
 *
 *    Returns the current context ID.  Note that since this is accessed only
 *    from code running in the host, this always returns the host context ID.
 *
 * Results:
 *    Context ID.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

#if defined(linux) && !defined(VMKERNEL)
EXPORT_SYMBOL(VMCI_GetContextID);
#endif

VMCIId
VMCI_GetContextID(void)
{
   return VMCI_HOST_CONTEXT_ID;
}


/*
 *-------------------------------------------------------------------------
 *
 *  VMCIPublicGroup_AddContext --
 *
 *    Adds a context to the public group handle.
 *     
 *  Result:
 *    None. 
 *   
 *  Side effects:
 *    None.
 *     
 *-------------------------------------------------------------------------
 */

void
VMCIPublicGroup_AddContext(VMCIId contextID)
{
   VMCIContext *context = VMCIContext_Get(contextID);
   if (context) {
      VMCILockFlags flags;

      VMCIGroup_AddMember(vmciPublicGroupHandle,
                          VMCI_MAKE_HANDLE(contextID, VMCI_CONTEXT_RESOURCE_ID),
                          TRUE);
      VMCI_GrabLock(&context->lock, &flags);
      VMCIHandleArray_AppendEntry(&context->groupArray, vmciPublicGroupHandle);
      VMCI_ReleaseLock(&context->lock, flags);

      VMCIContext_Release(context);
   }
}

/*
 *-------------------------------------------------------------------------
 *
 *  VMCIPublicGroup_RemoveContext --
 *
 *    Removes a context to the public group handle.
 *     
 *  Result:
 *    Returns the result from VMCIGroup_RemoveMember. 
 *   
 *  Side effects:
 *    None.
 *     
 *-------------------------------------------------------------------------
 */

int
VMCIPublicGroup_RemoveContext(VMCIId contextID)
{
   int rv = VMCI_ERROR_INVALID_ARGS;
   VMCIContext *context = VMCIContext_Get(contextID);
   if (context) {
      VMCILockFlags flags;

      VMCI_GrabLock(&context->lock, &flags);
      VMCIHandleArray_RemoveEntry(context->groupArray, vmciPublicGroupHandle);
      VMCI_ReleaseLock(&context->lock, flags);

      VMCIContext_Release(context);
      rv = VMCIGroup_RemoveMember(vmciPublicGroupHandle,
                                  VMCI_MAKE_HANDLE(contextID,
                                                 VMCI_CONTEXT_RESOURCE_ID));
   }
   return rv;
}
