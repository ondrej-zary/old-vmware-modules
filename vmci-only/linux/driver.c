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

/* Must come before any kernel header file */
#include "driver-config.h"

#define EXPORT_SYMTAB

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

#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/smp.h>

#include "compat_file.h"
#include "compat_highmem.h"
#include "compat_interrupt.h"
#include "compat_kernel.h"
#include "compat_mm.h"
#include "compat_module.h"
#if defined(HAVE_COMPAT_IOCTL) || defined(HAVE_UNLOCKED_IOCTL)
#   include "compat_mutex.h"
#endif
#include "compat_sched.h"
#include "compat_slab.h"
#include "compat_uaccess.h"
#include "compat_version.h"

#include "vmware.h"
#include "driverLog.h"
#include "circList.h"
#include "pgtbl.h"
#include "vmci_defs.h"
#include "vmci_infrastructure.h"
#include "vmci_iocontrols.h"
#include "vmci_kernel_if.h"
#include "vmciCommonInt.h"
#include "vmciDatagram.h"
#include "vmciDriver.h"
#include "vmciDsInt.h"
#include "vmciGroup.h"
#include "vmciHostKernelAPI.h"
#include "vmciProcess.h"
#include "vmciQueuePair.h"


/*
 * Per-instance driver state
 */

typedef struct VMCILinux {
   union {
      VMCIContext *context;
      VMCIProcess *process;
      VMCIDatagramProcess *dgmProc;
   } ct;
   int userVersion;
   VMCIObjType ctType;
#if defined(HAVE_COMPAT_IOCTL) || defined(HAVE_UNLOCKED_IOCTL)
   compat_mutex_t lock;
#endif
} VMCILinux;

#if defined(HAVE_COMPAT_IOCTL) || defined(HAVE_UNLOCKED_IOCTL)
#define LinuxDriverLockIoctlPerFD(mutex) compat_mutex_lock(mutex)
#define LinuxDriverUnlockIoctlPerFD(mutex) compat_mutex_unlock(mutex)
#else
#define LinuxDriverLockIoctlPerFD(mutex) do {} while (0)
#define LinuxDriverUnlockIoctlPerFD(mutex) do {} while (0)
#endif

/*
 * Static driver state.
 */

#define VM_DEVICE_NAME_SIZE 32
#define LINUXLOG_BUFFER_SIZE  1024

typedef struct VMCILinuxState {
   int major;
   int minor;
   struct miscdevice misc;
   char deviceName[VM_DEVICE_NAME_SIZE];
   char buf[LINUXLOG_BUFFER_SIZE];
} VMCILinuxState;

static struct VMCILinuxState linuxState;

static int VMCISetupNotify(VMCIContext *context, VA notifyUVA);


/*
 *----------------------------------------------------------------------
 *
 * Device Driver Interface --
 *
 *      Implements VMCI by implementing open/close/ioctl functions
 *
 *
 *----------------------------------------------------------------------
 */

static int LinuxDriver_Open(struct inode *inode, struct file *filp);

static int LinuxDriver_Ioctl(struct inode *inode, struct file *filp,
                             u_int iocmd, unsigned long ioarg);

#if defined(HAVE_COMPAT_IOCTL) || defined(HAVE_UNLOCKED_IOCTL)
static long LinuxDriver_UnlockedIoctl(struct file *filp,
                                      u_int iocmd, unsigned long ioarg);
#endif

static int LinuxDriver_Close(struct inode *inode, struct file *filp);
static unsigned int LinuxDriverPoll(struct file *file, poll_table *wait);

static struct file_operations vmuser_fops;


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

static int
register_ioctl32_handlers(void)
{
#ifndef HAVE_COMPAT_IOCTL
   {
      int i;

      for (i = IOCTL_VMCI_FIRST; i < IOCTL_VMCI_LAST; i++) {
         int retval = register_ioctl32_conversion(i,
                                                  LinuxDriver_Ioctl32_Handler);

         if (retval) {
            Warning("Fail to register ioctl32 conversion for cmd %d\n", i);
            return retval;
         }
      }

      for (i = IOCTL_VMCI_FIRST2; i < IOCTL_VMCI_LAST2; i++) {
         int retval = register_ioctl32_conversion(i,
                                                  LinuxDriver_Ioctl32_Handler);

         if (retval) {
            Warning("Fail to register ioctl32 conversion for cmd %d\n", i);
            return retval;
         }
      }
   }
#endif /* !HAVE_COMPAT_IOCTL */
   return 0;
}

