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

#include "driver-config.h"

#define EXPORT_SYMTAB

#include <linux/kernel.h>
#include "compat_module.h"
#include <linux/version.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/poll.h>

#include <linux/smp.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/mm.h>
#include "compat_skbuff.h"
#include <linux/if_ether.h>
#include <linux/sockios.h>
#include "compat_sock.h"

#define __KERNEL_SYSCALLS__
#include <asm/io.h>

#include <linux/proc_fs.h>
#include <linux/file.h>
#if defined(__x86_64__) && !defined(HAVE_COMPAT_IOCTL)
#include <asm/ioctl32.h>
#endif

#include "vnetInt.h"
#include "vnetFilter.h"

#include "compat_uaccess.h"
#include "compat_kdev_t.h"
#include "compat_sched.h"
#include "compat_mutex.h"
#include "vmnetInt.h"

/*
 * Initialization and creation routines from other files.
 * Putting them here reduces the need for so many header files.
 */

extern int VNetUserIf_Create(VNetPort **ret);
extern int VNetNetIf_Create(char *devName, VNetPort **ret, int hubNum);
extern int VNetBridge_Create(char *devName, uint32 flags, VNetJack *hubJack,
                             VNetPort **ret);
extern int VNetUserListener_Create(uint32 classMask, VNetJack *hubJack, VNetPort **ret);

#ifdef CONFIG_NETFILTER
/*
 * Filter routine from filter.c
 */
extern int VNetFilter_HandleUserCall(VNet_RuleHeader *ruleHeader, unsigned long ioarg);
extern void VNetFilter_Shutdown(void);
#endif

/*
 *  Structure for cycle detection of host interfaces.  This
 *  struct is only used by VNetCycleDetectIf().
 */

typedef struct VNetInterface {
   char           name[VNET_NAME_LEN];
   int            myGeneration;
   struct VNetInterface *next;
} VNetInterface;

static VNetInterface *vnetInterfaces = NULL;

/* this will let all multicast packets go through. */
const uint8 allMultiFilter[VNET_LADRF_LEN] = { 0xff, 0xff, 0xff, 0xff,
					       0xff, 0xff, 0xff, 0xff };

