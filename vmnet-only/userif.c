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
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
#include <linux/sched/signal.h>
#endif

#include <linux/slab.h>
#include <linux/poll.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include "compat_skbuff.h"
#include <linux/if_ether.h>
#include <linux/sockios.h>
#include "compat_sock.h"

#define __KERNEL_SYSCALLS__
#include <asm/io.h>

#include <linux/proc_fs.h>
#include <linux/file.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 4)
#include <net/checksum.h>
#endif

#include "vnetInt.h"

#include "compat_uaccess.h"
#include "compat_highmem.h"
#include "compat_mm.h"
#include "pgtbl.h"
#include "compat_wait.h"
#include "vmnetInt.h"
#include "vm_atomic.h"

typedef struct VNetUserIFStats {
   unsigned    read;
   unsigned    written;
   unsigned    queued;
   unsigned    droppedDown;
   unsigned    droppedMismatch;
   unsigned    droppedOverflow;
   unsigned    droppedLargePacket;
} VNetUserIFStats;

typedef struct VNetUserIF {
   VNetPort               port;
   struct sk_buff_head    packetQueue;
   uint32*                pollPtr;
   Atomic_uint32*         actPtr;
   uint32                 pollMask;
   uint32                 actMask;
   uint32*                recvClusterCount;
   wait_queue_head_t      waitQueue;
   struct page*           actPage;
   struct page*           pollPage;
   struct page*           recvClusterPage;
   VNetUserIFStats        stats;
} VNetUserIF;

static void VNetUserIfUnsetupNotify(VNetUserIF *userIf);
static int  VNetUserIfSetupNotify(VNetUserIF *userIf, VNet_Notify *vn);

/*
 *-----------------------------------------------------------------------------
 *
 * UserifLockPage --
 *
 *    Lock in core the physical page associated to a valid virtual
 *    address --hpreg
 * 
 * Results:
 *    The page structure on success
 *    NULL on failure: memory pressure. Retry later
 *
 * Side effects:
 *    Loads page into memory	
 *    Pre-2.4.19 version may temporarily lock another physical page
 *
 *-----------------------------------------------------------------------------
 */

static INLINE struct page *
UserifLockPage(VA addr) // IN
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 19)
   struct page *page = NULL;
   int retval;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
   mmap_read_lock(current->mm);
#else
   down_read(&current->mm->mmap_sem);
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 6, 0)
   retval = get_user_pages_remote(current, current->mm, addr,
#else
   retval = get_user_pages(current, current->mm, addr,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
                           1, FOLL_WRITE, &page, NULL, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
                           1, FOLL_WRITE, &page, NULL);
#else
			   1, 1, 0, &page, NULL);
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
   mmap_read_unlock(current->mm);
#else
   up_read(&current->mm->mmap_sem);
#endif

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
 * VNetUserIfInvalidPointer --
 *
 *    Reports if pointer provided by user is definitely wrong, 
 *    or only potentially wrong.
 *
 * Results:
 *    non-zero if pointer is definitely wrong, otherwise returns
 *    0 if the pointer might be okay.
 *
 * Side effects:
 *    Might sleep.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE int
VNetUserIfInvalidPointer(VA uAddr,    // IN: user-provided pointer 
			 size_t size) // IN: anticipated size of data
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
   return !access_ok((void *)uAddr, size);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
   return !access_ok(VERIFY_WRITE, (void *)uAddr, size);
#else
   return verify_area(VERIFY_READ, (void *)uAddr, size) ||
          verify_area(VERIFY_WRITE, (void *)uAddr, size);
#endif
}

