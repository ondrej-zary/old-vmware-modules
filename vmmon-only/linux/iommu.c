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
 * iommu.c --
 *
 *    This file consist of functions to implement PCI passthru functionality
 *    for hosted architecture, using drivers/base/iommu.c functionality.
 */


#include "driver-config.h"
#include "iommu.h"
#include "compat_sched.h"
#include "driver.h"
#include "hostif.h"


#ifdef HOSTED_IOMMU_SUPPORT

#define PCI_BDF_SLOTFUNC(bdf) PCI_DEVFN(PCI_SLOT(bdf), PCI_FUNC(bdf))
#define PCI_BDF_BUS(bdf)      (((bdf) >> 8) & 0xff)

typedef struct PassthruDevice {
   struct pci_dev *pdev;
   VMLinux *vmLinux;
   struct list_head list;
} PassthruDevice;


static LIST_HEAD(passthruDeviceList);
static spinlock_t passthruDeviceListLock = SPIN_LOCK_UNLOCKED;
static void *pciHolePage = NULL;

/*
 *----------------------------------------------------------------------------
 *
 * IOMMU_SetupMMU --
 *
 *      Maps entire VM's memory into IOMMU domain, the VM physical addresses
 *      mapped one to one into iommu domain address space.
 *
 * Results:
 *      0 on success.
 *      errno on failures.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------------
 */

int
IOMMU_SetupMMU(VMLinux *vmLinux,               // IN: virtual machine descriptor
               PassthruIOMMUMap *ioarg)       // IN: Guest's MPN/PPN map pointer
{
   int status = 0;
   uint64 currentPage = ~0;
   PPN ppn;
   MPN *data = NULL;

   printk(KERN_INFO "%s: setting up IOMMU...\n", __func__);

   if (vmLinux->iommuDomain)  {
      printk(KERN_ERR "%s: IOMMU domain already exists.\n", __func__);
      return -EBUSY;
   }
   if (!(vmLinux->iommuDomain = iommu_domain_alloc())) {
      printk(KERN_ERR "%s: IOMMU domain could not be allocated.\n", __func__);
      return -ENODEV;
   }
   if (copy_from_user(&vmLinux->numPages, &ioarg->numPages,
                      sizeof vmLinux->numPages) != 0) {
      printk(KERN_ERR "%s: could not get number of MPNs from user space.\n",
             __func__);
      status = -EFAULT;
      goto out;
   }
   printk(KERN_INFO "%s: user space requested %"FMT64"u pages\n", __func__,
          vmLinux->numPages);
   if (!(data = HostIF_AllocKernelMem(PAGE_SIZE, FALSE))) {
       printk(KERN_ERR "%s: temporary page could not be allocated.\n",
              __func__);
       status = -ENOMEM;
       goto out;
   }
   for (ppn = 0; ppn < vmLinux->numPages; ppn++) {
      uint64 userAddress = (uint64) &ioarg->mpn[ppn];
      uint64 userPage = userAddress >> PAGE_SHIFT;
      MPN mpn;
      uint64 pageIndex = (userAddress & (PAGE_SIZE - 1)) / sizeof(mpn);
      phys_addr_t map_to;
      int map_prot;

      if (iommu_iova_to_phys(vmLinux->iommuDomain, PPN_2_PA(ppn)) != 0) {
         printk(KERN_WARNING
                "%s: Mapping for IOVA %lx is already exists, skipping...\n",
                __func__, PPN_2_PA(ppn));
         continue; // This page is already mapped
      }
      if (currentPage != userPage) {
         if (copy_from_user(data, (void *)(userPage << PAGE_SHIFT),
                            PAGE_SIZE) != 0) {
            printk(KERN_ERR "%s: could not get %luth page of IOMMU map "
                   "from user space.\n", __func__, userPage);
            status = -EFAULT;
            goto out;
         }
         currentPage = userPage;
      }
      mpn = data[pageIndex];
      if (mpn == INVALID_MPN) {
         /*
          * The vmx is going to specify INVALID_MPN as the mpn if
          * the corresponding ppn isn't backed by main memory.
          */
         if (!pciHolePage) {
            pciHolePage = HostIF_AllocKernelMem(PAGE_SIZE, FALSE);

            if (!pciHolePage) {
               printk(KERN_ERR "%s: kmalloc failure. "
                      "Device could not be registered due lack of memory "
                      "in the system.\n",
                      __func__);
               return -ENOMEM;
            }
            memset(pciHolePage, 0xff, PAGE_SIZE);
         }
         map_to = virt_to_phys(pciHolePage);
         map_prot = IOMMU_READ;
      } else {
         if (!pfn_valid(mpn)) {
            printk(KERN_ERR "%s: the physical page number 0x%x is not valid.\n",
                   __func__, mpn);
            status = -EINVAL;
            goto out;
         }
         map_to =  PPN_2_PA(mpn);
         map_prot = IOMMU_READ | IOMMU_WRITE;
      }
      if ((status = iommu_map(vmLinux->iommuDomain,
                              PPN_2_PA(ppn), map_to,
                              get_order(PAGE_SIZE),
                              map_prot))) {
         printk(KERN_ERR "%s: IOMMU Mapping of PPN 0x%x -> MPN 0x%x "
                "could not be established.\n", __func__, ppn, mpn);
         goto out;
      }
   }
   printk(KERN_DEBUG "%s: IOMMU domain is created.\n", __func__);

out:

   if (status != 0 && vmLinux->iommuDomain) {
      iommu_domain_free(vmLinux->iommuDomain);
      vmLinux->iommuDomain = NULL;
   }
   HostIF_FreeKernelMem(data);
   return status;
}


