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
 * vmciHashtable.c --
 *
 *     Implementation of the VMCI Hashtable. 
 *     TODO: Look into what is takes to use lib/misc/hashTable.c instead of 
 *     our own implementation.
 */

#if defined(linux) && !defined VMKERNEL
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

#define LGPFX "VMCIHashTable: "

#include "vm_assert.h"
#ifdef VMKERNEL
#include "vm_libc.h"
#endif // VMKERNEL
#include "vm_atomic.h"
#include "vmci_kernel_if.h"
#include "vmci_infrastructure.h"
#include "vmciDriver.h"
#include "vmciHashtable.h"
#include "vmware.h"

static int HashTableUnlinkEntry(VMCIHashTable *table, VMCIHashEntry *entry);
static Bool VMCIHashTableEntryExistsLocked(VMCIHashTable *table,
                                           VMCIHandle handle);


/*
 *------------------------------------------------------------------------------
 *
 *  VMCIHashTable_Create --
 *     XXX Factor out the hashtable code to be shared amongst host and guest.
 * 
 *  Result:
 *     None.
 *     
 *------------------------------------------------------------------------------
 */

VMCIHashTable *
VMCIHashTable_Create(int size)
{
   VMCIHashTable *table = VMCI_AllocKernelMem(sizeof *table,
                                              VMCI_MEMORY_NONPAGED);
   if (table == NULL) {
      return NULL;
   }

   table->entries = VMCI_AllocKernelMem(sizeof *table->entries * size,
                                        VMCI_MEMORY_NONPAGED);
   if (table->entries == NULL) {
      VMCI_FreeKernelMem(table, sizeof *table);
      return NULL;
   }
   memset(table->entries, 0, sizeof *table->entries * size);
   table->size = size;
   VMCI_InitLock(&table->lock,
                 "VMCIHashTableLock",
                 VMCI_LOCK_RANK_HIGH);   

   return table;
}


/*
 *------------------------------------------------------------------------------
 *
 *  VMCIHashTable_Destroy --
 *     This function should be called at module exit time.
 *     We rely on the module ref count to insure that no one is accessing any
 *     hash table entries at this point in time. Hence we should be able to just
 *     remove all entries from the hash table.
 * 
 *  Result:
 *     None.
 *     
 *------------------------------------------------------------------------------
 */

void
VMCIHashTable_Destroy(VMCIHashTable *table)
{
   VMCILockFlags flags;
#if 0
   DEBUG_ONLY(int i;)
   DEBUG_ONLY(int leakingEntries = 0;)
#endif

   ASSERT(table);

   VMCI_GrabLock(&table->lock, &flags);
#if 0
#ifdef VMX86_DEBUG
   for (i = 0; i < table->size; i++) {
      VMCIHashEntry *head = table->entries[i];
      while (head) {
         leakingEntries++;
         head = head->next;
      }
   }
   if (leakingEntries) {
      VMCILOG((LGPFX"Leaking %d hash table entries for table %p.\n",
               leakingEntries, table));
   }
#endif // VMX86_DEBUG
#endif
   VMCI_FreeKernelMem(table->entries, sizeof *table->entries * table->size);
   table->entries = NULL;
   VMCI_ReleaseLock(&table->lock, flags);
   VMCI_CleanupLock(&table->lock);   
   VMCI_FreeKernelMem(table, sizeof *table);
}


/*
 *------------------------------------------------------------------------------
 *
 *  VMCIHashTable_InitEntry --
 *     Initializes a hash entry;
 * 
 *  Result:
 *     None.
 *     
 *------------------------------------------------------------------------------
 */
void
VMCIHashTable_InitEntry(VMCIHashEntry *entry,  // IN
                        VMCIHandle handle)     // IN
{
   ASSERT(entry);
   entry->handle = handle;
   entry->refCount = 0;
}


/*
 *------------------------------------------------------------------------------
 *
 *  VMCIHashTable_AddEntry --
 *     XXX Factor out the hashtable code to be shared amongst host and guest.
 * 
 *  Result:
 *     None.
 *     
 *------------------------------------------------------------------------------
 */