/* broadcast MAC */
const uint8 broadcast[ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/*
 * All jack->peer accesses are guarded by this lock.
 *
 * This lock is acquired for read from interrupt context:
 * use write_lock_irqsave() to gain write access.
 *
 * If you are acquiring this lock for write, and you do
 * not have vnetStructureMutex already acquired,
 * it is most certainly a bug.
 */
static DEFINE_RWLOCK(vnetPeerLock);

/*
 * All concurrent changes to the network structure are
 * guarded by this mutex.
 *
 * For change to peer field you must own both
 * vnetStructureMutex and vnetPeerLock for write.
 */
compat_define_mutex(vnetStructureMutex);
compat_define_mutex(vnetMutex);

#if defined(VM_X86_64) && !defined(HAVE_COMPAT_IOCTL)
/*
 * List of ioctl commands we translate 1:1 between 32bit
 * userspace and 64bit kernel.
 *
 * Whole range <VNET_FIRST_CMD, VNET_LAST_CMD> is translated
 * 1:1 in addition to the commands listed below.
 */
static const unsigned int ioctl32_cmds[] = {
	SIOCGBRSTATUS, SIOCSPEER, SIOCSPEER2, SIOCSBIND, SIOCGETAPIVERSION2,
        SIOCSFILTERRULES, SIOCSUSERLISTENER, SIOCSPEER3, 0,
};
#endif

/*
 * List of known ports. Use vnetStructureMutex for locking.
 */

static VNetPort *vnetAllPorts     = NULL;


#ifdef VMW_HAVE_SK_ALLOC_WITH_PROTO
struct proto vmnet_proto = {
   .name     = "VMNET",
   .owner    = THIS_MODULE,
   .obj_size = sizeof(struct sock),
};
#endif

/*
 *  Device driver interface.
 */

int VNetRegister(int value);
static int  VNetFileOpOpen(struct inode *inode, struct file *filp);
static int  VNetFileOpClose(struct inode *inode, struct file *filp);
static unsigned int VNetFileOpPoll(struct file *filp, poll_table *wait);
static ssize_t  VNetFileOpRead(struct file *filp, char *buf, size_t count,
			       loff_t *ppos);
static ssize_t  VNetFileOpWrite(struct file *filp, const char *buf, size_t count,
			        loff_t *ppos);
static int  VNetFileOpIoctl(struct inode *inode, struct file *filp,
                            unsigned int iocmd, unsigned long ioarg);
#if defined(HAVE_UNLOCKED_IOCTL) || defined(HAVE_COPAT_IOCTL)
static long  VNetFileOpUnlockedIoctl(struct file * filp,
                                     unsigned int iocmd, unsigned long ioarg);
#endif

static struct file_operations vnetFileOps;

/*
 * Utility functions
 */

static void VNetFreeInterfaceList(void);
static int VNetSwitchToDifferentPeer(VNetJack *jack, VNetJack *newPeer,
				     Bool connectNewToPeer,
				     struct file *filp, VNetPort *jackPort,
				     VNetPort *newPeerPort);

/*
 *----------------------------------------------------------------------
 *
 * VNetRegister --
 *
 *      (debugging support) Should be the first function of this file
 *
 * Results:
 *
 *      Registers the module.
 *      /sbin/ksyms -a | grep VNetRegister will return the base
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

int
VNetRegister(int value) // IN: unused
{
   LOG(0, (KERN_WARNING "/dev/vmnet: VNetRegister called\n"));
   return 0;
}

#ifdef VMW_HAVE_SK_ALLOC_WITH_PROTO

/*
 *----------------------------------------------------------------------
 *
 *  VNetProtoRegister --
 *  VNetProtoUnregister --
 *
 *      Register or unregister the struct proto that we use for sk_alloc.
 *
 * Results:
 *      int.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

#define VNetProtoRegister()   proto_register(&vmnet_proto, 0)
#define VNetProtoUnregister() \
   do { \
      proto_unregister(&vmnet_proto); \
   } while (0)

#else

#define VNetProtoRegister()     0
#define VNetProtoUnregister()

#endif

#if defined(VM_X86_64) && !defined(HAVE_COMPAT_IOCTL)
/*
 *----------------------------------------------------------------------
 *
 *  LinuxDriver_Ioctl32_Handler --
 *
 *      Wrapper for allowing 64-bit driver to handle ioctls()
 *      from 32-bit applications.
 *
 * Results:
 *
 *      -ENOTTY, or result of call to VNetFileOpIoctl().
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
LinuxDriver_Ioctl32_Handler(unsigned int fd,     // IN: (unused)
			    unsigned int iocmd,  // IN:
                            unsigned long ioarg, // IN:
			    struct file * filp)  // IN:
{
   int ret = -ENOTTY;
   compat_mutex_lock(&vnetMutex);
   if (filp && filp->f_op && filp->f_op->ioctl == VNetFileOpIoctl) {
      ret = VNetFileOpIoctl(filp->f_dentry->d_inode, filp, iocmd, ioarg);
   }
   compat_mutex_unlock(&vnetMutex);
   return ret;
}


/*
 *----------------------------------------------------------------------
 *
 *  register_ioctl32_handlers --
 *
 *      Registers LinuxDriver_Ioctl32_Handler as the wrapper for
 *      allowing 64-bit driver to handle ioctls()
 *      from 32-bit applications.
 *
 *      Does nothing on non-64bit systems.
 *
 * Results:
 *
 *      errno (0 on success)
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
register_ioctl32_handlers(void)
{
   int i;
   int retval;

   for (i = VNET_FIRST_CMD; i <= VNET_LAST_CMD; i++) {
      retval = register_ioctl32_conversion(i, LinuxDriver_Ioctl32_Handler);
      if (retval) {
	 int j;
         LOG(0, (KERN_WARNING "Fail to register ioctl32 conversion for cmd %d\n", i));
	 for (j = VNET_FIRST_CMD; j < i; ++j) {
	    unregister_ioctl32_conversion(j);
	 }
         return retval;
      }
   }
   for (i = 0; ioctl32_cmds[i]; i++) {
      retval = register_ioctl32_conversion(ioctl32_cmds[i], LinuxDriver_Ioctl32_Handler);
      if (retval) {
	 int j;
         LOG(0, (KERN_WARNING "Fail to register ioctl32 conversion for cmd %08X\n",
	         ioctl32_cmds[i]));
	 for (j = VNET_FIRST_CMD; j < VNET_LAST_CMD; ++j) {
	    unregister_ioctl32_conversion(j);
	 }
	 for (j = 0; j < i; ++j) {
	    unregister_ioctl32_conversion(ioctl32_cmds[j]);
	 }
         return retval;
      }
   }
   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 *  unregister_ioctl32_handlers --
 *
 *      Unregisters the wrappers we specified for
 *      allowing a 64-bit driver to handle ioctls()
 *      from 32-bit applications.
 *
 *      Does nothing on non-64bit systems.
 *
 * Results:
 *
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
unregister_ioctl32_handlers(void)
{
   int i;
   int retval;

   for (i = VNET_FIRST_CMD; i <= VNET_LAST_CMD; i++) {
      retval = unregister_ioctl32_conversion(i);
      if (retval) {
         LOG(0, (KERN_WARNING "Fail to unregister ioctl32 conversion for cmd %d\n", i));
      }
   }
   for (i = 0; ioctl32_cmds[i]; i++) {
      retval = unregister_ioctl32_conversion(ioctl32_cmds[i]);
      if (retval) {
         LOG(0, (KERN_WARNING "Fail to unregister ioctl32 conversion for cmd %08X\n",
		 ioctl32_cmds[i]));
      }
   }
}
#else
#define register_ioctl32_handlers() (0)
#define unregister_ioctl32_handlers() do { } while (0)
#endif


/*
 *----------------------------------------------------------------------
 *
 * VNetAddPortToList --
 *
 *      Add port to list of known ports.
 *	Caller must own vnetStructureMutex.
 *
 * Results:
 *
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE void
VNetAddPortToList(VNetPort *port) // IN: port to add to list
{
   port->next = vnetAllPorts;
   vnetAllPorts = port;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetRemovePortFromList --
 *
 *      Remove port from list of known ports.
 *	Caller must own vnetStructureMutex.
 *
 * Results:
 *
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE void
VNetRemovePortFromList(const VNetPort *port) // IN: port to remove from list
{
   VNetPort **p;

   for (p = &vnetAllPorts; *p; p = &(*p)->next) {
      if (*p == port) {
         *p = port->next;
         break;
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * init_module --
 *
 *      linux module entry point. Called by /sbin/insmod command.
 *      Initializes module and Registers this driver for a
 *      vnet major #.  The 64-bit version of this driver also
 *      registers handlers for 32-bit applications.
 *
 * Results:
 *      errno (0 on success).
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
init_module(void)
{
   int retval;

   /*
    * First initialize everything, and as a last step register
    * vmnet device: immediately after registration anybody can
    * ask driver for anything.
    */

   retval = VNetProc_Init();
   if (retval) {
      LOG(0, (KERN_NOTICE "/dev/vmnet: could not register proc fs\n"));
      return -ENOENT;
   }

   retval = VNetProtoRegister();
   if (retval) {
      goto err_proto;
   }

   /*
    * Initialize the file_operations structure. Because this code is always
    * compiled as a module, this is fine to do it here and not in a static
    * initializer.
    */

   memset(&vnetFileOps, 0, sizeof vnetFileOps);
   vnetFileOps.owner = THIS_MODULE;
   vnetFileOps.read = VNetFileOpRead;
   vnetFileOps.write = VNetFileOpWrite;
   vnetFileOps.poll = VNetFileOpPoll;
#ifdef HAVE_UNLOCKED_IOCTL
   vnetFileOps.unlocked_ioctl = VNetFileOpUnlockedIoctl;
#else
   vnetFileOps.ioctl = VNetFileOpIoctl;
#endif
#ifdef HAVE_COMPAT_IOCTL
   vnetFileOps.compat_ioctl = VNetFileOpUnlockedIoctl;
#endif
   vnetFileOps.open = VNetFileOpOpen;
   vnetFileOps.release = VNetFileOpClose;

   retval = register_chrdev(VNET_MAJOR_NUMBER, "vmnet", &vnetFileOps);
   if (retval) {
      LOG(0, (KERN_NOTICE "/dev/vmnet: could not register major device %d\n",
	      VNET_MAJOR_NUMBER));
      goto err_chrdev;
   }

   retval = register_ioctl32_handlers();
   if (retval) {
      goto err_ioctl;
   }

   return 0;

err_ioctl:
   unregister_chrdev(VNET_MAJOR_NUMBER, "vmnet");
err_chrdev:
   VNetProtoUnregister();
err_proto:
   VNetProc_Cleanup();
   return retval;
}


