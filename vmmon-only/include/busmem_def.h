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
 * busmem_def.h -- 
 *
 *      Definitions of constants/structs used in communicating 
 *	page info. between VMKernel/VMMon and VMM.
 */

#ifndef	_BUSMEM_DEF_H
#define	_BUSMEM_DEF_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMMON
#define INCLUDE_ALLOW_VMNIXMOD
#define INCLUDE_ALLOW_VMCORE
#define INCLUDE_ALLOW_MODULE
#include "includeCheck.h"

#include "vm_assert.h"

/*
 * constants
 */

#define BUSMEM_PAGELIST_MAX                                         \
           (PAGE_SIZE / (sizeof(BPN) + sizeof(MPN) + sizeof(Bool)))

/*
 * types
 */

/*
 * BusMem_PageList is used for communicating sets of BPNs/MPNs
 * between the monitor and platform.  The most common use is for passing
 * sets of pages that are intended to be shared, swapped, or ballooned.
 * The "stateList" passes extra info about each corresponding page.
 *
 * The page-list structure is sized so that it fits in a 4KB page. Three 
 * arrays are used to better pack the information into the page.
 */
typedef struct BusMem_PageList {
   BPN  bpnList[BUSMEM_PAGELIST_MAX];
   MPN  mpnList[BUSMEM_PAGELIST_MAX];
   Bool stateList[BUSMEM_PAGELIST_MAX];
} BusMem_PageList;

MY_ASSERTS(BUSMEMDEFS, 
           ASSERT_ON_COMPILE(sizeof(BusMem_PageList) <= PAGE_SIZE);)

#endif