/*
 *-----------------------------------------------------------------------------
 *
 * VNetUserIfMapUint32Ptr --
 *
 *    Maps a portion of user-space memory into the kernel.
 *
 * Results:
 *    0 on success
 *    < 0 on failure: the actual value determines the type of failure
 *
 * Side effects:
 *    Might sleep.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE int
VNetUserIfMapUint32Ptr(VA uAddr,        // IN: pointer to user memory
		       struct page **p, // OUT: locked page
		       uint32 **ptr)    // OUT: kernel mapped pointer
{
   if (VNetUserIfInvalidPointer(uAddr, sizeof (uint32)) ||
       (((uAddr + sizeof(uint32) - 1) & ~(PAGE_SIZE - 1)) != 
        (uAddr & ~(PAGE_SIZE - 1)))) {
      return -EINVAL;
   }

   *p = UserifLockPage(uAddr);
   if (*p == NULL) {
      return -EAGAIN;
   }

   *ptr = (uint32 *)((char *)kmap(*p) + (uAddr & (PAGE_SIZE - 1)));
   return 0;
}

/*
 *-----------------------------------------------------------------------------
 *
 * VNetUserIfSetupNotify --
 *
 *    Sets up notification by filling in pollPtr, actPtr, and recvClusterCount
 *    fields.
 * 
 * Results: 
 *    0 on success
 *    < 0 on failure: the actual value determines the type of failure
 *
 * Side effects:
 *    Fields pollPtr, actPtr, recvClusterCount, pollPage, actPage, and 
 *    recvClusterPage are filled in VNetUserIf structure.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE int
VNetUserIfSetupNotify(VNetUserIF *userIf, // IN
                      VNet_Notify *vn)           // IN
{
   int retval;

   if (userIf->pollPtr || userIf->actPtr || userIf->recvClusterCount) {
      LOG(0, (KERN_DEBUG "vmnet: Notification mechanism already active\n"));
      return -EBUSY;
   }

   if ((retval = VNetUserIfMapUint32Ptr((VA)vn->pollPtr, &userIf->pollPage, 
                                       &userIf->pollPtr)) < 0) {
      return retval;
   }
   
   if ((retval = VNetUserIfMapUint32Ptr((VA)vn->actPtr, &userIf->actPage,
                                       (uint32 **)&userIf->actPtr)) < 0) {
      VNetUserIfUnsetupNotify(userIf);
      return retval;
   }

   if ((retval = VNetUserIfMapUint32Ptr((VA)vn->recvClusterPtr, 
					&userIf->recvClusterPage,
					&userIf->recvClusterCount)) < 0) {
      VNetUserIfUnsetupNotify(userIf);
      return retval;
   }

   userIf->pollMask = vn->pollMask;
   userIf->actMask = vn->actMask;
   return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * VNetUserIfUnsetupNotify --
 *
 *      Destroys permanent mapping for notify structure provided by user.
 * 
 * Results: 
 *      None.
 *
 * Side effects:
 *      Fields pollPtr, actPtr, recvClusterCount, etc. in VNetUserIf
 *      structure are cleared.
 *
 *----------------------------------------------------------------------
 */