/*
 *----------------------------------------------------------------------
 *
 * cleanup_module --
 *
 *      Called by /sbin/rmmod.  Unregisters this driver for a
 *      vnet major #, and deinitializes the modules.  The 64-bit
 *      version of this driver also unregisters the handlers
 *      for 32-bit applications.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
cleanup_module(void)
{
   unregister_ioctl32_handlers();
   unregister_chrdev(VNET_MAJOR_NUMBER, "vmnet");
   VNetProtoUnregister();
   VNetProc_Cleanup();
#ifdef CONFIG_NETFILTER
   VNetFilter_Shutdown();
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * VNetFileOpOpen --
 *
 *      The virtual network's open file operation.  Connects to (and
 *      potentially allocates) a hub, then opens a connection to
 *      this virtual network (i.e., plugs a cable into the virtual
 *      hub).
 *
 * Results:
 *      Errno.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VNetFileOpOpen(struct inode  *inode, // IN: used to get hub number
               struct file   *filp)  // IN: filp
{
   VNetPort *port;
   VNetJack *hubJack;
   int hubNum;
   int retval;

   LOG(1, (KERN_DEBUG "/dev/vmnet: open called by PID %d (%s)\n",
           current->pid, current->comm));

   /*
    * Sanity check the hub number.
    */

   hubNum = minor(inode->i_rdev);
   if (hubNum < 0 || hubNum >= VNET_NUM_VNETS) {
      return -ENODEV;
   }

   /*
    * Allocate port
    */

   retval = VNetUserIf_Create(&port);
   if (retval) {
      return -retval;
   }

   /*
    * Allocate and connect to hub.
    */

   hubJack = VNetHub_AllocVnet(hubNum);
   if (!hubJack) {
      VNetFree(&port->jack);
      return -EBUSY;
   }

   compat_mutex_lock(&vnetStructureMutex);
   retval = VNetConnect(&port->jack, hubJack);
   if (retval) {
      compat_mutex_unlock(&vnetStructureMutex);
      VNetFree(&port->jack);
      VNetFree(hubJack);
      return retval;
   }

   VNetAddPortToList(port);
   compat_mutex_unlock(&vnetStructureMutex);

   /*
    * Store away jack in file pointer private field for later use.
    */

   filp->private_data = port;

   LOG(1, (KERN_DEBUG "/dev/vmnet: port on hub %d successfully opened\n", hubNum));

   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetFileOpClose --
 *
 *      The virtual network's close file operation.  Disconnects
 *      from the virtual hub (i.e., unplugs the cable).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
VNetFileOpClose(struct inode  *inode, // IN: (unused)
                struct file   *filp)  // IN: filp
{
   VNetPort *port = (VNetPort*)filp->private_data;
   VNetJack *peer;

   if (!port) {
      LOG(1, (KERN_DEBUG "/dev/vmnet: bad file pointer on close\n"));
      return -EBADF;
   }

   compat_mutex_lock(&vnetStructureMutex);
   peer = VNetDisconnect(&port->jack);
   VNetRemovePortFromList(port);
   compat_mutex_unlock(&vnetStructureMutex);

