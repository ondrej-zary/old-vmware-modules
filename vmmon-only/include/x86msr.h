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

/* 
 * x86msr.h --
 *
 *      MSR number definitions.
 */

#ifndef _X86MSR_H_
#define _X86MSR_H_
#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMX
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMNIXMOD
#define INCLUDE_ALLOW_DISTRIBUTE
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_VMCORE
#define INCLUDE_ALLOW_VMMON
#include "includeCheck.h"

#include "vm_basic_types.h"

/*
 * Results of calling rdmsr(msrNum) on all logical processors.
 */
#ifdef _MSC_VER
#pragma warning (disable :4200) // non-std extension: zero-sized array in struct
#endif

typedef
#include "vmware_pack_begin.h"
struct MSRReply {
   /*
    * Unique host logical CPU identifier. It does not change across queries, so
    * we use it to correlate the replies of multiple queries.
    */
   uint64 tag;              // OUT

   uint64 msrVal;           // OUT

   uint8  implemented;      // OUT

   uint8  _pad[7];
}
#include "vmware_pack_end.h"
MSRReply;

typedef
#include "vmware_pack_begin.h"
struct MSRQuery {
   uint32 msrNum;           // IN
   uint32 numLogicalCPUs;   // IN/OUT
   MSRReply logicalCPUs[0]; // OUT
}
#include "vmware_pack_end.h"
MSRQuery;

#define MSR_TSC               0x00000010
#define MSR_PLATFORM_ID       0x00000017
#define MSR_APIC_BASE         0x0000001b
#define MSR_FEATCTL           0x0000003a
#define MSR_BIOS_UPDT_TRIG    0x00000079
#define MSR_BIOS_SIGN_ID      0x0000008b
#define MSR_PERFCTR0          0x000000c1
#define MSR_PERFCTR1          0x000000c2
#define MSR_PLATFORM_INFO     0x000000ce // Intel Nehalem Family
#define MSR_MTRR_CAP          0x000000fe
#define MSR_L2CFG             0x0000011e
#define MSR_SYSENTER_CS       0x00000174
#define MSR_SYSENTER_ESP      0x00000175
#define MSR_SYSENTER_EIP      0x00000176
#define MSR_MCG_CAP           0x00000179
#define MSR_MCG_STATUS        0x0000017a
#define MSR_MCG_CTL           0x0000017b
#define MSR_EVNTSEL0          0x00000186
#define MSR_EVNTSEL1          0x00000187
#define MSR_MISC_ENABLE       0x000001a0
#define MSR_DEBUGCTL          0x000001d9
#define MSR_EFER              0xc0000080
#define MSR_FSBASE            0xc0000100
#define MSR_GSBASE            0xc0000101
#define MSR_KERNELGSBASE      0xc0000102
#define MSR_TSC_AUX           0xc0000103

/* Intel Core Architecture and later: use only architected counters. */
#define IA32_MSR_PERF_CAPABILITIES                0x345
#define MSR_PERF_CAPABILITIES_LBRFMT_SHIFT        0
#define MSR_PERF_CAPABILITIES_LBRFMT_MASK         0x3f
#define MSR_PERF_CAPABILITIES_PEBSTRAP            (1u << 6)
#define MSR_PERF_CAPABILITIES_PEBSSAVEARCHREGS    (1u << 7)
#define MSR_PERF_CAPABILITIES_PEBSRECORDFMT_SHIFT 8
#define MSR_PERF_CAPABILITIES_PEBSRECORDFMT_MASK  0xf
#define MSR_PERF_CAPABILITIES_FREEZE_WHILE_SMM    (1u << 12)

typedef enum {
   SL_PMC_FLAGS_NONE             = 0x00, /* No flags.                      */
   SL_PMC_FLAGS_LBR_VA32         = 0x01, /* LBR format: 32-bit VA.         */
   SL_PMC_FLAGS_LBR_LA64         = 0x02, /* LBR format: 64-bit LA.         */
   SL_PMC_FLAGS_LBR_VA64         = 0x04, /* LBR format: 64-bit VA.         */
   SL_PMC_FLAGS_LBR_PACKED_VA32  = 0x08, /* LBR format: 2x32-bit VAs.      */
} StateLoggerPMCFlags;

