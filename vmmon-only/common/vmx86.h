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
 *  vmx86.h - Platform independent data and interface for supporting 
 *            the vmx86 device driver. 
 */

#ifndef VMX86_H
#define VMX86_H

#define INCLUDE_ALLOW_VMMON
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

#include "x86apic.h"
#include "x86msr.h"
#include "modulecall.h"
#include "vcpuid.h"
#include "initblock.h"
#include "iocontrols.h"
#include "numa_defs.h"
#include "rateconv.h"

#define INVALID_HOST_CPU ((uint32)(-1))

/*
 * VMDriver - the main data structure for the driver side of a
 *            virtual machine.
 */

typedef struct VMDriver {
   /* Unique (in the driver), strictly positive, VM ID used by userland. */
   int                 userID;

   struct VMDriver    *nextDriver;   /* Next on list of all VMDrivers */

   Vcpuid              numVCPUs;     /* Number of vcpus in VM. */
   struct VMHost      *vmhost;       /* Host-specific fields. */

   /* Pointers to the crossover pages shared with the monitor. */
   struct VMCrossPage *crosspage[MAX_INITBLOCK_CPUS];
   volatile uint32    currentHostCpu[MAX_INITBLOCK_CPUS];
   volatile uint32   (*hostAPIC)[4]; /* kseg pointer to host APIC */

   struct MemTrack    *memtracker;   /* Memory tracker pointer */
   Bool                checkFuncFailed;
   struct PerfCounter *perfCounter;
   VMMemMgmtInfo       memInfo;
   unsigned            fastClockRate;/* Modified while holding fastClock lock only */
   int                 fastSuspResFlag;
} VMDriver;

typedef struct VmTimeStart {
   uint64 count;
   uint64 time;
} VmTimeStart;

typedef struct PseudoTSC {
   RateConv_Params refClockToTSC;
   uint64          hz;
   volatile Bool   useRefClock;
   Bool            neverSwitchToRefClock;
   volatile Bool   initialized;
} PseudoTSC;

extern PseudoTSC pseudoTSC;

#define MAX_LOCKED_PAGES (-1)

extern VMDriver *Vmx86_CreateVM(void);
extern int Vmx86_ReleaseVM(VMDriver *vm);
extern int Vmx86_InitVM(VMDriver *vm, InitBlock *initParams);
extern Bool Vmx86_InitNUMAInfo(NUMAInfoArgs *initParams);
extern void Vmx86_DestroyNUMAInfo(void);
extern Bool Vmx86_GetNUMAMemStats(VMDriver *curVM,
				 VMNUMAMemStatsArgs *outArgs);
extern NUMA_Node Vmx86_MPNToNodeNum(MPN mpn);
extern int Vmx86_LateInitVM(VMDriver *vm);
extern int Vmx86_RunVM(VMDriver *vm, Vcpuid vcpuid);
extern void Vmx86_ReadTSCAndUptime(VmTimeStart *st);
extern uint32 Vmx86_GetkHzEstimate(VmTimeStart *st);
extern int Vmx86_SetHostClockRate(VMDriver *vm, int rate);
extern MPN Vmx86_LockPage(VMDriver *vm, VA64 uAddr, Bool allowMultipleMPNsPerVA);
extern int Vmx86_UnlockPage(VMDriver *vm, VA64 uAddr);
extern int Vmx86_UnlockPageByMPN(VMDriver *vm, MPN mpn, VA64 uAddr);
extern MPN Vmx86_GetRecycledPage(VMDriver *vm);
extern int Vmx86_ReleaseAnonPage(VMDriver *vm, MPN mpn);
extern int Vmx86_AllocLockedPages(VMDriver *vm, VA64 addr,
				  unsigned numPages, Bool kernelMPNBuffer);
extern int Vmx86_FreeLockedPages(VMDriver *vm, VA64 addr,
				 unsigned numPages, Bool kernelMPNBuffer);
extern int Vmx86_GetLockedPageList(VMDriver *vm, VA64 uAddr,
				   unsigned int numPages);
extern Bool Vmx86_IsAnonPage(VMDriver *vm, const MPN32 mpn);

extern int32 Vmx86_GetNumVMs(void);
extern int32 Vmx86_GetTotalMemUsage(void);
extern Bool Vmx86_SetConfiguredLockedPagesLimit(unsigned limit);
extern void Vmx86_SetDynamicLockedPagesLimit(unsigned limit);
extern Bool Vmx86_GetMemInfo(VMDriver *curVM,
                             Bool curVMOnly,
                             VMMemInfoArgs *args,
                             int outArgsLength);
extern Bool Vmx86_GetMemInfoCopy(VMDriver *curVM, VMMemInfoArgs *buf);
extern void Vmx86_Admit(VMDriver *curVM, VMMemInfoArgs *args);
extern Bool Vmx86_Readmit(VMDriver *curVM, OvhdMem_Deltas *delta);
extern void Vmx86_UpdateMemInfo(VMDriver *curVM,
                                const VMMemMgmtInfoPatch *patch);
extern void Vmx86_Add2MonPageTable(VMDriver *vm, VPN vpn, MPN mpn,
				   Bool readOnly);
extern Bool Vmx86_PAEEnabled(void);
extern Bool Vmx86_VMXEnabled(void);
extern Bool Vmx86_HVEnabledCPUs(void);
extern void Vmx86_FixHVEnable(Bool force);
extern Bool Vmx86_VTSupportedCPU(void);
extern Bool Vmx86_GetAllMSRs(MSRQuery *query);
extern Bool Vmx86_BrokenCPUHelper(void);
extern void Vmx86_CompleteUserCall(VMDriver *vm, Vcpuid vcpuid);
extern void Vmx86_MonitorPollIPI(void);
extern void Vmx86_InitIDList(void);
extern VMDriver *Vmx86_LookupVMByUserID(int userID);
extern Bool Vmx86_FastSuspResSetOtherFlag(VMDriver *vm, int otherVmUserId);
extern int  Vmx86_FastSuspResGetMyFlag(VMDriver *vm, Bool blockWait);
extern Bool Vmx86_InCompatMode(void);
extern Bool Vmx86_InLongMode(void);
extern void Vmx86_Open(void);
extern void Vmx86_Close(void);

static INLINE Bool
Vmx86_PseudoTSCUsesRefClock(void)
{
   return pseudoTSC.useRefClock;
}

static INLINE Bool
Vmx86_SetPseudoTSCUseRefClock(void)
{
   if (!pseudoTSC.useRefClock && !pseudoTSC.neverSwitchToRefClock) {
      pseudoTSC.useRefClock = TRUE;
      return TRUE;
   }
   return FALSE;
}

static INLINE uint64
Vmx86_GetPseudoTSCHz(void)
{
   return pseudoTSC.hz;
}

extern void Vmx86_InitPseudoTSC(Bool forceRefClock, Bool forceTSC, 
                                RateConv_Params *refClockToTSC,
                                uint64 *tscHz);
extern Bool Vmx86_CheckPseudoTSC(uint64 *lastTSC, uint64 *lastRC);
extern uint64 Vmx86_GetPseudoTSC(void);


#endif 
