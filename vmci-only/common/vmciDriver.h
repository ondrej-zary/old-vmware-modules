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
 * vmciDriver.h --
 *
 *	VMCI host driver interface.
 */

#ifndef _VMCI_DRIVER_H_
#define _VMCI_DRIVER_H_

#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMMON
#define INCLUDE_ALLOW_VMCORE
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vmci_defs.h"
#include "vmci_infrastructure.h"
#include "vmciContext.h"

//#define VMCI_DEBUG
#ifdef VMCI_DEBUG
#  define VMCI_DEBUG_LOG(_a) Log _a
#else
#  define VMCI_DEBUG_LOG(_a)
#endif

/*
 * A few macros to encapsulate logging in common code. The macros
 * result in LOG/LOGThrottled on vmkernel and Log on hosted.
 */

#ifdef VMKERNEL
#  define LOGLEVEL_MODULE_LEN 0
#  define LOGLEVEL_MODULE VMCIVMK
#  include "log.h"
#  define _VMCILOG(_args...) LOG(0, _args)
#  define _VMCILOGThrottled(_args...) LOGThrottled(0, _args)
#  define VMCILOG(_args) _VMCILOG _args
#  define VMCILOGThrottled(_args) _VMCILOGThrottled _args
#else
#  define VMCILOG(_args) Log _args
#  define VMCILOGThrottled(_args) Log _args
#endif

int VMCI_Init(void);
void VMCI_Cleanup(void);
void VMCIPublicGroup_AddContext(VMCIId contextID);
int VMCIPublicGroup_RemoveContext(VMCIId contextID);
VMCIId VMCI_GetContextID(void);

#endif // _VMCI_DRIVER_H_