int
VMCIHashTable_AddEntry(VMCIHashTable *table,  // IN
                       VMCIHashEntry *entry)  // IN
{
   int idx;
   VMCILockFlags flags;

   ASSERT(entry);
   ASSERT(table);

   VMCI_GrabLock(&table->lock, &flags);
   if (VMCIHashTableEntryExistsLocked(table, entry->handle)) {
      VMCILOG((LGPFX"Entry's handle 0x%x:0x%x already exists.\n",
               entry->handle.context, entry->handle.resource));
      VMCI_ReleaseLock(&table->lock, flags);
      return VMCI_ERROR_DUPLICATE_ENTRY;
   }

   idx = VMCI_Hash(entry->handle, table->size);
   ASSERT(idx < table->size);

   /* New entry is added to top/front of hash bucket. */
   entry->refCount++;
   entry->next = table->entries[idx];
   table->entries[idx] = entry;
   VMCI_ReleaseLock(&table->lock, flags);

   return VMCI_SUCCESS;
}


/*
 *------------------------------------------------------------------------------
 *
 *  VMCIHashTable_RemoveEntry --
 *     XXX Factor out the hashtable code to shared amongst API and perhaps 
 *     host and guest.
 *
 *  Result:
 *     None.
 *     
 *------------------------------------------------------------------------------
 */

int
VMCIHashTable_RemoveEntry(VMCIHashTable *table, // IN
                          VMCIHashEntry *entry) // IN
{
   int result;
   VMCILockFlags flags;

   ASSERT(table);
   ASSERT(entry);

   VMCI_GrabLock(&table->lock, &flags);
   
   /* First unlink the entry. */
   result = HashTableUnlinkEntry(table, entry);
   if (result != VMCI_SUCCESS) {
      /* We failed to find the entry. */
      goto done;
   }

   /* Decrement refcount and check if this is last reference. */
   entry->refCount--;
   if (entry->refCount == 0) {
      result = VMCI_SUCCESS_ENTRY_DEAD;
      goto done;
   }
   
  done:
   VMCI_ReleaseLock(&table->lock, flags);
   
   return result;
}


/*
 *------------------------------------------------------------------------------
 *
 *  VMCIHashTableGetEntryLocked --
 *     
 *       Looks up an entry in the hash table, that is already locked.
 *
 *  Result:
 *       If the element is found, a pointer to the element is returned.
 *       Otherwise NULL is returned.
 *
 *  Side effects:
 *       The reference count of the returned element is increased.
 *     
 *------------------------------------------------------------------------------
 */

static INLINE VMCIHashEntry *
VMCIHashTableGetEntryLocked(VMCIHashTable *table,  // IN
                            VMCIHandle handle)     // IN
{
   VMCIHashEntry *cur = NULL;
   int idx;

   ASSERT(!VMCI_HANDLE_EQUAL(handle, VMCI_INVALID_HANDLE));
   ASSERT(table);

   idx = VMCI_Hash(handle, table->size);
   
   cur = table->entries[idx];
   while (TRUE) {
      if (cur == NULL) {
         break;
      }

      if (VMCI_HANDLE_EQUAL(cur->handle, handle)) {
         cur->refCount++;
         break;
      }
      cur = cur->next;
   }

   return cur;
}


/*
 *------------------------------------------------------------------------------
 *
 *  VMCIHashTable_GetEntry --
 *     XXX Factor out the hashtable code to shared amongst API and perhaps 
 *     host and guest.
 *
 *  Result:
 *     None.
 *     
 *------------------------------------------------------------------------------
 */

VMCIHashEntry *
VMCIHashTable_GetEntry(VMCIHashTable *table,  // IN
                       VMCIHandle handle)     // IN
{
   VMCIHashEntry *entry;
   VMCILockFlags flags;

   if (VMCI_HANDLE_EQUAL(handle, VMCI_INVALID_HANDLE)) {
     return NULL;
   }

   ASSERT(table);
   
   VMCI_GrabLock(&table->lock, &flags);
   entry = VMCIHashTableGetEntryLocked(table, handle);
   VMCI_ReleaseLock(&table->lock, flags);

   return entry;
}


