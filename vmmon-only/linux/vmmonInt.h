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

#ifndef __VMMONINT_H__
#define __VMMONINT_H__

#define INCLUDE_ALLOW_VMMON
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"


/*
 * Hide all kernel compatibility stuff in these macros and functions.
 */

#ifdef VMW_HAVE_SMP_CALL_3ARG
#define compat_smp_call_function(fn, info, wait) smp_call_function(fn, info, wait)
#else
#define compat_smp_call_function(fn, info, wait) smp_call_function(fn, info, 1, wait)
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
#define compat_num_online_cpus() num_online_cpus()
#else
#define compat_num_online_cpus() smp_num_cpus
#endif


/*
 * Although this is not really related to kernel-compatibility, I put this
 * helper macro here for now for a lack of better place --hpreg
 *
 * The exit(2) path does, in this order:
 * . set current->files to NULL
 * . close all fds, which potentially calls LinuxDriver_Close()
 *
 * fget() requires current->files != NULL, so we must explicitely check --hpreg
 */
#define vmware_fget(_fd) (current->files ? fget(_fd) : NULL)

extern void LinuxDriverWakeUp(Bool selective);

#endif /* __VMMONINT_H__ */
