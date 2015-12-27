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
 * hostif.c --
 *
 *    This file implements the platform-specific (here Linux) interface that
 *    the cross-platform code uses --hpreg
 *
 */


/* Must come before any kernel header file --hpreg */
#include "driver-config.h"

/* Must come before vmware.h --hpreg */
#include "compat_page.h"
#include <linux/binfmts.h>
#include "compat_fs.h"
#include "compat_sched.h"
#include "compat_slab.h"
#if defined(KERNEL_2_4_22)
#   include <linux/vmalloc.h>
   /* 2.4 kernels don't define VM_MAP.  Just use VM_ALLOC instead. */
#   ifndef VM_MAP
#      define VM_MAP VM_ALLOC
#   endif
#endif

#include <linux/poll.h>
#include <linux/mman.h>

#include <linux/smp.h>

#include <asm/io.h>
#include <linux/mc146818rtc.h>
#include <linux/capability.h>

#include "compat_uaccess.h"
#include "compat_highmem.h"
#include "compat_mm.h"
#include "compat_file.h"
#include "compat_wait.h"
#include "compat_completion.h"
#include "compat_kernel.h"
#include "compat_timer.h"
#include "compat_kthread.h"

#include "vmware.h"
#include "x86apic.h"
#include "vm_asm.h"
#include "modulecall.h"
#include "memtrack.h"
#include "phystrack.h"
#include "cpuid.h"
#include "cpuid_info.h"
#include "hostif.h"
#include "driver.h"
#include "vmhost.h"
#include "x86msr.h"

#include "pgtbl.h"
#include "vmmonInt.h"
#include "versioned_atomic.h"

/* 
 * Determine if we can use high resolution timers.
 */

#   ifdef CONFIG_HIGH_RES_TIMERS
#      include <linux/hrtimer.h>
#      define VMMON_USE_HIGH_RES_TIMERS
#      if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 28)
#         define VMMON_USE_SCHEDULE_HRTIMEOUT
#      else
#         define VMMON_USE_COMPAT_SCHEDULE_HRTIMEOUT
static void HostIFWakeupClockThread(unsigned long data);
static DECLARE_TASKLET(timerTasklet, HostIFWakeupClockThread, 0);
#      endif
#      define close_rtc(filp, files) do {} while(0)
#   else 
#      define close_rtc(filp, files) compat_filp_close(filp, files)
#   endif

#define UPTIME_FREQ CONST64(1000000)

/*
 * Linux seems to like keeping free memory around 30MB
 * even under severe memory pressure.  Let's give it a little 
 * more leeway than that for safety.
 */
#define LOCKED_PAGE_SLACK 10000

static struct {
   Atomic_uint64     uptimeBase;
   VersionedAtomic   version;
   uint64            monotimeBase;
   unsigned long     jiffiesBase;
   struct timer_list timer;
} uptimeState;

COMPAT_KTHREAD_DECLARE_STOP_INFO();

/*
 * First Page Locking strategy
 * ---------------------------
 *
 * An early implementation hacked the lock bit for the purpose of locking
 * memory. This had a couple of advantages:
 *   - the vmscan algorithm would never eliminate mappings from the process
 *     address space
 *   - easy to assert that things are ok
 *   - it worked with anonymous memory. Basically, vmscan jumps over these
 *     pages, their use count stays high, ....
 *
 * This approach however had a couple of problems:
 *
 *   - it relies on an undocumented interface. (in another words, a total hack)
 *   - it creates deadlock situations if the application gets a kill -9 or
 *     otherwise dies ungracefully. linux first tears down the address space,
 *     then closes file descriptors (including our own device). Unfortunately,
 *     this leads to a deadlock of the process on pages with the lock bit set.
 *
 *     There is a workaround for that, namely to detect that condition using
 *     a linux timer. (ugly)
 *
 * Current Page Locking strategy
 * -----------------------------
 *
 * The current scheme does not use the lock bit, rather it increments the use
 * count on the pages that need to be locked down in memory.
 *
 * The problem is that experiments on certain linux systems (e.g. 2.2.0-pre9)
 * showed that linux somehow swaps out anonymous pages, even with the
 * increased ref counter.
 * Swapping them out to disk is not that big of a deal, but bringing them back
 * to a different location is.  In any case, anonymous pages in linux are not
 * intended to be write-shared (e.g. try to MAP_SHARED /dev/zero).
 *
 * As a result, the current locking strategy requires that all locked pages are
 * backed by the filesystem, not by swap. For now, we use both mapped files and
 * sys V shared memory. The user application is responsible to cover these
 * cases.
 *
 * About MemTracker and PhysTracker
 * --------------------------------
 *
 * Redundancy is good for now. 
 * 
 * MemTracker is actually required for the NT host version
 * For the linux host, we use both for now
 *
 */


#define HOST_UNLOCK_PFN(_vm, _pfn) do {                  \
   PhysTrack_Remove((_vm)->vmhost->physTracker, (_pfn)); \
   put_page(pfn_to_page(_pfn));                          \
} while (0)

#define HOST_UNLOCK_PFN_BYMPN(_vm, _pfn) do {            \
   PhysTrack_Remove((_vm)->vmhost->lockedPages, (_pfn)); \
   put_page(pfn_to_page(_pfn));                          \
} while (0)

#define HOST_ISTRACKED_PFN(_vm, _pfn) \
   (PhysTrack_Test((_vm)->vmhost->physTracker, (_pfn)))


/*
 *-----------------------------------------------------------------------------
 *
 * MutexInit --
 *
 *      Initialize a Mutex. --hpreg
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

#ifdef VMX86_DEBUG
static INLINE void
MutexInit(Mutex *mutex,     // IN
          char const *name) // IN
{
   ASSERT(mutex);
   ASSERT(name);

   sema_init(&mutex->sem, 1);
   mutex->name = name;
   mutex->cur.pid = -1;
}
#else
#   define MutexInit(_mutex, _name) sema_init(&(_mutex)->sem, 1)
#endif


#ifdef VMX86_DEBUG
/*
 *-----------------------------------------------------------------------------
 *
 * MutexIsLocked --
 *
 *      Determine if a Mutex is locked by the current thread. --hpreg
 *
 * Results:
 *      TRUE if yes
 *      FALSE if no
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static INLINE Bool
MutexIsLocked(Mutex *mutex) // IN
{
   ASSERT(mutex);

   return mutex->cur.pid == current->pid;
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * MutexLock --
 *
 *      Acquire a Mutex. --hpreg
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

#ifdef VMX86_DEBUG
static INLINE void
MutexLock(Mutex *mutex, // IN
          int callerID) // IN
{
   ASSERT(mutex);
   ASSERT(!MutexIsLocked(mutex));

   down(&mutex->sem);
   mutex->cur.pid = current->pid;
   mutex->cur.callerID = callerID;
}
#else
#   define MutexLock(_mutex, _callerID) down(&(_mutex)->sem)
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * MutexUnlock --
 *
 *      Release a Mutex. --hpreg
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

#ifdef VMX86_DEBUG
static INLINE void
MutexUnlock(Mutex *mutex, // IN
            int callerID) // IN
{
   ASSERT(mutex);

   ASSERT(MutexIsLocked(mutex) && mutex->cur.callerID == callerID);
   mutex->prev = mutex->cur;
   mutex->cur.pid = -1;
   up(&mutex->sem);
}
#else
#   define MutexUnlock(_mutex, _callerID) up(&(_mutex)->sem)
#endif


/* This mutex protects the driver-wide state. --hpreg */
static Mutex globalMutex;

/*
 * This mutex protects the fast clock rate and is held while
 * creating/destroying the fastClockThread.  It ranks below
 * globalMutex.  We can't use globalMutex for this purpose because the
 * fastClockThread itself acquires the globalMutex, so trying to hold
 * the mutex while destroying the thread can cause a deadlock.
 */
static Mutex fastClockMutex;

/* This mutex protects linuxState.pollList.  */
static Mutex pollListMutex;

/*
 *----------------------------------------------------------------------
 *
 * HostIF_YieldCPU --
 *
 *      Yield the CPU.
 *
 *      usecs action
 *    -----   ------
 *    0       yield, if possible
 *    !0      yield by sleeping for the specified number of usecs.
 *
 * Results:
 *      The current CPU yields whenever possible.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
HostIF_YieldCPU(uint32 usecs)  // IN:
{
   if (usecs == 0) {
      compat_yield();
   } else {
      /* a sub-jiffy request will sleep for a jiffy */
      compat_msleep_interruptible(usecs / 1000);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_InitGlobalLock --
 *
 *      Initialize the global (across all VMs and vmmon) locks.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