   VNetFree(&port->jack);
   VNetFree(peer);

   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetFileOpRead --
 *
 *      The virtual network's read file operation.
 *
 * Results:
 *      On success the len of the packet received,
 *      else if no packet waiting and nonblocking 0,
 *      else -errno.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

ssize_t
VNetFileOpRead(struct file  *filp,  // IN:
               char         *buf,   // OUT:
               size_t        count, // IN:
	       loff_t	    *ppos)  // IN: (unused)
{
   VNetPort *port = (VNetPort*)filp->private_data;

   if (!port) {
      LOG(1, (KERN_DEBUG "/dev/vmnet: bad file pointer on read\n"));
      return -EBADF;
   }

   if (!port->fileOpRead) {
      return -EPERM;
   }

   return port->fileOpRead(port, filp, buf, count);
}


/*
 *----------------------------------------------------------------------
 *
 * VNetFileOpWrite --
 *
 *      The virtual network's write file operation.
 *
 * Results:
 *      On success the count of bytes written else errno.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

ssize_t
VNetFileOpWrite(struct file   *filp,  // IN:
                const char    *buf,   // IN:
                size_t         count, // IN:
		loff_t	      *ppos)  // IN: (unused)
{
   VNetPort *port = (VNetPort*)filp->private_data;

   if (!port) {
      LOG(1, (KERN_DEBUG "/dev/vmnet: bad file pointer on write\n"));
      return -EBADF;
   }

   if (!port->fileOpWrite) {
      return -EPERM;
   }

   return port->fileOpWrite(port, filp, buf, count);
}


/*
 *----------------------------------------------------------------------
 *
 * VNetFileOpPoll --
 *
 *      The virtual network's file select operation.
 *
 * Results:
 *      Return 1 if success, else sleep and return 0.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static unsigned int
VNetFileOpPoll(struct file *filp, // IN:
	       poll_table  *wait) // IN:
{
   VNetPort *port = (VNetPort*)filp->private_data;

   if (!port) {
      LOG(1, (KERN_DEBUG "/dev/vmnet: bad file pointer on poll\n"));
      return -EBADF;
   }

   if (!port->fileOpPoll) {
      return -EPERM;
   }

   return port->fileOpPoll(port, filp, wait);
}


/*
 *----------------------------------------------------------------------
 *
 * VNetFileOpIoctl --
 *
 *      The virtual network's ioctl file operation. This is used for
 *      setup of the connection. Currently supported commands are
 *      (taken from sockios.h):
 *
 *      SIOCGIFADDR - get ethernet address          - ioarg OUT: 6 bytes
 *      SIOCSIFADDR - set ethernet address          - ioarg IN:  6 bytes
 *      SIOCSIFFLAGS - set flags                    - ioarg IN:  4 bytes
 *
 *      Private ioctl calls, taken from device-private ioctl space
 *      in sockios.h, and defined in includes/vm_oui.h:
 *
 *      SIOCSLADRF (0x89F2) - set logical address filter (for
 *         filtering multicast packets)             - ioarg IN:  8 bytes
 *
 *      SIOCGBRSTATUS - get bridging status         - ioarg OUT: 4 bytes
 *      SIOCSPEER - set bridge peer interface       - ioarg IN:  8 bytes
 *      SIOCSPEER2 - set bridge peer interface      - ioarg IN: 32 bytes
 *      SIOCSBIND - bind to a particular vnet/PVN   - ioarg IN: VNet_Bind
 *      SIOCSFILTERRULES - set host filter rules    - ioarg IN: VNet_Filter
 *      SIOCBRIDGE - (legacy see SIOCSPEER)
 *      SIOCSUSERLISTENER - set user listener - ioarg IN: VNet_SetUserListener
 *
 *      Supported flags are (taken from if.h):
 *
 *      IFF_UP - ready to receive packets             - OFF by default
 *      IFF_BROADCAST - receive broadcast packets     - OFF by default
 *      IFF_DEBUG - turn on debugging                 - OFF by default
 *      IFF_PROMISC - promiscuous mode                - OFF by default
 *      IFF_MULTICAST - receive multicast packets     - OFF by default
 *      IFF_ALLMULTI - receive all multicast packets
 *            (like IFF_PROMISC but with multicast)   - OFF by default
 *
 * Results:
 *      On success 0 else errno.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VNetFileOpIoctl(struct inode   *inode, // IN:
                struct file    *filp,  // IN:
                unsigned int    iocmd, // IN:
                unsigned long   ioarg) // IN:
{
   VNetPort *port = (VNetPort*)filp->private_data;
   VNetJack *hubJack;
   VNetPort *new;
   char name[32];
   int retval;
   VNet_SetMacAddrIOCTL macAddr;
   VNet_Bind newNetwork;
#ifdef CONFIG_NETFILTER
   VNet_RuleHeader ruleHeader;
#endif
   VNet_BridgeParams bridgeParams;

   if (!port) {
      LOG(1, (KERN_DEBUG "/dev/vmnet: bad file pointer on ioctl\n"));
      return -EBADF;
   }

   // sprintf(vnetHub[hubNum]->devName, "vmnet%d", hubNum);

   switch (iocmd) {
   case SIOCSPEER:
   case SIOCBRIDGE:
   case SIOCSPEER2:
   case SIOCSPEER3:
      memset(&bridgeParams, 0, sizeof bridgeParams);
      if (iocmd == SIOCSPEER3) {
         retval = copy_from_user(&bridgeParams, (void *)ioarg,
                                 sizeof bridgeParams);
      } else if (iocmd == SIOCSPEER2) {
         retval = copy_from_user(&bridgeParams.name, (void *)ioarg,
                                 sizeof bridgeParams.name);
      } else {
         retval = copy_from_user(&bridgeParams.name, (void *)ioarg, 8);
      }

      if (retval) {
         return -EFAULT;
      }
      NULL_TERMINATE_STRING(bridgeParams.name);

      if (!capable(CAP_NET_RAW)) {
         return -EACCES;
      }
      retval = VNetBridge_Create(bridgeParams.name, bridgeParams.flags,
                                 port->jack.peer, &new);

      return retval ? retval : VNetSwitchToDifferentPeer(&port->jack,
                                                         &new->jack, TRUE,
                                                         filp, port, new);
      break;
   case SIOCSUSERLISTENER:
      {
         VNet_SetUserListener param;

         /* copy parameters */
         if (copy_from_user(&param, (void *)ioarg, sizeof param)) {
            return -EFAULT;
         }

         /* check version */
         if (param.version != VNET_EVENT_VERSION) {
            return -EINVAL;
         }

         /* create user listener */
         retval = VNetUserListener_Create(param.classMask, port->jack.peer, &new);
         if (retval != 0) {
            return retval;
         }

