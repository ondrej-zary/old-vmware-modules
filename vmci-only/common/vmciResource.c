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
 * vmciResource.c --
 *
 *     Implementation of the VMCI Resource Access Control API.
 */

#if defined(linux) && !defined(VMKERNEL)
#   include "driver-config.h"
#   include <linux/string.h> /* memset() in the kernel */
#elif defined(WINNT_DDK)
#   include <ntddk.h>
#   include <string.h>
#elif !defined(__APPLE__) && !defined VMKERNEL
#   error "Unknown platform"
#endif

/* Must precede all vmware headers. */
#include "vmci_kernel_if.h"

#include "vm_assert.h"
#include "vmci_defs.h"
#include "vmci_handle_array.h"
#include "vmci_infrastructure.h"
#include "vmciContext.h"
#include "vmciResource.h"
#include "vmciGroup.h"
#include "vmciDriver.h"
#include "vm_atomic.h"
#include "vmciCommonInt.h"

#define LGPFX "VMCIResource: "

/* 0 through VMCI_RESERVED_RESOURCE_ID_MAX are reserved. */
static Atomic_uint32 resourceID = { VMCI_RESERVED_RESOURCE_ID_MAX + 1 };

static int ResourceAddClient(VMCIResource *resource, 
                             VMCIHandle clientHandle,
                             int numAllowPrivs,
                             VMCIResourcePrivilegeType *allowPrivs,
                             int numDenyPrivs,
                             VMCIResourcePrivilegeType *denyPrivs);
static void ResourceRemoveClient(VMCIResource *resource,
                                 VMCIResourceClient *client);
static void VMCIResourceDoRemove(VMCIResource *resource);

static VMCIHashTable *resourceTable = NULL;

/* Helper functions. */

/*
 *-----------------------------------------------------------------------------------
 *
 * ResourceValidatePrivileges --
 *
 *      Checks given privileges are valid for the given resource.
 *
 * Results:
 *      Returns VMCI_SUCCESS on success, appropriate error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------------
 */

static INLINE int
ResourceValidatePrivileges(VMCIResource *resource, 
                           int numPrivs,
                           VMCIResourcePrivilegeType *privs)
{
   int i;

   for (i = 0; i < numPrivs; i++) {
      if (resource->validPrivs[privs[i]] != VMCI_PRIV_VALID) {
	 return VMCI_ERROR_INVALID_PRIV;
      }
   }
   return VMCI_SUCCESS;
}


/*
 *-----------------------------------------------------------------------------------
 *
 * ResourceGetClient --
 *
 *      Traverses resource's client list and returns client struct if found.
 *      Assumes resource->clientsLock is held.
 *
 * Results:
 *      Returns VMCI_SUCCESS on success, appropriate error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------------
 */

static INLINE VMCIResourceClient *
ResourceGetClient(VMCIResource *resource, 
                  VMCIHandle clientHandle)
{
   VMCIResourceClient *client = resource->clients;
   while (client && !VMCI_HANDLE_EQUAL(client->handle,clientHandle)) {
      client = client->next;
   }
   if (client != NULL) {
      client->refCount++;
   }

   return client;
}


/*
 *-----------------------------------------------------------------------------------
 *
 * ResourceReleaseClient --
 *
 *      Releases a client and checks if it should be freed. 
 *      Assumes resource->clientsLock is held.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------------
 */

static INLINE void
ResourceReleaseClient(VMCIResource *resource,
                      VMCIResourceClient *client)
{
   ASSERT(client && client->refCount > 0);

   client->refCount--;
   if (client->refCount == 0) {
      VMCI_FreeKernelMem(client, sizeof *client);
   }
}


/*
 *-----------------------------------------------------------------------------------
 *
 * ResourceAddClient --
 *
 *      Creates a new client for resource, set given privileges at the same time.
 *      Assumes resource->clientsLock is held.
 *
 * Results:
 *      Returns VMCI_SUCCESS on success, appropriate error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------------
 */

