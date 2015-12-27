/*********************************************************
 * Copyright (C) 2009 VMware, Inc. All rights reserved.
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
 * pageUtil.h --
 *
 *	Utilities on page contents.
 */

#ifndef _PAGE_UTIL_H
#define _PAGE_UTIL_H

#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMMON
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

#include "vm_basic_types.h"


#define DEFINE_PAGE_CLASS \
   MDEF(PC_ZERO,         "Z"      ) \
   MDEF(PC_WORD5_32,     "W5-32"  ) \
   MDEF(PC_WORD5_64,     "W5-64"  ) \
   MDEF(PC_LAST2,        "L2"     ) \
   MDEF(PC_PERIOD1,      "P1"     ) \
   MDEF(PC_PERIOD2,      "P2"     ) \
   MDEF(PC_PERIOD4_1_2,  "P4-12"  ) \
   MDEF(PC_UNKNOWN,      "U"      )

typedef enum PageClass {
#define MDEF(_class, _name) _class,
   DEFINE_PAGE_CLASS
#undef MDEF
   NUM_PAGE_CLASSES
} PageClass;

typedef uint8 PageClassID;




/*
 *-----------------------------------------------------------------------------
 *
 * PageUtil_IsZeroPage --
 *
 *     Checks if the contents of a page is zero.
 *
 * Results:
 *     Returns TRUE for a zero page, returns FALSE otherwise.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE Bool 
PageUtil_IsZeroPage(const void *data) 
{
   uintptr_t *p = (uintptr_t *)data;
   unsigned i, n = PAGE_SIZE / sizeof(uintptr_t);
   for (i = 0; i < n; i++) {
      if (p[i] != 0) {
         return FALSE;
      }
   }
   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * PageUtil_ArePagesEqual --
 *
 *     Compare the contents of two pages of memory.
 *
 * Results:
 *     TRUE iff the pages are equal.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE Bool
PageUtil_ArePagesEqual(const void *data1, const void *data2)
{
   uintptr_t *p1 = (uintptr_t *)data1, *p2 = (uintptr_t *)data2;
   unsigned i, n = PAGE_SIZE / sizeof(uintptr_t);
   for (i = 0; i < n; i++) {
      if (p1[i] != p2[i]) {
         return FALSE;
      }
   }
   return TRUE;
}
#endif
