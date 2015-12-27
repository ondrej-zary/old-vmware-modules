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
 * vmciGroup.c --
 *
 *     Implementation of the VMCI Group API.
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
#include "vmci_handle_array.h"
#include "vmciDriver.h"
#include "vmciResource.h"

#define LGPFX "VMCIAccess: "

typedef struct Group {
   VMCIResource      resource;
   VMCIHandleArray  *memberArray; /* List of members. */
   VMCILock          lock; /* Locks memberArray */
} Group;


static void GroupFreeCB(void *resource);


/*
 *------------------------------------------------------------------------------
 *
 *  GroupFreeCB --
 *     Callback to free group structure when resource is no longer used,
 *     ie. the reference count reached 0.
 * 
 *  Result:
 *     None.
 *     
 *------------------------------------------------------------------------------
 */

static void
GroupFreeCB(void *resource)
{
   Group *group;
   ASSERT(resource);

   group = RESOURCE_CONTAINER(resource, Group, resource);
   VMCIHandleArray_Destroy(group->memberArray);
   VMCI_CleanupLock(&group->lock);
   VMCI_FreeKernelMem(group, sizeof *group);
}


/*
 *-----------------------------------------------------------------------------------
 *
 *   VMCIGroup_Create --
 *
 *      Creates a new group. The group handle can be shared under a name via the 
 *      VMCI Discovery Service (CDS).
 *
 *   Results:
 *      Group handle if successful, VMCI_INVALID_HANDLE if not.
 *
 *   Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------------
 */

VMCIHandle
VMCIGroup_Create(void)
{
   int result;
   VMCIHandle handle;
   VMCIResourcePrivilegeType validPriv = VMCI_PRIV_ASSIGN_CLIENT;
   VMCIId resourceID = VMCIResource_GetID();
   Group *group = VMCI_AllocKernelMem(sizeof *group, VMCI_MEMORY_NONPAGED);
   if (group == NULL) {
      VMCILOG((LGPFX"Create: Failed allocating memory for group.\n"));
      return VMCI_INVALID_HANDLE;
   }

   group->memberArray = VMCIHandleArray_Create(0);
   if (group->memberArray == NULL) {
      VMCI_FreeKernelMem(group, sizeof *group);
      return VMCI_INVALID_HANDLE;
   }
   VMCI_InitLock(&group->lock, "VMCIGroupLock", VMCI_LOCK_RANK_HIGHEST);

   /* Groups are always host context resources. */
   handle = VMCI_MAKE_HANDLE(VMCI_HOST_CONTEXT_ID, resourceID);

   result = VMCIResource_Add(&group->resource, VMCI_RESOURCE_TYPE_GROUP,
                             handle, 
                             VMCI_MAKE_HANDLE(VMCI_HOST_CONTEXT_ID, 
                                            VMCI_CONTEXT_RESOURCE_ID),
                             1, &validPriv, GroupFreeCB, group);
   if (result != VMCI_SUCCESS) { 
      VMCI_CleanupLock(&group->lock);
      VMCIHandleArray_Destroy(group->memberArray);
      VMCI_FreeKernelMem(group, sizeof *group);
      handle = VMCI_INVALID_HANDLE;
   }
   return handle;
}


/*
 *-----------------------------------------------------------------------------------
 *
 *   VMCIGroup_Destroy --
 *
 *      Removes all members from the group and destroy the group data structure. 
 *      Handle is no longer a valid group handle.
 *
 *   Results:
 *      None. 
 *
 *   Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------------
 */

void
VMCIGroup_Destroy(VMCIHandle groupHandle)
{
   Group *group;
   VMCIHandle memberHandle;
   VMCILockFlags flags;
   VMCIResource *resource = VMCIResource_Get(groupHandle, VMCI_RESOURCE_TYPE_GROUP);
   if (resource == NULL) {
      return;
   }
   group = RESOURCE_CONTAINER(resource, Group, resource);
   
   /* 
    * Remove it from the resource table, destroy all resource clients. It is 
    * still guaranteed to be alive due to the above reference.
    */
   VMCIResource_Remove(groupHandle, VMCI_RESOURCE_TYPE_GROUP);

   /* 
    * Remove all members from the group. XXX Consider adding a callback to the
    * members to get notified when a group is destroyed.
    */
   VMCI_GrabLock(&group->lock, &flags);
   memberHandle = VMCIHandleArray_RemoveTail(group->memberArray);
   while (!VMCI_HANDLE_EQUAL(memberHandle, VMCI_INVALID_HANDLE)) {
      memberHandle = VMCIHandleArray_RemoveTail(group->memberArray);
   }
   VMCI_ReleaseLock(&group->lock, flags);
   
   VMCIResource_Release(resource);
}


