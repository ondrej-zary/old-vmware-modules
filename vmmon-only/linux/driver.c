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

/* Must come before any kernel header file */
#include "driver-config.h"

#define EXPORT_SYMTAB

#include "compat_version.h"
#include "compat_kernel.h"
#include "compat_module.h"
#include "compat_sched.h"
#include "compat_file.h"
#include "compat_slab.h"
#include "compat_interrupt.h"
#include "compat_wait.h"
#include "compat_uaccess.h"
#include "compat_page.h"
#include "compat_timer.h"
#include "compat_mm.h"
#include "compat_highmem.h"
#include "compat_cred.h"

#include <linux/smp.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36)
#include <linux/smp_lock.h>
#endif

#include <linux/poll.h>

#include "usercalldefs.h"

/*
 * Power Management: hook resume to work around
 * BIOS bugs where VT is not properly enabled after S4
 * resume.  In such buggy BIOSes, we are unable to avoid
 * entering the monitor and taking a #GP at the first VMXON.
 * Workaround: at any resume, apply VT fixups.
 *
 * 2.4.0 - 2.6.10 => uses pm_* power APIs
 *     (deprecated in 2.6.11)
 * 2.?.? - present => uses struct device_driver callbacks
 *     (GPL only)
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 11)
#   define DO_PM24
#else
#   define DO_PM26
/*
 * In kernels 2.6.11 and up, the way to get power
 * notifications is to add a callback to a registered
 * struct device_driver.  The APIs to register such a struct
 * are GPLed, so we cannot use them.
 *
 * Roughly: register a struct device_driver, and point the "resume"
 * callback at LinuxDriverPMImpl.
 */
#endif

#ifdef DO_PM24
#   include <linux/pm.h>
#endif
#ifdef DO_PM26
#endif

#include <asm/io.h>
#if defined(__x86_64__) && LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 12)
#   if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 0)
#      include <asm/ioctl32.h>
#   else
#      include <linux/ioctl32.h>
#   endif
/* Use weak: not all kernels export sys_ioctl for use by modules */
#   if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 5, 66)
asmlinkage __attribute__((weak)) long
sys_ioctl(unsigned int fd, unsigned int cmd, unsigned long arg);
#   else
asmlinkage __attribute__((weak)) int
sys_ioctl(unsigned int fd, unsigned int cmd, unsigned long arg);
#   endif
#endif

#include "vmware.h"
#include "driverLog.h"
#include "driver.h"
#include "modulecall.h"
#include "vm_asm.h"
#include "vmx86.h"
#include "initblock.h"
#include "task.h"
#include "speaker_reg.h"
#include "memtrack.h"
#include "task.h"
#include "cpuid.h"
#include "cpuid_info.h"
#include "circList.h"
#include "x86msr.h"
#include "iommu.h"

#ifdef VMX86_DEVEL
#include "private.h"
#endif

#include "hostif.h"
#include "vmhost.h"

#include "vmmonInt.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 4, 20)
int errno;       // required for compat_exit()
#endif
static void LinuxDriverQueue(VMLinux *vmLinux);
static void LinuxDriverDequeue(VMLinux *vmLinux);
static Bool LinuxDriverCheckPadding(void);

typedef enum {
   LINUXDRIVERPM_SUSPEND,
   LINUXDRIVERPM_RESUME,
} LinuxDriverPMState;

#if defined(DO_PM24)
static int LinuxDriverPMImpl(LinuxDriverPMState state);
#endif
#ifdef DO_PM24
static struct pm_dev *LinuxDriverPMDev;
static int LinuxDriverPM24Callback(struct pm_dev *dev, pm_request_t rqst, void *data);
#endif
#ifdef DO_PM26
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 24)
#define VMW_NOPAGE_2624
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0) && \
    (defined(CONFIG_SMP) || defined(CONFIG_PREEMPT))
#  define kernel_locked_by_current() kernel_locked()
#else
#  define kernel_locked_by_current() 0
#endif
#endif

#define VMMON_UNKNOWN_SWAP_SIZE -1ULL

struct VMXLinuxState linuxState;


/*
 *----------------------------------------------------------------------
 *
 * Device Driver Interface --
 *
 *      Runs the VM by implementing open/close/ioctl functions
 *
 *
 *----------------------------------------------------------------------
 */
static int LinuxDriver_Open(struct inode *inode, struct file *filp);

/*
 * gcc-4.5+ can name-mangle LinuxDriver_Ioctl, but our stack-size
 * script needs to find it.  So it shouldn't be static.  ("hidden"
 * visibility would be OK.)
 */
int LinuxDriver_Ioctl(struct inode *inode, struct file *filp,
                      u_int iocmd, unsigned long ioarg);
#if defined(HAVE_UNLOCKED_IOCTL) || defined(HAVE_COMPAT_IOCTL)
static long LinuxDriver_UnlockedIoctl(struct file *filp,
                           u_int iocmd, unsigned long ioarg);
#endif

static int LinuxDriver_Close(struct inode *inode, struct file *filp);
static unsigned int LinuxDriverPoll(struct file *file, poll_table *wait);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
static int LinuxDriverFault(struct vm_fault *fault);
#elif defined(VMW_NOPAGE_2624)
static int LinuxDriverFault(struct vm_area_struct *vma, struct vm_fault *fault);
#elif defined(VMW_NOPAGE_261)
static struct page *LinuxDriverNoPage(struct vm_area_struct *vma,
                           unsigned long address, int *type);
#else
static struct page *LinuxDriverNoPage(struct vm_area_struct *vma,
			   unsigned long address, int unused);
#endif
static int LinuxDriverMmap(struct file *filp, struct vm_area_struct *vma);

static void LinuxDriverPollTimeout(unsigned long clientData);

static struct vm_operations_struct vmuser_mops = {
#ifdef VMW_NOPAGE_2624
	.fault  = LinuxDriverFault
#else
	.nopage = LinuxDriverNoPage
#endif
};

static struct file_operations vmuser_fops;
static struct timer_list tscTimer;

/*
 *----------------------------------------------------------------------
 *
 * VMX86_RegisterMonitor --
 *
 *      (debugging support) Should be the first function of this file
 *
 * Results:
 *
 *      Registers the module.
 *      /sbin/ksyms -a | grep VMX86_RegisterMonitor will return the base
 *      address of that function as loaded in the kernel.
 *
 *      Since this is the first function of the kernel module,
 *      every other symbol can be computing by adding the base
 *      to the output of nm.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int VMX86_RegisterMonitor(int);

EXPORT_SYMBOL(VMX86_RegisterMonitor);

int
VMX86_RegisterMonitor(int value)
{
   printk("/dev/vmmon: RegisterMonitor(%d) \n",value);
   return 1291;
}

#ifdef VM_X86_64
#ifndef HAVE_COMPAT_IOCTL
static int
LinuxDriver_Ioctl32_Handler(unsigned int fd, unsigned int iocmd,
                            unsigned long ioarg, struct file * filp)
{
   int ret;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 4, 26) || \
   (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 5, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 3))
   lock_kernel();
#endif
   ret = -ENOTTY;
   if (filp && filp->f_op && filp->f_op->ioctl == LinuxDriver_Ioctl) {
      ret = LinuxDriver_Ioctl(filp->f_dentry->d_inode, filp, iocmd, ioarg);
   }
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 4, 26) || \
   (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 5, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 3))
   unlock_kernel();
#endif
   return ret;
}
#endif /* !HAVE_COMPAT_IOCTL */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 12)
static int
LinuxDriver_Ioctl3264Passthrough(unsigned int fd, unsigned int iocmd,
				 unsigned long ioarg, struct file * filp)
{
   VMIoctl64 cmd;

   if (copy_from_user(&cmd, (VMIoctl64*)ioarg, sizeof(cmd))) {
      return -EFAULT;
   }
   if (sys_ioctl) {
      return sys_ioctl(fd, cmd.iocmd, cmd.ioarg);
   }
   return -ENOTTY;
}
#endif /* KERNEL < 2.6.12 */

static int
register_ioctl32_handlers(void)
{
#ifndef HAVE_COMPAT_IOCTL
   {
      int i;
      for (i = IOCTL_VMX86_FIRST; i < IOCTL_VMX86_LAST; i++) {
         int retval = register_ioctl32_conversion(i, LinuxDriver_Ioctl32_Handler);
         if (retval) {
            Warning("Fail to register ioctl32 conversion for cmd %d\n", i);
            return retval;
         }
      }
   }
#endif /* !HAVE_COMPAT_IOCTL */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 12)
   if (!sys_ioctl) {
      Log("USB support will not work correctly in your virtual machines.\n");
   }
   {
      int retval = register_ioctl32_conversion(IOCTL_VMX86_IOCTL64,
                                               LinuxDriver_Ioctl3264Passthrough);
      if (retval) {
         Warning("Fail to register ioctl32 conversion for cmd 0x%08lX\n",
                 IOCTL_VMX86_IOCTL64);
         return retval;
      }
   }
#endif /* KERNEL < 2.6.12 */
   return 0;
}

static void
unregister_ioctl32_handlers(void)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 12)
   {
      int retval = unregister_ioctl32_conversion(IOCTL_VMX86_IOCTL64);
      if (retval) {
         Warning("Fail to unregister ioctl32 conversion for cmd 0x%08lX\n",
                 IOCTL_VMX86_IOCTL64);
      }
   }
#endif /* KERNEL < 2.6.12 */
#ifndef HAVE_COMPAT_IOCTL
   {
      int i;
      for (i = IOCTL_VMX86_FIRST; i < IOCTL_VMX86_LAST; i++) {
         int retval = unregister_ioctl32_conversion(i);
         if (retval) {
            Warning("Fail to unregister ioctl32 conversion for cmd %d\n", i);
         }
      }
   }
