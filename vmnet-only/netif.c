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
#include <linux/version.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/poll.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/mm.h>
#include "compat_skbuff.h"
#include <linux/sockios.h>
#include "compat_sock.h"

#define __KERNEL_SYSCALLS__
#include <asm/io.h>

#include <linux/proc_fs.h>
#include <linux/file.h>

#include "vnetInt.h"
#include "compat_netdevice.h"
#include "vmnetInt.h"


typedef struct VNetNetIF {
   VNetPort                port;
   struct net_device      *dev;
   char                    devName[VNET_NAME_LEN];
   struct net_device_stats stats;
} VNetNetIF;


static void VNetNetIfFree(VNetJack *this);
static void VNetNetIfReceive(VNetJack *this, struct sk_buff *skb);
static Bool VNetNetIfCycleDetect(VNetJack *this, int generation);

static int  VNetNetifOpen(struct net_device *dev);
static int  VNetNetifProbe(struct net_device *dev);
static int  VNetNetifClose(struct net_device *dev);
static int  VNetNetifStartXmit(struct sk_buff *skb, struct net_device *dev);
static struct net_device_stats *VNetNetifGetStats(struct net_device *dev);
static int  VNetNetifSetMAC(struct net_device *dev, void *addr);
static void VNetNetifSetMulticast(struct net_device *dev);
#if 0
static void VNetNetifTxTimeout(struct net_device *dev);
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
static int  VNetNetIfProcShow(struct seq_file *seqf, void *data);
#else
static int  VNetNetIfProcRead(char *page, char **start, off_t off,
                              int count, int *eof, void *data);
#endif


#if 0
/*
 *----------------------------------------------------------------------
 *
 * VNetNetIfTxTimeout --
 *
 *      Enables processing of Tx queue after it was stopped for so long.
 *      It should not happen with vmnet system.
 * 
 * Results: 
 *      None.
 *
 * Side effects:
 *      Tx queue enabled, message in log.
 *
 *----------------------------------------------------------------------
 */