/*
 *------------------------------------------------------------------------------
 *
 *  VMCIHashTable_GetEntries --
 *     
 *       Multiple entries are gotten from a hash table. This amortizes
 *       the locking costs of getting multiple entries from the same
 *       hash table.
 *
 *  Result:
 *       None.
 *     
 *------------------------------------------------------------------------------
 */

void
VMCIHashTable_GetEntries(VMCIHashTable *table,    // IN
                         VMCIHandle *handles,     // IN
                         size_t len,              // IN: Length of arrays.
                         VMCIHashEntry **entries) // OUT
                         
{
   VMCILockFlags flags;
   size_t i;

   ASSERT(table);
   ASSERT(handles);
   ASSERT(entries);

   
   VMCI_GrabLock(&table->lock, &flags);
   for (i = 0; i < len; i++) {
      if (VMCI_HANDLE_EQUAL(handles[i], VMCI_INVALID_HANDLE)) {
         entries[i] = NULL;
      } else {
         entries[i] = VMCIHashTableGetEntryLocked(table, handles[i]);
      }
   }
   VMCI_ReleaseLock(&table->lock, flags);
}


/*
 *------------------------------------------------------------------------------
 *
 *  VMCIHashTableReleaseEntryLocked --
 *      
 *       Releases an element previously obtained with
 *       VMCIHashTableGetEntryLocked.
 *
 *  Result:
 *       If the entry is removed from the hash table, VMCI_SUCCESS_ENTRY_DEAD
 *       is returned. Otherwise, VMCI_SUCCESS is returned.
 *
 *  Side effects:
 *       The reference count of the entry is decreased and the entry is removed
 *       from the hash table on 0.
 *
 *------------------------------------------------------------------------------
 */

static INLINE int
VMCIHashTableReleaseEntryLocked(VMCIHashTable *table,  // IN
                                VMCIHashEntry *entry)  // IN
{
   int result = VMCI_SUCCESS;

   ASSERT(table);
   ASSERT(entry);

   entry->refCount--;
   /* Check if this is last reference and report if so. */
   if (entry->refCount == 0) { 

      /*
       * Remove entry from hash table if not already removed. This could have
       * happened already because VMCIHashTable_RemoveEntry was called to unlink
       * it. We ignore if it is not found. Datagram handles will often have
       * RemoveEntry called, whereas SharedMemory regions rely on ReleaseEntry
       * to unlink the entry, since the creator does not call RemoveEntry when
       * it detaches.
       */

      HashTableUnlinkEntry(table, entry);
      result = VMCI_SUCCESS_ENTRY_DEAD;
   }

   return result;
}


/*
 *------------------------------------------------------------------------------
 *
 *  VMCIHashTable_ReleaseEntry --
 *     XXX Factor out the hashtable code to shared amongst API and perhaps 
 *     host and guest.
 *
 *  Result:
 *     None.
 *     
 *------------------------------------------------------------------------------
 */

int
VMCIHashTable_ReleaseEntry(VMCIHashTable *table,  // IN
                           VMCIHashEntry *entry)  // IN
{
   VMCILockFlags flags;
   int result;

   ASSERT(table);
   VMCI_GrabLock(&table->lock, &flags);
   result = VMCIHashTableReleaseEntryLocked(table, entry);
   VMCI_ReleaseLock(&table->lock, flags);

   return result;
}


/*
 *------------------------------------------------------------------------------
 *
 *  VMCIHashTable_ReleaseEntries --
 *
 *       Multiple entries are released from the given hash table. The
 *       result of each release operation is returned in the results
 *       array. This function is intended to amortize locking costs
 *       when multiple entries are released.
 *
 *  Result:
 *       VMCI_SUCCESS_ENTRY_DEAD is returned, if any of the releases resulted
 *       in VMCI_SUCCESS_ENTRY_DEAD. Otherwise, VMCI_SUCCESS is returned.
 *     
 *------------------------------------------------------------------------------
 */