         /* replace current port with user listener */
         retval = VNetSwitchToDifferentPeer(&port->jack, &new->jack, TRUE,
                                            filp, port, new);
      }
      break;
   case SIOCPORT:
      retval = VNetUserIf_Create(&new);

      return retval ? retval : VNetSwitchToDifferentPeer(&port->jack, &new->jack,
							 TRUE, filp, port, new);
      break;
   case SIOCNETIF:
      if (copy_from_user(name, (void *)ioarg, 8)) {
	 return -EFAULT;
      }
      name[8] = '\0'; /* allow 8-char unterminated string */

      retval = VNetNetIf_Create(name, &new, minor(inode->i_rdev));

      return retval ? retval : VNetSwitchToDifferentPeer(&port->jack, &new->jack,
							 TRUE, filp, port, new);
      break;

   case SIOCSBIND:
      if (copy_from_user(&newNetwork, (void *)ioarg, sizeof newNetwork)) {
	 return -EFAULT;
      }
      if (newNetwork.version != VNET_BIND_VERSION) {
	 LOG(1, (KERN_NOTICE "/dev/vmnet: bad bind version: %u %u\n",
		 newNetwork.version, VNET_BIND_VERSION));
	 return -EINVAL;
      }
      switch (newNetwork.bindType) {
      case VNET_BIND_TO_VNET:
	 if (newNetwork.number < 0 || newNetwork.number >= VNET_NUM_VNETS) {
	    LOG(1, (KERN_NOTICE "/dev/vmnet: invalid bind to vnet %d\n",
		    newNetwork.number));
	    return -EINVAL;
	 }
	 hubJack = VNetHub_AllocVnet(newNetwork.number);
	 break;
      case VNET_BIND_TO_PVN:
	 {
	    uint8 id[VNET_PVN_ID_LEN] = {0};

	    if (memcmp(id, newNetwork.id, sizeof id < sizeof newNetwork.id ?
		       sizeof id : sizeof newNetwork.id) == 0) {
	       LOG(0, (KERN_NOTICE "/dev/vmnet: invalid bind to pvn\n"));
	       return -EINVAL;
	    }
	    memcpy(id, newNetwork.id, sizeof id < sizeof newNetwork.id ?
		   sizeof id : sizeof newNetwork.id);
	    hubJack = VNetHub_AllocPvn(id);
	 }
	 break;
      default:
	 LOG(1, (KERN_NOTICE "/dev/vmnet: bad bind type: %u\n",
		 newNetwork.bindType));
	 return -EINVAL;
      }

      return VNetSwitchToDifferentPeer(&port->jack, hubJack, FALSE, NULL, NULL, NULL);
      break;

   case SIOCSFILTERRULES:
#ifdef CONFIG_NETFILTER
      if (copy_from_user(&ruleHeader, (void *)ioarg, sizeof ruleHeader)) {
         return -EFAULT;
      }

      /* Verify the call is for a known type */
      if (ruleHeader.type < VNET_FILTER_CMD_MIN ||
          ruleHeader.type > VNET_FILTER_CMD_MAX) {
         LOG(1, (KERN_NOTICE "/dev/vmnet: invalid filter command\n"));
         return -EINVAL;
      }

      /*
       * Version check should be done on a per-sub-command basis, but every
       * sub-command is currently using 1, so for now we globally check for
       * all sub-commands here in one place.
       */
      if (ruleHeader.ver != 1) {
         LOG(1, (KERN_NOTICE "/dev/vmnet: invalid version for "
                 "filter command\n"));
         return -EINVAL;
      }

      /*
       * Dispatch the sub-command.
       */
      return VNetFilter_HandleUserCall(&ruleHeader, ioarg);
#else
      LOG(0, (KERN_NOTICE "/dev/vmnet: kernel doesn't support netfilter\n"));
      return -EINVAL;
      break;
#endif

   case SIOCGBRSTATUS:
      {
         uint32 flags;

	 read_lock(&vnetPeerLock);
         flags = VNetIsBridged(&port->jack);
	 read_unlock(&vnetPeerLock);

         if (copy_to_user((void *)ioarg, &flags, sizeof flags)) {
            return -EFAULT;
	 }
      }
      break;

   case SIOCGIFADDR:
      if (copy_to_user((void *)ioarg, port->paddr, ETH_ALEN)) {
         return -EFAULT;
      }
      break;

   case SIOCSIFADDR:
      return -EFAULT;

   case SIOCSLADRF:
      if (copy_from_user(port->ladrf, (void *)ioarg, sizeof port->ladrf)) {
         return -EFAULT;
      }
      break;

   case SIOCSIFFLAGS:
      if (copy_from_user(&port->flags, (void *)ioarg, sizeof port->flags)) {
         return -EFAULT;
      }
      port->flags = ((port->flags
                      & (IFF_UP|IFF_BROADCAST|IFF_DEBUG
                         |IFF_PROMISC|IFF_MULTICAST|IFF_ALLMULTI))
                     | IFF_RUNNING);
      if (port->fileOpIoctl) {

         /*
          * Userif ports have some postprocessing when the IFF_UP flags is
          * changed.
          */
         port->fileOpIoctl(port, filp, iocmd, ioarg);
      }
      break;

   case SIOCSETMACADDR:
      if (copy_from_user(&macAddr, (void *)ioarg, sizeof macAddr)) {
         return -EFAULT;
      }

      switch (macAddr.version) {
      case 1:
         if (macAddr.flags & VNET_SETMACADDRF_UNIQUE) {
	    if (VMX86_IS_VIRT_ADAPTER_MAC(macAddr.addr)) {
	       return -EBUSY;
	    }
            return VNetSetMACUnique(port, macAddr.addr);
         }
         memcpy(port->paddr, macAddr.addr, ETH_ALEN);
         break;
      default:
         return -EINVAL;
         break;
      }
      break;
   case SIOCGETAPIVERSION2:
      {
	 uint32 verFromUser;
	 if (copy_from_user(&verFromUser, (void *)ioarg, sizeof verFromUser)) {
	    return -EFAULT;
	 }
	 /* Should we require verFromUser == VNET_API_VERSION? */
      }
      /* fall thru */
   case SIOCGETAPIVERSION:
      {
	 uint32 verToUser = VNET_API_VERSION;
	 if (copy_to_user((void*)ioarg, &verToUser, sizeof verToUser)) {
	    return -EFAULT;
	 }
      }
      break;
   default:
      if (!port->fileOpIoctl) {
         return -ENOIOCTLCMD;
      }
      return port->fileOpIoctl(port, filp, iocmd, ioarg);
      break;
   }

   return 0;
}


