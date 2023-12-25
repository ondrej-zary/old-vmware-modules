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
 * file.c --
 *
 *   File operations for the file system of the vmblock driver.
 *
 */

#include "driver-config.h"
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/pagemap.h>

#include "vmblockInt.h"
#include "filesystem.h"

/* Specifically for our filldir_t callback */
typedef struct FilldirInfo {
   filldir_t filldir;
   void *dirent;
} FilldirInfo;

/* File operations */
static int FileOpOpen(struct inode *inode, struct file *file);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
static int FileOpReaddir(struct file *file, struct dir_context *ctx);
#else
static int FileOpReaddir(struct file *file, void *dirent, filldir_t filldir);
#endif
static int FileOpRelease(struct inode *inode, struct file *file);

/* Local functions */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 11, 0)
#if defined(VMW_FILLDIR_2618)
static int Filldir(void *buf, const char *name, int namelen,
                   loff_t offset, u64 ino, unsigned int d_type);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 9)
static int Filldir(void *buf, const char *name, int namelen,
                   loff_t offset, ino_t ino, unsigned int d_type);
#else
static int Filldir(void *buf, const char *name, int namelen,
                   off_t offset, ino_t ino, unsigned int d_type);
#endif
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
WRAP_DIR_ITER(FileOpReaddir)
#endif

struct file_operations RootFileOps = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
   .iterate_shared = shared_FileOpReaddir,
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
   .iterate = FileOpReaddir,
#else
   .readdir = FileOpReaddir,
#endif
   .open    = FileOpOpen,
   .release = FileOpRelease,
};


/* File operations */

/*
 *----------------------------------------------------------------------------
 *
 * FileOpOpen --
 *
 *    Invoked when open(2) has been called on our root inode.  We get an open
 *    file instance of the actual file that we are providing indirect access
 *    to.
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
FileOpOpen(struct inode *inode,  // IN
           struct file *file)    // IN
{
   VMBlockInodeInfo *iinfo;
   struct file *actualFile;

   if (!inode || !file || !INODE_TO_IINFO(inode)) {
      Warning("FileOpOpen: invalid args from kernel\n");
      return -EINVAL;
   }

   iinfo = INODE_TO_IINFO(inode);

   /*
    * Get an open file for the directory we are redirecting to.  This ensure we
    * can gracefully handle cases where that directory is removed after we are
    * mounted.
    */
   actualFile = filp_open(iinfo->name, file->f_flags, file->f_flags);
   if (IS_ERR(actualFile)) {
      Warning("FileOpOpen: could not open file [%s]\n", iinfo->name);
      file->private_data = NULL;
      return PTR_ERR(actualFile);
   }

   /*
    * If the file opened is the same as the one retrieved for the file then we
    * shouldn't allow the open to happen.  This can only occur if the
    * redirected root directory specified at mount time is the same as where
    * the mount is placed.  Later in FileOpReaddir() we'd call vfs_readdir()
    * and that would try to acquire the inode's semaphore; if the two inodes
    * are the same we'll deadlock.
    */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
   if (actualFile->f_path.dentry && inode == actualFile->f_path.dentry->d_inode) {
#else
   if (actualFile->f_dentry && inode == actualFile->f_dentry->d_inode) {
#endif
      Warning("FileOpOpen: identical inode encountered, open cannot succeed.\n");
      if (filp_close(actualFile, current->files) < 0) {
         Warning("FileOpOpen: unable to close opened file.\n");
      }
      return -EINVAL;
   }

   file->private_data = actualFile;
   return 0;
}


/*
 *----------------------------------------------------------------------------
 *
 * FileOpReaddir --
 *
 *    Invoked when a user invokes getdents(2) or readdir(2) on the root of our
 *    file system.  We perform a readdir on the actual underlying file but
 *    interpose the callback by providing our own Filldir() function.  This
 *    enables us to change dentry types to symlinks.
 *
 * Results:
 *    0 on success, negative error code on error.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
FileOpReaddir(struct file *file,  // IN
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
              struct dir_context *ctx)  // IN
#else
              void *dirent,       // IN
              filldir_t filldir)  // IN
#endif
{
   int ret;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 11, 0)
   FilldirInfo info;
#endif
   struct file *actualFile;

   if (!file) {
      Warning("FileOpReaddir: invalid args from kernel\n");
      return -EINVAL;
   }

   actualFile = file->private_data;
   if (!actualFile) {
      Warning("FileOpReaddir: no actual file found\n");
      return -EINVAL;
   }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
   /* Ricky Wong Yung Fei:
    * Manipulation of pos is now handled internally by iterate_dir().
    */
   ret = iterate_dir(actualFile, ctx);
#else
   info.filldir = filldir;
   info.dirent = dirent;

   actualFile->f_pos = file->f_pos;
   ret = vfs_readdir(actualFile, Filldir, &info);
   file->f_pos = actualFile->f_pos;
#endif

   return ret;
}


/*
 *----------------------------------------------------------------------------
 *
 * FileOpRelease --
 *
 *    Invoked when a user close(2)s the root of our file system.  Here we just
 *    close the actual file we opened in FileOpOpen().
 *
 * Results:
 *    0 on success, negative value on error.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------------
 */

static int
FileOpRelease(struct inode *inode, // IN
              struct file *file)   // IN
{
   int ret;
   struct file *actualFile;

   if (!inode || !file) {
      Warning("FileOpRelease: invalid args from kerel\n");
      return -EINVAL;
   }

   actualFile = file->private_data;
   if (!actualFile) {
      Warning("FileOpRelease: no actual file found\n");
      return -EINVAL;
   }

   ret = filp_close(actualFile, current->files);

   return ret;
}


/* Local functions */

/*
 *----------------------------------------------------------------------------
 *
 * Filldir --
 *
 *    Callback function for readdir that we use in place of the one provided.
 *    This allows us to specify that each dentry is a symlink, but pass through
 *    everything else to the original filldir function.
 *
 * Results:
 *    Original filldir's return value.
 *
 * Side effects:
 *    Directory information gets copied to user's buffer.
 *
 *----------------------------------------------------------------------------
 */

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 11, 0)
#if defined(VMW_FILLDIR_2618)
static int
Filldir(void *buf,              // IN: Dirent buffer passed from FileOpReaddir
        const char *name,       // IN: Dirent name
        int namelen,            // IN: len of dirent's name
        loff_t offset,          // IN: Offset
        u64 ino,                // IN: Inode number of dirent
        unsigned int d_type)    // IN: Type of file
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 9)
static int
Filldir(void *buf,              // IN: Dirent buffer passed from FileOpReaddir
        const char *name,       // IN: Dirent name
        int namelen,            // IN: len of dirent's name
        loff_t offset,          // IN: Offset
        ino_t ino,              // IN: Inode number of dirent
        unsigned int d_type)    // IN: Type of file
#else
static int
Filldir(void *buf,              // IN: Dirent buffer passed from FileOpReaddir
        const char *name,       // IN: Dirent name
        int namelen,            // IN: len of dirent's name
        off_t offset,           // IN: Offset
        ino_t ino,              // IN: Inode number of dirent
        unsigned int d_type)    // IN: Type of file
#endif
{
   FilldirInfo *info = (FilldirInfo *)buf;

   /* Specify DT_LNK regardless */
   return info->filldir(info->dirent, name, namelen, offset, ino, DT_LNK);
}
#endif