static int
ResourceAddClient(VMCIResource *resource, 
                  VMCIHandle clientHandle,
                  int numAllowPrivs,
                  VMCIResourcePrivilegeType *allowPrivs,
                  int numDenyPrivs,
                  VMCIResourcePrivilegeType *denyPrivs)
{
   int i;
   VMCIResourceClient *client;
   
   if (VMCI_HANDLE_EQUAL(clientHandle, VMCI_INVALID_HANDLE)) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   client = VMCI_AllocKernelMem(sizeof *client, 
                                VMCI_MEMORY_NONPAGED | VMCI_MEMORY_ATOMIC);
   if (client == NULL) {
      VMCILOG((LGPFX"Failed to create new client for resource %p.\n",
               resource));
      return VMCI_ERROR_NO_MEM;
   }
   client->handle = clientHandle;
   client->refCount = 1;

   /* Initialize all privs to VMCI_PRIV_NOT_SET. */
   for (i = 0; i < VMCI_NUM_PRIVILEGES; i++) {
      client->privilege[i] = VMCI_PRIV_NOT_SET;
   }

   /* Set allow privileges. */
   for (i = 0; i < numAllowPrivs; i++) {
      client->privilege[allowPrivs[i]] = VMCI_PRIV_ALLOW;
   }

   /* Set deny privileges, any overlap results in privilege being denied. */
   for (i = 0; i < numDenyPrivs; i++) {
      client->privilege[denyPrivs[i]] = VMCI_PRIV_DENY;
   }

#ifdef VMX86_DEBUG
   {
      VMCIResourceClient *cur = resource->clients;
      while (cur && !VMCI_HANDLE_EQUAL(cur->handle, clientHandle)) {
	 cur = cur->next;
      }
      ASSERT(cur == NULL);
   }
#endif // VMX86_DEBUG
   client->next = resource->clients;
   resource->clients = client;

   return VMCI_SUCCESS;
}


/*
 *-----------------------------------------------------------------------------------
 *
 * ResourceRemoveClient --
 *
 *      Removes a client from the resource's client list and decrements the reference
 *      count. Assumes resource->clientsLock is held.
 *
 * Results:
 *      Returns VMCI_SUCCESS on success, appropriate error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------------
 */

static void
ResourceRemoveClient(VMCIResource *resource,
                     VMCIResourceClient *client)
{
   VMCIResourceClient *prev, *cur;

   ASSERT(resource && client && client->refCount > 0);
   prev = NULL;
   cur = resource->clients;
   while (cur && !VMCI_HANDLE_EQUAL(cur->handle, client->handle)) {
      prev = cur;
      cur = cur->next;
   }
   ASSERT(cur && cur == client);
   if (prev != NULL) {
      prev->next = cur->next;
   } else {
      resource->clients = cur->next;
   }

   ResourceReleaseClient(resource, client);
}


/* Public Resource Access Control API. */

/*
 *-----------------------------------------------------------------------------------
 *
 * VMCIResource_Init --
 *
 *      Initializes the VMCI Resource Access Control API. Creates a hashtable
 *      to hold all resources, and registers vectors and callbacks for hypercalls.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------------
 */

int
VMCIResource_Init(void)
{
   resourceTable = VMCIHashTable_Create(128);
   if (resourceTable == NULL) {
      VMCILOG((LGPFX"Failed creating a resource hash table for VMCI.\n"));
      return VMCI_ERROR_NO_MEM;
   }

   return VMCI_SUCCESS;
}


/*
 *-----------------------------------------------------------------------------------
 *
 * VMCIResource_Exit --
 *
 *      Cleans up resources.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------------
 */

void
VMCIResource_Exit(void)
{
   /* Cleanup resources.*/
  if (resourceTable) {
    VMCIHashTable_Destroy(resourceTable);
  }
}


/*
 *-------------------------------------------------------------------------
 *
 *  VMCIResource_GetID --
 *
 *     Return resource ID. The VMCI_CONTEXT_RESOURCE_ID is reserved so we we
 *     start from its value + 1. XXX should keep account to know when id is 
 *     free to use again.
 *
 *  Result:
 *     VMCI resource id.
 *
 *  Side effects:
 *     None.
 *     
 *     
 *-------------------------------------------------------------------------
 */

VMCIId
VMCIResource_GetID(void)
{
   VMCIId cid = Atomic_FetchAndInc(&resourceID);
   if (0 == cid) {
      /* Counter overflow -- FIXME */
      Warning("VMCIResource_GetID() counter overflow.\n");
      PANIC();
   }
   return cid;
}


/*
 *-----------------------------------------------------------------------------------
 *
 * VMCIResource_Add --
 *
 * Results:
 *      VMCI_SUCCESS if successful, error code if not.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------------
 */

