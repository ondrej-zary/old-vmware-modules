/*********************************************************
 * Copyright (C) 2001 VMware, Inc. All rights reserved.
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
 * overheadmem_types.h
 *
 *	Types for tracking memory overheads.
 */

#ifndef _OVERHEADMEM_TYPES_H
#define _OVERHEADMEM_TYPES_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_VMMON
#define INCLUDE_ALLOW_VMCORE
#define INCLUDE_ALLOW_MODULE
#include "includeCheck.h"

#include "vm_basic_types.h"
#include "vm_atomic.h"


/*
 * There are 4 types of memory we lock on the host.  Memory can be Mem_Mapped in
 * the vmx, anonymous memory for use by monitor is not mapped permanently in any
 * address space, guest memory regions other than main memory (can be
 * locked/unlocked on hosted but not on ESX), and main memory which can be 
 * locked/unlocked in hosted and esx.
 *
 * In addition, the vmx may malloc memory or declare (large) static structures.
 * Neither of these is locked on hosted platforms and the hostOS may swap it.
 * Therefore, on hosted platforms we do not track this memory and instead
 * include a working set component (sched.mem.hosted.perVMOverheadMBs).
 * On ESX, this memory must be accounted for so we account them to user
 * (nonpaged) overhead.  At present, the accounting is extremely coarse
 * and only aggregate sizes are hard-coded (see PR363997).
 */
typedef enum OvhdMemType {
   OvhdMem_memmap,
   OvhdMem_anon,
   OvhdMem_guest,
   OvhdMem_mainmem,
   OvhdMem_malloc,
   OvhdMem_static,
   NumOvhdMemTypes
} OvhdMemType;

#define OvhdMemMask(type)    (1 << type)

#define OVHDMEM_NONE      0x0
#define OVHDMEM_MEMMAP    0x1
#define OVHDMEM_ANON      0x2
#define OVHDMEM_GUEST     0x4
#define OVHDMEM_MAINMEM   0x8
#define OVHDMEM_MALLOC   0x10
#define OVHDMEM_STATIC   0x20
#define OVHDMEM_ALL      (OVHDMEM_MEMMAP | OVHDMEM_ANON |   \
                          OVHDMEM_GUEST | OVHDMEM_MAINMEM | \
                          OVHDMEM_MALLOC | OVHDMEM_STATIC)

/* ... and four categories of memory sources. */
typedef enum OvhdMemCategory {
   OvhdMemCat_paged,
   OvhdMemCat_nonpaged,
   OvhdMemCat_anonymous,
   OvhdMemCat_excluded,
   NumOvhdMemCategories
} OvhdMemCategory;

#ifdef VMX86_SERVER
/*
 * OVHDMEM_MAINMEM is left out of OVHD_PAGED because 
 * it is not accounted towards the number of user overhead
 * pages in ESX. 
 */
#define OVHDMEM_PAGED     (OVHDMEM_NONE)
#define OVHDMEM_NONPAGED  (OVHDMEM_GUEST | OVHDMEM_MEMMAP |          \
                           OVHDMEM_MALLOC | OVHDMEM_STATIC)
#define OVHDMEM_ANONYMOUS (OVHDMEM_ANON)
#define OVHDMEM_EXCLUDED  (OVHDMEM_MAINMEM)
#else 
/*
 * Hosted platforms lump the anonymous pages in with the
 * non-paged overhead.  Malloc'd and static sources are
 * already accounted as part of per-VM overhead.
 */
#define OVHDMEM_PAGED     (OVHDMEM_MAINMEM | OVHDMEM_GUEST)
#define OVHDMEM_NONPAGED  (OVHDMEM_ANON | OVHDMEM_MEMMAP)
#define OVHDMEM_ANONYMOUS (OVHDMEM_NONE)
#define OVHDMEM_EXCLUDED  (OVHDMEM_MALLOC | OVHDMEM_STATIC)
#endif

#if ((OVHDMEM_PAGED & OVHDMEM_NONPAGED) != 0)     ||                 \
    ((OVHDMEM_NONPAGED & OVHDMEM_ANONYMOUS) != 0) ||                 \
    ((OVHDMEM_PAGED & OVHDMEM_ANONYMOUS) != 0)    ||                 \
    ((OVHDMEM_PAGED | OVHDMEM_NONPAGED | OVHDMEM_ANONYMOUS) !=       \
              (OVHDMEM_ALL & ~OVHDMEM_EXCLUDED))
#error Overheadmem categories do not form a partition of the overheads
#endif

/* Categories of overhead for 32-bit and 64-bit mode. */
typedef struct OvhdMem_Overheads {
   uint32 paged;
   uint32 nonpaged;
   uint32 anonymous;
} OvhdMem_Overheads;

typedef struct OvhdMem_Deltas {
   int32 paged;
   int32 nonpaged;
   int32 anonymous;
} OvhdMem_Deltas;


/* Types for tracking vmx (user) overheads. */

#define OVHDMEM_MAX_NAME_LEN 24

typedef struct OvhdMemNode {
   char name[OVHDMEM_MAX_NAME_LEN];   // name of overhead source
   unsigned reserved;                 // max. allocatable pages for source
   unsigned used;                     // # of allocated pages for source
   OvhdMemType type;                  // how/where memory for source is managed
} OvhdMemNode;

/* Types for tracking vmm (anonymous) overheads. */

typedef enum {
   OvhdMem_PeerVmm32, 
   OvhdMem_PeerVmm64, 
   OvhdMem_PeerShared,
   NumOvhdMemPeerTypes
} OvhdMemPeerType;

typedef struct OvhdMemAnonPeerInfo {
   Atomic_uint32 reserved;
   Atomic_uint32 used;
} OvhdMemAnonPeerInfo;

typedef struct OvhdMemAnonVCPUInfo {
   int32 used;
} OvhdMemAnonVCPUInfo;

/*
 * For anonymous memory, we track information about reservations and usage
 * for each memory source for each peer (vmm32, vmm64, peerShared). We also
 * track usage counts for each vcpu.
 */
typedef struct OvhdMemAnon {
   char name[OVHDMEM_MAX_NAME_LEN];
   OvhdMemAnonPeerInfo peerInfo[NumOvhdMemPeerTypes];
   OvhdMemAnonVCPUInfo vcpuInfo[MAX_VCPUS][NumOvhdMemPeerTypes];
} OvhdMemAnon;

/*
 * An overheadmem configuration describes both the (primary) parameters
 * contributing to the overhead limits but those limits as well.
 */

typedef struct OvhdMemConfig {
   Bool     isConservative;

   Bool     vmx86Debug;
   Bool     vmx86Devel;
   Bool     vmx86Log;
   Bool     vmx86Stats;

   Bool     usesHV;
   Bool     usesNestedPaging;
   Bool     isIntel;
   Bool     supports64bit;
   Bool     supportsReplay;
   Bool     supportsReplayChecking;

   unsigned busMemFrameShift;
   
   unsigned numVcpus;
   uint32   memSize;
   uint32   svgaFBSize;
   uint32   svgaMemSize;
   uint32   pciPassthruSize;
   uint32   numPvscsiAdapters;
   uint32   numLsiAdapters;

   OvhdMemNode *ovhdmem;
   OvhdMemAnon *ovhdmemAnon;
} OvhdMemConfig;

#endif
