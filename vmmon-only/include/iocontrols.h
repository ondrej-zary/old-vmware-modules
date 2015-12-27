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
 * iocontrols.h
 *
 *        The driver io controls.
 */

#ifndef _IOCONTROLS_H_
#define _IOCONTROLS_H_

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_VMMON
#define INCLUDE_ALLOW_VMCORE
#define INCLUDE_ALLOW_MODULE
#include "includeCheck.h"

#include "pshare_ext.h"
#include "basic_initblock.h"
#include "x86segdescrs.h"
#include "rateconv.h"
#include "overheadmem_types.h"

#ifndef VMX86_SERVER
#include "numa_defs.h"
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * VA64ToPtr --
 *
 *      Convert a VA64 to a pointer.
 *
 *      Usage of this function is strictly limited to these 2 cases:
 *
 *      1) In a VMX function which does an ioctl to vmmon, and receives a VMX
 *         pointer as a result.
 *
 *      2) In the vmmon code, for the functions which have a VA64 and need
 *         to call kernel APIs which take pointers.
 *
 * Results:
 *      Virtual address.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void *
VA64ToPtr(VA64 va64) // IN
{
#ifdef VM_X86_64
   ASSERT_ON_COMPILE(sizeof (void *) == 8);
#else
   ASSERT_ON_COMPILE(sizeof (void *) == 4);
   // Check that nothing of value will be lost.
   ASSERT(!(va64 >> 32));
#endif
   return (void *)(uintptr_t)va64;
}


/*
 *-----------------------------------------------------------------------------
 *
 * PtrToVA64 --
 *
 *      Convert a pointer to a VA64.
 *
 *      Usage of this function is strictly limited to these 2 cases:
 *
 *      1) In a VMX function which does an ioctl to vmmon, and passes in a VMX
 *         pointer.
 *
 *      2) In the vmmon code, for the functions which need to pass in a kernel
 *         pointer to functions which can take either a user or a kernel
 *         pointer in the same parameter.
 *
 * Results:
 *      Virtual address.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static INLINE VA64
PtrToVA64(void const *ptr) // IN
{
   ASSERT_ON_COMPILE(sizeof ptr <= sizeof (VA64));
   return (VA64)(uintptr_t)ptr;
}


/*
 * Driver version.
 *
 * Increment major version when you make an incompatible change.
 * Compatibility goes both ways (old driver with new executable
 * as well as new driver with old executable).
 *
 * Note: Vmcore compatibility is different from driver versioning.
 * For vmcore puposes, the bora tree is conceptually split in two:
 * vmcore, and rest-of-bora. The vmmon driver is largely outside
 * vmcore and vmcore imports functionality from vmmon. Addition,
 * deletion or modification of an iocontrol used only by rest-of-bora
 * does not break vmcore compatibility. 
 *
 * See bora/doc/vmcore details.
 *
 */

#define VMMON_VERSION           (238 << 16 | 0)
#define VMMON_VERSION_MAJOR(v)  ((uint32) (v) >> 16)
#define VMMON_VERSION_MINOR(v)  ((uint16) (v))


/*
 * ENOMEM returned after MAX_VMS virtual machines created
 */

#ifdef VMX86_SERVER
#define MAX_VMS	128
#else
#define MAX_VMS 64
#endif
/* 
 * MsgWaitForMultipleObjects doesn't scale well enough on Win32. 
 * Allocate with MAX_VMS so static buffers are large, but do
 * admissions control with this value on Win32 until we check
 * scalability (probably in authd).
 */
#ifdef _WIN32
#define MAX_VMS_WIN32 64
#endif


#if !__linux__
/*
 * On platforms other than Linux, IOCTLCMD_foo values are just numbers, and
 * we build the IOCTL_VMX86_foo values around these using platform-specific
 * format for encoding arguments and sizes.
 */
#  define IOCTLCMD(_cmd) IOCTLCMD_ ## _cmd
#else // if __linux__
/*
 * Linux defines _IO* macros, but the core kernel code ignore the encoded
 * ioctl value. It is up to individual drivers to decode the value (for
 * example to look at the size of a structure to determine which version
 * of a specific command should be used) or not (which is what we
 * currently do, so right now the ioctl value for a given command is the
 * command itself).
 *
 * Hence, we just define the IOCTL_VMX86_foo values directly, with no
 * intermediate IOCTLCMD_ representation.
 */
#  define IOCTLCMD(_cmd) IOCTL_VMX86_ ## _cmd
#endif


