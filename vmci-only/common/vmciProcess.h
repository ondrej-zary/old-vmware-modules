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
 * vmciProcess.h --
 *
 *	VMCI process header.
 */

#ifndef _VMCI_PROCESS_H_
#define _VMCI_PROCESS_H_

#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMMON
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

#include "vmci_kernel_if.h"
#include "vmci_defs.h"
#include "vmci_infrastructure.h"
#include "vmci_handle_array.h"

#define MAX_QUEUED_GUESTCALLS_PER_VM  100

typedef struct VMCIProcess VMCIProcess;

int VMCIProcess_Init(void);
int VMCIProcess_Create(VMCIProcess **process);
void VMCIProcess_Destroy(VMCIProcess *process);
VMCIProcess *VMCIProcess_Get(VMCIId processID);
#endif // _VMCI_PROCESS_H_

