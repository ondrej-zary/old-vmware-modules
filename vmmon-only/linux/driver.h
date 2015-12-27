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

#ifndef __DRIVER_H__
#define __DRIVER_H__

#define INCLUDE_ALLOW_VMMON
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

#include <linux/sched.h>
#include <linux/miscdevice.h>

#include "vmx86.h"
#include "vm_time.h"
#include "compat_version.h"
#include "compat_spinlock.h"
#include "compat_sched.h"
#include "compat_wait.h"
#include "compat_mutex.h"
#include "driver_vmcore.h"


/*
 * Per-instance driver state
 */

struct VMDriver;

/* 16 pages (64KB) looks as a good limit for one allocation */
#define VMMON_MAX_LOWMEM_PAGES	16

typedef struct VMLinux {
   struct VMLinux *next;
   struct VMDriver *vm;

   /*
    * The semaphore protect accesses to size4Gb and pages4Gb
    * in mmap(). mmap() may happen only once, and all other
    * accesses except cleanup are read-only, and may happen
    * only after successful mmap.
    */
   struct semaphore lock4Gb;
   unsigned int size4Gb;
   struct page *pages4Gb[VMMON_MAX_LOWMEM_PAGES];

   /*
    * LinuxDriverPoll() support
    */

   wait_queue_head_t pollQueue;
   volatile uint32 *pollTimeoutPtr;
   struct page *pollTimeoutPage;
   VmTimeType pollTime;
   struct VMLinux *pollForw;
   struct VMLinux **pollBack;

#ifdef CONFIG_IOMMU_API
   struct iommu_domain *iommuDomain;
   uint64 numPages;
#endif
} VMLinux;


/*
 * Static driver state.
 */

#define VM_DEVICE_NAME_SIZE 32
#define LINUXLOG_BUFFER_SIZE  1024

typedef struct VMXLinuxState {
   int major;
   int minor;
   struct miscdevice misc;
   VmTimeStart startTime;	/* Used to compute kHz estimate */
   char deviceName[VM_DEVICE_NAME_SIZE];
   char buf[LINUXLOG_BUFFER_SIZE];
   VMLinux *head;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 36)
   compat_mutex_t lock;
#endif

   /*
    * for LinuxDriverPoll()
    */

   struct timer_list pollTimer;
   wait_queue_head_t pollQueue;

   struct VMLinux *pollList;
#ifdef POLLSPINLOCK
   spinlock_t pollListLock;
#endif

   struct task_struct *fastClockThread;
   unsigned fastClockRate;
   long fastClockPriority;
   uint64 swapSize;
} VMXLinuxState;

extern VMXLinuxState linuxState;

#endif
