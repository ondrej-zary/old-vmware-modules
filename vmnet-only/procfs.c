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
#include <linux/if_ether.h>
#include <linux/sockios.h>
#include "compat_sock.h"

#define __KERNEL_SYSCALLS__
#include <asm/io.h>

#include <linux/proc_fs.h>
#include <linux/file.h>

#include "vnetInt.h"


#if defined(CONFIG_PROC_FS)

static int VNetProcMakeEntryInt(VNetProcEntry *parent, char *name, int mode,
                                VNetProcEntry **ret);
static void VNetProcRemoveEntryInt(VNetProcEntry *node, VNetProcEntry *parent);

static VNetProcEntry *base = NULL;


/*
 *----------------------------------------------------------------------
 *
 * VNetProc_Init --
 *
 *      Initialize the vnets procfs entries.
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
VNetProc_Init(void)
{
   return VNetProcMakeEntryInt(NULL, "vmnet", S_IFDIR, &base);
}


/*
 *----------------------------------------------------------------------
 *
 * VNetProc_Cleanup --
 *
 *      Cleanup the vnets proc filesystem entries.
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
VNetProc_Cleanup(void)
{
   VNetProcRemoveEntryInt(base, NULL);
   base = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * VNetProcMakeEntryInt --
 *
 *      Make an entry in the vnets proc file system.
 *
 * Results: 
 *      errno. If errno is 0 and ret is non NULL then ret is filled
 *      in with the resulting proc entry.
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VNetProcMakeEntryInt(VNetProcEntry  *parent, // IN:
		     char            *name,  // IN:
		     int              mode,  // IN:
		     VNetProcEntry  **ret)   // OUT:
{
   VNetProcEntry *ent;
   ent = create_proc_entry(name, mode, parent);
   *ret = ent;
   if (!ent)
      return -ENOMEM;
   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetProcRemoveEntryInt --
 *
 *      Remove a previously installed proc entry.
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
VNetProcRemoveEntryInt(VNetProcEntry *node,
                       VNetProcEntry *parent)
{
   if (node) {
      remove_proc_entry(node->name, parent);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * VNetProc_MakeEntry --
 *
 *      Make an entry in the vnets proc file system.
 *
 * Results: 
 *      errno. If errno is 0 and ret is non NULL then ret is filled
 *      in with the resulting proc entry.
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VNetProc_MakeEntry(char            *name,  // IN:
		   int              mode,  // IN:
		   VNetProcEntry  **ret)   // OUT:
{
   return VNetProcMakeEntryInt(base, name, mode, ret);
}


/*
 *----------------------------------------------------------------------
 *
 * VNetProc_RemoveEntry --
 *
 *      Remove a previously installed proc entry.
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
VNetProc_RemoveEntry(VNetProcEntry *node)
{
   VNetProcRemoveEntryInt(node, base);
}


#else /* CONFIG_PROC_FS */


/*
 *----------------------------------------------------------------------
 *
 * VNetProc_Init --
 *
 *      Initialize the vnets procfs entries.
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
VNetProc_Init(void)
{
   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetProc_Cleanup --
 *
 *      Cleanup the vnets proc filesystem entries.
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
VNetProc_Cleanup(void)
{
}


/*
 *----------------------------------------------------------------------
 *
 * VNetProc_MakeEntry --
 *
 *      Make an entry in the vnets proc file system.
 *
 * Results: 
 *      errno. If errno is 0 and ret is non NULL then ret is filled
 *      in with the resulting proc entry.
 *      
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VNetProc_MakeEntry(char            *name,
                   int              mode,
                   VNetProcEntry  **ret)
{
   return -ENXIO;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetProc_RemoveEntry --
 *
 *      Remove a previously installed proc entry.
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
VNetProc_RemoveEntry(VNetProcEntry *parent)
{
}

#endif /* CONFIG_PROC_FS */
