/*********************************************************
 * Copyright (C) 2002 VMware, Inc. All rights reserved.
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
 * vcpuset.h --
 *
 *	ADT for a set of VCPUs. Currently implemented as a bitmask.
 */

#ifndef _VCPUSET_H_
#define _VCPUSET_H_

#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_VMX
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMMON
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

/*
 * This is how to get ffs():
 */
#if defined VMM || defined VMKERNEL
   #include "vm_libc.h"
#elif defined _WIN32
   #include "vm_basic_asm.h"
#elif defined VM_X86_64


/*
 *----------------------------------------------------------------------
 *
 * ffs_x86_64 --
 * 
 *      Find first bit set.
 * 
 *      Red Hat Enterprise Linux 3.0 (2.4.21-9.ELsmp) has broken
 *      asm-x86_64/bitops.h:ffs(). See Bug 42417. Define our own.
 *      This comes from SuSE 9 (2.4.21-201) which has the bug fixed,
 *      though we add the "cc" clobber.
 *
 *      Note that Intel documentation for BSF indicates that if 
 *      source operand is 0, then the destination operand is 
 *      undefined.  This is the reason for the CMOV after BSF.  
 *      AMD defines BSF to leave the destination unchanged in
 *      this case, so for AMD a shorter sequence is possible.
 * 
 * Results:
 *   
 *      The bit position of the first bit set in x, or 0 if no
 *      bits are set in x.  The least significant bit is position 1. 
 *
 *----------------------------------------------------------------------
 */

static INLINE int
ffs_x86_64(int x)
{
  int cnt;
  int tmp;
  
  __asm__("bsfl %2,%0\n\t"
          "cmovel %1, %0\n\t"
          : "=&r" (cnt), "=r" (tmp) : "rm" (x), "1" (-1) : "cc");
  return cnt + 1;
}

#define ffs(x) ffs_x86_64(x)

#elif defined MODULE
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 12, 0)
   #include <linux/bitops.h>
#endif
#elif defined __APPLE__ && defined KERNEL
   /* 
    * XXXMACOS An ugly hack to resolve redefinition of PAGE_ defines 
    * between libkern.h and vm_basic_types.h. 
    */
   #ifndef _MACH_I386_VM_PARAM_H_
      #undef PAGE_SIZE
      #undef PAGE_SHIFT
      #undef PAGE_MASK
   #endif
   #include <libkern/libkern.h> /* Get the right ffs() prototype. */
#else // assume user-level Posix
   #include <string.h>
#endif
#include "vm_atomic.h"
#include "vcpuid.h"

typedef uint32 VCPUSet;

EXTERN VCPUSet vcpusetFull;


static INLINE VCPUSet
VCPUSet_Empty(void)
{
   return 0;
}

static INLINE VCPUSet
VCPUSet_Singleton(Vcpuid v)
{
   ASSERT_ON_COMPILE(VCPUID_INVALID >= 32); // Ensure test below catches invalid VCPUs
   ASSERT(v < 32);                          // Shift by 32+ is undefined.
   return 1 << v;
}

static INLINE VCPUSet
VCPUSet_SingletonChecked(Vcpuid v)
{
   return v == VCPUID_INVALID ? VCPUSet_Empty() : VCPUSet_Singleton(v);
}

static INLINE Bool
VCPUSet_IsSingleton(VCPUSet vcs)
{
   return vcs != 0 && (vcs & (vcs - 1)) == 0;
}

/*
 *----------------------------------------------------------------------
 * VCPUSet_FindFirst --
 *      
 *      First (least significant) Vcpuid in a set.
 *
 * Results:
 *      Vcpuid if at least one is present in a set.
 *      VCPUID_INVALID if the set is empty.
 *----------------------------------------------------------------------
 */

static INLINE Vcpuid
VCPUSet_FindFirst(VCPUSet vcs)
{
   ASSERT(VCPUID_INVALID == (Vcpuid)-1);
   return ffs(vcs) - 1;
}

static INLINE Bool
VCPUSet_Equals(VCPUSet vcs1, VCPUSet vcs2)
{
   return vcs1 == vcs2;
}

static INLINE Bool
VCPUSet_IsEmpty(VCPUSet vcs)
{
   return VCPUSet_Equals(vcs, VCPUSet_Empty());
}

static INLINE VCPUSet
VCPUSet_Union(VCPUSet vcs1, VCPUSet vcs2)
{
   return vcs1 | vcs2;
}

static INLINE VCPUSet
VCPUSet_Intersection(VCPUSet s1, VCPUSet s2)
{
   return s1 & s2;
}

static INLINE VCPUSet
VCPUSet_Difference(VCPUSet s1, VCPUSet s2)
{
   return s1 & ~s2;
}

