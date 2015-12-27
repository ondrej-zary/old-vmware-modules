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
 *  hostif.h - Platform dependent interface for supporting 
 *             the vmx86 device driver. 
 */


#ifndef _HOSTIF_H_
#define _HOSTIF_H_

#define INCLUDE_ALLOW_VMMON
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

#include "vmx86.h"
#include "vcpuset.h"

#if !defined _WIN32
#include "cpuid_info.h"
#endif

#include "hostifMem.h"
#include "hostifGlobalLock.h"

/*
 * Host-specific definitions. 
 */
#if !__linux__ && !defined(WINNT_DDK) && !defined __APPLE__
#error "Only Linux or NT or Mac OS defined for now."
#endif


#ifdef linux
#   define IRQ_HOST_INTR1_BASE 0x20
#   define IRQ_HOST_INTR2_BASE 0x28

/* See arch/i386/kernel/irq.h in the kernel source --hpreg */
#   define IRQ0_TRAP_VECTOR 0x51
#elif defined WINNT_DDK
#   define IRQ_HOST_INTR1_BASE 0x30
#   define IRQ_HOST_INTR2_BASE 0x38
#elif defined __APPLE__
#   define IRQ_HOST_INTR1_BASE 0x30 /* XXXMACOS */
#   define IRQ_HOST_INTR2_BASE 0x38 /* XXXMACOS */
#else
#   error "Free host interrupt vectors are unknown."
#endif

EXTERN int   HostIF_Init(VMDriver *vm);
EXTERN MPN   HostIF_LookupUserMPN(VMDriver *vm, VA64 uAddr);
#if defined(__linux__) && defined(VMX86_DEVEL) && defined(VM_X86_64)
EXTERN MPN   HostIF_LookupLargeMPN(void *addr);
#endif
EXTERN void *HostIF_MapCrossPage(VMDriver *vm, VA64 uAddr);
EXTERN void  HostIF_InitFP(VMDriver *vm);
#if defined __APPLE__
#define HostIF_InitEvent(_vm) do {} while (0)
#else
EXTERN void  HostIF_InitEvent(VMDriver *vm);
#endif

EXTERN void *HostIF_AllocPage(void);
EXTERN void  HostIF_FreePage(void *ptr);

/*
 * XXX These are _not_ part of the platform-specific interface that the
 *     cross-platform code uses. Consequently, they should become private to
 *     their respective drivers. --hpreg
 */
#if !defined __APPLE__
EXTERN int HostIF_CopyFromUser(void *dst, const void *src, unsigned int len);
EXTERN int HostIF_CopyToUser(void *dst, const void *src, unsigned int len);
EXTERN void  HostIF_InitGlobalLock(void);
#if defined _WIN32
EXTERN void *HostIF_AllocContigPages(VMDriver *vm, MPN *mpns, size_t numPages,
                                     unsigned int order, MPN maxMPN);
EXTERN int HostIF_FreeContigPages(VMDriver *vm, VA64 uAddr);
EXTERN Bool HostIF_InitHostIF(void);
EXTERN void HostIF_CleanupHostIF(void);
EXTERN void HostIF_InitFastClock(void);
EXTERN void HostIF_StartFastClockThread(void);
EXTERN void HostIF_StopFastClockThread(void);
EXTERN void HostIF_SetUserCallHandle(VMDriver *vm, int h);
EXTERN int HostIF_SyncReadTSCs(TSCSet *tscSet, uintptr_t cpuSet);
EXTERN int HostIF_SyncWriteTSCs(TSCSet *tscSet, uintptr_t cpuSet);
EXTERN void HostIF_SynchronizeTSCs(void);
EXTERN void HostIF_SetMemoryParams(const VA64* params, int count);
EXTERN int HostIF_RememberkHzEstimate(uint32 currentEstimate, uint32* result);
#endif
#if __linux__
Bool HostIF_GetAllCpuInfo(CPUIDQuery *query);
EXTERN uint32 HostIF_BrokenCPUHelper(void);
EXTERN int HostIF_MarkLockedVARangeClean(const VMDriver *vm, VA uvAddr, 
                                         unsigned len, VA bv);
EXTERN void HostIF_PollListLock(int callerID);
EXTERN void HostIF_PollListUnlock(int callerID);
struct page;
EXTERN void *HostIF_MapUserMem(VA addr, size_t size, struct page **page);
EXTERN void HostIF_UnmapUserMem(struct page **page);
#endif
#endif

