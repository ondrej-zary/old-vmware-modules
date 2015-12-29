/*********************************************************
 * Copyright (C) 1998 VMware, Inc. All rights reserved.
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
 * phystrack.c --
 *
 *    track down the utilization of the physical pages.
 *
 *    Depending on configuration phystracker provides either 2-level or
 *    3-level structure to track whether page (specified by its MPN) is
 *    locked or no.  Linux uses 3-level structures with top limit of
 *    1TB (32bit) or 16TB (64bit).  Windows use 2-level structures
 *    ready to hold 128GB (32bit) or 2TB (64bit) of memory.  On Mac
 *    limit is 2-level 128GB.
 *
 *    2-level phystracker is built on top of 3-level one by collapsing
 *    middle level.
 */


#ifdef linux
/* Must come before any kernel header file --hpreg */
#   include "driver-config.h"

#   include <linux/string.h> /* memset() in the kernel */
#else
#   include <string.h>
#endif

#include "compat_kernel.h"
#include "vmware.h"
#include "vmx86.h"
#include "hostif.h"


#define BYTES_PER_ENTRY      (PAGE_SIZE)
#define PHYSTRACK_L3_ENTRIES (8 * BYTES_PER_ENTRY) /* 128MB */

#if defined(WINNT_DDK)
#if defined(VM_X86_64)
#define PHYSTRACK_L1_ENTRIES ((2048 + 4) * 8) /* 2 TB 64-bit W2k8 + 4 GB PCI */
#else
#define PHYSTRACK_L1_ENTRIES ((128 + 4) * 8) /* 128 GB hosts, max for 32-bit Windows */
#endif
#elif defined(__linux__)
#define PHYSTRACK_L2_ENTRIES (BYTES_PER_ENTRY / sizeof(void *)) /* 64GB or 128GB */
#ifdef VM_X86_64
/*
 * Currently MPN is 32 bits.  15 bits are in L3, 9 bits are in L2,
 * leaving 8 bits for L1...
 */
#define PHYSTRACK_L1_ENTRIES (256) /* 16TB. */
#else
#define PHYSTRACK_L1_ENTRIES (9)   /* 1TB.  Most probably impossible... */
#endif
#else
#define PHYSTRACK_L1_ENTRIES ((128 + 4) * 8) /* 128 GB */
#endif

#ifndef PHYSTRACK_L2_ENTRIES
#define PHYSTRACK_L2_ENTRIES (1)
#else
#define PHYSTRACK_3LEVEL (1)
#endif

typedef struct PhysTrackerL3 {
   uint8 bits[BYTES_PER_ENTRY];
} PhysTrackerL3;

#ifdef PHYSTRACK_3LEVEL
typedef struct PhysTrackerL2 {
   PhysTrackerL3 *dir[PHYSTRACK_L2_ENTRIES];
} PhysTrackerL2;
#else
typedef struct PhysTrackerL3 PhysTrackerL2;
#endif

typedef struct PhysTracker {
   int numVMs;
   PhysTrackerL2 *dir[PHYSTRACK_L1_ENTRIES];
} PhysTracker;

/* The global physTracker pointer used by PhysTrack_Init */
PhysTracker *physTracker = NULL;

/*
 * Convert MPN to p1, p2, and p3 indices.  p1/p2/p3 must be l-values.
 */
#define PHYSTRACK_MPN2IDX(mpn, p1, p2, p3) \
   do {                                    \
      p2 = (mpn) / PHYSTRACK_L3_ENTRIES;   \
      p1 = p2 / PHYSTRACK_L2_ENTRIES;      \
      p2 = p2 % PHYSTRACK_L2_ENTRIES;      \
      p3 = (mpn) % PHYSTRACK_L3_ENTRIES;   \
   } while (0)

/*
 * Convert L3 index to offset and bitmask.  offs/bitmask must be l-values.
 */
#define PHYSTRACK_GETL3POS(p3, offs, bitmask) \
   do {                          \
      offs = (p3) / 8;           \
      bitmask = 1 << ((p3) % 8); \
   } while (0)

/*
 * Helpers hiding middle level.
 */
#ifdef PHYSTRACK_3LEVEL
#define PHYSTRACK_GETL3(dir2, p2) (dir2)->dir[(p2)]
#define PHYSTRACK_ALLOCL3(dir2, p2) PhysTrackAllocL3((dir2), (p2))
#define PHYSTRACK_FREEL3(dir2, p2) PhysTrackFreeL3((dir2), (p2))
#else
#define PHYSTRACK_GETL3(dir2, p2) (dir2)
#define PHYSTRACK_ALLOCL3(dir2, p2) (dir2)
#define PHYSTRACK_FREEL3(dir2, p2) do { } while (0)
#endif


