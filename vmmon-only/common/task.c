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
 * task.c --
 *
 *      Task initialization and switching routines between the host
 *      and the monitor.
 *
 *      A task switch:
 *          -saves the EFLAGS,CR0,CR2,CR4, and IDT
 *          -jumps to code on the shared page
 *              which saves the registers, GDT and CR3
 *              which then restores the registers, GDT and CR3
 *          -restores the IDT,CR0,CR2,CR4 and EFLAGS
 *
 *      This file is pretty much independent of the host OS.
 *
 */

#ifdef linux
/* Must come before any kernel header file --hpreg */
#   include "driver-config.h"
#   include <linux/string.h> /* memset() in the kernel */

#   define EXPORT_SYMTAB
#else
#   include <string.h>
#endif

#include "vmware.h"
#include "modulecall.h"
#include "vmx86.h"
#include "task.h"
#include "vm_asm.h"
#include "cpuid.h"
#include "hostif.h"
/* On Linux, must come before any inclusion of asm/page.h --hpreg */
#include "hostKernel.h"
#include "comport.h"
#include "crossgdt.h"
#include "x86vt.h"

#if defined(_WIN64)
#   include "x86.h"
#   include "vmmon-asm-x86-64.h"
#   define USE_TEMPORARY_GDT 1
#else
/*
 * It is OK to set this to 1 on 64-bit Linux for testing.  It
 * won't work on 32-bit Mac OS running in compatibility mode, or 64-bit Mac OS
 * because our code does not handle that case.
 */
#   define USE_TEMPORARY_GDT 0
#endif

#define TS_ASSERT(t) do { \
   DEBUG_ONLY(if (!(t)) TaskAssertFail(__LINE__);)  \
} while (0)

#define TEMPGDT_SIZE 0x10000U /* maximal GDT size */

static CrossGDT *crossGDT = NULL;
static MPN crossGDTMPNs[CROSSGDT_NUMPAGES];
static DTR crossGDTDescHKLA;
static Selector kernelStackSegment = 0;
static uint32 dummyLVT;
static uint32 tempGDTCount = 0;
static Descriptor **tempGDT = NULL;
static Atomic_uint32 dummyVMCS[MAX_DUMMY_VMCSES];
static Atomic_uint32 rootVMCS[MAX_LAPIC_ID];

#if defined __APPLE__ && !vm_x86_64
/* #include <i386/seg.h> can't find mach_kdb.h. */
#   define KERNEL32_CS MAKE_SELECTOR_UNCHECKED(1, 0, 0)
#   define KERNEL32_DS MAKE_SELECTOR_UNCHECKED(2, 0, 0)
#   define KERNEL64_CS MAKE_SELECTOR_UNCHECKED(16, 0, 0)

static Bool inCompatMode;
static Bool inLongMode;

#   define TASK_IF_COMPAT_MODE_THEN_ELSE(_thenBlock, _elseBlock)   \
   if (inCompatMode) {                                             \
      _thenBlock                                                   \
   } else {                                                        \
      _elseBlock                                                   \
   }
#else
#   define TASK_IF_COMPAT_MODE_THEN_ELSE(_thenBlock, _elseBlock) { \
      _elseBlock                                                   \
   }
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * TaskInCompatMode --
 *
 *      Determine whether host is running in compatibility mode.
 *
 * Result:
 *      TRUE iff running in compatibility mode.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE Bool