HostIF_InitGlobalLock(void)
{
   MutexInit(&globalMutex, "global");
   MutexInit(&fastClockMutex, "fastClock");
   MutexInit(&pollListMutex, "pollList");
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_GlobalLock --
 *
 *      Grabs the global data structure lock.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Should be a very low contention lock. 
 *      The current thread is rescheduled if the lock is busy.
 *
 *-----------------------------------------------------------------------------
 */

void
HostIF_GlobalLock(int callerID) // IN
{
   MutexLock(&globalMutex, callerID);
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_GlobalUnlock --
 *
 *      Releases the global data structure lock.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
HostIF_GlobalUnlock(int callerID) // IN
{
   MutexUnlock(&globalMutex, callerID);
}


#ifdef VMX86_DEBUG
/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_GlobalLockIsHeld --
 *
 *      Determine if the global lock is held by the current thread.
 * 
 * Results:
 *      TRUE if yes
 *      FALSE if no
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
HostIF_GlobalLockIsHeld(void)
{
   return MutexIsLocked(&globalMutex);
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_FastClockLock --
 *
 *      Grabs the fast clock data structure lock.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Should be a very low contention lock. 
 *      The current thread is rescheduled if the lock is busy.
 *
 *-----------------------------------------------------------------------------
 */

void
HostIF_FastClockLock(int callerID) // IN
{
   MutexLock(&fastClockMutex, callerID);
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_FastClockUnlock --
 *
 *      Releases the fast clock data structure lock.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
HostIF_FastClockUnlock(int callerID) // IN
{
   MutexUnlock(&fastClockMutex, callerID);
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_PollListLock --
 *
 *      Grabs the linuxState.pollList lock.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      The current thread is rescheduled if the lock is busy.
 *
 *-----------------------------------------------------------------------------
 */

void
HostIF_PollListLock(int callerID) // IN
{
   MutexLock(&pollListMutex, callerID);
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_PollListUnlock --
 *
 *      Releases the linuxState.pollList lock.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
HostIF_PollListUnlock(int callerID) // IN
{
   MutexUnlock(&pollListMutex, callerID);
}


#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 3)
static INLINE void
down_write_mmap(void)
{
   down_write(&current->mm->mmap_sem);
}


static INLINE void
up_write_mmap(void)
{
   up_write(&current->mm->mmap_sem);
}


static INLINE void
down_read_mmap(void)
{
   down_read(&current->mm->mmap_sem);
}


static INLINE void
up_read_mmap(void)
{
   up_read(&current->mm->mmap_sem);
}
#else
static INLINE void
down_write_mmap(void)
{
   down(&current->mm->mmap_sem);
}


static INLINE void
up_write_mmap(void)
{
   up(&current->mm->mmap_sem);
}


static INLINE void
down_read_mmap(void)
{
   down_write_mmap();
}


static INLINE void
up_read_mmap(void)
{
   up_write_mmap();
}
#endif

/*
 *----------------------------------------------------------------------
 * 
 * MapCrossPage & UnmapCrossPage
 *
 *    Both x86-64 and ia32 need to map crosspage to an executable 
 *    virtual address.  We use the vmap interface to do this on 2.4.22 
 *    and greater kernels.  We can't just use the kmap interface due 
 *    to bug 43907.
 *
 *    Pre-vmap kernels use the kmap interface.  On 32 bit hosts this 
 *    protects us from the kernel unmapping the crosspage out from under us 
 *    on smp systems.   On pre-vmap noexec kernels (2.4.20 & 2.4.21 
 *    x86-64 and some 2.4.21 RH ia32 kernel) we also need to manually 
 *    clear the nx bit.  Actually two NX bits as ia32 2.4.21 mirrors
 *    hardware NX bit (63) in bit 9 so it has all flags in low 12 bits
 *    of pte.
 *
 *    We currently prefer vmap over kmap as this gives us simpler
 *    #ifdefs and hopefully better compatibility with future kernels.
 *
 * Side effects:
 *
 *    UnmapCrossPage assumes that the page has been refcounted up
 *    so it takes care of the put_page.
 *
 *----------------------------------------------------------------------
 */
#ifndef KERNEL_2_4_22
#ifdef _PAGE_NX
static void
TLBInvalidatePage(void *vaddr)  // IN:
{
   TLB_INVALIDATE_PAGE(vaddr);
}

static void
DoClearNXBit(VA vaddr)  // IN:
{
   int ptemap;
   pgd_t *pgd = pgd_offset_k(vaddr);
   pmd_t *pmd = pmd_offset_map(pgd, vaddr);
   pte_t *pte;

   if ((ptemap = pmd_val(*pmd) & _PAGE_PSE) == 0) {
      pte = pte_offset_map(pmd, vaddr);
   } else {
      pte = (pte_t*)pmd;
   }
   if (pte_val(*pte) & _PAGE_NX) {
      /* pte_val() is not lvalue on x86 PAE */
#ifdef CONFIG_X86_PAE
      pte->pte_low  &= ~_PAGE_NX;    /* Clear software NX bit (if present) */
      pte->pte_high &= ~(1UL << 31); /* Clear hardware NX bit */
#else
      pte_val(*pte) &= ~_PAGE_NX;
#endif
      compat_smp_call_function(TLBInvalidatePage, (void *)vaddr, 1);
      TLBInvalidatePage((void *)vaddr);
   }
   if (ptemap) {
      pte_unmap(pte);
   }
   pmd_unmap(pmd);
}


static INLINE void
ClearNXBit(VA vaddr)  // IN:
{
   if (_PAGE_NX != 0) {
      DoClearNXBit(vaddr);
   }
}
#else /* _PAGE_NX */
static INLINE void
ClearNXBit(VA vaddr)  // IN:
{
   /* No _PAGE_NX => no noexec => nothing to do */
}
#endif /* _PAGE_NX */


static void *
MapCrossPage(struct page *p)  // IN:
{
   void *va = kmap(p);
   ClearNXBit((VA)va);
   return va;
}


static void
UnmapCrossPage(struct page *p,  // IN:
               void *va)        // IN:
{
   kunmap(p);
   put_page(p);
}
#else /* KERNEL_2_4_22 */
static void *
MapCrossPage(struct page *p)  // IN:
{
   return vmap(&p, 1, VM_MAP, VM_PAGE_KERNEL_EXEC);
}


static void
UnmapCrossPage(struct page *p,  // IN:
               void *va)        // IN:
{
   vunmap(va);
   put_page(p);
}
#endif /* KERNEL_2_4_22 */


/*
 *----------------------------------------------------------------------
 *
 * HostIFHostMemInit --
 *
 *      Initialize per-VM pages lists.
 *
 * Results:
 *      0 on success,
 *      non-zero on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
HostIFHostMemInit(VMDriver *vm)  // IN:
{
   VMHost *vmh = vm->vmhost;
   
   vmh->lockedPages = PhysTrack_Alloc();
   if (!vmh->lockedPages) {
      return -1;
   }
   vmh->AWEPages = PhysTrack_Alloc();
   if (!vmh->AWEPages) {
      return -1;
   }

   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIFHostMemCleanup --
 *
 *      Release per-VM pages lists.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Locked and AWE pages are released.
 *
 *----------------------------------------------------------------------
 */

static void
HostIFHostMemCleanup(VMDriver *vm)  // IN:
{
   MPN mpn;
   VMHost *vmh = vm->vmhost;

   if (!vmh) {
      return;
   }

   if (vmh->lockedPages) {
      for (mpn = 0; 
           INVALID_MPN != (mpn = PhysTrack_GetNext(vmh->lockedPages, mpn));) {
         HOST_UNLOCK_PFN_BYMPN(vm, mpn);
      }
      PhysTrack_Cleanup(vmh->lockedPages);
      vmh->lockedPages = NULL;
   }

   if (vmh->AWEPages) {
      for (mpn = 0; 
           INVALID_MPN != (mpn = PhysTrack_GetNext(vmh->AWEPages, mpn));) {
	 PhysTrack_Remove(vmh->AWEPages, mpn);
         put_page(pfn_to_page(mpn));
      }
      PhysTrack_Cleanup(vmh->AWEPages);
      vmh->AWEPages = NULL;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_AllocMachinePage --
 *
 *      Alloc non-swappable memory page. The page is not billed to  
 *      a particular VM. Preferably the page should not be mapped into
 *      the kernel addresss space.
 *
 * Results:
 *      INVALID_MPN or a valid host mpn.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

MPN
HostIF_AllocMachinePage(void)
{
  struct page *pg = alloc_page(GFP_HIGHUSER);

  return (pg) ? page_to_pfn(pg) : INVALID_MPN;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_FreeMachinePage --
 *
 *      Free an anonymous machine page allocated by 
 *      HostIF_AllocMachinePage().  This page is not tracked in any 
 *      phystracker.
 *
 * Results:
 *      Host page is unlocked.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
HostIF_FreeMachinePage(MPN mpn)  // IN:
{
  struct page *pg = pfn_to_page(mpn);

  __free_page(pg);
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_AllocLockedPages --
 *
 *      Alloc non-swappable memory.
 *
 * Results:
 *      negative value on complete failure
 *      non-negative value on partial/full completion, number of MPNs
 *          allocated & filled in pmpn returned.
 *
 * Side effects:
 *      Pages allocated.
 *
 *----------------------------------------------------------------------
 */

int
HostIF_AllocLockedPages(VMDriver *vm,	     // IN: VM instance pointer
			VA64 addr,	     // OUT: pointer to user or kernel buffer for MPNs 
			unsigned numPages,   // IN: number of pages to allocate
			Bool kernelMPNBuffer)// IN: is the MPN buffer in kernel or user address space?
{
   MPN32 *pmpn = VA64ToPtr(addr);
   VMHost *vmh = vm->vmhost;
   unsigned int cnt;
   int err = 0;

   if (!vmh || !vmh->AWEPages) {
      return -EINVAL;
   }
   for (cnt = 0; cnt < numPages; cnt++) {
      struct page* pg;
      MPN32 mpn;
      
      pg = alloc_page(GFP_HIGHUSER);
      if (!pg) {
         err = -ENOMEM;
	 break;
      }
      mpn = page_to_pfn(pg);
      ASSERT(mpn == (MPN32)mpn);
      if (kernelMPNBuffer) {
         *pmpn = mpn;
      } else if (HostIF_CopyToUser(pmpn, &mpn, sizeof *pmpn) != 0) {
	__free_page(pg);
	err = -EFAULT;
	break;
      }
      pmpn++;
      if (PhysTrack_Test(vmh->AWEPages, mpn)) {
	Warning("%s: duplicate MPN %#x\n", __FUNCTION__, mpn);
      }
      PhysTrack_Add(vmh->AWEPages, mpn);
   }

   return cnt ? cnt : err;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_FreeLockedPages --
 *
 *      Free non-swappable memory.
 *
 * Results:
 *      On success: 0. All pages were unlocked.
 *      On failure: Non-zero system error code. No page was unlocked.
 *
 * Side effects:
 *      Pages freed.
 *
 *----------------------------------------------------------------------
 */

int
HostIF_FreeLockedPages(VMDriver *vm,	     // IN: VM instance pointer
		       VA64 addr,            // IN: user or kernel array of MPNs 
		       unsigned numPages,    // IN: number of pages to free
		       Bool kernelMPNBuffer) // IN: is the MPN buffer in kernel or user address space?
{
   MPN32 const *pmpn = VA64ToPtr(addr);
   VMHost *vmh = vm->vmhost;
   unsigned int cnt;
   struct page *pg; 
   MPN32 mpns[64];
      
   if (!vmh || !vmh->AWEPages) {
      return -EINVAL;
   }

   if (!kernelMPNBuffer) {
      if (numPages > ARRAYSIZE(mpns)) {
         return -EINVAL;
      }

      if (HostIF_CopyFromUser(mpns, pmpn, numPages * sizeof *pmpn)) {
         printk(KERN_DEBUG "Cannot read from process address space at %p\n",
                pmpn);

         return -EINVAL;
      }

      pmpn = mpns;
   }

   for (cnt = 0; cnt < numPages; cnt++) {
      if (!PhysTrack_Test(vmh->AWEPages, pmpn[cnt])) {
         printk(KERN_DEBUG "Attempted to free unallocated MPN %08X\n",
                pmpn[cnt]);

         return -EINVAL;
      }

      pg = pfn_to_page(pmpn[cnt]);
      if (page_count(pg) != 1) {
         // should this case be considered a failure?
         printk(KERN_DEBUG "Page %08X is still used by someone "
                "(use count %u, VM %p)\n", pmpn[cnt], page_count(pg), vm);
      }
   }

   for (cnt = 0; cnt < numPages; cnt++) {     
      pg = pfn_to_page(pmpn[cnt]);
      PhysTrack_Remove(vmh->AWEPages, pmpn[cnt]);
      __free_page(pg);
   }

   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_Init --
 *
 *      Initialize the host-dependent part of the driver.
 *
 * Results:
 *     zero on success, non-zero on error.
 *
 * Side effects:
 *     None
 *
 *----------------------------------------------------------------------
 */

int
HostIF_Init(VMDriver *vm)  // IN:
{
   int i;

   vm->memtracker = MemTrack_Init();
   if (vm->memtracker == NULL) {
      return -1;
   }

   vm->vmhost = (VMHost *) HostIF_AllocKernelMem(sizeof *vm->vmhost, TRUE);
   if (vm->vmhost == NULL) {
      return -1;
   }
   memset(vm->vmhost, 0, sizeof *vm->vmhost);

   vm->vmhost->physTracker = PhysTrack_Init();
   if (vm->vmhost->physTracker == NULL) {
      return -1;
   }

   init_waitqueue_head(&vm->vmhost->callQueue);
   atomic_set(&vm->vmhost->pendingUserCalls, 0);

   for (i = 0; i < MAX_INITBLOCK_CPUS; i++) {
      init_waitqueue_head(&vm->vmhost->replyQueue[i]);
   }

   if (HostIFHostMemInit(vm)) {
      return -1;
   }
   MutexInit(&vm->vmhost->vmMutex, "vm");

   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_InitEvent --
 *
 *      Initialize the user call return event objects on Windows.
 *      Nothing to do on Linux.
 *
 * Results:
 *     No.
 *
 * Side effects:
 *     No.
 *
 *----------------------------------------------------------------------
 */

void
HostIF_InitEvent(VMDriver *vm)  // IN:
{
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_MarkLockedVARangeClean --
 *    
 *     Clear the dirty bit in the HW page tables for [VA, VA+len) if
 *     the page is already locked by the monitor/userlevel.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     None.
 *
 *----------------------------------------------------------------------
 */

int
HostIF_MarkLockedVARangeClean(const VMDriver *vm,  // IN:
                              VA va,               // IN:
                              unsigned len,        // IN:
                              VA bv)               // IN:
{
   struct mm_struct *mm = current->mm;
   VA end = va + len;
   int i = 0;
   uint8 localBV[256];
   unsigned nPages = BYTES_2_PAGES(len);

   if (nPages > (256 * 8) || vm->vmhost->lockedPages == NULL) {
      return -EINVAL;
   }
   if (HostIF_CopyFromUser(localBV, (void *)bv, CEILING(nPages, 8)) != 0) {
      return -EINVAL;
   }
   if (compat_get_page_table_lock(mm)) {
      spin_lock(compat_get_page_table_lock(mm));
   }
   for (;va < end; va += PAGE_SIZE, i++) {
      MPN mpn;
      pte_t *pte;
      
      pte = PgtblVa2PTELocked(mm, va);
      if (pte != NULL) {
         /* PgtblPte2MPN does pte_present. */
         mpn = PgtblPte2MPN(pte);
         if (mpn != INVALID_MPN && pte_dirty(*pte)) {
            if (PhysTrack_Test(vm->vmhost->lockedPages, mpn)) {
               uint32 *p = (uint32 *)pte;
               int index = i >> 3;
               int offset = i & 7;
               localBV[index] |= (1 << offset);
               *p &= ~_PAGE_DIRTY;
            }
         }
         pte_unmap(pte);
      }
   }
   if (compat_get_page_table_lock(mm)) {
      spin_unlock(compat_get_page_table_lock(mm));
   }
   if (HostIF_CopyToUser((void *)bv, localBV, CEILING(nPages, 8))) {
      return -EFAULT;
   }

   return 0;
}


/*
 *------------------------------------------------------------------------------
 *
 * HostIF_LookupUserMPN --
 *
 *      Lookup the MPN of a locked user page by user VA.
 *
 * Results:
 *      Returned page is a valid MPN, zero on error. 
 *
 * Side effects:
 *     None
 *
 *------------------------------------------------------------------------------
 */

MPN 
HostIF_LookupUserMPN(VMDriver *vm, // IN: VMDriver
                     VA64 uAddr)   // IN: user VA of the page
{
   void *uvAddr = VA64ToPtr(uAddr);
   MPN mpn = PgtblVa2MPN((VA)uvAddr);

   /*
    * On failure, check whether the page is locked.
    *
    * While we don't require the page to be locked by HostIF_LockPage(),
    * it does provide extra information.
    *
    * -- edward
    */

   if (mpn == INVALID_MPN) {
      if (vm == NULL) {
	 mpn += PAGE_LOOKUP_NO_VM;
      } else {
	 MemTrackEntry *entryPtr =
	    MemTrack_LookupVPN(vm->memtracker, PTR_2_VPN(uvAddr));
	 if (entryPtr == NULL) {
	    mpn += PAGE_LOOKUP_NOT_TRACKED;
	 } else if (entryPtr->mpn == 0) {
	    mpn += PAGE_LOOKUP_NO_MPN;
	 } else if (!HOST_ISTRACKED_PFN(vm, entryPtr->mpn)) {
	    mpn += PAGE_LOOKUP_NOT_LOCKED;
	 } else {
	    /*
	     * Kernel can remove PTEs/PDEs from our pagetables even if pages
	     * are locked...
	     */
	    volatile int c;

	    compat_get_user(c, (char *)uvAddr);
	    mpn = PgtblVa2MPN((VA)uvAddr);
	    if (mpn == entryPtr->mpn) {
#ifdef VMX86_DEBUG	    
	       printk(KERN_DEBUG "Page %p disappeared from %s(%u)... "
                      "now back at %#x\n", 
		      uvAddr, current->comm, current->pid, mpn);
#endif
	    } else if (mpn != INVALID_MPN) {
	       printk(KERN_DEBUG "Page %p disappeared from %s(%u)... "
                      "now back at %#x (old=%#x)\n", uvAddr, current->comm, 
                      current->pid, mpn, entryPtr->mpn);
	       mpn = INVALID_MPN;
	    } else {
	       printk(KERN_DEBUG "Page %p disappeared from %s(%u)... "
                      "and is lost (old=%#x)\n", uvAddr, current->comm, 
                      current->pid, entryPtr->mpn);
	       mpn = entryPtr->mpn;
	    }
	 }
      }
   }

   return mpn;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_InitFP --
 *
 *      masks IRQ13 if not previously the case.
 *
 * Results:
 *      prevents INTR #0x2d (IRQ 13) from being generated --
 *      assume that Int16 works for interrupt reporting
 *      
 *
 * Side effects:
 *      PIC
 *
 *----------------------------------------------------------------------
 */

void
HostIF_InitFP(VMDriver *vm)  // IN:
{
   int mask = (1 << (0xD - 0x8));

   uint8 val = inb(0xA1);

   if (!(val & mask)) { 
      val = val | mask;
      outb(val, 0xA1);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIFGetUserPage --
 *
 *      Lock the page of an user-level address space in memory.
 *	If ppage is NULL, page is only marked as dirty.
 *
 * Results:
 *      Zero on success, non-zero on failure. 
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static int
HostIFGetUserPage(void *uvAddr,		// IN
		  struct page** ppage)	// OUT
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 19)
   int retval;
      
   down_read(&current->mm->mmap_sem);
   retval = get_user_pages(current, current->mm, (unsigned long)uvAddr, 
                           1, 0, 0, ppage, NULL);
   up_read(&current->mm->mmap_sem);

   return retval != 1;
#else
   struct page* page;
   struct page* check;
   volatile int c;

   compat_get_user(c, (char *)uvAddr);

   /*
    * now locate it and write the results back to the pmap
    *
    * Under extreme memory pressure, the page may be gone again.
    * Just fail the lock in that case.  (It was ASSERT_BUG(6339, mpn).)
    * -- edward
    */

   page = PgtblVa2Page((VA)uvAddr);
   if (page == NULL) {
      return 1;
   }
   get_page(page);
   check = PgtblVa2Page((VA)uvAddr);
   if (page != check) {
      put_page(page);
      return 1;
   }
   if (ppage) {
      *ppage = page;
   } else {
      put_page(page);
   }

   return 0;
#endif
}


#if defined(__linux__) && defined(VMX86_DEVEL) && defined(VM_X86_64)
/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_LookupLargeMPN --
 *
 *      Gets the first MPN of a hugetlb page. 
 *
 * Results:
 *      The MPN or PAGE_LOCK_FAILED on an error. 
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

MPN
HostIF_LookupLargeMPN(void *uvAddr)  // IN: user VA of the page
{
   struct page *page;
   MPN mpn;

   if (HostIFGetUserPage(uvAddr, &page)) {
      return PAGE_LOCK_FAILED;
   }

   mpn = page_to_pfn(page);
   put_page(page);

   return mpn;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * HostIF_IsLockedByMPN --
 *
 *      Checks if mpn was locked using allowMultipleMPNsPerVA.  
 *
 * Results:
 *      TRUE if mpn is present in the physTracker.
 *      
 *
 * Side effects:
 *     None.
 *
 *----------------------------------------------------------------------
 */

Bool
HostIF_IsLockedByMPN(VMDriver *vm,  // IN:
                     MPN mpn)       // IN:
{
  return PhysTrack_Test(vm->vmhost->lockedPages, mpn);
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_LockPage --
 *
 *      Lockup the MPN of an pinned user-level address space 
 *
 * Results:
 *      The MPN or zero on an error. 
 *
 * Side effects:
 *      Adds the page to the MemTracker,
 *	if allowMultipleMPNsPerVA then the page is added 
 *      to the VM's PhysTracker.
 *
 *-----------------------------------------------------------------------------
 */

MPN
HostIF_LockPage(VMDriver *vm,		     // IN: VMDriver
                VA64 uAddr,		     // IN: user VA of the page
		Bool allowMultipleMPNsPerVA) // IN: allow to lock many pages per VA
{
   void *uvAddr = VA64ToPtr(uAddr);
   struct page *page;
   MPN mpn;
   VPN vpn;
   MemTrackEntry *entryPtr = NULL;

   vpn = PTR_2_VPN(uvAddr);
   if (!allowMultipleMPNsPerVA) {
      entryPtr = MemTrack_LookupVPN(vm->memtracker, vpn);
      
      /*
       * Already tracked and locked
       */

      if (entryPtr != NULL && entryPtr->mpn != 0) {
	 return PAGE_LOCK_ALREADY_LOCKED;
      }
   }

   if (HostIFGetUserPage(uvAddr, &page)) {
      return PAGE_LOCK_FAILED;
   }
   mpn = page_to_pfn(page);

   if (HOST_ISTRACKED_PFN(vm, mpn)) {
      Warning("%s vpn=%p mpn=%#x already tracked\n", __FUNCTION__,
              (void*)vpn, mpn);
      put_page(page);

      return PAGE_LOCK_PHYSTRACKER_ERROR;
   }

   if (allowMultipleMPNsPerVA) {
      /*
       *  Add the MPN to the PhysTracker that tracks locked pages.
       */

      struct PhysTracker* const pt = vm->vmhost->lockedPages;
      
      if (PhysTrack_Test(pt, mpn)) {
	 put_page(page);

	 return PAGE_LOCK_ALREADY_LOCKED;
      }
      PhysTrack_Add(pt, mpn);
   } else {
      /*
       * If the entry doesn't exist, add it to the memtracker
       * otherwise we just update the mpn.
       */

      if (entryPtr == NULL) {
	 entryPtr = MemTrack_Add(vm->memtracker, vpn, mpn);
	 if (entryPtr == NULL) {
	    HOST_UNLOCK_PFN(vm, mpn);

	    return PAGE_LOCK_MEMTRACKER_ERROR;
	 }
      } else {
	 entryPtr->mpn = mpn;  
      }
      PhysTrack_Add(vm->vmhost->physTracker, mpn);
   }

   return mpn;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_UnlockPage --
 *
 *      Unlock an pinned user-level page.
 *
 * Results:
 *      0 if successful, otherwise non-zero
 *      
 * Side effects:
 *     None
 *
 *----------------------------------------------------------------------
 */

int
HostIF_UnlockPage(VMDriver *vm,  // IN:
                  VA64 uAddr)    // IN:
{
   void *addr = VA64ToPtr(uAddr);
   VPN vpn;
   MemTrackEntry *e;

   vpn = VA_2_VPN((VA)addr);
   e = MemTrack_LookupVPN(vm->memtracker, vpn);
    
   if (e == NULL) {
      return PAGE_UNLOCK_NOT_TRACKED;
   }
   if (e->mpn == 0) {
      return PAGE_UNLOCK_NO_MPN;
   }

   HOST_UNLOCK_PFN(vm, e->mpn);
   e->mpn = 0;

   return PAGE_UNLOCK_NO_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_UnlockPageByMPN --
 *
 *      Unlock a locked user mode page. The page doesn't need to be mapped
 *      anywhere.
 *
 * Results:
 *      returns PAGE_UNLOCK_NO_ERROR, or PAGE_UNLOCK_xxx error code.
 *
 * Side effects:
 *     Removes the MPN from from VM's PhysTracker.
 *
 *----------------------------------------------------------------------
 */

int
HostIF_UnlockPageByMPN(VMDriver *vm, // IN: VMDriver
                       MPN mpn,	     // IN: the MPN to unlock
                       VA64 uAddr)   // IN: optional(debugging) VA for the MPN
{
   if (!PhysTrack_Test(vm->vmhost->lockedPages, mpn)) {
      return PAGE_UNLOCK_NO_MPN;
   }

#ifdef VMX86_DEBUG
   {
      void *va = VA64ToPtr(uAddr);
      MemTrackEntry *e;
      
      /*
       * Verify for debugging that VA and MPN make sense.
       * PgtblVa2MPN() can fail under high memory pressure.
       */

      if (va != NULL) {
         MPN lookupMpn = PgtblVa2MPN((VA)va);

         if (lookupMpn != INVALID_MPN && mpn != lookupMpn) {
            Warning("Page lookup fail %#x %#x %p\n", mpn, lookupMpn, va);

            return PAGE_LOOKUP_INVALID_ADDR;
         }
      }

      /*
       * Verify that this MPN was locked with 
       * HostIF_LockPage(allowMultipleMPNsPerVA = TRUE).
       * That means that this MPN should not be in the MemTracker.
       */

      e = MemTrack_LookupMPN(vm->memtracker, mpn);
      if (e) {
	 Warning("%s(): mpn=%#x va=%p was permanently locked with "
                 "vpn=0x%"FMT64"x\n", __FUNCTION__, mpn, va, e->vpn);

	 return PAGE_UNLOCK_MISMATCHED_TYPE;
      }
   }
#endif 

   HOST_UNLOCK_PFN_BYMPN(vm, mpn);

   return PAGE_UNLOCK_NO_ERROR;
}


static void 
UnlockEntry(void *clientData,         // IN:
            MemTrackEntry *entryPtr)  // IN:
{
   VMDriver *vm = (VMDriver *)clientData;

   if (entryPtr->mpn) {
      if (HOST_ISTRACKED_PFN(vm, entryPtr->mpn)) {
         HOST_UNLOCK_PFN(vm,entryPtr->mpn);
      } else { 
         Warning("%s vpn=0x%"FMT64"x mpn=%#x not owned\n", __FUNCTION__,
                 entryPtr->vpn, entryPtr->mpn);
      }
      entryPtr->mpn = 0;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_FreeAllResources --
 *
 *      Free all host-specific VM resources.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
HostIF_FreeAllResources(VMDriver *vm) // IN
{
   unsigned int cnt;

   HostIFHostMemCleanup(vm);
   if (vm->memtracker) {
      /*
       * If the memtracker is not empty, this call uses
       * 'vm->vmhost->physTracker'.
       */

      MemTrack_Cleanup(vm->memtracker, UnlockEntry, vm);
      vm->memtracker = NULL;
   }
   if (vm->vmhost) {
      for (cnt = vm->vmhost->crosspagePagesCount; cnt > 0; ) {
         struct page* p = vm->vmhost->crosspagePages[--cnt];
         UnmapCrossPage(p, vm->crosspage[cnt]);
      }
      vm->vmhost->crosspagePagesCount = 0;
      if (vm->vmhost->hostAPICIsMapped) {
	 ASSERT(vm->hostAPIC != NULL);
	 iounmap((void*)vm->hostAPIC);
	 vm->hostAPIC = NULL;
	 vm->vmhost->hostAPICIsMapped = FALSE;
      }
      if (vm->vmhost->physTracker) {
         PhysTrack_Cleanup(vm->vmhost->physTracker);
         vm->vmhost->physTracker = NULL;
      }
      HostIF_FreeKernelMem(vm->vmhost);
      vm->vmhost = NULL;
   }
}



/*
 *----------------------------------------------------------------------
 *
 * HostIF_AllocKernelMem
 *
 *      Allocate some kernel memory for the driver. 
 *
 * Results:
 *      The address allocated or NULL on error. 
 *      
 *
 * Side effects:
 *      memory is malloced
 *----------------------------------------------------------------------
 */

void *
HostIF_AllocKernelMem(size_t size,  // IN:
                      int wired)    // IN:
{
   void * ptr = kmalloc(size, GFP_KERNEL);
   
   if (ptr == NULL) { 
      Warning("%s failed (size=%p)\n", __FUNCTION__, (void*)size);
   }

   return ptr;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_AllocPage --
 *
 *    Allocate a page (whose content is undetermined)
 *
 * Results:
 *    The kernel virtual address of the page
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

void *
HostIF_AllocPage(void)
{
   VA kvAddr;
   
   kvAddr = __get_free_page(GFP_KERNEL);
   if (kvAddr == 0) {
      Warning("%s: __get_free_page() failed\n", __FUNCTION__);
   }

   return (void *)kvAddr;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_FreeKernelMem
 *
 *      Free kernel memory allocated for the driver. 
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      memory is freed.
 *----------------------------------------------------------------------
 */

void
HostIF_FreeKernelMem(void *ptr)  // IN:
{
   kfree(ptr);
}


void
HostIF_FreePage(void *ptr)  // IN:
{
   VA vAddr = (VA)ptr;

   if (vAddr & (PAGE_SIZE-1)) {
      Warning("%s %p misaligned\n", __FUNCTION__, (void*)vAddr);
   } else {
      free_page(vAddr);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_IsAnonPage --
 *
 *      Is the mpn an anonymous page we have given to the monitor?
 *
 * Results:
 *      True if the mpn is an anonymous page, false otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

INLINE_SINGLE_CALLER Bool
HostIF_IsAnonPage(VMDriver *vm,      // IN: VM instance pointer
                  MPN32 mpn)         // IN: MPN we are asking about
{
   VMHost *vmh = vm->vmhost;

   if (!vmh || !vmh->AWEPages) {
      return FALSE;
   }

   return PhysTrack_Test(vmh->AWEPages, mpn);
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_GetNUMAAnonPageDistribution --
 *
 *     Gets the Anonymous Page distribution on the different NUMA nodes
 *     Scans the PhysTracker for AWE Pages, and counts the number of MPNs
 *     on each NUMA node. Expects the buffer perNodeCnt to be of length 
 *     numNodes.
 *
 * Results:
 *      Returns TRUE  : success
 *              FALSE : failure 
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
HostIF_GetNUMAAnonPageDistribution(VMDriver *vm,      //IN
                                   int numNodes,      //IN
                                   uint32 *perNodeCnt)//OUT: Array of num
                                                      //MPNs on each node
{
   struct PhysTracker *AWEPagesPtr;   
   NUMA_Node node;
   MPN mpn;
   unsigned count;

   if (!perNodeCnt) {
      return FALSE;
   }
   memset(perNodeCnt, 0, (numNodes * sizeof *perNodeCnt)); 

   if (!vm || !vm->vmhost || !vm->vmhost->AWEPages) {
      return FALSE;
   }
   AWEPagesPtr =  vm->vmhost->AWEPages;
   
   if (AWEPagesPtr == NULL) {
      Warning("VM has no anonymous pages!\n");

      return FALSE;
   }

   /*
    * Iterating over the AWE Pages phystracker of the VM, which stores
    * MPNs of anonymous pages 
    */

   for (mpn = 0, count = 0; 
        (INVALID_MPN != (mpn = PhysTrack_GetNext(AWEPagesPtr, mpn)));
        count++) {

      node = Vmx86_MPNToNodeNum(mpn);
      ASSERT(node != INVALID_NUMANODE);
      ASSERT(node < NUMA_MAX_NODES);
      perNodeCnt[node]++;
   }  
   
   return TRUE;

}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_EstimateLockedPageLimit --
 *
 *      Estimates how many memory pages can be locked or allocated
 *      from the kernel without causing the host to die or to be really upset.
 *
 * Results:
 *	number of pages that can be locked 
 *      or 7/8ths of physical memory if not available.
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */

unsigned int
HostIF_EstimateLockedPageLimit(const VMDriver* vm,		  // IN
			       unsigned int currentlyLockedPages) // IN
{
   /*
    * This variable is available and exported to modules,
    * since at least 2.6.0.
    */

   extern unsigned long totalram_pages;
   unsigned int forHost;

   /* 
    * Allow to lock 87.5% of host's memory but 
    * leave at least 128 MB to the host. 
    */

   unsigned int totalPhysicalPages = totalram_pages;
   unsigned int reservedPages = (128 * 1024 * 1024) / PAGE_SIZE;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 28)
   unsigned int lowerBound = MIN(totalPhysicalPages, reservedPages);
   forHost = MAX(totalPhysicalPages / 8, lowerBound);
#else
   /*
    * Use the memory information linux exports as of late for a more
    * precise estimate of locked memory.  All kernel page-related structures
    * (slab, pagetable) are as good as locked.  Unevictable includes things
    * that are explicitly marked as such (like mlock()).  Huge pages are 
    * also as good as locked, since we don't use them.  Lastly, without 
    * available swap, anonymous pages become locked in memory as well. 
    */

   unsigned int lockedPages = global_page_state(NR_PAGETABLE) +
                              global_page_state(NR_SLAB_UNRECLAIMABLE) +
                              global_page_state(NR_UNEVICTABLE) +
                              reservedPages;
   unsigned int anonPages = global_page_state(NR_ANON_PAGES); 
   unsigned int swapPages = BYTES_2_PAGES(linuxState.swapSize);

   /*
    * vm can be NULL during early module initialization.
    */

   if (vm != NULL) {
      lockedPages += vm->memInfo.hugePageBytes;
      if (vm->vmhost->usingMlock) {
         /*
          * Our locked pages will be on unevictable list, don't double count.
          */

         lockedPages = currentlyLockedPages < lockedPages ? 
                       lockedPages - currentlyLockedPages : 0;
      } else if (vm->vmhost->swapBacked) {
         anonPages = currentlyLockedPages < anonPages ? 
                     anonPages - currentlyLockedPages : 0;
      }
   }
   if (anonPages > swapPages) {
      lockedPages += anonPages - swapPages; 
   }
   forHost = lockedPages + LOCKED_PAGE_SLACK;
   if (forHost > totalPhysicalPages) {
      forHost = totalPhysicalPages;
   }
#endif

   return totalPhysicalPages - forHost;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_Wait --
 *
 *      Waits for specified number of milliseconds.
 *
 *----------------------------------------------------------------------
 */

void
HostIF_Wait(unsigned int timeoutMs)
{
   compat_msleep_interruptible(timeoutMs);
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_WaitForFreePages --
 *
 *      Waits for pages to be available for allocation or locking.
 *
 * Results:
 *	New pages are likely to be available for allocation or locking.
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */

void 
HostIF_WaitForFreePages(unsigned int timeoutMs)  // IN:
{
   static unsigned count;
   compat_msleep_interruptible(timeoutMs);
   count++;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIFReadUptimeWork --
 *
 *      Reads the current uptime.  The uptime is based on getimeofday,
 *      which provides the needed high resolution.  However, we don't
 *      want uptime to be warped by e.g. calls to settimeofday.  So, we
 *      use a jiffies based monotonic clock to sanity check the uptime.
 *      If the uptime is more than one second from the monotonic time,
 *      we assume that the time of day has been set, and recalculate the
 *      uptime base to get uptime back on track with monotonic time.  On
 *      the other hand, we do expect jiffies based monotonic time and
 *      timeofday to have small drift (due to NTP rate correction, etc).
 *      We handle this by rebasing the jiffies based monotonic clock
 *      every second (see HostIFUptimeResyncMono).
 *      
 * Results:
 *      The uptime, in units of UPTIME_FREQ.  Also returns the jiffies
 *      value that was used in the monotonic time calculation.
 *
 * Side effects:
 *      May reset the uptime base in the case gettimeofday warp was 
 *      detected.
 *
 *----------------------------------------------------------------------
 */

static uint64
HostIFReadUptimeWork(unsigned long *j)  // OUT: current jiffies 
{
   struct timeval tv;
   uint64 monotime, uptime, upBase, newUpBase, monoBase;
   int64 diff;
   uint32 version;
   unsigned long jifs, jifBase;
   unsigned attempts = 0;
   /* Assert that HostIF_InitUptime has been called. */
   ASSERT(uptimeState.timer.function);

 retry:
   do {
      version  = VersionedAtomic_BeginTryRead(&uptimeState.version);
      jifs     = jiffies;
      jifBase  = uptimeState.jiffiesBase;
      monoBase = uptimeState.monotimeBase;
   } while (!VersionedAtomic_EndTryRead(&uptimeState.version, version));

   do_gettimeofday(&tv);
   upBase = Atomic_Read64(&uptimeState.uptimeBase);
   
   monotime = (uint64)(jifs - jifBase) * (UPTIME_FREQ / HZ);
   monotime += monoBase;

   uptime = tv.tv_usec * (UPTIME_FREQ / 1000000) + tv.tv_sec * UPTIME_FREQ;
   uptime += upBase;
   
   /* 
    * Use the jiffies based monotonic time to sanity check gettimeofday.
    * If they differ by more than one second, assume the time of day has
    * been warped, and use the jiffies time to undo (most of) the warp.
    */

   diff = uptime - monotime;
   if (UNLIKELY(diff < -UPTIME_FREQ || diff > UPTIME_FREQ)) {
      /* Compute a new uptimeBase to get uptime back on track. */
      newUpBase = monotime - (uptime - upBase);
      attempts++;
      if (!Atomic_CMPXCHG64(&uptimeState.uptimeBase, &upBase, &newUpBase) && 
          attempts < 5) {
         /* Another thread updated uptimeBase.  Recalculate uptime. */
         goto retry;
      }
      uptime = monotime;
   }

   if (UNLIKELY(attempts)) {
      Log("HostIF_ReadUptime: detected settimeofday: fixed uptimeBase "
          "old %"FMT64"u new %"FMT64"u attempts %u\n",
          upBase, newUpBase, attempts);
   }
   *j = jifs;

   return uptime;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIFUptimeResyncMono --
 *
 *      Timer that fires ever second to resynchronize the jiffies based
 *      monotonic time with the uptime.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Resets the monotonic time bases so that jiffies based monotonic
 *      time does not drift from gettimeofday over the long term.
 *
 *----------------------------------------------------------------------
 */

static void
HostIFUptimeResyncMono(unsigned long data)  // IN: ignored
{
   unsigned long jifs;
   uintptr_t flags;

   /* 
    * Read the uptime and the corresponding jiffies value.  This will
    * also correct the uptime (which is based on time of day) if needed
    * before we rebase monotonic time (which is based on jiffies).
    */

   uint64 uptime = HostIFReadUptimeWork(&jifs);

   /* 
    * Every second, recalculate monoBase and jiffiesBase to squash small
    * drift between gettimeofday and jiffies.  Also, this prevents
    * (jiffies - jiffiesBase) wrap on 32-bits.
    */

   SAVE_FLAGS(flags);
   CLEAR_INTERRUPTS();
   VersionedAtomic_BeginWrite(&uptimeState.version);

   uptimeState.monotimeBase = uptime;
   uptimeState.jiffiesBase  = jifs;

   VersionedAtomic_EndWrite(&uptimeState.version);
   RESTORE_FLAGS(flags);

   /* Reschedule this timer to expire in one second. */
   mod_timer(&uptimeState.timer, jifs + HZ);
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_InitUptime --
 *
 *      Initialize the uptime clock's state.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Sets the initial values for the uptime state, and schedules
 *      the uptime timer.
 *
 *----------------------------------------------------------------------
 */

void
HostIF_InitUptime(void)
{
   struct timeval tv;

   uptimeState.jiffiesBase = jiffies;
   do_gettimeofday(&tv);
   Atomic_Write64(&uptimeState.uptimeBase, 
                  -(tv.tv_usec * (UPTIME_FREQ / 1000000) + 
                    tv.tv_sec * UPTIME_FREQ));

   init_timer(&uptimeState.timer);
   uptimeState.timer.function = HostIFUptimeResyncMono;
   mod_timer(&uptimeState.timer, jiffies + HZ);
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_CleanupUptime --
 *
 *      Cleanup uptime state, called at module unloading time.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Deschedule the uptime timer.
 *
 *----------------------------------------------------------------------
 */

void
HostIF_CleanupUptime(void)
{
   compat_del_timer_sync(&uptimeState.timer);
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_ReadUptime --
 *
 *      Read the system time.  Returned value has no particular absolute
 *      value, only difference since previous call should be used.
 *
 * Results:
 *      Units are given by HostIF_UptimeFrequency.
 *
 * Side effects:
 *      See HostIFReadUptimeWork
 *
 *----------------------------------------------------------------------
 */

uint64
HostIF_ReadUptime(void)
{
   unsigned long jifs;

   return HostIFReadUptimeWork(&jifs);
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_UptimeFrequency
 *
 *      Return the frequency of the counter that HostIF_ReadUptime reads.
 *
 * Results:
 *      Frequency in Hz.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

uint64
HostIF_UptimeFrequency(void)
{
   return UPTIME_FREQ;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_CopyFromUser --
 *
 *      Copy memory from the user application into a kernel buffer. This
 *      function may block, so don't call it while holding any kind of
 *      lock. --hpreg
 *
 * Results:
 *      0 on success
 *      -EFAULT on failure.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

int
HostIF_CopyFromUser(void *dst,	      // OUT
                    const void *src,  // IN
                    unsigned int len) // IN
{
   return copy_from_user(dst, src, len) ? -EFAULT : 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_CopyToUser --
 *
 *      Copy memory to the user application from a kernel buffer. This
 *      function may block, so don't call it while holding any kind of
 *      lock. --hpreg
 *
 * Results:
 *      0 on success
 *      -EFAULT on failure.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

int 
HostIF_CopyToUser(void *dst,	    // OUT
                  const void *src,  // IN
                  unsigned int len) // IN
{
   return copy_to_user(dst, src, len) ? -EFAULT : 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_MapCrossPage --
 *    
 *    Obtain kernel pointer to crosspage. 
 *
 *    We must return a VA that is obtained through a kernel mapping, so that 
 *    the mapping never goes away (see bug 29753).
 *
 *    However, the LA corresponding to that VA must not overlap with the 
 *    monitor (see bug 32922). The userland code ensures that by only 
 *    allocating cross pages from low memory. For those pages, the kernel 
 *    uses a permanent mapping, instead of a temporary one with a high LA.
 *
 * Results:
 *    The kernel virtual address on success
 *    NULL on failure
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

void *
HostIF_MapCrossPage(VMDriver *vm, // IN
                    VA64 uAddr)   // IN
{
   void *p = VA64ToPtr(uAddr);
   struct page *page;
   VA           vPgAddr;
   VA           ret;

   if (HostIFGetUserPage(p, &page)) {
      return NULL;
   }
   vPgAddr = (VA) MapCrossPage(page);
   HostIF_GlobalLock(16);
   if (vm->vmhost->crosspagePagesCount >= MAX_INITBLOCK_CPUS) {
      HostIF_GlobalUnlock(16);
      UnmapCrossPage(page, (void*)vPgAddr);

      return NULL;
   }
   vm->vmhost->crosspagePages[vm->vmhost->crosspagePagesCount++] = page;
   HostIF_GlobalUnlock(16);

   ret = vPgAddr | (((VA)p) & (PAGE_SIZE - 1));

   return (void*)ret;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_AllocCrossGDT --
 *
 *      Allocate the per-vmmon cross GDT page set.
 *
 *      See bora/doc/worldswitch-pages.txt for the requirements on the cross
 *      GDT page set addresses.
 *
 * Results:
 *      On success: Host kernel virtual address of the first cross GDT page.
 *                  Use HostIF_FreeCrossGDT() with the same value to free.
 *                  The 'crossGDTMPNs' array is filled with the MPNs of all the
 *                  cross GDT pages.
 *      On failure: NULL.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void *
HostIF_AllocCrossGDT(uint32 numPages,   // IN: Number of pages
                     MPN maxValidFirst, // IN: Highest valid MPN of first page
                     MPN *crossGDTMPNs) // OUT: Array of MPNs
{
   MPN startMPN;
   struct page *pages;
   uint32 i;
   void *crossGDT;

   /*
    * In practice, allocating a low page (MPN <= 0x100000 - 1) is equivalent to
    * allocating a page with MPN <= 0xFEC00 - 1:
    *
    * o PC architecture guarantees that there is no RAM in top 16MB of 4GB
    *   range.
    *
    * o 0xFEC00000 is IOAPIC base.  There could be RAM immediately below,
    *   but not above.
    *
    * How do we allocate a low page? We can safely use GFP_DMA32 when
    * available.  On 64bit kernels before GFP_DMA32 was introduced we
    * fall back to DMA zone (which is not quite necessary for boxes
    * with less than ~3GB of memory).  On 32bit kernels we are using
    * normal zone - which is usually 1GB, and at most 4GB (for 4GB/4GB
    * kernels).  And for 4GB/4GB kernels same restriction as for 64bit
    * kernels applies - there is no RAM in top 16MB immediately below
    * 4GB so alloc_pages() cannot return such page.
    */

   ASSERT(0xFEC00 - 1 <= maxValidFirst);
   for (i = 0; (1 << i) < numPages; i++) { }
#ifdef VM_X86_64
#   ifdef GFP_DMA32
   pages = alloc_pages(GFP_KERNEL | GFP_DMA32, i);
#   else
   pages = alloc_pages(GFP_KERNEL | GFP_DMA, i);
#   endif
#else
   pages = alloc_pages(GFP_KERNEL, i);
#endif
   crossGDT = NULL;
   if (pages == NULL) {
      Warning("%s: unable to alloc crossGDT (%u)\n", __FUNCTION__, i);
   } else {
      startMPN = page_to_pfn(pages);
      for (i = 0; i < numPages; i++) {
         crossGDTMPNs[i] = startMPN + i;
      }
      crossGDT = (void *)page_address(pages);
   }

   return crossGDT;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_FreeCrossGDT --
 *
 *      Free the per-vmmon cross GDT page set allocated with
 *      HostIF_AllocCrossGDT().
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

void
HostIF_FreeCrossGDT(uint32 numPages, // IN: Number of pages
                    void *crossGDT)  // IN: Kernel VA of first cross GDT page
{
   uint32 i;

   for (i = 0; (1 << i) < numPages; i++) { }
   free_pages((VA)crossGDT, i);
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_VMLock --
 *
 *      Grabs per-VM data structure lock. The lock is not recursive.
 *      The global lock has lower rank so the global lock should be grabbed
 *      first if both locks are acquired.
 *
 *      It should be a medium contention lock. Also it should be fast:
 *      it is used for protecting of frequent page allocation and locking.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      The current thread is rescheduled if the lock is busy.
 *
 *-----------------------------------------------------------------------------
 */

void
HostIF_VMLock(VMDriver *vm, // IN
              int callerID) // IN
{
   ASSERT(vm);

   ASSERT(vm->vmhost);
   MutexLock(&vm->vmhost->vmMutex, callerID);
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_VMUnlock --
 *
 *      Releases per-VM data structure lock.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Can wake up the thread blocked on this lock. 
 *
 *-----------------------------------------------------------------------------
 */

void
HostIF_VMUnlock(VMDriver *vm, // IN
                int callerID) // IN
{
   ASSERT(vm);

   ASSERT(vm->vmhost);
   MutexUnlock(&vm->vmhost->vmMutex, callerID);
}


#ifdef VMX86_DEBUG
/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_VMLockIsHeld --
 *
 *      Determine if the per-VM lock is held by the current thread.
 * 
 * Results:
 *      TRUE if yes
 *      FALSE if no
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
HostIF_VMLockIsHeld(VMDriver *vm) // IN
{
   ASSERT(vm);

   ASSERT(vm->vmhost);
   return MutexIsLocked(&vm->vmhost->vmMutex);
}
#endif


/*
 * Utility routines for accessing and enabling the APIC
 */

/*
 * Defines for accessing the APIC.  We use readl/writel to access the APIC
 * which is how Linux wants you to access I/O memory (though on the x86
 * just dereferencing a pointer works just fine).
 */
#define APICR_TO_ADDR(apic, reg)      (apic + (reg << 4))
#define GET_APIC_REG(apic, reg)       (readl(APICR_TO_ADDR(apic, reg)))
#define SET_APIC_REG(apic, reg, val)  (writel(val, APICR_TO_ADDR(apic, reg)))

#define APIC_MAXLVT(apic)             ((GET_APIC_REG(apic, APICR_VERSION) >> 16) & 0xff)
#define APIC_VERSIONREG(apic)         (GET_APIC_REG(apic, APICR_VERSION) & 0xff)


/*
 *----------------------------------------------------------------------
 *
 * GetMSR --
 *
 *      Wrapper for the macro that calls the rdmsr instruction.
 *
 * Results:
 *      Returns the value of the MSR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE uint64
GetMSR(int index)  // IN:
{
   uint64 msr = __GET_MSR (index);

   return msr;
}


#if defined(CONFIG_SMP) || defined(CONFIG_X86_UP_IOAPIC) || \
    defined(CONFIG_X86_UP_APIC) || defined(CONFIG_X86_LOCAL_APIC)
/*
 *----------------------------------------------------------------------
 *
 * isVAReadable --
 *
 *      Verify that passed VA is accessible without crash...
 *
 * Results:
 *      TRUE if address is readable, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
 
static Bool
isVAReadable(VA r)  // IN:
{
   mm_segment_t old_fs;
   uint32 dummy;
   int ret;
   
   old_fs = get_fs();
   set_fs(get_ds());
   r = APICR_TO_ADDR(r, APICR_VERSION);
   ret = HostIF_CopyFromUser(&dummy, (void*)r, sizeof(dummy));
   set_fs(old_fs);

   return ret == 0;
}


/*
 *----------------------------------------------------------------------
 *
 * SetVMAPICPtr --
 *
 *      Sets a pointer to the APIC's virtual address in the VMDriver
 *      structure.  
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The VMDriver structure is updated.
 *
 *----------------------------------------------------------------------
 */

static void
SetVMAPICPtr(VMDriver *vm, // IN/OUT: driver state
	     MPN mpn)	   // IN: host APIC's mpn
{
   volatile void *hostapic;

   hostapic = (volatile void *) ioremap_nocache(MPN_2_MA(mpn), PAGE_SIZE);
   if (hostapic) {
      if ((APIC_VERSIONREG(hostapic) & 0xF0) == 0x10) {
	 vm->hostAPIC = (volatile uint32 (*)[4]) hostapic;
	 ASSERT(vm->vmhost != NULL);
	 vm->vmhost->hostAPICIsMapped = TRUE;
      } else {
	 iounmap((void*)hostapic);
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * ProbeAPIC --
 *
 *      Find the base physical address of the APIC.  On P6 family
 *      processors, this is done by reading the address from an MSR.
 *
 * Results:
 *      TRUE if APIC was found, FALSE if not.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Bool
ProbeAPIC(VMDriver *vm,       // IN/OUT: driver state
	  Bool setVMPtr)      // IN: set a pointer to the APIC's virtual address
{
   uint64 msr;
   MPN    mpn;
   uint64 mpn64; /* MPN just 32 bits currently; K8L may need more, in theory */
   CpuidVendors cpuVendor = CPUID_GetVendor();

   if (cpuVendor == CPUID_VENDOR_INTEL || cpuVendor == CPUID_VENDOR_AMD) {
      uint32 version = CPUID_GetVersion();
      uint32 features = CPUID_GetFeatures();

      if ((features & CPUID_FEATURE_COMMON_ID1EDX_MSR) &&
	  (features & CPUID_FEATURE_COMMON_ID1EDX_APIC)) {

	 /* APIC is present and enabled */
         if (CPUID_FAMILY_IS_P6(version)       || 
             CPUID_FAMILY_IS_PENTIUM4(version) ||
             CPUID_FAMILY_IS_K8STAR(version)) {
            msr = GetMSR(MSR_APIC_BASE);
            mpn64 = msr >> 12;
            if (CPUID_FAMILY_IS_K8(version)) {
               mpn64 &= 0xFFFFFFF;
            } else if (CPUID_FAMILY_IS_K8L(version)) {
               mpn64 &= 0xFFFFFFFFFull;
            } else {
               mpn64 &= 0xFFFFFF;
            }

            mpn = (MPN)mpn64;
            if (mpn != mpn64) {
               /* Not expected, but conceivable with K8L */
               Warning("Unable to handle local APIC base MSR value "
                       "0x%"FMT64"x.\n",
                       msr);

               return FALSE;
            }

	    if (setVMPtr) {
	       /*
                * Obtain a system address the APIC, only for P6.
		* Not recommended for P5, per Intel Book 3, page 7-16.
                */

	       SetVMAPICPtr(vm, mpn);
	    } else {
	       vm->hostAPIC = NULL;
	    }

	    return TRUE;
	 }
      }
   }

   return FALSE;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * HostIF_APICInit --
 *
 *      Initialize APIC behavior.
 *
 * Results:
 *      TRUE if everything went fine.
 *
 * Side effects:
 *     None
 *
 *----------------------------------------------------------------------
 */

Bool
HostIF_APICInit(VMDriver *vm,   // IN:
                Bool setVMPtr,  // IN:
                Bool probe)     // IN:
{
#if defined(CONFIG_SMP) || defined(CONFIG_X86_UP_IOAPIC) || \
    defined(CONFIG_X86_UP_APIC) || defined(CONFIG_X86_LOCAL_APIC)
   VA kAddr;

   /* APIC support may be compiled in with APIC disabled - Bug 61969. */
   if ((CPUID_GetFeatures() & CPUID_FEATURE_COMMON_ID1EDX_APIC) == 0) {
      return TRUE;
   }   

   if (probe) {
      if (ProbeAPIC(vm, setVMPtr)) {
	 return TRUE;
      }
   }

   kAddr = __fix_to_virt(FIX_APIC_BASE);
   if (!isVAReadable(kAddr)) {
      return TRUE;
   }
   if (setVMPtr) {
      vm->hostAPIC = (void *)kAddr;
   } else {
      vm->hostAPIC = NULL;
   }
#endif
   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_APIC_ID --
 *
 *      Read the local APIC ID from the APIC ID register
 *
 * Results:
 *      Returned value is APIC ID, or APIC_INVALID_ID if
 *      it is not present or disabled.
 *      
 * Side effects:
 *     None
 *
 *----------------------------------------------------------------------
 */

uint8
HostIF_APIC_ID(void)
{
#if defined(CONFIG_SMP) || defined(CONFIG_X86_UP_IOAPIC) || \
    defined(CONFIG_X86_UP_APIC) || defined(CONFIG_X86_LOCAL_APIC)

   VA kAddr;
   volatile void* apic;

   /* APIC support may be compiled in with APIC disabled - Bug 61969. */
   if ((CPUID_GetFeatures() & CPUID_FEATURE_COMMON_ID1EDX_APIC) == 0) {
      return APIC_INVALID_ID;
   }   

   kAddr = __fix_to_virt(FIX_APIC_BASE);
   if (!isVAReadable(kAddr)) {
      return APIC_INVALID_ID;
   }
   apic = (volatile void*) kAddr;
   return (GET_APIC_REG(apic, APICR_ID) & XAPIC_ID_MASK) >> APIC_ID_SHIFT;
#else
   return APIC_INVALID_ID;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_SemaphoreWait --
 *
 *    Perform the semaphore wait (P) operation, possibly blocking.
 *
 * Result:
 *    1 (which equals MX_WAITNORMAL) if success, 
 *    negated error code otherwise.
 *
 * Side-effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

int   
HostIF_SemaphoreWait(VMDriver *vm,   // IN:
                     Vcpuid vcpuid,  // IN:
                     uint32 *args)   // IN:
{
   struct file *file;
   mm_segment_t old_fs;
   int res;
   int waitFD = args[0];
   int timeoutms = args[2];
   uint64 value;

   file = vmware_fget(waitFD);
   if (file == NULL) {
      return MX_WAITERROR;
   }

   old_fs = get_fs();
   set_fs(get_ds());

   {
      compat_poll_wqueues table;
      poll_table *wait;
      unsigned int mask;
      
      compat_poll_initwait(wait, &table);
      current->state = TASK_INTERRUPTIBLE;
      mask = file->f_op->poll(file, wait);
      if (!(mask & (POLLIN | POLLERR | POLLHUP))) {
	 vm->vmhost->vcpuSemaTask[vcpuid] = current;
	 schedule_timeout(timeoutms * HZ / 1000);  // convert to Hz
	 vm->vmhost->vcpuSemaTask[vcpuid] = NULL;
      }
      current->state = TASK_RUNNING;
      compat_poll_freewait(wait, &table);
   }

   /*
    * Userland only writes in multiples of sizeof(uint64). This will allow
    * the code to happily deal with a pipe or an eventfd. We only care about
    * reading no bytes (EAGAIN - non blocking fd) or sizeof(uint64).
    */

   res = file->f_op->read(file, (char *) &value, sizeof value, &file->f_pos);

   if (res == sizeof value) {
      res = MX_WAITNORMAL;
   } else {
      if (res == 0) {
         res = -EBADF;
      }
   }

   set_fs(old_fs);
   compat_fput(file);

   /*
    * Handle benign errors:
    * EAGAIN is MX_WAITTIMEDOUT.
    * The signal-related errors are all mapped into MX_WAITINTERRUPTED.
    */

   switch (res) {
   case -EAGAIN:
      res = MX_WAITTIMEDOUT;
      break;
   case -EINTR:
   case -ERESTART:
   case -ERESTARTSYS:
   case -ERESTARTNOINTR:
   case -ERESTARTNOHAND:
      res = MX_WAITINTERRUPTED;
      break;
   case -EBADF:
      res = MX_WAITERROR;
      break;
   }
   return res;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_SemaphoreForceWakeup --
 *
 *    If the target process is sleeping lightly(i.e. TASK_INTERRUPTIBLE)
 *    wake it up.
 *
 * Result:
 *    None.
 *
 * Side-effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

void 
HostIF_SemaphoreForceWakeup(VMDriver *vm,   // IN:
                            Vcpuid vcpuid)  // IN:
{
   struct task_struct *t = vm->vmhost->vcpuSemaTask[vcpuid];

   if (t && (t->state & TASK_INTERRUPTIBLE)) {
      wake_up_process(t);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_SemaphoreSignal --
 *
 *      Perform the semaphore signal (V) operation.
 *
 * Result:
 *      On success: MX_WAITNORMAL (1).
 *      On error: MX_WAITINTERRUPTED (3) if interrupted by a Unix signal (we
 *                   can block on a preemptive kernel).
 *                MX_WAITERROR (0) on generic error.
 *                Negated system error (< 0).
 *
 * Side-effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

int
HostIF_SemaphoreSignal(uint32 *args)  // IN:
{
   struct file *file;
   mm_segment_t old_fs;
   int res;
   int signalFD = args[1];
   uint64 value = 1;  // make an eventfd happy should it be there

   file = vmware_fget(signalFD);
   if (!file) {
      return MX_WAITERROR;
   }

   old_fs = get_fs();
   set_fs(get_ds());

   /*
    * Always write sizeof(uint64) bytes. This works fine for eventfd and
    * pipes. The data written is formatted to make an eventfd happy should
    * it be present.
    */

   res = file->f_op->write(file, (char *) &value, sizeof value, &file->f_pos);

   if (res == sizeof value) {
      res = MX_WAITNORMAL;
   }

   set_fs(old_fs);
   compat_fput(file);

   /*
    * Handle benign errors:
    * EAGAIN is MX_WAITTIMEDOUT.
    * The signal-related errors are all mapped into MX_WAITINTERRUPTED.
    */

   switch (res) {
   case -EAGAIN:
      // The pipe is full, so it is already signalled. Success.
      res = MX_WAITNORMAL;
      break;
   case -EINTR:
   case -ERESTART:
   case -ERESTARTSYS:
   case -ERESTARTNOINTR:
   case -ERESTARTNOHAND:
      res = MX_WAITINTERRUPTED;
      break;
   }
   return res;
}


   /*
    * The COW stuff gets us wrapped around GPL compliance issues. One gets IPI
    * targetting when things are aligned with the COW stuff - beta and release
    * builds (at the time of writing this comment).
    */

#   if ((LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 27)) || !defined(CONFIG_SMP))
#      define VMMON_USE_CALL_FUNC
#   endif

#if defined(VMMON_USE_CALL_FUNC)
/*
 *----------------------------------------------------------------------
 *
 * LinuxDriverIPIHandler  --
 *
 *      Null IPI handler - for monitor to notice AIO completion
 *
 *----------------------------------------------------------------------
 */
void
LinuxDriverIPIHandler(void *info)
{
   return;
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 17)
#define VMMON_CALL_FUNC_SYNC 0  // async; we've not seen any problems
#else
#define VMMON_CALL_FUNC_SYNC 1  // sync; insure no problems from old releases
#endif

#endif


/*
 *----------------------------------------------------------------------
 *
 * HostIF_IPI --
 *
 *    If the passed VCPU threads are on some CPUs in the system,
 *    attempt to hit them with an IPI.  If "all" is true, the caller
 *    wants us to hit them all; if not, hitting at least one is
 *    sufficient.
 *
 *    On older Linux systems we do a broadcast.
 *
 * Result:
 *    TRUE if any IPIs were sent, FALSE if none.  Also indicate
 *    whether a broadcast was used in *didBroadcast.
 *
 *----------------------------------------------------------------------
 */

Bool
HostIF_IPI(VMDriver *vm,         // IN:
           VCPUSet ipiTargets,   // IN:
           Bool all,             // IN:
	   Bool *didBroadcast)   // OUT:
{
   Vcpuid v;
   Bool ret = FALSE;
   uint32 targetHostCpu;

   ASSERT(vm);

   *didBroadcast = FALSE;
   while ((v = VCPUSet_FindFirst(ipiTargets)) != VCPUID_INVALID) {
      targetHostCpu = vm->currentHostCpu[v];
      if (targetHostCpu != INVALID_HOST_CPU) {
         ASSERT(targetHostCpu < MAX_PROCESSORS);
         ret = TRUE;

#if defined(VMMON_USE_CALL_FUNC)
         /* older kernels IPI broadcast; use async when possible */
         (void) compat_smp_call_function(LinuxDriverIPIHandler,
                                         NULL, VMMON_CALL_FUNC_SYNC);

	 *didBroadcast = TRUE;
	 break;
#else
         /* Newer kernels have (async) IPI targetting */
         arch_send_call_function_single_ipi(targetHostCpu);
         if (!all) {
            break;
         }
#endif
      }
      ipiTargets = VCPUSet_Remove(ipiTargets, v);
   }

   return ret;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_UserCall --
 *
 *	Ask the main thread to process a cross user call.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
HostIF_UserCall(VMDriver *vm,   // IN:
                Vcpuid vcpuid)  // IN:
{
   ASSERT(!vm->vmhost->replyWaiting[vcpuid]);
   vm->vmhost->replyWaiting[vcpuid] = TRUE;
   atomic_inc(&vm->vmhost->pendingUserCalls);
   wake_up(&vm->vmhost->callQueue);
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_UserCallWait --
 *
 *	Wait for a cross user call to complete.
 *
 * Results:
 *	TRUE if successful (call completed).
 *	FALSE otherwise
 *	   signals pending,
 *	   timed out,
 *	   or otherwise unsuccessful
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
HostIF_UserCallWait(VMDriver *vm,   // IN:
                    Vcpuid vcpuid,  // IN:
                    int timeoutms)  // IN:
{
   if (vm->vmhost->replyWaiting[vcpuid]) {
      wait_queue_t wait;
      wait_queue_head_t *q = &vm->vmhost->replyQueue[vcpuid];

      current->state = TASK_INTERRUPTIBLE;
      init_waitqueue_entry(&wait, current);
      add_wait_queue(q, &wait);
      if (vm->vmhost->replyWaiting[vcpuid]) {
	 schedule_timeout(timeoutms * HZ / 1000); // convert to Hz
      }
      current->state = TASK_RUNNING;
      remove_wait_queue(q, &wait);
   }

   return !vm->vmhost->replyWaiting[vcpuid] && !signal_pending(current);
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_AwakenVcpu --
 *
 *      Make the VCPU thread continue upon completion of a user call.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      VCPU thread continues.
 *
 *----------------------------------------------------------------------
 */

void
HostIF_AwakenVcpu(VMDriver *vm,   // IN:
                  Vcpuid vcpuid)  // IN:
{
   ASSERT(vm->vmhost->replyWaiting[vcpuid]);
   vm->vmhost->replyWaiting[vcpuid] = FALSE;
   wake_up(&vm->vmhost->replyQueue[vcpuid]);
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_AckUserCall --
 *
 *      Host-dependent part of acknowledgement of user call notification.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Yes.
 *
 *----------------------------------------------------------------------
 */

void
HostIF_AckUserCall(VMDriver *vm,   // IN:
                   Vcpuid vcpuid)  // IN:
{
   atomic_sub(1, &vm->vmhost->pendingUserCalls);
}


typedef struct {
   Atomic_uint32 index;
   CPUIDQuery *query;
} HostIFGetCpuInfoData;


/*
 *-----------------------------------------------------------------------------
 *
 * HostIFGetCpuInfo --
 *
 *      Collect CPUID information on the current logical CPU.
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
HostIFGetCpuInfo(void *clientData) // IN/OUT: A HostIFGetCpuInfoData *
{
   HostIFGetCpuInfoData *data = (HostIFGetCpuInfoData *)clientData;
   CPUIDQuery *query;
   uint32 index;

   ASSERT(data);
   query = data->query;
   ASSERT(query);

   index = Atomic_ReadInc32(&data->index);
   if (index >= query->numLogicalCPUs) {
      return;
   }

   query->logicalCPUs[index].tag = HostIF_GetCurrentPCPU();
   __GET_CPUID2(query->eax, query->ecx, &query->logicalCPUs[index].regs);
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_GetAllCpuInfo --
 *
 *      Collect CPUID information on all logical CPUs.
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
HostIF_GetAllCpuInfo(CPUIDQuery *query) // IN/OUT
{
   HostIFGetCpuInfoData data;

   Atomic_Write32(&data.index, 0);
   data.query = query;

   /*
    * XXX Linux has userland APIs to bind a thread to a processor, so we could
    *     probably implement this in userland like we do on Win32.
    */

   HostIF_CallOnEachCPU(HostIFGetCpuInfo, &data);

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


/*
 *----------------------------------------------------------------------
 *
 * HostIF_CallOnEachCPU --
 *
 *      Call specified function once on each CPU.  No ordering guarantees.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.  May be slow.
 *
 *----------------------------------------------------------------------
 */

void
HostIF_CallOnEachCPU(void (*func)(void*), // IN: function to call
                     void *data)          // IN/OUT: argument to function
{
   compat_preempt_disable();
   (*func)(data);
   (void)compat_smp_call_function(*func, data, 1);
   compat_preempt_enable();
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_BrokenCPUHelper --
 *
 *      Collect broken CPU flag for all machine CPU id numbers.
 *
 * Results:
 *      bitmask of broken CPU machine id numbers.
 *
 * Side effects:
 *      none.
 *
 *----------------------------------------------------------------------
 */

static void 
HostIFBrokenCPUHelper(void *badcpumaskv) // IN/OUT 
{
   if (Vmx86_BrokenCPUHelper ()) {
      Atomic_Or (Atomic_VolatileToAtomic ((uint32 volatile *)badcpumaskv), 
                                          1 << smp_processor_id());
   }
}

uint32
HostIF_BrokenCPUHelper(void)
{
   uint32 volatile badcpumask = 0;

   compat_preempt_disable();
   HostIFBrokenCPUHelper((void*)&badcpumask); // run on this machine CPU
   (void)compat_smp_call_function(HostIFBrokenCPUHelper, (void*)&badcpumask, 
                                  1); // run on all other machine CPUs
   compat_preempt_enable();

   return badcpumask;
}

/*
 *----------------------------------------------------------------------
 *
 * HostIF_ReadPage --
 *
 *      puts the content of a machine page into a kernel or user mode 
 *      buffer. 
 *
 * Results:
 *	0 on success
 *	negative error code on error
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */

int 
HostIF_ReadPage(MPN mpn,	    // MPN of the page
		VA64 addr,	    // buffer for data
		Bool kernelBuffer)  // is the buffer in kernel space?
{
   void *buf = VA64ToPtr(addr);
   int ret = 0;
   const void* ptr;
   struct page* page; 
   
   if (mpn == INVALID_MPN) {
      return -EFAULT;
   }   
   page = pfn_to_page(mpn);
   ptr = kmap(page);
   if (ptr == NULL) {
      return -ENOMEM;
   }
   
   if (kernelBuffer) {
      memcpy(buf, ptr, PAGE_SIZE);
   } else {
      ret = HostIF_CopyToUser(buf, ptr, PAGE_SIZE);
   }
   kunmap(page);

   return ret;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_WritePage --
 *
 *      Put the content of a kernel or user mode buffer into a machine 
 *      page.
 *
 * Results:
 *	0 on success
 *	negative error code on error
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */

int 
HostIF_WritePage(MPN mpn,	    // MPN of the page
		 VA64 addr,         // data to write to the page
		 Bool kernelBuffer) // is the buffer in kernel space?
{
   void const *buf = VA64ToPtr(addr);
   int ret = 0;
   void* ptr;
   struct page* page;  

   if (mpn == INVALID_MPN) {
      return -EFAULT;
   }   
   page = pfn_to_page(mpn);
   ptr = kmap(page);
   if (ptr == NULL) {
      return -ENOMEM;
   }
   
   if (kernelBuffer) {
      memcpy(ptr, buf, PAGE_SIZE);
   } else {
      ret = HostIF_CopyFromUser(ptr, buf, PAGE_SIZE);
   }
   kunmap(page);

   return ret;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_GetLockedPageList --
 *
 *      puts MPNs of pages that were allocated by HostIF_AllocLockedPages()
 *      into user mode buffer.
 *
 * Results:
 *	non-negative number of the MPNs in the buffer on success.
 *	negative error code on error (-EFAULT)
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */

int 
HostIF_GetLockedPageList(VMDriver* vm,          // IN: VM instance pointer
			 VA64 uAddr,            // OUT: user mode buffer for MPNs
		         unsigned int numPages) // IN: size of the buffer in MPNs 
{
   MPN32 *mpns = VA64ToPtr(uAddr);
   MPN mpn;
   unsigned count;

   struct PhysTracker* AWEPages;

   if (!vm->vmhost || !vm->vmhost->AWEPages) {
      return 0;
   }
   AWEPages = vm->vmhost->AWEPages;

   for (mpn = 0, count = 0;
	(count < numPages) &&
        (INVALID_MPN != (mpn = PhysTrack_GetNext(AWEPages, mpn)));
        count++) {

      if (HostIF_CopyToUser(&mpns[count], &mpn, sizeof *mpns) != 0) {
	 return -EFAULT;
      }
   }
   return count;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_GetCurrentPCPU --
 *
 *    Get current physical CPU id.  Interrupts should be disabled so
 *    that the thread cannot move to another CPU.
 *
 * Results:
 *    Host CPU number.
 *
 * Side effects:
 *    None.
 *
 *---------------------------------------------------------------------- 
 */

uint32
HostIF_GetCurrentPCPU(void)
{
   uint32 result = smp_processor_id();

   ASSERT(result < MAX_LAPIC_ID);

   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIF_NumOnlineLogicalCPUs --
 *
 *    Return the current number of online (working) logical CPUs
 *    in the system.
 *
 * Results:
 *    None
 *
 * Side effects:
 *    none.
 *
 *---------------------------------------------------------------------- 
 */

unsigned int
HostIF_NumOnlineLogicalCPUs(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
   return num_online_cpus();
#else
   return smp_num_cpus;
#endif
}


#ifdef VMMON_USE_COMPAT_SCHEDULE_HRTIMEOUT
/*
 *----------------------------------------------------------------------
 *
 * HostIFWakeupClockThread --
 *
 *      Wake up the fast clock thread.  Can't do this from the timer
 *      callback, because it holds locks that the scheduling code
 *      might take. 
 *
 * Results:
 *      None.
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void 
HostIFWakeupClockThread(unsigned long data)  //IN:
{
   wake_up_process(linuxState.fastClockThread);
}


/*
 *----------------------------------------------------------------------
 *
 * HostIFTimerCallback --
 *      
 *      Schedule a tasklet to wake up the fast clock thread.
 *
 * Results:
 *      Tell the kernel not to restart the timer.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
 
static enum hrtimer_restart 
HostIFTimerCallback(struct hrtimer *timer)  //IN:
{
   tasklet_schedule(&timerTasklet);

   return HRTIMER_NORESTART;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIFScheduleHRTimeout --
 *      
 *      Schedule an hrtimer to wake up the fast clock thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Sleep.
 *
 *----------------------------------------------------------------------
 */

static void 
HostIFScheduleHRTimeout(ktime_t *expires)  //IN:
{
   struct hrtimer t;

   if (expires && !expires->tv64) {
      __set_current_state(TASK_RUNNING);
      return;
   }

   hrtimer_init(&t, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
   t.function = HostIFTimerCallback;
   hrtimer_start(&t, *expires, HRTIMER_MODE_REL);

   if (hrtimer_active(&t)) {
      schedule();
   }
   
   hrtimer_cancel(&t);
   __set_current_state(TASK_RUNNING);

   return;
}
#endif //VMMON_USE_COMPAT_SCHEDULE_HRTIMEOUT


#ifndef VMMON_USE_HIGH_RES_TIMERS
/*
 *----------------------------------------------------------------------
 *
 * HostIFDoIoctl --
 *
 *    Issue ioctl.  Assume kernel is not locked.  It is not true now,
 *    but it makes things easier to understand, and won't surprise us
 *    later when we get rid of kernel lock from our code.
 *
 * Results:
 *    Same as ioctl method.
 *
 * Side effects:
 *    none.
 *
 *---------------------------------------------------------------------- 
 */

static long
HostIFDoIoctl(struct file *filp,
              u_int iocmd,
              unsigned long ioarg)
{
#ifdef HAVE_UNLOCKED_IOCTL
   if (filp->f_op->unlocked_ioctl) {
      return filp->f_op->unlocked_ioctl(filp, iocmd, ioarg);
   }
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36)
   if (filp->f_op->ioctl) {
      long err;

      lock_kernel();
      err = filp->f_op->ioctl(filp->f_dentry->d_inode, filp, iocmd, ioarg);
      unlock_kernel();

      return err;
   }
#endif

   return -ENOIOCTLCMD;
}
#endif //VMON_USE_HIGH_RES_TIMERS


/*
 *----------------------------------------------------------------------
 *
 * HostIFStartTimer --
 *
 *      Starts the timer using either /dev/rtc or high-resolution timers.
 *
 * Results:
 *      Returns 0 on success, -1 on failure.
 *
 * Side effects:
 *      Sleep until timer expires.
 *
 *----------------------------------------------------------------------
 */

int
HostIFStartTimer(Bool rateChanged,  //IN: Did rate change? 
		 unsigned int rate, //IN: current clock rate
                 struct file *filp) //IN: /dev/rtc descriptor
{
#ifdef VMMON_USE_HIGH_RES_TIMERS
   static unsigned long slack = 0;
   static ktime_t expires;
   int timerPeriod;

   if (rateChanged) {
      timerPeriod = NSEC_PER_SEC / rate; 
      expires = ktime_set(0, timerPeriod);
      /*
       * Allow the kernel to expire the timer at its convenience.
       * ppoll() uses 0.1% of the timeout value.  I think we can
       * tolerate 1%.
       */
          
      slack = timerPeriod / 100;
   }
   set_current_state(TASK_INTERRUPTIBLE);
#   ifdef VMMON_USE_SCHEDULE_HRTIMEOUT
   schedule_hrtimeout_range(&expires, slack, HRTIMER_MODE_REL);
#   else
   HostIFScheduleHRTimeout(&expires);
#   endif
#else
   unsigned p2rate;
   int res;
   unsigned long buf;
   loff_t pos = 0;

   if (rateChanged) {
      /*
       * The host will already have HZ timer interrupts per second.  So
       * in order to satisfy the requested rate, we need up to (rate -
       * HZ) additional interrupts generated by the RTC.  That way, if
       * the guest ask for a bit more than 1024 virtual interrupts per
       * second (which is a common case for Windows with multimedia
       * timers), we'll program the RTC to 1024 rather than 2048, which
       * saves a considerable amount of CPU.  PR 519228.
       */
      if (rate > HZ) {
         rate -= HZ;
      } else {
         rate = 0;
      }
      /* 
       * Don't set the RTC rate to 64 Hz or lower: some kernels have a
       * bug in the HPET emulation of RTC that will cause the RTC
       * frequency to get stuck at 64Hz.  See PR 519228 comment #23.
       */
      p2rate = 128;
      // Hardware rate must be a power of 2
      while (p2rate < rate && p2rate < 8192) {
         p2rate <<= 1;
      }

      res = HostIFDoIoctl(filp, RTC_IRQP_SET, p2rate);
      if (res < 0) {
         Warning("/dev/rtc set rate %d failed: %d\n", p2rate, res);
         return -1;
      }
      if (compat_kthread_should_stop()) {
         return -1;
      }
   }
   res = filp->f_op->read(filp, (void *) &buf, sizeof(buf), &pos);
   if (res <= 0) {
      if (res != -ERESTARTSYS) {
         Log("/dev/rtc read failed: %d\n", res);
      }

      return -1;
   }
#endif

   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * HostIFFastClockThread --
 *
 *      Kernel thread that provides finer-grained wakeups than the
 *      main system timers by using /dev/rtc.  We can't do this at
 *      user level because /dev/rtc is not sharable (PR 19266).  Also,
 *      we want to avoid the overhead of a context switch out to user
 *      level on every RTC interrupt.
 *
 * Results:
 *      Returns 0.
 *
 * Side effects:
 *      Wakeups and IPIs.
 *
 *----------------------------------------------------------------------
 */

static int
HostIFFastClockThread(void *data)  // IN:
{
   struct file *filp = (struct file *) data;
   int res;
   mm_segment_t oldFS;
   unsigned int rate = 0;
   unsigned int prevRate = 0;

   oldFS = get_fs();
   set_fs(KERNEL_DS);
   compat_allow_signal(SIGKILL);
   compat_set_user_nice(current, linuxState.fastClockPriority);

   while ((rate = linuxState.fastClockRate) > HZ + HZ / 16) {
      if (compat_kthread_should_stop()) {
         goto out;
      }
      res = HostIFStartTimer(rate != prevRate, rate, filp);
      if (res < 0) {
         goto out;
      }
      prevRate = rate;

#if defined(CONFIG_SMP)
      /*
       * IPI each VCPU thread that is in the monitor and is due to
       * fire a MonitorPoll callback.
       */
      Vmx86_MonitorPollIPI();
#endif

      /*
       * Wake threads that are waiting for a fast poll timeout at
       * userlevel.  This is needed only on Linux.  On Windows,
       * we get shorter timeouts simply by increasing the host
       * clock rate.
       */

      LinuxDriverWakeUp(TRUE);
   }

 out:
   close_rtc(filp, current->files);
   LinuxDriverWakeUp(TRUE);
   set_fs(oldFS);

   /*
    * Do not exit thread until we are told to do so.
    */

   do {
      set_current_state(TASK_UNINTERRUPTIBLE);
      if (compat_kthread_should_stop()) {
         break;
      }
      schedule();
   } while (1);
   set_current_state(TASK_RUNNING);

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_SetFastClockRate --
 *
 *      The monitor wants to poll for events at the given rate.
 *      Ensure that the host OS's timer interrupts come at least at
 *      this rate.  If the requested rate is greater than the rate at
 *      which timer interrupts will occur on CPUs other than 0, then
 *      also arrange to call Vmx86_MonitorPollIPI on every timer
 *      interrupt, in order to relay IPIs to any other CPUs that need
 *      them.
 *
 * Locking:
 *      The caller must hold the fast clock lock.
 *
 * Results:
 *      0 for success; positive error code if /dev/rtc could not be opened.
 *
 * Side effects:
 *      As described above.
 *
 *-----------------------------------------------------------------------------
 */

int
HostIF_SetFastClockRate(unsigned int rate) // IN: Frequency in Hz.
{
   ASSERT(MutexIsLocked(&fastClockMutex));
   linuxState.fastClockRate = rate;

   /*
    * Overview
    * --------
    * An SMP Linux kernel programs the 8253 timer (to increment the 'jiffies'
    * counter) _and_ all local APICs (to run the scheduler code) to deliver
    * interrupts HZ times a second.
    *
    * Time
    * ----
    * The kernel tries very hard to spread all these interrupts evenly over
    * time, i.e. on a 1 CPU system, the 1 local APIC phase is shifted by 1/2
    * period compared to the 8253, and on a 2 CPU system, the 2 local APIC
    * phases are respectively shifted by 1/3 and 2/3 period compared to the
    * 8253. This is done to reduce contention on locks guarding the global task
    * queue.
    *
    * Space
    * -----
    * The 8253 interrupts are distributed between physical CPUs, evenly on a P3
    * system, whereas on a P4 system physical CPU 0 gets all of them.
    *
    * Long story short, unless the monitor requested rate is significantly
    * higher than HZ, we don't need to send IPIs or exclusively grab /dev/rtc
    * to periodically kick vCPU threads running in the monitor on all physical
    * CPUs.
    */

   if (rate > HZ + HZ / 16) {
      if (!linuxState.fastClockThread) {
         struct task_struct *rtcTask;
         struct file *filp = NULL;

#if !defined(VMMON_USE_HIGH_RES_TIMERS)
         int res;

         filp = filp_open("/dev/rtc", O_RDONLY, 0);
         if (IS_ERR(filp)) {
            Warning("/dev/rtc open failed: %d\n", (int)(VA)filp);

            return -(int)(VA)filp;
         }
         res = HostIFDoIoctl(filp, RTC_PIE_ON, 0);
         if (res < 0) {
            Warning("/dev/rtc enable interrupt failed: %d\n", res);
            compat_filp_close(filp, current->files);

            return -res;
         }
#endif
         rtcTask = compat_kthread_run(HostIFFastClockThread, filp,
                                      "vmware-rtc");
         if (IS_ERR(rtcTask)) {
            long err = PTR_ERR(rtcTask);

            /*
             * Ignore ERESTARTNOINTR silently, it occurs when signal is
             * pending, and syscall layer automatically reissues operation
             * after signal is handled.
             */

            if (err != -ERESTARTNOINTR) {
               Warning("/dev/rtc cannot start watch thread: %ld\n", err);
            }
	    close_rtc(filp, current->files);

            return -err;
         }
         linuxState.fastClockThread = rtcTask;
      }
   } else {
      if (linuxState.fastClockThread) {
         force_sig(SIGKILL, linuxState.fastClockThread);
         compat_kthread_stop(linuxState.fastClockThread);
         linuxState.fastClockThread = NULL;
      }
   }

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_MapUserMem --
 *
 *	Obtain kernel pointer to user memory.
 *
 * Results:
 *	The kernel virtual address on success
 *	Page structure to keep around for unmapping.
 *
 * Side effects:
 *	Yes.
 *
 *-----------------------------------------------------------------------------
 */

void *
HostIF_MapUserMem(VA addr,		// IN
                  size_t size,		// IN
		  struct page **page)	// IN/OUT
{
   void *p = (void *) (uintptr_t) addr;
   VA offset = addr & (PAGE_SIZE - 1);

   if (*page != NULL) {
      return NULL;
   }
   if (offset + size > PAGE_SIZE) {
      return NULL;
   }
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0) 
   if (!access_ok(VERIFY_WRITE, p, size)) {
      return NULL;
   }
#else
   if (verify_area(VERIFY_READ, p, size) != 0 ||
       verify_area(VERIFY_WRITE, p, size) != 0) {
      return NULL;
   }
#endif
   if (HostIFGetUserPage(p, page) != 0) {
      *page = NULL;

      return NULL;
   }

   return kmap(*page) + offset;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_UnmapUserMem --
 *
 *	Unmap user memory from HostIF_MapUserMem().
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Yes.
 *
 *-----------------------------------------------------------------------------
 */

void
HostIF_UnmapUserMem(struct page **page)	// IN/OUT
{
   struct page *p;

   if ((p = *page) == NULL) {
      return;
   }
   *page = NULL;
   kunmap(p);
   put_page(p);
}

/*
 *-----------------------------------------------------------------------------
 *
 * HostIF_SafeRDMSR --
 *
 *      Attempt to read a MSR, and handle the exception if the MSR
 *      is unimplemented.
 *
 * Results:
 *      0 if successful, and MSR value is returned via *val.
 *
 *      If the MSR is unimplemented, *val is set to 0, and a
 *      non-zero value is returned: -1 for Win32, -EFAULT for Linux,
 *      and 1 for MacOS.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */
int
HostIF_SafeRDMSR(unsigned int msr,   // IN
                 uint64 *val)        // OUT: MSR value
{
   int ret;
#if defined(VM_X86_64)
   unsigned low, high;

   asm volatile("2: rdmsr ; xor %0,%0\n"
                "1:\n\t"
                ".section .fixup,\"ax\"\n\t"
                "3: mov %4,%0 ; jmp 1b\n\t"
                ".previous\n\t"
                ".section __ex_table,\"a\"\n\t"
                ".balign 8\n"
                ".quad 2b,3b\n"
                ".previous\n"
                : "=r"(ret), "=a"(low), "=d"(high)
                : "c"(msr), "i"(-EFAULT), "1"(0), "2"(0)); // init eax/edx to 0
   *val = (low | ((u64)(high) << 32));
#else
   asm volatile("2: rdmsr ; xor %0,%0\n"
                "1:\n\t"
                ".section .fixup,\"ax\"\n\t"
                "3: mov %3,%0 ; jmp 1b\n\t"
                ".previous\n\t"
                ".section __ex_table,\"a\"\n"
                ".balign 4\n"
                ".long 2b,3b\n"
                ".previous\n"
                : "=r"(ret), "=A"(*val)
                : "c"(msr), "i"(-EFAULT), "1"(0)); // init rax to 0

#endif // VM_X86_64

   return ret;
}