enum IOCTLCmd {
   /*
    * We need to bracket the range of values used for ioctls, because x86_64
    * Linux forces us to explicitly register ioctl handlers by value for
    * handling 32 bit ioctl syscalls.  Hence FIRST and LAST.  FIRST must be
    * 2001 so that VERSION is 2001 for backwards compatibility.
    */
#if defined __linux__ || defined _WIN32
   /* Start at 2001 because legacy code did. */
   IOCTLCMD(FIRST) = 2001,
#else
   /* Start at 0. */
   IOCTLCMD(FIRST),
#endif
   IOCTLCMD(VERSION) = IOCTLCMD(FIRST),
   IOCTLCMD(CREATE_VM),
   IOCTLCMD(RELEASE_VM),
   IOCTLCMD(GET_NUM_VMS),
   IOCTLCMD(ALLOC_CROSSGDT),
   IOCTLCMD(INIT_VM),
   IOCTLCMD(INIT_CROSSGDT),
   IOCTLCMD(LATE_INIT_VM),
   IOCTLCMD(RUN_VM),
   IOCTLCMD(LOOK_UP_MPN),
#if defined __linux__
   IOCTLCMD(LOOK_UP_LARGE_MPN),
#endif
   IOCTLCMD(LOCK_PAGE),
   IOCTLCMD(UNLOCK_PAGE),
   IOCTLCMD(APIC_INIT),
   IOCTLCMD(SET_HARD_LIMIT),
   IOCTLCMD(GET_MEM_INFO),
   IOCTLCMD(ADMIT),
   IOCTLCMD(UPDATE_MEM_INFO),
   IOCTLCMD(READMIT),
   IOCTLCMD(PAE_ENABLED),
#ifndef __APPLE__
   IOCTLCMD(HOST_X86_64),
#else
   IOCTLCMD(HOST_X86_CM),
#endif
   IOCTLCMD(GET_TOTAL_MEM_USAGE),
   IOCTLCMD(COMPLETE_USER_CALL),
   IOCTLCMD(GET_KHZ_ESTIMATE),
   IOCTLCMD(SET_HOST_CLOCK_RATE),
   IOCTLCMD(READ_PAGE),
   IOCTLCMD(WRITE_PAGE),
   IOCTLCMD(LOCK_PAGE_NEW),
   IOCTLCMD(UNLOCK_PAGE_BY_MPN),
   IOCTLCMD(MARK_LOCKEDVARANGE_CLEAN),
   IOCTLCMD(COW_SHARE),
   IOCTLCMD(COW_CHECK),
   IOCTLCMD(COW_UPDATE_HINT),
   IOCTLCMD(COW_COPY_PAGE),
   IOCTLCMD(COW_GET_ZERO_MPN),
   IOCTLCMD(COW_INC_ZERO_REF),
    /* AWE calls */
   IOCTLCMD(ALLOC_LOCKED_PAGES),
   IOCTLCMD(FREE_LOCKED_PAGES),
   IOCTLCMD(GET_LOCKED_PAGES_LIST),

   IOCTLCMD(APIC_ID),
   IOCTLCMD(SVM_ENABLED_CPU),
   IOCTLCMD(VT_ENABLED_CPU),
   IOCTLCMD(VT_SUPPORTED_CPU),
   IOCTLCMD(GET_ALL_MSRS),
   IOCTLCMD(BROKEN_CPU_HELPER),

   IOCTLCMD(COUNT_PRESENT_PAGES),
  
   IOCTLCMD(INIT_NUMA_INFO),
   IOCTLCMD(GET_NUMA_MEM_STATS),
   
   IOCTLCMD(GET_REFERENCE_CLOCK_HZ),
   IOCTLCMD(INIT_PSEUDO_TSC),
   IOCTLCMD(CHECK_PSEUDO_TSC),
   IOCTLCMD(GET_PSEUDO_TSC),

   IOCTLCMD(SYNC_GET_TSCS),
   IOCTLCMD(SYNC_SET_TSCS),

   IOCTLCMD(GET_IPI_VECTORS),
   IOCTLCMD(SEND_IPI),

   /*
    * Keep host-specific calls at the end so they can be undefined
    * without renumbering the common calls.
    */

#if defined __linux__ || defined __APPLE__
   IOCTLCMD(SET_UID),		// VMX86_DEVEL only
   IOCTLCMD(ALLOW_CORE_DUMP),

   // Not implemented in the main branch.
   IOCTLCMD(REGISTER_PASSTHROUGH_IO),
   IOCTLCMD(REGISTER_PASSTHROUGH_IRQ),
   IOCTLCMD(FREE_PASSTHROUGH_IO),
   IOCTLCMD(FREE_PASSTHROUGH_IRQ),
   IOCTLCMD(START_PASSTHROUGH),
   IOCTLCMD(STOP_PASSTHROUGH),
   IOCTLCMD(QUERY_PASSTHROUGH),

   IOCTLCMD(REGISTER_PERFCTR),
   IOCTLCMD(START_PERFCTR),
   IOCTLCMD(STOP_PERFCTR),
   IOCTLCMD(RELEASE_PERFCTR),