int
VMCIResource_Add(VMCIResource *resource,                // IN
                 VMCIResourceType resourceType,         // IN
                 VMCIHandle resourceHandle,             // IN
                 VMCIHandle ownerHandle,                // IN
                 int numValidPrivs,                     // IN
                 VMCIResourcePrivilegeType *validPrivs, // IN
                 VMCIResourceFreeCB containerFreeCB,    // IN
                 void *containerObject)                 // IN
{
   int result, i;
   VMCIResourcePrivilegeType ownerPrivs[2] = {VMCI_PRIV_CH_PRIV, 
                                              VMCI_PRIV_DESTROY_RESOURCE};
   ASSERT(resource);

   if (VMCI_HANDLE_EQUAL(resourceHandle, VMCI_INVALID_HANDLE) || 
       VMCI_HANDLE_EQUAL(ownerHandle, VMCI_INVALID_HANDLE) ||
       numValidPrivs < 1) {
      VMCILOG((LGPFX"Invalid arguments resource 0x%x:0x%x, owner 0x%x:0x%x, "
               "num valid privs %d.\n",
               resourceHandle.context, resourceHandle.resource,
               ownerHandle.context, ownerHandle.resource, numValidPrivs));
      return VMCI_ERROR_INVALID_ARGS;
   }

   VMCIHashTable_InitEntry(&resource->hashEntry, resourceHandle);
   resource->type = resourceType;
   resource->containerFreeCB = containerFreeCB;
   resource->containerObject = containerObject;
   resource->handle = resourceHandle;
   resource->registrationCount = 0;
   
   for (i = 0; i < VMCI_NUM_PRIVILEGES; i++) {
      resource->validPrivs[i] = VMCI_PRIV_NOT_SET;
   }

   /* Owner privs are always valid. */
   resource->validPrivs[VMCI_PRIV_CH_PRIV] = VMCI_PRIV_VALID;
   resource->validPrivs[VMCI_PRIV_DESTROY_RESOURCE] = VMCI_PRIV_VALID;

   /* Specify what privs aside from owner privs can be set. */
   for (i = 0; i < numValidPrivs; i++) {
      resource->validPrivs[validPrivs[i]] = VMCI_PRIV_VALID;
   }

   VMCI_InitLock(&resource->clientsLock,
                 "VMCIResourceClientsLock",
                 VMCI_LOCK_RANK_MIDDLE_LOW);
   resource->clients = NULL;

   /* Add owner as client with the ownerPrivs privileges. */
   result = ResourceAddClient(resource, ownerHandle, 2, ownerPrivs, 0, NULL);
   if (result != VMCI_SUCCESS) {
      VMCILOG((LGPFX"Failed to create owner client.\n"));
      VMCI_CleanupLock(&resource->clientsLock);
      return result;
   }

   /* Add resource to hashtable. */
   result = VMCIHashTable_AddEntry(resourceTable, &resource->hashEntry);
   if (result != VMCI_SUCCESS) {
      VMCILOG((LGPFX"Failed to add entry to hash table.\n"));
      VMCI_CleanupLock(&resource->clientsLock);
      return result;
   }

   return result;
}


/*
 *-----------------------------------------------------------------------------------
 *
 * VMCIResource_Remove --
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------------
 */

void
VMCIResource_Remove(VMCIHandle resourceHandle,     // IN:
                    VMCIResourceType resourceType) // IN:
{
   VMCILockFlags flags;
   VMCIResource *resource = VMCIResource_Get(resourceHandle, resourceType);

   if (resource == NULL) {
      return;
   }
   
   /*
    * Remove all clients from resource, this will cause others to fail accessing
    * the resource.
    */
   VMCI_GrabLock(&resource->clientsLock, &flags);
   while (resource->clients) {
      ResourceRemoveClient(resource, resource->clients);
   }
   VMCI_ReleaseLock(&resource->clientsLock, flags);

   /* Remove resource from hashtable. */
   VMCIHashTable_RemoveEntry(resourceTable, &resource->hashEntry);
   
   VMCIResource_Release(resource);
   /* resource could be freed by now. */
}


/*
 *-----------------------------------------------------------------------------------
 *
 * VMCIResource_Get --
 *
 * Results:
 *      Resource is successful. Otherwise NULL.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------------
 */