#endif /* !HAVE_COMPAT_IOCTL */
}
#else /* VM_X86_64 */
#define register_ioctl32_handlers() (0)
#define unregister_ioctl32_handlers() do { } while (0)
#endif /* VM_X86_64 */


/*
 *----------------------------------------------------------------------
 *
 * LinuxDriverComputeTSCFreq --
 *
 *      Compute TSC frequency based on time and TSC cycles which passed
 *      since Vmx86_SetStartTime() was invoked.  Should be issued only
 *      once by callback 4 seconds after vmmon loads.
 *
 * Results:
 *
 *      vmmon learns tsc frequency if some reasonable result is computed.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
LinuxDriverComputeTSCFreq(unsigned long data)
{
   Vmx86_GetkHzEstimate(&linuxState.startTime);
}


/*
 *----------------------------------------------------------------------
 *
 * init_module --
 *
 *      linux module entry point. Called by /sbin/insmod command
 *
 * Results:
 *      registers a device driver for a major # that depends
 *      on the uid. Add yourself to that list.  List is now in
 *      private/driver-private.c.
 *
 *----------------------------------------------------------------------
 */
int
init_module(void)
{
   int retval;

   DriverLog_Init("/dev/vmmon");
   HostIF_InitGlobalLock();

   if (!LinuxDriverCheckPadding()) {
      return -ENOEXEC;
   }

   CPUID_Init();
   if (!Task_Initialize()) {
      return -ENOEXEC;
   }

   /*
    * Initialize LinuxDriverPoll state
    */

   init_waitqueue_head(&linuxState.pollQueue);
   init_timer(&linuxState.pollTimer);
   linuxState.pollTimer.data = 0;
   linuxState.pollTimer.function = LinuxDriverPollTimeout;

   linuxState.fastClockThread = NULL;
   linuxState.fastClockRate = 0;
   linuxState.fastClockPriority = -20;
   linuxState.swapSize = VMMON_UNKNOWN_SWAP_SIZE;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 36)
   compat_mutex_init(&linuxState.lock);
#endif

#ifdef POLLSPINLOCK
   spin_lock_init(&linuxState.pollListLock);
#endif

   /*
    * Initialize the file_operations structure. Because this code is always
    * compiled as a module, this is fine to do it here and not in a static
    * initializer.
    */

   memset(&vmuser_fops, 0, sizeof vmuser_fops);
   vmuser_fops.owner = THIS_MODULE;
   vmuser_fops.poll = LinuxDriverPoll;
#ifdef HAVE_UNLOCKED_IOCTL
   vmuser_fops.unlocked_ioctl = LinuxDriver_UnlockedIoctl;
#else
   vmuser_fops.ioctl = LinuxDriver_Ioctl;
#endif
#ifdef HAVE_COMPAT_IOCTL
   vmuser_fops.compat_ioctl = LinuxDriver_UnlockedIoctl;
#endif
   vmuser_fops.open = LinuxDriver_Open;
   vmuser_fops.release = LinuxDriver_Close;
   vmuser_fops.mmap = LinuxDriverMmap;

#ifdef VMX86_DEVEL
   devel_init_module();
   linuxState.minor = 0;
   retval = register_chrdev(linuxState.major, linuxState.deviceName,
			    &vmuser_fops);
#else
   sprintf(linuxState.deviceName, "vmmon");
   linuxState.major = 10;
   linuxState.minor = 165;
   linuxState.misc.minor = linuxState.minor;
   linuxState.misc.name = linuxState.deviceName;
   linuxState.misc.fops = &vmuser_fops;

   retval = misc_register(&linuxState.misc);
#endif

   if (retval) {
      Warning("Module %s: error registering with major=%d minor=%d\n",
	      linuxState.deviceName, linuxState.major, linuxState.minor);
      return -ENOENT;
   }
   Log("Module %s: registered with major=%d minor=%d\n",
       linuxState.deviceName, linuxState.major, linuxState.minor);


   retval = register_ioctl32_handlers();
   if (retval) {
#ifdef VMX86_DEVEL
      unregister_chrdev(linuxState.major, linuxState.deviceName);
#else
      misc_deregister(&linuxState.misc);
#endif
      return retval;
   }

   HostIF_InitUptime();

   /*
    * Snap shot the time stamp counter and the real time so we
    * can later compute an estimate of the cycle time.
    */

   Vmx86_ReadTSCAndUptime(&linuxState.startTime);
   init_timer(&tscTimer);
   tscTimer.data = 0;
   tscTimer.function = LinuxDriverComputeTSCFreq;
   tscTimer.expires = jiffies + 4 * HZ;
   add_timer(&tscTimer);

   Vmx86_InitIDList();

   /*
    * Workaround for buggy BIOSes that don't handle VT enable well.
    * (Some BIOSes enable VT on one core but not others; other
    *  BIOSes enable VT at power-on but forget during S4 resume.)
    *
    * We also call Vmx86_FixHVEnable after an S4 resume, when
    * appropriate power management hooks are available.
    * (2.6.10 kernels and LOWER; later kernels are GPL-only symbols.)
    */
   Vmx86_FixHVEnable(FALSE);
#ifdef DO_PM24
   LinuxDriverPMDev = pm_register(PM_UNKNOWN_DEV,
                                  PM_SYS_UNKNOWN,
                                  &LinuxDriverPM24Callback);
#endif
#ifdef DO_PM26
#endif

   Log("Module %s: initialized\n", linuxState.deviceName);

   return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * cleanup_module --
 *
 *      Called by /sbin/rmmod
 *
 *
 *----------------------------------------------------------------------
 */

void
cleanup_module(void)
{
#ifdef DO_PM26
#endif
#ifdef DO_PM24
   if (LinuxDriverPMDev) {
      pm_unregister(LinuxDriverPMDev);
   }
#endif

   if (Task_IsVMXDisabledOnAllCPUs()) {
      Task_FreeVMCS();
   }
   unregister_ioctl32_handlers();

   /*
    * XXX smp race?
    */
#ifdef VMX86_DEVEL
   unregister_chrdev(linuxState.major, linuxState.deviceName);
#else
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 3, 0)
   misc_deregister(&linuxState.misc);
#else
   if (misc_deregister(&linuxState.misc)) {
      Warning("Module %s: error unregistering\n", linuxState.deviceName);
   }
#endif
#endif

   Log("Module %s: unloaded\n", linuxState.deviceName);

   compat_del_timer_sync(&linuxState.pollTimer);
   compat_del_timer_sync(&tscTimer);

   Task_Terminate();
   // Make sure fastClockThread is dead
   HostIF_FastClockLock(1);
   HostIF_SetFastClockRate(0);
   HostIF_FastClockUnlock(1);

   HostIF_CleanupUptime();


   Vmx86_DestroyNUMAInfo();

#ifdef HOSTED_IOMMU_SUPPORT
   IOMMU_ModuleCleanup();
#endif
}



/*
 *----------------------------------------------------------------------
 *
 * LinuxDriver_Open  --
 *
 *      called on open of /dev/vmmon or /dev/vmx86.$USER. Use count used
 *      to determine eventual deallocation of the module
 *
 * Side effects:
 *     Increment use count used to determine eventual deallocation of
 *     the module
 *
 *----------------------------------------------------------------------
 */
static int
LinuxDriver_Open(struct inode *inode, // IN
                 struct file *filp)   // IN
{
   VMLinux *vmLinux;

   vmLinux = kmalloc(sizeof *vmLinux, GFP_KERNEL);
   if (vmLinux == NULL) {
      return -ENOMEM;
   }
   memset(vmLinux, 0, sizeof *vmLinux);

   sema_init(&vmLinux->lock4Gb, 1);
   init_waitqueue_head(&vmLinux->pollQueue);

   filp->private_data = vmLinux;
   LinuxDriverQueue(vmLinux);

   Vmx86_Open();

   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * LinuxDriverAllocPages --
 *
 *    Allocate physically contiguous block of memory with specified order.
 *    Pages in the allocated block are configured so that caller can pass
 *    independent pages to the VM.
 *
 * Results:
 *    Zero on success, non-zero (error code) on failure.
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

static int
LinuxDriverAllocPages(unsigned int  gfpFlag, // IN
		      unsigned int  order,   // IN
                      struct page **pg,      // OUT
                      unsigned int  size)    // IN
{
   struct page* page;

   page = alloc_pages(gfpFlag, order);
   if (page) {
      unsigned int i;

      /*
       * Grab an extra reference on all pages except first one - first
       * one was already refcounted by alloc_pages.
       *
       * Under normal situation all pages except first one in the block
       * have refcount zero.  As we pass these pages to the VM, we must
       * bump their count, otherwise VM will release these pages every
       * time they would be unmapped from user's process, causing crash.
       *
       * Note that this depends on Linux VM internals.  It works on all
       * kernels we care about.
       */
      order = 1 << order;
      for (i = 0; i < order; i++) {
         if (i) {
            get_page(page);
         }
         if (i >= size) {
            put_page(page);
         } else {
            void* addr = kmap(page);

            memset(addr, 0, PAGE_SIZE);
            kunmap(page);
            *pg++ = page;
         }
         page++;
      }
      return 0;
   }
   return -ENOMEM;
}


/*
 *-----------------------------------------------------------------------------
 *
 * LinuxDriverDestructor4Gb --
 *
 *    Deallocate all directly mappable memory.
 *
 * Results:
 *    None
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

static void
LinuxDriverDestructor4Gb(VMLinux *vmLinux) // IN
{
   unsigned int pg;

   if (!vmLinux->size4Gb) {
      return;
   }
   for (pg = 0; pg < vmLinux->size4Gb; pg++) {
      put_page(vmLinux->pages4Gb[pg]);
   }
   vmLinux->size4Gb = 0;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxDriver_Close  --
 *
 *      called on close of /dev/vmmon or /dev/vmx86.$USER, most often when the
 *      process exits. Decrement use count, allowing for possible uninstalling
 *      of the module.
 *
 *----------------------------------------------------------------------
 */

static int
LinuxDriver_Close(struct inode *inode, // IN
                  struct file *filp)   // IN
{
   VMLinux *vmLinux;

