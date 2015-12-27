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
 * vmciResource.h --
 *
 *	VMCI Resource Access Control API.
 */

#ifndef _VMCI_RESOURCE_H_
#define _VMCI_RESOURCE_H_

#define INCLUDE_ALLOW_VMMON
#define INCLUDE_ALLOW_VMCORE
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"

#include "vmci_defs.h"
#include "vmci_kernel_if.h"
#include "vmciHashtable.h"
#include "vmciContext.h"

#define RESOURCE_CONTAINER(ptr, type, member) \
   ((type *)((char *)(ptr) - offsetof(type, member)))

typedef void(*VMCIResourceFreeCB)(void *resource);

typedef enum {
   VMCI_PRIV_ALLOW = 0x1000,
   VMCI_PRIV_DENY,
   VMCI_PRIV_VALID,
   VMCI_PRIV_NOT_SET,
} VMCIResourcePrivilege;

typedef struct VMCIResourceClient {
   VMCIHandle                handle;
   int                       refCount;
   VMCIResourcePrivilege     privilege[VMCI_NUM_PRIVILEGES];
   struct VMCIResourceClient *next;
} VMCIResourceClient;

typedef enum {
   VMCI_RESOURCE_TYPE_ANY,
   VMCI_RESOURCE_TYPE_API,
   VMCI_RESOURCE_TYPE_GROUP,
   VMCI_RESOURCE_TYPE_DATAGRAM,
   VMCI_RESOURCE_TYPE_SHAREDMEM,
} VMCIResourceType;

typedef struct VMCIResource {
   VMCIHashEntry         hashEntry;
   VMCIResourceType      type;
   VMCIResourcePrivilege validPrivs[VMCI_NUM_PRIVILEGES];
   VMCILock              clientsLock;
   VMCIResourceClient    *clients;
   VMCIResourceFreeCB    containerFreeCB;    // Callback to free container 
                                             // object when refCount is 0.
   void                  *containerObject;   // Container object reference.
   VMCIHandle            handle;
   uint32                registrationCount;
} VMCIResource;


int VMCIResource_Init(void);
void VMCIResource_Exit(void);
VMCIId VMCIResource_GetID(void);

int VMCIResource_Add(VMCIResource *resource, VMCIResourceType resourceType,
                     VMCIHandle resourceHandle, VMCIHandle ownerHandle,
                     int numValidPrivs, VMCIResourcePrivilegeType *validPriv,
                     VMCIResourceFreeCB containerFreeCB, void *containerObject);
void VMCIResource_Remove(VMCIHandle resourceHandle, VMCIResourceType resourceType);
VMCIResource *VMCIResource_Get(VMCIHandle resourceHandle,
                               VMCIResourceType resourceType);
void VMCIResource_GetPair(VMCIHandle resourceHandles[2],
                          VMCIResourceType resourceTypes[2],
                          VMCIResource *resources[2]);
int VMCIResource_Release(VMCIResource *resource);
int VMCIResource_ReleasePair(VMCIResource *resource[2], int results[2]);

int VMCIResource_AddClientPrivileges(VMCIHandle resourceHandle,
                                     VMCIHandle clientHandle,
                                     int numAllowPrivs,
                                     VMCIResourcePrivilegeType *allowPrivs,
                                     int numDenyPrivs,
                                     VMCIResourcePrivilegeType *denyPrivs);
int VMCIResource_RemoveClientPrivileges(VMCIHandle resourceHandle,
                                        VMCIHandle clientHandle,
                                        int numPrivs, 
                                        VMCIResourcePrivilegeType *privs);
int VMCIResource_RemoveAllClientPrivileges(VMCIHandle resourceHandle,
                                           VMCIHandle clientHandle);
int VMCIResource_CheckClientPrivilegePtr(VMCIResource *resource,
                                         VMCIHandle clientHandle,
                                         VMCIResourcePrivilegeType priv);
int VMCIResource_CheckClientPrivilege(VMCIHandle resourceHandle,
                                      VMCIHandle clientHandle,
                                      VMCIResourcePrivilegeType priv);

int VMCIResource_IncDsRegCount(VMCIResource *resource);
int VMCIResource_DecDsRegCount(VMCIResource *resource);

#endif // _VMCI_RESOURCE_H_