VMCIResource *
VMCIResource_Get(VMCIHandle resourceHandle,     // IN
                 VMCIResourceType resourceType) // IN
{
   VMCIResource *resource;
   VMCIHashEntry *entry = VMCIHashTable_GetEntry(resourceTable, resourceHandle);
   if (entry == NULL) {
      return NULL;
   }
   resource = RESOURCE_CONTAINER(entry, VMCIResource, hashEntry);
   if ((resourceType == VMCI_RESOURCE_TYPE_ANY) || (resource->type == resourceType)) {
      return resource;
   }
   VMCIHashTable_ReleaseEntry(resourceTable, entry);
   return NULL;
}


/*
 *-----------------------------------------------------------------------------------
 *
 * VMCIResource_GetPair --
 *
 *      Retrieves the pointers for a pair of resources. The handles
 *      need not be of the same type. Either or both of the returned
 *      pointers may be NULL but only if the respective handle wasn't
 *      found.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------------
 */

void
VMCIResource_GetPair(VMCIHandle resourceHandles[2],     // IN
                     VMCIResourceType resourceTypes[2], // IN
                     VMCIResource *resources[2])        // OUT: the resource pointers
{
   VMCIHashEntry *entries[2];
   VMCIResource *resource;
   int i;

   VMCIHashTable_GetEntries(resourceTable,
                            resourceHandles,
                            ARRAYSIZE(entries),
                            entries);

   for (i = 0; i < ARRAYSIZE(entries); i++) {      
      if (entries[i] == NULL) {
         resources[i] = NULL;
      } else {
         resource = RESOURCE_CONTAINER(entries[i], VMCIResource, hashEntry);
         if ((resourceTypes[i] == VMCI_RESOURCE_TYPE_ANY) ||
             (resource->type == resourceTypes[i])) {
            resources[i] = resource;
         } else {
            VMCIHashTable_ReleaseEntry(resourceTable, entries[i]);
            resources[i] = NULL;
         }
      }
   }
}


/*
 *-----------------------------------------------------------------------------------
 *
 * VMCIResourceDoRemove --
 *
 *      Deallocates data structures associated with the given resource
 *      and invoke any call back registered for the resource.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May deallocate memory and invoke a callback for the removed resource.
 *
 *-----------------------------------------------------------------------------------
 */

static void INLINE
VMCIResourceDoRemove(VMCIResource *resource)
{
   VMCILockFlags flags;

   ASSERT(resource);

   VMCI_GrabLock(&resource->clientsLock, &flags);
   while (resource->clients) {
      ResourceRemoveClient(resource, resource->clients);
   }
   VMCI_ReleaseLock(&resource->clientsLock, flags);
   VMCI_CleanupLock(&resource->clientsLock);
   
   if (resource->containerFreeCB) {
      resource->containerFreeCB(resource->containerObject);
      /* Resource has been freed don't dereference it. */
   }
}


/*
 *-----------------------------------------------------------------------------------
 *
 * VMCIResource_Release --
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      resource's containerFreeCB will get called if last reference.
 *
 *-----------------------------------------------------------------------------------
 */

int
VMCIResource_Release(VMCIResource *resource)
{
   int result;

   ASSERT(resource);

   result = VMCIHashTable_ReleaseEntry(resourceTable, &resource->hashEntry);
   if (result == VMCI_SUCCESS_ENTRY_DEAD) {
      VMCIResourceDoRemove(resource);
   }

   /* 
    * We propagate the information back to caller in case it wants to know
    * whether entry was freed.
    */      
   return result;
}


/*
 *-----------------------------------------------------------------------------------
 *
 * VMCIResource_ReleasePair --
 *
 *      Releases a pair of resources. If a resource pointer is NULL,
 *      it will be ignored and the corresponding result value will be
 *      set to VMCI_SUCCES.
 *
 * Results:
 *      VMCI_SUCCESS_ENTRY_DEAD if any of the resources were deleted - the
 *         results for the individual resources can be obtained from the
 *         results array.
 *      VMCI_SUCCESS otherwise.
 *
 * Side effects:
 *      Resources' containerFreeCB will get called if last reference.
 *
 *-----------------------------------------------------------------------------------
 */