#define MSR_MTRR_BASE0        0x00000200
#define MSR_MTRR_MASK0        0x00000201
#define MSR_MTRR_BASE1        0x00000202
#define MSR_MTRR_MASK1        0x00000203
#define MSR_MTRR_BASE2        0x00000204
#define MSR_MTRR_MASK2        0x00000205
#define MSR_MTRR_BASE3        0x00000206
#define MSR_MTRR_MASK3        0x00000207
#define MSR_MTRR_BASE4        0x00000208
#define MSR_MTRR_MASK4        0x00000209
#define MSR_MTRR_BASE5        0x0000020a
#define MSR_MTRR_MASK5        0x0000020b
#define MSR_MTRR_BASE6        0x0000020c
#define MSR_MTRR_MASK6        0x0000020d
#define MSR_MTRR_BASE7        0x0000020e
#define MSR_MTRR_MASK7        0x0000020f
#define MSR_MTRR_FIX64K_00000 0x00000250
#define MSR_MTRR_FIX16K_80000 0x00000258
#define MSR_MTRR_FIX16K_A0000 0x00000259
#define MSR_MTRR_FIX4K_C0000  0x00000268
#define MSR_MTRR_FIX4K_C8000  0x00000269
#define MSR_MTRR_FIX4K_D0000  0x0000026a
#define MSR_MTRR_FIX4K_D8000  0x0000026b
#define MSR_MTRR_FIX4K_E0000  0x0000026c
#define MSR_MTRR_FIX4K_E8000  0x0000026d
#define MSR_MTRR_FIX4K_F0000  0x0000026e
#define MSR_MTRR_FIX4K_F8000  0x0000026f
#define MSR_MTRR_DEF_TYPE     0x000002ff

#define MSR_MC0_CTL          0x00000400
#define MSR_MC0_STATUS       0x00000401
#define MSR_MC0_ADDR         0x00000402
#define MSR_MC0_MISC         0x00000403

#define MSR_DS_AREA          0x00000600

#define MSR_LASTBRANCHFROMIP 0x000001db // Intel P6 Family
#define MSR_LASTBRANCHTOIP   0x000001dc // Intel P6 Family
#define MSR_LASTINTFROMIP    0x000001dd // Intel P6 Family
#define MSR_LASTINTTOIP      0x000001de // Intel P6 Family

#define MSR_LER_FROM_LIP     0x000001d7 // Intel Pentium4 Family
#define MSR_LER_TO_LIP       0x000001d8 // Intel Pentium4 Family
#define MSR_LASTBRANCH_TOS   0x000001da // Intel Pentium4 Family
#define MSR_LASTBRANCH_0     0x000001db // Intel Pentium4 Family
#define MSR_LASTBRANCH_1     0x000001dc // Intel Pentium4 Family
#define MSR_LASTBRANCH_2     0x000001dd // Intel Pentium4 Family
#define MSR_LASTBRANCH_3     0x000001de // Intel Pentium4 Family

#define CORE_LBR_SIZE        8
#define CORE2_LBR_SIZE       4

/* Power Management MSRs */
#define MSR_PERF_STATUS      0x00000198 // Current Performance State (ro)
#define MSR_PERF_CTL         0x00000199 // Target Performance State (rw)
#define MSR_POWER_CTL        0x000001fc // Power Control Register
#define MSR_CST_CONFIG_CTL   0x000000e2 // C-state Configuration (CORE)
#define MSR_MISC_PWR_MGMT    0x000001aa // Misc Power Management (NHM)

/* MSR_POWER_CTL bits (Intel) */
#define MSR_POWER_CTL_C1E    0x00000001 // C1E enable (NHM)

/* P-State Hardware Coordination Feedback Capability (Intel) */
#define MSR_MPERF            0x000000e7 // Maximum Performance (rw)
#define MSR_APERF            0x000000e8 // Actual Performance (rw)

/* Software Controlled Clock Modulation and Thermal Monitors (Intel) */
#define MSR_CLOCK_MODULATION 0x0000019a // Thermal Monitor Control (rw)
#define MSR_THERM_INTERRUPT  0x0000019b // Thermal Interrupt Control (rw)
#define MSR_THERM_STATUS     0x0000019c // Thermal Monitor Status (rw)
#define MSR_THERM2_CTL       0x0000019d // Thermal Monitor 2 Control (ro)

/* MSR_MISC_ENABLE bits (Intel) */
#define MSR_MISC_ENABLE_FOPCODE_COMPAT  (1LL<<2)
#define MSR_MISC_ENABLE_TM1             (1LL<<3)  // Enable Thermal Monitor 1
#define MSR_MISC_ENABLE_BTS_UNAVAILABLE (1LL<<11)
#define MSR_MISC_ENABLE_TM2             (1LL<<13) // Enable Thermal Monitor 2
#define MSR_MISC_ENABLE_ESS             (1LL<<16) // Enable Enhanced SpeedStep
#define MSR_MISC_ENABLE_LIMIT_CPUID     (1LL<<22) // Enable CPUID maxval
#define MSR_MISC_ENABLE_C1E             (1LL<<25) // C1E enable (Merom/Penryn)
#define MSR_MISC_ENABLE_TURBO_DISABLE   (1LL<<38) // Turbo Mode Disabled

