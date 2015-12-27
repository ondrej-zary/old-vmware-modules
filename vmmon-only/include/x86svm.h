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
 * x86svm.h --
 *
 *    This header file contains basic definitions related to
 *    AMD's hardware virtualization implementation, which you
 *    may see referred to as SVM, AMD-V, or Pacifica.
 */


#ifndef _X86SVM_H_
#define _X86SVM_H_

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMNIXMOD
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMCORE
#define INCLUDE_ALLOW_VMMON
#include "includeCheck.h"

#include "x86msr.h"
#include "vm_asm.h"

#ifdef VMM
#include "cpuidMonitor.h"
#endif

#define SVM_MIN_ASIDS              2

/* SVM related MSRs */
#define MSR_VM_CR                  0xc0010114
#define MSR_IGNNE                  0xc0010115
#define MSR_SMM_CTL                0xc0010116
#define MSR_VM_HSAVE_PA            0xc0010117

#define MSR_VM_CR_SVM_LOCK         0x0000000000000008ULL
#define MSR_VM_CR_SVME_DISABLE     0x0000000000000010ULL

/* SVM CPUID feature leaf */
#define CPUID_SVM_FEATURES         0x8000000a

#define SVM_VMCB_IO_BITMAP_SIZE    (3 * PAGE_SIZE)
#define SVM_VMCB_MSR_BITMAP_SIZE   (2 * PAGE_SIZE)

/* Exit controls for the CR/DR access and hardware exceptions */
#define SVM_CR_RD_CTL(num)         (0x1 << (num))
#define SVM_CR_RD_CTL_ALL          0x0000ffff
#define SVM_CR_WR_CTL(num)         (0x10000 << (num))
#define SVM_CR_WR_CTL_ALL          0xffff0000
#define SVM_DR_RD_CTL(num)         (0x1 << (num))
#define SVM_DR_RD_CTL_ALL          0x0000ffff
#define SVM_DR_WR_CTL(num)         (0x10000 << (num))
#define SVM_DR_WR_CTL_ALL          0xffff0000
#define SVM_XCP_CTL(vector)        (0x1 << (vector))
#define SVM_XCP_CTL_ALL            0xffffffff

/* Execution intercept controls */
/* VMCB.exitCtl */
#define SVM_VMCB_EXEC_CTL_INTR        0x0000000000000001ULL
#define SVM_VMCB_EXEC_CTL_NMI         0x0000000000000002ULL
#define SVM_VMCB_EXEC_CTL_SMI         0x0000000000000004ULL
#define SVM_VMCB_EXEC_CTL_INIT        0x0000000000000008ULL
#define SVM_VMCB_EXEC_CTL_VINTR       0x0000000000000010ULL
#define SVM_VMCB_EXEC_CTL_CR0_SEL_WR  0x0000000000000020ULL
#define SVM_VMCB_EXEC_CTL_SIDT        0x0000000000000040ULL
#define SVM_VMCB_EXEC_CTL_SGDT        0x0000000000000080ULL
#define SVM_VMCB_EXEC_CTL_SLDT        0x0000000000000100ULL
#define SVM_VMCB_EXEC_CTL_STR         0x0000000000000200ULL
#define SVM_VMCB_EXEC_CTL_LIDT        0x0000000000000400ULL
#define SVM_VMCB_EXEC_CTL_LGDT        0x0000000000000800ULL
#define SVM_VMCB_EXEC_CTL_LLDT        0x0000000000001000ULL
#define SVM_VMCB_EXEC_CTL_LTR         0x0000000000002000ULL
#define SVM_VMCB_EXEC_CTL_RDTSC       0x0000000000004000ULL
#define SVM_VMCB_EXEC_CTL_RDPMC       0x0000000000008000ULL
#define SVM_VMCB_EXEC_CTL_PUSHF       0x0000000000010000ULL
#define SVM_VMCB_EXEC_CTL_POPF        0x0000000000020000ULL
#define SVM_VMCB_EXEC_CTL_CPUID       0x0000000000040000ULL
#define SVM_VMCB_EXEC_CTL_RSM         0x0000000000080000ULL
#define SVM_VMCB_EXEC_CTL_IRET        0x0000000000100000ULL
#define SVM_VMCB_EXEC_CTL_SWINT       0x0000000000200000ULL
#define SVM_VMCB_EXEC_CTL_INVD        0x0000000000400000ULL
#define SVM_VMCB_EXEC_CTL_PAUSE       0x0000000000800000ULL
#define SVM_VMCB_EXEC_CTL_HLT         0x0000000001000000ULL
#define SVM_VMCB_EXEC_CTL_INVLPG      0x0000000002000000ULL
#define SVM_VMCB_EXEC_CTL_INVLPGA     0x0000000004000000ULL
#define SVM_VMCB_EXEC_CTL_IOIO        0x0000000008000000ULL
#define SVM_VMCB_EXEC_CTL_MSR         0x0000000010000000ULL
#define SVM_VMCB_EXEC_CTL_TS          0x0000000020000000ULL
#define SVM_VMCB_EXEC_CTL_FERR_FRZ    0x0000000040000000ULL
#define SVM_VMCB_EXEC_CTL_SHUTDOWN    0x0000000080000000ULL
#define SVM_VMCB_EXEC_CTL_VMRUN       0x0000000100000000ULL
#define SVM_VMCB_EXEC_CTL_VMMCALL     0x0000000200000000ULL
#define SVM_VMCB_EXEC_CTL_VMLOAD      0x0000000400000000ULL
#define SVM_VMCB_EXEC_CTL_VMSAVE      0x0000000800000000ULL
#define SVM_VMCB_EXEC_CTL_STGI        0x0000001000000000ULL
#define SVM_VMCB_EXEC_CTL_CLGI        0x0000002000000000ULL
#define SVM_VMCB_EXEC_CTL_SKINIT      0x0000004000000000ULL
#define SVM_VMCB_EXEC_CTL_RDTSCP      0x0000008000000000ULL
#define SVM_VMCB_EXEC_CTL_ICEBP       0x0000010000000000ULL
#define SVM_VMCB_EXEC_CTL_WBINVD      0x0000020000000000ULL
#define SVM_VMCB_EXEC_CTL_MONITOR     0x0000040000000000ULL
#define SVM_VMCB_EXEC_CTL_MWAIT       0x0000080000000000ULL
#define SVM_VMCB_EXEC_CTL_MWAIT_COND  0x0000100000000000ULL
#define SVM_VMCB_EXEC_CTL_RSVD        0xffffe00000000000ULL