static void
VNetUserIfUnsetupNotify(VNetUserIF *userIf) // IN
{
   if (userIf->pollPage) {
      kunmap(userIf->pollPage);
      put_page(userIf->pollPage);
   } else {
      LOG(0, (KERN_DEBUG "vmnet: pollPtr was already deactivated\n"));
   }
   if (userIf->actPage) {
      kunmap(userIf->actPage);
      put_page(userIf->actPage);
   } else {
      LOG(0, (KERN_DEBUG "vmnet: actPtr was already deactivated\n"));
   }
   if (userIf->recvClusterPage) {
      kunmap(userIf->recvClusterPage);
      put_page(userIf->recvClusterPage);
   } else {
      LOG(0, (KERN_DEBUG "vmnet: recvClusterPtr was already deactivated\n"));
   }
   userIf->pollPtr = NULL;
   userIf->pollPage = NULL;
   userIf->actPtr = NULL;
   userIf->actPage = NULL;
   userIf->recvClusterCount = NULL;
   userIf->recvClusterPage = NULL;
   userIf->pollMask = 0;
   userIf->actMask = 0;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetUserIfFree --
 *
 *      Free the user interface port.
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
VNetUserIfFree(VNetJack *this) // IN
{
   VNetUserIF *userIf = (VNetUserIF*)this;
   struct sk_buff *skb;

   for (;;) {
      skb = skb_dequeue(&userIf->packetQueue);
      if (skb == NULL) {
	 break;
      }
      dev_kfree_skb(skb);
   }
   
   if (userIf->pollPtr) {
      VNetUserIfUnsetupNotify(userIf);
   }

   if (this->procEntry) {
      VNetProc_RemoveEntry(this->procEntry);
   }

   kfree(userIf);
}


/*
 *----------------------------------------------------------------------
 *
 * VNetUserIfReceive --
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

static void
VNetUserIfReceive(VNetJack       *this, // IN
                  struct sk_buff *skb)  // IN
{
   VNetUserIF *userIf = (VNetUserIF*)this->private;
   uint8 *dest = SKB_2_DESTMAC(skb);
   
   if (!UP_AND_RUNNING(userIf->port.flags)) {
      userIf->stats.droppedDown++;
      goto drop_packet;
   }
   
   if (!VNetPacketMatch(dest,
                        userIf->port.paddr,
                        userIf->port.ladrf,
                        userIf->port.flags)) {
      userIf->stats.droppedMismatch++;
      goto drop_packet;
   }
   
   if (skb_queue_len(&userIf->packetQueue) >= VNET_MAX_QLEN) {
      userIf->stats.droppedOverflow++;
      goto drop_packet;
   }
   
   if (skb->len > ETHER_MAX_QUEUED_PACKET) {
      userIf->stats.droppedLargePacket++;
      goto drop_packet;
   }

   userIf->stats.queued++;

   skb_queue_tail(&userIf->packetQueue, skb);
   if (userIf->pollPtr) {
      *userIf->pollPtr |= userIf->pollMask;
      if (skb_queue_len(&userIf->packetQueue) >= (*userIf->recvClusterCount)) {
         Atomic_Or(userIf->actPtr, userIf->actMask);
      }
   }
   wake_up(&userIf->waitQueue);
   return;
   
 drop_packet:
   dev_kfree_skb(skb);
}


/*
 *----------------------------------------------------------------------
 *
 * VNetUserIfProcRead/VNetUserIfProcShow --
 *
 *      Callback for read operation on this userif entry in vnets proc fs.
 *
 * Results: 
 *      Length of read operation.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
VNetUserIfProcShow(struct seq_file    *seqf,  // IN/OUT: buffer to write into
#else
VNetUserIfProcRead(char    *page,  // IN/OUT: buffer to write into
                   char   **start, // OUT: 0 if file < 4k, else offset into
                                   //      page
                   off_t    off,   // IN: offset of read into the file
                   int      count, // IN: maximum number of bytes to read
                   int     *eof,   // OUT: TRUE if there is nothing more to
                                   //      read
#endif
                   void    *data)  // IN: client data - not used
{
   VNetUserIF *userIf = (VNetUserIF*)data; 
   int len = 0;
   
   if (!userIf) {
      return len;
   }
   
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
   VNetPrintPort(&userIf->port, seqf);
#else
   len += VNetPrintPort(&userIf->port, page+len);
#endif
   
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
   seq_printf(seqf, "read %u written %u queued %u ",
#else
   len += sprintf(page+len, "read %u written %u queued %u ",
#endif
                  userIf->stats.read,
                  userIf->stats.written,
                  userIf->stats.queued);
   
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
   seq_printf(seqf,
#else
   len += sprintf(page+len, 
#endif
		  "dropped.down %u dropped.mismatch %u "
		  "dropped.overflow %u dropped.largePacket %u",
                  userIf->stats.droppedDown,
                  userIf->stats.droppedMismatch,
                  userIf->stats.droppedOverflow,
		  userIf->stats.droppedLargePacket);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
   seq_printf(seqf, "\n");
   return 0;
#else
   len += sprintf(page+len, "\n");
   
   *start = 0;
   *eof   = 1;
   return len;
#endif
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
static int proc_userif_open(struct inode *inode, struct file *file)
{
       return single_open(file, VNetUserIfProcShow, PDE_DATA(inode));
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops proc_userif_fops = {
       .proc_open           = proc_userif_open,
       .proc_read           = seq_read,
       .proc_lseek          = seq_lseek,
       .proc_release        = seq_release,
#else
static const struct file_operations proc_userif_fops = {
       .open           = proc_userif_open,
       .read           = seq_read,
       .llseek         = seq_lseek,
       .release        = seq_release,
#endif
};
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 4)
/*
 *----------------------------------------------------------------------
 *
 * VNetCopyDatagram --
 *
 *      Copy part of datagram to userspace.
 *
 * Results: 
 *	zero    on success,
 *	-EFAULT if buffer is an invalid area
 *
 * Side effects:
 *      Data copied to the buffer.
 *
 *----------------------------------------------------------------------
 */

static int
VNetCopyDatagram(const struct sk_buff *skb,	// IN: skb to copy
		 char *buf,			// OUT: where to copy data
		 int len)			// IN: length
{
   struct iovec iov = {
      .iov_base = buf,
      .iov_len  = len,
   };
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
   struct iov_iter to;
   iov_iter_init(&to, READ, &iov, 1, len);
   return skb_copy_datagram_iter(skb, 0, &to, len);
#else
   return skb_copy_datagram_iovec(skb, 0, &iov, len);
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * VNetCsumCopyDatagram --
 *
 *      Copy part of datagram to userspace doing checksum at same time.
 *
 *	Do not mark this function INLINE, it is recursive! With all gcc's 
 *	released up to now (<= gcc-3.3.1) inlining this function just
 *	consumes 120 more bytes of code and goes completely mad on
 *	register allocation, storing almost everything in the memory.
 *
 * Results: 
 *	folded checksum (non-negative value) on success,
 *	-EINVAL if offset is too big,
 *	-EFAULT if buffer is an invalid area
 *
 * Side effects:
 *      Data copied to the buffer.
 *
 *----------------------------------------------------------------------
 */

static int
VNetCsumCopyDatagram(const struct sk_buff *skb,	// IN: skb to copy
		     unsigned int offset,	// IN: how many bytes skip
		     char *buf)			// OUT: where to copy data
{
   unsigned int csum;
   int err = 0;
   int len = skb_headlen(skb) - offset;
   char *curr = buf;
   const skb_frag_t *frag;

   /* 
    * Something bad happened. We skip only up to skb->nh.raw, and skb->nh.raw
    * must be in the header, otherwise we are in the big troubles.
    */
   if (len < 0) {
      return -EINVAL;
   }

   csum = csum_and_copy_to_user(skb->data + offset, curr, len, 0, &err);
   if (err) {
      return err;
   }
   curr += len;

   for (frag = skb_shinfo(skb)->frags;
	frag != skb_shinfo(skb)->frags + skb_shinfo(skb)->nr_frags;
	frag++) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
      if (skb_frag_size(frag) > 0) {
#else
      if (frag->size > 0) {
#endif
	 unsigned int tmpCsum;
	 const void *vaddr;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 2, 0)
	 vaddr = kmap(skb_frag_page(frag));
#else
	 vaddr = kmap(frag->page);
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
	 tmpCsum = csum_and_copy_to_user(vaddr + frag->bv_offset,
					 curr, skb_frag_size(frag), 0, &err);
#else
	 tmpCsum = csum_and_copy_to_user(vaddr + frag->page_offset,
					 curr, frag->size, 0, &err);
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 2, 0)
	 kunmap(skb_frag_page(frag));
#else
	 kunmap(frag->page);
#endif
	 if (err) {
	    return err;
	 }
	 csum = csum_block_add(csum, tmpCsum, curr - buf);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
	 curr += skb_frag_size(frag);
#else
	 curr += frag->size;
#endif
      }
   }

   for (skb = skb_shinfo(skb)->frag_list; skb != NULL; skb = skb->next) {
      int tmpCsum;

      tmpCsum = VNetCsumCopyDatagram(skb, 0, curr);
      if (tmpCsum < 0) {
	 return tmpCsum;
      }
      /* Folded checksum must be inverted before we can use it */
      csum = csum_block_add(csum, tmpCsum ^ 0xFFFF, curr - buf);
      curr += skb->len;
   }
   return csum_fold(csum);
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * VNetCopyDatagramToUser --
 *
 *      Copy complete datagram to the user space. Fill correct checksum
 *	into the copied datagram if nobody did it yet.
 *
 * Results: 
 *      On success byte count, on failure -EFAULT.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static INLINE_SINGLE_CALLER int
VNetCopyDatagramToUser(const struct sk_buff *skb,	// IN
		       char *buf,			// OUT
		       size_t count)			// IN
{
   if (count > skb->len) {
      count = skb->len;
   }
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 4, 4)
   if (copy_to_user(buf, skb->data, count)) {
      return -EFAULT;
   }
#else
   /*
    * If truncation occurs, we do not bother with checksumming - caller cannot
    * verify checksum anyway in such case, and copy without checksum is
    * faster.
    */
   if (skb->pkt_type == PACKET_OUTGOING && 	/* Packet must be outgoing */
       skb->ip_summed == VM_TX_CHECKSUM_PARTIAL &&	/* Without checksum */
       compat_skb_network_header_len(skb) &&    /* We must know where header is */
       skb->len == count) {			/* No truncation may occur */
      size_t skl;
      int csum;
      u_int16_t csum16;
     
      skl = compat_skb_csum_start(skb);
      if (VNetCopyDatagram(skb, buf, skl)) {
	 return -EFAULT;
      }
      csum = VNetCsumCopyDatagram(skb, skl, buf + skl);
      if (csum < 0) {
	 return csum;
      }
      csum16 = csum;
      if (copy_to_user(buf + skl + compat_skb_csum_offset(skb),
                       &csum16, sizeof csum16)) {
	 return -EFAULT;
      }
   } else {
      if (VNetCopyDatagram(skb, buf, count)) {
	 return -EFAULT;
      }
   }
#endif
   return count;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetUserIfRead --
 *
 *      The virtual network's read file operation. Reads the next pending
 *      packet for this network connection.
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

static int 
VNetUserIfRead(VNetPort    *port, // IN
               struct file *filp, // IN
               char        *buf,  // OUT
               size_t      count) // IN
{
   VNetUserIF *userIf = (VNetUserIF*)port->jack.private;
   struct sk_buff *skb;
   int ret;
   DECLARE_WAITQUEUE(wait, current);

   add_wait_queue(&userIf->waitQueue, &wait);
   for (;;) {
      set_current_state(TASK_INTERRUPTIBLE);
      skb = skb_peek(&userIf->packetQueue);
      if (skb && (skb->len > count)) {
         skb = NULL;
         ret = -EMSGSIZE;
         break;
      }
      ret = -EAGAIN;
      skb = skb_dequeue(&userIf->packetQueue);

      if (userIf->pollPtr) {
         if (skb_queue_empty(&userIf->packetQueue)) {
            *userIf->pollPtr &= ~userIf->pollMask;
         }
#if 0
         /*
          * Disable this for now since the monitor likes to assert that
          * actions are present and thus can't cope with them disappearing
          * out from under it.  See bug 47760.  -Jeremy. 22 July 2004
          */

         if (skb_queue_len(&userIf->packetQueue) < (*userIf->recvClusterCount) &&
             (Atomic_Read(userIf->actPtr) & userIf->actMask) != 0) {
            Atomic_And(userIf->actPtr, ~userIf->actMask);
         }
#endif
      }

      if (skb != NULL || filp->f_flags & O_NONBLOCK) {
         break;
      }
      ret = -EINTR;
      if (signal_pending(current)) {
         break;
      }
      schedule();
   }
   __set_current_state(TASK_RUNNING);
   remove_wait_queue(&userIf->waitQueue, &wait);
   if (! skb) {
      return ret;
   }

   userIf->stats.read++;

   count = VNetCopyDatagramToUser(skb, buf, count);
   dev_kfree_skb(skb);
   return count;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetUserIfWrite --
 *
 *      The virtual network's write file operation. Send the raw packet
 *      to the network.
 *
 * Results: 
 *      On success the count of bytes written else errno.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int 
VNetUserIfWrite(VNetPort    *port, // IN
                struct file *filp, // IN
                const char  *buf,  // IN
                size_t      count) // IN
{
   VNetUserIF *userIf = (VNetUserIF*)port->jack.private;
   struct sk_buff *skb;

   /*
    * Check size
    */
   
   if (count < sizeof (struct ethhdr) || 
       count > ETHER_MAX_QUEUED_PACKET) {
      return -EINVAL;
   }

   /*
    * Required to enforce the downWhenAddrMismatch policy in the MAC
    * layer. --hpreg
    */
   if (!UP_AND_RUNNING(userIf->port.flags)) {
      userIf->stats.droppedDown++;
      return count;
   }

   /*
    * Allocate an sk_buff.
    */
   
   skb = dev_alloc_skb(count + 7);
   if (skb == NULL) {
      // XXX obey O_NONBLOCK?
      return -ENOBUFS;
   }
   
   skb_reserve(skb, 2);
   
   /*
    * Copy the data and send it.
    */
   
   userIf->stats.written++;
   if (copy_from_user(skb_put(skb, count), buf, count)) {
      dev_kfree_skb(skb);
      return -EFAULT;
   }
   
   VNetSend(&userIf->port.jack, skb);

   return count;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VNetUserIfIoctl --
 *
 *      XXX
 *
 * Results: 
 *      0 on success
 *      -errno on failure
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static int
VNetUserIfIoctl(VNetPort      *port,  // IN
                struct file   *filp,  // IN
                unsigned int   iocmd, // IN
                unsigned long  ioarg) // IN or OUT depending on iocmd
{
   VNetUserIF *userIf = (VNetUserIF*)port->jack.private;

   switch (iocmd) {
   case SIOCSETNOTIFY:
      return -EINVAL;
   case SIOCSETNOTIFY2:
#ifdef VMX86_SERVER
      /* 
       * This ioctl always return failure on ESX since we cannot map pages into 
       * the console os that are from the VMKernel address space which  was the
       * only case we used this.
       */
      return -EINVAL;
#else // VMX86_SERVER
   /*
    * ORs pollMask into the integer pointed to by ptr if pending packet. Is
    * cleared when all packets are drained.
    */
   {
      int retval;
      VNet_Notify vn;

      if (copy_from_user(&vn, (void *)ioarg, sizeof vn)) {
         return -EFAULT;
      }

      if (vn.version != 3) {
         return -EINVAL;
      }

      retval = VNetUserIfSetupNotify(userIf, &vn);
      if (retval < 0) {
         return retval;
      }

      break;
   }
#endif // VMX86_SERVER
   case SIOCUNSETNOTIFY:
      if (!userIf->pollPtr) {
	 /* This should always happen on ESX. */
         return -EINVAL;
      }
      VNetUserIfUnsetupNotify(userIf);
      break;

   case SIOCSIFFLAGS:
      /* 
       * Drain queue when interface is no longer active. We drain the queue to 
       * avoid having old packets delivered to the guest when reneabled.
       */
      
      if (!UP_AND_RUNNING(userIf->port.flags)) {
         struct sk_buff *skb;
         
         while ((skb = skb_dequeue(&userIf->packetQueue)) != NULL) {
            dev_kfree_skb(skb);
         }
         
         if (userIf->pollPtr) {
            /* Clear the pending bit as no packets are pending at this point. */
            *userIf->pollPtr &= ~userIf->pollMask;
         }
      }
      break;

   default:
      return -ENOIOCTLCMD;
      break;
   }
   
   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetUserIfPoll --
 *
 *      The virtual network's file poll operation.
 *
 * Results: 
 *      Return POLLIN if success, else sleep and return 0.
 *      FIXME: Should not we always return POLLOUT?
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
VNetUserIfPoll(VNetPort     *port, // IN
               struct file  *filp, // IN
               poll_table   *wait) // IN
{
   VNetUserIF *userIf = (VNetUserIF*)port->jack.private;
   
   poll_wait(filp, &userIf->waitQueue, wait);
   if (!skb_queue_empty(&userIf->packetQueue)) {
      return POLLIN;
   }

   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetUserIf_Create --
 *
 *      Create a user level port to the wonderful world of virtual
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
VNetUserIf_Create(VNetPort **ret) // OUT
{
   VNetUserIF *userIf;
   static unsigned id = 0;
   int retval;
   
   userIf = kmalloc(sizeof *userIf, GFP_USER);
   if (!userIf) {
      return -ENOMEM;
   }

   /*
    * Initialize fields.
    */
   
   userIf->port.id = id++;

   userIf->port.jack.peer = NULL;
   userIf->port.jack.numPorts = 1;
   VNetSnprintf(userIf->port.jack.name, sizeof userIf->port.jack.name,
		"userif%u", userIf->port.id);
   userIf->port.jack.private = userIf;
   userIf->port.jack.index = 0;
   userIf->port.jack.procEntry = NULL;
   userIf->port.jack.free = VNetUserIfFree;
   userIf->port.jack.rcv = VNetUserIfReceive;
   userIf->port.jack.cycleDetect = NULL;
   userIf->port.jack.portsChanged = NULL;
   userIf->port.jack.isBridged = NULL;
   userIf->pollPtr = NULL;
   userIf->actPtr = NULL;
   userIf->recvClusterCount = NULL;
   userIf->pollPage = NULL;
   userIf->actPage = NULL;
   userIf->recvClusterPage = NULL;
   userIf->pollMask = userIf->actMask = 0;

   /*
    * Make proc entry for this jack.
    */
   
   retval = VNetProc_MakeEntry(userIf->port.jack.name, S_IFREG,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
                               &userIf->port.jack.procEntry, &proc_userif_fops, userIf);
#else
                               &userIf->port.jack.procEntry);
#endif
   if (retval) {
      if (retval == -ENXIO) {
         userIf->port.jack.procEntry = NULL;
      } else {
         kfree(userIf);
         return retval;
      }
   } else {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
      userIf->port.jack.procEntry->read_proc = VNetUserIfProcRead;
      userIf->port.jack.procEntry->data = userIf;
#endif
   }

   /*
    * Rest of fields.
    */
   
   userIf->port.flags = IFF_RUNNING;

   memset(userIf->port.paddr, 0, sizeof userIf->port.paddr);
   memset(userIf->port.ladrf, 0, sizeof userIf->port.ladrf);

   VNet_MakeMACAddress(&userIf->port);

   userIf->port.fileOpRead = VNetUserIfRead;
   userIf->port.fileOpWrite = VNetUserIfWrite;
   userIf->port.fileOpIoctl = VNetUserIfIoctl;
   userIf->port.fileOpPoll = VNetUserIfPoll;
   
   skb_queue_head_init(&(userIf->packetQueue));
   init_waitqueue_head(&userIf->waitQueue);

   memset(&userIf->stats, 0, sizeof userIf->stats);
   
   *ret = (VNetPort*)userIf;
   return 0;
}
