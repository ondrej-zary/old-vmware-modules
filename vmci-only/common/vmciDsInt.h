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

#ifndef VMCI_DS_INT_H_
#define VMCI_DS_INT_H_

#include "vmciResource.h"

Bool VMCIDs_Init(void);
void VMCIDs_Exit(void);
void VMCIDs_AddContext(VMCIId contextID);
void VMCIDs_RemoveContext(VMCIId contextID);
int VMCIDs_Register(const char* name, VMCIHandle handle, VMCIId contextID);
int VMCIDs_Unregister(const char *name, VMCIId contextID);
int VMCIDs_UnregisterResource(VMCIResource *resource);

#endif // VMCI_DS_INT_H_