static void
unregister_ioctl32_handlers(void)
{
#ifndef HAVE_COMPAT_IOCTL
   {
      int i;

      for (i = IOCTL_VMCI_FIRST; i < IOCTL_VMCI_LAST; i++) {
         int retval = unregister_ioctl32_conversion(i);

         if (retval) {
            Warning("Fail to unregister ioctl32 conversion for cmd %d\n", i);
         }
      }

      for (i = IOCTL_VMCI_FIRST2; i < IOCTL_VMCI_LAST2; i++) {
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

   DriverLog_Init("/dev/vmci");

   /* Initialize VMCI core and APIs. */
   if (VMCI_Init() < VMCI_SUCCESS) {
      return -ENOMEM;
   }

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

   sprintf(linuxState.deviceName, "vmci");
   linuxState.major = 10;
   linuxState.misc.minor = MISC_DYNAMIC_MINOR;
   linuxState.misc.name = linuxState.deviceName;
   linuxState.misc.fops = &vmuser_fops;

   retval = misc_register(&linuxState.misc);

   if (retval) {
      Warning("Module %s: error %u registering with major=%d minor=%d\n",
	      linuxState.deviceName, -retval, linuxState.major, linuxState.minor);
      return -ENOENT;
   }
   linuxState.minor = linuxState.misc.minor;
   Log("Module %s: registered with major=%d minor=%d\n",
       linuxState.deviceName, linuxState.major, linuxState.minor);


   retval = register_ioctl32_handlers();
   if (retval) {
      misc_deregister(&linuxState.misc);
      return retval;
   }

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
   int retval;

   unregister_ioctl32_handlers();

   VMCI_Cleanup();

   /*
    * XXX smp race?
    */
   retval = misc_deregister(&linuxState.misc);

   if (retval) {
      Warning("Module %s: error unregistering\n", linuxState.deviceName);
   } else {
      Log("Module %s: unloaded\n", linuxState.deviceName);
   }
}



/*
 *----------------------------------------------------------------------
 *
 * LinuxDriver_Open  --
 *
 *      called on open of /dev/vmci. Use count used
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
   VMCILinux *vmciLinux;

   vmciLinux = kmalloc(sizeof *vmciLinux, GFP_KERNEL);
   if (vmciLinux == NULL) {
      return -ENOMEM;
   }
   memset(vmciLinux, 0, sizeof *vmciLinux);
   vmciLinux->ctType = VMCIOBJ_NOT_SET;
   vmciLinux->userVersion = 0;
#if defined(HAVE_COMPAT_IOCTL) || defined(HAVE_UNLOCKED_IOCTL)
   compat_mutex_init(&vmciLinux->lock);
#endif

   filp->private_data = vmciLinux;

   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxDriver_Close  --
 *
 *      called on close of /dev/vmci, most often when the
 *      process exits. Decrement use count, allowing for possible uninstalling
 *      of the module.
 *
 *----------------------------------------------------------------------
 */

static int
LinuxDriver_Close(struct inode *inode, // IN
                  struct file *filp)   // IN
{
   VMCILinux *vmciLinux;

   vmciLinux = (VMCILinux *)filp->private_data;
   ASSERT(vmciLinux);

   if (vmciLinux->ctType == VMCIOBJ_CONTEXT) {
      VMCIId cid;
      
      ASSERT(vmciLinux->ct.context);
      cid = VMCIContext_GetId(vmciLinux->ct.context);

      /*
       * Remove the context from the datagram and DS API groups. Meaning it
       * can no longer access the API functions.
       */
      VMCIDs_RemoveContext(cid);
      
      /* Remove context from the public group handle. */
      VMCIPublicGroup_RemoveContext(cid);

      VMCIContext_ReleaseContext(vmciLinux->ct.context);
      vmciLinux->ct.context = NULL;
   } else if (vmciLinux->ctType == VMCIOBJ_PROCESS) {
      VMCIProcess_Destroy(vmciLinux->ct.process);
      vmciLinux->ct.process = NULL;
   } else if (vmciLinux->ctType == VMCIOBJ_DATAGRAM_PROCESS) {
      VMCIDatagramProcess_Destroy(vmciLinux->ct.dgmProc);
      vmciLinux->ct.dgmProc = NULL;
   }
   vmciLinux->ctType = VMCIOBJ_NOT_SET;

   kfree(vmciLinux);
   filp->private_data = NULL;
   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * LinuxDriverPoll  --
 *
 *      This is used to wake up the VMX when a VMCI call arrives, or
 *      to wake up select() or poll() at the next clock tick.
 *
 *----------------------------------------------------------------------
 */

static unsigned int
LinuxDriverPoll(struct file *filp,
		poll_table *wait)
{
   VMCILockFlags flags;
   VMCILinux *vmciLinux = (VMCILinux *) filp->private_data;
   unsigned int mask = 0;

   if (vmciLinux->ctType == VMCIOBJ_CONTEXT) {
      ASSERT(vmciLinux->ct.context != NULL);
      /* 
       * Check for VMCI calls to this VM context. 
       */

      if (wait != NULL) {
         poll_wait(filp, &vmciLinux->ct.context->hostContext.waitQueue, wait);
      }

      VMCI_GrabLock(&vmciLinux->ct.context->lock, &flags);
      if (vmciLinux->ct.context->pendingDatagrams > 0) {
         mask = POLLIN;
      }
      VMCI_ReleaseLock(&vmciLinux->ct.context->lock, flags);

   } else if (vmciLinux->ctType == VMCIOBJ_PROCESS) {
      /* nop */
   } else if (vmciLinux->ctType == VMCIOBJ_DATAGRAM_PROCESS) {
      ASSERT(vmciLinux->ct.dgmProc);

      /* 
       * Check for messages to this datagram fd. 
       */

      if (wait) {
         poll_wait(filp, &vmciLinux->ct.dgmProc->host.waitQueue, wait);
      }

      VMCI_GrabLock(&vmciLinux->ct.dgmProc->lock, &flags);
      if (vmciLinux->ct.dgmProc->pendingDatagrams > 0) {
         mask = POLLIN;
      }
      VMCI_ReleaseLock(&vmciLinux->ct.dgmProc->lock, flags);
   }
   return mask;
}


/*
 *-----------------------------------------------------------------------------
 *
 * LinuxDriver_Ioctl --
 *
 *      Main path for UserRPC
 *
 * Results:
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static int
LinuxDriver_Ioctl(struct inode *inode,
                  struct file *filp,
                  u_int iocmd,
                  unsigned long ioarg)
{
   VMCILinux *vmciLinux = (VMCILinux *) filp->private_data;
   int retval = 0;

   switch (iocmd) {
   case IOCTL_VMCI_VERSION2: {
      int verFromUser;

      if (copy_from_user(&verFromUser, (void *)ioarg, sizeof verFromUser)) {
         retval = -EFAULT;
         break;
      }

      vmciLinux->userVersion = verFromUser;
   }
      /* Fall through. */
   case IOCTL_VMCI_VERSION:
      /*
       * The basic logic here is:
       *
       * If the user sends in a version of 0 tell it our version.
       * If the user didn't send in a version, tell it our version.
       * If the user sent in an old version, tell it -its- version.
       * If the user sent in an newer version, tell it our version.
       *
       * The rationale behind telling the caller its version is that
       * Workstation 6.5 required that VMX and VMCI kernel module were
       * version sync'd.  All new VMX users will be programmed to
       * handle the VMCI kernel module version.
       */

      if (vmciLinux->userVersion > 0 &&
          vmciLinux->userVersion < VMCI_VERSION_HOSTQP) {
         retval = vmciLinux->userVersion;
      } else {
         retval = VMCI_VERSION;
      }
      retval = VMCI_VERSION;
      break;

   case IOCTL_VMCI_INIT_CONTEXT: {
      VMCIInitBlock initBlock;

      retval = copy_from_user(&initBlock, (void *)ioarg, sizeof initBlock);
      if (retval != 0) {
         Log("VMCI: Error reading init block.\n");
         retval = -EFAULT;
         break;
      }

      LinuxDriverLockIoctlPerFD(&vmciLinux->lock);
      if (vmciLinux->ctType != VMCIOBJ_NOT_SET) {
         Log("VMCI: Received VMCI init on initialized handle\n");
         retval = -EINVAL;
         goto init_release;
      }

      if (initBlock.flags & ~VMCI_PRIVILEGE_FLAG_RESTRICTED) {
         Log("VMCI: Unsupported VMCI restriction flag.\n");
         retval = -EINVAL;
         goto init_release;
      }

      retval = VMCIContext_InitContext(initBlock.cid, initBlock.flags,
                                       0 /* Unused */, vmciLinux->userVersion,
                                       &vmciLinux->ct.context);
      if (retval < VMCI_SUCCESS) {
         Log("VMCI: Error initializing context.\n");
         retval = retval == VMCI_ERROR_DUPLICATE_ENTRY ? -EEXIST : -EINVAL;
         goto init_release;
      }

      /* 
       * Copy cid to userlevel, we do this to allow the VMX to enforce its 
       * policy on cid generation.
       */
      initBlock.cid = VMCIContext_GetId(vmciLinux->ct.context); 
      retval = copy_to_user((void *)ioarg, &initBlock, sizeof initBlock); 
      if (retval != 0) {
	 VMCIContext_ReleaseContext(vmciLinux->ct.context);
	 vmciLinux->ct.context = NULL;
	 Log("VMCI: Error writing init block.\n");
	 retval = -EFAULT;
	 goto init_release;
      }
      ASSERT(initBlock.cid != VMCI_INVALID_ID);

      /* Give VM context access to the datagram and DS API. */
      VMCIDs_AddContext(initBlock.cid);

      /* Add VM to the public group handle. */
      VMCIPublicGroup_AddContext(initBlock.cid);

      vmciLinux->ctType = VMCIOBJ_CONTEXT;
      
     init_release:
      LinuxDriverUnlockIoctlPerFD(&vmciLinux->lock);
      break;
   }

   case IOCTL_VMCI_CREATE_PROCESS: {
      LinuxDriverLockIoctlPerFD(&vmciLinux->lock);
      if (vmciLinux->ctType != VMCIOBJ_NOT_SET) {
         Log("VMCI: Received VMCI init on initialized handle\n");
         retval = -EINVAL;
         goto create_release;
      }

      if (VMCIProcess_Create(&vmciLinux->ct.process) < VMCI_SUCCESS) {
         Log("VMCI: Error initializing process.\n");
         retval = -EINVAL;
         goto create_release;
      }
      vmciLinux->ctType = VMCIOBJ_PROCESS;

     create_release:
      LinuxDriverUnlockIoctlPerFD(&vmciLinux->lock);
      break;
   }

   case IOCTL_VMCI_CREATE_DATAGRAM_PROCESS: {
      VMCIDatagramCreateInfo dgCreateInfo;
      VMCIDatagramProcess *dgmProc;

      /* Get datagram create info. */
      retval = copy_from_user(&dgCreateInfo, (char *)ioarg, sizeof dgCreateInfo);
      if (retval != 0) {
         Log("VMCI: Error getting datagram create info, %d\n", retval);
         retval = -EFAULT;
	 break;
      }

      LinuxDriverLockIoctlPerFD(&vmciLinux->lock);
      if (vmciLinux->ctType != VMCIOBJ_NOT_SET) {
         Log("VMCI: Received IOCTLCMD_VMCI_CREATE_DATAGRAM_PROCESS on initialized handle\n");
         retval = -EINVAL;
         goto create_dg_release;
      }

      /* Create process and datagram. */
      if (VMCIDatagramProcess_Create(&dgmProc, &dgCreateInfo,
                                     0 /* Unused */) < VMCI_SUCCESS) {
	 retval = -EINVAL;
	 goto create_dg_release;
      }
      retval = copy_to_user((void *)ioarg, &dgCreateInfo, sizeof dgCreateInfo);
      if (retval != 0) {
	 VMCIDatagramProcess_Destroy(dgmProc);
         Log("VMCI: Error copying create info out, %d.\n", retval);
         retval = -EFAULT;
	 break;
      }
      vmciLinux->ct.dgmProc = dgmProc;
      vmciLinux->ctType = VMCIOBJ_DATAGRAM_PROCESS;

     create_dg_release:
      LinuxDriverUnlockIoctlPerFD(&vmciLinux->lock);
      break;
   }

   case IOCTL_VMCI_DATAGRAM_SEND: {
      VMCIDatagramSendRecvInfo sendInfo;
      VMCIDatagram *dg = NULL;
      VMCIId cid;

      if (vmciLinux->ctType != VMCIOBJ_DATAGRAM_PROCESS &&
	  vmciLinux->ctType != VMCIOBJ_CONTEXT) {
         Warning("VMCI: Ioctl %d only valid for context and process datagram "
		 "handle.\n", iocmd);
         retval = -EINVAL;
         break;
      }

      retval = copy_from_user(&sendInfo, (void *) ioarg, sizeof sendInfo);
      if (retval) {
         Warning("VMCI: copy_from_user failed.\n");
         retval = -EFAULT;
         break;
      }

      if (sendInfo.len > VMCI_MAX_DG_SIZE) {
         Warning("VMCI: datagram size too big.\n");
	 retval = -EINVAL;
	 break;
      }

      if (sendInfo.len < sizeof *dg) {
         Warning("VMCI: datagram size too small.\n");
	 retval = -EINVAL;
	 break;
      }

      dg = VMCI_AllocKernelMem(sendInfo.len, VMCI_MEMORY_NORMAL);
      if (dg == NULL) {
         Log("VMCI: Cannot allocate memory to dispatch datagram.\n");
         retval = -ENOMEM;
         break;
      }

      retval = copy_from_user(dg, (char *)(VA)sendInfo.addr, sendInfo.len);
      if (retval != 0) {
         Log("VMCI: Error getting datagram: %d\n", retval);
         VMCI_FreeKernelMem(dg, sendInfo.len);
         retval = -EFAULT;
         break;
      }

      VMCI_DEBUG_LOG(("VMCI: Datagram dst handle 0x%"FMT64"x, "
	        "src handle 0x%"FMT64"x, payload size %"FMT64"u.\n",
                dg->dstHandle, dg->srcHandle, dg->payloadSize));

      /* Get source context id. */
      if (vmciLinux->ctType == VMCIOBJ_CONTEXT) {
	 ASSERT(vmciLinux->ct.context);
	 cid = VMCIContext_GetId(vmciLinux->ct.context);               
      } else {
	 /* XXX Will change to dynamic id when we make host context id random. */
	 cid = VMCI_HOST_CONTEXT_ID;
      }
      ASSERT(cid != VMCI_INVALID_ID);
      sendInfo.result = VMCIDatagram_Dispatch(cid, dg);
      VMCI_FreeKernelMem(dg, sendInfo.len);
      retval = copy_to_user((void *)ioarg, &sendInfo, sizeof sendInfo);
      break;
   }

   case IOCTL_VMCI_DATAGRAM_RECEIVE: {
      VMCIDatagramSendRecvInfo recvInfo;
      VMCIDatagram *dg = NULL;

      if (vmciLinux->ctType != VMCIOBJ_DATAGRAM_PROCESS &&
	  vmciLinux->ctType != VMCIOBJ_CONTEXT) {
         Warning("VMCI: Ioctl %d only valid for context and process datagram "
		 "handle.\n", iocmd);
         retval = -EINVAL;
         break;
      }

      retval = copy_from_user(&recvInfo, (void *) ioarg, sizeof recvInfo);
      if (retval) {
         Warning("VMCI: copy_from_user failed.\n");
         retval = -EFAULT;
         break;
      }

      if (vmciLinux->ctType == VMCIOBJ_CONTEXT) {
	 size_t size = recvInfo.len;
	 ASSERT(vmciLinux->ct.context);
	 recvInfo.result = VMCIContext_DequeueDatagram(vmciLinux->ct.context,
						       &size, &dg);
      } else {
	 ASSERT(vmciLinux->ctType == VMCIOBJ_DATAGRAM_PROCESS);
	 ASSERT(vmciLinux->ct.dgmProc);
	 recvInfo.result = VMCIDatagramProcess_ReadCall(vmciLinux->ct.dgmProc, 
							recvInfo.len, &dg);
      }	 
      if (recvInfo.result >= VMCI_SUCCESS) {
	 ASSERT(dg);
	 retval = copy_to_user((void *) ((uintptr_t) recvInfo.addr), dg,
			       VMCI_DG_SIZE(dg));
	 VMCI_FreeKernelMem(dg, VMCI_DG_SIZE(dg));
	 if (retval != 0) {
	    break;
	 }
      }
      retval = copy_to_user((void *)ioarg, &recvInfo, sizeof recvInfo);
      break;
   }

   case IOCTL_VMCI_QUEUEPAIR_ALLOC: {
      VMCIQueuePairAllocInfo queuePairAllocInfo;
      VMCIQueuePairAllocInfo *info = (VMCIQueuePairAllocInfo *)ioarg;
      int32 result;
      VMCIId cid;

      if (vmciLinux->ctType != VMCIOBJ_CONTEXT) {
         Log("VMCI: IOCTL_VMCI_QUEUEPAIR_ALLOC only valid for contexts.\n");
         retval = -EINVAL;
         break;
      }

      retval = copy_from_user(&queuePairAllocInfo, (void *)ioarg,
                              sizeof queuePairAllocInfo);
      if (retval) {
         retval = -EFAULT;
         break;
      }

      cid = VMCIContext_GetId(vmciLinux->ct.context);
      QueuePairList_Lock();

      {
	 QueuePairPageStore pageStore = { TRUE,
					  queuePairAllocInfo.producePageFile,
					  queuePairAllocInfo.consumePageFile,
					  queuePairAllocInfo.producePageFileSize,
					  queuePairAllocInfo.consumePageFileSize,
					  0,
					  0 };

	 result = QueuePair_Alloc(queuePairAllocInfo.handle,
	                          queuePairAllocInfo.peer,
				  queuePairAllocInfo.flags,
                                  VMCI_NO_PRIVILEGE_FLAGS,
				  queuePairAllocInfo.produceSize,
				  queuePairAllocInfo.consumeSize,
				  &pageStore,
				  vmciLinux->ct.context);
      }
      Log("VMCI: IOCTL_VMCI_QUEUEPAIR_ALLOC cid = %u result = %d.\n", cid,
          result);
      retval = copy_to_user(&info->result, &result, sizeof result);
      if (retval) {
         retval = -EFAULT;
         if (result >= VMCI_SUCCESS) {
            result = QueuePair_Detach(queuePairAllocInfo.handle,
                                      vmciLinux->ct.context, TRUE);
            ASSERT(result >= VMCI_SUCCESS);
         }
      }

      QueuePairList_Unlock();
      break;
   }

   case IOCTL_VMCI_QUEUEPAIR_SETPAGEFILE: {
      VMCIQueuePairPageFileInfo pageFileInfo;
      VMCIQueuePairPageFileInfo *info = (VMCIQueuePairPageFileInfo *)ioarg;
      int32 result;
      VMCIId cid;
      Bool useUVA;
      int size;

      if (vmciLinux->ctType != VMCIOBJ_CONTEXT) {
         Log("VMCI: IOCTL_VMCI_QUEUEPAIR_SETPAGEFILE only valid for "
             "contexts.\n");
         retval = -EINVAL;
         break;
      }

      if (VMCIContext_SupportsHostQP(vmciLinux->ct.context)) {
         useUVA = TRUE;
         size = sizeof *info;
      } else {
         /*
          * An older VMX version won't supply the UVA of the page
          * files backing the queue pair contents (and headers)
          */

         useUVA = FALSE;
         size = sizeof(VMCIQueuePairPageFileInfo_NoHostQP);
      }

      retval = copy_from_user(&pageFileInfo, (void *)ioarg, size);
      if (retval) {
         retval = -EFAULT;
         break;
      }

      /*
       * Communicate success pre-emptively to the caller.  Note that
       * the basic premise is that it is incumbent upon the caller not
       * to look at the info.result field until after the ioctl()
       * returns.  And then, only if the ioctl() result indicates no
       * error.  We send up the SUCCESS status before calling
       * SetPageStore() store because failing to copy up the result
       * code means unwinding the SetPageStore().
       *
       * It turns out the logic to unwind a SetPageStore() opens a can
       * of worms.  For example, if a host had created the QueuePair
       * and a guest attaches and SetPageStore() is successful but
       * writing success fails, then ... the host has to be stopped
       * from writing (anymore) data into the QueuePair.  That means
       * an additional test in the VMCI_Enqueue() code path.  Ugh.
       */

      result = VMCI_SUCCESS;
      retval = copy_to_user(&info->result, &result, sizeof result);
      if (retval == 0) {
         cid = VMCIContext_GetId(vmciLinux->ct.context);
         QueuePairList_Lock();

         {
            QueuePairPageStore pageStore = { TRUE,
                                             pageFileInfo.producePageFile,
                                             pageFileInfo.consumePageFile,
                                             pageFileInfo.producePageFileSize,
                                             pageFileInfo.consumePageFileSize,
                                             useUVA ? pageFileInfo.produceVA : 0,
                                             useUVA ? pageFileInfo.consumeVA : 0 };

            result = QueuePair_SetPageStore(pageFileInfo.handle,
                                            &pageStore,
                                            vmciLinux->ct.context);
         }
         QueuePairList_Unlock();

         if (result < VMCI_SUCCESS) {
            Log("VMCI: IOCTL_VMCI_QUEUEPAIR_SETPAGEFILE cid = %u result = %d.\n",
                cid, result);

            retval = copy_to_user(&info->result, &result, sizeof result);
            if (retval != 0) {
               /*
                * Note that in this case the SetPageStore() call
                * failed but we were unable to communicate that to the
                * caller (because the copy_to_user() call failed).
                * So, if we simply return an error (in this case
                * -EFAULT) then the caller will know that the
                * SetPageStore failed even though we couldn't put the
                * result code in the result field and indicate exactly
                * why it failed.
                *
                * That says nothing about the issue where we were once
                * able to write to the caller's info memory and now
                * can't.  Something more serious is probably going on
                * than the fact that SetPageStore() didn't work.
                */
               retval = -EFAULT;
            }
         }

      } else {
         /*
          * In this case, we can't write a result field of the
          * caller's info block.  So, we don't even try to
          * SetPageStore().
          */
         retval = -EFAULT;
      }

      break;
   }

   case IOCTL_VMCI_QUEUEPAIR_DETACH: {
      VMCIQueuePairDetachInfo detachInfo;
      VMCIQueuePairDetachInfo *info = (VMCIQueuePairDetachInfo *)ioarg;
      int32 result;
      VMCIId cid;

      if (vmciLinux->ctType != VMCIOBJ_CONTEXT) {
         Log("VMCI: IOCTL_VMCI_QUEUEPAIR_DETACH only valid for contexts.\n");
         retval = -EINVAL;
         break;
      }

      retval = copy_from_user(&detachInfo, (void *)ioarg, sizeof detachInfo);
      if (retval) {
         retval = -EFAULT;
         break;
      }

      cid = VMCIContext_GetId(vmciLinux->ct.context);
      QueuePairList_Lock();
      result = QueuePair_Detach(detachInfo.handle, vmciLinux->ct.context,
                                FALSE); /* Probe detach operation. */
      Log("VMCI: IOCTL_VMCI_QUEUEPAIR_DETACH cid = %u result = %d.\n",
          cid, result);
      
      retval = copy_to_user(&info->result, &result, sizeof result);
      if (retval) {
         /* Could not copy to userland, don't perform the actual detach. */
         retval = -EFAULT;
      } else {
         if (result >= VMCI_SUCCESS) {
            /* Now perform the actual detach. */
            int32 result2 = QueuePair_Detach(detachInfo.handle,
                                             vmciLinux->ct.context, TRUE);
            if (UNLIKELY(result != result2)) {
               /*
                * This should never happen.  But it's better to log a warning
                * than to crash the host.
                */
               Warning("QueuePair_Detach returned different results:  "
                       "previous = %d, current = %d.\n", result, result2);
            }
         }
      }

      QueuePairList_Unlock();
      break;
   }

   case IOCTL_VMCI_DATAGRAM_REQUEST_MAP: {
      VMCIDatagramMapInfo mapInfo;
      VMCIDatagramMapInfo *info = (VMCIDatagramMapInfo *)ioarg;
      int32 result;
      VMCIId cid;

      if (vmciLinux->ctType != VMCIOBJ_CONTEXT) {
         Log("VMCI: IOCTL_VMCI_REQUEST_MAP only valid for contexts.\n");
         retval = -EINVAL;
         break;
      }

      retval = copy_from_user(&mapInfo, (void *)ioarg, sizeof mapInfo);
      if (retval) {
         retval = -EFAULT;
         break;
      }

      cid = VMCIContext_GetId(vmciLinux->ct.context);
      result = VMCIDatagramRequestWellKnownMap(mapInfo.wellKnownID, cid,
					       VMCIContext_GetPrivFlagsInt(cid));
      retval = copy_to_user(&info->result, &result, sizeof result);
      if (retval) {
         retval = -EFAULT;
         break;
      }
      break;
   }

   case IOCTL_VMCI_DATAGRAM_REMOVE_MAP: {
      VMCIDatagramMapInfo mapInfo;
      VMCIDatagramMapInfo *info = (VMCIDatagramMapInfo *)ioarg;
      int32 result;
      VMCIId cid;

      if (vmciLinux->ctType != VMCIOBJ_CONTEXT) {
         Log("VMCI: IOCTL_VMCI_REMOVE_MAP only valid for contexts.\n");
         retval = -EINVAL;
         break;
      }

      retval = copy_from_user(&mapInfo, (void *)ioarg, sizeof mapInfo);
      if (retval) {
         retval = -EFAULT;
         break;
      }

      cid = VMCIContext_GetId(vmciLinux->ct.context);
      result = VMCIDatagramRemoveWellKnownMap(mapInfo.wellKnownID, cid);
      retval = copy_to_user(&info->result, &result, sizeof result);
      if (retval) {
         retval = -EFAULT;
         break;
      }
      break;
   }

   case IOCTL_VMCI_CTX_ADD_NOTIFICATION: {
      VMCINotifyAddRemoveInfo arInfo;
      VMCINotifyAddRemoveInfo *info = (VMCINotifyAddRemoveInfo *)ioarg;
      int32 result;
      VMCIId cid;

      if (vmciLinux->ctType != VMCIOBJ_CONTEXT) {
         Log("VMCI: IOCTL_VMCI_CTX_ADD_NOTIFICATION only valid for contexts.\n");
         retval = -EINVAL;
         break;
      }

      retval = copy_from_user(&arInfo, (void *)ioarg, sizeof arInfo);
      if (retval) {
         retval = -EFAULT;
         break;
      }

      cid = VMCIContext_GetId(vmciLinux->ct.context);
      result = VMCIContext_AddNotification(cid, arInfo.remoteCID);
      retval = copy_to_user(&info->result, &result, sizeof result);
      if (retval) {
         retval = -EFAULT;
         break;
      }
      break;
   }

   case IOCTL_VMCI_CTX_REMOVE_NOTIFICATION: {
      VMCINotifyAddRemoveInfo arInfo;
      VMCINotifyAddRemoveInfo *info = (VMCINotifyAddRemoveInfo *)ioarg;
      int32 result;
      VMCIId cid;

      if (vmciLinux->ctType != VMCIOBJ_CONTEXT) {
         Log("VMCI: IOCTL_VMCI_CTX_REMOVE_NOTIFICATION only valid for "
	     "contexts.\n");
         retval = -EINVAL;
         break;
      }

      retval = copy_from_user(&arInfo, (void *)ioarg, sizeof arInfo);
      if (retval) {
         retval = -EFAULT;
         break;
      }

      cid = VMCIContext_GetId(vmciLinux->ct.context);
      result = VMCIContext_RemoveNotification(cid, arInfo.remoteCID);
      retval = copy_to_user(&info->result, &result, sizeof result);
      if (retval) {
         retval = -EFAULT;
         break;
      }
      break;
   }

   case IOCTL_VMCI_CTX_GET_CPT_STATE: {
      VMCICptBufInfo getInfo;
      VMCIId cid;
      char *cptBuf;

      if (vmciLinux->ctType != VMCIOBJ_CONTEXT) {
         Log("VMCI: IOCTL_VMCI_CTX_GET_CPT_STATE only valid for "
	     "contexts.\n");
         retval = -EINVAL;
         break;
      }

      retval = copy_from_user(&getInfo, (void *)ioarg, sizeof getInfo);
      if (retval) {
         retval = -EFAULT;
         break;
      }

      cid = VMCIContext_GetId(vmciLinux->ct.context);
      getInfo.result = VMCIContext_GetCheckpointState(cid, getInfo.cptType,
						      &getInfo.bufSize,
						      &cptBuf);
      if (getInfo.result == VMCI_SUCCESS && getInfo.bufSize) {
	 retval = copy_to_user((void *)(VA)getInfo.cptBuf, cptBuf,
			       getInfo.bufSize);
	 VMCI_FreeKernelMem(cptBuf, getInfo.bufSize);
	 if (retval) {
	    retval = -EFAULT;
	    break;
	 }
      }
      retval = copy_to_user((void *)ioarg, &getInfo, sizeof getInfo);
      if (retval) {
         retval = -EFAULT;
         break;
      }
      break;
   }

   case IOCTL_VMCI_CTX_SET_CPT_STATE: {
      VMCICptBufInfo setInfo;
      VMCIId cid;
      char *cptBuf;

      if (vmciLinux->ctType != VMCIOBJ_CONTEXT) {
         Log("VMCI: IOCTL_VMCI_CTX_SET_CPT_STATE only valid for "
	     "contexts.\n");
         retval = -EINVAL;
         break;
      }

      retval = copy_from_user(&setInfo, (void *)ioarg, sizeof setInfo);
      if (retval) {
         retval = -EFAULT;
         break;
      }

      cptBuf = VMCI_AllocKernelMem(setInfo.bufSize, VMCI_MEMORY_NORMAL);
      if (cptBuf == NULL) {
         Log("VMCI: Cannot allocate memory to set cpt state of type %d.\n",
	     setInfo.cptType);
         retval = -ENOMEM;
         break;
      }
      retval = copy_from_user(cptBuf, (void *)(VA)setInfo.cptBuf,
			      setInfo.bufSize);
      if (retval) {
	 VMCI_FreeKernelMem(cptBuf, setInfo.bufSize);
         retval = -EFAULT;
         break;
      }

      cid = VMCIContext_GetId(vmciLinux->ct.context);
      setInfo.result = VMCIContext_SetCheckpointState(cid, setInfo.cptType,
						      setInfo.bufSize, cptBuf);
      VMCI_FreeKernelMem(cptBuf, setInfo.bufSize);
      retval = copy_to_user((void *)ioarg, &setInfo, sizeof setInfo);
      if (retval) {
         retval = -EFAULT;
         break;
      }
      break;
   }

   case IOCTL_VMCI_GET_CONTEXT_ID: {
      VMCIId cid = VMCI_HOST_CONTEXT_ID;

      retval = copy_to_user((void *)ioarg, &cid, sizeof cid);
      break;
   }

   case IOCTL_VMCI_SET_NOTIFY: {
      VMCISetNotifyInfo notifyInfo;

      if (vmciLinux->ctType != VMCIOBJ_CONTEXT) {
         Log("VMCI: IOCTL_VMCI_SET_NOTIFY only valid for contexts.\n");
         retval = -EINVAL;
         break;
      }

      retval = copy_from_user(&notifyInfo, (void *)ioarg, sizeof notifyInfo);
      if (retval) {
         retval = -EFAULT;
         break;
      }

      notifyInfo.result = VMCISetupNotify(vmciLinux->ct.context,
                                          (VA)notifyInfo.notifyUVA);

      retval = copy_to_user((void *)ioarg, &notifyInfo, sizeof notifyInfo);
      if (retval) {
         retval = -EFAULT;
         break;
      }

      break;
   }

   default:
      Warning("Unknown ioctl %d\n", iocmd);
      retval = -EINVAL;
   }

   return retval;
}


#if defined(HAVE_COMPAT_IOCTL) || defined(HAVE_UNLOCKED_IOCTL)
/*
 *-----------------------------------------------------------------------------
 *
 * LinuxDriver_UnlockedIoctl --
 *
 *      Wrapper for LinuxDriver_Ioctl supporting the compat_ioctl and
 *      unlocked_ioctl methods that have signatures different from the
 *      old ioctl. Used as compat_ioctl method for 32bit apps running
 *      on 64bit kernel and for unlocked_ioctl on systems supporting
 *      those.  LinuxDriver_Ioctl may safely be called without holding
 *      the BKL.
 *
 * Results:
 *      Same as LinuxDriver_Ioctl.
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
   return LinuxDriver_Ioctl(NULL, filp, iocmd, ioarg);
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIUserVAInvalidPointer --
 *
 *      Checks if a given user VA is valid or not.  Copied from
 *      bora/modules/vmnet/linux/hostif.c:VNetUserIfInvalidPointer().  TODO
 *      libify the common code.
 *
 * Results:
 *      TRUE iff invalid.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE Bool
VMCIUserVAInvalidPointer(VA uva,      // IN:
                         size_t size) // IN:
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
   return !access_ok(VERIFY_WRITE, (void *)uva, size);
#else
   return verify_area(VERIFY_READ, (void *)uva, size) ||
          verify_area(VERIFY_WRITE, (void *)uva, size);
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIUserVALockPage --
 *
 *      Lock physical page backing a given user VA.  Copied from
 *      bora/modules/vmnet/linux/userif.c:UserIfLockPage().  TODO libify the
 *      common code.
 *
 * Results:
 *      Pointer to struct page on success, NULL otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE struct page *
VMCIUserVALockPage(VA addr) // IN:
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 19)
   struct page *page = NULL;
   int retval;

   down_read(&current->mm->mmap_sem);
   retval = get_user_pages(current, current->mm, addr,
                           1, 1, 0, &page, NULL);
   up_read(&current->mm->mmap_sem);

   if (retval != 1) {
      return NULL;
   }

   return page;
#else
   struct page *page;
   struct page *check;
   volatile int c;

   /*
    * Establish a virtual to physical mapping by touching the physical
    * page. Because the address is valid, there is no need to check the return
    * value here --hpreg
    */
   compat_get_user(c, (char *)addr);

   page = PgtblVa2Page(addr);
   if (page == NULL) {
      /* The mapping went away --hpreg */
      return NULL;
   }

   /* Lock the physical page --hpreg */
   get_page(page);

   check = PgtblVa2Page(addr);
   if (check != page) {
      /*
       * The mapping went away or was modified, so we didn't lock the right
       * physical page --hpreg
       */

      /* Unlock the physical page --hpreg */
      put_page(page);

      return NULL;
   }

   /* We locked the right physical page --hpreg */
   return page;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIMapBoolPtr --
 *
 *      Lock physical page backing a given user VA and maps it to kernel
 *      address space.  The range of the mapped memory should be within a
 *      single page otherwise an error is returned.  Copied from
 *      bora/modules/vmnet/linux/userif.c:VNetUserIfMapUint32Ptr().  TODO
 *      libify the common code.
 *
 * Results:
 *      0 on success, negative error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE int
VMCIMapBoolPtr(VA notifyUVA,     // IN:
               struct page **p,  // OUT:
               Bool **notifyPtr) // OUT:
{
   if (VMCIUserVAInvalidPointer(notifyUVA, sizeof **notifyPtr) ||
       (((notifyUVA + sizeof **notifyPtr - 1) & ~(PAGE_SIZE - 1)) !=
        (notifyUVA & ~(PAGE_SIZE - 1)))) {
      return -EINVAL;
   }

   *p = VMCIUserVALockPage(notifyUVA);
   if (*p == NULL) {
      return -EAGAIN;
   }

   *notifyPtr = (Bool *)((uint8 *)kmap(*p) + (notifyUVA & (PAGE_SIZE - 1)));
   return 0;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCISetupNotify --
 *
 *      Sets up a given context for notify to work.  Calls VMCIMapBoolPtr()
 *      which maps the notify boolean in user VA in kernel space.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static int
VMCISetupNotify(VMCIContext *context, // IN:
                VA notifyUVA)         // IN:
{
   int retval;

   if (context->notify) {
      Warning("VMCI:  Notify mechanism is already set up.\n");
      return VMCI_ERROR_DUPLICATE_ENTRY;
   }

   retval =
      VMCIMapBoolPtr(notifyUVA, &context->notifyPage, &context->notify) == 0 ?
         VMCI_SUCCESS : VMCI_ERROR_GENERIC;
   if (retval == VMCI_SUCCESS) {
      VMCIContext_CheckAndSignalNotify(context);
   }

   return retval;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIUnsetNotify --
 *
 *      Reverts actions set up by VMCISetupNotify().  Unmaps and unlocks the
 *      page mapped/locked by VMCISetupNotify().
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
VMCIUnsetNotify(VMCIContext *context) // IN:
{
   if (context->notifyPage) {
      kunmap(context->notifyPage);
      put_page(context->notifyPage);
      context->notify = NULL;
      context->notifyPage = NULL;
   }
}


MODULE_AUTHOR("VMware, Inc.");
MODULE_DESCRIPTION("VMware Virtual Machine Communication Interface (VMCI).");
MODULE_LICENSE("GPL v2");
/*
 * Starting with SLE10sp2, Novell requires that IHVs sign a support agreement
 * with them and mark their kernel modules as externally supported via a
 * change to the module header. If this isn't done, the module will not load
 * by default (i.e., neither mkinitrd nor modprobe will accept it).
 */
MODULE_INFO(supported, "external");