int
VMCIHashTable_ReleaseEntries(VMCIHashTable *table,    // IN
                             VMCIHashEntry **entries, // IN
                             size_t len,              // IN: Length of arrays.
                             int *results)            // OUT
{
   VMCILockFlags flags;
   int result = VMCI_SUCCESS;
   size_t i;

   ASSERT(table);
   ASSERT(entries);
   ASSERT(results);

   VMCI_GrabLock(&table->lock, &flags);
   for (i = 0; i < len; i++) {
      results[i] = VMCIHashTableReleaseEntryLocked(table, entries[i]);
      if (results[i] == VMCI_SUCCESS_ENTRY_DEAD) {
         result = VMCI_SUCCESS_ENTRY_DEAD;
      }
   }
   VMCI_ReleaseLock(&table->lock, flags);

   return result;
}


/*
 *------------------------------------------------------------------------------
 *
 *  VMCIHashTable_EntryExists --
 *     XXX Factor out the hashtable code to shared amongst API and perhaps 
 *     host and guest.
 *
 *  Result:
 *     TRUE if handle already in hashtable. FALSE otherwise.
 *
 *  Side effects:
 *     None.
 *     
 *------------------------------------------------------------------------------
 */

Bool
VMCIHashTable_EntryExists(VMCIHashTable *table,  // IN
                          VMCIHandle handle)     // IN
{
   Bool exists;
   VMCILockFlags flags;

   ASSERT(table);

   VMCI_GrabLock(&table->lock, &flags);
   exists = VMCIHashTableEntryExistsLocked(table, handle);
   VMCI_ReleaseLock(&table->lock, flags);

   return exists;
}


/*
 *------------------------------------------------------------------------------
 *
 *  VMCIHashTableEntryExistsLocked --
 *     
 *     Unlocked version of VMCIHashTable_EntryExists.
 *
 *  Result:
 *     TRUE if handle already in hashtable. FALSE otherwise.
 *
 *  Side effects:
 *     None.
 *     
 *------------------------------------------------------------------------------
 */

static Bool
VMCIHashTableEntryExistsLocked(VMCIHashTable *table,  // IN
                               VMCIHandle handle)     // IN

{
   VMCIHashEntry *entry;
   int idx;
   
   ASSERT(table);

   idx = VMCI_Hash(handle, table->size);

   entry = table->entries[idx];
   while (entry) {
      if (VMCI_HANDLE_EQUAL(entry->handle, handle)) {
         return TRUE;
      }
      entry = entry->next;
   }

   return FALSE;
}


/*
 *------------------------------------------------------------------------------
 *
 *  HashTableUnlinkEntry --
 *     XXX Factor out the hashtable code to shared amongst API and perhaps 
 *     host and guest.
 *     Assumes caller holds table lock.
 *
 *  Result:
 *     None.
 *     
 *------------------------------------------------------------------------------
 */

static int
HashTableUnlinkEntry(VMCIHashTable *table, // IN
                     VMCIHashEntry *entry) // IN 
{
   int result;
   VMCIHashEntry *prev, *cur;
   int idx;

   idx = VMCI_Hash(entry->handle, table->size);

   prev = NULL;
   cur = table->entries[idx];
   while (TRUE) {
      if (cur == NULL) {
         result = VMCI_ERROR_NOT_FOUND;
         break;
      }
      if (VMCI_HANDLE_EQUAL(cur->handle, entry->handle)) {
         ASSERT(cur == entry);

         /* Remove entry and break. */
         if (prev) {
            prev->next = cur->next;
         } else {
            table->entries[idx] = cur->next;
         }
         cur->next = NULL;
         result = VMCI_SUCCESS;
         break;
      }
      prev = cur;
      cur = cur->next;
   }
   return result;
}