static INLINE VCPUSet
VCPUSet_Remove(VCPUSet vcs, Vcpuid v)
{
   return VCPUSet_Intersection(vcs, ~VCPUSet_Singleton(v));
}

static INLINE VCPUSet
VCPUSet_Include(VCPUSet vcs, Vcpuid v)
{
   return VCPUSet_Union(vcs, VCPUSet_Singleton(v));
}

static INLINE Bool
VCPUSet_IsMember(VCPUSet vcs, Vcpuid v)
{
   return !VCPUSet_IsEmpty(VCPUSet_Intersection(vcs, VCPUSet_Singleton(v)));
}

/*
 *----------------------------------------------------------------------
 * VCPUSet_IsSuperset --
 *    Returns true iff vcs1 contains a superset of the vcpus contained
 *    by vcs2.
 *----------------------------------------------------------------------
 */
static INLINE Bool
VCPUSet_IsSuperset(VCPUSet vcs1, VCPUSet vcs2)
{
   return !(vcs2 & ~vcs1);
}

static INLINE Bool
VCPUSet_IsSubset(VCPUSet vcs1, VCPUSet vcs2)
{
   return VCPUSet_IsSuperset(vcs2, vcs1);
}

static INLINE void
VCPUSet_AtomicInit(volatile VCPUSet *dst, VCPUSet vcs)
{
   Atomic_Write(Atomic_VolatileToAtomic(dst), vcs);
}

static INLINE VCPUSet
VCPUSet_AtomicRead(volatile VCPUSet *src)
{
   return Atomic_Read(Atomic_VolatileToAtomic(src));
}

static INLINE void
VCPUSet_AtomicUnion(volatile VCPUSet *dst, VCPUSet newset)
{
   Atomic_Or(Atomic_VolatileToAtomic(dst), newset);
}

static INLINE void
VCPUSet_AtomicDifference(volatile VCPUSet *dst, VCPUSet gone)
{
   Atomic_And(Atomic_VolatileToAtomic(dst), ~gone);
}

static INLINE void
VCPUSet_AtomicRemove(volatile VCPUSet *dst, Vcpuid v)
{
   Atomic_And(Atomic_VolatileToAtomic(dst), ~VCPUSet_Singleton(v));
}

static INLINE void
VCPUSet_AtomicInclude(volatile VCPUSet *dst, Vcpuid v)
{
   Atomic_Or(Atomic_VolatileToAtomic(dst), VCPUSet_Singleton(v));
}

static INLINE Bool
VCPUSet_AtomicIsMember(volatile VCPUSet *vcs, Vcpuid v)
{
   VCPUSet ivcs = Atomic_Read(Atomic_VolatileToAtomic(vcs));
   return VCPUSet_IsMember(ivcs, v);
}

static INLINE Bool
VCPUSet_AtomicIsEmpty(volatile VCPUSet *vcs)
{
   VCPUSet ivcs =  Atomic_Read(Atomic_VolatileToAtomic(vcs));
   return VCPUSet_IsEmpty(ivcs);
}

/*
 *----------------------------------------------------------------------
 *
 * VCPUSet_Size --
 *
 *    Return the number of VCPUs in this set.
 *
 *----------------------------------------------------------------------
 */
static INLINE int
VCPUSet_Size(VCPUSet vcs)
{
   int     n = 0;
   while (vcs != 0) {
      vcs = vcs & (vcs - 1);
      n++;
   }
   return n;
}

/*
 *----------------------------------------------------------------------
 *
 * VCPUSet_Full --
 *
 *  Return the set representing all the VCPUs in the system.
 *
 *----------------------------------------------------------------------
 */
static INLINE VCPUSet
VCPUSet_Full(void)
{
#if defined(VMM) || defined(VMX)
   /*
    * Read too early, we may get the wrong notion of how many
    * vcpus the VM has. Cf. pr286243 and pr289186.
    */
   ASSERT(NumVCPUs() != 0 && vcpusetFull != 0);
#endif
   return vcpusetFull;
}

/*
 *----------------------------------------------------------------------
 *
 * VCPUSet_IsFull --
 *
 *  Returns true iff v contains the set of all vcpus.
 *
 *----------------------------------------------------------------------
 */
static INLINE Bool
VCPUSet_IsFull(VCPUSet v)
{
   return VCPUSet_Equals(v, VCPUSet_Full());
}

static INLINE Bool
VCPUSet_AtomicIsFull(volatile VCPUSet *vcs)
{
   VCPUSet ivcs = Atomic_Read(Atomic_VolatileToAtomic(vcs));
   return VCPUSet_IsFull(ivcs);
}

#if defined  VM_X86_64
#undef ffs
#endif

#endif /* _VCPUSET_H_ */