/*
 *----------------------------------------------------------------------------
 *
 * IOMMUUnregisterDeviceInt --
 *
 *      The internal function, used to unregister device from iommu domain,
 *      once unregistred, the device's DMA request will no longer be directed
 *      to VM's address space.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------------
 */

static void
IOMMUUnregisterDeviceInt(PassthruDevice *passthruDevice)// IN: The device
                                                        //     to unregister
{
   int error;

   ASSERT(passthruDevice->pdev);
   iommu_detach_device(passthruDevice->vmLinux->iommuDomain,
                       &passthruDevice->pdev->dev);
   pci_release_regions(passthruDevice->pdev);
   pci_disable_device(passthruDevice->pdev);

   /*
    * At this point the guest driver is no longer driving the device and
    * the host believes the device is disabled. Issue a function reset to
    * clear out any remaining device state, then return control of the
    * device back to the host.
    *
    * XXX: The docs say to hold dev->parent->sem if calling on a USB
    * interface. Given that we're dealing with PCI devices, is this
    * necessary?
    */
   pci_reset_function(passthruDevice->pdev);
   if ((error = device_attach(&passthruDevice->pdev->dev)) != 1) {
      /* Can't do much, just log and move on. */
      printk(KERN_ERR "%s: device_attach failed on %s, error %d.\n",
	     __func__, pci_name(passthruDevice->pdev), error);
   }

   printk(KERN_DEBUG "%s: Device %s is detached from IOMMU domain.\n",
          __func__, pci_name(passthruDevice->pdev));

   pci_dev_put(passthruDevice->pdev);
   HostIF_FreeKernelMem(passthruDevice);
}


/*
 *----------------------------------------------------------------------------
 *
 * IOMMU_RegisterDevice --
 *
 *      Registers device into IOMMU domain, means, that once registered,
 *      the device's DMA(s) will be redirected according to IOMMU table context,
 *      what maps entire VM address space into IOMMU domain one to one.
 *
 * Results:
 *      0 on success,
 *      errno on failure.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------------
 */

int
IOMMU_RegisterDevice(VMLinux *vmLinux, // IN: virtual machine descriptor
                     uint32 bdf)       // IN: Block/Device/Function
{
   struct PassthruDevice *passthruDevice = NULL;
   int status = 0;

   printk(KERN_INFO "%s: Registering PCI device for IOMMU\n", __func__);

   if (!vmLinux->iommuDomain) {
      printk(KERN_ERR "%s: No IOMMU domain to assign device to.\n", __func__);
      return -ENOENT;
   }
   if (!(passthruDevice =
         HostIF_AllocKernelMem(sizeof *passthruDevice, FALSE))) {
      printk(KERN_ERR "%s: kmalloc failure. "
             "Device could not be registered due lack of memory "
             "in the system.\n",
             __func__);
      return -ENOMEM;
   }
   passthruDevice->vmLinux = vmLinux;
   passthruDevice->pdev = pci_get_bus_and_slot(PCI_BDF_BUS(bdf),
                                               PCI_BDF_SLOTFUNC(bdf));
   if (!passthruDevice->pdev) {
      printk(KERN_ERR "%s: No device found (bdf=%x).\n", __func__, bdf);
      status = -ENODEV;
      goto exitNoPCI;
   }

   /*
    * Before setting up the PCI device for passthru, take it away from its
    * host kernel driver. Note that we also issue a function reset on the
    * device just in case the host driver didn't fully deinitialize the device.
    */
   device_release_driver(&passthruDevice->pdev->dev);
   pci_reset_function(passthruDevice->pdev);

   if ((status = pci_enable_device(passthruDevice->pdev))) {
      printk(KERN_ERR "%s: Could not enable PCI device. %s\n", __func__,
             pci_name(passthruDevice->pdev));
      goto exitNoEnable;
   }
   if ((status = pci_request_regions(passthruDevice->pdev,
                                     "vmware/passthru device"))) {
      printk(KERN_ERR "%s: Failed to reserve PCI regions for %s\n", __func__,
             pci_name(passthruDevice->pdev));
      goto exitNoRegions;
   }
   if ((status = iommu_attach_device(vmLinux->iommuDomain,
                                     &passthruDevice->pdev->dev))) {
      printk(KERN_ERR "%s: Attaching device failed for  %s\n", __func__,
             pci_name(passthruDevice->pdev));
      goto exitNoIOMMU;
   }
   spin_lock(&passthruDeviceListLock);
   list_add(&passthruDevice->list, &passthruDeviceList);
   spin_unlock(&passthruDeviceListLock);

   printk(KERN_INFO "%s: Device %s is successfully attached to IOMMU domain "
          "for passthru.\n",
          __func__, pci_name(passthruDevice->pdev));
   return 0;

  exitNoIOMMU:
   pci_release_regions(passthruDevice->pdev);
  exitNoRegions:
   pci_disable_device(passthruDevice->pdev);
  exitNoEnable:
   pci_dev_put(passthruDevice->pdev);
  exitNoPCI:
   HostIF_FreeKernelMem(passthruDevice);
   return status;
}


