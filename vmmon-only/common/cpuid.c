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

#ifdef linux
/* Must come before any kernel header file --hpreg */
#   include "driver-config.h"

#   include <linux/string.h>
#endif
#ifdef __APPLE__
#   include <string.h> // For strcmp().
#endif

#include "vmware.h"
#include "vm_assert.h"
#include "hostif.h"
#include "cpuid.h"

static CpuidVendors vendor;
static uint32 features;
static uint32 version;


/*
 *-----------------------------------------------------------------------------
 *
 * CPUIDExtendedSupported --
 *
 *     Determine whether processor supports extended CPUID (0x8000xxxx)
 *     and how many of them.
 *
 * Results:
 *     0         if extended CPUID is not supported
 *     otherwise maximum extended CPUID supported (bit 31 set)
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */

static uint32
CPUIDExtendedSupported(void)
{
   uint32 eax;

   eax = __GET_EAX_FROM_CPUID(0x80000000);
   if ((eax & 0x80000000) != 0x80000000) {
      return 0;
   }
   return eax;
}


void
CPUID_Init(void)
{
   CPUIDRegs regs;
   uint32 *ptr;
   char name[16];

   __GET_CPUID(1, &regs);
   version = regs.eax;
   features = regs.edx;

   __GET_CPUID(0, &regs);
   ptr = (uint32 *)name;
   ptr[0] = regs.ebx;
   ptr[1] = regs.edx;
   ptr[2] = regs.ecx;
   ptr[3] = 0;

   if (strcmp(name, CPUID_INTEL_VENDOR_STRING_FIXED) == 0) {
      vendor = CPUID_VENDOR_INTEL;
   } else if (strcmp(name, CPUID_AMD_VENDOR_STRING_FIXED) == 0) {
      vendor = CPUID_VENDOR_AMD;
   } else if (strcmp(name, CPUID_CYRIX_VENDOR_STRING_FIXED) == 0) {
      vendor = CPUID_VENDOR_CYRIX;
   } else {
      Warning("VMMON CPUID: Unrecognized CPU\n");
      vendor = CPUID_VENDOR_UNKNOWN;
   }
}

CpuidVendors
CPUID_GetVendor(void)
{
   return vendor;
}

uint32
CPUID_GetFeatures(void)
{
   return features;
}

uint32
CPUID_GetVersion(void)
{
   return version;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CPUID_SyscallSupported --
 *
 *     Determine whether processor supports syscall opcode and MSRs.
 *
 * Results:
 *     FALSE     if processor does not support syscall
 *     TRUE      if processor supports syscall
 *
 * Side effects:
 *     It determines value only on first call, caching it for future.
 *
 *-----------------------------------------------------------------------------
 */

Bool
CPUID_SyscallSupported(void)
{
   static int supported = -1;

   if (UNLIKELY(supported == -1)) {
      supported =    CPUIDExtendedSupported() >= 0x80000001
                  && (__GET_EDX_FROM_CPUID(0x80000001) & (1 << 11));
   }

   return supported != 0;
}


Bool
CPUID_LongModeSupported(void)
{
   static int supported = -1;

   if (UNLIKELY(supported == -1)) {
      supported =    CPUIDExtendedSupported() >= 0x80000001
                  && (__GET_EDX_FROM_CPUID(0x80000001) & (1 << 29));
   }

   return supported != 0;
}