   IOCTLCMD(GET_ALL_CPUID),

   IOCTLCMD(SET_THREAD_AFFINITY),
   IOCTLCMD(GET_THREAD_AFFINITY),

   IOCTLCMD(GET_KERNEL_CLOCK_RATE),
#endif

#if defined _WIN32 || defined __APPLE__
   IOCTLCMD(ALLOC_CONTIG_PAGES),
#endif

#if defined _WIN32 || defined __linux__
   IOCTLCMD(ACK_USER_CALL),
#endif

#if defined _WIN32
   IOCTLCMD(FREE_CONTIG_PAGES),
   IOCTLCMD(BEEP),
   IOCTLCMD(HARD_LIMIT_MONITOR_STATUS),	// Windows 2000 only
   IOCTLCMD(BLUE_SCREEN),	// USE_BLUE_SCREEN only
   IOCTLCMD(CHANGE_HARD_LIMIT),
   IOCTLCMD(GET_KERNEL_PROC_ADDRESS),
   IOCTLCMD(READ_VA64),
   IOCTLCMD(SET_MEMORY_PARAMS),
   IOCTLCMD(REMEMBER_KHZ_ESTIMATE),
#endif

#if defined __APPLE__
   IOCTLCMD(ALLOC_LOW_PAGES),
   IOCTLCMD(INIT_DRIVER),
   IOCTLCMD(BLUEPILL),
#endif

   IOCTLCMD(SET_POLL_TIMEOUT_PTR),

   IOCTLCMD(FAST_SUSP_RES_SET_OTHER_FLAG),
   IOCTLCMD(FAST_SUSP_RES_GET_MY_FLAG),

#if defined __linux__
   IOCTLCMD(SET_HOST_CLOCK_PRIORITY),
   IOCTLCMD(VMX_ENABLED),
   IOCTLCMD(IOMMU_SETUP_MMU),
   IOCTLCMD(IOMMU_REGISTER_DEVICE),
   IOCTLCMD(IOMMU_UNREGISTER_DEVICE),
   IOCTLCMD(USING_SWAPBACKED_PAGEFILE),
   IOCTLCMD(USING_MLOCK),
   IOCTLCMD(SET_HOST_SWAP_SIZE),
#endif

   // Must be last.
   IOCTLCMD(LAST)
};


#if defined _WIN32
/*
 * Windows ioctl definitions.
 *
 * We use the IRP Information field for the return value
 * of IOCTLCMD_RUN_VM, to be faster since it is used a lot.
 */

#define FILE_DEVICE_VMX86        0x8101
#define VMX86_IOCTL_BASE_INDEX   0x801
#define VMIOCTL_BUFFERED(name) \
     CTL_CODE(FILE_DEVICE_VMX86, \
	       VMX86_IOCTL_BASE_INDEX + IOCTLCMD_ ## name, \
	       METHOD_BUFFERED, \
	       FILE_ANY_ACCESS)
#define VMIOCTL_NEITHER(name) \
      CTL_CODE(FILE_DEVICE_VMX86, \
	       VMX86_IOCTL_BASE_INDEX + IOCTLCMD_ ## name, \
	       METHOD_NEITHER, \
	       FILE_ANY_ACCESS)