/*
 *----------------------------------------------------------------------------
 *
 * IOMMU_UnregisterDevice --
 *
 *      Destroy IOMMU context for device. Device became unmapped.
 *
 * Results:
 *      0 on success.
*       errno on failure.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------------
 */

int
IOMMU_UnregisterDevice(uint32 bdf) // IN: Block/Device/Function
{
   struct PassthruDevice *passthruDevice;
   struct pci_dev *pdev;

   if (!(pdev = pci_get_bus_and_slot(PCI_BDF_BUS(bdf),
                                     PCI_BDF_SLOTFUNC(bdf)))) {
      printk(KERN_ERR "%s: No device found (bdf=%x).\n", __func__, bdf);
      return -ENOENT;
   }
   printk(KERN_INFO "%s: Unregistering PCI device %s for IOMMU\n", __func__,
          pci_name(pdev));

   spin_lock(&passthruDeviceListLock);
   list_for_each_entry(passthruDevice, &passthruDeviceList, list) {
      if (pdev == passthruDevice->pdev) {
         list_del(&passthruDevice->list);
         spin_unlock(&passthruDeviceListLock);
         IOMMUUnregisterDeviceInt(passthruDevice);
         pci_dev_put(pdev);
         return 0;
      }
   }
   spin_unlock(&passthruDeviceListLock);
   pci_dev_put(pdev);
   return -ENOENT;
}


/*
 *----------------------------------------------------------------------------
 *
 * IOMMU_VMCleanup --
 *
 *      Detach each device from this VM's IOMMU domain and free the domain
 *      itself.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      All detached devices will be reinitialized for use by the host kernel.
 *
 *----------------------------------------------------------------------------
 */

void
IOMMU_VMCleanup(VMLinux *vmLinux)  // IN: virtual machine descriptor
{
   struct PassthruDevice *passthruDevice, *tmp;
   PPN ppn;

   /* Unregister each device being passed through to this VM. */
   spin_lock(&passthruDeviceListLock);
   list_for_each_entry_safe(passthruDevice, tmp, &passthruDeviceList, list) {
      if (passthruDevice->vmLinux == vmLinux) {
         list_del(&passthruDevice->list);
         IOMMUUnregisterDeviceInt(passthruDevice);
      }
   }
   spin_unlock(&passthruDeviceListLock);

   /* Relinquish the IOMMU domain used by this VM. */
   for (ppn = 0; ppn < vmLinux->numPages; ppn++) {
      iommu_unmap(vmLinux->iommuDomain, PPN_2_PA(ppn), get_order(PAGE_SIZE));
   }
   if (vmLinux->iommuDomain) {
      iommu_domain_free(vmLinux->iommuDomain);
      vmLinux->iommuDomain = NULL;
      printk(KERN_INFO "%s: IOMMU domain is destroyed.\n", __func__);
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * IOMMU_ModuleCleanup --
 *
 *      Clean up IOMMU module internals on vmmon unload.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

void
IOMMU_ModuleCleanup(void)
{
   if (pciHolePage) {
      HostIF_FreeKernelMem(pciHolePage);
   }
}
#endif