static void
VNetNetifTxTimeout(struct net_device *dev) // IN:
{
   static int netRateLimit = 0;
   
   if (netRateLimit < 10) {
      LOG(0, (KERN_NOTICE "%s: Transmit timeout\n", dev->name));
      netRateLimit++;
   }
   /* We cannot stuck due to hardware, so always wake up processing */
   netif_wake_queue(dev);
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * VNetNetIfSetup --
 *
 *      Sets initial netdevice state.
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
VNetNetIfSetup(struct net_device *dev)  // IN:
{
#ifdef HAVE_NET_DEVICE_OPS
   static const struct net_device_ops vnetNetifOps = {
      .ndo_init = VNetNetifProbe,
      .ndo_open = VNetNetifOpen,
      .ndo_start_xmit = VNetNetifStartXmit,
      .ndo_stop = VNetNetifClose,
      .ndo_get_stats = VNetNetifGetStats,
      .ndo_set_mac_address = VNetNetifSetMAC,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 2, 0)
      .ndo_set_rx_mode = VNetNetifSetMulticast,
#else
      .ndo_set_multicast_list = VNetNetifSetMulticast,
#endif
      /*
       * We cannot stuck... If someone will report problems under
       * low memory conditions or some such, we should enable it.
       */
#if 0
      .ndo_tx_timeout = VNetNetifTxTimeout,
#endif
   };
#endif /* HAVE_NET_DEVICE_OPS */

   ether_setup(dev); // turns on IFF_BROADCAST, IFF_MULTICAST
#ifdef HAVE_NET_DEVICE_OPS
   dev->netdev_ops = &vnetNetifOps;
#else
   dev->init = VNetNetifProbe;
   dev->open = VNetNetifOpen;
   dev->hard_start_xmit = VNetNetifStartXmit;
   dev->stop = VNetNetifClose;
   dev->get_stats = VNetNetifGetStats;
   dev->set_mac_address = VNetNetifSetMAC;
   dev->set_multicast_list = VNetNetifSetMulticast;
   /*
    * We cannot stuck... If someone will report problems under
    * low memory conditions or some such, we should enable it.
    */
#if 0
   dev->tx_timeout = VNetNetifTxTimeout;
#endif
#endif /* HAVE_NET_DEVICE_OPS */

#if 0
   /* Only necessary if tx_timeout is set.  See above. */
   dev->watchdog_timeo = TX_TIMEOUT;
#endif
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
static int proc_netif_open(struct inode *inode, struct file *file)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
       return single_open(file, VNetNetIfProcShow, pde_data(inode));
#else
       return single_open(file, VNetNetIfProcShow, PDE_DATA(inode));
#endif
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops proc_netif_fops = {
       .proc_open           = proc_netif_open,
       .proc_read           = seq_read,
       .proc_lseek          = seq_lseek,
       .proc_release        = seq_release,
#else
static const struct file_operations proc_netif_fops = {
       .open           = proc_netif_open,
       .read           = seq_read,
       .llseek         = seq_lseek,
       .release        = seq_release,
#endif
};
#endif

/*
 *----------------------------------------------------------------------
 *
 * VNetNetIfNetDeviceToNetIf --
 *
 *      Converts net_device to netIf.
 * 
 * Results: 
 *      Converted pointer.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE VNetNetIF *
VNetNetIfNetDeviceToNetIf(struct net_device *dev)
{
#ifdef HAVE_NETDEV_PRIV
   VNetNetIF** devPriv = netdev_priv(dev);

   return *devPriv;
#else
   return dev->priv;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * VNetNetIf_Create --
 *
 *      Create a net level port to the wonderful world of virtual
 *      networking.
 * 
 * Results: 
 *      Errno. Also returns an allocated port to connect to,
 *      NULL on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VNetNetIf_Create(char *devName,  // IN:
                 VNetPort **ret, // OUT:
                 int hubNum)     // IN: 
{
   VNetNetIF *netIf;
   struct net_device *dev;
   int retval = 0;
   static unsigned id = 0;
   
   netIf = kmalloc(sizeof *netIf, GFP_KERNEL);
   if (!netIf) {
      retval = -ENOMEM;
      goto out;
   }

   /*
    * Initialize fields.
    */
   
   netIf->port.id = id++;   
   netIf->port.next = NULL;

   netIf->port.jack.peer = NULL;
   netIf->port.jack.numPorts = 1;
   VNetSnprintf(netIf->port.jack.name, sizeof netIf->port.jack.name,
		"netif%u", netIf->port.id);
   netIf->port.jack.private = netIf;
   netIf->port.jack.index = 0;
   netIf->port.jack.procEntry = NULL;
   netIf->port.jack.free = VNetNetIfFree;
   netIf->port.jack.rcv = VNetNetIfReceive;
   netIf->port.jack.cycleDetect = VNetNetIfCycleDetect;
   netIf->port.jack.portsChanged = NULL;
   netIf->port.jack.isBridged = NULL;
   
   /*
    * Make proc entry for this jack.
    */
   
   retval = VNetProc_MakeEntry(netIf->port.jack.name, S_IFREG,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
                               &netIf->port.jack.procEntry, &proc_netif_fops, netIf);
#else
                               &netIf->port.jack.procEntry);
#endif
   if (retval) {
      if (retval == -ENXIO) {
         netIf->port.jack.procEntry = NULL;
      } else {
         netIf->port.jack.procEntry = NULL;
         goto out;
      }
   } else {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
      netIf->port.jack.procEntry->read_proc = VNetNetIfProcRead;
      netIf->port.jack.procEntry->data = netIf;
#endif
   }

   /*
    * Rest of fields.
    */
   
   netIf->port.flags = IFF_RUNNING;

   memset(netIf->port.paddr, 0, sizeof netIf->port.paddr);
   memset(netIf->port.ladrf, 0, sizeof netIf->port.ladrf);

   /* This will generate the reserved MAC address c0:00:?? where ?? == hubNum. */
   VMX86_BUILD_MAC(netIf->port.paddr, hubNum);
   
   /* Make sure the MAC is unique. */
   retval = VNetSetMACUnique(&netIf->port, netIf->port.paddr);
   if (retval) {
     goto out;
   }

   netIf->port.fileOpRead = NULL;
   netIf->port.fileOpWrite = NULL;
   netIf->port.fileOpIoctl = NULL;
   netIf->port.fileOpPoll = NULL;
   
   memset(&netIf->stats, 0, sizeof netIf->stats);
   
   memcpy(netIf->devName, devName, sizeof netIf->devName);
   NULL_TERMINATE_STRING(netIf->devName);

#ifdef HAVE_NETDEV_PRIV
   dev = compat_alloc_netdev(sizeof(VNetNetIF *), netIf->devName, VNetNetIfSetup);
   if (!dev) {
      retval = -ENOMEM;
      goto out;
   }
   *(VNetNetIF**)netdev_priv(dev) = netIf;
#else
   dev = compat_alloc_netdev(0, netIf->devName, VNetNetIfSetup);
   if (!dev) {
      retval = -ENOMEM;
      goto out;
   }
   dev->priv = netIf;
#endif
   netIf->dev = dev;
   
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
   dev_addr_set(dev, netIf->port.paddr);
#else
   memcpy(dev->dev_addr, netIf->port.paddr, sizeof netIf->port.paddr);
#endif
   
   if (register_netdev(dev) != 0) {
      LOG(0, (KERN_NOTICE "%s: could not register network device\n", devName));
      retval = -ENODEV;
      goto outFreeDev;
   }

   *ret = (VNetPort*)netIf;
   return 0;

outFreeDev:
   compat_free_netdev(dev);
out:
   if (netIf) {
      if (netIf->port.jack.procEntry) {
         VNetProc_RemoveEntry(netIf->port.jack.procEntry);
      }
      kfree(netIf);
   }
   return retval;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetNetIfFree --
 *
 *      Free the net interface port.
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
VNetNetIfFree(VNetJack *this) // IN: jack
{
   VNetNetIF *netIf = (VNetNetIF*)this;

   unregister_netdev(netIf->dev);
   compat_free_netdev(netIf->dev);
   if (this->procEntry) {
      VNetProc_RemoveEntry(this->procEntry);
   }
   kfree(netIf);
}


/*
 *----------------------------------------------------------------------
 *
 * VNetNetIfReceive --
 *
 *      This jack is receiving a packet. Take appropriate action.
 *
 * Results: 
 *      None.
 *
 * Side effects:
 *      Frees skb.
 *
 *----------------------------------------------------------------------
 */

void
VNetNetIfReceive(VNetJack        *this, // IN: jack
		 struct sk_buff  *skb)  // IN: packet 
{
   VNetNetIF *netIf = (VNetNetIF*)this->private;
   uint8 *dest = SKB_2_DESTMAC(skb);
   
   if (!NETDEV_UP_AND_RUNNING(netIf->dev)) {
      goto drop_packet;
   }

   if (!VNetPacketMatch(dest,
                        netIf->dev->dev_addr,
                        allMultiFilter, 
                        netIf->dev->flags)) {
      goto drop_packet;
   }
   
   /* send to the host interface */
   skb->dev = netIf->dev;
   skb->protocol = eth_type_trans(skb, netIf->dev);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
   netif_rx(skb);
#else
   netif_rx_ni(skb);
#endif
   netIf->stats.rx_packets++;

   return;
   
 drop_packet:
   dev_kfree_skb(skb);
}


/*
 *----------------------------------------------------------------------
 *
 * VNetNetIfCycleDetect --
 *
 *      Cycle detection algorithm.
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
VNetNetIfCycleDetect(VNetJack *this,       // IN: jack
                     int       generation) // IN: 
{
   VNetNetIF *netIf = (VNetNetIF*)this->private;
   return VNetCycleDetectIf(netIf->devName, generation);
}


/*
 *----------------------------------------------------------------------
 *
 * VNetNetifOpen --
 *
 *      The virtual network's open dev operation. 
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
VNetNetifOpen(struct net_device *dev) // IN:
{
   /*
    * The host interface is not available if the hub is bridged.
    *
    * It's actually okay to support both.  We just need
    * to tag packets when VNetXmitPacket gives them to the interface
    * so they can be dropped by VNetBridgeReceive().
    *
    *  if so return -EBUSY;
    */

   netif_start_queue(dev);
   // xxx need to change flags
   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetNetifProbe --
 *
 *      ???
 *
 * Results: 
 *      0.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VNetNetifProbe(struct net_device *dev) // IN: unused
{
   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetNetifClose --
 *
 *      The virtual network's close dev operation. 
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
VNetNetifClose(struct net_device *dev) // IN:
{
   netif_stop_queue(dev);
   // xxx need to change flags
   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetNetifStartXmit --
 *
 *      The virtual network's start xmit dev operation. 
 *
 * Results: 
 *      ???, 0.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VNetNetifStartXmit(struct sk_buff    *skb, // IN:
                   struct net_device *dev) // IN:
{
   VNetNetIF *netIf;

   if(skb == NULL) {
      return 0;
   }

   netIf = VNetNetIfNetDeviceToNetIf(dev);

   /* 
    * Block a timer-based transmit from overlapping.  This could better be
    * done with atomic_swap(1, dev->tbusy), but set_bit() works as well.
    * If this ever occurs the queue layer is doing something evil!
    */

   VNetSend(&netIf->port.jack, skb);

   netIf->stats.tx_packets++;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0)
   netif_trans_update(dev);
#else
   dev->trans_start = jiffies;
#endif

   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetNetifSetMAC --
 *
 *      Sets MAC address (i.e. via ifconfig) of netif device.
 *
 * Results: 
 *      Errno.
 *
 * Side effects:
 *      The MAC address may be changed.
 *
 *----------------------------------------------------------------------
 */

int
VNetNetifSetMAC(struct net_device *dev, // IN:
                void *p)                // IN:
{
   VNetNetIF *netIf;
   struct sockaddr const *addr = p;

   if (!VMX86_IS_STATIC_MAC(addr->sa_data)) {
      return -EINVAL;
   }
   netIf = VNetNetIfNetDeviceToNetIf(dev);
   memcpy(netIf->port.paddr, addr->sa_data, dev->addr_len);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
   dev_addr_set(dev, addr->sa_data);
#else
   memcpy(dev->dev_addr, addr->sa_data, dev->addr_len);
#endif
   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetNetifSetMulticast --
 *
 *      Sets or clears the multicast address list.  This information
 *      comes from an array in dev->mc_list, and with a counter in
 *      dev->mc_count.
 *
 *      Since host-only network ifaces can't be bridged, it's debatable
 *      whether this is at all useful, but at least now you can turn it 
 *      on from ifconfig without getting an ioctl error.
 * Results: 
 *      Void.
 *
 * Side effects:
 *      Multicast address list might get changed.
 *
 *----------------------------------------------------------------------
 */

void
VNetNetifSetMulticast(struct net_device *dev) // IN: unused
{
}


/*
 *----------------------------------------------------------------------
 *
 * VNetNetifGetStats --
 *
 *      The virtual network's get stats dev operation. 
 *
 * Results: 
 *      A struct full of stats.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static struct net_device_stats *
VNetNetifGetStats(struct net_device *dev) // IN:
{
   VNetNetIF *netIf = VNetNetIfNetDeviceToNetIf(dev);

   return &netIf->stats;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetNetIfProcRead/VNetNetIfProcShow --
 *
 *      Callback for read operation on this netif entry in vnets proc fs.
 *
 * Results: 
 *      Length of read operation.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
VNetNetIfProcShow(struct seq_file *seqf, // IN/OUT: buffer to write into
#else
VNetNetIfProcRead(char   *page,  // IN/OUT: buffer to write into
                  char  **start, // OUT: 0 if file < 4k, else offset into page
                  off_t   off,   // IN: (unused) offset of read into the file
                  int     count, // IN: (unused) maximum number of bytes to read
                  int    *eof,   // OUT: TRUE if there is nothing more to read
#endif
                  void   *data)  // IN: client data
{
   VNetNetIF *netIf = (VNetNetIF*)data; 
   int len = 0;
   
   if (!netIf) {
      return len;
   }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
   VNetPrintPort(&netIf->port, seqf);

   seq_printf(seqf, "dev %s ", netIf->dev->name);
   
   seq_printf(seqf, "\n");

   return 0;
#else
   len += VNetPrintPort(&netIf->port, page+len);

   len += sprintf(page+len, "dev %s ", netIf->devName);
   
   len += sprintf(page+len, "\n");

   *start = 0;
   *eof   = 1;
   return len;
#endif
}