   vmLinux = (VMLinux *)filp->private_data;
   ASSERT(vmLinux);

#ifdef HOSTED_IOMMU_SUPPORT
   IOMMU_VMCleanup(vmLinux);
#endif

   LinuxDriverDequeue(vmLinux);
   if (vmLinux->vm != NULL) {
      Vmx86_ReleaseVM(vmLinux->vm);
      vmLinux->vm = NULL;
   }

   Vmx86_Close();

   /*
    * Destroy all low memory allocations.
    * We are closing the struct file here, so clearly no other process
    * uses it anymore, and we do not need to hold the semaphore.
    */

   LinuxDriverDestructor4Gb(vmLinux);

   /*
    * Clean up poll state.
    */

#ifdef POLLSPINLOCK
   {
   unsigned long flags;
   spin_lock_irqsave(&linuxState.pollListLock, flags);
#else
   HostIF_PollListLock(0);
#endif
   if (vmLinux->pollBack != NULL) {
      if ((*vmLinux->pollBack = vmLinux->pollForw) != NULL) {
	 vmLinux->pollForw->pollBack = vmLinux->pollBack;
      }
   }
#ifdef POLLSPINLOCK
   spin_unlock_irqrestore(&linuxState.pollListLock, flags);
   }
#else
   HostIF_PollListUnlock(0);
#endif
   // XXX call wake_up()?
   HostIF_UnmapUserMem(&vmLinux->pollTimeoutPage);

   kfree(vmLinux);
   filp->private_data = NULL;
   return 0;
}


#define POLLQUEUE_MAX_TASK 1000
static DEFINE_SPINLOCK(pollQueueLock);
static void *pollQueue[POLLQUEUE_MAX_TASK];
static unsigned int pollQueueCount = 0;


/*
 *-----------------------------------------------------------------------------
 *
 * LinuxDriverQueuePoll --
 *
 *      Remember that current process waits for next timer event.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER void
LinuxDriverQueuePoll(void)
{
   unsigned long flags;

   spin_lock_irqsave(&pollQueueLock, flags);

   /*
    * Under normal circumstances every process should be listed
    * only once in this array. If it becomes problem that process
    * can be in the array twice, walk array! Maybe you can keep
    * it sorted by 'current' value then, making IsPollQueued
    * a bit faster...
    */
   if (pollQueueCount < POLLQUEUE_MAX_TASK) {
      pollQueue[pollQueueCount++] = current;
   }
   spin_unlock_irqrestore(&pollQueueLock, flags);
}


/*
 *-----------------------------------------------------------------------------
 *
 * LinuxDriverIsPollQueued --
 *
 *      Determine whether timer event occurred since we queued for it using
 *      LinuxDriverQueuePoll.
 *
 * Results:
 *      0    Event already occurred.
 *      1    Event did not occur yet.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER int
LinuxDriverIsPollQueued(void)
{
   unsigned long flags;
   unsigned int i;
   int retval = 0;

   spin_lock_irqsave(&pollQueueLock, flags);
   for (i = 0; i < pollQueueCount; i++) {
      if (current == pollQueue[i]) {
         retval = 1;
         break;
      }
   }
   spin_unlock_irqrestore(&pollQueueLock, flags);
   return retval;
}


/*
 *-----------------------------------------------------------------------------
 *
 * LinuxDriverFlushPollQueue --
 *
 *      Signal to queue that timer event occurred.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER void
LinuxDriverFlushPollQueue(void)
{
   unsigned long flags;

   spin_lock_irqsave(&pollQueueLock, flags);
   pollQueueCount = 0;
   spin_unlock_irqrestore(&pollQueueLock, flags);
}


/*
 *-----------------------------------------------------------------------------
 *
 * LinuxDriverWakeUp --
 *
 *      Wake up processes waiting on timer event.
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
LinuxDriverWakeUp(Bool selective)
{
   if (selective && linuxState.pollList != NULL) {
      struct timeval tv;
      VmTimeType now;
      VMLinux *p;
      VMLinux *next;

      //compat_preempt_disable();
#ifdef POLLSPINLOCK
      unsigned long flags;
      spin_lock_irqsave(&linuxState.pollListLock, flags);
#else
      HostIF_PollListLock(1);
#endif
      do_gettimeofday(&tv);
      now = tv.tv_sec * 1000000ULL + tv.tv_usec;
      for (p = linuxState.pollList; p != NULL; p = next) {
	 next = p->pollForw;
	 if (p->pollTime <= now) {
	    if ((*p->pollBack = next) != NULL) {
	       next->pollBack = p->pollBack;
	    }
	    p->pollForw = NULL;
	    p->pollBack = NULL;
	    wake_up(&p->pollQueue);
	 }
      }
#ifdef POLLSPINLOCK
      spin_unlock_irqrestore(&linuxState.pollListLock, flags);
#else
      HostIF_PollListUnlock(1);
#endif
      //compat_preempt_enable();
   }

   LinuxDriverFlushPollQueue();
   wake_up(&linuxState.pollQueue);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxDriverPoll  --
 *
 *      This is used to wake up the VMX when a user call arrives, or
 *      to wake up select() or poll() at the next clock tick.
 *
 *----------------------------------------------------------------------
 */

