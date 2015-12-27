/*********************************************************
 * Copyright (C) 2009 VMware, Inc. All rights reserved.
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
 * iommu.h --
 *
 *      This file defines interface for vmmon to manage
 *      IOMMU mapping for PCI passthru devices.
 */

#ifndef _IOMMU_H_
#define _IOMMU_H_

#define INCLUDE_ALLOW_VMMON

#include "driver-config.h"
#include "compat_pci.h"
#include "compat_list.h"

/*
 * Unfortunately, we do depend on HOSTED_COW_DIS*BLED (misspell it here,
 * otherwise grep in make/mk/desktop-vmmontar.make will fail), then it is defined
 * vmmon gets built with GPL license (otherwise Proprietary), only in such case
 * we could use GPL symbols of iommu support in iommu.c: such as
 * iommu_domain_alloc, iommu_domain_free, etc...
 */


#ifdef CONFIG_IOMMU_API
#define HOSTED_IOMMU_SUPPORT
#include <linux/iommu.h>

#include "driver.h"  /* for VMLinux */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35)
/* Kernels up to 2.6.34 had iommu_[un]map_range. */
static inline int
iommu_map(struct iommu_domain *domain, unsigned long iova,
          phys_addr_t paddr, int gfp_order, int prot)
{
   return iommu_map_range(domain, iova, paddr, PAGE_SIZE << gfp_order, prot);
}

static inline void
iommu_unmap(struct iommu_domain *domain, unsigned long iova, int gfp_order)
{
   iommu_unmap_range(domain, iova, PAGE_SIZE << gfp_order);
}
#endif

extern int IOMMU_SetupMMU(VMLinux *vmLinux,  struct PassthruIOMMUMap *ioarg);
extern int IOMMU_RegisterDevice(VMLinux *vmLinux, uint32 bdf);
extern int IOMMU_UnregisterDevice(uint32 bdf);
extern void IOMMU_VMCleanup(VMLinux *vmLinux);
extern void IOMMU_ModuleCleanup(void);


#endif /* CONFIG_IOMMU_API */
#endif /* _IOMMU_H_ */
