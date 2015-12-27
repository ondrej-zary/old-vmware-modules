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
 * numa_defs.h --
 *	This is the internal header file for the NUMA module.
 */


#ifndef _NUMA_DEFS_H
#define _NUMA_DEFS_H

#define INCLUDE_ALLOW_VMX
#define INCLUDE_ALLOW_VMCORE
#define INCLUDE_ALLOW_VMMON
#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_MODULE
#include "includeCheck.h"

#include "vm_basic_types.h"

/*
 * Constants
 */

#define NUMA_MAX_NODES_SHIFT        (3)
#define NUMA_MAX_NODES              (1 << NUMA_MAX_NODES_SHIFT)
#define NUMA_MAX_MEM_RANGES         (8)
#define NUMA_MAX_TOTAL_MEM_RANGES   (NUMA_MAX_NODES * NUMA_MAX_MEM_RANGES)
#define INVALID_NUMANODE            ((NUMA_Node) -1)
#define NUMA_MAX_CPUS_PER_NODE      32
#define MAX_LAPIC_ID                256

typedef uint32   NUMA_Node;

/*
 * Structures
 */
typedef struct {
   MPN          startMPN;
   MPN          endMPN;
   NUMA_Node    id;
} NUMA_MemRange;

typedef struct {
   uint32        numPCPUs;
   uint32        numMemRanges;
   NUMA_Node     id;
   uint32        apicIDs[NUMA_MAX_CPUS_PER_NODE];
   NUMA_MemRange memRange[NUMA_MAX_MEM_RANGES];
} NUMA_NodeInfo;

#endif // _NUMA_DEFS_H