#define IOCTL_VMX86_VERSION             VMIOCTL_BUFFERED(VERSION)
#define IOCTL_VMX86_CREATE_VM           VMIOCTL_BUFFERED(CREATE_VM)
#define IOCTL_VMX86_RELEASE_VM          VMIOCTL_BUFFERED(RELEASE_VM)
#define IOCTL_VMX86_GET_NUM_VMS         VMIOCTL_BUFFERED(GET_NUM_VMS)
#define IOCTL_VMX86_ALLOC_CROSSGDT      VMIOCTL_BUFFERED(ALLOC_CROSSGDT)
#define IOCTL_VMX86_INIT_VM             VMIOCTL_BUFFERED(INIT_VM)
#define IOCTL_VMX86_INIT_CROSSGDT       VMIOCTL_BUFFERED(INIT_CROSSGDT)
#define IOCTL_VMX86_INIT_NUMA_INFO      VMIOCTL_BUFFERED(INIT_NUMA_INFO)
#define IOCTL_VMX86_GET_NUMA_MEM_STATS  VMIOCTL_BUFFERED(GET_NUMA_MEM_STATS)
#define IOCTL_VMX86_LATE_INIT_VM        VMIOCTL_BUFFERED(LATE_INIT_VM)
#define IOCTL_VMX86_RUN_VM              VMIOCTL_NEITHER(RUN_VM)
#define IOCTL_VMX86_SEND_IPI            VMIOCTL_NEITHER(SEND_IPI)
#define IOCTL_VMX86_GET_IPI_VECTORS     VMIOCTL_BUFFERED(GET_IPI_VECTORS)
#define IOCTL_VMX86_LOOK_UP_MPN         VMIOCTL_BUFFERED(LOOK_UP_MPN)
#define IOCTL_VMX86_LOCK_PAGE           VMIOCTL_BUFFERED(LOCK_PAGE)
#define IOCTL_VMX86_UNLOCK_PAGE         VMIOCTL_BUFFERED(UNLOCK_PAGE)
#define IOCTL_VMX86_APIC_INIT           VMIOCTL_BUFFERED(APIC_INIT)
#define IOCTL_VMX86_SET_HARD_LIMIT      VMIOCTL_BUFFERED(SET_HARD_LIMIT)
#define IOCTL_VMX86_GET_MEM_INFO        VMIOCTL_BUFFERED(GET_MEM_INFO)
#define IOCTL_VMX86_ADMIT               VMIOCTL_BUFFERED(ADMIT)
#define IOCTL_VMX86_READMIT             VMIOCTL_BUFFERED(READMIT)
#define IOCTL_VMX86_UPDATE_MEM_INFO     VMIOCTL_BUFFERED(UPDATE_MEM_INFO)
#define IOCTL_VMX86_PAE_ENABLED         VMIOCTL_BUFFERED(PAE_ENABLED)
#define IOCTL_VMX86_HOST_X86_64         VMIOCTL_BUFFERED(HOST_X86_64)
#define IOCTL_VMX86_COW_SHARE           VMIOCTL_BUFFERED(COW_SHARE)
#define IOCTL_VMX86_COW_CHECK           VMIOCTL_BUFFERED(COW_CHECK)
#define IOCTL_VMX86_COW_UPDATE_HINT     VMIOCTL_BUFFERED(COW_UPDATE_HINT)
#define IOCTL_VMX86_COW_COPY_PAGE       VMIOCTL_BUFFERED(COW_COPY_PAGE)
#define IOCTL_VMX86_COW_GET_ZERO_MPN    VMIOCTL_BUFFERED(COW_GET_ZERO_MPN)
#define IOCTL_VMX86_COW_INC_ZERO_REF    VMIOCTL_BUFFERED(COW_INC_ZERO_REF)
#define IOCTL_VMX86_BEEP                VMIOCTL_BUFFERED(BEEP)
#define IOCTL_VMX86_HARD_LIMIT_MONITOR_STATUS   VMIOCTL_BUFFERED(HARD_LIMIT_MONITOR_STATUS)
#define IOCTL_VMX86_CHANGE_HARD_LIMIT   VMIOCTL_BUFFERED(CHANGE_HARD_LIMIT)
#define IOCTL_VMX86_ALLOC_CONTIG_PAGES  VMIOCTL_BUFFERED(ALLOC_CONTIG_PAGES)
#define IOCTL_VMX86_FREE_CONTIG_PAGES   VMIOCTL_BUFFERED(FREE_CONTIG_PAGES)

#define IOCTL_VMX86_GET_TOTAL_MEM_USAGE	VMIOCTL_BUFFERED(GET_TOTAL_MEM_USAGE)
#define IOCTL_VMX86_ACK_USER_CALL       VMIOCTL_BUFFERED(ACK_USER_CALL)
#define IOCTL_VMX86_COMPLETE_USER_CALL  VMIOCTL_BUFFERED(COMPLETE_USER_CALL)
#define IOCTL_VMX86_GET_KHZ_ESTIMATE    VMIOCTL_BUFFERED(GET_KHZ_ESTIMATE)
#define IOCTL_VMX86_SET_HOST_CLOCK_RATE VMIOCTL_BUFFERED(SET_HOST_CLOCK_RATE)
#define IOCTL_VMX86_SYNC_GET_TSCS       VMIOCTL_BUFFERED(SYNC_GET_TSCS)
#define IOCTL_VMX86_SYNC_SET_TSCS       VMIOCTL_BUFFERED(SYNC_SET_TSCS)
#define IOCTL_VMX86_READ_PAGE           VMIOCTL_BUFFERED(READ_PAGE)
#define IOCTL_VMX86_WRITE_PAGE          VMIOCTL_BUFFERED(WRITE_PAGE)
#define IOCTL_VMX86_LOCK_PAGE_NEW       VMIOCTL_BUFFERED(LOCK_PAGE_NEW)
#define IOCTL_VMX86_UNLOCK_PAGE_BY_MPN  VMIOCTL_BUFFERED(UNLOCK_PAGE_BY_MPN)
#define IOCTL_VMX86_ALLOC_LOCKED_PAGES  VMIOCTL_BUFFERED(ALLOC_LOCKED_PAGES)
#define IOCTL_VMX86_FREE_LOCKED_PAGES   VMIOCTL_BUFFERED(FREE_LOCKED_PAGES)
#define IOCTL_VMX86_GET_LOCKED_PAGES_LIST VMIOCTL_BUFFERED(GET_LOCKED_PAGES_LIST)