/* VMCB.tlbCtl */
#define SVM_VMCB_TLB_CTL_GUEST_ASID   0x00000000ffffffffULL
#define SVM_VMCB_TLB_CTL_FLUSH        0x0000000100000000ULL
#define SVM_VMCB_TLB_CTL_RSVD         0xffffff0000000000ULL

/* VMCB.vAPIC */
#define SVM_VMCB_APIC_VTPR_MASK            0x00000000000000ffULL
#define SVM_VMCB_APIC_VIRQ                 0x0000000000000100ULL
#define SVM_VMCB_APIC_VINTR_PRIO_MASK      0x00000000000f0000ULL
#define SVM_VMCB_APIC_VINTR_PRIO_SHIFT     16
#define SVM_VMCB_APIC_VIGN_TPR             0x0000000000100000ULL
#define SVM_VMCB_APIC_VINTR_MASKING        0x0000000001000000ULL
#define SVM_VMCB_APIC_VINTR_VECTOR_MASK    0x000000ff00000000ULL
#define SVM_VMCB_APIC_VINTR_VECTOR_SHIFT   32
#define SVM_VMCB_APIC_RSVD                 0xffffff0000e0fe00ULL

/* VMCB.intrShadow */
#define SVM_VMCB_INTR_SHADOW          0x0000000000000001ULL
#define SVM_VMCB_INTR_RSVD            0xfffffffffffffffeULL

/* Segment attribute masks (used for conversion to unpacked format) */
#define SVM_VMCB_ATTRIB_LOW           0x000000ff
#define SVM_VMCB_ATTRIB_HI            0x00000f00

#define SVM_VMCB_AR_ACCESSED        DT_ACCESS_RIGHTS_ACCESSED
#define SVM_VMCB_AR_WRITE           DT_ACCESS_RIGHTS_WRITE
#define SVM_VMCB_AR_READ            DT_ACCESS_RIGHTS_READ
#define SVM_VMCB_AR_CONFORM         DT_ACCESS_RIGHTS_CONFORM
#define SVM_VMCB_AR_CODE            DT_ACCESS_RIGHTS_CODE
#define SVM_VMCB_AR_TYPE            DT_ACCESS_RIGHTS_TYPE
#define SVM_VMCB_AR_S               DT_ACCESS_RIGHTS_S
#define SVM_VMCB_AR_DPL             DT_ACCESS_RIGHTS_DPL
#define SVM_VMCB_AR_PRES            DT_ACCESS_RIGHTS_PRES
#define SVM_VMCB_AR_AVL             (DT_ACCESS_RIGHTS_AVL      >> 4)
#define SVM_VMCB_AR_LONGMODE        (DT_ACCESS_RIGHTS_LONGMODE >> 4)
#define SVM_VMCB_AR_DB              (DT_ACCESS_RIGHTS_DB       >> 4)
#define SVM_VMCB_AR_GRAN            (DT_ACCESS_RIGHTS_GRAN     >> 4)