#ifdef PHYSTRACK_3LEVEL
/*
 *----------------------------------------------------------------------
 *
 * PhysTrackAllocL3 --
 *
 *      Allocate and hook L3 table to the L2 directory if does not exist.
 *      Or get existing one if it exists.
 *
 * Results:
 *      L3 table.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER PhysTrackerL3 *
PhysTrackAllocL3(PhysTrackerL2 *dir2,
                 unsigned int p2)
{
   PhysTrackerL3 *dir3;

   dir3 = dir2->dir[p2];
   if (!dir3) {
      ASSERT_ON_COMPILE(sizeof *dir3 == PAGE_SIZE);
      dir3 = HostIF_AllocPage();
      if (!dir3) {
         PANIC();
      }
      memset(dir3, 0, sizeof *dir3);
      dir2->dir[p2] = dir3;
   }
   return dir3;
}


/*
 *----------------------------------------------------------------------
 *
 * PhysTrackFreeL3 --
 *
 *      Unhook L3 table from L2 directory, and free it.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER void
PhysTrackFreeL3(PhysTrackerL2 *dir2,
                unsigned int p2)
{
   HostIF_FreePage(dir2->dir[p2]);
   dir2->dir[p2] = NULL;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * PhysTrack_Alloc --
 *
 *      Create new PhysTracker.
 *
 * Results:
 *      
 *      Creates new PhysTracker.
 *      
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

PhysTracker *
PhysTrack_Alloc(void)
{
   PhysTracker *tracker;

   /* allocate a new phystracker */
   tracker = HostIF_AllocKernelMem(sizeof *tracker, FALSE);
   if (tracker) {
      memset(tracker, 0, sizeof *tracker);
      tracker->numVMs = 1;
   } else {
      Warning("PhysTrack_Alloc failed\n");
   }
   return tracker;
} 


/*
 *----------------------------------------------------------------------
 *
 * PhysTrack_Init --
 *
 *      module initialization 
 *
 * Results:
 *      
 *      returns the PhysTracker. creates one if does not exist
 *      
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

PhysTracker *
PhysTrack_Init(void)
{
   /* allocate a new phystracker */
   if (physTracker == NULL) {
      physTracker = HostIF_AllocKernelMem(sizeof *physTracker, FALSE);
      if (physTracker) {
         memset(physTracker, 0, sizeof *physTracker);
      }
   }

   /* increment use count */
   if (physTracker) {
      physTracker->numVMs++;
   } else {
      Warning("PhysTrack_Init failed\n");
   }
   return physTracker;
} 


/*
 *----------------------------------------------------------------------
 *
 * PhysTrack_Cleanup --
 *
 *      module deallocation
 *
 * Results:
 *      
 *      reallocates all structures, including 'tracker'
 *      
 *      
 *
 * Side effects:
 *      tracker deallocated
 *
 *----------------------------------------------------------------------
 */

