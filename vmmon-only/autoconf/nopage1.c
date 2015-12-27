/*********************************************************
 * Copyright (C) 2004 VMware, Inc. All rights reserved.
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
 * nopage prototype changed in 2.6.1.  For 2.6.2 and newer assume
 * it uses new prototype.  For 2.6.1 (and its -rc) and older do
 * compile test.
 */
#include "compat_version.h"
#include "compat_autoconf.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 2)
#include <linux/mm.h>
#include <linux/stddef.h> /* NULL */

static struct page *LinuxDriverNoPage(struct vm_area_struct *vma,
                           unsigned long address, int *type) {
	(void)vma;
	(void)address;
	*type = VM_FAULT_MAJOR;
	return NULL;
}

struct vm_operations_struct vmuser_mops = {
        .nopage = LinuxDriverNoPage
};
#endif