int
VMCIResource_ReleasePair(VMCIResource *resource[2], // IN: resources to be released
                         int results[2])            // OUT: per resource results
{
   int result;
   
   if ((resource[0] != NULL) && (resource[1] != NULL)) {
      VMCIHashEntry *entries[2];

      entries[0] = &resource[0]->hashEntry;
      entries[1] = &resource[1]->hashEntry;

      result = VMCIHashTable_ReleaseEntries(resourceTable,
                                            entries, 
                                            ARRAYSIZE(entries),
                                            results);

      if (result == VMCI_SUCCESS_ENTRY_DEAD) {
         if (results[0] == VMCI_SUCCESS_ENTRY_DEAD) {
            VMCIResourceDoRemove(resource[0]);
         }
         if (results[1] == VMCI_SUCCESS_ENTRY_DEAD) {
            VMCIResourceDoRemove(resource[1]);
         }
      }
   } else {
      result = results[0] = results[1] = VMCI_SUCCESS;
      if (resource[0] != NULL) {
         result = results[0] = VMCIHashTable_ReleaseEntry(resourceTable,
                                                          &resource[0]->hashEntry);
      } else if (resource[1] != NULL) {
         result = results[1] = VMCIHashTable_ReleaseEntry(resourceTable,
                                                          &resource[1]->hashEntry);
      }
   }
   return result;
}


/*
 *-----------------------------------------------------------------------------------
 *
 * VMCIResource_AddClientPrivileges --
 *
 * Results:
 *      VMCI_SUCCESS if successful, error code if not.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------------
 */

int
VMCIResource_AddClientPrivileges(VMCIHandle resourceHandle,
                                 VMCIHandle clientHandle,
                                 int numAllowPrivs,
                                 VMCIResourcePrivilegeType *allowPrivs,
                                 int numDenyPrivs,
                                 VMCIResourcePrivilegeType *denyPrivs)
{
   int i;
   VMCIResource *resource;
   VMCIResourceClient *client;
   int result = VMCI_SUCCESS;
   VMCILockFlags flags;

   if (VMCI_HANDLE_EQUAL(resourceHandle, VMCI_INVALID_HANDLE) || 
       VMCI_HANDLE_EQUAL(clientHandle, VMCI_INVALID_HANDLE) ||
       numAllowPrivs + numDenyPrivs < 1) {
      VMCILOG((LGPFX"AddClientPrivs: Invalid args.\n"));
      return VMCI_ERROR_INVALID_ARGS;
   }

#ifdef VMX86_DEBUG
   VMCI_DEBUG_LOG((LGPFX"AddClientPrivs: Adding allow privs:\n"));
   for (i = 0; i < numAllowPrivs; i++) {
      VMCI_DEBUG_LOG((LGPFX"AddClientPrivs: %d. 0x%x.\n", i, allowPrivs[i]));
   }
   VMCI_DEBUG_LOG((LGPFX"AddClientPrivs: Adding deny privs:\n"));
   for (i = 0; i < numDenyPrivs; i++) {
      VMCI_DEBUG_LOG((LGPFX"AddClientPrivs: %d. 0x%x.\n", i, denyPrivs[i]));
   }
   VMCI_DEBUG_LOG((LGPFX"AddClientPrivs: to client 0x%"FMT64"x for resource "
             "0x%"FMT64"x.\n", clientHandle, resourceHandle));
#endif // VMX86_DEBUG

   resource = VMCIResource_Get(resourceHandle, VMCI_RESOURCE_TYPE_ANY);
   if (resource == NULL) {
      VMCILOG((LGPFX"AddClientPrivs: No resource.\n"));
      return VMCI_ERROR_INVALID_ARGS;
   }

   /* Validate privileges up front. */
   result = ResourceValidatePrivileges(resource, numAllowPrivs, allowPrivs);
   if (result != VMCI_SUCCESS) {
      VMCILOG((LGPFX"AddClientPrivs: Invalid allow privs.\n"));
      goto done;
   }
   result = ResourceValidatePrivileges(resource, numDenyPrivs, denyPrivs);
   if (result != VMCI_SUCCESS) {
      VMCILOG((LGPFX"AddClientPrivs: Invalid deny privs.\n"));
      goto done;
   }

   /* If client doesn't exists, create it. */
   VMCI_GrabLock(&resource->clientsLock, &flags);
   client = ResourceGetClient(resource, clientHandle);
   if (client == NULL) {
      result = ResourceAddClient(resource, clientHandle, numAllowPrivs, 
                                 allowPrivs, numDenyPrivs, denyPrivs);
      VMCI_ReleaseLock(&resource->clientsLock, flags);
      goto done;
   }

   /* 
    * If same privilege is present in both the allow and deny array. The deny
    * privilege takes precedence.
    */
   for (i = 0; i < numAllowPrivs; i++) {
      client->privilege[allowPrivs[i]] = VMCI_PRIV_ALLOW;
   }
   for (i = 0; i < numDenyPrivs; i++) {
      client->privilege[denyPrivs[i]] = VMCI_PRIV_DENY;
   }

   ResourceReleaseClient(resource, client);
   VMCI_ReleaseLock(&resource->clientsLock, flags);
  done:
   if (resource) {
      VMCIResource_Release(resource);
   }

   return result;
}