#define SVM_VMCB_AR_TYPE_SHIFT      DT_ACCESS_RIGHTS_TYPE_SHIFT
#define SVM_VMCB_AR_S_SHIFT         DT_ACCESS_RIGHTS_S_SHIFT
#define SVM_VMCB_AR_DPL_SHIFT       DT_ACCESS_RIGHTS_DPL_SHIFT
#define SVM_VMCB_AR_PRES_SHIFT      DT_ACCESS_RIGHTS_PRES_SHIFT
#define SVM_VMCB_AR_AVL_SHIFT       (DT_ACCESS_RIGHTS_AVL_SHIFT      - 4)
#define SVM_VMCB_AR_LONGMODE_SHIFT  (DT_ACCESS_RIGHTS_LONGMODE_SHIFT - 4)
#define SVM_VMCB_AR_DB_SHIFT        (DT_ACCESS_RIGHTS_DB_SHIFT       - 4)
#define SVM_VMCB_AR_GRAN_SHIFT      (DT_ACCESS_RIGHTS_GRAN_SHIFT     - 4)

/* Unique Exit Codes */
#define SVM_EXITCODE_CR_READ(n)       (0 + (n))
#define SVM_EXITCODE_CR_WRITE(n)     (16 + (n))
#define SVM_EXITCODE_DR_READ(n)      (32 + (n))
#define SVM_EXITCODE_DR_WRITE(n)     (48 + (n))
#define SVM_EXITCODE_XCP(n)          (64 + (n))
#define SVM_EXITCODE_INTR            96
#define SVM_EXITCODE_NMI             97
#define SVM_EXITCODE_SMI             98
#define SVM_EXITCODE_INIT            99
#define SVM_EXITCODE_VINTR          100
#define SVM_EXITCODE_CR0_SEL_WR     101
#define SVM_EXITCODE_SIDT           102
#define SVM_EXITCODE_SGDT           103
#define SVM_EXITCODE_SLDT           104
#define SVM_EXITCODE_STR            105
#define SVM_EXITCODE_LIDT           106
#define SVM_EXITCODE_LGDT           107
#define SVM_EXITCODE_LLDT           108
#define SVM_EXITCODE_LTR            109
#define SVM_EXITCODE_RDTSC          110
#define SVM_EXITCODE_RDPMC          111
#define SVM_EXITCODE_PUSHF          112
#define SVM_EXITCODE_POPF           113
#define SVM_EXITCODE_CPUID          114
#define SVM_EXITCODE_RSM            115
#define SVM_EXITCODE_IRET           116
#define SVM_EXITCODE_SWINT          117
#define SVM_EXITCODE_INVD           118
#define SVM_EXITCODE_PAUSE          119
#define SVM_EXITCODE_HLT            120
#define SVM_EXITCODE_INVLPG         121
#define SVM_EXITCODE_INVLPGA        122
#define SVM_EXITCODE_IOIO           123
#define SVM_EXITCODE_MSR            124
#define SVM_EXITCODE_TS             125
#define SVM_EXITCODE_FERR_FRZ       126
#define SVM_EXITCODE_SHUTDOWN       127
#define SVM_EXITCODE_VMRUN          128
#define SVM_EXITCODE_VMMCALL        129
#define SVM_EXITCODE_VMLOAD         130
#define SVM_EXITCODE_VMSAVE         131
#define SVM_EXITCODE_STGI           132
#define SVM_EXITCODE_CLGI           133
#define SVM_EXITCODE_SKINIT         134
#define SVM_EXITCODE_RDTSCP         135
#define SVM_EXITCODE_ICEBP          136
#define SVM_EXITCODE_WBINVD         137
#define SVM_EXITCODE_MONITOR        138
#define SVM_EXITCODE_MWAIT          139
#define SVM_EXITCODE_MWAIT_COND     140
#define SVM_EXITCODE_NPF_INTERNAL   141
#define SVM_EXITCODE_NPF           1024
#define SVM_EXITCODE_INVALID         (-1ULL)

#define SVM_NUM_EXITCODES           (SVM_EXITCODE_NPF_INTERNAL + 1)