#define IOCTL_VMX86_GET_KERNEL_PROC_ADDRESS  VMIOCTL_BUFFERED(GET_KERNEL_PROC_ADDRESS)
#define IOCTL_VMX86_READ_VA64           VMIOCTL_BUFFERED(READ_VA64)
#define IOCTL_VMX86_SET_MEMORY_PARAMS   VMIOCTL_BUFFERED(SET_MEMORY_PARAMS)

#define IOCTL_VMX86_REMEMBER_KHZ_ESTIMATE VMIOCTL_BUFFERED(REMEMBER_KHZ_ESTIMATE)

#define IOCTL_VMX86_APIC_ID             VMIOCTL_BUFFERED(APIC_ID)
#define IOCTL_VMX86_SVM_ENABLED_CPU     VMIOCTL_BUFFERED(SVM_ENABLED_CPU)
#define IOCTL_VMX86_VT_ENABLED_CPU      VMIOCTL_BUFFERED(VT_ENABLED_CPU)
#define IOCTL_VMX86_VT_SUPPORTED_CPU    VMIOCTL_BUFFERED(VT_SUPPORTED_CPU)
#define IOCTL_VMX86_GET_ALL_MSRS        VMIOCTL_BUFFERED(GET_ALL_MSRS)
#define IOCTL_VMX86_BROKEN_CPU_HELPER   VMIOCTL_BUFFERED(BROKEN_CPU_HELPER)
#define IOCTL_VMX86_COUNT_PRESENT_PAGES	VMIOCTL_BUFFERED(COUNT_PRESENT_PAGES)

#define IOCTL_VMX86_FAST_SUSP_RES_SET_OTHER_FLAG VMIOCTL_BUFFERED(FAST_SUSP_RES_SET_OTHER_FLAG)
#define IOCTL_VMX86_FAST_SUSP_RES_GET_MY_FLAG    VMIOCTL_BUFFERED(FAST_SUSP_RES_GET_MY_FLAG)

#define IOCTL_VMX86_GET_REFERENCE_CLOCK_HZ   VMIOCTL_BUFFERED(GET_REFERENCE_CLOCK_HZ)
#define IOCTL_VMX86_INIT_PSEUDO_TSC          VMIOCTL_BUFFERED(INIT_PSEUDO_TSC)
#define IOCTL_VMX86_CHECK_PSEUDO_TSC         VMIOCTL_BUFFERED(CHECK_PSEUDO_TSC)
#define IOCTL_VMX86_GET_PSEUDO_TSC           VMIOCTL_NEITHER(GET_PSEUDO_TSC)
#define IOCTL_VMX86_SET_HOST_CLOCK_PRIORITY  VMIOCTL_BUFFERED(SET_HOST_CLOCK_PRIORITY)
#endif


/*
 * Return codes from page locking, unlocking, and MPN lookup.
 * They share an error code space because they call one another
 * internally.
 *
 *    PAGE_LOCK_FAILED              The host refused to lock a page.
 *    PAGE_LOCK_LIMIT_EXCEEDED      We have reached the limit of locked
 *                                  pages for all VMs
 *    PAGE_LOCK_TOUCH_FAILED        Failed to touch page after lock.
 *    PAGE_LOCK_IN_TRANSITION       The page is locked but marked by Windows
 *                                  as nonpresent in CPU PTE and in transition 
 *                                  in Windows PFN.  
 *
 *    PAGE_LOCK_SYS_ERROR           System call error.
 *    PAGE_LOCK_ALREADY_LOCKED      Page already locked.
 *    PAGE_LOCK_MEMTRACKER_ERROR    MemTracker fails.
 *    PAGE_LOCK_PHYSTRACKER_ERROR   PhysTracker fails.
 *    PAGE_LOCK_MDL_ERROR           Mdl error on Windows.
 *
 *    PAGE_UNLOCK_NO_ERROR          Unlock successful (must be 0).
 *    PAGE_UNLOCK_NOT_TRACKED       Not in memtracker.
 *    PAGE_UNLOCK_NO_MPN            Tracked but no MPN.
 *    PAGE_UNLOCK_NOT_LOCKED        Not locked.
 *    PAGE_UNLOCK_TOUCH_FAILED      Failed to touch page.
 *    PAGE_UNLOCK_MISMATCHED_TYPE   Tracked but was locked by different API 
 *
 *    PAGE_LOOKUP_INVALID_ADDR      Consistency checking on Windows.
 *    PAGE_LOOKUP_BAD_HIGH_ADDR     Consistency checking on Windows.
 *    PAGE_LOOKUP_ZERO_ADDR         Consistency checking on Windows.
 *    PAGE_LOOKUP_SMALL_ADDR        Consistency checking on Windows.
 *
 * All error values must be negative values less than -4096 to avoid
 * conflicts with errno values on Linux.
 *
 * -- edward
 */

