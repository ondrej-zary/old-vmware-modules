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
 * vmciProcess.c --
 *
 *     VMCI Process code. 
 */

#ifdef linux
#   include "driver-config.h"
#   include <linux/string.h> /* memset() in the kernel */
#elif defined(WINNT_DDK)
#   include <ntddk.h>
#   include <string.h>
#elif !defined(__APPLE__)
#   error "Unknown platform"
#endif

/* Must precede all vmware headers. */
#include "vmci_kernel_if.h"

#include "vm_assert.h"
#include "vmciProcess.h"
#include "vmciDriver.h"
#include "vmciDatagram.h"
#include "vmware.h"
#include "circList.h"
#include "vmciCommonInt.h"

#define LGPFX "VMCIProcess: "

static struct {
   ListItem *head;
   VMCILock lock;
} processList;


/*
 *----------------------------------------------------------------------
 *
 * VMCIProcess_Init --
 *
 *      Initialize the process module.
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
VMCIProcess_Init(void) {
   processList.head = NULL;
   VMCI_InitLock(&processList.lock,
                 "VMCIProcessListLock",
                 VMCI_LOCK_RANK_MIDDLE);
   return VMCI_SUCCESS;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIProcess_Create --
 *
 *      Creates a new VMCI process.
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
VMCIProcess_Create(VMCIProcess **outProcess) // IN
{
   VMCIProcess *process;
   int result;
   VMCILockFlags flags;

   process = VMCI_AllocKernelMem(sizeof *process,
                                 VMCI_MEMORY_NONPAGED);
   if (process == NULL) {
      VMCILOG((LGPFX"Failed to allocate memory for process.\n"));
      result = VMCI_ERROR_NO_MEM;
      goto error;
   }

   process->pid = (VMCIId)(uintptr_t)process >> 1;

   VMCI_GrabLock(&processList.lock, &flags);
   LIST_QUEUE(&process->listItem, &processList.head);
   VMCI_ReleaseLock(&processList.lock, flags);

   *outProcess = process;
   return 0;

error:
   if (process) {
      VMCI_FreeKernelMem(process, sizeof *process);
   }
   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIProcess_Destroy --
 *
 *      Destroys a vmci process.
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
VMCIProcess_Destroy(VMCIProcess *process)
{
   VMCILockFlags flags;
   VMCIContext *hostCtx;

   hostCtx = VMCIContext_Get(VMCI_HOST_CONTEXT_ID);
   ASSERT(hostCtx != NULL); // Host context must be around when executing this.

   /* Dequeue process. */
   VMCI_GrabLock(&processList.lock, &flags);
   LIST_DEL(&process->listItem, &processList.head);
   VMCI_ReleaseLock(&processList.lock, flags);

   VMCIContext_Release(hostCtx);

   VMCI_FreeKernelMem(process, sizeof *process);
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIProcess_Get --
 *
 *      Get the process corresponding to the pid.
 *
 * Results:
 *      VMCI process on success, NULL otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

VMCIProcess *
VMCIProcess_Get(VMCIId processID)  // IN
{
   VMCIProcess *process = NULL;  
   ListItem *next;
   VMCILockFlags flags;

   VMCI_GrabLock(&processList.lock, &flags);
   if (LIST_EMPTY(processList.head)) {
      goto out;
   }

   LIST_SCAN(next, processList.head) {
      process = LIST_CONTAINER(next, VMCIProcess, listItem);
      if (process->pid == processID) {
         break;
      }
   }

out:
   VMCI_ReleaseLock(&processList.lock, flags);
   return (process && process->pid == processID) ? process : NULL;
}