#if defined(HAVE_COMPAT_IOCTL) || defined(HAVE_UNLOCKED_IOCTL)
/*
 *----------------------------------------------------------------------
 *
 * VNetFileOpUnlockedIoctl --
 *
 *      Wrapper around VNetFileOpIoctl.  See VNetFileOpIoctl for
 *      supported arguments.  Called without big kernel lock held.
 *
 * Results:
 *      On success 0 else errno.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static long
VNetFileOpUnlockedIoctl(struct file    *filp,  // IN:
                        unsigned int    iocmd, // IN:
                        unsigned long   ioarg) // IN:
{
   struct inode *inode = NULL;
   long err;

   if (filp && filp->f_dentry) {
      inode = filp->f_dentry->d_inode;
   }
   compat_mutex_lock(&vnetMutex);
   err = VNetFileOpIoctl(inode, filp, iocmd, ioarg);
   compat_mutex_unlock(&vnetMutex);
   return err;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * VNetSwitchToDifferentPeer --
 *
 *      This function is used to disconnect from a one peer and
 *      connect to another peer.  If the connect to the new peer
 *      fails (e.g., if the connect would create a cycle), then
 *      the function will reconnect back to the original peer.
 *      The function will deallocate the old or the new peer,
 *      whichever is the one that has been disconnected.
 *
 *      optional behavior:
 *
 *      For the case where the function was successful in switching
 *      to the new peer, the caller can optionally provide a 'filp'
 *      (private_data is set to the new port), and the caller
 *      can also provide one each of a port to be added to
 *      and/or removed from the port list.  If the caller provides
 *      ports to add/remove from the list, then
 *      connectNewToPeerOfJack should be set to TRUE (otherwise
 *      inconsistencies in the port list are likely to occur).
 *
 * Results:
 *      errno (0 on success).
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
VNetSwitchToDifferentPeer(VNetJack *jack,              // IN: jack whose peer is to be changed
			  VNetJack *newPeer,           // IN: the new peer to try to switch to
			  Bool connectNewToPeerOfJack, // IN: connect new to peer of 'jack' or to 'jack' itself?
			  struct file *filp,           // IN: (optional) set filp to 'newpeerPort' on success
			  VNetPort *jackPort,          // IN: (optional) port to remove from list on success
			  VNetPort *newPeerPort)       // IN: (optional) port to add to list on success
{
   VNetJack *oldPeer;
   int retval;

   if (newPeer == NULL) {
      LOG(0, (KERN_NOTICE "/dev/vmnet: failed to alloc new peer\n"));
      return -EINVAL;
   }

   /*
    * OK this is tricky. Try and connect the new peer while saving
    * enough information so that we can reconnect back to the
    * old peer if a cycle is detected.
    */

   compat_mutex_lock(&vnetStructureMutex);

   /* Disconnect from the old peer */
   oldPeer = VNetDisconnect(jack);

   /* Try to connect to the new peer */
   if (connectNewToPeerOfJack) {
      retval = VNetConnect(oldPeer, newPeer);
   } else {
      retval = VNetConnect(jack, newPeer);
   }
   if (retval) {

      /* Connect failed, so reconnect back to old peer */
      int retval2 = VNetConnect(jack, oldPeer);
      compat_mutex_unlock(&vnetStructureMutex);

      /* Free the new peer */
      VNetFree(newPeer);
      if (retval2) {
	 // assert xxx redo this
	 LOG(1, (KERN_NOTICE "/dev/vmnet: cycle on connect failure\n"));
	 return -EBADF;
      }
      return retval;
   }

   if (newPeerPort != NULL) {
      VNetAddPortToList(newPeerPort);
   }
   if (filp != NULL) {
      filp->private_data = newPeerPort;
   }
   if (jackPort != NULL) {
      VNetRemovePortFromList(jackPort);
   }

   compat_mutex_unlock(&vnetStructureMutex);

   /* Connected to new peer, so dealloc the old peer */
   if (connectNewToPeerOfJack) {
      VNetFree(jack);
   } else {
      VNetFree(oldPeer);
   }

   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetMulticastFilter --
 *
 *      Utility function that filters multicast packets according
 *      to a 64-bit logical address filter (like the one on the
 *      lance chipset).  AllMultiFilter lets all packets through.
 *
 *      We generate a hash value from the destination MAC address
 *      and see if it's in our filter.  Broadcast packets have
 *      already OK'd by PacketMatch, so we don't have to worry
 *      about that.
 *
 *      (This is in the green AMD "Ethernet Controllers" book,
 *      page 1-53.)
 *
 * Results:
 *      TRUE if packet is in filter, FALSE if not.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

#define CRC_POLYNOMIAL_BE 0x04c11db7UL  /* Ethernet CRC, big endian */