/* ExitInfo1 for I/O exits */
#define SVM_IOEXIT_IN            0x00000001
#define SVM_IOEXIT_STR           0x00000004
#define SVM_IOEXIT_REP           0x00000008
#define SVM_IOEXIT_SIZE_MASK     0x00000070
#define SVM_IOEXIT_SIZE_SHIFT    4
#define SVM_IOEXIT_SZ8           0x00000010
#define SVM_IOEXIT_SZ16          0x00000020
#define SVM_IOEXIT_SZ32          0x00000040
#define SVM_IOEXIT_ADDR_MASK     0x00000380
#define SVM_IOEXIT_ADDR_SHIFT    7
#define SVM_IOEXIT_A16           0x00000080
#define SVM_IOEXIT_A32           0x00000100
#define SVM_IOEXIT_A64           0x00000200
#define SVM_IOEXIT_PORT_MASK     0xffff0000
#define SVM_IOEXIT_PORT_SHIFT    16
#define SVM_IOEXIT_MBZ           0x00000c02

/* ExitInfo2 for Task Switch exits */
#define SVM_TSEXIT_ERRORCODE_MASK  0x00000000ffffffffULL
#define SVM_TSEXIT_IRET            0x0000001000000000ULL
#define SVM_TSEXIT_LJMP            0x0000004000000000ULL
#define SVM_TSEXIT_EV              0x0000100000000000ULL
#define SVM_TSEXIT_RF              0x0001000000000000ULL

/* ExitInfo1 for SMI exits */
#define SVM_SMIEXIT_EXTERNAL       0x0000000000000001ULL
#define SVM_SMIEXIT_IN             0x0000000100000000ULL
#define SVM_SMIEXIT_VALID          0x0000000200000000ULL
#define SVM_SMIEXIT_STR            0x0000000400000000ULL
#define SVM_SMIEXIT_REP            0x0000000800000000ULL
#define SVM_SMIEXIT_SZ8            0x0000001000000000ULL
#define SVM_SMIEXIT_SZ16           0x0000002000000000ULL
#define SVM_SMIEXIT_SZ32           0x0000004000000000ULL
#define SVM_SMIEXIT_A16            0x0000008000000000ULL
#define SVM_SMIEXIT_A32            0x0000010000000000ULL
#define SVM_SMIEXIT_A64            0x0000020000000000ULL
#define SVM_SMIEXIT_PORT           0xffff000000000000ULL
#define SVM_SMIEXIT_MBZ            0x0000fc00fffffffeULL

/* Event Injection */
#define SVM_INTINFO_VECTOR_MASK   0x000000ff
#define SVM_INTINFO_TYPE_SHIFT    8
#define SVM_INTINFO_TYPE_MASK     (7 << SVM_INTINFO_TYPE_SHIFT)
#define SVM_INTINFO_TYPE_EXTINT   (0 << SVM_INTINFO_TYPE_SHIFT)
#define SVM_INTINFO_TYPE_RSVD     (1 << SVM_INTINFO_TYPE_SHIFT)
#define SVM_INTINFO_TYPE_NMI      (2 << SVM_INTINFO_TYPE_SHIFT)
#define SVM_INTINFO_TYPE_XCP      (3 << SVM_INTINFO_TYPE_SHIFT)
#define SVM_INTINFO_TYPE_INTN     (4 << SVM_INTINFO_TYPE_SHIFT)
#define SVM_INTINFO_EV            0x00000800
#define SVM_INTINFO_RSVD          0x7ffff000
#define SVM_INTINFO_VALID         0x80000000


#define SVM_EXEC_CTL_BIT(exitCode) (1ULL << (exitCode - SVM_EXITCODE_INTR))