#define PAGE_LOCK_FAILED              (-10001)
#define PAGE_LOCK_LIMIT_EXCEEDED      (-10002)
#define PAGE_LOCK_TOUCH_FAILED        (-10003)
#define PAGE_LOCK_IN_TRANSITION       (-10004)

#define PAGE_LOCK_SYS_ERROR           (-10010)
#define PAGE_LOCK_ALREADY_LOCKED      (-10011)
#define PAGE_LOCK_MEMTRACKER_ERROR    (-10012)
#define PAGE_LOCK_PHYSTRACKER_ERROR   (-10013)
#define PAGE_LOCK_MDL_ERROR           (-10014)

#define PAGE_UNLOCK_NO_ERROR		   0
#define PAGE_UNLOCK_NOT_TRACKED	      (-10100)
#define PAGE_UNLOCK_NO_MPN            (-10101)
#define PAGE_UNLOCK_NOT_LOCKED        (-10102)
#define PAGE_UNLOCK_TOUCH_FAILED      (-10103)
#define PAGE_UNLOCK_MISMATCHED_TYPE   (-10104)

#define PAGE_LOOKUP_INVALID_ADDR      (-10200)
#define PAGE_LOOKUP_BAD_HIGH_ADDR     (-10201)
#define PAGE_LOOKUP_ZERO_ADDR         (-10202)
#define PAGE_LOOKUP_SMALL_ADDR        (-10203)
#define PAGE_LOOKUP_NOT_TRACKED          (-10)	// added to another code
#define PAGE_LOOKUP_NO_MPN               (-20)	// added to another code
#define PAGE_LOOKUP_NOT_LOCKED           (-30)	// added to another code
#define PAGE_LOOKUP_NO_VM                (-40)	// added to another code

#define PAGE_LOCK_SUCCESS(mpn) ((int) (mpn) >= 0)
#define PAGE_LOCK_SOFT_FAILURE(mpn) ((int) (mpn) <= PAGE_LOCK_FAILED && \
                                     (int) (mpn) > PAGE_LOCK_SYS_ERROR)


/*
 * Flags sent into APICBASE ioctl
 */

#define APIC_FLAG_DISABLE_NMI       0x00000001
#define APIC_FLAG_PROBE             0x00000002
#define APIC_FLAG_FORCE_ENABLE      0x00000004


/*
 * Structure sent into REGISTER_PERFCOUNTERS ioctl
 */

#define PERFCTR_MAX_COUNTERS        2
#define PERFCTR_INVALID_EVENT       0
#define PERFCTR_INVALID_IRQ         (-1)

typedef struct PerfCtrInfo {
   uint32 eventNum;
   uint32 samplingRate;
} PerfCtrInfo;

typedef struct PerfCtrRegisterArgs {
   PerfCtrInfo counterInfo[PERFCTR_MAX_COUNTERS];
   int irq;
   Bool totalOnly;
} PerfCtrRegisterArgs;

typedef struct VMAPICInfo {
   uint32 flags;
} VMAPICInfo; 

#ifndef MAX_PROCESSORS
#define MAX_PROCESSORS 64
#endif

typedef struct TSCSet {
   uint64 TSCs[MAX_PROCESSORS];
   /* 
    * Bitset indicating which TSC values were successfully read by the driver.
    * Allocated in groups of 2 for 32 & 64-bit agreement on struct size.
    */
   Atomic_uint32 valid[2 * ((MAX_PROCESSORS + 63) / 64)];
} TSCSet;

typedef struct PassthruIOMMUMap {
   uint64 numPages;      // How many memory pages guest has
   MPN    mpn[0];        // GPN->PPN mapping
} PassthruIOMMUMap;


static INLINE void
TSCSet_SetValid(TSCSet *tscSet, unsigned cpuNum)
{
   ASSERT(cpuNum < MAX_PROCESSORS);
   Atomic_Or32(&tscSet->valid[cpuNum / 32], 1 << (cpuNum % 32));
}

static INLINE Bool
TSCSet_IsValid(TSCSet *tscSet, unsigned cpuNum)
{
   ASSERT(cpuNum < MAX_PROCESSORS);
   return (Atomic_Read32(&tscSet->valid[cpuNum / 32]) & 
           (1 << (cpuNum % 32))) != 0;
}

#define VMX86_DRIVER_VCPUID_OFFSET	1000