static unsigned int
LinuxDriverPoll(struct file *filp,
		poll_table *wait)
{
   VMLinux *vmLinux = (VMLinux *) filp->private_data;
   unsigned int mask = 0;
   VMDriver *vm = vmLinux->vm;

   if (vm != NULL) {
      /*
       * Check for user call requests.
       */

      if (wait != NULL) {
         poll_wait(filp, &vm->vmhost->callQueue, wait);
      }
      if (atomic_read(&vm->vmhost->pendingUserCalls) > 0) {
         mask = POLLIN;
      }

   } else {
      /*
       * Set up or check the timeout for fast wakeup.
       *
       * Thanks to Petr for this simple and correct implementation:
       *
       * There are four cases of wait == NULL:
       *    another file descriptor is ready in the same poll()
       *    just slept and woke up
       *    nonblocking poll()
       *    did not sleep due to memory allocation on 2.4.21-9.EL
       * In first three cases, it's okay to return POLLIN.
       * Unfortunately, for 4th variant we have to do some
       * bookkeeping to not return POLLIN when timer did not expire
       * yet.
       *
       * We may schedule a timer unnecessarily if an existing
       * timer fires between poll_wait() and timer_pending().
       *
       * -- edward
       */

      if (wait == NULL) {
	 if (vmLinux->pollBack == NULL && !LinuxDriverIsPollQueued()) {
	    mask = POLLIN;
	 }
      } else {
         if (linuxState.fastClockThread && vmLinux->pollTimeoutPtr != NULL) {
	    struct timeval tv;
	    do_gettimeofday(&tv);
	    poll_wait(filp, &vmLinux->pollQueue, wait);
	    vmLinux->pollTime = *vmLinux->pollTimeoutPtr +
	                        tv.tv_sec * 1000000ULL + tv.tv_usec;
	    if (vmLinux->pollBack == NULL) {
#ifdef POLLSPINLOCK
	       unsigned long flags;
	       spin_lock_irqsave(&linuxState.pollListLock, flags);
#else
	       HostIF_PollListLock(2);
#endif
	       if (vmLinux->pollBack == NULL) {
		  if ((vmLinux->pollForw = linuxState.pollList) != NULL) {
		     vmLinux->pollForw->pollBack = &vmLinux->pollForw;
		  }
		  linuxState.pollList = vmLinux;
		  vmLinux->pollBack = &linuxState.pollList;
	       }
#ifdef POLLSPINLOCK
	       spin_unlock_irqrestore(&linuxState.pollListLock, flags);
#else
	       HostIF_PollListUnlock(2);
#endif
	    }
	 } else {
	    LinuxDriverQueuePoll();
	    poll_wait(filp, &linuxState.pollQueue, wait);
	    if (!timer_pending(&linuxState.pollTimer)) {
	       /*
		* RedHat 7.2's SMP kernel, 2.4.9-34, contains serious bug
		* which prevents concurrent mod_timer() requests to work.
		* See bug 34603 for details.
		*
		* This spinlock is not needed for non-RedHat kernels,
		* but unfortunately there is no way how to detect that
		* we are building for RedHat's kernel...
		*/
	       static DEFINE_SPINLOCK(timerLock);

	       spin_lock(&timerLock);
	       mod_timer(&linuxState.pollTimer, jiffies + 1);
	       spin_unlock(&timerLock);
	    }
	 }
      }
   }
   return mask;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxDriverPollTimeout  --
 *
 *      Wake up a process waiting in poll/select.  This is called from
 *      the timer, and hence processed in the bottom half
 *
 *----------------------------------------------------------------------
 */

static void
LinuxDriverPollTimeout(unsigned long clientData)
{
   LinuxDriverWakeUp(FALSE);
}


/*
 *-----------------------------------------------------------------------------
 *
 * LinuxDriverNoPage/LinuxDriverFault --
 *
 *      Callback for returning allocated page for memory mapping
 *
 * Results:
 *    NoPage:
 *      Page or page address on success, NULL or 0 on failure.
 *    Fault:
 *      Error code; 0, minor page fault.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
static int LinuxDriverFault(struct vm_fault *fault)
#elif defined(VMW_NOPAGE_2624)
static int LinuxDriverFault(struct vm_area_struct *vma, //IN
			    struct vm_fault *fault)     //IN/OUT
#elif defined(VMW_NOPAGE_261)
static struct page *LinuxDriverNoPage(struct vm_area_struct *vma, //IN
				      unsigned long address, 	  //IN
				      int *type)		  //OUT: Fault type
#else
static struct page *LinuxDriverNoPage(struct vm_area_struct *vma, //IN
				      unsigned long address, 	  //IN
				      int unused)		  //nothing
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
	VMLinux *vmLinux = (VMLinux *) fault->vma->vm_file->private_data;
#else
	VMLinux *vmLinux = (VMLinux *) vma->vm_file->private_data;
#endif
	unsigned long pg;
	struct page* page;

#ifdef VMW_NOPAGE_2624
	pg = fault->pgoff;
#else
	pg = ((address - vma->vm_start) >> PAGE_SHIFT) + compat_vm_pgoff(vma);
#endif
	pg = VMMON_MAP_OFFSET(pg);
	if (pg >= vmLinux->size4Gb) {
#ifdef VMW_NOPAGE_2624
		return VM_FAULT_SIGBUS;
#else
		return 0;
#endif
	}
	page = vmLinux->pages4Gb[pg];
	get_page(page);
#ifdef VMW_NOPAGE_2624
	fault->page = page;
	return 0;
#else
#ifdef VMW_NOPAGE_261
        *type = VM_FAULT_MINOR;
#endif
	return page;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * LinuxDriverAllocContig --
 *
 *      Create mapping for contiguous memory areas.
 *
 * Results:
 *
 *      0       on success,
 *      -EINVAL on invalid arguments or
 *      -ENOMEM on out of memory
 *
 * Side effects:
 *      Pages for mapping are allocated.
 *
 *-----------------------------------------------------------------------------
 */

static int LinuxDriverAllocContig(VMLinux *vmLinux,
                                  struct vm_area_struct *vma,
				  unsigned long off,
				  unsigned long size) {
   unsigned long vmaOrder      = VMMON_MAP_ORDER(off);
   unsigned long vmaAllocSize;
   unsigned int gfpFlag;
   unsigned long i;

   if (VMMON_MAP_RSVD(off)) {
      /* Reserved bits set... */
      return -EINVAL;
   }
   if (VMMON_MAP_OFFSET(off)) {
      /* We do not need non-zero offsets... */
      return -EINVAL;
   }
   switch (VMMON_MAP_MT(off)) {
      case VMMON_MAP_MT_LOW4GB:
#ifdef VM_X86_64
#   ifdef GFP_DMA32
         gfpFlag = GFP_USER | GFP_DMA32;
#   else
         gfpFlag = GFP_USER | GFP_DMA;
#   endif
#else
         gfpFlag = GFP_USER;
#endif
         break;
      case VMMON_MAP_MT_LOW16MB:
         gfpFlag = GFP_USER | GFP_DMA;
	 break;
      case VMMON_MAP_MT_ANY:
         gfpFlag = GFP_HIGHUSER;
	 break;
      default:
         /* Invalid memory type */
         return -EINVAL;
   }
   if (size > VMMON_MAP_OFFSET_MASK + 1) {
      /* Size is too big to fit to our window. */
      return -ENOMEM;
   }

   /* 16 pages looks like a good limit... */
   if (size > VMMON_MAX_LOWMEM_PAGES) {
      return -ENOMEM;
   }
   /* Sorry. Only one mmap per one open. */
   down(&vmLinux->lock4Gb);
   if (vmLinux->size4Gb) {
      up(&vmLinux->lock4Gb);
      return -EINVAL;
   }
   vmaAllocSize = 1 << vmaOrder;
   for (i = 0; i < size; i += vmaAllocSize) {
      int err;

      err = LinuxDriverAllocPages(gfpFlag, vmaOrder,
                                  vmLinux->pages4Gb + i, size - i);
      if (err) {
	 while (i > 0) {
            put_page(vmLinux->pages4Gb[--i]);
	 }
	 up(&vmLinux->lock4Gb);
	 return err;
      }
   }
   vmLinux->size4Gb = size;
   up(&vmLinux->lock4Gb);
   vma->vm_ops = &vmuser_mops;
   /*
    * It seems that SuSE's 2.6.4-52 needs this.  Hopefully
    * it will not break anything else.
    *
    * It breaks on post 2.6.14 kernels, so get rid of it on them.
    */
#ifdef VM_RESERVED
#  if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 14)
   vma->vm_flags |= VM_RESERVED;
#  endif
#endif
   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * LinuxDriverMmap --
 *
 *      Create mapping for lowmem or locked memory.
 *
 * Results:
 *
 *      0       on success,
 *      -EINVAL on invalid arguments or
 *      -ENOMEM on out of memory
 *
 * Side effects:
 *      Pages for mapping are allocated.
 *
 *-----------------------------------------------------------------------------
 */

static int LinuxDriverMmap(struct file *filp, struct vm_area_struct *vma) {
   VMLinux *vmLinux = (VMLinux *) filp->private_data;
   unsigned long size;
   int err;

   /* Only shared mappings */
   if (!(vma->vm_flags & VM_SHARED)) {
      return -EINVAL;
   }
   if ((vma->vm_end | vma->vm_start) & (PAGE_SIZE - 1)) {
      return -EINVAL;
   }
   size = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
   if (size < 1) {
      return -EINVAL;
   }
   if (vmLinux->vm) {
      err = -EINVAL;
   } else {
      err = LinuxDriverAllocContig(vmLinux, vma, compat_vm_pgoff(vma), size);
   }
   if (err) {
      return err;
   }
   /* Clear VM_IO, otherwise SuSE's kernels refuse to do get_user_pages */
   vma->vm_flags &= ~VM_IO;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 2, 3)
   vma->vm_file = filp;
   filp->f_count++;
#endif
   return 0;
}


typedef Bool (*SyncFunc)(void *data, unsigned cpu);

typedef struct {
   Atomic_uint32 numCPUs;
   Atomic_uint32 ready;
   Atomic_uint32 failures;
   Atomic_uint32 done;
   SyncFunc      func;
   void          *data;
} SyncFuncArgs;


/*
 *-----------------------------------------------------------------------------
 *
 * LinuxDriverSyncCallHook --
 *
 *      Called on each CPU, waits for them all to show up, and executes
 *      the callback.
 *
 * Results:
 *
 * Side effects:
 *      Whatever side effects the callback has.
 *
 *-----------------------------------------------------------------------------
 */

static void
LinuxDriverSyncCallHook(void *data)
{
   Bool success;
   uint32 numCPUs;
   volatile unsigned timeRemaining = 100000;
   SyncFuncArgs *args = (SyncFuncArgs *)data;
   unsigned cpu = smp_processor_id();

   /*
    * We need to careful about reading cpu_online_map on kernels that
    * have hot add/remove cpu support.  The kernel's smp_call_function
    * blocks hot add from occuring between the time it computes the set
    * of cpus it will IPI and when all those cpus have entered their IPI
    * handlers.  Additionally, we disabled preemption on the initiating
    * cpu during the entire sync call sequence.  So, since a cpu hot add
    * is initiated from process context, a cpu cannot be hot added until
    * at least one cpu has exited this code, and therefore it is safe
    * for the first cpu to reach this point to read cpu_online_map.
    *
    * Hot remove works by stopping the entire machine, which is done by
    * waiting for a set of kernel threads to be scheduled on all cpus.
    * This cannot happen until all cpus are preemptible.  Since the
    * initiating cpu has preemption disabled during this entire
    * sequence, this code is also safe from cpu hot remove.
    *
    * So, the first cpu to reach this code will read the same value of
    * cpu_online_map that was used by smp_call_function, and therefore
    * we can safely assume that numCPUs cpus will execute this routine.
    */
   Atomic_CMPXCHG32(&args->numCPUs, 0, compat_num_online_cpus());
   numCPUs = Atomic_Read(&args->numCPUs);

   Atomic_Inc(&args->ready);
   /*
    * Wait for all CPUs, but not forever since we could deadlock.  The
    * potential deadlock scenerio is this: cpu0 has IF=1 and holds a
    * lock.  cpu1 has IF=0 and is spinning waiting for the lock.
    */
   while (Atomic_Read(&args->ready) != numCPUs && --timeRemaining) ;

   /* Now simultaneously call the routine. */
   success = args->func(args->data, cpu);

   if (!timeRemaining || !success) {
      /* Indicate that we either timed out or the callback failed. */
      Atomic_Inc(&args->failures);
   }
   /* Indicate that we are finished. */
   Atomic_Inc(&args->done);
}


/*
 *-----------------------------------------------------------------------------
 *
 * LinuxDriverSyncCallOnEachCPU --
 *
 *      Calls func on each cpu at (nearly) the same time.
 *
 * Results:
 *      TRUE if func was called at the same time on all cpus.  Note that
 *      func is called regardless of whether all cpus showed up in time.
 *
 * Side effects:
 *      func's side effects, on all cpus.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
LinuxDriverSyncCallOnEachCPU(SyncFunc func,
			     void *data)
{
   SyncFuncArgs args;
   uintptr_t flags;

   args.func = func;
   args.data = data;

   Atomic_Write(&args.numCPUs, 0); // Must be calculated inside the callback.
   Atomic_Write(&args.ready, 0);
   Atomic_Write(&args.failures, 0);
   Atomic_Write(&args.done, 0);

   compat_preempt_disable();
   /*
    * Call all other CPUs, but do not wait so we can enter the callback
    * on this CPU too.
    */
   compat_smp_call_function(LinuxDriverSyncCallHook, &args, 0);
   /*
    * smp_call_function doesn't return until all cpus have been
    * interrupted.  It's safe to disable interrupts now that all other
    * cpus are in their IPI handlers.
    */
   SAVE_FLAGS(flags);
   CLEAR_INTERRUPTS();

   LinuxDriverSyncCallHook(&args);

   RESTORE_FLAGS(flags);
   compat_preempt_enable();

   /*
    * Wait for everyone else to finish so we can get an accurate
    * failures count.
    */
   while (Atomic_Read(&args.done) != Atomic_Read(&args.numCPUs)) ;

   /*
    * This routine failed if any CPU bailed out early to avoid deadlock,
    * or the callback routine failed on any CPU.  Both conditions are
    * recorded in the failures field.
    */
   return Atomic_Read(&args.failures) == 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * LinuxDriverReadTSC --
 *
 *      Callback that is executed simultaneously on all cpus to read the TSCs.
 *
 * Results:
 *      TRUE if the current cpu's TSC was stored into the array.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
LinuxDriverReadTSC(void *data,   // OUT: TSC values
                   unsigned cpu) // IN: the pcpu number
{
   TSCSet *tscSet = (TSCSet *)data;

   if (LIKELY(cpu < ARRAYSIZE(tscSet->TSCs))) {
      CPUID_FOR_SIDE_EFFECTS();	/* Serialize RDTSC. */
      tscSet->TSCs[cpu] = RDTSC();
      TSCSet_SetValid(tscSet, cpu);
      return TRUE;
   }
   return FALSE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * LinuxDriverWriteTSC --
 *
 *      Callback that is executed simultaneously on all cpus to write the TSCs.
 *
 * Results:
 *      TRUE if the current cpu's TSC was written.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
LinuxDriverWriteTSC(void *data,   // IN: TSC values
                    unsigned cpu) // IN: the pcpu number
{
   TSCSet *tscSet = (TSCSet *)data;

   if (LIKELY(cpu < ARRAYSIZE(tscSet->TSCs))) {
      __SET_MSR(MSR_TSC, tscSet->TSCs[cpu]);
      return TRUE;
   }
   return FALSE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * LinuxDriverSyncReadTSCs --
 *
 *      Simultaneously read the TSCs on all cpus.
 *
 * Results:
 *      The set of all TSCs.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
LinuxDriverSyncReadTSCs(TSCSet *tscSet) // OUT: TSC values
{
   TSCSet *tmpTSCSet = HostIF_AllocKernelMem(sizeof *tmpTSCSet, TRUE);
   unsigned i;
   Bool okay = FALSE;

   if (tmpTSCSet == NULL) {
      return FALSE;
   }

   /* Loop at least twice to warm up the cache. */
   for (i = 0; i < 1; i++) {
      memset(tmpTSCSet, 0, sizeof *tmpTSCSet);
      if (LinuxDriverSyncCallOnEachCPU(LinuxDriverReadTSC, tmpTSCSet)) {
         /* We return the last successful simultaneous read of the TSCs. */
         memcpy(tscSet, tmpTSCSet, sizeof *tmpTSCSet);
         okay = TRUE;
      }
   }
   HostIF_FreeKernelMem(tmpTSCSet);
   return okay;
}


/*
 *-----------------------------------------------------------------------------
 *
 * LinuxDriverSyncReadTSCs --
 *
 *      Simultaneously write the TSCs on all cpus.
 *
 * Results:
 *      The set of all TSCs.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
LinuxDriverSyncWriteTSCs(TSCSet *tscSet) // IN: TSC values
{
   return LinuxDriverSyncCallOnEachCPU(LinuxDriverWriteTSC, tscSet);
}


/*
 *-----------------------------------------------------------------------------
 *
 * LinuxDriver_Ioctl --
 *
 *      Main path for UserRPC
 *
 *      Be VERY careful with stack usage; gcc's stack allocation is iffy
 *      and allocations from individual "case" statements do not overlap,
 *      so it is easy to use kilobytes of stack space here.
 *
 * Results:
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

int
LinuxDriver_Ioctl(struct inode *inode,
                  struct file *filp,
                  u_int iocmd,
                  unsigned long ioarg)
{
   VMLinux *vmLinux = (VMLinux *) filp->private_data;
   int retval = 0;
   Vcpuid vcpuid;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 36)
   compat_mutex_lock(&linuxState.lock);
#endif

   switch (iocmd) {
   case IOCTL_VMX86_VERSION:
      retval = VMMON_VERSION;
      break;

   case IOCTL_VMX86_CREATE_VM: {
      if (vmLinux->vm != NULL) {
	 retval = -EINVAL;
	 break;
      }
      vmLinux->vm = Vmx86_CreateVM();
      if (vmLinux->vm == NULL) {
	 retval = -ENOMEM;
	 break;
      }
      retval = vmLinux->vm->userID;
      break;
   }

   case IOCTL_VMX86_RELEASE_VM: {
      VMDriver *vm;
      if (vmLinux->vm == NULL) {
	 retval = -EINVAL;
	 break;
      }
      vm = vmLinux->vm;
      vmLinux->vm = NULL;
      Vmx86_ReleaseVM(vm);
      break;
   }

   case IOCTL_VMX86_ALLOC_CROSSGDT: {
      InitBlock initBlock;
      if (vmLinux->vm == NULL) {
	 retval = -EINVAL;
	 break;
      }
      if (!Task_AllocCrossGDT(&initBlock)) {
	 retval = -EINVAL;
	 break;
      }
      retval = HostIF_CopyToUser((char*)ioarg, &initBlock,
				   sizeof initBlock);
      break;
   }

   case IOCTL_VMX86_INIT_VM: {
      InitBlock initParams;
      if (vmLinux->vm == NULL) {
	 retval = -EINVAL;
	 break;
      }
      retval = HostIF_CopyFromUser(&initParams, (char*)ioarg,
				   sizeof initParams);
      if (retval != 0) {
	 break;
      }
      if (Vmx86_InitVM(vmLinux->vm, &initParams)) {
	 retval = -EINVAL;
	 break;
      }
      retval = HostIF_CopyToUser((char*)ioarg, &initParams,
				   sizeof initParams);
      break;
   }

   case IOCTL_VMX86_INIT_CROSSGDT: {
      InitCrossGDT initCrossGDT;

      retval = HostIF_CopyFromUser(&initCrossGDT,
                                   (char *)ioarg,
                                   sizeof initCrossGDT);
      if ((retval == 0) && Task_InitCrossGDT(&initCrossGDT)) {
         retval = -EIO;
      }
      break;
   }

   case IOCTL_VMX86_INIT_NUMA_INFO: {
      NUMAInfoArgs *initParams;
      unsigned int numNodes;

      retval = HostIF_CopyFromUser(&numNodes, (char*)ioarg, sizeof(uint32));
      if (retval != 0) {
	 break;
      }
      if (numNodes > NUMA_MAX_NODES) {
         retval = -EINVAL;
         break;
      }
      initParams = HostIF_AllocKernelMem(NUMA_INFO_ARGS_SIZE(*initParams,
                                                             numNodes), FALSE);
      if (!initParams) {
         retval = -EINVAL;
         break;
      }
      retval = HostIF_CopyFromUser(initParams, (char*)ioarg,
                                   (NUMA_INFO_ARGS_SIZE(*initParams, numNodes)));
      if (retval != 0) {
	 HostIF_FreeKernelMem(initParams);
         break;
      }
      if (!Vmx86_InitNUMAInfo(initParams)) {
         retval = -EINVAL;
         HostIF_FreeKernelMem(initParams);
      }
      break;
   }

   case IOCTL_VMX86_GET_NUMA_MEM_STATS: {
      VMNUMAMemStatsArgs args;
      if (vmLinux->vm == NULL) {
	 retval = -EINVAL;
	 break;
      }
      if (!Vmx86_GetNUMAMemStats(vmLinux->vm, &args)) {
         retval = -EINVAL;
         break;
      }
      retval = HostIF_CopyToUser((void *)ioarg, &args, sizeof args);
      break;
   }

   case IOCTL_VMX86_LATE_INIT_VM:
      if (vmLinux->vm == NULL) {
	 retval = -EINVAL;
	 break;
      }
      if (Vmx86_LateInitVM(vmLinux->vm)) {
	 retval = -EINVAL;
	 break;
      }
      break;

   case IOCTL_VMX86_RUN_VM:
      if (vmLinux->vm == NULL) {
	 retval = -EINVAL;
	 break;
      }
      vcpuid = ioarg;

      if (vcpuid >= vmLinux->vm->numVCPUs) {
         retval = -EINVAL;
         break;
      }
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36)
      unlock_kernel();
      if (kernel_locked_by_current()) {
         retval = USERCALL_VMX86KRNLLOCK;
      } else {
         retval = Vmx86_RunVM(vmLinux->vm, vcpuid);
      }
      lock_kernel();
#else
      compat_mutex_unlock(&linuxState.lock);
      retval = Vmx86_RunVM(vmLinux->vm, vcpuid);
      compat_mutex_lock(&linuxState.lock);
#endif
      break;

   case IOCTL_VMX86_SET_UID:
#ifdef VMX86_DEVEL
      devel_suid();
#else
      retval = -EPERM;
#endif
      break;

   case IOCTL_VMX86_LOCK_PAGE: {
      VA64 uAddr;
      MPN mpn;

      if (vmLinux->vm == NULL) {
	 retval = -EINVAL;
	 break;
      }
      retval = HostIF_CopyFromUser(&uAddr, (void *)ioarg, sizeof uAddr);
      if (retval) {
         break;
      }
      mpn = Vmx86_LockPage(vmLinux->vm, uAddr, FALSE);
      retval = (int)mpn;
      // Make sure mpn is within 32 bits.
      ASSERT(mpn == (MPN)retval);
   } break;

   case IOCTL_VMX86_LOCK_PAGE_NEW: {
      VA64 uAddr;
      MPN mpn;

      if (vmLinux->vm == NULL) {
	 retval = -EINVAL;
	 break;
      }
      retval = HostIF_CopyFromUser(&uAddr, (void *)ioarg, sizeof uAddr);
      if (retval) {
         break;
      }
      mpn = Vmx86_LockPage(vmLinux->vm, uAddr, TRUE);
      retval = (int)mpn;
      // Make sure mpn is within 32 bits.
      ASSERT(mpn == (MPN)retval);
   } break;

   case IOCTL_VMX86_UNLOCK_PAGE: {
      VA64 uAddr;
      MPN mpn;

      if (vmLinux->vm == NULL) {
	 retval = -EINVAL;
	 break;
      }
      retval = HostIF_CopyFromUser(&uAddr, (void *)ioarg, sizeof uAddr);
      if (retval) {
         break;
      }
      mpn = Vmx86_UnlockPage(vmLinux->vm, uAddr);
      retval = (int)mpn;
      // Make sure mpn is within 32 bits.
      ASSERT(mpn == (MPN)retval);
   } break;

   case IOCTL_VMX86_UNLOCK_PAGE_BY_MPN: {
      VMMUnlockPageByMPN args;
      MPN mpn;

      if (vmLinux->vm == NULL) {
	 retval = -EINVAL;
	 break;
      }
      retval = HostIF_CopyFromUser(&args, (void *)ioarg, sizeof args);
      if (retval) {
         break;
      }
      mpn = Vmx86_UnlockPageByMPN(vmLinux->vm, args.mpn, args.uAddr);
      retval = (int)mpn;
      // Make sure mpn is within 32 bits.
      ASSERT(mpn == (MPN)retval);
   } break;

   case IOCTL_VMX86_LOOK_UP_MPN: {
      VA64 uAddr;
      MPN mpn;

      if (vmLinux->vm == NULL) {
	 retval = -EINVAL;
	 break;
      }
      retval = HostIF_CopyFromUser(&uAddr, (void *)ioarg, sizeof uAddr);
      if (retval) {
         break;
      }
      mpn = HostIF_LookupUserMPN(vmLinux->vm, uAddr);
      retval = (int)mpn;
      // Make sure mpn is within 32 bits.
      ASSERT(mpn == (MPN)retval);
   } break;

#if defined(__linux__) && defined(VMX86_DEVEL) && defined(VM_X86_64)
  case IOCTL_VMX86_LOOK_UP_LARGE_MPN: {
      void *addr = (void *)ioarg;
      MPN   mpn; 
      mpn = HostIF_LookupLargeMPN(addr);
      retval = (int)mpn;
      break;
   }
#endif

   case IOCTL_VMX86_GET_NUM_VMS: {
      retval = Vmx86_GetNumVMs();
      break;
   }

   case IOCTL_VMX86_GET_TOTAL_MEM_USAGE: {
      retval = Vmx86_GetTotalMemUsage();
      break;
   }

   case IOCTL_VMX86_SET_HARD_LIMIT: {
      int32 limit;
      retval = HostIF_CopyFromUser(&limit, (void *)ioarg, sizeof limit);
      if (retval != 0) {
	 break;
      }
      if (!Vmx86_SetConfiguredLockedPagesLimit(limit)) {
         retval = -EINVAL;
      }
      break;
   }

   case IOCTL_VMX86_ADMIT: {
      VMMemInfoArgs args;

      if (vmLinux->vm == NULL) {
	 retval = -EINVAL;
	 break;
      }
      retval = HostIF_CopyFromUser(&args, (void *)ioarg, sizeof args);
      if (retval != 0) {
	 break;
      }
      Vmx86_Admit(vmLinux->vm, &args);
      retval = HostIF_CopyToUser((void *)ioarg, &args, sizeof args);
      break;
   }

   case IOCTL_VMX86_READMIT: {
      OvhdMem_Deltas delta;
      if (vmLinux->vm == NULL) {
         retval = -EINVAL;
         break;
      }
      retval = HostIF_CopyFromUser(&delta, (void *)ioarg, sizeof delta);
      if (retval != 0) {
         break;
      }
      if (!Vmx86_Readmit(vmLinux->vm, &delta)) {
         retval = -1;
      }

      break;
   }

   case IOCTL_VMX86_UPDATE_MEM_INFO: {
      VMMemMgmtInfoPatch patch;
      if (vmLinux->vm == NULL) {
         retval = -EINVAL;
         break;
      }
      retval = HostIF_CopyFromUser(&patch, (void *)ioarg, sizeof patch);
      if (retval != 0) {
	 break;
      }
      Vmx86_UpdateMemInfo(vmLinux->vm, &patch);
      break;
   }

   case IOCTL_VMX86_GET_MEM_INFO: {
      VA64 uAddr;
      VMMemInfoArgs *userVA;
      VMMemInfoArgs in;
      VMMemInfoArgs *out;

      if (vmLinux->vm == NULL) {
	 retval = -EINVAL;
	 break;
      }

      retval = HostIF_CopyFromUser(&uAddr, (void *)ioarg, sizeof uAddr);
      if (retval) {
         break;
      }

      userVA = VA64ToPtr(uAddr);
      retval = HostIF_CopyFromUser(&in, userVA, sizeof in);
      if (retval) {
         break;
      }

      if (in.numVMs < 1 || in.numVMs > MAX_VMS) {
         retval = -EINVAL;
         break;
      }
      out = HostIF_AllocKernelMem(VM_GET_MEM_INFO_SIZE(in.numVMs), TRUE);
      if (!out) {
         retval = -ENOMEM;
         break;
      }

      *out = in;
      if (!Vmx86_GetMemInfoCopy(vmLinux->vm, out)) {
         HostIF_FreeKernelMem(out);
         retval = -ENOBUFS;
         break;
      }

      retval = HostIF_CopyToUser(userVA, out,
                                 VM_GET_MEM_INFO_SIZE(out->numVMs));
      HostIF_FreeKernelMem(out);
   } break;

   case IOCTL_VMX86_PAE_ENABLED:
      retval = Vmx86_PAEEnabled();
      break;

   case IOCTL_VMX86_VMX_ENABLED:
      retval = Vmx86_VMXEnabled();
      break;

   case IOCTL_VMX86_SVM_ENABLED_CPU:
   case IOCTL_VMX86_VT_ENABLED_CPU:
      if (ioarg != 0) {
         Vmx86_FixHVEnable(TRUE);
      }
      retval = Vmx86_HVEnabledCPUs();
      break;

   case IOCTL_VMX86_VT_SUPPORTED_CPU:
      retval = Vmx86_VTSupportedCPU();
      break;

   case IOCTL_VMX86_BROKEN_CPU_HELPER:
      retval = HostIF_BrokenCPUHelper();
      break;

   case IOCTL_VMX86_HOST_X86_64:
#ifdef VM_X86_64
      retval = TRUE;
#else
      retval = FALSE;
#endif
      break;

   case IOCTL_VMX86_APIC_INIT: {
      VMAPICInfo info;
      Bool setVMPtr;
      Bool probe;

      retval = HostIF_CopyFromUser(&info, (VMAPICInfo *)ioarg,
                                   sizeof info);
      if (retval != 0) {
         break;
      }
      setVMPtr = ((info.flags & APIC_FLAG_DISABLE_NMI) != 0);
      probe = ((info.flags & APIC_FLAG_PROBE) != 0);

      if (vmLinux->vm == NULL) {
	 retval = -EINVAL;
	 break;
      }
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 3, 20)
      // Kernel uses NMIs for deadlock detection -
      //  set APIC VMptr so that NMIs get disabled in the monitor
      setVMPtr = TRUE;
#endif
      retval = HostIF_APICInit(vmLinux->vm, setVMPtr, probe) ? 0 : -ENODEV;
      break;
   }

   case IOCTL_VMX86_SET_HOST_CLOCK_RATE: {
      if (vmLinux->vm == NULL) {
         retval = -EINVAL;
         break;
      }
      retval = -Vmx86_SetHostClockRate(vmLinux->vm, (int)ioarg);
      break;
   }

   case IOCTL_VMX86_ALLOW_CORE_DUMP:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0)
      if (current_euid().val == current_uid().val &&
         current_fsuid().val == current_uid().val &&
          current_egid().val == current_gid().val &&
         current_fsgid().val == current_gid().val) {
#else
      if (current_euid() == current_uid() &&
	  current_fsuid() == current_uid() &&
          current_egid() == current_gid() &&
	  current_fsgid() == current_gid()) {
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0)
         /* copied from set_dumpable() in fs/exec.c */
         unsigned long old, new;
         do {
            old = ACCESS_ONCE(current->mm->flags);
            new = (old & ~MMF_DUMPABLE_MASK) | SUID_DUMP_USER;
         } while (cmpxchg(&current->mm->flags, old, new) != old);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 23) || defined(MMF_DUMPABLE)
         /* Dump core, readable by user. */
         set_bit(MMF_DUMPABLE, &current->mm->flags);
         clear_bit(MMF_DUMP_SECURELY, &current->mm->flags);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 7)
	 current->mm->dumpable = 1;
#else
	 current->dumpable = 1;
#endif
         retval = 0;
      } else {
         retval = -EPERM;
      }
      break;

   case IOCTL_VMX86_SEND_IPI: {
      VCPUSet ipiTargets;
      Bool didBroadcast;

      if (vmLinux->vm == NULL) {
         retval = -EINVAL;
         break;
      }

      retval = HostIF_CopyFromUser(&ipiTargets, (VCPUSet *) ioarg,
                                   sizeof ipiTargets);

      if (retval == 0) {
         HostIF_IPI(vmLinux->vm, ipiTargets, TRUE, &didBroadcast);
      }

      break;
   }

   case IOCTL_VMX86_GET_IPI_VECTORS: {
      IPIVectors ipiVectors;

      ipiVectors.vectors[0] = CALL_FUNCTION_VECTOR;
#ifdef CALL_FUNCTION_SINGLE_VECTOR
      ipiVectors.vectors[1] = CALL_FUNCTION_SINGLE_VECTOR;
#else
      ipiVectors.vectors[1] = 0;
#endif

      retval = HostIF_CopyToUser((void *)ioarg, &ipiVectors, sizeof ipiVectors);
      break;
   }

   case IOCTL_VMX86_GET_KHZ_ESTIMATE:
      retval = Vmx86_GetkHzEstimate(&linuxState.startTime);
      break;

   case IOCTL_VMX86_ACK_USER_CALL:
      if (vmLinux->vm == NULL) {
         retval = -EINVAL;
         break;
      }
      vcpuid = (Vcpuid) ioarg;
      if (vcpuid >= vmLinux->vm->numVCPUs) {
         retval = -EINVAL;
         break;
      }
      HostIF_AckUserCall(vmLinux->vm, vcpuid);
      break;

   case IOCTL_VMX86_COMPLETE_USER_CALL:
      if (vmLinux->vm == NULL) {
	 retval = -EINVAL;
	 break;
      }
      vcpuid = (Vcpuid) ioarg;
      if (vcpuid >= vmLinux->vm->numVCPUs) {
         retval = -EINVAL;
         break;
      }
      Vmx86_CompleteUserCall(vmLinux->vm, vcpuid);
      break;

   case IOCTL_VMX86_GET_ALL_CPUID: {
      VA64 uAddr;
      CPUIDQuery *userVA;
      CPUIDQuery in;
      CPUIDQuery *out;

      retval = HostIF_CopyFromUser(&uAddr, (void *)ioarg, sizeof uAddr);
      if (retval) {
         break;
      }

      userVA = VA64ToPtr(uAddr);
      retval = HostIF_CopyFromUser(&in, userVA, sizeof in);
      if (retval) {
         break;
      }

      /*
       * Some kernels panic on kmalloc request larger than 128KB.
       * XXX This test should go inside HostIF_AllocKernelMem() then.
       */
      if (  in.numLogicalCPUs
          > (131072 - sizeof *out) / sizeof out->logicalCPUs[0]) {
         retval = -EINVAL;
         break;
      }
      out = HostIF_AllocKernelMem(
         sizeof *out + in.numLogicalCPUs * sizeof out->logicalCPUs[0],
         TRUE);
      if (!out) {
         retval = -ENOMEM;
         break;
      }

      *out = in;
      if (!HostIF_GetAllCpuInfo(out)) {
         HostIF_FreeKernelMem(out);
         retval = -ENOBUFS;
         break;
      }

      retval = HostIF_CopyToUser((int8 *)userVA + sizeof *userVA,
         &out->logicalCPUs[0],
         out->numLogicalCPUs * sizeof out->logicalCPUs[0]);
      HostIF_FreeKernelMem(out);
   } break;

   case IOCTL_VMX86_GET_ALL_MSRS: {
      VA64 uAddr;
      MSRQuery *userVA;
      MSRQuery in;
      MSRQuery *out;

      retval = HostIF_CopyFromUser(&uAddr, (void *)ioarg, sizeof uAddr);
      if (retval) {
         break;
      }

      userVA = VA64ToPtr(uAddr);
      retval = HostIF_CopyFromUser(&in, userVA, sizeof in);
      if (retval) {
         break;
      }

      /*
       * Some kernels panic on kmalloc request larger than 128KB.
       * XXX This test should go inside HostIF_AllocKernelMem() then.
       */
      if (  in.numLogicalCPUs
          > (131072 - sizeof *out) / sizeof out->logicalCPUs[0]) {
         retval = -EINVAL;
         break;
      }
      out = HostIF_AllocKernelMem(
         sizeof *out + in.numLogicalCPUs * sizeof out->logicalCPUs[0],
         TRUE);
      if (!out) {
         retval = -ENOMEM;
         break;
      }

      *out = in;
      if (!Vmx86_GetAllMSRs(out)) {
         HostIF_FreeKernelMem(out);
         retval = -ENOBUFS;
         break;
      }

      retval = HostIF_CopyToUser((int8 *)userVA + sizeof *userVA,
         &out->logicalCPUs[0],
         out->numLogicalCPUs * sizeof out->logicalCPUs[0]);
      HostIF_FreeKernelMem(out);
   } break;

   case IOCTL_VMX86_ALLOC_LOCKED_PAGES:
   case IOCTL_VMX86_FREE_LOCKED_PAGES:
      {
         VMMPNList req;

	 retval = HostIF_CopyFromUser(&req, (void*)ioarg, sizeof req);
	 if (retval) {
	    break;
	 }
	 if (!vmLinux->vm) {
	    retval = -EINVAL;
	    break;
	 }
         if (iocmd == IOCTL_VMX86_ALLOC_LOCKED_PAGES) {
            retval = Vmx86_AllocLockedPages(vmLinux->vm,
                                            req.mpn32List,
                                            req.mpnCount, FALSE);
         } else {
            retval = Vmx86_FreeLockedPages(vmLinux->vm,
                                           req.mpn32List,
                                           req.mpnCount, FALSE);
         }
      }
      break;

   case IOCTL_VMX86_GET_LOCKED_PAGES_LIST:
      {
         VMMPNList req;

	 retval = HostIF_CopyFromUser(&req, (void*)ioarg, sizeof req);
	 if (retval) {
	    break;
	 }
	 if (!vmLinux->vm) {
	    retval = -EINVAL;
	    break;
	 }
	 retval = Vmx86_GetLockedPageList(vmLinux->vm,
                                          req.mpn32List,
                                          req.mpnCount);
      }
      break;

   case IOCTL_VMX86_MARK_LOCKEDVARANGE_CLEAN: {
         struct VARange var;
         if (vmLinux->vm == NULL) {
            retval = -EINVAL;
            break;
         }
         if (HostIF_CopyFromUser(&var, (void *)ioarg, sizeof(struct VARange)) != 0) {
            retval = -EINVAL;
         } else {
            retval = HostIF_MarkLockedVARangeClean(vmLinux->vm, var.addr,
                                                   var.len, var.bv);
         }
      }
      break;

   case IOCTL_VMX86_READ_PAGE:
      {
         VMMReadWritePage req;

	 retval = HostIF_CopyFromUser(&req, (void*)ioarg, sizeof req);
	 if (retval) {
	    break;
	 }
	 retval = HostIF_ReadPage(req.mpn, req.uAddr, FALSE);
	 break;
      }

   case IOCTL_VMX86_WRITE_PAGE:
      {
         VMMReadWritePage req;

	 retval = HostIF_CopyFromUser(&req, (void*)ioarg, sizeof req);
	 if (retval) {
	    break;
	 }
	 retval = HostIF_WritePage(req.mpn, req.uAddr, FALSE);
	 break;
      }

   case IOCTL_VMX86_COW_SHARE:
   {
      retval = -ENOTTY;
      break;
   }

   case IOCTL_VMX86_COW_INC_ZERO_REF:
   {
      retval = -ENOTTY;
      break;
   }

   case IOCTL_VMX86_COW_GET_ZERO_MPN:
   {
      retval = -ENOTTY;
      break;
   }
   case IOCTL_VMX86_COW_CHECK:
   {
      retval = -ENOTTY;
      break;
   }

  case IOCTL_VMX86_COW_UPDATE_HINT:
   {
      retval = -ENOTTY;
      break;
   }

   case IOCTL_VMX86_COW_COPY_PAGE:
   {
      retval = -ENOTTY;
      break;
   }

   case IOCTL_VMX86_SET_THREAD_AFFINITY:
   case IOCTL_VMX86_GET_THREAD_AFFINITY:
   {
      struct VMMonAffinity vma;
      if (HostIF_CopyFromUser(&vma, (void *)ioarg, sizeof vma)) {
         retval = -EFAULT;
         break;
      }
      /* Support only current thread, it seems sufficient */
      if (vma.pid != 0 && vma.pid != current->pid) {
         retval = -ESRCH;
         break;
      }
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 21) && \
    LINUX_VERSION_CODE < KERNEL_VERSION(2, 5, 0)
      if (iocmd == IOCTL_VMX86_SET_THREAD_AFFINITY) {
         vma.affinity &= cpu_online_map;
         if (vma.affinity == 0) {
            retval = -EINVAL;
         } else {
            set_cpus_allowed(current, vma.affinity);
            retval = 0;
         }
      } else {
         vma.affinity = current->cpus_allowed;
         if (HostIF_CopyToUser((void *)ioarg, &vma, sizeof vma)) {
            retval = -EFAULT;
         } else {
            retval = 0;
         }
      }