static INLINE_SINGLE_CALLER Bool
VNetMulticastFilter(const uint8 *destAddr, // IN: multicast MAC
		    const uint8 *ladrf)    // IN: multicast filter
{
   uint16 hashcode;
   int32 crc;
   int32 poly = CRC_POLYNOMIAL_BE;
   int j;
   int bit;
   int byte;

   crc = 0xffffffff;                  /* init CRC for each address */
   for (byte = 0; byte < ETH_ALEN; byte++) { /* for each address byte */
      /* process each address bit */
      for (bit = *destAddr++, j = 0;
	   j < VNET_LADRF_LEN;
	   j++, bit >>= 1) {
	 crc = (crc << 1) ^ ((((crc<0?1:0) ^ bit) & 0x01) ? poly : 0);
      }
   }
   hashcode = (crc & 1);              /* hashcode is 6 LSb of CRC ... */
   for (j = 0; j < 5; j++) {                /* ... in reverse order. */
      hashcode = (hashcode << 1) | ((crc>>=1) & 1);
   }

   byte = hashcode >> 3;              /* bit[3-5] -> byte in filter */
   bit = 1 << (hashcode & 0x07);      /* bit[0-2] -> bit in byte */
   if (ladrf[byte] & bit) {
      return TRUE;
   } else {
      return FALSE;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * VNetPacketMatch --
 *
 *      Determines whether the packet should be given to the interface.
 *
 * Results:
 *      TRUE if the pasket is OK for this interface, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
VNetPacketMatch(const uint8   *destAddr, // IN: destination MAC
                const uint8   *ifAddr,   // IN: MAC of interface
		const uint8   *ladrf,    // IN: multicast filter
                uint32   flags)          // IN: filter flags
{
   /*
    * Return TRUE if promiscuous requested, or unicast destined
    * for interface, or broadcast (and broadcast requested), or
    * if multicast (and all multicast, or this specific
    * multicast MAC, was requested).
    */

   return ((flags & IFF_PROMISC) || MAC_EQ(destAddr, ifAddr) ||
	   ((flags & IFF_BROADCAST) && MAC_EQ(destAddr, broadcast)) ||
	   ((destAddr[0] & 0x1) && (flags & IFF_ALLMULTI ||
	     (flags & IFF_MULTICAST &&
	      VNetMulticastFilter(destAddr, ladrf)))));
}


/*
 *----------------------------------------------------------------------
 *
 * VNet_MakeMACAddress --
 *
 *      Generate a unique MAC address and assign it to the given port.
 *      The address will be in the range:
 *
 *      VMX86_STATIC_OUI:e0:00:00 - VMX86_STATIC_OUI:ff:ff:ff
 *
 * Results:
 *      errno.
 *
 * Side effects:
 *      The address is changed.
 *
 *----------------------------------------------------------------------
 */

int
VNet_MakeMACAddress(VNetPort *port) // IN: port
{
   uint8 paddr[ETH_ALEN] = {0};
   int conflict;
   int maxTries = 1000;

   do {
      VMX86_GENERATE_RANDOM_MAC(paddr);

      conflict = VNetSetMACUnique(port, paddr);

      /*
       * We don't have to check for conflicts with the virtual
       * host adapters since they are in the range
       * c0:00:00-c0:00:FF.
       */

   } while (maxTries-- > 0 && conflict);

   return conflict;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetConnect --
 *
 *      Connect 2 jacks.
 *	vnetStructureMutex must be held.
 *
 * Results:
 *      errno.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VNetConnect(VNetJack *jack1, // IN: jack
            VNetJack *jack2) // IN: jack
{
   static int vnetGeneration = 0;
   Bool foundCycle;
   unsigned long flags;

   vnetGeneration++;

   foundCycle = VNetCycleDetect(jack1, vnetGeneration);
   if (foundCycle) {
      VNetFreeInterfaceList();
      return -EDEADLK;
   }

   foundCycle = VNetCycleDetect(jack2, vnetGeneration);
   if (foundCycle) {
      VNetFreeInterfaceList();
      return -EDEADLK;
   }
   VNetFreeInterfaceList();

   /*
    * Synchronize with peer readers
    */

   write_lock_irqsave(&vnetPeerLock, flags);
   jack1->peer = jack2;
   jack2->peer = jack1;
   write_unlock_irqrestore(&vnetPeerLock, flags);

   if (jack2->numPorts) {
      VNetPortsChanged(jack1);
   }

   if (jack1->numPorts) {
      VNetPortsChanged(jack2);
   }

   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetDisconnect --
 *
 *	Disconnect 2 jacks.
 *	vnetStructureMutex must be held.
 *
 * Results:
 *      Return the peer jack (returns NULL on error, or if no peer)
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

VNetJack *
VNetDisconnect(VNetJack *jack) // IN: jack
{
   VNetJack *peer;
   unsigned long flags;

   write_lock_irqsave(&vnetPeerLock, flags);
   peer = jack->peer;
   if (!peer) {
      write_unlock_irqrestore(&vnetPeerLock, flags);
      return NULL;
   }
   jack->peer = NULL;
   peer->peer = NULL;
   write_unlock_irqrestore(&vnetPeerLock, flags);

   if (peer->numPorts) {
      VNetPortsChanged(jack);
   }

   if (jack->numPorts) {
      VNetPortsChanged(peer);
   }

   return peer;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetCycleDetectIf --
 *
 *      Perform the cycle detect alogorithm for this generation on a
 *      specific interface. This could be a bridged interface, host
 *      interface or both.
 *      vnetStructureMutex must be held.
 *
 * Results:
 *      TRUE if a cycle was detected, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
VNetCycleDetectIf(const char *name, // IN:
                  int   generation) // IN:
{
   VNetInterface *p;

   for (p = vnetInterfaces; p != NULL; p = p->next) {
      if (!strcmp(name, p->name)) {
         if (p->myGeneration == generation) {
            return TRUE;
         } else {
            p->myGeneration = generation;
            return FALSE;
         }
      }
   }

   p = kmalloc(sizeof *p, GFP_USER);
   if (!p) {
      // assert
      return TRUE;
   }

   memcpy(p->name, name, sizeof p->name);
   NULL_TERMINATE_STRING(p->name);
   p->myGeneration = generation;
   p->next = vnetInterfaces;
   vnetInterfaces = p;

   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetFreeInterfaceList --
 *
 *      Free's the linked list that may have been constructed
 *      during a recent run on the cycle detect alogorithm.
 *      vnetStructureMutex must be held.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
VNetFreeInterfaceList()
{
   while (vnetInterfaces != NULL) {
      VNetInterface *next = vnetInterfaces->next;
      kfree(vnetInterfaces);
      vnetInterfaces = next;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * VNetSend --
 *
 *      Send a packet through this jack. Note, the packet goes to the
 *      jacks peer.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The skb is no longer owned by us.
 *
 *----------------------------------------------------------------------
 */

void
VNetSend(const VNetJack *jack, // IN: jack
         struct sk_buff *skb)  // IN: packet
{
   read_lock(&vnetPeerLock);
   if (jack && jack->peer && jack->peer->rcv) {
      jack->peer->rcv(jack->peer, skb);
   } else {
      dev_kfree_skb(skb);
   }
   read_unlock(&vnetPeerLock);
}


/*
 *----------------------------------------------------------------------
 *
 * VNetSetMACUnique --
 *
 *      Verify that MAC address is not used by other ports.
 *      Function grabs mutex, so caller shouldn't hold any
 *      locks.
 *
 * Results:
 *
 *      0      if address is unique. Port's paddr is updated.
 *      -EBUSY if address is already in use. Port's paddr is unchanged.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VNetSetMACUnique(VNetPort *port,            // IN:
		 const uint8 mac[ETH_ALEN]) // IN:
{
   VNetPort *p;

   compat_mutex_lock(&vnetStructureMutex);
   for (p = vnetAllPorts; p != NULL; p = p->next) {
      if (p != port && MAC_EQ(p->paddr, mac)) {
         compat_mutex_unlock(&vnetStructureMutex);
         return -EBUSY;
      }
   }
   memcpy(port->paddr, mac, ETH_ALEN);
   compat_mutex_unlock(&vnetStructureMutex);
   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetPrintJack --
 *
 *      Print info about the jack to a buffer.
 *
 * Results:
 *      Length of the write.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VNetPrintJack(const VNetJack *jack, // IN: jack
              char           *buf)  // OUT: info about jack
{
   int len = 0;

   read_lock(&vnetPeerLock);
   if (!jack->peer) {
      len += sprintf(buf+len, "connected not ");
   } else {
      len += sprintf(buf+len, "connected %s ", jack->peer->name);
   }
   read_unlock(&vnetPeerLock);

   return len;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetPrintPort --
 *
 *      Print info about the port to a buffer.
 *
 * Results:
 *      Length of the write.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VNetPrintPort(const VNetPort *port, // IN: port
              char           *buf)  // OUT: info about port
{
   int len = 0;

   len += VNetPrintJack(&port->jack, buf+len);

   len += sprintf(buf+len, "mac %02x:%02x:%02x:%02x:%02x:%02x ",
                  port->paddr[0], port->paddr[1], port->paddr[2],
                  port->paddr[3], port->paddr[4], port->paddr[5]);

   len += sprintf(buf+len, "ladrf %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x ",
                  port->ladrf[0], port->ladrf[1], port->ladrf[2],
                  port->ladrf[3], port->ladrf[4], port->ladrf[5],
                  port->ladrf[6], port->ladrf[7]);

   len += sprintf(buf+len, "flags IFF_RUNNING");

   if (port->flags & IFF_UP) {
      len += sprintf(buf+len, ",IFF_UP");
   }

   if (port->flags & IFF_BROADCAST) {
      len += sprintf(buf+len, ",IFF_BROADCAST");
   }

   if (port->flags & IFF_DEBUG) {
      len += sprintf(buf+len, ",IFF_DEBUG");
   }

   if (port->flags & IFF_PROMISC) {
      len += sprintf(buf+len, ",IFF_PROMISC");
   }

   if (port->flags & IFF_MULTICAST) {
      len += sprintf(buf+len, ",IFF_MULTICAST");
   }

   if (port->flags & IFF_ALLMULTI) {
      len += sprintf(buf+len, ",IFF_ALLMULTI");
   }

   len += sprintf(buf+len, " ");

   return len;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetSnprintf --
 *
 *      Wrapper to account for lack of snprintf() in older kernels,
 *      where 'old' appears to be older than 2.4.8.
 *
 * Results:
 *      Refer to docs for snprintf() and / or sprintf().  This
 *      version unconditionally adds NULL termination to the end
 *      of the string.
 *
 * Side effects:
 *      Might overrun buffer on older kernels.
 *
 *----------------------------------------------------------------------
 */

int
VNetSnprintf(char *str,          // OUT: resulting string
	     size_t size,        // IN: length of 'result' in bytes
	     const char *format, // IN: format string
	     ...)                // IN: (optional)
{
   int length;
   va_list args;

   va_start(args, format);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 8)
   length = vsnprintf(str, size, format, args);
#else
   length = vsprintf(str, format, args);
#endif

   va_end(args);

   str[size - 1] = '\0';

   return length;
}


MODULE_AUTHOR("VMware, Inc.");
MODULE_DESCRIPTION("VMware Virtual Networking Driver.");
MODULE_LICENSE("GPL v2");
/*
 * Starting with SLE10sp2, Novell requires that IHVs sign a support agreement
 * with them and mark their kernel modules as externally supported via a
 * change to the module header. If this isn't done, the module will not load
 * by default (i.e., neither mkinitrd nor modprobe will accept it).
 */
MODULE_INFO(supported, "external");