/*
 * We keep track of 3 different limits on the number of pages we can lock.
 * The host limit is determined at driver load time (in windows only) to
 * make sure we do not starve the host by locking too many pages.
 * The static limit is user defined in the UI and the dynamic limit is 
 * set by authd's hardLimitMonitor code (windows only), which queries
 * host load and adjusts the limit accordingly.  We lock the minimum of 
 * all these values at any given time.
 */
typedef struct LockedPageLimit {
   uint32 host;        // driver calculated maximum for this host
   uint32 configured;  // user defined maximum pages to lock
   uint32 dynamic;     // authd hardLimitMonitor pages to lock
} LockedPageLimit;

/*
 * Data structures for the GET_MEM_INFO and ADMIT ioctls.
 */

typedef struct VMMemMgmtInfo {
   uint32          minAllocation;   // minimum pages for vm
   uint32          maxAllocation;   // maximum pages the vm could lock
   uint32          shares;          // proportional sharing weight
   uint32          nonpaged;        // overhead memory
   uint32          paged;           // guest memory(mainmem + other regions)
   uint32          mainMemSize;     // guest main memory size
   uint32          locked;          // number of pages locked by this vm
   uint32          shared;          // number of guest pages shared
   uint32          perVMOverhead;   // memory for vmx/vmmon overheads
   uint32          breaksAvg;       // average number of broken COW pages
   Percent         sharedPctAvg;    // average success rate of page sharing
   Percent         usedPct;         // % of guest memory being touched
   Bool            admitted;        // admission control
   uint8           _pad[5];
   PShare_MgmtInfo pshareMgmtInfo;  // management info for pshare scan rates
   uint64          hugePageBytes;    // number of bytes occupied by huge pages
} VMMemMgmtInfo;

typedef struct VMMemMgmtInfoPatch {
   uint32          breaksAvg;
   Percent         sharedPctAvg;
   Percent         usedPct;
   uint8           _pad[2];
   uint64          hugePageBytes;
} VMMemMgmtInfoPatch;

#define VMMEM_COW_HOT_PAGES 10

typedef struct VMMemCOWHotPage {
   MPN             mpn;
   uint32          ref;
   uint64          key;
   uint8           pageClass;
   uint8           _pad[7];
} VMMemCOWHotPage;

typedef struct VMMemCOWInfo {
   uint32          numHints;
   uint32          uniqueMPNs;       // current MPNs used for COW
   uint32          totalUniqueMPNs;  // total MPNs ever used as COW
   uint32          numBreaks;
   uint32          numRef;
   uint32          _pad[1];
   VMMemCOWHotPage hot[VMMEM_COW_HOT_PAGES];
} VMMemCOWInfo;

typedef struct VMMemInfoArgs {
   VMMemCOWInfo    cowInfo;            // global page sharing state
   uint32          minVmMemPct;        // % of vm that must fit in memory
   uint32          globalMinAllocation;// pages that must fit in maxLockedPages
   uint32          numLockedPages;     // total locked pages by all vms
   LockedPageLimit lockedPageLimit;    // set of locked page limits
   uint32          maxLockedPages;     // effective limit on locked pages
   uint32          callerIndex;        // this vm's index memInfo array
   uint32          numVMs;             // number of running VMs
   uint8           _pad[4];
   VMMemMgmtInfo   memInfo[1];
} VMMemInfoArgs;

#define VM_GET_MEM_INFO_SIZE(numVMs) \
   (sizeof(VMMemInfoArgs) - sizeof(VMMemMgmtInfo) + (numVMs) * sizeof(VMMemMgmtInfo))

typedef struct VMMPNList {
   uint32    mpnCount;   // IN (and OUT on Mac OS)
   uint32    pad;
   VA64      mpn32List;  // IN: User VA of an array of 32-bit MPNs.
} VMMPNList;

typedef struct VARange {
   VA64     addr;
   VA64     bv;
   unsigned len;
   uint32   pad;
} VARange;

typedef struct VMMUnlockPageByMPN {
   MPN32     mpn;
   uint32    pad;
   VA64      uAddr;	    /* IN: User VA of the page (optional). */
} VMMUnlockPageByMPN;

typedef struct VMMReadWritePage {
   MPN32        mpn; // IN
   uint32       pad;
   VA64         uAddr; // IN: User VA of a PAGE_SIZE-large buffer.
} VMMReadWritePage;

struct passthrough_iorange {
   unsigned short ioBase;   /* Base of range to pass through. */
   unsigned short numPorts; /* Length of range. */
};

typedef struct COWShareInfo {
   uint32       numPages;
   MPN          pshareMPN;
   Bool         shareFailure;
} COWShareInfo;

typedef struct COWHintInfo {
   uint32            numHints;
   PShare_HintUpdate updates[PSHARE_HINT_BATCH_PAGES_MAX];
} COWHintInfo;

