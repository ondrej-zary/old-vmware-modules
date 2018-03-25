/*********************************************************
 * Copyright (C) 2006 VMware, Inc. All rights reserved.
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
 * inode.c --
 *
 *   Inode operations for the file system of the vmblock driver.
 *
 */

#include "driver-config.h"
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <linux/namei.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0)
#include <linux/uaccess.h>
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
#include <linux/iversion.h>
#endif

#include "vmblockInt.h"
#include "filesystem.h"
#include "block.h"


/* Inode operations */
static struct dentry *InodeOpLookup(struct inode *dir,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0)
                                    struct dentry *dentry, unsigned int flags);
#else
                                    struct dentry *dentry, struct nameidata *nd);
#endif
static int InodeOpReadlink(struct dentry *dentry, char __user *buffer, int buflen);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)
static const char *InodeOpGetlink(struct dentry *dentry, struct inode *inode, struct delayed_call *dc);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
static const char *InodeOpFollowlink(struct dentry *dentry, void **cookie);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 13)
static void *InodeOpFollowlink(struct dentry *dentry, struct nameidata *nd);
#else
static int InodeOpFollowlink(struct dentry *dentry, struct nameidata *nd);
#endif


struct inode_operations RootInodeOps = {
   .lookup = InodeOpLookup,
};

static struct inode_operations LinkInodeOps = {
   .readlink    = InodeOpReadlink,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)
   .get_link    = InodeOpGetlink,
#else
   .follow_link = InodeOpFollowlink,
#endif
};


/*
 *----------------------------------------------------------------------------
 *
 * InodeOpLookup --
 *
 *    Looks up a name (dentry) in provided directory.  Invoked every time
 *    a directory entry is traversed in path lookups.
 *
 * Results:
 *    NULL on success, negative error code on error.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static struct dentry *
InodeOpLookup(struct inode *dir,      // IN: parent directory's inode
              struct dentry *dentry,  // IN: dentry to lookup
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0)
              unsigned int flags)
#else
              struct nameidata *nd)   // IN: lookup intent and information
#endif
{
   char *filename;
   struct inode *inode;
   int ret;

   if (!dir || !dentry) {
      Warning("InodeOpLookup: invalid args from kernel\n");
      return ERR_PTR(-EINVAL);
   }

   /* The kernel should only pass us our own inodes, but check just to be safe. */
   if (!INODE_TO_IINFO(dir)) {
      Warning("InodeOpLookup: invalid inode provided\n");
      return ERR_PTR(-EINVAL);
   }

   /* Get a slab from the kernel's names_cache of PATH_MAX-sized buffers. */
   filename = __getname();
   if (!filename) {
      Warning("InodeOpLookup: unable to obtain memory for filename.\n");
      return ERR_PTR(-ENOMEM);
   }

   ret = MakeFullName(dir, dentry, filename, PATH_MAX);
   if (ret < 0) {
      Warning("InodeOpLookup: could not construct full name\n");
      __putname(filename);
      return ERR_PTR(ret);
   }

   /* Block if there is a pending block on this file */
   BlockWaitOnFile(filename, NULL);
   __putname(filename);

   inode = Iget(dir->i_sb, dir, dentry, GetNextIno());
   if (!inode) {
      Warning("InodeOpLookup: failed to get inode\n");
      return ERR_PTR(-ENOMEM);
   }

   dentry->d_op = &LinkDentryOps;
   dentry->d_time = jiffies;

   /*
    * If the actual file's dentry doesn't have an inode, it means the file we
    * are redirecting to doesn't exist.  Give back the inode that was created
    * for this and add a NULL dentry->inode entry in the dcache.  (The NULL
    * entry is added so ops to create files/directories are invoked by VFS.)
    */
   if (!INODE_TO_ACTUALDENTRY(inode) || !INODE_TO_ACTUALINODE(inode)) {
      iput(inode);
      d_add(dentry, NULL);
      return NULL;
   }

   inode->i_mode = S_IFLNK | S_IRWXUGO;
   inode->i_size = INODE_TO_IINFO(inode)->nameLen;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
   inode_set_iversion_raw(inode, 1);
#else
   inode->i_version = 1;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)
   inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
#else
   inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0)
   inode->i_uid.val = inode->i_gid.val = 0;
#else
   inode->i_uid = inode->i_gid = 0;
#endif
   inode->i_op = &LinkInodeOps;

   d_add(dentry, inode);
   return NULL;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0)
int readlink_copy(char __user *buffer, int buflen, const char *link)
{
   int len = PTR_ERR(link);
   if (IS_ERR(link))
      goto out;

   len = strlen(link);
   if (len > (unsigned) buflen)
      len = buflen;
   if (copy_to_user(buffer, link, len))
      len = -EFAULT;
out:
   return len;
}
#endif

/*
 *----------------------------------------------------------------------------
 *
 * InodeOpReadlink --
 *
 *    Provides the symbolic link's contents to the user.  Invoked when
 *    readlink(2) is invoked on our symlinks.
 *
 * Results:
 *    0 on success, negative error code on failure.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
InodeOpReadlink(struct dentry *dentry,  // IN : dentry of symlink
                char __user *buffer,    // OUT: output buffer (user space)
                int buflen)             // IN : length of output buffer
{
   VMBlockInodeInfo *iinfo;

   if (!dentry || !buffer) {
      Warning("InodeOpReadlink: invalid args from kernel\n");
      return -EINVAL;
   }

   iinfo = INODE_TO_IINFO(dentry->d_inode);
   if (!iinfo) {
      return -EINVAL;
   }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 15, 0)
   return readlink_copy(buffer, buflen, iinfo->name);
#else
   return vfs_readlink(dentry, buffer, buflen, iinfo->name);
#endif
}


/*
 *----------------------------------------------------------------------------
 *
 * InodeOpFollowlink --
 *
 *    Provides the inode corresponding to this symlink through the nameidata
 *    structure.
 *
 * Results:
 *    0 on success, negative error on failure.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
static const char *
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 13)
static void *
#else
static int
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)
InodeOpGetlink(struct dentry *dentry,  // IN : dentry of symlink
#else
InodeOpFollowlink(struct dentry *dentry,  // IN : dentry of symlink
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)
		  struct inode *inode,
		  struct delayed_call *dc)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
		  void **cookie)          // OUT: stores result
#else
                  struct nameidata *nd)   // OUT: stores result
#endif
{
   int ret = 0;
   VMBlockInodeInfo *iinfo;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)
   if (!dentry && !inode) {
#else
   if (!dentry) {
#endif
      Warning("InodeOpReadlink: invalid args from kernel\n");
      ret = -EINVAL;
      goto out;
   }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)
   if (inode)
      iinfo = INODE_TO_IINFO(inode);
   else
#endif
   iinfo = INODE_TO_IINFO(dentry->d_inode);
   if (!iinfo) {
      ret = -EINVAL;
      goto out;
   }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)
   return (char *)(iinfo->name);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
   return *cookie = (char *)(iinfo->name);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 12, 0)
   nd_set_link(nd, iinfo->name);
#else
   ret = vfs_follow_link(nd, iinfo->name);
#endif

out:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 13)
   return ERR_PTR(ret);
#else
   return ret;
#endif
}