#else
      /* No way before 2.4.21, use affinity syscalls after 2.5.0 */
      retval = -ENOSYS;
#endif
      break;
   }

   case IOCTL_VMX86_APIC_ID: {
      uint8 apicId;

      apicId = HostIF_APIC_ID();
      retval = HostIF_CopyToUser((void *)ioarg, &apicId, sizeof apicId);
      break;
   }

   case IOCTL_VMX86_SET_POLL_TIMEOUT_PTR: {
      vmLinux->pollTimeoutPtr = NULL;
      HostIF_UnmapUserMem(&vmLinux->pollTimeoutPage);
      if (ioarg != 0) {
	 vmLinux->pollTimeoutPtr =
	    HostIF_MapUserMem((VA)ioarg, sizeof *vmLinux->pollTimeoutPtr,
			      &vmLinux->pollTimeoutPage);
	 if (vmLinux->pollTimeoutPtr == NULL) {
	    retval = -EINVAL;
	    break;
	 }
      }
      break;
   }

   case IOCTL_VMX86_GET_KERNEL_CLOCK_RATE:
      retval = HZ;
      break;

   case IOCTL_VMX86_FAST_SUSP_RES_SET_OTHER_FLAG: {
      if (vmLinux->vm == NULL) {
         retval = -EINVAL;
         break;
      }
      retval = Vmx86_FastSuspResSetOtherFlag(vmLinux->vm, ioarg);
      break;
   }

   case IOCTL_VMX86_FAST_SUSP_RES_GET_MY_FLAG: {
      if (vmLinux->vm == NULL) {
         retval = -EINVAL;
         break;
      }
      retval = Vmx86_FastSuspResGetMyFlag(vmLinux->vm, ioarg);
      break;
   }

   case IOCTL_VMX86_GET_REFERENCE_CLOCK_HZ: {
      uint64 refClockHz = HostIF_UptimeFrequency();
      retval = HostIF_CopyToUser((void *)ioarg, &refClockHz, sizeof refClockHz);
      break;
   }

   case IOCTL_VMX86_INIT_PSEUDO_TSC: {
      PTSCInitParams params;
      retval = HostIF_CopyFromUser(&params, (void *)ioarg, sizeof params);
      if (retval != 0) {
	 break;
      }
      Vmx86_InitPseudoTSC(params.forceRefClock, params.forceTSC,
                          &params.refClockToTSC, &params.tscHz);
      retval = HostIF_CopyToUser((void *)ioarg, &params, sizeof params);
      break;
   }

   case IOCTL_VMX86_CHECK_PSEUDO_TSC: {
      PTSCCheckParams params;
      retval = HostIF_CopyFromUser(&params, (void *)ioarg, sizeof params);
      if (retval != 0) {
	 break;
      }
      params.usingRefClock = Vmx86_CheckPseudoTSC(&params.lastTSC,
						  &params.lastRC);

      retval = HostIF_CopyToUser((void *)ioarg, &params, sizeof params);
      break;
   }

   case IOCTL_VMX86_GET_PSEUDO_TSC: {
      uint64 ptsc = Vmx86_GetPseudoTSC();
      retval = HostIF_CopyToUser((void *)ioarg, &ptsc, sizeof ptsc);
      break;
   }

   case IOCTL_VMX86_SET_HOST_CLOCK_PRIORITY: {
      /*
       * This affects the global fast clock priority, and it only
       * takes effect when the fast clock rate transitions from zero
       * to a non-zero value.
       *
       * This is used to allow VMs to optionally work around
       * bug 218750 by disabling our default priority boost. If any
       * VM chooses to apply this workaround, the effect is permanent
       * until vmmon is reloaded!
       */
      HostIF_FastClockLock(3);
      linuxState.fastClockPriority = MAX(-20, MIN(19, (int)ioarg));
      HostIF_FastClockUnlock(3);
      retval = 0;
      break;
   }

   case IOCTL_VMX86_SYNC_GET_TSCS: {
      TSCSet *tscSet = HostIF_AllocKernelMem(sizeof *tscSet, TRUE);
      if (tscSet != NULL) {
         if (LinuxDriverSyncReadTSCs(tscSet)) {
            retval = HostIF_CopyToUser((void *)ioarg, tscSet, sizeof *tscSet);
          } else {
            retval = -EBUSY;
         }
         HostIF_FreeKernelMem(tscSet);
      } else {
         retval = -ENOMEM;
      }
      break;
   }

   case IOCTL_VMX86_SYNC_SET_TSCS: {
      TSCSet *tscSet = HostIF_AllocKernelMem(sizeof *tscSet, TRUE);
      if (tscSet != NULL) {
         retval = HostIF_CopyFromUser(tscSet, (void *)ioarg, sizeof *tscSet);
         if (retval == 0) {
            if (LinuxDriverSyncWriteTSCs(tscSet)) {
               ; // Success
            } else {
               retval = -EBUSY;
            }
         }
         HostIF_FreeKernelMem(tscSet);
      } else {
         retval = -ENOMEM;
      }
      break;
   }

   case IOCTL_VMX86_USING_SWAPBACKED_PAGEFILE: {
      if (vmLinux->vm == NULL) {
         retval = -EINVAL;
         break;
      }
      retval = 0;
      vmLinux->vm->vmhost->swapBacked = TRUE;
      break;
   }

   case IOCTL_VMX86_USING_MLOCK: {
      if (vmLinux->vm == NULL) {
         retval = -EINVAL;
         break;
      }
      retval = 0;
      vmLinux->vm->vmhost->usingMlock = TRUE;
      break;
   }
   case IOCTL_VMX86_SET_HOST_SWAP_SIZE: {
      uint64 swapSize;
      retval = HostIF_CopyFromUser(&swapSize, (void *)ioarg, sizeof swapSize);
      if (retval != 0) {
         Warning("Could not copy swap size from user, status %d\n", retval);
	 break;
      }
      linuxState.swapSize = swapSize;
      break;
   }