#define VERIFY_EXEC_CTL(name)                                 \
   ASSERT_ON_COMPILE(SVM_EXEC_CTL_BIT(SVM_EXITCODE_##name) == \
                     SVM_VMCB_EXEC_CTL_##name);

static INLINE uint64
SVM_ExecCtlBit(uint32 exitCode)
{
   VERIFY_EXEC_CTL(INTR);
   VERIFY_EXEC_CTL(NMI);
   VERIFY_EXEC_CTL(SMI);
   VERIFY_EXEC_CTL(INIT);
   VERIFY_EXEC_CTL(VINTR);
   VERIFY_EXEC_CTL(CR0_SEL_WR);
   VERIFY_EXEC_CTL(SIDT);
   VERIFY_EXEC_CTL(SGDT);
   VERIFY_EXEC_CTL(SLDT);
   VERIFY_EXEC_CTL(STR);
   VERIFY_EXEC_CTL(LIDT);
   VERIFY_EXEC_CTL(LGDT);
   VERIFY_EXEC_CTL(LLDT);
   VERIFY_EXEC_CTL(LTR);
   VERIFY_EXEC_CTL(RDTSC);
   VERIFY_EXEC_CTL(RDPMC);
   VERIFY_EXEC_CTL(PUSHF);
   VERIFY_EXEC_CTL(POPF);
   VERIFY_EXEC_CTL(CPUID);
   VERIFY_EXEC_CTL(RSM);
   VERIFY_EXEC_CTL(IRET);
   VERIFY_EXEC_CTL(SWINT);
   VERIFY_EXEC_CTL(INVD);
   VERIFY_EXEC_CTL(PAUSE);
   VERIFY_EXEC_CTL(HLT);
   VERIFY_EXEC_CTL(INVLPG);
   VERIFY_EXEC_CTL(INVLPGA);
   VERIFY_EXEC_CTL(IOIO);
   VERIFY_EXEC_CTL(MSR);
   VERIFY_EXEC_CTL(TS);
   VERIFY_EXEC_CTL(FERR_FRZ);
   VERIFY_EXEC_CTL(SHUTDOWN);
   VERIFY_EXEC_CTL(VMRUN);
   VERIFY_EXEC_CTL(VMMCALL);
   VERIFY_EXEC_CTL(VMLOAD);
   VERIFY_EXEC_CTL(VMSAVE);
   VERIFY_EXEC_CTL(STGI);
   VERIFY_EXEC_CTL(CLGI);
   VERIFY_EXEC_CTL(SKINIT);
   VERIFY_EXEC_CTL(RDTSCP);
   VERIFY_EXEC_CTL(ICEBP);
   VERIFY_EXEC_CTL(WBINVD);
   VERIFY_EXEC_CTL(MONITOR);
   VERIFY_EXEC_CTL(MWAIT);
   VERIFY_EXEC_CTL(MWAIT_COND);
   ASSERT(SVM_EXITCODE_INTR <= exitCode && exitCode <= SVM_EXITCODE_MWAIT_COND);
   return SVM_EXEC_CTL_BIT(exitCode);
}


/*
 *----------------------------------------------------------------------
 * SVM_EnabledCPU --
 *
 *   Returns TRUE if SVM is enabled on this CPU.  This function assumes
 *   that the processor is SVM_Capable().
 *----------------------------------------------------------------------
 */
static INLINE Bool
SVM_EnabledCPU(void)
{
   return ((__GET_MSR(MSR_VM_CR) & MSR_VM_CR_SVME_DISABLE) == 0);
}


#ifndef VMM
/*
 *----------------------------------------------------------------------
 * SVM_CapableCPU --
 *
 *   Verify that this CPU is SVM-capable.
 *----------------------------------------------------------------------
 */
static INLINE Bool
SVM_CapableCPU(void)
{
   return ((__GET_EAX_FROM_CPUID(0x80000000) >= 0x8000000a) &&
           ((__GET_ECX_FROM_CPUID(0x80000001) &
             CPUID_FEATURE_AMD_ID81ECX_SVM) != 0) &&
           ((__GET_EAX_FROM_CPUID(0x8000000a) &
             CPUID_FEATURE_AMD_ID8AEAX_SVM_REVISION) != 0));
}


/*
 *----------------------------------------------------------------------
 * SVM_SupportedVersion --
 *
 *   Verify that a CPU has the SVM capabilities required to run the
 *   SVM-enabled monitor.  This function assumes that the processor is
 *   SVM_Capable().  We only support CPUs that populate the exitIntInfo
 *   field of the VMCB when IDT vectoring is interrupted by a task switch
 *   intercept.  That behavior was first introduced with Family 10H.
 *----------------------------------------------------------------------
 */
static INLINE Bool
SVM_SupportedVersion(uint32 version)
{
   return CPUID_EFFECTIVE_FAMILY(version) >= CPUID_FAMILY_K8L;
}


/*
 *
 *----------------------------------------------------------------------
 * SVM_SupportedCPU --
 *
 *   Wrapper to call SVM_SupportedVersion() with the right
 *   parameter(s) for the current CPU.
 *----------------------------------------------------------------------
 */
static INLINE Bool
SVM_SupportedCPU(void)
{
   return SVM_SupportedVersion(__GET_EAX_FROM_CPUID(1));
}


#endif /* VMM */

#endif /* _X86SVM_H_ */
