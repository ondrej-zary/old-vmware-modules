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
 * vm_time.h  --
 *
 *    Time management functions.
 *    Part of driver-only distribution
 *
 *    see comment in poll.c
 */


#ifndef VM_TIME_H
#define VM_TIME_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_VMMON
#define INCLUDE_ALLOW_VMCORE
#define INCLUDE_ALLOW_VMKERNEL
#include "includeCheck.h"


/* TS stands for "timestamp", which is in units of "cycles" */
/* Use these types to express time, RDTSC should return a
   VmAbsoluteTS -- converting to US results in a VmAbsoluteUS.
   Subtracting two VmAbsoluteTS's results in a VmRelativeTS, likewise
   for VmAbsoluteUS and VmRelativeUS */
/* I use these instead of VmTimeRealClock and VmTimeVirtualClock
   because those types are not used consistently in our code (cycles vs
   us) */
typedef uint64 VmAbsoluteTS; // a particular point in time (in cycles)
typedef int64  VmRelativeTS; // a signed delta in cycles
typedef uint64 VmIntervalTS; // an unsigned delta in cycles
typedef uint64 VmAbsoluteUS; // a particular point in time (in us)
typedef int64  VmRelativeUS; // a signed delta in us
typedef uint64 VmIntervalUS; // an unsigned delta in us

/*
 * Compare two VmAbsoluteTS's using comparison operator op, allowing
 * for wrap.  The assumption is that differences should not be more
 * than 2**63, so a larger difference is taken as negative.
 */
#define COMPARE_TS(ts1, op, ts2) (((int64) ((ts1) - (ts2))) op 0)

#define MAX_ABSOLUTE_TS \
   ((VmAbsoluteTS) CONST64U(0xffffffffffffffff))

/*
 * Largest possible unambiguous difference between two VmAbsoluteTS's
 * according to COMPARE_TS's method of comparison.
 */
#define MAX_RELATIVE_TS \
   ((VmRelativeTS) CONST64(0x7fffffffffffffff))

#define MAX_ABSOLUTE_US \
   ((VmAbsoluteUS) CONST64U(0xffffffffffffffff))


struct VmTimeVirtualRealClock;
typedef struct VmTimeVirtualRealClock VmTimeVirtualRealClock;

#define VMTIME_VIRTUAL_INFINITE \
   ((VmTimeVirtualClock) CONST64(0x3fffffffffffffff))

#define CYCLES_TO_USECS(_c) \
   (((_c) * (uint64)1000) / MISCSHARED->khzEstimate)

#define USECS_TO_CYCLES(_us) \
   ((((uint64)(_us)) * MISCSHARED->khzEstimate) / 1000)

#define MSECS_TO_CYCLES(_ms) \
   (((uint64)(_ms)) * MISCSHARED->khzEstimate)

#define HZ_ESTIMATE (MISCSHARED->hzEstimate)

#ifdef USERLEVEL

extern VmTimeType VmTime_ReadVirtualTime(void);
extern VmTimeVirtualRealClock *VmTime_NewVirtualRealClock(void);
extern void VmTime_StartVirtualRealClock(VmTimeVirtualRealClock *, double);
extern void VmTime_ResetVirtualRealClock(VmTimeVirtualRealClock *);
extern VmTimeType VmTime_ReadVirtualRealTime(VmTimeVirtualRealClock *);
extern VmTimeType VmTime_RemainingVirtualRealTime(VmTimeVirtualRealClock *,
						  VmTimeType realTime);
extern void VmTime_UpdateVirtualRealTime(VmTimeVirtualRealClock *clock,
                                         VmTimeType realTime,
                                         VmTimeType virtualTime);
#endif
#endif /* VM_TIME_H */

