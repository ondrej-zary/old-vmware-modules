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
 * vmx86.c --
 *
 *     Platform independent routines for creating/destroying/running
 *     virtual machine monitors.
 *
 */

#ifdef linux
/* Must come before any kernel header file --hpreg */
#   include "driver-config.h"

#   include <linux/string.h> /* memset() in the kernel */
#   include <linux/sched.h> /* jiffies from the kernel */
#else
#   include <string.h>
#endif

#include "vmware.h"
#include "vm_assert.h"
#include "vm_basic_math.h"
#include "vmx86.h"
#include "task.h"
#include "initblock.h"
#include "vm_asm.h"
#include "iocontrols.h"
#include "hostif.h"
#include "cpuid.h"
#include "memtrack.h"
#include "hashFunc.h"
#include "pageUtil.h"
#if defined(_WIN64)
#include "x86.h"
#include "vmmon-asm-x86-64.h"
#endif
#include "x86vt.h"
#include "x86svm.h"
#if defined(linux)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0) && \
    (!defined(VM_X86_64) || LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 9))
#include <asm/timex.h>
#define VMW_HAS_CPU_KHZ
#endif
#endif


PseudoTSC pseudoTSC;

/*
 * Dynamic structures for keeping NUMA info. 
 */
static unsigned int     numaNumNodes;         // Count of NodeInfos
static unsigned int     numaNumMemRanges;     // Count of MemRanges
static NUMA_NodeInfo    *numaNodes;           // Array of NodeInfos
static NUMA_MemRange    *numaMemRangesList;   // Array of MemRanges
// A fast APIC->NumaInfo lookup table
static NUMA_NodeInfo*   apicToNUMANode[MAX_LAPIC_ID];

/*
 * Keep track of the virtual machines that have been
 * created using the following structures.
 *
 */

static VMDriver *vmDriverList = NULL;

static LockedPageLimit lockedPageLimit = {
   0,                // host: does not need to be initialized.
   0,                // configured: must be set by some VM as it is powered on.
   MAX_LOCKED_PAGES, // dynamic
};

/* Percentage of guest "paged" memory that must fit within the hard limit. */
static unsigned minVmMemPct;

/* Number of pages actually locked by all virtual machines */
static unsigned numLockedPages;

/* Total virtual machines on this host */
static unsigned vmCount;

/* Total number of open vmmon file handles. */
static unsigned fdCount;


/*
 * We implement a list of allocated VM ID's using an array.
 * The array is initialized with the values 1...MAX_VMS-1, INVALID_VMID.
 * vmIDsAllocated holds the last VM ID given out and vmIDsUnused
 * holds the next VM ID to give out.
 */

#define INVALID_VMID (-1)
static int vmIDList[MAX_VMS];
static int vmIDsAllocated;
static int vmIDsUnused;

/* Max rate requested for fast clock by any virtual machine. */
static unsigned globalFastClockRate;

/* Cached checking for VT / SVM bits across all processors. */
static Bool hvCapable = FALSE;
static Bool hvEnabled = FALSE;

/* Structure for map-reduce across all CPUs. */
typedef struct HVEnableData {
   Bool anyEnabled;    // OUT: set by any CPU that is locked enabled
   Bool anyDisabled;   // OUT: set by any CPU that is locked disabled
   Bool anyUnlocked;   // OUT: set by any CPU that is unlocked
   Bool anyNotCapable; // OUT: set by any CPU that is not HV-capable
   Bool hvForce;       // IN: whether to force HV if possible
} HVEnableData;




/*
 *----------------------------------------------------------------------
 *
 * Vmx86AdjustLimitForOverheads --
 *
 *        This function adjusts an overall limit on the number of
 *        locked pages to take into account overhead for the vmx processes, etc.
 *        since the hostOS will also see this as overhead. We do this for all
 *        vmx processes, not just ones whose vms have been admitted.
 *
 *        If "vm" is NULL, we are allocating a global page and have no
 *        perVMOverhead term to take into account.
 *
 * Results:
 *       Number of remaining pages considered to be lockable on this host.
 *
 * Side effects:
 *       None.
 *
 *----------------------------------------------------------------------
 */