/*
 *-----------------------------------------------------------------------------------
 *
 * VMCIResource_RemoveClientPrivileges --
 *
 * Results:
 *      VMCI_SUCCESS if successful, error code if not.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------------
 */

int
VMCIResource_RemoveClientPrivileges(VMCIHandle resourceHandle,
                                    VMCIHandle clientHandle,
                                    int numPrivs, 
                                    VMCIResourcePrivilegeType *privs)
{
   int i;
   VMCIResource *resource;
   VMCIResourceClient *client;
   Bool noPrivs;
   int result = VMCI_SUCCESS;
   VMCILockFlags flags;

   if (VMCI_HANDLE_EQUAL(resourceHandle, VMCI_INVALID_HANDLE) || 
       VMCI_HANDLE_EQUAL(clientHandle, VMCI_INVALID_HANDLE) ||
       numPrivs < 1) {
      VMCILOG((LGPFX"RemoveClientPrivs: Invalid args.\n"));
      return VMCI_ERROR_INVALID_ARGS;
   }

#ifdef VMX86_DEBUG
   VMCI_DEBUG_LOG((LGPFX"RemoveClientPrivs: Removing privs:\n"));
   for (i = 0; i < numPrivs; i++) {
      VMCI_DEBUG_LOG((LGPFX"RemoveClientPrivs: %d. 0x%x.\n", i, privs[i]));
   }
   VMCI_DEBUG_LOG((LGPFX"RemoveClientPrivs: from client 0x%"FMT64"x for "
                   "resource 0x%"FMT64"x.\n", clientHandle, resourceHandle));
#endif // VMX86_DEBUG

   resource = VMCIResource_Get(resourceHandle, VMCI_RESOURCE_TYPE_ANY);
   if (resource == NULL) {
      VMCILOG((LGPFX"RemoveClientPrivs: Failed getting resource.\n"));
      result = VMCI_ERROR_INVALID_ARGS;
      goto done;
   }

   /* Validate privileges up front to avoid partial changes of privileges. */
   result = ResourceValidatePrivileges(resource, numPrivs, privs);
   if (result != VMCI_SUCCESS) {
      VMCILOG((LGPFX"RemoveClientPrivs: Invalid privs.\n"));
      goto done;
   }

   VMCI_GrabLock(&resource->clientsLock, &flags);
   client = ResourceGetClient(resource, clientHandle);
   if (client == NULL) {
      VMCI_ReleaseLock(&resource->clientsLock, flags);
      VMCILOG((LGPFX"RemoveClientPrivs: No client.\n"));
      result = VMCI_ERROR_INVALID_ARGS;
      goto done;
   }

   for (i = 0; i < numPrivs; i++) {
      /* Remove client privilege. */
      client->privilege[privs[i]] = VMCI_PRIV_NOT_SET;
   }

   /* Validate if client has no more privileges set and remove if so. */
   noPrivs = TRUE;
   for (i = 0; i < VMCI_NUM_PRIVILEGES; i++) {
      if (client->privilege[i] != VMCI_PRIV_NOT_SET) {
	 noPrivs = FALSE;
	 break;
      }
   }
   if (noPrivs) {
      /* 
       * This client no longer has any privileges set for resource. We remove it 
       * which also decrements the reference count.
       */
      VMCI_DEBUG_LOG((LGPFX"RemoveClientPrivs: Removing client 0x%"FMT64"x.\n",
                      clientHandle));
      ResourceRemoveClient(resource, client);
   }
   ResourceReleaseClient(resource, client);
   VMCI_ReleaseLock(&resource->clientsLock, flags);

  done:
   if (resource) {
      VMCIResource_Release(resource);
   }

   return result;
}


/*
 *-----------------------------------------------------------------------------------
 *
 * VMCIResource_RemoveAllClientPrivileges --
 *
 * Results:
 *      VMCI_SUCCESS if successful, error code if not.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------------
 */