TaskInCompatMode(void)
{
#if defined __APPLE__ && !vm_x86_64
   return inCompatMode;
#else
   return FALSE;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskInLongMode --
 *
 *      Determine whether host is running in long mode.
 *
 * Result:
 *      TRUE iff running in long mode.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE Bool
TaskInLongMode(void)
{
#if defined __APPLE__ && !vm_x86_64
   return inLongMode;
#else
   return vm_x86_64;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskAllocVMCS --
 *
 *      Allocate and initialize a VMCS page. Upon success, race to be the first
 *      to store its MPN in '*slot'.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      When the call returns, '*slot' contains the MPN of a VMCS page if a
 *      thread succeeded, or INVALID_MPN if all threads failed.
 *
 *-----------------------------------------------------------------------------
 */

static void
TaskAllocVMCS(Atomic_uint32 *slot) // IN/OUT
{
   uint32 *content = NULL;
   uint64 vmxMsr;
   MPN mpn = INVALID_MPN;

   ASSERT(slot);

   /* Allocate the VMCS page content. */
   content = HostIF_AllocKernelMem(PAGE_SIZE, TRUE);
   if (!content) {
      Warning("%s: Failed to allocate content.\n", __FUNCTION__);
      goto exit;
   }

   /*
    * Write the VMCS revision identifier at the beginning of the VMCS page
    * content. In theory, nobody should read the random bytes after that.
    * But we are paranoid, so we set them to a deterministic value.
    */
   memset(content, 0, PAGE_SIZE);
   if (HostIF_SafeRDMSR(MSR_VMX_BASIC, &vmxMsr)) {
      Warning("%s: Failed to read MSR.\n", __FUNCTION__);
      goto exit;
   }
   *content = LODWORD(vmxMsr);

   /* Allocate the VMCS page. */
   mpn = HostIF_AllocMachinePage();
   if (mpn == INVALID_MPN) {
      Warning("%s: Failed to allocate page.\n", __FUNCTION__);
      goto exit;
   }

   /* Copy the VMCS page content to the VMCS page. */
   if (HostIF_WritePage(mpn, PtrToVA64(content), TRUE)) {
      Warning("%s: Failed to copy content.\n", __FUNCTION__);
      goto exit;
   }

   /*
    * Store the MPN of the VMCS page. This is done atomically, so if several
    * threads concurrently race and call TaskAllocVMCS() with the same 'slot',
    * only the first one to pass this finish line will win.
    */
   if (!Atomic_CMPXCHG32(slot, INVALID_MPN, mpn)) {
      /* This thread lost the race. It must free its VMCS page. */
      goto exit;
   }

   /* This thread won the race. It must not free its VMCS page. */
   mpn = INVALID_MPN;

exit:
   if (mpn != INVALID_MPN) {
      HostIF_FreeMachinePage(mpn);
   }

   if (content) {
      HostIF_FreeKernelMem(content);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskGetVMCS --
 *
 *      Lazily allocate a VMCS page, and return its MPN.
 *
 * Results:
 *      On success: The MPN of the VMCS page.
 *      On failure: INVALID_MPN.
 *
 * Side effects:
 *      Might allocate memory, and transition '*slot' from
 *      INVALID_MPN to a valid MPN.
 *
 *-----------------------------------------------------------------------------
 */

static MPN
TaskGetVMCS(Atomic_uint32 *slot) // IN/OUT
{
   MPN mpn = Atomic_Read32(slot);

   if (mpn != INVALID_MPN) {
      return mpn;
   }

   TaskAllocVMCS(slot);

   return Atomic_Read32(slot);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Task_GetDummyVMCS --
 *
 *      Lazily allocate a dummy VMCS page, and return its MPN.
 *
 * Results:
 *      On success: The MPN of the dummy VMCS page.
 *      On failure: INVALID_MPN.
 *
 * Side effects:
 *      Might allocate memory, and transition 'dummyVMCS[vmcsId]' from
 *      INVALID_MPN to a valid MPN.
 *
 *-----------------------------------------------------------------------------
 */

MPN
Task_GetDummyVMCS(int vmcsId) // IN
{
   ASSERT(vmcsId < ARRAYSIZE(dummyVMCS));
   return TaskGetVMCS(&dummyVMCS[vmcsId]);
}


/*
 *-----------------------------------------------------------------------------
 *
 * Task_GetRootVMCS --
 *
 *      Lazily allocate the root VMCS page for a pCPU, and return its MPN.
 *
 * Results:
 *      On success: The MPN of the root VMCS page.
 *      On failure: INVALID_MPN.
 *
 * Side effects:
 *      Might allocate memory, and transition 'rootVMCS[pCPU]' from
 *      INVALID_MPN to a valid MPN.
 *
 *-----------------------------------------------------------------------------
 */

MPN
Task_GetRootVMCS(uint32 pCPU) // IN
{
   ASSERT(pCPU < ARRAYSIZE(rootVMCS));
   return TaskGetVMCS(&rootVMCS[pCPU]);
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskCheckVMXEPerCPU --
 *
 *      Check for Intel VT VMX operation on the current logical CPU.
 *
 *	Function must not block (it is invoked from interrupt context).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static void
TaskCheckVMXEPerCPU(void *data) // IN/OUT: Pointer to atomic counter
{
   Atomic_uint32 *vmxeBitCount = (Atomic_uint32 *)data;

   if (Vmx86_VMXEnabled()) {
      Atomic_Add(vmxeBitCount, 1);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Task_IsVMXDisabledOnAllCPUs --
 *
 *      Check for Intel VT VMX operation on all CPUs.
 *
 * Results:
 *      TRUE if none of CPUs in VMX operation, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

Bool
Task_IsVMXDisabledOnAllCPUs(void)
{
   Atomic_uint32 vmxeBitCount;

   Atomic_Write(&vmxeBitCount, 0);
   HostIF_CallOnEachCPU(TaskCheckVMXEPerCPU, &vmxeBitCount);
   return Atomic_Read(&vmxeBitCount) == 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Task_FreeVMCS --
 *
 *      Free all VMCS pages (allocated by TaskAllocVMCS), if any.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
Task_FreeVMCS(void)
{
   MPN mpn;
   unsigned i;

   for (i = 0; i < ARRAYSIZE(dummyVMCS); i++) {
      mpn = Atomic_Read32(&dummyVMCS[i]);
      if (mpn != INVALID_MPN) {
         HostIF_FreeMachinePage(mpn);
      }
   }
   for (i = 0; i < ARRAYSIZE(rootVMCS); i++) {
      mpn = Atomic_Read32(&rootVMCS[i]);
      if (mpn != INVALID_MPN) {
         HostIF_FreeMachinePage(mpn);
      }
   }
}


#ifdef VMX86_DEBUG
/*
 *-----------------------------------------------------------------------------
 *
 * TaskAssertFail --
 *
 *      Output line number to comport and crash.
 *
 *-----------------------------------------------------------------------------
 */

static void
TaskAssertFail(int line)
{
   CP_PutStr("TaskAssertFail*: ");
   CP_PutDec(line);
   CP_PutCrLf();
   SET_CR3(0);
}


#endif
/*
 *-----------------------------------------------------------------------------
 *
 * TaskSaveGDT64 --
 *
 *      Save the current GDT in the caller-supplied struct.
 *
 * Results:
 *      *hostGDT64 = copy of the processor's GDT.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
TaskSaveGDT64(DTR64 *hostGDT64)  // OUT
{
   TASK_IF_COMPAT_MODE_THEN_ELSE({
      __asm__ __volatile__("lcall %1,$TaskCM_SaveGDT64"
                           :
                           : "a" (hostGDT64),
                             "i" (KERNEL64_CS)
                           : "memory");
   }, {
      hostGDT64->offset = 0;
      _Get_GDT((DTR *)hostGDT64);
   })
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskSaveIDT64 --
 *
 *      Save the current IDT in the caller-supplied struct.
 *
 * Results:
 *      *hostIDT64 = copy of the processor's IDT.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
TaskSaveIDT64(DTR64 *hostIDT64)  // OUT
{
   TASK_IF_COMPAT_MODE_THEN_ELSE({
      __asm__ __volatile__("lcall %1,$TaskCM_SaveIDT64"
                           :
                           : "a" (hostIDT64),
                             "i" (KERNEL64_CS)
                           : "memory");
   }, {
      hostIDT64->offset = 0;
      _Get_IDT((DTR *)hostIDT64);
   })
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskLoadGDT64 --
 *
 *      Load the current GDT from the caller-supplied struct.
 *
 * Results:
 *      Processor's GDT = *hostGDT64.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
TaskLoadGDT64(DTR64 *hostGDT64)  // IN
{
   TASK_IF_COMPAT_MODE_THEN_ELSE({
      __asm__ __volatile__("lcall %1,$TaskCM_LoadGDT64"
                           :
                           : "a" (hostGDT64),
                             "i" (KERNEL64_CS)
                           : "memory");
   }, {
      _Set_GDT((DTR *)hostGDT64);
   })
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskLoadIDT64 --
 *
 *      Load the current IDT from the caller-supplied struct.
 *
 * Results:
 *      Processor's IDT = *hostIDT64.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
TaskLoadIDT64(DTR64 *hostIDT64)  // IN
{
   TASK_IF_COMPAT_MODE_THEN_ELSE({
      __asm__ __volatile__("lcall %1,$TaskCM_LoadIDT64"
                           :
                           : "a" (hostIDT64),
                             "i" (KERNEL64_CS)
                           : "memory");
   }, {
      _Set_IDT((DTR *)hostIDT64);
   })
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskLoadTR64 --
 *
 *      Load the current TR, switching to 64-bit mode from compatibility mode
 *      so we get the full 64-bit base, if necessary.
 *
 * Results:
 *      Processor's TR = hostTR
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
TaskLoadTR64(Selector hostTR)  // IN
{
   TASK_IF_COMPAT_MODE_THEN_ELSE({
      __asm__ __volatile__("lcall %1,$TaskCM_LoadTR64"
                           :
                           : "a" (hostTR),
                             "i" (KERNEL64_CS)
                           : "memory");
   }, {
      SET_TR(hostTR);
   })
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskLoadLDT64 --
 *
 *      Load the current LDT, switching to 64-bit mode from compatibility mode
 *      so we get the full 64-bit base, if necessary.
 *
 * Results:
 *      Processor's LDT = hostLDT
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
TaskLoadLDT64(Selector hostLDT)  // IN
{
   TASK_IF_COMPAT_MODE_THEN_ELSE({
      __asm__ __volatile__("lcall %1,$TaskCM_LoadLDT64"
                           :
                           : "a" (hostLDT),
                             "i" (KERNEL64_CS)
                           : "memory");
   }, {
      SET_LDT(hostLDT);
   })
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskCopyGDT64 --
 *
 *      Copy the given GDT contents to the caller-supplied buffer.
 *
 *      This routine assumes the caller has already verified there is enough
 *      room in the output buffer.
 *
 * Results:
 *      *out = copy of the processor's GDT contents.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
TaskCopyGDT64(DTR64 *hostGDT64,  // IN  GDT to be copied from
              Descriptor *out)   // OUT where to copy contents to
{
   TASK_IF_COMPAT_MODE_THEN_ELSE({
      uint32 ediGetsWiped;

      __asm__ __volatile__("lcall %3,$TaskCM_CopyGDT64"
                           : "=D" (ediGetsWiped)
                           : "0" (out),
                             "d" (hostGDT64),
                             "i" (KERNEL64_CS)
                           : "ecx", "esi", "cc", "memory");
   }, {
      memcpy(out,
             (void *)HOST_KERNEL_LA_2_VA((LA)hostGDT64->offset),
             hostGDT64->limit + 1);
   })
}


/*
 *-----------------------------------------------------------------------------
 *
 * Task_Terminate --
 *
 *      Called at driver unload time.  Undoes whatever Task_Initialize did.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Release temporary GDT memory.
 *
 *-----------------------------------------------------------------------------
 */

void
Task_Terminate(void)
{
   if (crossGDT != NULL) {
      HostIF_FreeCrossGDT(CROSSGDT_NUMPAGES, crossGDT);
      crossGDT = NULL;
      crossGDTDescHKLA.limit  = 0;
      crossGDTDescHKLA.offset = 0;
   }

   if (USE_TEMPORARY_GDT) {
      unsigned i;

      for (i = 0; i < tempGDTCount; ++i) {
         HostIF_FreeKernelMem(tempGDT[i]);
      }
      HostIF_FreeKernelMem(tempGDT);
      tempGDTCount = 0;
      tempGDT = NULL;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Task_Initialize --
 *
 *      Called at driver load time to initialize module's static data.
 *
 * Results:
 *      TRUE iff initialization successful.
 *
 * Side effects:
 *      Temporary GDT memory allocated and initialized.
 *
 *-----------------------------------------------------------------------------
 */

Bool
Task_Initialize(void)
{
   unsigned i;

   ASSERT_ON_COMPILE(sizeof (Atomic_uint32) == sizeof (MPN));
   for (i = 0; i < ARRAYSIZE(dummyVMCS); i++) {
      Atomic_Write32(&dummyVMCS[i], INVALID_MPN);
   }
   for (i = 0; i < ARRAYSIZE(rootVMCS); i++) {
      Atomic_Write32(&rootVMCS[i], INVALID_MPN);
   }

#if defined __APPLE__ && !vm_x86_64
   inCompatMode = Vmx86_InCompatMode();
   inLongMode   = Vmx86_InLongMode();
#endif

   /*
    * The worldswitch code doesn't work with a zero stack segment
    * because it temporarily restores the data segments to the stack
    * segment.  So here we make sure we have a non-zero kernel
    * read/write flat data segment.
    */
#if defined __APPLE__ && !vm_x86_64
   kernelStackSegment = KERNEL32_DS;
#else
   kernelStackSegment = GET_SS();
   if (vm_x86_64 && (kernelStackSegment == 0)) {
      DTR hostGDTR;

      GET_GDT(hostGDTR);
      for (kernelStackSegment = 8;
           kernelStackSegment + 7 <= hostGDTR.limit;
           kernelStackSegment += 8) {
         uint64 gdte = *(uint64 *)(hostGDTR.offset + kernelStackSegment);

         if ((gdte & 0xFFCFFEFFFFFFFFFFULL) == 0x00CF92000000FFFFULL) {
            goto gotnzss;
         }
      }
      Warning("Task_Initialize: no non-null flat kernel data GDT segment\n");
      return FALSE;
gotnzss:;
   }
#endif
   if ((kernelStackSegment == 0) || ((kernelStackSegment & 7) != 0)) {
           Warning("Task_Initialize: unsupported SS %04x\n", 
                   kernelStackSegment);
         return FALSE;
   }
 
   if (USE_TEMPORARY_GDT) {
#if defined __APPLE__
      /*
       * USE_TEMPORARY_GDT cannot be enabled for testing on 64-bit Mac OS
       * because of this call to HostIF_NumOnlineLogicalCPUs().
       */
      const unsigned cpus        = 0;
#else
      const unsigned cpus        = HostIF_NumOnlineLogicalCPUs();
#endif
      const unsigned numPtrBytes = cpus * sizeof(Descriptor *);
      unsigned       i;

      /* Some linux kernels panic when allocating > 128Kb */
      ASSERT(numPtrBytes <= 131072);
      tempGDT = HostIF_AllocKernelMem(numPtrBytes, TRUE);
      if (tempGDT != NULL) {
         for (i = 0; i < cpus; ++i) {
            tempGDT[i] = HostIF_AllocKernelMem(TEMPGDT_SIZE, TRUE);
            if (tempGDT[i] == NULL) {
               int j;
               Warning("Task_Initialize: Unable to allocate space "
                       "for temporary GDT[%u]", i);
               for (j = 0; j < i; ++j) {
                  HostIF_FreeKernelMem(tempGDT[j]);
               }
               HostIF_FreeKernelMem(tempGDT);
               return FALSE;
            }
         }
         tempGDTCount = cpus;
         return TRUE;
      } else {
         Warning("Task_Initialize: Unable to allocate space for temporary GDT "
                 "pointers");
         return FALSE;
      }
   } else {
      return TRUE;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * RestoreHostGDTTRLDT --
 *
 *
 * Results:
 *      The host's GDT is copied (or partially copied) to the
 *      dynamically allocated temporary GDT.
 *
 *      The TR is restored using the temporary GDT then the host's real GDT is
 *      restored.  Finally, the host LDT is restored.
 *
 * Notes:
 *      An OS which checks critical data structures, such as the GDT,
 *      can fail when this module changes the TSS busy bit in the host
 *      GDT.  To avoid this problem, we use a sparse copy of the host
 *      GDT to perform the manipulation of the TSS busy bit.
 *
 *      See PR 68144.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER void
RestoreHostGDTTRLDT(uint32 pcpuid, VMCrossPage *crosspage,
                    DTR64 hostGDT64, Selector ldt, Selector cs, Selector tr)
{
   TS_ASSERT(tr != 0);
   TS_ASSERT((tr & 7) == 0);

   if (USE_TEMPORARY_GDT) {
      Descriptor *tempGDTBase;
      DTR64 tempGDT64;

      /*
       * Set up a temporary GDT so that the TSS 'busy bit' can be
       * changed without affecting the host's data structures.
       */
      const VA hostGDTVA  = HOST_KERNEL_LA_2_VA(hostGDT64.offset);
      const unsigned size = sizeof(Descriptor);
      const Selector ss   = SELECTOR_CLEAR_RPL(GET_SS());

      /*
       * Compat mode is when we're compiled as 32-bit code but the host is
       * 64-bit, in which case you could have a GDT with a 64-bit address but
       * we can only access 32-bit addresses.  This ASSERT make sure we can
       * access the host GDT.
       *
       * Note the only time we run in compat mode is on 32-bit Mac OS, in
       * which case we don't use the temporary GDT stuff.
       */
      ASSERT(!TaskInCompatMode());

      ASSERT(pcpuid < tempGDTCount);
      ASSERT(hostGDTVA == HOST_KERNEL_LA_2_VA(hostGDT64.offset));
      tempGDTBase = tempGDT[pcpuid];

      ASSERT(SELECTOR_RPL(cs) == 0 && SELECTOR_TABLE(cs) == 0);
      ASSERT(SELECTOR_RPL(ss) == 0 && SELECTOR_TABLE(ss) == 0);

      /*
       * Copy code and data segments so they remain valid in case of NMI.
       * Worldswitch code returns with DS==ES==SS so we don't have to set
       * up DS,ES explicitly.
       */
      ASSERT(SELECTOR_CLEAR_RPL(GET_DS()) == ss);
      ASSERT(SELECTOR_CLEAR_RPL(GET_ES()) == ss);
      tempGDTBase[cs / size]     = *(Descriptor *)(hostGDTVA + cs);
      tempGDTBase[ss / size]     = *(Descriptor *)(hostGDTVA + ss);

      /*
       * TR descriptors use two entries (64-bits wide) in 64-bit mode.
       */
      tempGDTBase[tr / size]     = *(Descriptor *)(hostGDTVA + tr);
      tempGDTBase[tr / size + 1] = *(Descriptor *)(hostGDTVA + tr + size);

      /*
       * Clear the 'task busy' bit so we can reload TR.
       */
      if (Desc_Type(&tempGDTBase[tr / size]) == TASK_DESC_BUSY) {
         Desc_SetType(&tempGDTBase[tr / size], TASK_DESC);
      }

      /*
       * Restore the TR using the temp GDT then restore the host's real GDT
       * then host LDT.
       */
      tempGDT64.limit  = hostGDT64.limit;
      tempGDT64.offset = HOST_KERNEL_VA_2_LA((VA)tempGDTBase);
      _Set_GDT((DTR *)&tempGDT64);
      SET_TR(tr);
      _Set_GDT((DTR *)&hostGDT64);
      SET_LDT(ldt);
   } else {
      /*
       * The host isn't picky about the TR entry.  So clear the TSS<busy> bit
       * in the host GDT, then restore host GDT and TR, then LDT.
       */
      TASK_IF_COMPAT_MODE_THEN_ELSE({
         __asm__ __volatile__("lcall %3,$TaskCM_RestoreGDTTRLDT64"
                              :
                              : "c" (&hostGDT64),
                                "a" ((uint32)tr),
                                "d" ((uint32)ldt),
                                "i" (KERNEL64_CS)
                              : "cc", "memory");
      }, {
         Descriptor *desc;

         desc = (Descriptor *)((VA)HOST_KERNEL_LA_2_VA(hostGDT64.offset + tr));
         if (Desc_Type(desc) == TASK_DESC_BUSY) {
            Desc_SetType(desc, TASK_DESC);
         }
         _Set_GDT((DTR *)&hostGDT64);
         SET_TR(tr);
         SET_LDT(ldt);
      })
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * Task_AllocCrossGDT --
 *
 *      Make sure the crossGDT is allocated and initialized.
 *
 * Results:
 *      TRUE iff crossGDT was already initialized or successfully initialized.
 *
 * Side effects:
 *      crossGDT static vars set up if not already.
 *
 *-----------------------------------------------------------------------------
 */

Bool
Task_AllocCrossGDT(InitBlock *initBlock)  // OUT: crossGDT values filled in
{
   DTR64 hostGDT64;

   /*
    * Make sure only one of these runs at a time on the whole system, because
    * there is only one crossGDT for the whole system.
    */
   HostIF_GlobalLock(2);

   /*
    * Maybe the crossGDT has already been set up.
    */
   if (crossGDT == NULL) {
      static const MPN maxValidFirst =
         0xFFC00 /* 32-bit MONITOR_LINEAR_START */ - CROSSGDT_NUMPAGES;

      /*
       * The host entries must fit on pages of the crossGDT that are mapped.
       * Since we know they are below CROSSGDT_LOWSEG, we can just check that
       * CROSSGDT_LOWSEG and below are mapped.
       *
       * Because the CROSSGDT_LOWSEG segments must reside on the first page of
       * the crossGDT (as they must remain valid with paging off), all we need
       * do is check that bit 0 of CROSSGDT_PAGEMASK is set (indicating that
       * page 0 of the crossGDT will be mapped).
       */
      ASSERT_ON_COMPILE(CROSSGDT_LOWSEG < PAGE_SIZE);
      ASSERT_ON_COMPILE(CROSSGDT_PAGEMASK & 1);

      /*
       * Allocate the crossGDT.
       */
      ASSERT_ON_COMPILE(sizeof *crossGDT == CROSSGDT_NUMPAGES * PAGE_SIZE);
      crossGDT = HostIF_AllocCrossGDT(CROSSGDT_NUMPAGES, maxValidFirst,
                                      crossGDTMPNs);
      if (crossGDT == NULL) {
         HostIF_GlobalUnlock(2);
         Warning("TaskAllocCrossGDT: unable to allocate crossGDT\n");
         return FALSE;
      }

      /*
       * Check that the crossGDT meets the address requirements documented in
       * bora/doc/worldswitch-pages.txt.
       */
      if (crossGDTMPNs[0] > maxValidFirst) {
         HostIF_FreeCrossGDT(CROSSGDT_NUMPAGES, crossGDT);
         crossGDT = NULL;
         HostIF_GlobalUnlock(2);
         Warning("TaskAllocCrossGDT: crossGDT MPN %X gt %X\n",
                 crossGDTMPNs[0], maxValidFirst);
         return FALSE;
      }

      /*
       * Fill the crossGDT with a copy of our host GDT.  VMX will have to fill
       * in monitor segments via Task_InitCrossGDT.
       *
       * We are assuming that all the host segments we will ever need are below
       * CROSSGDT_LOWSEG.  If this assumption ever breaks, the host segments
       * would have to be unconditionally transitioned to the CROSSGDT
       * intermediate segments before switching to the monitor.  The only time
       * the GDT has been found to be bigger than CROSSGDT_LOWSEG is when they
       * are running KVM or Xen, and we never see the large segment numbers.
       */
      memset(crossGDT, 0, sizeof *crossGDT);
      TaskSaveGDT64(&hostGDT64);
      if (hostGDT64.limit > CROSSGDT_LOWSEG * 8 - 1) {
         hostGDT64.limit = CROSSGDT_LOWSEG * 8 - 1;
      }
      TaskCopyGDT64(&hostGDT64, crossGDT->gdtes);

      /*
       * Set up descriptor for the crossGDT using host kernel LA as a base.
       */
      crossGDTDescHKLA.limit  = sizeof *crossGDT - 1;
      crossGDTDescHKLA.offset = HOST_KERNEL_VA_2_LA((VA)crossGDT);
   }

   HostIF_GlobalUnlock(2);

   initBlock->crossGDTHKLA = crossGDTDescHKLA.offset;
   ASSERT_ON_COMPILE(sizeof initBlock->crossGDTMPNs == sizeof crossGDTMPNs);
   memcpy(initBlock->crossGDTMPNs, crossGDTMPNs, sizeof crossGDTMPNs);

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Task_InitCrosspage  --
 *
 *    Initialize the crosspage used to switch to the monitor task.
 *
 * Results:
 *    0 on success
 *    != 0 on failure
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

int
Task_InitCrosspage(VMDriver *vm,          // IN
                   InitBlock *initParams) // IN/OUT: Initial params from the
                                          //         VM
{
   Vcpuid vcpuid;

   if (crossGDT == NULL) {
      return 1;
   }

   initParams->crossGDTHKLA = crossGDTDescHKLA.offset;
   ASSERT_ON_COMPILE(sizeof initParams->crossGDTMPNs == sizeof crossGDTMPNs);
   memcpy(initParams->crossGDTMPNs, crossGDTMPNs, sizeof crossGDTMPNs);

   ASSERT(sizeof(VMCrossPage) < PAGE_SIZE);
   ASSERT(MODULECALL_CROSS_PAGE_LEN == 1);
   for (vcpuid = 0; vcpuid < initParams->numVCPUs;  vcpuid++) {
      VA64 crossPageUserAddr = initParams->crosspage[vcpuid];
      VMCrossPage *p = HostIF_MapCrossPage(vm, crossPageUserAddr);
      MPN crossPageMPN;

      if (p == NULL) {
         return 1;
      }

      crossPageMPN = HostIF_LookupUserMPN(vm, crossPageUserAddr);

      if ((int64)crossPageMPN <= 0) {
         return 1;
      }

      {
         /* The version of the crosspage must be the first four
          * bytes of the crosspage.  See the declaration
          * of VMCrossPage in modulecall.h.
          */
         ASSERT_ON_COMPILE(offsetof(VMCrossPage, version) == 0);
         ASSERT_ON_COMPILE(sizeof(p->version) == sizeof(uint32));

         /* p->version is VMX's version; CROSSPAGE_VERSION is vmmon's. */
         if (p->version != CROSSPAGE_VERSION) {
            Warning("crosspage version mismatch: "
                    "vmmon claims %#x, must match vmx version of %#x.\n",
                    (int)CROSSPAGE_VERSION, p->version);
            return 1;
         }
      }
      {
         /* The following constants are the size and offset of the
          * VMCrossPage->crosspage_size field as defined by the
          * vmm/vmx.
          */
         ASSERT_ON_COMPILE(offsetof(VMCrossPage, crosspage_size) ==
                           sizeof(uint32));
         ASSERT_ON_COMPILE(sizeof(p->crosspage_size) == sizeof(uint32));

         if (p->crosspage_size != sizeof(VMCrossPage)) {
            Warning("crosspage size mismatch: "
                    "vmmon claims %#x bytes, must match vmm size of %#x bytes.\n",
                    (int)sizeof(VMCrossPage), p->crosspage_size);
            return 1;
         }
      }

      if (crossPageMPN > MA_2_MPN(0xFFFFFFFF)) {
         Warning("Task_InitCrosspage*: crossPageMPN 0x%llx invalid\n", (unsigned long long)crossPageMPN);
         return 1;
      }
      p->crosspageMA = (uint32)MPN_2_MA(crossPageMPN);
      p->hostCrossPageLA = (LA64)(uintptr_t)p;

      /*
       * Pass our kernel code segment numbers back to MonitorPlatformInit.
       * They have to be in the GDT so they will be valid when the crossGDT is
       * active.
       */
#if defined __APPLE__ && !vm_x86_64
      p->hostInitial32CS = KERNEL32_CS;
      p->hostInitial64CS = KERNEL64_CS;
#else
      p->hostInitial64CS = p->hostInitial32CS = GET_CS();
#endif
      TS_ASSERT(SELECTOR_RPL  (p->hostInitial32CS) == 0 &&
                SELECTOR_TABLE(p->hostInitial32CS) == 0);
      TS_ASSERT(SELECTOR_RPL  (p->hostInitial64CS) == 0 &&
                SELECTOR_TABLE(p->hostInitial64CS) == 0);

      p->irqRelocateOffset[0]  = IRQ_HOST_INTR1_BASE;
      p->irqRelocateOffset[1]  = IRQ_HOST_INTR2_BASE;
      p->userCallRequest       = MODULECALL_USERCALL_NONE;
      p->moduleCallInterrupted = FALSE;
      p->pseudoTSCConv.p.mult  = 1;
      p->pseudoTSCConv.p.shift = 0;
      p->pseudoTSCConv.p.add   = 0;
      p->pseudoTSCConv.changed = TRUE;
      vm->crosspage[vcpuid]    = p;
   }

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Task_InitCrossGDT --
 *
 *    Fill in a crossGDT entry from the given template.
 *
 * Results:
 *    0 on success
 *    != 0 on failure
 *
 * Side effects:
 *    CrossGDT entry filled from template.  If crossGDT has already been
 *    initialized, the entry is compared to the given template.  Any
 *    discrepancy is logged and an error is returned.  This is necessary
 *    because this same GDT is shared among all VMs on this host, so really,
 *    the first call initializes it and the others just do compares.
 *
 *-----------------------------------------------------------------------------
 */

int
Task_InitCrossGDT(InitCrossGDT *initCrossGDT)  // IN
{
   Descriptor v;
   int rc;
   uint32 i;

   rc = 1;
   i  = initCrossGDT->index;
   v  = initCrossGDT->value;

   HostIF_GlobalLock(3);
   if (i >= sizeof crossGDT->gdtes / sizeof crossGDT->gdtes[0]) {
      HostIF_GlobalUnlock(3);
      Warning("Task_InitCrossGDT: index %u too big\n", i);
   } else if (!(  (1 << (i * sizeof crossGDT->gdtes[0] / PAGE_SIZE))
                & CROSSGDT_PAGEMASK)) {
      HostIF_GlobalUnlock(3);
      Warning("Task_InitCrossGDT: index %u not in CROSSGDT_PAGEMASK %x\n",
              i, CROSSGDT_PAGEMASK);
   } else if (!Desc_Present(&v)) {
      HostIF_GlobalUnlock(3);
      Warning("Task_InitCrossGDT: entry %u not present\n", i);
   } else if (!Desc_Present(crossGDT->gdtes + i)) {
      crossGDT->gdtes[i] = v;
      HostIF_GlobalUnlock(3);
      rc = 0;
   } else if (Desc_EqualIgnoreAccessed(crossGDT->gdtes + i, &v)) {
      HostIF_GlobalUnlock(3);
      rc = 0;
   } else {
      HostIF_GlobalUnlock(3);
      Warning("Task_InitCrossGDT: entry 0x%X mismatch\n", i);
      Warning("Task_InitCrossGDT:   crossGDT %16.16llX\n",
              (long long unsigned)*(uint64 *)(crossGDT->gdtes + i));
      Warning("Task_InitCrossGDT:   template %16.16llX\n",
              (long long unsigned)*(uint64 *)&v);
   }

   return rc;
}


/*
 *-----------------------------------------------------------------------------
 *
 *      Disable and restore APIC NMI delivery.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
DisableNMIDelivery(volatile uint32 *regPtr) // IN/OUT
{
   uint32 reg;

   reg = *regPtr;
   if ((APIC_LVT_DELVMODE(reg) == APIC_LVT_DELVMODE_NMI) &&
       (!APIC_LVT_ISMASKED(reg))) {
      *regPtr = reg | APIC_LVT_MASK;
      dummyLVT = *regPtr; // Force completion of masking, Bug 78470.
      return TRUE;
   }
   return FALSE;
}


static void
DisableNMI(VMDriver *vm,     // IN
           Bool *lint0NMI,   // OUT
           Bool *lint1NMI,   // OUT
           Bool *pcNMI,      // OUT
           Bool *thermalNMI) // OUT
{
   if (vm->hostAPIC) {
      *lint0NMI = DisableNMIDelivery(&APIC_LINT0_REG(vm->hostAPIC));
      *lint1NMI = DisableNMIDelivery(&APIC_LINT1_REG(vm->hostAPIC));
      *pcNMI = DisableNMIDelivery(&APIC_PC_REG(vm->hostAPIC));

      /*
       * The LVT thermal monitor register was introduced
       * in Pentium 4 and Xeon processors.
       */

      if (APIC_MAX_LVT(vm->hostAPIC) >= 5) {
         *thermalNMI = DisableNMIDelivery(&APIC_THERM_REG(vm->hostAPIC));
      } else {
         *thermalNMI = FALSE;
      }
   } else {
      *lint0NMI = FALSE;
      *lint1NMI = FALSE;
      *pcNMI = FALSE;
      *thermalNMI = FALSE;
   }
}


static void
RestoreNMI(VMDriver *vm,    // IN
           Bool lint0NMI,   // IN
           Bool lint1NMI,   // IN
           Bool pcNMI,      // IN
           Bool thermalNMI) // IN
{
#define RestoreNMIDelivery(cond, apicr)                         \
   do {                                                         \
      if (cond) {                                               \
         uint32 reg;                                            \
                                                                \
         reg = apicr;                                           \
         apicr = reg & ~APIC_LVT_MASK;                          \
      }                                                         \
   } while (0)

   RestoreNMIDelivery(lint0NMI, APIC_LINT0_REG(vm->hostAPIC));
   RestoreNMIDelivery(lint1NMI, APIC_LINT1_REG(vm->hostAPIC));
   RestoreNMIDelivery(pcNMI, APIC_PC_REG(vm->hostAPIC));
   RestoreNMIDelivery(thermalNMI, APIC_THERM_REG(vm->hostAPIC));
#undef RestoreNMIDelivery
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskEnableTF --
 *
 *     Turn on EFLAGS<TF>.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Trace trapping enabled.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
TaskEnableTF(void)
{
#if defined(__GNUC__)
#ifdef VM_X86_64
   asm volatile ("pushfq ; orb $1,1(%rsp) ; popfq");
#else
   asm volatile ("pushfl ; orb $1,1(%esp) ; popfl");
#endif
#elif defined(_MSC_VER)
#ifdef VM_X86_64
   static CODE_BUF uint8 const setTFCode64[] = {
      0x9C,                          // pushfq
      0x80, 0x4C, 0x24, 0x01, 0x01,  // orb $1,1(%rsp)
      0x9D,                          // popfq
      0xC3 };                        // retq

   ((void (*)(void))setTFCode64)();
#else
   __asm {
      pushf
      mov    al,1
      or     [esp+1],al
      popf
   }
#endif
#else
#error no compiler for setting stress flag
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskDisableTF --
 *
 *     Turn off EFLAGS<TF>.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Trace trapping disabled.
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
TaskDisableTF(void)
{
#if defined(__GNUC__)
#ifdef VM_X86_64
   asm volatile ("pushfq ; andb $~1,1(%rsp) ; popfq");
#else
   asm volatile ("pushfl ; andb $~1,1(%esp) ; popfl");
#endif
#elif defined(_MSC_VER)
#ifdef VM_X86_64
   static CODE_BUF uint8 const clrTFCode64[] = {
      0x9C,                          // pushfq
      0x80, 0x64, 0x24, 0x01, 0xFE,  // andb $~1,1(%rsp)
      0x9D,                          // popfq
      0xC3 };                        // retq

   ((void (*)(void))clrTFCode64)();
#else
   __asm {
      pushf
      mov    al,254
      and    [esp+1],al
      popf
   }
#endif
#else
#error no compiler for clearing stress flag
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskSaveDebugRegisters --
 *
 *      Save debug registers in the host context area of the crosspage.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      crosspage->hostDR[*] = some filled with debug register contents
 *               hostDRSaved = bits set for those we wrote to hostDR[*] array
 *                hostDRInHW = bits set indicating which hardware DR contents
 *                             still match what the host wants
 *      hardware DR7<GD> = 0
 *      hardware DR7<bp enables> = 0
 *
 *-----------------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER void
TaskSaveDebugRegisters(VMCrossPage *crosspage)
{
   uint8 saveGotDB;

#define SAVE_DR(n)                             \
   do {                                        \
      uintptr_t drReg;                         \
      GET_DR##n(drReg);                        \
      crosspage->hostDR[n] = drReg;            \
   } while (0)

   /* Hardware contains the host's %dr7, %dr6, %dr3, %dr2, %dr1, %dr0 */
   crosspage->hostDRInHW = ((1 << 7) | (1 << 6) |
                            (1 << 3) | (1 << 2) | (1 << 1) | (1 << 0));

   /*
    * Save DR7 since we need to disable debug breakpoints during the world
    * switch code.  We will get a #DB if DR7<GD> is set, but the
    * SwitchDBHandler simply IRETs after setting crosspage gotDB flag.
    */
   saveGotDB = SWITCHNMI(crosspage)->gotDB;
   SWITCHNMI(crosspage)->gotDB = 0;
   SAVE_DR(7);

   /*
    * In any case, the DR7 at this point shouldn't have the GD bit set.
    */
   TS_ASSERT(!(crosspage->hostDR[7] & DR7_GD));

   /*
    * Save DR6 in order to accommodate the ICEBP instruction and other stuff
    * that can modify DR6 bits (trace traps, task switch traps, any others?).
    */
   SAVE_DR(6);

   /*
    * It may be that DR7 had the GD bit set, in which case the crosspage gotDB
    * flag would have just been set and DR6<BD> will be set.  If so, fix the
    * saved values to look like they were when DR7<GD> was set (before we
    * tripped the #DB), so they'll get restored to what they were.  Then make
    * sure breakpoints are disabled during switch.
    *
    * Note that I am assuming DR6_BD was clear before the #DB and so I'm
    * clearing it here.  If it was set, we will end up restoring it cleared,
    * but there's no way to tell.  Someone suggested that ICEBP would tell us
    * but it may also clear DR6<3:0>.
    */
   if (SWITCHNMI(crosspage)->gotDB && (crosspage->hostDR[6] & DR6_BD)) {
      crosspage->hostDR[6] -= DR6_BD;
      crosspage->hostDR[7] |= DR7_GD;
      SET_DR7(DR7_DEFAULT);

      /* HW: %dr7 and %dr6 are the guest, %dr3, %dr2, %dr1, %dr0 are host */
      crosspage->hostDRInHW = (1 << 3) | (1 << 2) | (1 << 1) | (1 << 0);
   }

   /*
    * No GD bit, check for enabled breakpoints.  Disable them as they may
    * coincidentally trip during the switch.
    */
   else if (crosspage->hostDR[7] & DR7_ENABLED) {
      SET_DR7(DR7_DEFAULT);          // no #DB here, just simple set
      /* HW: %dr7 = guest, %dr6, %dr3, %dr2, %dr1, %dr0 = host */
      crosspage->hostDRInHW = ((1 << 6) | (1 << 3) | (1 << 2) |
                               (1 << 1) | (1 << 0));
   }

   /*
    * We shouldn't generate any more #DBs.
    */
   SWITCHNMI(crosspage)->gotDB = saveGotDB;

   /*
    * At any rate, hostDR[6,7] have host contents in them now.
    */
   crosspage->hostDRSaved = 0xC0;

   if (TaskInLongMode() && !crosspage->runVmm64) {

      /*
       * The host might be using the top 32 bits of the debug registers but the
       * monitor is only a 32-bit monitor, so it might just wipe out the top
       * halves of DR0..DR3.  So we must save them before letting the monitor
       * touch them.
       */
      TASK_IF_COMPAT_MODE_THEN_ELSE({
         __asm__ __volatile__("lcall %1,$TaskCM_SaveDebugRegisters64"
                              :
                              : "c" (crosspage->hostDR),
                                "i" (KERNEL64_CS)
                              : "eax", "edx", "cc", "memory");
      }, {
         SAVE_DR(0);
         SAVE_DR(1);
         SAVE_DR(2);
         SAVE_DR(3);
      })
      crosspage->hostDRSaved = 0xCF;  // DR0..3,6,7 are saved in hostDR[n]
   }

#undef SAVE_DR
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskRestoreDebugRegisters --
 *
 *      Put the debug registers back the way they were when
 *      TaskSaveDebugRegisters was called.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Debug registers restored from values saved in the crosspage.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER void
TaskRestoreDebugRegisters(VMCrossPage *crosspage)
{
#define RESTORE_DR(n)                                                 \
   if ((crosspage->hostDRInHW & (1 << n)) == 0) {                     \
      /* Guest value for register 'n' in hardware. */                 \
      const uintptr_t drReg = (uintptr_t)crosspage->hostDR[n];        \
      if (!(crosspage->shadDRInHW & (1 << n)) ||                      \
          (drReg != SHADOW_DR(crosspage, n))) {                       \
         SET_DR##n(drReg);                                            \
      }                                                               \
   }

   TASK_IF_COMPAT_MODE_THEN_ELSE({
      uint32 edxGetsWiped;

      __asm__ __volatile__("lcall %3,$TaskCM_RestoreDebugRegisters64"
                           : "=d" (edxGetsWiped)
                           : "c" (crosspage->hostDR),
                             "0" (crosspage->hostDRInHW),
                             "i" (KERNEL64_CS)
                           : "eax", "cc", "memory");
   }, {
      RESTORE_DR(0);
      RESTORE_DR(1);
      RESTORE_DR(2);
      RESTORE_DR(3);
   })

   RESTORE_DR(6);

   /*
    * DR7 must be restored last in case DR7<GD> is set.
    */
   RESTORE_DR(7);

#undef RESTORE_DR
}


/*
 *-----------------------------------------------------------------------------
 *
 * TaskUpdatePTSCParameters --
 *
 *      If the PTSC is behind where it should be, based on the host's
 *      uptime, then adjust the PTSC parameters.  PR 118376.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May update the PTSC parameters.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER void
TaskUpdatePTSCParameters(VMCrossPage *crosspage)
{
   uint64 ptsc;

   ASSERT_NO_INTERRUPTS();
   ptsc = Vmx86_GetPseudoTSC();

   if (UNLIKELY(ptsc <= crosspage->worldSwitchPTSC)) {
      /*
       * If the PTSC went backwards since we last left the monitor, then either:
       *  a) TSC is unsynchronized across cores.
       *  b) TSC was reset (probably due to host stand by or hibernate).
       *  c) khzEstimate was incorrect (too low).
       *  d) the host's reference clock is too low resolution.
       *  e) the host's reference clock is broken.
       *
       * We handle case (a) and (b) by switch PTSC over to using the
       * reference clock as the basis for pseudo TSC.
       *
       * For case (c), ideally we'd want to get khzEstimate correct in
       * the first place.  Using the reference clock for pseudo TSC is
       * just a backup if all else failed.  It will prevent PTSC from
       * drifting from real time over the long run.  Additionally, we
       * could try to adopt the mult/shift of pseudoTSCConv to make PTSC
       * run at the (incorrect) TSC kHz estimate, so that PTSC
       * progresses at the correct rate over the short term (while in
       * the monitor).
       *
       * We don't do anything for case (e).  If we see it happen, we
       * could try to pin the value returned by HostIF_ReadUptime to
       * some sane range to help compensate.
       */
      if (Vmx86_SetPseudoTSCUseRefClock()) {
         ptsc = Vmx86_GetPseudoTSC();
      }
      /*
       * For case (d), check for PTSC between (worldSwitchPTSC - Hz) and
       * worldSwitchPTSC.  That is, if ptsc is still behind
       * worldSwitchPTSC (even after ensuring the PTSC is based on the
       * reference clock), but by less than a second, assume that the
       * reference clock is too low of resolution, and nudge PTSC
       * forward to ensure it doesn't go backwards on this VCPU.  If we
       * are more than a second behind, then we assume that the
       * reference clock was stepped (or broken) and we just stay in
       * sync with it.
       */
      if ((uint64)(crosspage->worldSwitchPTSC - ptsc) <
          Vmx86_GetPseudoTSCHz()) {
         ptsc = crosspage->worldSwitchPTSC;
      }
   }

   if (Vmx86_PseudoTSCUsesRefClock()) {
      /*
       * In this case, the pseudo TSC basis is the reference clock.
       * While running in the monitor, we can't read the reference
       * clock, which is implemented by the host OS.  So, offset from
       * the current pseudoTSC value using the TSC in order to provide
       * high resolution PTSC while in the monitor.  The RDTSC below
       * must be executed on the same pcpu that the vmm vcpu thread will
       * run on (in case of out of sync TSCs).  This is guaranteed since
       * we are on the on-ramp into the monitor with interrupts
       * disabled.
       */
      uint64 tsc = RDTSC();
      crosspage->pseudoTSCConv.p.add   = ptsc - tsc;
      crosspage->pseudoTSCConv.changed = TRUE;
   }
   /* Cache PTSC value for BackToHost. */
   crosspage->worldSwitchPTSC = ptsc;
}


/*
 *-----------------------------------------------------------------------------
 *
 * SwitchToMonitor --
 *
 *      Wrapper that calls code to switch from the host to the monitor.
 *
 *      The basic idea is to do a (*(crosspage->hostToVmm))(crosspage)
 *      but it's complicated because we must have a common call format
 *      between GCC and MSC.
 *
 *      Since we have complete control over what GCC does with asm volatile,
 *      this amounts to having GCC do exactly what MSC does.  So for
 *      32-bit hosts, we use the fastcall format, i.e., pass the one-and-only
 *      parameter in ECX.  For 64-bit hosts, we pass the parameter in RCX.
 *
 *      For 32-bit GCC and MSC, the callee is expected to preserve
 *      EBX,ESI,EDI,EBP,ESP, so the worldswitch just saves those registers.
 *      For 64-bit GCC, the callee is expected to preserve
 *      RBX,RBP,RSP,R12..R15, whereas MSC expects the callee to preserve
 *      RBX,RSI,RDI,RBP,RSP,R12..R15.  So for simplicity, we have the
 *      worldswitch code save RBX,RSI,RDI,RBP,RSP,R12..R15.
 *
 *      The worldswitch code figures out if it's jumping to a 32-bit or 64-bit
 *      monitor and acts accordingly, so we don't have to split off those cases
 *      here.
 *
 * From an email with Petr regarding gcc's handling of the stdcall
 * attribute for x86-64:
 *
 *    As far as I can tell, for x86_64 there is only one calling
 *    convention:
 *       On GCC rdi/rsi/rdx/rcx/r8d/r9d for <= 6 arguments,
 *       others always on stack, caller always adjusts stack.
 *
 *       On MSC it is rcx/rdx/r8d/r9d for <= 4 arguments, rest on
 *       stack.  When more than 4 arguments are passed, spill space is
 *       reserved on the stack for the register arguments.  Argument
 *       5 is accessed at (5 * 8)(rsp).
 *
 * Side effects:
 *      The monitor does many things, but it's irrelevant to this code.  The
 *      worldswitch should eventually return here with the host state intact.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER void
SwitchToMonitor(VMCrossPage *crosspage)
{
   uint8 *codePtr;

   codePtr  = (uint8 *)WSMODULE(crosspage);
   codePtr += WSMODULE(crosspage)->hostToVmm;

#if defined(__GNUC__)

#if defined(VM_X86_64)
   /*
    * GCC on 64-bit host:
    *
    * Pass the crosspage pointer in RCX just like the 64-bit MSC does.
    * Tell GCC that the worldswitch preserves RBX,RSI,RDI,RBP,RSP,
    * R12..R15 just like the MSC 64-bit calling convention.
    *
    * Using ifdef so we don't ever try to compile this on 32-bit system.
    */
   {
      uint64 raxGetsWiped, rcxGetsWiped;

      asm volatile ("call *%%rax"
                    : "=a" (raxGetsWiped),
                      "=c" (rcxGetsWiped)
                    : "0" (codePtr),
                      "1" (crosspage)
                    : "rdx", "r8", "r9", "r10", "r11", "cc", "memory");
   }
#else

   /*
    * GCC on 32-bit (compatibility or legacy) mode host:
    *
    * Pass the crosspage pointer in ECX just like the 32-bit MSC fastcall
    * convention.  MSC calling also expects worldswitch to preserve
    * EBX,ESI,EDI,EBP,ESP so tell GCC likewise.
    */
   TASK_IF_COMPAT_MODE_THEN_ELSE({
      uint32 eaxGetsWiped;
      uint32 ecxGetsWiped;

      __asm__ __volatile__("lcall %4,$TaskCM_CallWS"
                           : "=a" (eaxGetsWiped),
                             "=c" (ecxGetsWiped)
                           : "0" (codePtr),
                             "1" (crosspage),
                             "i" (KERNEL64_CS)
                           : "edx", "cc", "memory");
   }, {
      uint32 eaxGetsWiped;
      uint32 ecxGetsWiped;

      __asm__ __volatile__("call *%%eax"
                           : "=a" (eaxGetsWiped),
                             "=c" (ecxGetsWiped)
                           : "0" (codePtr),
                             "1" (crosspage)
                           : "edx", "cc", "memory");
   })
#endif // defined(VM_X86_64)

#elif defined(_MSC_VER)

   /*
    * MSC on 64-bit host:
    *
    * The 64-bit calling convention is to pass the argument in RCX and that
    * the called function must preserve RBX,RSI,RDI,RBP,RSP,R12..R15.
    *
    * While we strictly don't need ifdef in this case, we do it to be
    * consistent with the GCC case.
    */
#if defined(VM_X86_64)
   (*(void (*)(VMCrossPage *))codePtr)(crosspage);
#else

   /*
    * MSC on 32-bit host:
    *
    * The 32-bit fastcall convention is to pass the argument in ECX and that
    * the called function must preserve EBX,ESI,EDI,EBP,ESP.
    */
   TS_ASSERT(!TaskInCompatMode());
   (*(void (__fastcall *)(VMCrossPage *))codePtr)(crosspage);
#endif // defined(VM_X86_64)

#else
#error No compiler defined for SwitchToMonitor
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * Task_Switch --
 *
 *      Switches from the host context into the monitor
 *      context. Think of it as a coroutine switch that changes
 *      not only the registers, but also the address space
 *      and all the hardware state.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Jump to the other side. Has no direct effect on the
 *      host-visible state except that it might generate an interrupt.
 *
 *-----------------------------------------------------------------------------
 */

void
Task_Switch(VMDriver *vm,  // IN
            Vcpuid vcpuid) // IN
{
   static Bool testSwitchNMI = TRUE;
   Bool        loopForced;
   uintptr_t   flags, cr0reg, cr2reg, cr4reg, new_cr4;
#ifdef VM_X86_64
   uintptr_t   cr3reg;
#endif
   uint64      fs64  = 0;
   uint64      gs64  = 0;
   uint64      kgs64 = 0;
   DTR64       hostGDT64, hostIDT64;
   Selector    cs, ds, es, fs, gs, ss;
   Selector    hostTR;
   Selector    hostLDT;
   Bool lint0NMI;
   Bool lint1NMI;
   Bool pcNMI;
   Bool thermalNMI;
   VMCrossPage *crosspage = vm->crosspage[vcpuid];

   DisableNMI(vm, &lint0NMI, &lint1NMI, &pcNMI, &thermalNMI);
   SAVE_FLAGS(flags);
   CLEAR_INTERRUPTS();

   loopForced = FALSE;
   while (TRUE) {
      uint32 pCPU = HostIF_GetCurrentPCPU();

      if (crosspage->inVMXOperation) {
         MPN mpn;

         ASSERT(pCPU < ARRAYSIZE(rootVMCS));
         mpn = Atomic_Read32(&rootVMCS[pCPU]);
         if (UNLIKELY(mpn == INVALID_MPN)) {
            crosspage->userCallType = MODULECALL_USERCALL_NONE;
            crosspage->moduleCallType = MODULECALL_ALLOC_VMX_PAGE;
            crosspage->args[0] = pCPU;
            break;
         }

         crosspage->rootVMCS = MPN_2_MA(mpn);
      }

      /*
       * Save CR state.  The switchcode deals with CR3, except we clear its
       * CR3.PCD and CR3.PWT bits. The monitor and switchcode deal with EFER.
       */
      GET_CR0(cr0reg);
      GET_CR2(cr2reg);
      GET_CR4(cr4reg);
#ifdef VM_X86_64
      GET_CR3(cr3reg);
#endif

      vm->currentHostCpu[vcpuid] = pCPU;

      /*
       * The 32-bit switchNMI handlers assume there is a data segment at what
       * their code segment is + 8.  So we verify the host segments.
       * (ASSERT_ON_COMPILEs inside monitorHosted32.c verified the monitor and
       * temp crossGDT segments).
       */
      if (vmx86_debug && !TaskInLongMode()) {
         Descriptor *hostGDT;
         DTR hostGDTR;
         uint16 nmiCS, nmiDS;

         nmiCS = crosspage->switchHostIDT[4] >> 16;
         nmiDS = nmiCS + 8;
         GET_GDT(hostGDTR);
         TS_ASSERT(nmiDS + 7 <= hostGDTR.limit);

         hostGDT = (Descriptor *)HOST_KERNEL_LA_2_VA(hostGDTR.offset);
         nmiCS  /= sizeof *hostGDT;
         nmiDS  /= sizeof *hostGDT;

         TS_ASSERT(hostGDT[nmiCS].present);
         TS_ASSERT(hostGDT[nmiDS].present);
         TS_ASSERT(hostGDT[nmiCS].DPL == 0);
         TS_ASSERT(hostGDT[nmiDS].DPL == 0);
         TS_ASSERT(hostGDT[nmiCS].S);
         TS_ASSERT(hostGDT[nmiDS].S);
         TS_ASSERT(DT_NONCONFORMING_CODE(hostGDT[nmiCS]));
         TS_ASSERT(DT_WRITEABLE_DATA(hostGDT[nmiDS]));
         TS_ASSERT(Desc_GetBase(&hostGDT[nmiCS]) == 0);
         TS_ASSERT(Desc_GetBase(&hostGDT[nmiDS]) == 0);
         TS_ASSERT(Desc_GetLimit(&hostGDT[nmiCS]) == 0xFFFFFU);
         TS_ASSERT(Desc_GetLimit(&hostGDT[nmiDS]) == 0xFFFFFU);
      }

      TaskUpdatePTSCParameters(crosspage);

      /*
       * Save the host's standard IDT and set up an IDT that only has small DB
       * and NMI handlers in the crosspage.  Those handlers just set a flag in
       * the crosspage.
       */
      TaskSaveIDT64(&hostIDT64);
      TaskLoadIDT64(&crosspage->switchHostIDTR);

      /*
       * Test the DB,NMI,MCE handlers to make sure they can set the flags.
       * This is calling the handlers in switchNMI.S.
       */
      if (vmx86_debug && testSwitchNMI) {
         uint8 gotSave;

         testSwitchNMI = FALSE;

         /*
          * RAISE_INTERRUPT calls Switch{32,64}DBHandler in switchNMI.S
          * (depending on host bitsize).
          */
         gotSave = SWITCHNMI(crosspage)->gotDB;
         SWITCHNMI(crosspage)->gotDB = 0;
         RAISE_INTERRUPT(1);
         TS_ASSERT(SWITCHNMI(crosspage)->gotDB);
         SWITCHNMI(crosspage)->gotDB = gotSave;

         /*
          * RAISE_INTERRUPT calls Switch{32,64}NMIHandler in switchNMI.S
          * (depending on host bitsize).
          */
         gotSave = SWITCHNMI(crosspage)->gotNMI;
         SWITCHNMI(crosspage)->gotNMI = 0;
         RAISE_INTERRUPT(2);
         TS_ASSERT(SWITCHNMI(crosspage)->gotNMI);

#if defined(VM_X86_64) && defined(__GNUC__)
         /*
          * Test the LRETQ in the 64-bit mini NMI handler to make sure
          * it works with any 16-byte offset of the stack pointer.
          * The INT 2 calls Switch64NMIHandler in switchNMI.S.
          */
         {
            uint64 v1, v2;

            asm volatile ("\n"
                  "        movl    $16, %%ecx      \n"
                  "1000:                           \n"
                  "        decq    %%rsp           \n"
                  "        movb    $0xDB, (%%rsp)  \n"
                  "        int     $2              \n"
                  "        loop    1000b           \n"
                  "        popq    %%rcx           \n"
                  "        popq    %%rax           \n"
                  : "=a" (v1), "=c" (v2));

            /*
             * Ensure nothing overwritten just above where it is
             * allowed to, because the decq rsp/movb 0xDBs pushed 16
             * of them one byte at a time.
             */
            TS_ASSERT(v1 == 0xDBDBDBDBDBDBDBDBULL);
            TS_ASSERT(v2 == 0xDBDBDBDBDBDBDBDBULL);
         }
#endif
         SWITCHNMI(crosspage)->gotNMI = gotSave;

         /*
          * RAISE_INTERRUPT calls Switch{32,64}MCEHandler in switchNMI.S
          * (depending on host bitsize).
          */
         gotSave = SWITCHNMI(crosspage)->gotMCE;
         SWITCHNMI(crosspage)->gotMCE = 0;
         RAISE_INTERRUPT(18);
         TS_ASSERT(SWITCHNMI(crosspage)->gotMCE);
         SWITCHNMI(crosspage)->gotMCE = gotSave;
      }

#ifdef VM_X86_64
      /*
       * Clear CR3.PCD and CR3.PWT bits.
       */
      if ((cr3reg & CR3_IGNORE) != 0) {
         uintptr_t new_cr3 = cr3reg & ~CR3_IGNORE;
         SET_CR3(new_cr3);
      }
#endif

      /*
       * Following Intel's recommendation, we clear all the reserved
       * bits in CR4.
       *
       * CR4.PGE is cleared to ensure global pages are flushed.
       */
      new_cr4 = cr4reg & ~(CR4_PGE | CR4_RESERVED);
      SET_CR4(new_cr4);

      crosspage->hostCR4 = new_cr4;

      TaskSaveDebugRegisters(crosspage);

      TaskSaveGDT64(&hostGDT64);

      /*
       * If NMI stress testing enabled, set EFLAGS<TF>.  This will
       * make sure there is a valid IDT, GDT, stack, etc. at every
       * instruction boundary during the switch.
       */
      if (WS_NMI_STRESS) {
         TaskEnableTF();
      }

      /*
       * If this section is enabled, it will send the host an NMI via
       * the local APIC to make sure it really can handle NMIs.
       */
      if (FALSE && vmx86_debug) {
         uint32 index, timeout;

         static uint32 countdowns[2] = { 0, 0 };

         index = crosspage->runVmm64;
         TS_ASSERT((index == 0) || (index == 1));
         if (++countdowns[index] >= 100000) {
            countdowns[index] = 0;
            SWITCHNMI(crosspage)->gotNMI = 0;      // no NMI seen
            for (timeout = 0; --timeout != 0;) {   // wait for LAPIC idle
               if (!(vm->hostAPIC[APICR_ICRLO][0] & 0x1000)) {
                  break;
               }
            }
            TS_ASSERT(timeout != 0);
            index = vm->hostAPIC[APICR_ID][0];
            vm->hostAPIC[APICR_ICRHI][0] = index;  // send NMI to self
            vm->hostAPIC[APICR_ICRLO][0] = 0x402;  // (shorthand doesn't work)
            for (timeout = 0; --timeout != 0;) {   // wait for NMI delivery
               if (SWITCHNMI(crosspage)->gotNMI) {
                  break;
               }
            }
            TS_ASSERT(timeout != 0);
         }
      }

      /*
       * If this section is enabled, it will force an NMI loop on each
       * switch so we can test each callback to make sure it can
       * survive a loop.
       */
      if (FALSE && vmx86_debug) {
         if (!loopForced) {
            loopForced = TRUE;
            SWITCHNMI(crosspage)->gotNMI = 1;
         }
      }

      /*
       * GS and FS are saved outside of the SwitchToMonitor() code to
       *
       * 1) minimize the amount of code handled there, and
       *
       * 2) prevent us from faulting if they happen to be in the LDT
       *    (since the LDT is saved and restored here too).
       *
       * Also, the 32-bit Mac OS running in legacy mode has
       * CS, DS, ES, SS in the LDT!
       */

      cs = GET_CS();
      ss = GET_SS();
#if defined __APPLE__ && vm_x86_64
      /*
       * The 64-bit Mac OS kernel leaks segment selectors from
       * other threads into 64-bit threads.  When the selectors
       * reference a foreign thread's LDT, we may not be able to
       * reload them using our thread's LDT.  So, let's just clear
       * them instead of trying to preserve them.  [PR 467140]
       */
      ds = 0;
      es = 0;
      fs = 0;
      gs = 0;
#else
      ds = GET_DS();
      es = GET_ES();
      fs = GET_FS();
      gs = GET_GS();
#endif
      GET_LDT(hostLDT);
      GET_TR(hostTR);

      if (TaskInLongMode()) {
         kgs64 = GET_KernelGS64();
         gs64  = GET_GS64();
         fs64  = GET_FS64();
      }

      /*
       * Make sure stack segment is non-zero so worldswitch can use it
       * to temporarily restore DS,ES on return.
       */
      if (vm_x86_64 && (ss == 0)) {
         SET_SS(kernelStackSegment);
      }

#if defined __APPLE__ && !vm_x86_64
      if (!TaskInLongMode()) {
         /*
          * This is 32-bit Mac OS running in legacy mode, which has an
          * LDT-based CS,DS,SS.  So switch to GDT segments as the worldswitch
          * must have segments that are in the crossGDT so the segments will
          * remain valid throughout worldswitching.
          */
         if (SELECTOR_TABLE(cs) == SELECTOR_LDT) {
            __asm__ __volatile__("pushl %0"  "\n\t"
                                 "pushl $1f" "\n\t"
                                 "lretl"     "\n"
                                 "1:"
                                 :
                                 : "i" (KERNEL32_CS));
         }
         if (SELECTOR_TABLE(ds) == SELECTOR_LDT) {
            SET_DS(KERNEL32_DS);
         }
         if (SELECTOR_TABLE(ss) == SELECTOR_LDT) {
            SET_SS(KERNEL32_DS);
         }
      } else
#endif
      {
         TS_ASSERT(SELECTOR_TABLE(cs) == SELECTOR_GDT);
         TS_ASSERT(SELECTOR_TABLE(ds) == SELECTOR_GDT);
         TS_ASSERT(SELECTOR_TABLE(ss) == SELECTOR_GDT);
      }

      DEBUG_ONLY(crosspage->tinyStack[0] = 0xDEADBEEF;)
      SwitchToMonitor(crosspage);
      TS_ASSERT(crosspage->tinyStack[0] == 0xDEADBEEF);

      /*
       * Restore CR state.  The monitor shouldn't have modified CR8.
       */
      SET_CR0(cr0reg);
      SET_CR2(cr2reg);
      SET_CR4(cr4reg);
#ifdef VM_X86_64
      if ((cr3reg & CR3_IGNORE) != 0) {
         SET_CR3(cr3reg);
      }
#endif

      /*
       * SwitchToMonitor returns with GDT = crossGDT so switch back to the host
       * GDT here.  We will also restore host TR as the task busy bit needs to
       * be fiddled with.  Also restore host LDT while we're at it.
       */
      RestoreHostGDTTRLDT(vm->currentHostCpu[vcpuid],
                          crosspage,
                          hostGDT64,
                          hostLDT,
                          cs,
                          hostTR);

#if defined __APPLE__ && !vm_x86_64
      if (!TaskInLongMode()) {
         if (SELECTOR_TABLE(cs) == SELECTOR_LDT) {
            __asm__ __volatile__("pushl %0"  "\n\t"
                                 "pushl $1f" "\n\t"
                                 "lretl"     "\n"
                                 "1:"
                                 :
                                 : "g" ((uint32)cs));
         }
         if (SELECTOR_TABLE(ss) == SELECTOR_LDT) {
            SET_SS(ss);
         }
      }
#endif

      SET_DS(ds);
      SET_ES(es);
      /*
       * First, restore %fs and %gs from the in-memory descriptor tables,
       * and then overwrite the bases in the descriptor cache with the
       * saved 64-bit values.
       */
      SET_FS(fs);
      SET_GS(gs);
      if (TaskInLongMode()) {
         SET_FS64(fs64);
         SET_GS64(gs64);
         SET_KernelGS64(kgs64);
      }

      /* Restore debug registers and host's IDT; turn off stress test. */
      if (WS_NMI_STRESS) {
         TaskDisableTF();
      }

      TaskRestoreDebugRegisters(crosspage);

      ASSERT_NO_INTERRUPTS();

      /*
       * Restore standard host interrupt table.
       */
      TaskLoadIDT64(&hostIDT64);

      vm->currentHostCpu[vcpuid] = INVALID_HOST_CPU;
      /*
       * If we got an NMI, do an INT 2 so the host will see it.  Theoretically,
       * since the switchNMI handlers return with POPF/LRET instead of IRET,
       * and we don't execute any IRET, NMI delivery should still be inhibited
       * by the PCPU.  So using an INT 2 to forward the NMI to the host should
       * work as NMI delivery is still inhibited.
       *
       * Don't bother with the INT 2 if stress testing or the host (Linux
       * anyway) will bleep out a log message each time.
       *
       * Likewise, if MCE, do an INT 18.
       */
      if (UNLIKELY(SWITCHNMI(crosspage)->gotNMI)) {
         SWITCHNMI(crosspage)->gotNMI = 0;
         if (!WS_NMI_STRESS && !loopForced) {
            RAISE_INTERRUPT(2);
         }
      }

      if (UNLIKELY(SWITCHNMI(crosspage)->gotMCE)) {
         SWITCHNMI(crosspage)->gotMCE = 0;
         if (!WS_NMI_STRESS && !loopForced) {
            if (vmx86_debug) {
               CP_PutStr("Task_Switch*: forwarding MCE to host\n");
            }
            RAISE_INTERRUPT(18);
         }
      }

      /*
       * It is possible that the gotNMI and/or gotMCE was detected when
       * switching in the host->monitor direction, in which case the
       * retryWorldSwitch flag will be set.  If such is the case, we want to
       * immediately loop back to the monitor as that is what it is expecting
       * us to do.
       */
      if (LIKELY(crosspage->retryWorldSwitch == 0)) {
         break;
      }
      crosspage->retryWorldSwitch = 0;
   }

   if (crosspage->moduleCallType == MODULECALL_INTR) {
      /*
       * Note we must do the RAISE_INTERRUPT before ever enabling
       * interrupts or bad things have happened (might want to know exactly
       * what bad things btw).
       * Note2: RAISE_INTERRUPT() only takes an constant and hence with switch
       * statement.
       */

#define IRQ_INT(_x) case _x: RAISE_INTERRUPT(_x); break
#define IRQ_INT2(_x) IRQ_INT(_x); IRQ_INT(_x + 1)
#define IRQ_INT4(_x) IRQ_INT2(_x); IRQ_INT2(_x + 2)
#define IRQ_INT8(_x) IRQ_INT4(_x); IRQ_INT4(_x + 4)
#define IRQ_INT16(_x) IRQ_INT8(_x); IRQ_INT8(_x + 8)
#define IRQ_INT32(_x) IRQ_INT16(_x); IRQ_INT16(_x + 16)

      switch (crosspage->args[0]) {
         // These are the general IO interrupts
         // It would be nice to generate this dynamically, but see Note2 above.

         /*
          * Pass Machine Check Exception (Interrupt 0x12) to the host.
          * See bug #45286 for details.
          */
         IRQ_INT(0x12);

         /*
          * pass the reserved vectors (20-31) as well. amd64 windows
          * generates these.
          */
         IRQ_INT8(0x14);
         IRQ_INT4(0x1c);

         IRQ_INT32(0x20);
         IRQ_INT32(0x40);
         IRQ_INT32(0x60);
         IRQ_INT32(0x80);
         IRQ_INT32(0xa0);
         IRQ_INT32(0xc0);
         IRQ_INT32(0xe0);

      default:
         /*
          * XXXX nt
          * running on a 2 processor machine we hit this Panic with int 0xD1 0x61 ...
          */
         Warning("Received Unexpected Interrupt: 0x%X in Task_Switch()\n", crosspage->args[0]);
         Panic("Received Unexpected Interrupt: 0x%X\n", crosspage->args[0]);
      }
   }

   RESTORE_FLAGS(flags);
   RestoreNMI(vm, lint0NMI, lint1NMI, pcNMI, thermalNMI);
}