/* DebugCtlMSR bits */
#define MSR_DEBUGCTL_LBR     0x00000001
#define MSR_DEBUGCTL_BTF     0x00000002
#define MSR_DEBUGCTL_TR      0x00000040
#define MSR_DEBUGCTL_BTS     0x00000080
#define MSR_DEBUGCTL_BTINT   0x00000100
#define MSR_DEBUGCTL_SMM_FRZ (1 << 14)

/* Feature control bits */
#define MSR_FEATCTL_LOCK     0x00000001
#define MSR_FEATCTL_SMXE     0x00000002
#define MSR_FEATCTL_VMXE     0x00000004

/* MSR_EFER bits. */
#define MSR_EFER_SCE         0x0000000000000001ULL  /* Sys call ext'ns:  r/w */
#define MSR_EFER_RAZ         0x00000000000000feULL  /* Read as zero          */
#define MSR_EFER_LME         0x0000000000000100ULL  /* Long mode enable: r/w */
#define MSR_EFER_LMA         0x0000000000000400ULL  /* Long mode active: r/o */
#define MSR_EFER_NXE         0x0000000000000800ULL  /* No-exec enable:   r/w */
#define MSR_EFER_SVME        0x0000000000001000ULL  /* SVM(AMD) enabled? r/w */
#define MSR_EFER_LMSLE       0x0000000000002000ULL  /* LM seg lim enable:r/w */
#define MSR_EFER_FFXSR       0x0000000000004000ULL  /* Fast FXSAVE:      r/w */
#define MSR_EFER_MBZ         0xffffffffffff8200ULL  /* Must be zero (resrvd) */

/* This ifndef is necessary because this is defined by some kernel headers. */
#ifndef MSR_K7_HWCR
#define MSR_K7_HWCR                0xc0010015    // Available on AMD processors
#endif
#define MSR_K7_HWCR_SSEDIS         0x00008000ULL // Disable SSE bit
#define MSR_K7_HWCR_MONMWAITUSEREN 0x00000400ULL // Enable MONITOR/MWAIT CPL>0
#define MSR_K7_HWCR_TLBFFDIS       0x00000040ULL // Disable TLB Flush Filter

#ifndef MSR_K8_SYSCFG
#define MSR_K8_SYSCFG        0xc0010010
#endif
#define MSR_K8_SYSCFG_MTRRTOM2EN         (1ULL<<21)
#define MSR_K8_SYSCFG_TOM2FORCEMEMTYPEWB (1ULL<<22)
#define MSR_K8_TOPMEM2       0xc001001d

/* AMD "Greyhound" P-state MSRs */
#define MSR_GH_PSTATE_LIMIT      0xc0010061  // P-state Limit Register
#define MSR_GH_PSTATE_CONTROL    0xc0010062  // P-state Control Register [2:0]
#define MSR_GH_PSTATE_STATUS     0xc0010063  // P-state Status Register [2:0]
#define MSR_GH_PSTATE0           0xc0010064  // P-state 0
#define MSR_GH_PSTATE1           0xc0010065  // P-state 1
#define MSR_GH_PSTATE2           0xc0010066  // P-state 2
#define MSR_GH_PSTATE3           0xc0010067  // P-state 3
#define MSR_GH_PSTATE4           0xc0010068  // P-state 4
#define MSR_GH_COFVID_CONTROL    0xc0010070  // COFVID Control Register
#define MSR_GH_COFVID_STATUS     0xc0010071  // COFVID Status Register

/* Syscall/Sysret related MSRs (x86_64) */
#define MSR_STAR             0xc0000081 // Also present on Athlons.
#define MSR_LSTAR            0xc0000082
#define MSR_CSTAR            0xc0000083
#define MSR_SFMASK           0xc0000084

/*
 * MTRR bit description
 */
#define MTRR_CAP_WC           0x400
#define MTRR_CAP_FIX          0x100
#define MTRR_CAP_VCNT_MASK    0xff

#define MTRR_DEF_ENABLE       0x800
#define MTRR_DEF_FIXED_ENABLE 0x400
#define MTRR_DEF_TYPE_MASK    0xff

#define MTRR_BASE_TYPE_MASK   0xff

#define MTRR_MASK_VALID       0x800

#define MTRR_TYPE_UC          0
#define MTRR_TYPE_WC          1
#define MTRR_TYPE_WT          4
#define MTRR_TYPE_WP          5
#define MTRR_TYPE_WB          6

/*
 * PERF_STATUS bits
 */
#define MSR_PERF_STATUS_MAX_BUS_RATIO_SHIFT 40
#define MSR_PERF_STATUS_MAX_BUS_RATIO_MASK  0x1f

/*
 * PLATFORM_INFO bits
 */
#define MSR_PLATFORM_INFO_MAX_NONTURBO_RATIO_SHIFT 8
#define MSR_PLATFORM_INFO_MAX_NONTURBO_RATIO_MASK 0xff



#endif /* _X86MSR_H_ */