int
VMCIResource_RemoveAllClientPrivileges(VMCIHandle resourceHandle,
                                       VMCIHandle clientHandle)
{
   VMCIResource *resource;
   VMCIResourceClient *client;
   int result = VMCI_SUCCESS;
   VMCILockFlags flags;

   if (VMCI_HANDLE_EQUAL(resourceHandle, VMCI_INVALID_HANDLE) || 
       VMCI_HANDLE_EQUAL(clientHandle, VMCI_INVALID_HANDLE)) {
      VMCILOG((LGPFX"RemoveAllClientPrivs: Invalid args.\n"));
      return VMCI_ERROR_INVALID_ARGS;
   }

   resource = VMCIResource_Get(resourceHandle, VMCI_RESOURCE_TYPE_ANY);
   if (resource == NULL) {
      result = VMCI_ERROR_INVALID_ARGS;
      goto done;
   }

   VMCI_GrabLock(&resource->clientsLock, &flags);
   client = ResourceGetClient(resource, clientHandle);
   if (client == NULL) {
      VMCI_ReleaseLock(&resource->clientsLock, flags);
      result = VMCI_ERROR_INVALID_ARGS;
      goto done;
   }

   ResourceRemoveClient(resource, client);

   ResourceReleaseClient(resource, client);
   VMCI_ReleaseLock(&resource->clientsLock, flags);
  done:
   if (resource) {
      VMCIResource_Release(resource);
   }

   return result;
}


/*
 *-----------------------------------------------------------------------------------
 *
 * VMCIResource_CheckClientPrivilege --
 *
 * Results:
 *      VMCI_SUCCESS_ACCESS_GRANTED if privilege is allowed, VMCI_ERROR_NO_ACCESS if 
 *      privilege denied, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------------
 */

int
VMCIResource_CheckClientPrivilege(VMCIHandle resourceHandle,       // IN:
                                  VMCIHandle clientHandle,         // IN:
                                  VMCIResourcePrivilegeType priv)  // IN:
{
   VMCIResource *resource;
   int result = VMCI_ERROR_INVALID_PRIV;

   if (VMCI_HANDLE_EQUAL(resourceHandle, VMCI_INVALID_HANDLE)) {
      VMCILOG((LGPFX"CheckClientPriv: Invalid args.\n"));
      return VMCI_ERROR_INVALID_ARGS;
   }

   resource = VMCIResource_Get(resourceHandle, VMCI_RESOURCE_TYPE_ANY);
   if (resource == NULL) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   result = VMCIResource_CheckClientPrivilegePtr(resource,
                                                 clientHandle,
                                                 priv);

   VMCIResource_Release(resource);

   return result;
}


/*
 *-----------------------------------------------------------------------------------
 *
 * VMCIResource_CheckClientPrivilegePtr --
 *
 *      A version of VMCIResource_CheckClientPrivilege, that takes an already
 *      know resource pointer as argument instead of a handle.
 *
 * Results:
 *      VMCI_SUCCESS_ACCESS_GRANTED if privilege is allowed, VMCI_ERROR_NO_ACCESS if 
 *      privilege denied, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------------
 */