static INLINE unsigned
Vmx86AdjustLimitForOverheads(const VMDriver* vm, const uint32 limit)
{
   uint32 extraCost = (vm != NULL) ? vmCount * vm->memInfo.perVMOverhead : 0;
   ASSERT(HostIF_GlobalLockIsHeld());

   return (extraCost < limit) ?  (limit - extraCost) : 0;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86LockedPageLimit --
 *
 *       There are three limits controlling how many pages we can lock on 
 *       a host:  
 *
 *       lockedPageLimit.configured is controlled by UI,  
 *       lockedPageLimit.dynamic is controlled by authd's hardLimitMonitor,
 *       lockedPageLimit.host is calculated dynamically based on kernel stats 
 *       by vmmon using kernel stats.
 *
 *       We can lock the MIN of these values.
 *
 * Results:
 *       Number of pages to lock on this host.
 *
 * Side effects:
 *       Updates the host locked pages limit.
 *
 *----------------------------------------------------------------------
 */

static INLINE unsigned
Vmx86LockedPageLimit(const VMDriver* vm)
{
   uint32 overallLimit;
   ASSERT(HostIF_GlobalLockIsHeld());

   lockedPageLimit.host = HostIF_EstimateLockedPageLimit(vm, numLockedPages);
   overallLimit = MIN(MIN(lockedPageLimit.configured, lockedPageLimit.dynamic),
                      lockedPageLimit.host);
 
   return Vmx86AdjustLimitForOverheads(vm, overallLimit);
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86LockedPageLimitForAdmissonControl --
 *
 *       There are two limits controlling how many pages we can lock on 
 *       a host:  
 *
 *       lockedPageLimit.configured is controlled by UI,  
 *       lockedPageLimit.dynamic is controled by authd's hardLimitMonitor,
 *
 *       We can lock the MIN of these values.
 *
 *       Using lockedPageLimit.host would be too pessimistic.  After admission
 *       of a new VM but before allocation/locking memory for the new VM 
 *       our memory sharing code will put pressure on other VMs and should 
 *       produce enough free pages to successfully finish poweron.
 *
 * Results:
 *       Number of pages to lock on this host.
 *
 * Side effects:
 *       None
 *
 *----------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER unsigned
Vmx86LockedPageLimitForAdmissonControl(const VMDriver *vm)
{
   uint32 overallLimit = MIN(lockedPageLimit.configured, 
                             lockedPageLimit.dynamic);
   ASSERT(HostIF_GlobalLockIsHeld());
   return Vmx86AdjustLimitForOverheads(vm, overallLimit);
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86HasFreePages --
 *
 *       Returns TRUE if the vm can lock more pages.  This is true if 
 *       we are below the host's hard memory limit and this vm has not
 *       exceeded its maximum allocation.
 *       Callers must ensure driver-wide and VM serialization
 *       typically by using HostIF_GlobalLock() and  HostIF_VMLock().
 *
 * Results:
 *       TRUE if pages can be locked, FALSE otherwise
 *
 * Side effects:
 *       None
 *
 *----------------------------------------------------------------------
 */

static INLINE Bool
Vmx86HasFreePages(VMDriver *vm, 
		  unsigned int numPages,
                  Bool checkVM)
{
   /*  
    * 1) Be careful with overflow. 
    * 2) lockedPageLimit and vm->memInfo.maxAllocation can be decreased below
    *    the current numLockedPages and vm->memInfo.locked
    * 3) lockedPageLimit.host can go lower than numLockedPages.
    */
   ASSERT(HostIF_GlobalLockIsHeld() &&
          (!checkVM || HostIF_VMLockIsHeld(vm)));

   if (checkVM) {
      /*
       * Check the per-vm limit.
       */
      ASSERT(HostIF_VMLockIsHeld(vm));
      if (vm->memInfo.admitted) {
	 if (vm->memInfo.maxAllocation <= vm->memInfo.locked) {
	    return FALSE;
	 } else if (vm->memInfo.maxAllocation - vm->memInfo.locked < numPages) {
	    return FALSE;
	 }
      }
   } else {
      /*
       * Check the global limit.
       */
      unsigned limit = Vmx86LockedPageLimit(vm);

      if (limit <= numLockedPages) {
	 return FALSE;
      } else if (limit - numLockedPages < numPages) {
	 return FALSE;
      }
   }
   return TRUE;
}




#ifdef VMX86_DEBUG
/*
 *----------------------------------------------------------------------
 *
 * Vmx86VMIsRegistered --
 *
 *      Check if "vm" is on the list of VMDrivers.
 *
 * Results:
 *      Return TRUE iff "vm" is on the list of VMDrivers.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------
 */

static Bool
Vmx86VMIsRegistered(VMDriver *vm, Bool needsLock) 
{
   VMDriver *tmp;
   Bool      found = FALSE;

   ASSERT(needsLock || HostIF_GlobalLockIsHeld());

   if (needsLock) {
      HostIF_GlobalLock(5);
   }

   for (tmp = vmDriverList; tmp != NULL; tmp = tmp->nextDriver) {
      if (tmp == vm) {
         found = TRUE;
         break;
      }
   }

   if (needsLock) {
      HostIF_GlobalUnlock(5);
   }

   return found;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_InitIDList --
 *
 *       Called when the driver is initialized.
 *       Set up the list of available VM ID's.
 *
 * Results:
 *       None. Sets up global data.
 *
 * Side effects:
 *       None.
 *
 *----------------------------------------------------------------------
 */

void
Vmx86_InitIDList(void)
{
   int i;

   HostIF_GlobalLock(32);

   for (i = 0; i < MAX_VMS; i++) {
      vmIDList[i] = i + 1;
   }
   vmIDList[MAX_VMS - 1] = INVALID_VMID;
   vmIDsUnused = 0;
   vmIDsAllocated = INVALID_VMID;


   HostIF_GlobalUnlock(32);
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86FreeVMID --
 *
 *       Return a VM ID to the list of available VM ID's.
 *
 * Results:
 *       None
 *
 * Side effects:
 *       None
 *
 *----------------------------------------------------------------------
 */

static void
Vmx86FreeVMID(int vmID) // IN
{
   int i;

   ASSERT(HostIF_GlobalLockIsHeld());

   /* Deleting head of the list. */
   if (vmID == vmIDsAllocated) {
      int tmp;

      tmp = vmIDList[vmIDsAllocated];
      vmIDList[vmIDsAllocated] = vmIDsUnused;
      vmIDsAllocated = tmp;
      vmIDsUnused = vmID;
      return;
   }

   for (i = vmIDsAllocated; vmIDList[i] != INVALID_VMID; i = vmIDList[i]) {
      if (vmIDList[i] == vmID) {
         vmIDList[i] = vmIDList[vmID];
         vmIDList[vmID] = vmIDsUnused;
         vmIDsUnused = vmID;
         return;
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86AllocVMID --
 *
 *       Grab a VM ID from the list of available VM ID's.
 *
 * Results:
 *       The VM ID, in the range [ 0 ; MAX_VMS ).
 *
 * Side effects:
 *       None
 *
 *----------------------------------------------------------------------
 */

static int
Vmx86AllocVMID(void)
{
   int vmID;

   ASSERT(HostIF_GlobalLockIsHeld());

   vmID = vmIDsUnused;
   ASSERT(0 <= vmID && vmID < MAX_VMS);
   vmIDsUnused = vmIDList[vmID];
   vmIDList[vmID] = vmIDsAllocated;
   vmIDsAllocated = vmID;

   return vmID;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86RegisterVMOnList --
 *
 *      Add a VM to the list of registered VMs and increment
 *      the count of VMs.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Add VM to linked list.
 *	Increment count of VMs.
 *
 *----------------------------------------------------------------
 */

static void
Vmx86RegisterVMOnList(VMDriver *vm) // IN
{
   int vmID;
   VMDriver **vmp;

   ASSERT(HostIF_GlobalLockIsHeld());
   vmCount++;
   vmID = Vmx86AllocVMID();
   ASSERT(vm->userID == 0);
   vm->userID = vmID + 1;
   ASSERT(vm->userID > 0);
   for (vmp = &vmDriverList; *vmp != NULL; vmp = &(*vmp)->nextDriver) {
      if (*vmp == vm) {
         Warning("VM %p already registered on the list of VMs.\n", vm);
         return;
      }
   }
   *vmp = vm;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86DeleteVMFromList --
 *
 *      Delete a VM from the list of registered VMs and decrement
 *      the count of VMs. This function should be called on any
 *      VM registered on the VMDriverList before invoking 
 *      Vmx86FreeAllVMResources to free its memory.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Remove VM from linked list.
 *	Decrement count of VMs.
 *
 *----------------------------------------------------------------
 */

static void
Vmx86DeleteVMFromList(VMDriver *vm) 
{
   VMDriver **vmp;

   ASSERT(vm);
   ASSERT(HostIF_GlobalLockIsHeld());
   for (vmp = &vmDriverList; *vmp != vm; vmp = &(*vmp)->nextDriver) {
      if (*vmp == NULL) {
         Warning("VM %p is not on the list of registered VMs.\n", vm);
         return;
      }
   }
   *vmp = vm->nextDriver;
   vmCount--;

   Vmx86FreeVMID(vm->userID - 1);
   numLockedPages -= vm->memInfo.locked;

   /*
    * If no VM is running, reset the configured locked-page limit so
    * that the next VM to power on sets it appropriately.
    */
   if (vmCount == 0) {
      lockedPageLimit.configured = 0;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * Vmx86FreeAllVMResources
 *
 *     Free the resources allocated for a vm that is not registered
 *     on the VMDriverList.  Except in the case of Vmx86_CreateVM(), 
 *     this should be called only after a call to Vmx86DeleteVMFromList().
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Memory freed.
 *
 *----------------------------------------------------------------------
 */

static void
Vmx86FreeAllVMResources(VMDriver *vm)
{
   ASSERT(!HostIF_GlobalLockIsHeld());
   if (vm) {   
      ASSERT(!Vmx86VMIsRegistered(vm, TRUE));

      Vmx86_SetHostClockRate(vm, 0);

      HostIF_FreeAllResources(vm);

      HostIF_FreeKernelMem(vm);
   }
}




/*
 *----------------------------------------------------------------------
 *
 * Vmx86ReserveFreePages --
 *
 *       Returns TRUE and increases locked page counts if the vm can lock 
 *       more pages.  This is true if we are below the host's hard memory 
 *       limit and this vm has not exceeded its maximum allocation.
 *       The function is thread-safe.
 *
 * Results:
 *       TRUE if pages are reserved for locking, FALSE otherwise
 *
 * Side effects:
 *       The global lock and VM's lock are acquired and released.
 *
 *----------------------------------------------------------------------
 */

static Bool
Vmx86ReserveFreePages(VMDriver *vm, 
		      unsigned int numPages)
{
   Bool retval = FALSE;
   int retries = 3;

   ASSERT(vm);
   
   for (retries = 3; !retval && (retries > 0); retries--) {
      HostIF_GlobalLock(17);
      HostIF_VMLock(vm, 0);
      
      // Check VM's limit and don't wait.
      retval = Vmx86HasFreePages(vm, numPages, TRUE);
      if (!retval) {
         HostIF_VMUnlock(vm, 0);
         HostIF_GlobalUnlock(17);
	 break;
      } else {
	 // Wait to satisfy the global limit.
	 retval = Vmx86HasFreePages(vm, numPages, FALSE);
	 if (retval) {
	    numLockedPages += numPages;
	    vm->memInfo.locked += numPages;
            HostIF_VMUnlock(vm, 0);
	    HostIF_GlobalUnlock(17);
	    break;
	 } else {
            /*
             * There are not enough pages -- drop the locks and wait for 
             * the host and/or other VMs to produce free pages.
	     */ 
            HostIF_VMUnlock(vm, 0);
	    HostIF_GlobalUnlock(17);
	    HostIF_WaitForFreePages(10);
	 }
      }
   }
   return retval;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86UnreserveFreePages --
 *
 *       Decreases the global and VM's locked page counts. 
 *       The function is thread-safe. 
 *
 * Results:
 *       void
 *
 * Side effects:
 *       The global lock and VM's lock are acquired and released.
 *
 *----------------------------------------------------------------------
 */

static void
Vmx86UnreserveFreePages(VMDriver *vm, 
			unsigned int numPages)
{
   ASSERT(vm);

   HostIF_GlobalLock(18);
   HostIF_VMLock(vm, 1);

   ASSERT(numLockedPages >= numPages);
   ASSERT(vm->memInfo.locked >= numPages);

   numLockedPages -= numPages;
   vm->memInfo.locked -= numPages;

   HostIF_VMUnlock(vm, 1);
   HostIF_GlobalUnlock(18);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmx86_CreateVM --
 *
 *      Allocate and initialize a driver structure for a virtual machine.
 *
 * Results:
 *      VMDriver structure or NULL on error.
 *
 * Side effects:
 *      May allocate kernel memory.
 *
 *-----------------------------------------------------------------------------
 */

VMDriver *
Vmx86_CreateVM(void)
{
   VMDriver *vm;
   Vcpuid v;


   vm = HostIF_AllocKernelMem(sizeof *vm, TRUE);
   if (vm == NULL) {
      return NULL;
   }
   memset(vm, 0, sizeof *vm);

   vm->userID = 0;
   vm->memInfo.admitted = FALSE;
   vm->fastSuspResFlag = 0;
   for (v = 0; v < MAX_INITBLOCK_CPUS; v++) {
      vm->currentHostCpu[v] = INVALID_HOST_CPU;
   }

   if (HostIF_Init(vm)) {
      goto cleanup;
   }

   HostIF_GlobalLock(0);

#ifdef _WIN32
   if (vmCount >= MAX_VMS_WIN32) {
      HostIF_GlobalUnlock(0);
      goto cleanup;
   }
#endif
   if (vmCount >= MAX_VMS) {
      HostIF_GlobalUnlock(0);
      goto cleanup;
   }

   Vmx86RegisterVMOnList(vm);

   HostIF_GlobalUnlock(0);

   return vm;

cleanup:
   /*
    * The VM is not on a list, "vmCount" has not been incremented,
    * "vm->cowID" is INVALID_VMID, and either the VM's mutex hasn't
    * been initialized or we've only taken the global lock and checked
    * a counter since, so we know that the VM has not yet locked any
    * pages.
    */
   ASSERT(vm->memInfo.locked == 0);
   Vmx86FreeAllVMResources(vm);
   return NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_ReleaseVM  --
 *
 *      Release a VM (either created here or from a bind).
 *
 * Results:
 *      zero if successful
 *
 * Side effects:
 *	Decrement VM reference count.
 *      Release resources (those that are left) when count reaches 0.
 *
 *----------------------------------------------------------------------
 */
int
Vmx86_ReleaseVM(VMDriver *vm)
{
   ASSERT(vm);
   HostIF_GlobalLock(1);
   Vmx86DeleteVMFromList(vm);
   HostIF_GlobalUnlock(1);
   Vmx86FreeAllVMResources(vm);

   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_Open --
 *
 *      Called on open of the fd.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Bumps fdCount.
 *
 *----------------------------------------------------------------------
 */

void
Vmx86_Open(void)
{
   HostIF_GlobalLock(123);
   ASSERT(fdCount < MAX_INT32);
   if (fdCount < MAX_INT32) {
      fdCount++;
   }
   HostIF_GlobalUnlock(123);
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_Close --
 *
 *      Called on close of the fd.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Decrements fdCount
 *	May de-initialize ptsc.
 *
 *----------------------------------------------------------------------
 */

void
Vmx86_Close(void)
{
   HostIF_GlobalLock(124);

   /* 
    * If fdCount hits MAX_INT32 saturate the counter and leave it at
    * MAX_INT32. 
    */
   ASSERT(fdCount > 0);
   if (fdCount < MAX_INT32) {
      fdCount--;
   }

   /* 
    * If no VMs are running and there are no open file handles, reset the
    * pseudo TSC state so that the next VM to initialize is free to
    * initialize the system wide PTSC however it wants.  See PR 403505.
    */
   if (fdCount == 0) {
      ASSERT(vmCount == 0);
      pseudoTSC.initialized = FALSE;
   }
   HostIF_GlobalUnlock(124);
}


/*
 *------------------------------------------------------------------------------
 *
 * Vmx86_InitNUMAInfo --
 *
 *    Initializaiton of the NUMA structures in the vmmon.  
 *    Expects all initial arguments to be part of the NUMAInfoArgs structure.
 *
 * Results:
 *    TRUE on success
 *    FALSE on failure
 *
 * Side effects:
 *    None.
 *
 *------------------------------------------------------------------------------
 */

Bool
Vmx86_InitNUMAInfo(NUMAInfoArgs *initParams) // IN 
{
   unsigned int nodeNum, rangeCount = 0;
   if (initParams == NULL) {
      return FALSE;
   }
   if (initParams->numNodes <= 0 || initParams->numMemRanges <= 0 ||
       initParams->numNodes > NUMA_MAX_NODES ||
       initParams->numMemRanges > (initParams->numNodes * NUMA_MAX_MEM_RANGES)) {
      return FALSE;
   }

   /* Locking for the NUMA structres, which will be shared across all VMs*/
   HostIF_GlobalLock(27);

   /* If it's greater than 0, the structures have already been initialized*/
   if (numaNumNodes > 0) {
      goto succeed;
   }
   
   numaNodes = HostIF_AllocKernelMem((sizeof(NUMA_NodeInfo) * 
                                      initParams->numNodes), TRUE);
   numaMemRangesList = HostIF_AllocKernelMem((sizeof(NUMA_MemRange) * 
                                              initParams->numMemRanges),TRUE);

   if (!numaNodes || !numaMemRangesList) {
      goto abort;
   } 

   /*
    *  Should not memcpy NUMA_NodeInfo * NUMA_MAX_NODES, since we cut down
    *  the buffer to the number of numaNodes that we actually pass down.
    */
   memcpy(numaNodes, initParams->nodes, 
          sizeof(NUMA_NodeInfo) * (initParams->numNodes));
   memset(apicToNUMANode, 0, sizeof(*apicToNUMANode) * MAX_LAPIC_ID);
   
   numaNumNodes = initParams->numNodes;
   numaNumMemRanges = initParams->numMemRanges;
   
   /*
    *  Updating the numaMemRangesList and apicTonode structures so it's faster
    *  for lookups
    */
   for (nodeNum = 0; nodeNum < numaNumNodes; nodeNum++) {
      NUMA_NodeInfo *node = &numaNodes[nodeNum];
      int range, pcpu;
      for (range = 0; range < node->numMemRanges; range++) {
         ASSERT(rangeCount < numaNumMemRanges);
         memcpy(&numaMemRangesList[rangeCount], &node->memRange[range], 
                sizeof(NUMA_MemRange));
	 rangeCount++;
      }
      for (pcpu = 0; pcpu < node->numPCPUs; pcpu++) {
         ASSERT(node->apicIDs[pcpu] < MAX_LAPIC_ID);
         apicToNUMANode[node->apicIDs[pcpu]] = node;
      }
 
   }
   ASSERT(rangeCount == numaNumMemRanges);
   ASSERT(rangeCount < NUMA_MAX_TOTAL_MEM_RANGES);
   Log("Vmx86_InitNUMAInfo : numaNumMemRanges=%d and numaNumNodes=%d\n",
       numaNumMemRanges, numaNumNodes);

succeed:
   HostIF_GlobalUnlock(27);
   return TRUE;

abort:
   Vmx86_DestroyNUMAInfo(); 
   HostIF_GlobalUnlock(27);
   return FALSE;   
}


/*
 *------------------------------------------------------------------------------
 *
 * Vmx86_DestroyNUMAInfo --
 *
 *    Destruction of the NUMA structures in the vmmon.  
 *    Will happen during unloading of the vmmon.
 *
 * Results:
 *    TRUE on success
 *
 * Side effects:
 *    None.
 *
 *------------------------------------------------------------------------------
 */

void
Vmx86_DestroyNUMAInfo(void) 
{
   if (numaNodes) {
      HostIF_FreeKernelMem(numaNodes);
   }
   if (numaMemRangesList) {
      HostIF_FreeKernelMem(numaMemRangesList);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * NUMA_MPNToNodeNum --
 *
 *      Returns the node corresponding to the given machine page.
 *
 * Results:
 *      node or INVALID_NUMANODE if the MPN is not within any
 *      node's memory ranges
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
*/

NUMA_Node
Vmx86_MPNToNodeNum(MPN mpn) //IN
{
   unsigned int i;

   for (i = 0; i < numaNumMemRanges; i++) {
      if (mpn >= numaMemRangesList[i].startMPN &&
          mpn <= numaMemRangesList[i].endMPN ) {
         return numaMemRangesList[i].id ;
      }	
   }

   return INVALID_NUMANODE;
}


/*
 *------------------------------------------------------------------------------
 *
 * Vmx86_GetNUMAMemStats --
 *
 *    Gets statistics about the current VM memory usage from the static 
 *    NUMA structures that were initialized by the VMx86_InitNUMAInfo function.
 *    Packs all the results in the VMNUMAMemStatsArgs structure and returns.
 *
 * Results:
 *    TRUE on success
 *    FALSE on failure
 *
 * Side effects:
 *    None.
 *
 *------------------------------------------------------------------------------
 */

Bool
Vmx86_GetNUMAMemStats(VMDriver *curVM,            //IN
                      VMNUMAMemStatsArgs *outArgs)//OUT: stats for curVM
{
   VMDriver *vm;
   int i;
   NUMA_NodeInfo *curNode;
   int apicId;
   
   ASSERT(curVM);
   
   apicId = HostIF_APIC_ID();
   
   if (apicId == APIC_INVALID_ID) {
      Warning("Vmx86_GetNUMAMemStats: cant read LAPIC ID\n");
      return FALSE;
   }
   curNode = apicToNUMANode[apicId];

   if (curNode == NULL) {
      Warning("Vmx86_GetNUMAMemStats: Invalid node\n");
      return FALSE;
   }

   outArgs->curNUMANode = curNode->id;

   for (i = 0; i < curNode->numPCPUs; i++) {
      if (curNode->apicIDs[i] == apicId ) {
         outArgs->curCPU = i;
      }
   }
   if (i == NUMA_MAX_CPUS_PER_NODE) {
      Warning("Processor not part of this node, structures are wrong \n");
   }
   
   vm = curVM;
   HostIF_VMLock(vm, 17);

   /* 
    * We dont have access to the VMHost structure members in this file, 
    * but we do have it in the hostif.c file. Hence, this function simply,
    * does the scanning of anonymous pages and gives the result back.
    */
   ASSERT(sizeof outArgs->numPagesPerNode == (NUMA_MAX_NODES * sizeof(uint32)));
   if (!HostIF_GetNUMAAnonPageDistribution(vm, NUMA_MAX_NODES, 
                                           outArgs->numPagesPerNode)) {
      Log("VM has no anonymous pages\n");
   }
   HostIF_VMUnlock(vm, 17);
  
   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmx86_InitVM --
 *
 *    Initializaiton of the VM.  Expects all initial arguments
 *    to be part of the InitBlock structure.
 *
 * Results:
 *    0 on success
 *    != 0 on failure
 *
 * Side effects:
 *    Many
 *
 *-----------------------------------------------------------------------------
 */

int
Vmx86_InitVM(VMDriver *vm,          // IN
             InitBlock *initParams) // IN/OUT: Initial params from the VM
{
   int retval;

   if (initParams->magicNumber != INIT_BLOCK_MAGIC) {
      Warning("Bad magic number for init block 0x%x\n", initParams->magicNumber);
      return 1;
   }
   if (initParams->numVCPUs > MAX_INITBLOCK_CPUS) {
      Warning("Too many VCPUs for init block %d\n", initParams->numVCPUs);
      return 1;
   }
   vm->numVCPUs = initParams->numVCPUs;

   HostIF_InitFP(vm);
   HostIF_InitEvent(vm);

   /*
    * Initialize the driver's part of the cross-over page used to
    * talk to the monitor
    */

   retval = Task_InitCrosspage(vm, initParams);
   if (retval) {
      Warning("Task crosspage init died with retval=%d\n", retval);
      /*
       *  Note that any clean-up of resources will be handled during
       *  power-off when Vmx86_ReleaseVM() is called as part of
       *  MonitorLoop_PowerOff(). 
       */
      return 1;
   }   

   /*
    *  Check if we want to arbitrarily fail every N VM initializations.
    *  Useful in testing PR 72482.
    */
   if (initParams->vmInitFailurePeriod != 0) {
      static uint32 counter = 0;
      if ((++counter) % initParams->vmInitFailurePeriod == 0) {
         Warning("VM initialization failed on %d iteration\n", counter);
         return 1;
      }
   }

   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_LateInitVM --
 *
 *      Do late initialization of the driver.
 *	This should be called after Vmx86_CreateVM and
 *	after all the user-level device initialization.
 *
 * Results: 
 *	non-zero on error, zero on success;
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Vmx86_LateInitVM(VMDriver *vm)
{
   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_ReadTSCAndUptime --
 *
 *      Atomically read the TSC and the uptime.
 *
 * Results:
 *      The current TSC and uptime values.
 *
 * Side effects:
 *      none
 *
 *
 *----------------------------------------------------------------------
 */

void
Vmx86_ReadTSCAndUptime(VmTimeStart *st)	// OUT: return value
{
   uintptr_t flags;

   SAVE_FLAGS(flags);
   CLEAR_INTERRUPTS();

   st->count = RDTSC();
   st->time = HostIF_ReadUptime();

   RESTORE_FLAGS(flags);
}


#ifdef __APPLE__
/*
 *----------------------------------------------------------------------
 *
 * Vmx86GetBusyKHzEstimate
 *
 *      Return an estimate the of the processor's kHz rating, based on
 *      a spinloop.  This is especially useful on systems where the TSC
 *      is known to run at its maximum rate when we are using the CPU.
 *      As of 2006, Intel Macs are this way... the TSC rate is 0 if the
 *      CPU is in a deep enough sleep state, or at its max rate otherwise.
 *
 * Results:
 *      Processor speed in kHz.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER uint32
Vmx86GetBusyKHzEstimate(void)
{
   static const int ITERS = 100;
   static const int CYCLES_PER_ITER = 20000;
   int i;
   uint64 j;
   uint64 freq;
   uint64 hz;
   uint64 aggregateCycles = 0;
   uint64 aggregateUptime = 0;

   for (i = 0; i < ITERS; i++) {
      NO_INTERRUPTS_BEGIN() {
         aggregateCycles -= RDTSC();
         aggregateUptime -= HostIF_ReadUptime();
         for (j = RDTSC() + CYCLES_PER_ITER; RDTSC() < j; )
            ;
         aggregateCycles += RDTSC();
         aggregateUptime += HostIF_ReadUptime();
      } NO_INTERRUPTS_END();
   }
   freq = HostIF_UptimeFrequency();
   while (aggregateCycles > MAX_UINT64 / freq) {
      aggregateCycles >>= 1;
      aggregateUptime >>= 1;
   }
   hz = aggregateCycles * freq / aggregateUptime;
   return (hz + 500) / 1000;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * Vmx86GetkHzEstimate
 *
 *      Return an estimate of the processor's kHz rating, based on
 *      the ratio of the cycle counter and system uptime since the
 *      driver was loaded.
 *      This function could be called (on Windows) at IRQL DISPATCH_LEVEL.
 *
 *----------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER uint32
Vmx86GetkHzEstimate(VmTimeStart *st)	// IN: start time
{
   uint64 cDiff, tDiff, freq, hz;
   uintptr_t flags;
   uint32 kHz = 0;

   SAVE_FLAGS(flags);
   CLEAR_INTERRUPTS();
   cDiff = RDTSC() - st->count;
   tDiff = HostIF_ReadUptime() - st->time;
   RESTORE_FLAGS(flags);

   if (tDiff == 0) {
      goto failure;
   }

   /*
    * Compute the CPU speed in kHz, which is cDiff / (tDiff /
    * HostIF_UptimeFrequency()) / 1000.  We need to do the computation
    * carefully to avoid overflow or undue loss of precision.  Also,
    * on Linux we can't do a 64/64=64 bit division directly, as the
    * gcc stub for that is not linked into the kernel.
    */
   freq = HostIF_UptimeFrequency();
#if defined VM_X86_64 || !defined linux
   while (cDiff > ((uint64) -1) / freq) {
      cDiff >>= 1;
      tDiff >>= 1;
   }
   hz  = (cDiff * freq) / tDiff;
   kHz = (uint32) ((hz + 500) / 1000);
#else
   {
      uint32 tmpkHz, tmp;
      /* On Linux we can't do a 64/64=64 bit division, as the gcc stub
       * for that is not linked into the kernel.  We'll assume that cDiff
       * * freq fits into 64 bits and that tDiff fits into 32 bits.  This
       * is safe given the values used on Linux.
       */
      Div643264(cDiff * freq, tDiff, &hz, &tmp);
      hz += 500;
      /*
       * If result in kHz cannot fit into 32 bits, we would get a divide
       * by zero exception.
       */
      if ((uint32)(hz >> 32) >= 1000) {
         goto failure;
      }
      Div643232(hz, 1000, &tmpkHz, &tmp);
      kHz = tmpkHz;
   }
#endif
   return kHz;

failure:
#ifdef VMW_HAS_CPU_KHZ
   /* If we have some reasonable value, use it... */
   kHz = cpu_khz;
#endif
   return kHz;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_GetkHzEstimate
 *
 *      Return an estimate of the processor's kHz rating, based on
 *      the ratio of the cycle counter and system uptime since the
 *      driver was loaded.  Or based on a spinloop.
 *
 *      This function could be called (on Windows) at IRQL DISPATCH_LEVEL.
 *
 * Results:
 *      Processor speed in kHz.
 *
 * Side effects:
 *      Result is cached.
 *
 *----------------------------------------------------------------------
 */

uint32
Vmx86_GetkHzEstimate(VmTimeStart *st)	// IN: start time
{
   static uint32 kHz; 

   /* 
    * Cache and return the first result for consistency. 
    * TSC values can be changed without notification.
    * TSC frequency can be vary too (SpeedStep, slowing clock on HALT, etc.)
    */
   if (kHz != 0) {
      return kHz;
   }

#ifdef __APPLE__
   return kHz = Vmx86GetBusyKHzEstimate();
#else
   return kHz = Vmx86GetkHzEstimate(st);
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_SetHostClockRate --
 *
 *      The monitor wants to poll for events at the given rate. If no VM
 *      is specified, then 'rate' is ignored and the last set rate is set
 *      again.
 *
 * Results:
 *      0 for success, host-specific error code for failure.
 *
 * Side effects:
 *      May increase the host timer interrupt rate, etc.
 *
 *----------------------------------------------------------------------
 */

int
Vmx86_SetHostClockRate(VMDriver *vm,  // IN: VM instance pointer
                       int rate)      // IN: rate in Hz
{
   unsigned newGlobalRate;
   VMDriver *cur;
   int retval = 0;

   if (!vm) {
      Log("Resetting last set host clock rate of %d\n", globalFastClockRate);
      HostIF_FastClockLock(0);
      retval = HostIF_SetFastClockRate(globalFastClockRate);
      HostIF_FastClockUnlock(0);
      return retval;
   }

   /* Quick test before locks are acquired. */
   if (vm->fastClockRate == rate) {
      return retval;
   }

   HostIF_FastClockLock(2);
   if (vm->fastClockRate == rate) {
      HostIF_FastClockUnlock(2);
      return retval;
   }

   /*
    * Loop through all vms to find new max rate.
    */
   newGlobalRate = rate;
   HostIF_GlobalLock(19);
   for (cur = vmDriverList; cur != NULL; cur = cur->nextDriver) {
      if (cur != vm && cur->fastClockRate > newGlobalRate) {
         newGlobalRate = cur->fastClockRate;
      }
   }
   HostIF_GlobalUnlock(19);

   if (newGlobalRate != globalFastClockRate) {
      retval = HostIF_SetFastClockRate(newGlobalRate);
      if (!retval) {
         globalFastClockRate = newGlobalRate;
      }
   }
   if (!retval) {
      vm->fastClockRate = rate;
   }
   HostIF_FastClockUnlock(2);
   
   return retval;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_MonitorPollIPI --
 *
 *      Check for VCPUs that are in the monitor and need an IPI to
 *      fire their next MonitorPoll callback.  Should be called once
 *      per fast timer interrupt if the fast timer is in use.
 *      Otherwise does not need to be called at all, as the normal
 *      timer interrupts will wake up MonitorPoll often enough.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May send IPIs.
 *
 *----------------------------------------------------------------------
 */

void
Vmx86_MonitorPollIPI(void)
{
   VMDriver *vm;
   VmAbsoluteTS pNow, expiry;
   Bool sentIPI;
   /*
    * Loop through all vms -- needs the global lock to protect vmDriverList.
    */
   HostIF_GlobalLock(21);

   pNow = Vmx86_GetPseudoTSC();
   
   for (vm = vmDriverList; vm != NULL; vm = vm->nextDriver) {
      Vcpuid v;
      VCPUSet expiredVCPUs = VCPUSet_Empty();
      for (v = 0; v < vm->numVCPUs; v++) {
         VMCrossPage *crosspage = vm->crosspage[v];
         if (!crosspage) {
            continue;  // VCPU is not initialized yet
         }
         expiry = crosspage->monitorPollExpiry;
         if (expiry && COMPARE_TS(expiry, <=, pNow)) {
            expiredVCPUs = VCPUSet_Include(expiredVCPUs, v);
         }
      }
      if (!VCPUSet_IsEmpty(expiredVCPUs)) {
         Bool didBroadcast = FALSE;
         sentIPI = HostIF_IPI(vm, expiredVCPUs, TRUE, &didBroadcast);
         if (didBroadcast) {
            // no point in doing a broadcast for more than one VM.
            break;
         }
      }
   }
   HostIF_GlobalUnlock(21);
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_GetNumVMs  --
 *
 *      Return the number of VMs.
 *
 * Results:
 *      The number of VMs.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int32
Vmx86_GetNumVMs()
{
   return vmCount;
}

int32
Vmx86_GetTotalMemUsage()
{
   VMDriver *vm;
   int totalmem = 0;

   HostIF_GlobalLock(15);
   vm = vmDriverList;

   for (vm = vmDriverList; vm != NULL; vm = vm->nextDriver) {
      /*
       * The VM lock is not strictly necessary as the vm will
       * stay on the list until we release the global lock and
       * because of order in which "admitted" and "mainMemSize"
       * are set when each VM is admitted.
       */
      if (vm->memInfo.admitted) {
          totalmem += PAGES_2_MBYTES(ROUNDUP(vm->memInfo.mainMemSize,
                                             MBYTES_2_PAGES(1)));
      }
   }
   
   HostIF_GlobalUnlock(15);
   return totalmem;
}


static INLINE unsigned
Vmx86MinAllocationFunc(unsigned paged, unsigned nonpaged, unsigned swappable,
                       unsigned memPct)
{
   swappable = MIN(swappable, paged);
   return RatioOf(memPct, swappable, 100) + (paged - swappable) + nonpaged;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86MinAllocation --
 *
 *      Computes the minimum number of pages that must be allocated to a
 *      specific vm.  The minAllocation for a vm is defined as 
 *      some percentage of guest memory plus 100% of nonpagable (overhead) 
 *      memory.  
 * 
 * Results:
 *	The minAllocation for this vm.  
 *	
 *
 * Side effects:
 *      Analyzes the vm info, requiring the vm lock.
 *
 *----------------------------------------------------------------------
 */

static INLINE unsigned
Vmx86MinAllocation(VMDriver *vm, unsigned memPct) {
   ASSERT(HostIF_VMLockIsHeld(vm));
   return Vmx86MinAllocationFunc(vm->memInfo.paged, vm->memInfo.nonpaged,
                                 vm->memInfo.mainMemSize, memPct);
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86CalculateGlobalMinAllocation --
 *
 *      Computes the sum of minimum allocations of each vm assuming a given
 *      percentage of guest memory must fit within host RAM. 
 *      
 * Results:
 *	Number of pages that must fit within host ram for a given overcommit
 *      level.
 *	
 *
 * Side effects:
 *      None. The actual minAllocations of each vm are NOT updated during
 *      this computation.
 *
 *----------------------------------------------------------------------
 */

static unsigned
Vmx86CalculateGlobalMinAllocation(unsigned memPct)
{
   VMDriver *vm;
   unsigned minAllocation = 0;
   
   ASSERT(HostIF_GlobalLockIsHeld());
   /* Pages of other vms required to fit inside the hard limit. */
   for (vm = vmDriverList; vm; vm = vm->nextDriver) {  
      HostIF_VMLock(vm, 2);
      if (vm->memInfo.admitted) {
         minAllocation += Vmx86MinAllocation(vm, memPct);
      }
      HostIF_VMUnlock(vm, 2);
   }
   return minAllocation;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86UpdateMinAllocations --
 *
 *      Updates the minimum allocation for each vm based on the global
 *      overcommitment percentage. 
 * 
 * Results:
 *      minAllocations for vms are changed.
 *	
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER void
Vmx86UpdateMinAllocations(unsigned memPct)
{
   VMDriver *vm;
   ASSERT(HostIF_GlobalLockIsHeld());
   /* Pages of other vms required to fit inside the hard limit. */
   for (vm = vmDriverList; vm; vm = vm->nextDriver) {
      HostIF_VMLock(vm, 3);
      if (vm->memInfo.admitted) {
         vm->memInfo.minAllocation = Vmx86MinAllocation(vm, memPct);
      }
      HostIF_VMUnlock(vm, 3);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_SetConfiguredLockedPagesLimit --
 *
 *      Set the user defined limit on the number of pages that can
 *      be locked.  This limit can be raised at any time but not lowered.
 *      This avoids having a user lower the limit as vms are running and
 *      inadvertently cause the vms to crash because of memory starvation.
 *
 * Results:
 *      Returns TRUE on success and FALSE on failure to set the limit
 *
 * Side effects:
 *      Hard limit may be changed.
 *
 *----------------------------------------------------------------------
 */

Bool
Vmx86_SetConfiguredLockedPagesLimit(unsigned limit)
{
   Bool retval = FALSE;

   HostIF_GlobalLock(4);
   if (limit >= lockedPageLimit.configured) {
      lockedPageLimit.configured = limit;
      retval = TRUE;
   }
   HostIF_GlobalUnlock(4);

   return retval;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_SetDynamicLockedPageLimit --
 *
 *      Set the dynamic locked page limit.  This limit is determined by
 *      authd in response to host pressure.  It can be both raised and
 *      lowered at any time.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Hard limit may be changed.
 *
 *----------------------------------------------------------------------
 */

void
Vmx86_SetDynamicLockedPagesLimit(unsigned limit)
{
   HostIF_GlobalLock(11);
   lockedPageLimit.dynamic = limit;
   HostIF_GlobalUnlock(11);
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_LockPage --
 *
 *      Lock a page.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Number of global and per-VM locked pages increased.
 *
 *----------------------------------------------------------------------
 */

MPN
Vmx86_LockPage(VMDriver *vm,		     // IN: VMDriver
	       VA64 uAddr,		     // IN: VA of the page to lock
	       Bool allowMultipleMPNsPerVA)  // IN: allow locking many pages with the same VA
{
   MPN mpn;

   /* Atomically check and reserve locked memory */
   if (!Vmx86ReserveFreePages(vm, 1)) {
      return PAGE_LOCK_LIMIT_EXCEEDED;
   }

   HostIF_VMLock(vm, 4);
   mpn = HostIF_LockPage(vm, uAddr, allowMultipleMPNsPerVA);
   HostIF_VMUnlock(vm, 4);

   if (!PAGE_LOCK_SUCCESS(mpn)) {
      Vmx86UnreserveFreePages(vm, 1);
   }

   return mpn;
}



/*
 *----------------------------------------------------------------------
 *
 * Vmx86_UnlockPage --
 *
 *      Unlock a page.
 *
 * Results:
 *      
 *
 * Side effects:
 *      Number of global and per-VM locked pages decreased.
 *
 *----------------------------------------------------------------------
 */

int
Vmx86_UnlockPage(VMDriver *vm, // IN
                 VA64 uAddr)   // IN
{
   int retval;
   
   HostIF_VMLock(vm, 5);
   retval = HostIF_UnlockPage(vm, uAddr);
   HostIF_VMUnlock(vm, 5);

   if (PAGE_LOCK_SUCCESS(retval)) {
      Vmx86UnreserveFreePages(vm, 1);
   }
   return retval;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_UnlockPageByMPN --
 *
 *      Unlock a page.
 *
 * Results:
 *      
 *
 * Side effects:
 *      Number of global and per-VM locked pages decreased.
 *
 *----------------------------------------------------------------------
 */

int
Vmx86_UnlockPageByMPN(VMDriver *vm, // IN: VMDriver
		      MPN mpn,	    // IN: the page to unlock
		      VA64 uAddr)   // IN: optional valid VA for this MPN
{
   int retval;

   HostIF_VMLock(vm, 6);
   retval = HostIF_UnlockPageByMPN(vm, mpn, uAddr);
   HostIF_VMUnlock(vm, 6);

   if (PAGE_LOCK_SUCCESS(retval)) {
      Vmx86UnreserveFreePages(vm, 1);
   }
   return retval;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmx86_AllocLockedPages --
 *
 *      Allocate physical locked pages from the kernel.
 *
 *      Initially the pages are not mapped to any user or kernel 
 *      address space.
 *
 * Results:
 *      Non-negative value on partial/full completion: actual number of
 *      allocated MPNs. MPN32s of the allocated pages are copied to the
 *      caller's buffer at 'addr'.
 *
 *	Negative system specific error code on error (NTSTATUS on Windows,
 *      etc.)
 *
 * Side effects:
 *      Number of global and per-VM locked pages is increased.
 *
 *-----------------------------------------------------------------------------
 */

int
Vmx86_AllocLockedPages(VMDriver *vm,	     // IN: VMDriver
		       VA64 addr,	     // OUT: VA of an array for
                                             //      allocated MPN32s.
		       unsigned numPages,    // IN: number of pages to allocate
		       Bool kernelMPNBuffer) // IN: is the MPN buffer in kernel or user address space?
{
   int allocatedPages;

   if (!Vmx86ReserveFreePages(vm, numPages)) {
      // XXX What kind of system-specific error code is that? --hpreg
      return PAGE_LOCK_LIMIT_EXCEEDED;
   }
   
   HostIF_VMLock(vm, 7);
   allocatedPages = HostIF_AllocLockedPages(vm, addr, numPages, kernelMPNBuffer);
   HostIF_VMUnlock(vm, 7);

   if (allocatedPages < 0) {
      Vmx86UnreserveFreePages(vm, numPages);
   } else if (allocatedPages < numPages) {
      Vmx86UnreserveFreePages(vm, numPages - allocatedPages);
   }

   return allocatedPages;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_FreeLockedPages --
 *
 *      Frees physical locked pages from the kernel previosly allocated 
 *      by Vmx86_AllocLockedPages().
 *
 * Results:
 *	0 on success,
 *	non-0 system specific error code on error (NTSTATUS on Windows, etc.)
 *
 * Side effects:
 *      Number of global and per-VM locked pages is decreased.
 *
 *----------------------------------------------------------------------
 */

int
Vmx86_FreeLockedPages(VMDriver *vm,	    // IN: VM instance pointer
		      VA64 addr,            // IN: user or kernel array of MPNs to free 
		      unsigned numPages,    // IN: number of pages to free
		      Bool kernelMPNBuffer) // IN: is the MPN buffer in kernel or user address space?
{
   int ret;

   HostIF_VMLock(vm, 8);
   ret = HostIF_FreeLockedPages(vm, addr, numPages, kernelMPNBuffer);
   HostIF_VMUnlock(vm, 8);

   if (ret == 0) {
      Vmx86UnreserveFreePages(vm, numPages);
   }

   return ret;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_IsAnonPage --
 *
 *      Queries the driver to see if the mpn is an anonymous page.
 *
 * Results:
 *      True if mpn is an anonymous page, false otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
Vmx86_IsAnonPage(VMDriver *vm,       // IN: VM instance pointer
                 const MPN32 mpn)    // IN: MPN we are asking about
{
   Bool ret;

   HostIF_VMLock(vm, 16);
   ret = HostIF_IsAnonPage(vm, mpn);
   HostIF_VMUnlock(vm, 16);
   return ret;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_GetLockedPageList --
 *
 *      puts MPNs of pages that were allocated by HostIF_AllocLockedPages()
 *      into user mode buffer.
 *
 * Results:
 *	non-negative number of the MPNs in the buffer on success.
 *	negative error code on error.
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */

int 
Vmx86_GetLockedPageList(VMDriver *vm,          // IN: VM instance pointer
                        VA64 uAddr,            // OUT: user mode buffer for MPNs
		        unsigned int numPages) // IN: size of the buffer in MPNs 
{
   int ret;
      
   HostIF_VMLock(vm, 9);
   ret = HostIF_GetLockedPageList(vm, uAddr, numPages);
   HostIF_VMUnlock(vm, 9);
   
   return ret;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86COWStats --
 *
 *       Fill structure with no data...
 *
 * Results:
 *       None.
 *
 * Side effects:
 *       None
 *
 *----------------------------------------------------------------------
 */

static void
Vmx86COWStats(VMMemCOWInfo *info)
{
   int i;

   for (i = 0; i < VMMEM_COW_HOT_PAGES; i++) {
      info->hot[i].mpn = INVALID_MPN;
      info->hot[i].ref = 0;
      info->hot[i].key = 0;
      info->hot[i].pageClass = PC_UNKNOWN;
   }
   info->numRef = 0;
   info->numHints = 0;
   info->uniqueMPNs = 0;
   info->numBreaks = 0;
   info->totalUniqueMPNs = 0;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_GetMemInfo --
 *
 *      Return the info about all VMs.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      VMGetMemInfoArgs is filled in.
 *
 *----------------------------------------------------------------------
 */

Bool
Vmx86_GetMemInfo(VMDriver *curVM,
                 Bool curVMOnly,
                 VMMemInfoArgs *outArgs,
                 int outArgsLength)
{
   VMDriver *vm;
   int i;
   int outSize;
   int wantedVMs;

   HostIF_GlobalLock(7);

   if (curVMOnly) {
      wantedVMs = 1;
   } else {
      wantedVMs = vmCount;
   }

   outSize = VM_GET_MEM_INFO_SIZE(wantedVMs);
   if (outSize > outArgsLength) {
      HostIF_GlobalUnlock(7);
      return FALSE;
   }
   
   outArgs->numVMs = wantedVMs;
   outArgs->numLockedPages = numLockedPages;
   outArgs->maxLockedPages = Vmx86LockedPageLimit(curVM);
   outArgs->lockedPageLimit = lockedPageLimit;
   outArgs->globalMinAllocation = Vmx86CalculateGlobalMinAllocation(minVmMemPct);
   outArgs->minVmMemPct = minVmMemPct;
   outArgs->callerIndex = -1;
   Vmx86COWStats(&outArgs->cowInfo);

   if (curVM != NULL) {
      if (wantedVMs == 1) {
         outArgs->memInfo[0] = curVM->memInfo;
         outArgs->callerIndex = 0;
      } else {
         vm = vmDriverList;
         i = 0;
         outArgs->callerIndex = -1;
         while (vm != NULL && i < vmCount) {
	    if (vm == curVM) {
	       outArgs->callerIndex = i;
	    }
            HostIF_VMLock(vm, 10);
            outArgs->memInfo[i] = vm->memInfo;
            HostIF_VMUnlock(vm, 10);
            i++;
	    vm = vm->nextDriver;
         }
      }
   }

   HostIF_GlobalUnlock(7);

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmx86_GetMemInfoCopy --
 *
 *      Return the information about all VMs.
 *
 *      On input, buf->numVMs indicates how much space has been allocated
 *      for the information. On output, it indicates how much space has been
 *      filled.
 *
 * Results:
 *      TRUE on success
 *      FALSE on failure
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
Vmx86_GetMemInfoCopy(VMDriver *curVM,    // IN
                     VMMemInfoArgs *buf) // IN/OUT
{
   VMDriver *vm;
   Bool ret;

   ASSERT(curVM);

   HostIF_GlobalLock(8);

   /* Now that we have the lock, we can read vmCount --hpreg */
   if (buf->numVMs < vmCount) {
      ret = FALSE;
      goto exit;
   }

   buf->numLockedPages = numLockedPages;
   buf->maxLockedPages = Vmx86LockedPageLimit(curVM);
   buf->lockedPageLimit = lockedPageLimit;
   buf->globalMinAllocation = Vmx86CalculateGlobalMinAllocation(minVmMemPct);
   buf->minVmMemPct = minVmMemPct;
   Vmx86COWStats(&buf->cowInfo);

   for (vm = vmDriverList, buf->numVMs = 0;
        vm;
        vm = vm->nextDriver, buf->numVMs++) {
      ASSERT(buf->numVMs < vmCount);
      if (vm == curVM) {
         buf->callerIndex = buf->numVMs;
      }
      HostIF_VMLock(vm, 11);
      buf->memInfo[buf->numVMs] = vm->memInfo;
      HostIF_VMUnlock(vm, 11);
   }
   ASSERT(buf->numVMs == vmCount);

   ret = TRUE;

exit:
   HostIF_GlobalUnlock(8);

   return ret;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86SetMemoryUsage --
 *
 *      Updates the paged and nonpaged memory reserved memory values for 
 *      the vm.  
 *
 * Results:
 *      If VM size is not compatible with limits, returns FALSE.  Otherwise
 *      returns TRUE. In either case, updates VM memory information.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static Bool
Vmx86SetMemoryUsage(VMDriver *curVM,       // IN/OUT
                    unsigned paged,        // IN
                    unsigned nonpaged,     // IN
                    unsigned aminVmMemPct) // IN
{
   ASSERT(HostIF_VMLockIsHeld(curVM));
   curVM->memInfo.paged         = paged;
   curVM->memInfo.nonpaged      = nonpaged;
   curVM->memInfo.minAllocation = Vmx86MinAllocation(curVM, aminVmMemPct);
   curVM->memInfo.maxAllocation = paged + nonpaged;
   return curVM->memInfo.mainMemSize > 0 && curVM->memInfo.mainMemSize <= paged;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_Admit --
 *
 *      Set the memory management information about this VM and handles
 *      admission control. We allow vm to power on if there is room for
 *      the minimum allocation for all running vms in memory.  Note that 
 *      the hard memory limit can change dynamically in windows so we
 *      don't have guarantees due to admission control.  
 *
 * Results:
 *      Returns global information about the memory state in args as well
 *      as a value indicating whether or not the virtual machine was
 *      started.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

void
Vmx86_Admit(VMDriver *curVM,     // IN
            VMMemInfoArgs *args) // IN/OUT
{
   Bool allowAdmissionCheck = FALSE;
   unsigned int globalMinAllocation;

   HostIF_GlobalLock(9);
   /*
    * Update the overcommitment level and minimums for all vms if they can 
    * fit under new minimum limit.  If they do not fit, do nothing.  And of 
    * course if existing VMs cannot fit under limit, likelihood that new VM 
    * will fit in is zero.
    */
   globalMinAllocation = Vmx86CalculateGlobalMinAllocation(args->minVmMemPct);
   if (args->memInfo->mainMemSize <= args->memInfo->paged &&
       globalMinAllocation <= Vmx86LockedPageLimitForAdmissonControl(NULL)) {
      allowAdmissionCheck = TRUE;
      minVmMemPct = args->minVmMemPct;
      Vmx86UpdateMinAllocations(args->minVmMemPct);
   }

   HostIF_VMLock(curVM, 12);

   curVM->memInfo.shares = args->memInfo->shares;
   curVM->memInfo.usedPct = 100;
   curVM->memInfo.mainMemSize = args->memInfo->mainMemSize;
   curVM->memInfo.perVMOverhead = args->memInfo->perVMOverhead;
   curVM->memInfo.pshareMgmtInfo = args->memInfo->pshareMgmtInfo;

  /*
   * Always set the allocations required for the current configuration
   * so that the user will know how bad situation really is with the
   * suggested percentage.
   */
  curVM->memInfo.admitted = FALSE;
  if (Vmx86SetMemoryUsage(curVM, args->memInfo->paged, args->memInfo->nonpaged,
                          args->minVmMemPct) &&
      allowAdmissionCheck) {
      // Preliminary admission control to put memory pressure on other VMs.
      if (globalMinAllocation + curVM->memInfo.minAllocation <= 
          Vmx86LockedPageLimitForAdmissonControl(curVM)) {
         curVM->memInfo.admitted = TRUE;
      }
   }

#if defined _WIN32
   if (curVM->memInfo.admitted) {
      unsigned int allocatedPages, nonpaged;
      signed int pages;
      MPN32* mpns;
      /* 
       * More admission control: Get enough memory for the nonpaged portion 
       * of the VM.  Drop locks for this long operation.
       * XXX Timeout?
       */
      HostIF_VMUnlock(curVM, 12);
      HostIF_GlobalUnlock(9);

#define ALLOCATE_CHUNK_SIZE 64
      allocatedPages = 0;
      nonpaged = args->memInfo->nonpaged;
      mpns = HostIF_AllocKernelMem(nonpaged * sizeof(MPN32), FALSE);
      if (mpns == NULL) {
         goto undoAdmission;
      }
      while (allocatedPages < nonpaged) {
         pages = Vmx86_AllocLockedPages(curVM,
                                        PtrToVA64(mpns + allocatedPages),
	                                MIN(ALLOCATE_CHUNK_SIZE, nonpaged - allocatedPages),
	                                TRUE);
         if (pages <= 0) {
            break;
         }
         allocatedPages += pages;
      }

      /* 
       * Free the allocated pages. 
       * XXX Do not free the pages but hand them directly to the admitted VM.
       */

      for (pages = 0; pages < allocatedPages; pages += ALLOCATE_CHUNK_SIZE) {
         Vmx86_FreeLockedPages(curVM, PtrToVA64(mpns + pages),
                               MIN(ALLOCATE_CHUNK_SIZE, allocatedPages - pages), TRUE);
      }
      HostIF_FreeKernelMem(mpns);
#undef ALLOCATE_CHUNK_SIZE

undoAdmission:
      if (allocatedPages != nonpaged) {
          curVM->memInfo.admitted = FALSE; // undo admission
      }

      HostIF_GlobalLock(9);
      HostIF_VMLock(curVM, 12);
   }
#endif

   /* Return global state to the caller. */
   args->memInfo[0] = curVM->memInfo;
   args->numVMs = vmCount;
   args->numLockedPages = numLockedPages;
   args->maxLockedPages = Vmx86LockedPageLimit(curVM);
   args->lockedPageLimit = lockedPageLimit; 
   args->globalMinAllocation = globalMinAllocation;
   HostIF_VMUnlock(curVM, 12);
   HostIF_GlobalUnlock(9);
}


Bool
Vmx86_Readmit(VMDriver *curVM, OvhdMem_Deltas *delta)
{
   unsigned globalMinAllocation, newMinAllocation;
   Bool retval = FALSE;
   int paged;
   int nonpaged;

   HostIF_GlobalLock(31);
   globalMinAllocation = Vmx86CalculateGlobalMinAllocation(minVmMemPct);
   HostIF_VMLock(curVM, 31);
   paged = curVM->memInfo.paged + delta->paged;
   nonpaged = curVM->memInfo.nonpaged + delta->nonpaged;
   if (nonpaged >= 0 && paged >= (int)curVM->memInfo.mainMemSize) {
      globalMinAllocation -= Vmx86MinAllocation(curVM, minVmMemPct);
      newMinAllocation = Vmx86MinAllocationFunc(paged, nonpaged,
                                                curVM->memInfo.mainMemSize,
                                                minVmMemPct);
      if (globalMinAllocation + newMinAllocation <= Vmx86LockedPageLimit(curVM) ||
          (delta->paged <= 0 && delta->nonpaged <= 0)) {
         retval = Vmx86SetMemoryUsage(curVM, paged, nonpaged, minVmMemPct);
      }
   }
   HostIF_VMUnlock(curVM, 31);
   HostIF_GlobalUnlock(31);
   return retval;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_UpdateMemInfo --
 *
 *      Updates information about this VM with the new data supplied in
 *      a patch.
 *
 * Results:
 *      Sets the memory usage by this vm based on its memSample data and
 *      updates page sharing stats.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

void
Vmx86_UpdateMemInfo(VMDriver *curVM, 
		    const VMMemMgmtInfoPatch *patch)
{
   HostIF_VMLock(curVM, 13);
   if (patch->usedPct <= 100) {
      curVM->memInfo.usedPct = AsPercent(patch->usedPct);
   }
   curVM->memInfo.sharedPctAvg = patch->sharedPctAvg;
   curVM->memInfo.breaksAvg = patch->breaksAvg;
   curVM->memInfo.hugePageBytes = patch->hugePageBytes;
   HostIF_VMUnlock(curVM, 13);
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_PAEEnabled --
 *
 *      Is PAE enabled?
 *
 * Results:
 *      TRUE if PAE enabled.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
Vmx86_PAEEnabled(void)
{
   uintptr_t cr4;
   GET_CR4(cr4);
   return (cr4 & CR4_PAE) != 0;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_VMXEnabled --
 *
 *      Test the VMXE bit as an easy proxy for whether VMX operation
 *      is enabled.
 *
 * Results:
 *      TRUE if the CPU supports VT and CR4.VMXE is set.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
Vmx86_VMXEnabled(void)
{
   if (VT_CapableCPU()) {
      uintptr_t cr4;
      GET_CR4(cr4);
      return (cr4 & CR4_VMXE) != 0;
   } else {
      return FALSE;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_HVEnabledCPUs --
 * 
 *      Verify that HV (VT or SVM) is enabled on all CPUs.
 *
 * Results:
 *      TRUE if HV is enabled everywhere, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
Vmx86_HVEnabledCPUs(void)
{
   /* 
    * The monitor is capable of setting HV MSRs correctly if for
    * some reason they get dropped by the host (e.g. S4 sleep).
    * So this function only needs to return true if VT MSRs were
    * enabled at some point in the past (e.g. at driver load); the
    * two variables here represent that cached check.
    *
    * This assumes that HV MSRs will never revert to an unsettable
    * state (e.g. VT lock-enabled, S4 sleep, VT lock-disabled).  No
    * such crazed buggy BIOS has ever been observed.
    */
   return hvCapable && hvEnabled;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_VTSupportedCPU --
 * 
 *   Verify that the CPU has the VT capabilities required to run the
 *   VT-enabled monitor.
 *
 * Results:
 *      TRUE if the CPU is capable of running the VT-enabled monitor, 
 *      FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Bool
Vmx86_VTSupportedCPU(void)
{
   return VT_CapableCPU() && VT_SupportedCPU();
}

/*
 *----------------------------------------------------------------------
 * Vmx86_BrokenCPUHelper --
 *
 *   Process IOCTL_VMX86_BROKEN_CPU_HELPER to Test for a broken CPU.
 *----------------------------------------------------------------------
 */
Bool
Vmx86_BrokenCPUHelper(void)
{
   CPUIDRegs regs;
   uint32 family, model, step;
   uint64 msr_val8b, msr_val17;
   uint32 microcode_version;

   __GET_CPUID(0, &regs);
   /*
    * Currently only have problems with Genuine Intel CPUs:-)
    */
   if ((regs.ebx != 0x756e6547) ||  // Genu
       (regs.ecx != 0x6c65746e) ||  // ntel
       (regs.edx != 0x49656e69)) {  // ineI
      return FALSE;
   }
   __GET_CPUID(1, &regs);
   family = CPUID_FAMILY(regs.eax);
   model = CPUID_MODEL(regs.eax);
   step = CPUID_STEPPING(regs.eax);
   if ((family != 6) || !((model == 7) || (model == 8))) { 
      /* Only have problems with P6 family PIII CPUs*/
      /* Also, we don't know if the below MSR sequence will 
       * work on future Intel processors so we only deal with
       * model 0x67x and 0x68x. 
       */
      return FALSE;
   }

   __SET_MSR(0x8b, 0); // Clear MSR 0x8b
   __GET_CPUID(1, &regs); // Issue CPUID command 1
   msr_val8b = __GET_MSR(0x8b);
   msr_val17 = __GET_MSR(0x17);

   if (!((family == 6) && (model == 8) && (step == 1))) { 
      /* Only have problems with Coppermine CPU cores. */
      return FALSE;
   }

   microcode_version = (uint32)((msr_val8b>>32) & 0x0ff);

   if (microcode_version == 0) { 
      return TRUE;
   }

   return FALSE; // it's good
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_InCompatMode --
 *
 *      See if kernel is running in compatibility mode.
 *
 * Returns:
 *      FALSE if running in full 64-bit mode.
 *      FALSE if running in legacy 32-bit mode.
 *      TRUE if running in compatibility mode.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
Vmx86_InCompatMode(void)
{
#if defined __APPLE__ && !vm_x86_64
   return Vmx86_InLongMode();
#else
   return FALSE;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_InLongMode --
 *
 *      See if kernel is running in long (64-bit or compatibility) mode.
 *
 * Returns:
 *      FALSE if running in legacy 32-bit mode.
 *      TRUE if running in full 64-bit mode.
 *      TRUE if running in compatibility mode.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
Vmx86_InLongMode(void)
{
#if defined __APPLE__ && !vm_x86_64
   uint64 efer;

   efer = __GET_MSR(MSR_EFER);
   return (efer & MSR_EFER_LME) != 0;
#else
   return vm_x86_64;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmx86LookupVMByUserIDLocked --
 *
 *      Lookup a VM by userID. The caller must hold the global lock.
 *
 * Returns:
 *      On success: Pointer to the driver's VM instance.
 *      On failure: NULL (not found).
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static VMDriver *
Vmx86LookupVMByUserIDLocked(int userID) // IN
{
   VMDriver *vm;

   ASSERT(HostIF_GlobalLockIsHeld());

   for (vm = vmDriverList; vm != NULL; vm = vm->nextDriver) {
      if (vm->userID == userID) {
         return vm;
      }
   }

   return NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmx86_LookupVMByUserID --
 *
 *      Lookup a VM by userID.
 *
 * Returns:
 *      On success: Pointer to the driver's VM instance.
 *      On failure: NULL (not found).
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

VMDriver *
Vmx86_LookupVMByUserID(int userID) // IN
{
   VMDriver *vm;

   HostIF_GlobalLock(10);
   vm = Vmx86LookupVMByUserIDLocked(userID);
   HostIF_GlobalUnlock(10);

   return vm;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_FastSuspResSetOtherFlag --
 *
 *      Sets the value of other VM's fastSuspResFlag.
 *
 * Returns:
 *      TRUE if VM was found and flag was set successfully.
 *      FALSE if VM was not found.
 *
 * Side effects:
 *      The value we set the flag to is this VM's userID.
 *
 *----------------------------------------------------------------------
 */

Bool
Vmx86_FastSuspResSetOtherFlag(VMDriver *vm,      // IN
                              int otherVmUserId) // IN
{
   VMDriver *otherVM;

   HostIF_GlobalLock(35);
   otherVM = Vmx86LookupVMByUserIDLocked(otherVmUserId);
   if (otherVM != NULL) {
      ASSERT(otherVM->fastSuspResFlag == 0);
      otherVM->fastSuspResFlag = vm->userID;
   } else {
      Warning("otherVmUserId (%d) is invalid", otherVmUserId);
   }
   HostIF_GlobalUnlock(35);
   return otherVM != NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * Vmx86_FastSuspResGetMyFlag --
 *
 *      Gets the value of fastSuspResFlag. If blockWait is true, this
 *      function will not return until the flag is non-zero, or until
 *      timeout.
 *
 * Returns:
 *      The value of the flag which, if non-zero, should be the userID of
 *      the vm that set it.
 *
 * Side effects:
 *      The flag is reset to zero once read.
 *
 *----------------------------------------------------------------------
 */

int
Vmx86_FastSuspResGetMyFlag(VMDriver *vm,   // IN
                           Bool blockWait) // IN
{
   int retval = 0;
   int ntries = 1;
   const int waitInterval = 10;     /* Wait 10ms at a time. */
   const int maxWaitTime  = 100000; /* Wait maximum of 100 seconds. */

   if (blockWait) {
      ntries = maxWaitTime / waitInterval;
   }

   while (ntries--) {
      HostIF_GlobalLock(6);
      retval = vm->fastSuspResFlag;
      vm->fastSuspResFlag = 0;
      HostIF_GlobalUnlock(6);
      if (retval || !ntries) {
         break;
      }
      HostIF_Wait(waitInterval);
   }
   return retval;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmx86GetSVMEnableOnCPU --
 *
 *      Check SVM_CapableCPU and MSR-specific enablement bits, 
 *      and set map-reduce flags.  The MSR-specific bits are in
 *      case the BIOS failed to set the MSRs correctly.
 *
 * Results:
 *      '*data' is filled.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static void
Vmx86GetSVMEnableOnCPU(void *clientData) // IN/OUT: A HVEnableData *
{
   HVEnableData *data = (HVEnableData *)clientData;
   Bool svmCapable = SVM_CapableCPU();
   ASSERT(data);

   if (svmCapable) {
      uint64 featctl = __GET_MSR(MSR_VM_CR);
      /* 
       * If we've seen an SVM-enabled CPU before, try to enable any
       * disabled but unlocked CPUs.
       */
      if ((featctl & (MSR_VM_CR_SVM_LOCK | MSR_VM_CR_SVME_DISABLE)) ==
          MSR_VM_CR_SVME_DISABLE) {
         if (data->hvForce) {
            __SET_MSR(MSR_VM_CR, (featctl & ~MSR_VM_CR_SVME_DISABLE));
            ASSERT(SVM_EnabledCPU());
            featctl = __GET_MSR(MSR_VM_CR);
         }
      }
      if ((featctl & MSR_VM_CR_SVM_LOCK) == 0) {
         data->anyUnlocked = TRUE;
      }
      if ((featctl & MSR_VM_CR_SVME_DISABLE) != 0) {
         data->anyDisabled = TRUE;
      } else {
         ASSERT(SVM_EnabledCPU());
         data->anyEnabled = TRUE;
      }
   } else {
      data->anyNotCapable = TRUE;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmx86GetVTEnableOnCPU --
 *
 *      Check VT_CapableCPU and MSR-specific enablement bits, 
 *      and set map-reduce flags.  The MSR-specific bits are in
 *      case the BIOS failed to set the MSRs correctly.
 *
 * Results:
 *      *data' is filled.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static void
Vmx86GetVTEnableOnCPU(void *clientData) // IN/OUT: A HVEnableData *
{
   HVEnableData *data = (HVEnableData *)clientData;
   Bool vtCapable = VT_CapableCPU();
   ASSERT(data);

   if (vtCapable) {
      uint64 featctl = __GET_MSR(MSR_FEATCTL);
      /* 
       * If we've seen a VT-enabled CPU before, try to enable any
       * unlocked CPUs.
       */
      if ((featctl & (MSR_FEATCTL_LOCK)) == 0) {
         if (data->hvForce) {
            __SET_MSR(MSR_FEATCTL, featctl | MSR_FEATCTL_LOCK | MSR_FEATCTL_VMXE);
            ASSERT(VT_EnabledCPU());
            featctl = __GET_MSR(MSR_FEATCTL);
         }
      }
      if ((featctl & MSR_FEATCTL_LOCK) == 0) {
         data->anyUnlocked = TRUE;
      } else {
         if ((featctl & MSR_FEATCTL_VMXE) == 0) {
            data->anyDisabled = TRUE;
         } else {
            ASSERT(VT_EnabledCPU());
            data->anyEnabled = TRUE;
         }
      }
   } else {
      data->anyNotCapable = TRUE;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmx86_FixHVEnable --
 *
 *      Force and cache hvCapable / hvEnabled for all CPUs.
 *
 *      Some BIOSes mis-enable the HV MSR for non-boot CPUs or after S3.
 *      So cache the HV-enabled state here, and always try to re-achieve
 *      it at future queries.
 *
 *      Enabling HV itself can be construed as a security hole ("blue pill"),
 *      even though such attacks are implausible.  By respecting the
 *      boot-time HV state, we ensure that the opening is no larger
 *      than it was at boot.  Users can set hv.enableIfUnlocked in the 
 *      system config file to force HV enablement if the BIOS has not 
 *      locked it.
 *
 * Results:
 *      TRUE if all CPUs are hvEnabled.
 *      FALSE if any CPU cannot be hvEnabled.
 *
 * Side effects:
 *      May change MSR to enable HV (VT / SVM).
 *
 *-----------------------------------------------------------------------------
 */

void
Vmx86_FixHVEnable(Bool force)
{
   static Bool once = FALSE;
   static Bool forceLatch = FALSE;
   static void (*perCPUFunc)(void*);

   /* 
    * Check HV capabilities and cache "force" at boot. 
    * Callers of Vmx86_HVEnabledCPUs can set also "force".
    */
   if (!once) {
      /* Potential race, but idempotent so don't care. */
      if (VT_CapableCPU()) {
         perCPUFunc = Vmx86GetVTEnableOnCPU;
      } else if (SVM_CapableCPU()) {
         perCPUFunc = Vmx86GetSVMEnableOnCPU;
      } else {
         hvCapable = FALSE;
      }
      if (perCPUFunc) {
         HVEnableData data = { FALSE, FALSE, FALSE, FALSE };
         HostIF_CallOnEachCPU(*perCPUFunc, &data);
         Log("Initial HV check: anyNotCapable=%d anyUnlocked=%d anyEnabled=%d anyDisabled=%d\n",
             data.anyNotCapable, data.anyUnlocked,
             data.anyEnabled, data.anyDisabled);
         ASSERT(data.anyNotCapable || data.anyUnlocked || 
                data.anyEnabled || data.anyDisabled);
         hvCapable = !data.anyNotCapable;
         /* 
          * If all CPUs are HV-capable and any CPU is HV-enabled, the user 
          * is OK with HV, and we'll force-enable unlocked CPUs. 
          */
         forceLatch = data.anyEnabled;
      }
      once = TRUE;
   }
   if (force) {
      forceLatch = TRUE;
   }
   
   /* Propogate VT-enablement to all CPUs. */
   if (hvCapable) {
      HVEnableData data = { FALSE, FALSE, FALSE };
      data.hvForce = forceLatch;
      ASSERT(perCPUFunc);
      HostIF_CallOnEachCPU(*perCPUFunc, &data);
      /* If CPUID said HV-capable before, must still be true. */
      ASSERT(!data.anyNotCapable);

      /* VT requires enabled and locked.  SVM only requires enabled. */
      if (VT_CapableCPU()) {
         hvEnabled = !(data.anyDisabled || data.anyUnlocked);
      } else if (SVM_CapableCPU()) {
         hvEnabled = !data.anyDisabled;
      }
      Log("HV check: anyNotCapable=%d anyUnlocked=%d anyEnabled=%d anyDisabled=%d\n",
          data.anyNotCapable, data.anyUnlocked,
          data.anyEnabled, data.anyDisabled);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmx86RefClockToPTSC --
 *
 *    Convert from the reference clock (HostIF_Uptime) time to pseudo TSC.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE uint64
Vmx86RefClockToPTSC(uint64 uptime)
{
   return RateConv_Unsigned(&pseudoTSC.refClockToTSC, uptime);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmx86_InitPseudoTSC --
 *
 *      Initialize the pseudo TSC state if it is not already initialized.
 *      If another vmx has initialized the pseudo TSC, then we continue to
 *      use the parameters specified by the first vmx.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      - Completes initialization of refClkToTSC parameters by
 *        setting refClkToTSC->add. Currently, the updated refClkToTSC
 *        is used only on the Mac to perform user-level refClock to TSC
 *        conversion.
 *      - Updates tscHz, the frequency of the PTSC in Hz. That frequency may
 *        differ from the value passed in if another VM is already running.
 *
 *-----------------------------------------------------------------------------
 */

void
Vmx86_InitPseudoTSC(Bool forceRefClock,           // IN: always use the ref clock as the basis
                    Bool forceTSC,                // IN: never use the ref clock as the basis
                    RateConv_Params *refClkToTSC, // IN/OUT: conversion from ref clock to TSC
                    uint64 *tscHz)                // IN/OUT: TSC frequency in Hz
{
   VmTimeStart startTime;
   uint64 tsc, uptime;

   HostIF_GlobalLock(36);

   if (!pseudoTSC.initialized) {
      pseudoTSC.hz = *tscHz;
      pseudoTSC.refClockToTSC.mult = refClkToTSC->mult;
      pseudoTSC.refClockToTSC.shift = refClkToTSC->shift;
      pseudoTSC.refClockToTSC.add = 0;

      Vmx86_ReadTSCAndUptime(&startTime);
      tsc    = startTime.count;
      uptime = startTime.time;

      pseudoTSC.refClockToTSC.add += tsc - Vmx86RefClockToPTSC(uptime);

      pseudoTSC.useRefClock = forceRefClock;
      pseudoTSC.neverSwitchToRefClock = forceTSC; // forceRefClock gets priority.
      Log("PTSC: initialized at %"FMT64"u Hz using %s\n",
          pseudoTSC.hz, pseudoTSC.useRefClock ? "reference clock" : "TSC");

      pseudoTSC.initialized = TRUE;
   }
   ASSERT(refClkToTSC->add == 0); /* The caller needs us to initialize .add */
   refClkToTSC->add = pseudoTSC.refClockToTSC.add;
   *tscHz = pseudoTSC.hz;

   HostIF_GlobalUnlock(36);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmx86_GetPseudoTSC --
 *
 *    Read the pseudo TSC.  We prefer to implement the pseudo TSC using
 *    TSC.  On systems where the TSC varies its rate (e.g. Pentium M),
 *    stops advancing when the core is in deep sleep (e.g. Core 2 Duo),
 *    or the TSCs can get out of sync across cores (e.g. Opteron due to
 *    halt clock ramping, Core 2 Duo due to independent core deep sleep
 *    states; though WinXP does handle the Core 2 Duo out of sync case;
 *    and on IBM x-Series NUMA machines), we use a reference clock
 *    (HostIF_ReadUptime()) as the basis for pseudo TSC.
 *      
 *    Note that we depend on HostIF_ReadUptime being a high resolution
 *    timer that is synchronized across all cores.
 * 
 * Results:
 *    Current value of the PTSC.
 *
 *-----------------------------------------------------------------------------
 */

uint64
Vmx86_GetPseudoTSC(void)
{
   if (Vmx86_PseudoTSCUsesRefClock()) {
      return Vmx86RefClockToPTSC(HostIF_ReadUptime());
   }
   return RDTSC();
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmx86_CheckPseudoTSC --
 *
 *    Periodically called by userspace to check whether the TSC is
 *    reliable, using the reference clock as the trusted time source.
 *    If the TSC is unreliable, switch the basis of the PTSC from the
 *    TSC to the reference clock.
 *
 *    Note that we might be executing concurrently with other threads,
 *    but it doesn't matter since we only ever go from using the TSC to
 *    using the reference clock, never the other direction.
 *
 * Results: 
 *    TRUE if the PTSC is implemented by the reference clock.
 *    FALSE if the PTSC is implemented by the TSC.
 *
 * Side effects: 
 *    May switch the basis of the PTSC from the TSC to the reference clock.
 *
 *-----------------------------------------------------------------------------
 */

Bool
Vmx86_CheckPseudoTSC(uint64 *lastTSC, // IN/OUT: last/current value of the TSC
                     uint64 *lastRC)  // IN/OUT: last/current value of the reference clock
{
   VmTimeStart curTime;
   uint64 tscDiff, ptscDiff;
   
   Vmx86_ReadTSCAndUptime(&curTime);
   
   if (pseudoTSC.initialized && *lastTSC && 
       !Vmx86_PseudoTSCUsesRefClock()) {

      tscDiff = curTime.count - *lastTSC;
      ptscDiff = Vmx86RefClockToPTSC(curTime.time) - 
         Vmx86RefClockToPTSC(*lastRC);
      
      if (((int64)tscDiff < 0) ||
          (tscDiff * 100 < ptscDiff * 95) ||
          (tscDiff * 95 > ptscDiff * 100)) {
         /* 
          * TSC went backwards or drifted from the reference clock by
          * more than 5% over the last poll period.
          */
         Vmx86_SetPseudoTSCUseRefClock();
      }
   }
   *lastTSC = curTime.count;
   *lastRC  = curTime.time;
   return Vmx86_PseudoTSCUsesRefClock();
}


typedef struct {
   Atomic_uint32 index;
   MSRQuery *query;
} Vmx86GetMSRData;


/*
 *-----------------------------------------------------------------------------
 *
 * Vmx86GetMSR --
 *
 *      Collect MSR value on the current logical CPU.
 *
 *	Function must not block (it is invoked from interrupt context).
 *      Only VT MSRs are supported on VT-capable processors.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      'data->index' is atomically incremented by one.
 *
 *-----------------------------------------------------------------------------
 */

static void
Vmx86GetMSR(void *clientData) // IN/OUT: A Vmx86GetMSRData *
{
   Vmx86GetMSRData *data = (Vmx86GetMSRData *)clientData;
   MSRQuery *query;
   uint32 index;
   int err;

   ASSERT(data);
   query = data->query;
   ASSERT(query);

   index = Atomic_ReadInc32(&data->index);
   if (index >= query->numLogicalCPUs) {
      return;
   }

   query->logicalCPUs[index].tag = HostIF_GetCurrentPCPU();

   /*
    * We treat BIOS_SIGN_ID (microcode version) specially on Intel,
    * where the preferred read sequence involves a macro.
    */
   if (CPUID_GetVendor() == CPUID_VENDOR_INTEL &&
       query->msrNum == MSR_BIOS_SIGN_ID) {
      /* safe to read: MSR_BIOS_SIGN_ID architectural since Pentium Pro */
      query->logicalCPUs[index].msrVal = INTEL_MICROCODE_VERSION();
      err = 0;
   } else {
      err = HostIF_SafeRDMSR(query->msrNum, &query->logicalCPUs[index].msrVal);
   }

   query->logicalCPUs[index].implemented = (err == 0) ? 1 : 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmx86_GetAllMSRs --
 *
 *      Collect MSR value on all logical CPUs.
 *
 *      The caller is responsible for ensuring that the requested MSR is valid
 *      on all logical CPUs.
 *
 *      'query->numLogicalCPUs' is the size of the 'query->logicalCPUs' output
 *      array.
 *
 * Results:
 *      On success: TRUE. 'query->logicalCPUs' is filled and
 *                  'query->numLogicalCPUs' is adjusted accordingly.
 *      On failure: FALSE. Happens if 'query->numLogicalCPUs' was too small.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
Vmx86_GetAllMSRs(MSRQuery *query) // IN/OUT
{
   Vmx86GetMSRData data;

   Atomic_Write32(&data.index, 0);
   data.query = query;

   HostIF_CallOnEachCPU(Vmx86GetMSR, &data);

   /*
    * At this point, Atomic_Read32(&data.index) is the number of logical CPUs
    * who replied.
    */
   if (Atomic_Read32(&data.index) > query->numLogicalCPUs) {
      return FALSE;
   }

   ASSERT(Atomic_Read32(&data.index) <= query->numLogicalCPUs);
   query->numLogicalCPUs = Atomic_Read32(&data.index);
   return TRUE;
}