#ifdef HOSTED_IOMMU_SUPPORT
   case IOCTL_VMX86_IOMMU_SETUP_MMU: {
      if (vmLinux->vm == NULL) {
         retval = -EINVAL;
         break;
      }
      retval = IOMMU_SetupMMU(vmLinux, (PassthruIOMMUMap *)ioarg);
      break;
   }

   case IOCTL_VMX86_IOMMU_REGISTER_DEVICE: {
      if (vmLinux->vm == NULL) {
         retval = -EINVAL;
         break;
      }
      retval = IOMMU_RegisterDevice(vmLinux, (uint32)ioarg);
      break;
   }

   case IOCTL_VMX86_IOMMU_UNREGISTER_DEVICE: {
      retval = IOMMU_UnregisterDevice((uint32)ioarg);
      break;
   }
#endif

   default:
      Warning("Unknown ioctl %d\n", iocmd);
      retval = -EINVAL;
   }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 36)
   compat_mutex_unlock(&linuxState.lock);
#endif
   return retval;
}


#if defined(HAVE_UNLOCKED_IOCTL) || defined(HAVE_COMPAT_IOCTL)
/*
 *-----------------------------------------------------------------------------
 *
 * LinuxDriver_UnlockedIoctl --
 *
 *      Main path for UserRPC.
 *
 *      Unfortunately our LinuxDriver_Ioctl needs some surgery before it can
 *      run without big kernel lock, so for now we do not use this
 *      as unlocked_ioctl handler.  Though we could, it does not matter
 *      who'll obtain big kernel lock, whether we or our caller...
 *
 * Results:
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static long
LinuxDriver_UnlockedIoctl(struct file *filp,
                          u_int iocmd,
                          unsigned long ioarg)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36)
   long err;

   lock_kernel();
   err = LinuxDriver_Ioctl(NULL, filp, iocmd, ioarg);
   unlock_kernel();
   return err;
#else
   return LinuxDriver_Ioctl(NULL, filp, iocmd, ioarg);
#endif
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * LinuxDriverQueue --
 *
 *      add the vmLinux to the global queue
 *
 * Results:
 *
 *      void
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
LinuxDriverQueue(VMLinux *vmLinux)
{
   /*
    * insert in global vm queue
    */

   HostIF_GlobalLock(12);

   vmLinux->next = linuxState.head;
   linuxState.head = vmLinux;

   HostIF_GlobalUnlock(12);
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxDriveDequeue --
 *
 *      remove from active list
 *
 * Results:
 *
 *      void
 * Side effects:
 *      printk if it is not in the list (error condition)
 *
 *----------------------------------------------------------------------
 */
