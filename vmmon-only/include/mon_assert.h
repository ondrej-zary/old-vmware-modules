/*********************************************************
 * Copyright (C) 2007 VMware, Inc. All rights reserved.
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

#ifndef _MON_ASSERT_H_
#define _MON_ASSERT_H_

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_VMMON
#define INCLUDE_ALLOW_VMCORE
#define INCLUDE_ALLOW_VMIROM
#include "includeCheck.h"

#include "vm_assert.h"

/*
 * Monitor source location
 *
 * To give the densest expansion of ASSERT() (and friends),
 * the monitor encodes source location in 32 bits.  This is
 * made possible by source rewriting (see make/mk/vmm-strings.pl).
 * The script converts the construct @__FILE__@ in the source
 * (see below) into a small constant, which can be converted
 * back to the file name with ASSERT_MONSRCFILE().
 *
 * -- edward
 */


typedef uint32 Assert_MonSrcLoc;

#define ASSERT_MONSRCFILEOFFSET(loc)    LOWORD(loc)
#define ASSERT_MONSRCLINE(loc)          HIWORD(loc)

#define ASSERT_NULL_MONSRCLOC     0             // there is never line 0
#define ASSERT_ILLEGAL_MONSRCLOC  0xffffffff    // and never 4 billion files

#ifdef VMM // {
#ifdef MONITOR_APP // {

#define ASSERT_MONSRCLOC() ASSERT_NULL_MONSRCLOC

#else // } {

#define ASSERT_MONSRCLOC() \
   ((uint32) (uintptr_t) (@__FILE__@ + (__LINE__ << 16)))

#define ASSERT_MONSRCFILE(loc) \
   (AssertMonSrcFileBase + ASSERT_MONSRCFILEOFFSET(loc))

extern const char AssertMonSrcFileBase[];

#define _ASSERT_PANIC(name)          ((name)(ASSERT_MONSRCLOC()))
#define _ASSERT_PANIC_BUG(bug, name) ((name##Bug)(ASSERT_MONSRCLOC(), bug))

EXTERN NORETURN void AssertPanic(Assert_MonSrcLoc loc);
EXTERN NORETURN void AssertAssert(Assert_MonSrcLoc loc);
EXTERN NORETURN void AssertNotImplemented(Assert_MonSrcLoc loc);
EXTERN NORETURN void AssertNotReached(Assert_MonSrcLoc loc);
EXTERN NORETURN void AssertPanicBug(Assert_MonSrcLoc loc, int bug);
EXTERN NORETURN void AssertAssertBug(Assert_MonSrcLoc loc, int bug);
EXTERN NORETURN void AssertNotImplementedBug(Assert_MonSrcLoc loc, int bug);
EXTERN NORETURN void AssertNotReachedBug(Assert_MonSrcLoc loc, int bug);

extern const char AssertLengthFmt[];
extern const char AssertUnexpectedFmt[];
extern const char AssertNotTestedFmt[];

#endif // }
#endif // }

#endif