EXTERN MPN   HostIF_LockPage(VMDriver *vm, VA64 uAddr, Bool allowMultipleMPNsPerVA);
EXTERN int   HostIF_UnlockPage(VMDriver *vm, VA64 uAddr);
EXTERN int   HostIF_UnlockPageByMPN(VMDriver *vm, MPN mpn, VA64 uAddr);
EXTERN Bool  HostIF_IsLockedByMPN(VMDriver *vm, MPN mpn);
EXTERN void  HostIF_FreeAllResources(VMDriver *vm);
#if __linux__
void HostIF_InitUptime(void);
void HostIF_CleanupUptime(void);
#endif
EXTERN uint64 HostIF_ReadUptime(void);
EXTERN uint64 HostIF_UptimeFrequency(void);
EXTERN unsigned int HostIF_EstimateLockedPageLimit(const VMDriver *vm, 
 						   unsigned int lockedPages);
EXTERN void  HostIF_Wait(unsigned int timeoutMs);
EXTERN void  HostIF_WaitForFreePages(unsigned int timeoutMs);
EXTERN Bool  HostIF_IsAnonPage(VMDriver *vm, MPN mpn);
EXTERN Bool  HostIF_GetNUMAAnonPageDistribution(VMDriver *vm, int numNodes, 
                                                uint32 *perNodeCnt);
EXTERN void *HostIF_AllocCrossGDT(uint32 numPages, MPN maxValidFirst,
                                  MPN *crossGDTMPNs);
EXTERN void  HostIF_FreeCrossGDT(uint32 numPages, void *crossGDT);
EXTERN void  HostIF_VMLock(VMDriver *vm, int callerID);
EXTERN void  HostIF_VMUnlock(VMDriver *vm, int callerID);
#ifdef VMX86_DEBUG
Bool HostIF_VMLockIsHeld(VMDriver *vm);
#endif

EXTERN Bool  HostIF_APICInit(VMDriver *vm, Bool setVMPtr, Bool probe);
EXTERN uint8 HostIF_APIC_ID(void);

EXTERN int   HostIF_SemaphoreWait(VMDriver *vm,
                                  Vcpuid vcpuid,
                                  uint32 *args);

EXTERN int   HostIF_SemaphoreSignal(uint32 *args);

EXTERN void  HostIF_SemaphoreForceWakeup(VMDriver *vm, Vcpuid vcpuid);
EXTERN Bool  HostIF_IPI(VMDriver *vm, VCPUSet vcs, Bool all, Bool *didBroadcast);

EXTERN void  HostIF_UserCall(VMDriver *vm, Vcpuid vcpuid);
EXTERN Bool  HostIF_UserCallWait(VMDriver *vm, Vcpuid vcpuid, int timeoutms);
EXTERN void  HostIF_AwakenVcpu(VMDriver *vm, Vcpuid vcpuid);

#if defined __APPLE__
/*
 * On Mac OS, MonitorLoopCrossUserCallPoll() unconditionnally does this for all
 * cross usercalls with VMMon_LowerCrossUserCallEvent() entirely in user mode.
 */
#define HostIF_AckUserCall(_vm, _vcpuid) do {} while (0)
#else
EXTERN void  HostIF_AckUserCall(VMDriver *vm, Vcpuid vcpuid);
#endif

EXTERN uint32 HostIF_GetCurrentPCPU(void);
EXTERN void HostIF_CallOnEachCPU(void (*func)(void *), void *data);
#if defined __APPLE__
/*
 * It is a bad idea to implement HostIF_NumOnlineLogicalCPUs() on Mac OS
 * because that value can change at any time (the user can use a BeOS-style GUI
 * to enable/disable CPUs).
 */
#else
EXTERN unsigned int HostIF_NumOnlineLogicalCPUs(void);
#endif

EXTERN void HostIF_YieldCPU(uint32 usecs);

EXTERN int
HostIF_AllocLockedPages(VMDriver *vm, VA64 addr,
			unsigned int numPages, Bool kernelMPNBuffer);
EXTERN int
HostIF_FreeLockedPages(VMDriver *vm, VA64 addr,
		       unsigned int numPages, Bool kernelMPNBuffer);
EXTERN int 
HostIF_GetLockedPageList(VMDriver *vm, VA64 uAddr, unsigned int numPages);

EXTERN int HostIF_ReadPage(MPN mpn, VA64 addr, Bool kernelBuffer);
EXTERN int HostIF_WritePage(MPN mpn, VA64 addr, Bool kernelBuffer);
#if defined __APPLE__
// There is no need for a fast clock lock on Mac OS.
#define HostIF_FastClockLock(_callerID) do {} while (0)
#define HostIF_FastClockUnlock(_callerID) do {} while (0)
#else
EXTERN void HostIF_FastClockLock(int callerID);
EXTERN void HostIF_FastClockUnlock(int callerID);
#endif
EXTERN int HostIF_SetFastClockRate(unsigned rate);

EXTERN MPN HostIF_AllocMachinePage(void);
EXTERN void HostIF_FreeMachinePage(MPN mpn);

EXTERN int HostIF_SafeRDMSR(uint32 msr, uint64 *val);

#endif // ifdef _HOSTIF_H_