static void
LinuxDriverDequeue(VMLinux *vmLinux)
{
   VMLinux **p;

   HostIF_GlobalLock(13);
   for (p = &linuxState.head; *p != vmLinux; p = &(*p)->next) {
      ASSERT(*p != NULL);
   }
   *p = vmLinux->next;
   vmLinux->next = NULL;
   HostIF_GlobalUnlock(13);
}


/*
 *----------------------------------------------------------------------
 *
 * CheckPadding --
 *
 *      check for expected padding --
 *      this check currently fails on the egcs compiler
 *
 * Results:
 *
 *      TRUE if the check succeeds -- module will be loaded
 *
 *
 *
 * Side effects:
 *      output to kernel log on error
 *
 *----------------------------------------------------------------------
 */
static Bool
LinuxDriverCheckPadding(void)
{
   DTRWords32 dtr;
   uint16 *x;

   memset(&dtr, 0, sizeof dtr);
   dtr.dtr.limit = 0x1111;
   dtr.dtr.offset = 0x22223333;

   x = (uint16*)&dtr;

   if (x[0] == 0x1111 && x[1] == 0x3333 && x[2] == 0x2222) {
   } else {
      Warning("DTR padding\n");
      goto error;
   }

   return TRUE;

error:
   printk("/dev/vmmon: Cannot load module. Use standard gcc compiler\n");
   return FALSE;
}