typedef struct COWCheckInfo {
   uint32              numPages;
   PShare_COWCheckInfo check[PSHARE_MAX_COW_CHECK_PAGES];
} COWCheckInfo;

/*
 * Data structure for the INIT_PSEUDO_TSC and CHECK_PSEUDO_TSC.
 */

typedef struct PTSCInitParams {
   RateConv_Params refClockToTSC;
   uint64          tscHz;
   Bool            forceRefClock;
   Bool            forceTSC;
   uint8           _pad[6];
} PTSCInitParams;

typedef struct PTSCCheckParams {
   uint64 lastTSC;
   uint64 lastRC;
   Bool   usingRefClock;
   uint8  _pad[7];
} PTSCCheckParams;

#ifndef VMX86_SERVER
typedef struct NUMAInfoArgs {
   uint32           numNodes;              // Keep this first in the structure
   uint32           numMemRanges;
   NUMA_NodeInfo    nodes[NUMA_MAX_NODES]; // Info about all NUMA nodes
} NUMAInfoArgs;
 
#define NUMA_INFO_ARGS_SIZE(args, numNodes) \
  (sizeof(args) - (sizeof(NUMA_NodeInfo) * NUMA_MAX_NODES) \
                                     + (sizeof(NUMA_NodeInfo) * numNodes))

typedef struct VMNUMAMemStatsArgs {
   uint32     	curCPU;
   uint32	curNUMANode;
   uint32     	numPagesPerNode[NUMA_MAX_NODES]; //For each NUMA node
} VMNUMAMemStatsArgs;

typedef struct {
   uint8 vectors[2];
} IPIVectors;

#endif
 
/*
 * This struct is passed to IOCTL_VMX86_INIT_CROSSGDT to fill in a crossGDT 
 * entry.
 */
typedef struct InitCrossGDT {
   uint32 index;      // index in crossGDT to update (offset / 8)
   Descriptor value;  // value to set the crossGDT entry to
} InitCrossGDT;


#if !defined _WIN32

typedef struct {
   uint64              ioarg;	// 64bit address
   uint64              iocmd;	// 64bit command
} VMIoctl64;

/*
 * We can move it at the beginning of our ioctl list (just below
 * IOCTLCMD_VERSION). It just should not change very often...
 */
// XXXMACOS Do we need this?
#define IOCTL_VMX86_IOCTL64  _IOW(0x99, 0x80, VMIoctl64)

/*
 * Structures for passing process affinity between vmmon and userspace.
 */
struct VMMonAffinity {
   uint32   pid;      // IN
   uint32   affinity; // IN for set, OUT for get, bit 0 == processor 0...
};
#endif

#if __linux__
/*
 * Linux uses mmap(2) to allocate contiguous locked pages, and uses these
 * macros to marshall real arguments to mmap's made-up 'offset' argument.
 */

#define VMMON_MAP_MT_LOW4GB	0
#define VMMON_MAP_MT_LOW16MB	1
#define VMMON_MAP_MT_ANY	2

#define VMMON_MAP_OFFSET_SHIFT	0
#define VMMON_MAP_OFFSET_MASK	0x00000FFF
#define VMMON_MAP_ORDER_SHIFT	12
#define VMMON_MAP_ORDER_MASK	0xF
#define VMMON_MAP_MT_SHIFT	16
#define VMMON_MAP_MT_MASK	0x7
#define VMMON_MAP_RSVD_SHIFT	19

#define VMMON_MAP_RSVD(base)	\
		((base) >> VMMON_MAP_RSVD_SHIFT)
#define VMMON_MAP_MT(base)	\
		(((base) >> VMMON_MAP_MT_SHIFT) & VMMON_MAP_MT_MASK)
#define VMMON_MAP_ORDER(base)	\
		(((base) >> VMMON_MAP_ORDER_SHIFT) & VMMON_MAP_ORDER_MASK)
#define VMMON_MAP_OFFSET(base)	\
		(((base) >> VMMON_MAP_OFFSET_SHIFT) & VMMON_MAP_OFFSET_MASK)

#define VMMON_MAP_BASE(mt, order)	(((mt) << VMMON_MAP_MT_SHIFT) | \
					 ((order) << VMMON_MAP_ORDER_SHIFT))

#elif defined _WIN32
/*
 * Windows uses an ioctl to allocate contiguous locked pages.
 */

typedef struct VMAllocContiguousMem {
   VA64   mpn32List;  // IN: User VA of an array of 32-bit MPNs.
   uint32 mpnCount; // IN
   uint32 order;    // IN
   MPN32  maxMPN;   // IN
   uint32 _pad[1];
} VMAllocContiguousMem;
#elif defined __APPLE__
#   include "iocontrolsMacos.h"
#endif

/* Clean up helper macros */
#undef IOCTLCMD

#endif // ifndef _IOCONTROLS_H_
