/*********************************************************
 * Copyright (C) 2005 VMware, Inc. All rights reserved.
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
 * Detect whether sk_alloc takes a struct proto * as third parameter.
 * This API change was introduced between 2.6.12-rc1 and 2.6.12-rc2.
 */

#include "compat_version.h"
#include "compat_autoconf.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 13)
#include <net/sock.h>

static struct proto test_proto = {
   .name     = "TEST",
};

struct sock * 
vmware_sk_alloc(void)
{
   return sk_alloc(PF_NETLINK, 0, &test_proto, 1);
}
#endif