void
PhysTrack_Cleanup(PhysTracker *tracker) 
{ 
   ASSERT(tracker);

   /* decrement use count */
   tracker->numVMs--;

   /* deallocate phystracker if no more VMs */
   if (tracker->numVMs == 0) {
      unsigned int p1;

      for (p1 = 0; p1 < PHYSTRACK_L1_ENTRIES; p1++) {
         PhysTrackerL2 *dir2 = tracker->dir[p1];

         if (dir2) {
            unsigned int p2;

            for (p2 = 0; p2 < PHYSTRACK_L2_ENTRIES; p2++) {
               PhysTrackerL3 *dir3 = PHYSTRACK_GETL3(dir2, p2);

               if (dir3) {
                  unsigned int pos;

                  for (pos = 0; pos < BYTES_PER_ENTRY; pos++) {
                     if (dir3->bits[pos]) {
                        Warning("PhysTrack_Cleanup: pfns still locked\n");
                        PANIC();
                     }
                  }
                  PHYSTRACK_FREEL3(dir2, p2);
               }
            }
            HostIF_FreePage(dir2);
            tracker->dir[p1] = NULL;
         }
      }
      HostIF_FreeKernelMem(tracker);
      if (tracker == physTracker) {
         physTracker = NULL;
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * PhysTrack_Add --
 *
 *      add a page to the core map tracking.
 *
 * Results:
 *      
 *      void
 *
 * Side effects:
 *      Fatal if the page is already tracked.
 *
 *----------------------------------------------------------------------
 */

void
PhysTrack_Add(PhysTracker *tracker,
              MPN mpn)
{
   unsigned int p1;
   unsigned int p2;
   unsigned int p3;
   unsigned int pos;
   unsigned int bit;
   PhysTrackerL2 *dir2;
   PhysTrackerL3 *dir3;

   ASSERT(tracker);
   PHYSTRACK_MPN2IDX(mpn, p1, p2, p3);
   ASSERT(p1 < PHYSTRACK_L1_ENTRIES);

   dir2 = tracker->dir[p1];
   if (!dir2) { 
      // more efficient with page alloc
      ASSERT_ON_COMPILE(sizeof *dir2 == PAGE_SIZE);
      dir2 = HostIF_AllocPage();
      if (!dir2) { 
         PANIC();
      }
      memset(dir2, 0, sizeof *dir2);
      tracker->dir[p1] = dir2;
   }
   dir3 = PHYSTRACK_ALLOCL3(dir2, p2);
   PHYSTRACK_GETL3POS(p3, pos, bit);
   if (dir3->bits[pos] & bit) {
      PANIC();
   }
   dir3->bits[pos] |= bit;
}


/*
 *----------------------------------------------------------------------
 *
 * PhysTrack_Remove --
 *
 *      remove a page from the core map tracking
 *
 * Results:
 *      
 *      void
 *
 * Side effects:
 *      Fatal if the page is not tracked
 *
 *----------------------------------------------------------------------
 */

void
PhysTrack_Remove(PhysTracker *tracker,
                 MPN mpn)
{
   unsigned int p1;
   unsigned int p2;
   unsigned int p3;
   unsigned int pos;
   unsigned int bit;
   PhysTrackerL2 *dir2;
   PhysTrackerL3 *dir3;

   ASSERT(tracker);
   PHYSTRACK_MPN2IDX(mpn, p1, p2, p3);
   ASSERT(p1 < PHYSTRACK_L1_ENTRIES);

   dir2 = tracker->dir[p1];
   if (!dir2) {
      PANIC();
   }
   dir3 = PHYSTRACK_GETL3(dir2, p2);
   if (!dir3) {
      PANIC();
   }
   PHYSTRACK_GETL3POS(p3, pos, bit);
   if (!(dir3->bits[pos] & bit)) {
      PANIC();
   }
   dir3->bits[pos] &= ~bit;
}


/*
 *----------------------------------------------------------------------
 *
 * PhysTrack_Test --
 *
 *      tests whether a page is being tracked
 *
 * Results:
 *      
 *      TRUE if the page is tracked
 *      FALSE otherwise
 *      
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
PhysTrack_Test(const PhysTracker *tracker,
               MPN mpn)
{
   unsigned int p1;
   unsigned int p2;
   unsigned int p3;
   unsigned int pos;
   unsigned int bit;
   PhysTrackerL2 *dir2;
   PhysTrackerL3 *dir3;

   ASSERT(tracker);
   PHYSTRACK_MPN2IDX(mpn, p1, p2, p3);
   if (p1 >= PHYSTRACK_L1_ENTRIES) {
      return FALSE;
   }
   dir2 = tracker->dir[p1];
   if (!dir2) {
      return FALSE;
   }
   dir3 = PHYSTRACK_GETL3(dir2, p2);
   if (!dir3) {
      return FALSE;
   }
   PHYSTRACK_GETL3POS(p3, pos, bit);
   return (dir3->bits[pos] & bit) != 0;
}


/*
 *----------------------------------------------------------------------
 *
 * PhysTrack_GetNext --
 *
 *      Return next tracked page
 *
 * Results:
 *      
 *      MPN         when some tracked page was found
 *      INVALID_MPN otherwise
 *      
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

MPN
PhysTrack_GetNext(const PhysTracker *tracker,
                  MPN mpn)
{
   unsigned int p1;
   unsigned int p2;
   unsigned int p3;

   mpn++; /* We want the next MPN. */

   PHYSTRACK_MPN2IDX(mpn, p1, p2, p3);

   ASSERT(tracker);
   for (; p1 < PHYSTRACK_L1_ENTRIES; p1++) {
      PhysTrackerL2 *dir2;

      dir2 = tracker->dir[p1];
      if (dir2) {
         for (; p2 < PHYSTRACK_L2_ENTRIES; p2++) {
            PhysTrackerL3 *dir3;

            dir3 = PHYSTRACK_GETL3(dir2, p2);
            if (dir3) {
               for (; p3 < PHYSTRACK_L3_ENTRIES; p3++) {
                  unsigned int pos;
                  unsigned int bit;

                  PHYSTRACK_GETL3POS(p3, pos, bit);
                  if (dir3->bits[pos] & bit) {
                     return (p1 * PHYSTRACK_L2_ENTRIES + p2) * PHYSTRACK_L3_ENTRIES + p3;
                  }
               }
            }
            p3 = 0;
         }
      }
      p2 = 0; p3 = 0;
   }
   return INVALID_MPN;
}