int
VMCIResource_CheckClientPrivilegePtr(VMCIResource *resource,          // IN:
                                     VMCIHandle clientHandle,         // IN:
                                     VMCIResourcePrivilegeType priv)  // IN:
{
   VMCILockFlags flags;
   VMCIResourceClient *client;
   VMCIContext *context = NULL;
   int result = VMCI_ERROR_INVALID_PRIV;

   if (resource == NULL ||
       VMCI_HANDLE_EQUAL(clientHandle, VMCI_INVALID_HANDLE) || 
       priv >= VMCI_NUM_PRIVILEGES) {
      VMCILOG((LGPFX"CheckClientPrivPtr: Invalid args.\n"));
      return VMCI_ERROR_INVALID_ARGS;
   }

   /* 
    * We short-circuit this for now until we decide what if any privilege 
    * checking we want.
    */
   return VMCI_SUCCESS_ACCESS_GRANTED;

   /*
    * Clients can be either groups or contexts. No other clients are supported
    * at this point. ResourceAddClientPrivileges enforces this via 
    * ResourceCanSeeClient by looking for the VMCI_PRIV_ASSIGN_CLIENT privilege
    * or by verifying that the client is the caller context.
    * For both group and context clients we check the client directly to see if
    * it has the privilege set. If the privilege is not set we return error 
    * for groups but for contexts we continue by checking if the context is
    * a member of a group that has the requested privilege to the given resource
    * and grant access if so.
    */

   VMCI_GrabLock(&resource->clientsLock, &flags);
   client = ResourceGetClient(resource, clientHandle);

   if (clientHandle.resource == VMCI_CONTEXT_RESOURCE_ID && 
       (client == NULL || client->privilege[priv] == VMCI_PRIV_NOT_SET)) {

      /* 
       * At this point we know the client is a context. Check if it is a member
       * of a group that is a client to the resource and has the privilege set.
       */

      int i;
      VMCILockFlags flags;
      VMCIId contextID = VMCI_HANDLE_TO_CONTEXT_ID(clientHandle);

      if (client) {
	 ResourceReleaseClient(resource, client);
	 client = NULL;
      }

      context = VMCIContext_Get(contextID);
      if (context == NULL) {
	 result = VMCI_ERROR_INVALID_ARGS;
	 goto done;
      }
    
      VMCI_GrabLock(&context->lock, &flags);
      for (i = 0; i < VMCIHandleArray_GetSize(context->groupArray); i++) {
	 VMCIHandle groupHandle = VMCIHandleArray_GetEntry(context->groupArray, i);
	 client = ResourceGetClient(resource, groupHandle);
	 if (client != NULL) {
	    /* 
	     * Check if client has privilege, if so stop, otherwise continue.
	     * Semantic currently is first group with privilege set has
	     * precedence. This could be enhance with a group priority where
	     * higher priority was checked first.
	     */
	    if (client->privilege[priv] != VMCI_PRIV_NOT_SET) {
	       VMCI_DEBUG_LOG((LGPFX"Client 0x%"FMT64"x is a member of group "
                               "0x%"FMT64"x which has priv 0x%x set to %d for "
                               "resource 0x%"FMT64"x.\n", 
                               clientHandle, groupHandle, priv, 
                               client->privilege[priv], resourceHandle));
	       break;
	    }
	    ResourceReleaseClient(resource, client);
	    client = NULL;
	 }
      }
      VMCI_ReleaseLock(&context->lock, flags);
   }

   if (client) {
      if (client->privilege[priv] == VMCI_PRIV_ALLOW) {
	 result = VMCI_SUCCESS_ACCESS_GRANTED;
      }
      if (client->privilege[priv] == VMCI_PRIV_DENY) {
	 result = VMCI_ERROR_NO_ACCESS;
      }
      ResourceReleaseClient(resource, client);
   }
   VMCI_DEBUG_LOG((LGPFX"Checking if client 0x%"FMT64"x has priv 0x%x for "
                   "resource 0x%"FMT64"x, result %d.\n", clientHandle, priv,
                   resourceHandle, result));

  done:
   VMCI_ReleaseLock(&resource->clientsLock, flags);
   if (context != NULL) {
      /*
       * We cannot release the context while holding a lock.
       */

      VMCIContext_Release(context);
   }
   return result;
}


/*
 *-----------------------------------------------------------------------------------
 *
 * VMCIResource_IncDsRegCount --
 *       Increments the registrationCount associated with a resource.
 *
 * Results:
 *       VMCI_SUCCESS on success, error code otherwise.      
 *
 * Side effects:
 *       None.
 *
 *-----------------------------------------------------------------------------------
 */

int
VMCIResource_IncDsRegCount(VMCIResource *resource) // IN:
{
   VMCILockFlags flags;

   if (!resource) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   VMCI_GrabLock(&resource->clientsLock, &flags);
   resource->registrationCount++;
   VMCI_ReleaseLock(&resource->clientsLock, flags);

   return VMCI_SUCCESS;
}


/*
 *-----------------------------------------------------------------------------------
 *
 * VMCIResource_DecDsRegCount --
 *       Decrements the registrationCount associated with a resource.
 *
 * Results:
 *       VMCI_SUCCESS on success, error code otherwise.       
 *
 * Side effects:
 *       None.
 *
 *-----------------------------------------------------------------------------------
 */

int
VMCIResource_DecDsRegCount(VMCIResource *resource) // IN:
{
   VMCILockFlags flags;

   if (!resource) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   VMCI_GrabLock(&resource->clientsLock, &flags);
   ASSERT(resource->registrationCount > 0);
   resource->registrationCount--;
   VMCI_ReleaseLock(&resource->clientsLock, flags);

   return VMCI_SUCCESS;
}