#if defined(DO_PM24)
/*
 *----------------------------------------------------------------------
 *
 * LinuxDriverPMImpl --
 *
 *      Implementation-agnostic power management hook.
 *
 * Results:
 *      0 to acknowledge PM event, error otherwise.
 *
 * Side effects:
 *      Fixes VT-enable state, if applicable.
 *
 *----------------------------------------------------------------------
 */
static int
LinuxDriverPMImpl(LinuxDriverPMState state)  // IN
{
   if (state == LINUXDRIVERPM_RESUME) {
      Vmx86_FixHVEnable(FALSE);
   }
   return 0;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * LinuxDriverPM24Callback --
 * LinuxDriverAPMCallback --
 *
 *      Implementation-specific power management hooks.
 *
 * Results:
 *      0 to acknowledge PM event, error otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
#ifdef DO_PM24
static int
LinuxDriverPM24Callback(struct pm_dev *dev, pm_request_t rqst, void *data)
{
   if (rqst == PM_SUSPEND) {
      return LinuxDriverPMImpl(LINUXDRIVERPM_SUSPEND);
   } else if (rqst == PM_RESUME) {
      return LinuxDriverPMImpl(LINUXDRIVERPM_RESUME);
   } else {
      return 0; /* 0 for success - most states ignored. */
   }
}
#endif

MODULE_AUTHOR("VMware, Inc.");
MODULE_DESCRIPTION("VMware Virtual Machine Monitor.");
MODULE_LICENSE("GPL v2");
/*
 * Starting with SLE10sp2, Novell requires that IHVs sign a support agreement
 * with them and mark their kernel modules as externally supported via a
 * change to the module header. If this isn't done, the module will not load
 * by default (i.e., neither mkinitrd nor modprobe will accept it).
 */
MODULE_INFO(supported, "external");