/*
 *-----------------------------------------------------------------------------------
 *
 *   VMCIGroup_AddMember --
 *
 *      Add the member as a client to the group resource.
 *
 *   Results:
 *      VMCI_SUCCESS if successfully added, error if not.
 *
 *   Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------------
 */

int
VMCIGroup_AddMember(VMCIHandle groupHandle,
                    VMCIHandle memberHandle,
                    Bool canAssign)
{
   int result = VMCI_SUCCESS;
   Group *group;
   VMCIResource *resource;
   VMCIResourcePrivilegeType privs = VMCI_PRIV_ASSIGN_CLIENT;
   VMCILockFlags flags;

   if (VMCI_HANDLE_EQUAL(memberHandle, VMCI_INVALID_HANDLE)) {
      return VMCI_ERROR_INVALID_ARGS;
   }
  
   resource = VMCIResource_Get(groupHandle, VMCI_RESOURCE_TYPE_GROUP);
   if (resource == NULL) {
      return VMCI_ERROR_INVALID_ARGS;
   }
   group = RESOURCE_CONTAINER(resource, Group, resource);

   /* Update group's member array. */
   VMCI_GrabLock(&group->lock, &flags);
   VMCIHandleArray_AppendEntry(&group->memberArray, memberHandle);
   VMCI_ReleaseLock(&group->lock, flags);

   /* Set group privilege for member. */
   if (canAssign) {
      result = VMCIResource_AddClientPrivileges(groupHandle, memberHandle,
                                                1, &privs, 0, NULL);
   } else {
      result = VMCIResource_AddClientPrivileges(groupHandle, memberHandle,
                                                0, NULL, 1, &privs);
   }
   VMCIResource_Release(resource);

   return result;
}


/*
 *-----------------------------------------------------------------------------------
 *
 *   VMCIGroup_RemoveMember --
 *
 *      Removes the member from the group's member list.
 *
 *   Results:
 *      VMCI_SUCCESS if successfully removed, error if not.
 *
 *   Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------------
 */

int
VMCIGroup_RemoveMember(VMCIHandle groupHandle,
                       VMCIHandle memberHandle)
{
   int result;
   Group *group;
   VMCIResource *resource;
   VMCILockFlags flags;

   if (VMCI_HANDLE_EQUAL(memberHandle, VMCI_INVALID_HANDLE)) {
      return VMCI_ERROR_INVALID_ARGS;
   }
   
   /* Remove group resource's reference to member. */
   resource = VMCIResource_Get(groupHandle, VMCI_RESOURCE_TYPE_GROUP);
   if (resource == NULL) {
      VMCILOG((LGPFX"RemoveMember: Failed to get group resource for "
               "0x%x:0x%x.\n", groupHandle.context, groupHandle.resource));
      return VMCI_ERROR_INVALID_ARGS;
   }
   group = RESOURCE_CONTAINER(resource, Group, resource);
   VMCI_GrabLock(&group->lock, &flags);
   VMCIHandleArray_RemoveEntry(group->memberArray, memberHandle);
   VMCI_ReleaseLock(&group->lock, flags);

   /*
    * Remove all client privileges to resource. This essentially removes
    * the client from the group resource.
    */
   result = VMCIResource_RemoveAllClientPrivileges(groupHandle, memberHandle);

   VMCIResource_Release(resource);

   return result;
}


/*
 *-----------------------------------------------------------------------------------
 *
 *   VMCIGroup_IsMember --
 *
 *      Checks if memberHandle is a member of the given group.
 *
 *   Results:
 *      TRUE if a member, FALSE if not.
 *
 *   Side effects:
 *      None.
 *
 *------------------------------------------------------------------------------------
 */

Bool
VMCIGroup_IsMember(VMCIHandle groupHandle,
                   VMCIHandle memberHandle)
{
   Bool isMember;
   Group *group;
   VMCIResource *resource;
   VMCILockFlags flags;

   if (VMCI_HANDLE_EQUAL(memberHandle, VMCI_INVALID_HANDLE)) {
      return FALSE;
   }
   
   /* Remove group resource's reference to member. */
   resource = VMCIResource_Get(groupHandle, VMCI_RESOURCE_TYPE_GROUP);
   if (resource == NULL) {
      VMCILOG((LGPFX"IsMember: Failed to get group resource for 0x%x:0x%x.\n",
               groupHandle.context, groupHandle.resource));
      return FALSE;
   }
   group = RESOURCE_CONTAINER(resource, Group, resource);
   VMCI_GrabLock(&group->lock, &flags);
   isMember = VMCIHandleArray_HasEntry(group->memberArray, memberHandle);
   VMCI_ReleaseLock(&group->lock, flags);
   VMCIResource_Release(resource);

   return isMember;
}
