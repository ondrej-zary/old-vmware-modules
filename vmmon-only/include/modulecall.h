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
 * modulecall.h
 *
 *        Monitor <-->  Module (kernel driver) interface
 */

#ifndef _MODULECALL_H
#define _MODULECALL_H

#define INCLUDE_ALLOW_VMCORE
#define INCLUDE_ALLOW_VMMON
#include "includeCheck.h"

#include "x86types.h"
#include "x86desc.h"
#include "vm_time.h"
#include "vcpuid.h"
#include "vcpuset.h"
#include "vmm_constants.h"
#include "contextinfo.h"
#include "rateconv.h"
#include "modulecallstructs.h"
#include "mon_assert.h"

/*
 *----------------------------------------------------------------------
 *
 * ModuleCallType --
 *
 *      Enumeration of support calls done by the module.
 *
 *      If anything changes in the enum, please update KStatModuleCallPtrs
 *      for stats purposes.
 *
 *----------------------------------------------------------------------
 */

typedef enum ModuleCallType {
   MODULECALL_NONE = 100,

   MODULECALL_INTR,
   MODULECALL_SEMAWAIT,
   MODULECALL_SEMASIGNAL,
   MODULECALL_SEMAFORCEWAKEUP,
   MODULECALL_IPI,                   // hit thread with IPI
   MODULECALL_SWITCH_TO_PEER,

   /*
    * Return codes for user calls
    */

   MODULECALL_USERRETURN,
   MODULECALL_USERTIMEOUT,

   MODULECALL_GET_RECYCLED_PAGE,
   MODULECALL_RELEASE_ANON_PAGES,
   MODULECALL_IS_ANON_PAGE,

   MODULECALL_YIELD,

   /*
    * Here "VMX" refers to the Intel VT VMX operation, not the VMware VMX
    * userland process.
    */
   MODULECALL_START_VMX_OP,
   MODULECALL_ALLOC_VMX_PAGE,

   MODULECALL_LAST                   // Number of entries. Must be the last one
} ModuleCallType;

#define MODULECALL_USER_START 300
#define MODULECALL_USER_END   399

#define MODULECALL_CROSS_PAGE_LEN    1
#define MODULECALL_CROSS_PAGE_START  6

#define MODULECALL_USERCALL_NONE     300

/*
 * Define VMX86_UCCOST in the makefiles (Local.mk,
 * typically) if you want a special build whose only purpose
 * is to measure the overhead of a user call and its
 * breakdown.
 *
 * WINDOWS NOTE: I don't know how to pass VMX86_UCCOST to
 * the driver build on Windows.  It must be defined by hand.
 *
 * ESX Note: we don't have a crosspage in which to store these
 * timestamps.  Such a feature would perhaps be nice (if we
 * ever tire of the argument that esx does so few usercalls
 * that speed doesn't matter).
 */

#if defined(VMX86_UCCOST) && !defined(VMX86_SERVER)
#define UCTIMESTAMP(cp, stamp) \
             do { (cp)->ucTimeStamps[UCCOST_ ## stamp] = RDTSC(); } while (0)
#else
#define UCTIMESTAMP(cp, stamp)
#endif

#ifdef VMX86_SERVER
typedef struct UCCostResults {
   uint32 vmksti;
   uint32 vmkcli;
   uint32 ucnop;
} UCCostResults;
#else

typedef struct UCCostResults {
   uint32 htom;
   uint32 mtoh;
   uint32 ucnop;
} UCCostResults;

typedef enum UCCostStamp {
#define UC(x) UCCOST_ ## x,
#include "uccostTable.h"
   UCCOST_MAX
} UCCostStamp;
#endif // VMX86_SERVER

#ifndef VMX86_SERVER
/*
 * Header for the wsBody32.S and wsBody64.S worldswitch code files.
 * The wsBody32 or wsBody64 module loaded depends on the host bitsize.
 */
typedef struct WSModule {
   uint32 vmmonVersion;  // VMMON_VERSION when assembled as part of monitor
   uint16 moduleSize;    // size of whole wsBody{32,64} module
   uint16 hostToVmm;     // offset from beg of header to Host{32,64}toVmm
   uint16 vmm32ToHost;   // offsets to other routines
   uint16 vmm64ToHost;
   uint16 handleUD;
   uint16 handleGP;

   union {               // offsets to patches
      struct {
         uint16 ljmplma;
         uint16 ljmplmabig;
         uint16 va2pa;
         uint16 jump32Dest;
         uint16 pa2va;
      } wsBody32;
      struct {
         uint16 jump32Dest;
         uint16 jump64Dest;
         uint16 jumpFarPtr;
      } wsBody64;
   } patch;
} WSModule;

#define WSMODULE(crosspage) \
   ((WSModule *)((VA)(crosspage) + (crosspage)->wsModOffs))

/*
 * This is a header for the switchNMI.S module.
 * It contains code for DB, NMI, MCE exceptions for use during worldswitch.
 * The code gets copied to the crosspage by initialization.
 */
typedef struct SwitchNMI {
   uint8 codeSize;          // actual size of codeBlock contents
   uint8 offs32DBHandler;   // offset in codeBlock to start of 32-bit DB handler
   uint8 offs32NMIHandler;  // offset in codeBlock to start of 32-bit NMI handler
   uint8 offs32MCEHandler;  // offset in codeBlock to start of 32-bit MCE handler
   uint8 offs64DBHandler;   // offset in codeBlock to start of 64-bit DB handler
   uint8 offs64NMIHandler;  // offset in codeBlock to start of 64-bit NMI handler
   uint8 offs64MCEHandler;  // offset in codeBlock to start of 64-bit MCE handler
   uint8 volatile gotDB;    // mini 32/64-bit DB handlers set to 1 on execution
   uint8 volatile gotNMI;   // mini 32/64-bit NMI handlers set to 1 on execution
   uint8 volatile gotMCE;   // mini 32/64-bit MCE handlers set to 1 on execution
   uint8 codeBlock[1];      // code for the handlers - stretched by initialization code
} SwitchNMI;
#endif

#ifdef VMX86_SERVER
#define SHADOW_DR64(cp, n)    (cp->shadowDR[n].ureg64)
#define SHADOW_DR32(cp, n)    (cp->shadowDR[n].ureg32)
#else
#define SHADOW_DR64(cp, n)    (cp->_shadowDR[n].ureg64)
#define SHADOW_DR32(cp, n)    (cp->_shadowDR[n].ureg32)
#endif

#ifdef VMM64
#define SHADOW_DR(cp, n) SHADOW_DR64(cp, n)
#else
#define SHADOW_DR(cp, n) SHADOW_DR32(cp, n)
#endif


/*----------------------------------------------------------------------
 *
 * MAX_SWITCH_PT_PATCHES
 *
 *   This is the maximum number of patches that must be placed into
 *   the monitor page tables so that two pages of the host GDT and the
 *   crosspage can be accessed during worldswitch.
 *
 *----------------------------------------------------------------------
 */
#define MAX_SWITCH_PT_PATCHES 3

/*----------------------------------------------------------------------
 *
 * WS_NMI_STRESS
 *
 *   When set to non-zero, this causes the NMI-safe worldswitch code
 *   to be automatically stress tested by simulating NMIs arriving
 *   between various instructions.
 *
 *   When set to zero, normal worldswitch operation occurs.
 *
 *   See the worldswitch assembly code for details.
 *
 *----------------------------------------------------------------------
 */
#define WS_NMI_STRESS 0


/*----------------------------------------------------------------------
 *
 * VMM64PageTablePatch
 *
 *    Describes an entry in the monitor page table which needs to be
 *    patched during the back-to-host worldswitch.
 *
 *    o A patch can appear at any place in the page table, and so four
 *      items are required to uniquely describe the patch:
 *
 *      o level
 *
 *        This is the level in the page table to which the patch must
 *        be applied: L4, L3, L2, L1.  This information is used to
 *        determine the base of the region of memory which must be
 *        patched.  The level value corresponds to the following
 *        regions in monitor memory:
 *
 *          MMU_ROOT_64
 *          MMU_L3_64
 *          MMU_L2_64
 *          MON_PAGE_TABLE_64
 *
 *        The value zero is reserved to indicate an empty spot in the
 *        array of patches.
 *
 *      o level offset
 *
 *        The monitor memory regions corresponding to the page table
 *        levels may be more than one page in length, so a 'page
 *        offset' is required to know the starting address of the page
 *        table page which must be patched in 'level'.
 *
 *      o page index
 *
 *        The 'index' value specifies the element in the page which
 *        should be patched.
 *
 *      o pte
 *
 *        This is the PTE value which will be patched into the monitor
 *        page table.
 *
 *----------------------------------------------------------------------
 */
typedef
#include "vmware_pack_begin.h"
struct VMM64PageTablePatch {
#define PTP_EMPTY    (0U) /* Unused array entry. (must be 0) */
#define PTP_LEVEL_L1 (1U)
#define PTP_LEVEL_L2 (2U)
#define PTP_LEVEL_L3 (3U)
#define PTP_LEVEL_L4 (4U)
   uint16   level;              /* [0, 4]  (maximal size: 3 bits) */
   uint16   page;               /* Index of 'page' in 'level'.    */
   uint32   index;              /* Index of 'pte' in 'page'.      */
   VM_PDPTE pte;                /* PTE.                           */
}
#include "vmware_pack_end.h"
VMM64PageTablePatch;

#define MAX_DUMMY_VMCSES 16

/*
 *----------------------------------------------------------------------
 *
 * VMCrossPage --
 *
 *      data structure shared between the monitor and the module
 *      that is used for crossing between the two.
 *      Accessible as vm->cross (kernel module) and CROSS_PAGE
 *      (monitor)
 *
 *      Exactly one page long
 *
 *----------------------------------------------------------------------
 */

typedef
#include "vmware_pack_begin.h"
struct VMCrossPage {
   /*
    * Version checking. Should remain at offset 0
    */
   uint32 version;
   uint32 crosspage_size;

   /*
    * Tiny stack that is used during switching so it can remain valid.  It's
    * good to keep the end 16-byte aligned for 64-bit processors.  There must
    * be enough room for two interrupt frames (in case of NMI during #DB
    * handling), plus a half dozen registers.
    *
    * Also, the cache-line stuff below assumes we end on a 64-byte boundary.
    */
   uint32 tinyStack[46];

   /*
    * Cache Line 1
    */
   uint64 hostCR4;           //00 host CR4 & ~CR4_PGE
   uint32 crosspageMA;       //08
   uint8  hostDRSaved;       //12 Host DR spilled to hostDR[x].
   uint8  hostDRInHW;        //13 0: shadowDR in h/w, 1: hostDR in h/w.
   DTR64  switchHostIDTR;    //14 baseLA = switchHostIDT's host knl LA
                             //   contains host-sized DB,NMI,MCE entries
   uint64 hostSwitchCR3;     //24
   uint64 hostRBX;           //32
   uint64 hostRSI;           //40
   uint64 hostRDI;           //48
   uint64 hostRBP;           //56

   /*
    * Cache line 2
    */
   uint64 hostRSP;           //00
   uint64 hostCR3;           //08
   Bool   runVmm64;          //16
   uint8  shadDRInHW;        //17 bit n set iff %DRn == _shadowDR[n]
   DTR32  switchMon32IDTR;   //18 has baseLA = switchMon32IDT's monitor LA
   uint64 hostR12;           //24
   uint64 hostR13;           //32
   uint64 hostR14;           //40
   uint64 hostR15;           //48
   uint16 mon32TR;           //56
   uint16 mon32SS;           //58
   uint32 mon32EBX;          //60

   /*
    * Cache line 3
    */
   uint32 mon32EBP;          //00
   uint16 hostSS;            //04
   DTR64  crossGDTHKLADesc;  //06 always uses host kernel linear address
   uint32 mon32EDI;          //16
   uint32 mon32CR3;          //20
   uint64 mon64CR3;          //24
   uint16 mon64SS;           //32
   DTR32  mon32GDTR;         //34
   uint32 mon32ESP;          //40
   uint16 mon32DS;           //44
   DTR64  mon64GDTR;         //46
   FarPtr32 jump64Code;      //56 &worldswitch_64h_32v_mode_64
   uint16 mon64ES;           //62

   /*
    * Cache line 4
    */
   uint32 mon32ESI;          //00
   uint16 mon32ES;           //04
   DTR64  crossGDTMADesc;    //06 always uses machine address
                             //   contains 32-bit DB,NMI,MCE entries
   uint64 mon64RBX;          //16
   uint64 mon64RSP;          //24
   uint64 mon64RBP;          //32
   uint64 mon64RSI;          //40
   uint64 mon64RDI;          //48
   uint64 mon64R12;          //56

   /*
    * Cache line 5
    */
   uint64 mon64R13;          //00
   uint64 mon64R14;          //08
   uint64 mon64R15;          //16
   uint64 farPtr;            //24 &worldswitch_64h_32v_mon_switch
   FarPtr32 jump32Code;      //32 &worldswitch_64h_32v_mode_32compat
   DTR64  switchMixIDTR;     //38 has baseLA = switchMixIDT's MA
                             //   contains 32-bit and 64-bit NMI,MCE entries
   uint16 mon64DS;           //48
   DTR32  crossGDTVmm32;     //50 32-bit host: HKLA based
                             //   64-bit host: MA based
   uint64 mon64RIP;          //56

   /*
    * Cache lines 6,7
    */
   uint32 mon32EIP;          //00
   uint16 mon64TR;           //04
   DTR64  switchMon64IDTR;   //06 has baseLA = switchMon64IDT's monitor LA
                             //   contains 64-bit DB,NMI,MCE entries
   uint64 hostDR[8];         //16

   /*
    * Offset from start of crosspage to where worldswitch code module is
    * loaded.  The worldswitch code module is loaded at the very end of the
    * page that the VMCrossPage struct is loaded at the beginning of.
    *
    * Access fields of the worldswitch code module with the WSMODULE macro.
    */
   uint32 wsModOffs;

   /*
    * The interrupt redirection bitmap, must immediately follow
    * monContext.task (ASSERT fail otherwise).
    */
   Task32      monTask32;
   uint8       interruptRedirectionBitMap[INTERRUPT_REDIRECTION_BITMAP_SIZE]; /* vmm32 */
   Task64      monTask64;                 /* vmm64's task */

   /*
    * These values are used by BackToHost to patch in the crosspage at either
    * its host kernel linear address (host/monitor bitsizes match) or at its
    * machine address (host/monitor bitsize mismatch).
    *
    * The L4 values are used by the 64-bit monitor and the indices index the
    * 64-bit monitor's L4 (root) page.  They are in the range 0..511.
    *
    * The L2 values are used by the 32-bit monitor and the indices index the
    * 32-bit monitor's L2 (directory) page.  If the table is nonPAE, the number
    * is in the range 0..1023.  If the table is PAE, the number is in the range
    * 0..2047, as the monitor's L2 pages are virtually contiguous and so they
    * are treated as a single array.
    *
    * For 32-bit monitor, the patch is determined by the *host* PAEness.  The
    * only mismatch case we allow is host nonPAE, monitor PAE.  In this case,
    * the 32-bit BackToHost routine switches to nonPAE mode before switching
    * and restores PAE mode before returning out to the monitor.  So as far as
    * these patches are concerned, they are done with host PAEness, not monitor
    * hapPAEness (is a warm gun...).
    *
    * For nonPAE patches, the setup code guarantees the values fit in 32 bits.
    */
   uint32 vmm32L2PIs[MAX_SWITCH_PT_PATCHES];   // vmm32: patch indices
   uint64 vmm32L2PEs[MAX_SWITCH_PT_PATCHES];   // vmm32: patch entries

   VMM64PageTablePatch vmm64PTP[MAX_SWITCH_PT_PATCHES]; /* page table patch */

   /*
    * This is only needed when the host is nonPAE and the 32-bit monitor is
    * PAE.  It is a nonPAE L1 (table) page that maps the monitor's top 4M
    * address space.  It is not kept completely updated, it contains just
    * enough to switch from monitor to host and back.
    */
   MPN32 crossMonPageTableMPN;

   /*
    * The monitor may requests up to two actions when
    * returning to the host.  The moduleCallType field and
    * args encode a request for some action in the driver.
    * The userCallType field (together with the RPC block)
    * encodes a user call request.  The two requests are
    * independent.  The user call is executed first, with
    * the exception of MODULECALL_INTR which has a special
    * effect.
    */
   ModuleCallType moduleCallType;
   uint32 args[4];
   uint32 retval;

   int userCallType;
   volatile int userCallRequest;       // VCPU/VMX synchronization
   Bool userCallCross;
   Bool userCallRestart;

   /*
    * TRUE if moduleCall was interrupted by signal. Only
    * vmmon uses this field to remember that it should
    * restart RunVM call, nobody else should look at it.
    */
   Bool moduleCallInterrupted;
   uint8 yieldVCPU;

   // host irq relocation values
   int irqRelocateOffset[2];

#if !defined(VMX86_SERVER)
   uint64 ucTimeStamps[UCCOST_MAX];
#endif

   /*
    * The values in the shadow debug registers must match
    * those in the hardware debug register immediately after
    * a task switch in either direction.  They are used to
    * minimize moves to and from the debug registers.
    */
   SharedUReg64 _shadowDR[8];

   Assert_MonSrcLoc switchError;

   SystemCallState systemCall;

   /*
    * Adjustment for machines where the hardware TSC does not run
    * constantly (laptops) or is out of sync between different PCPUs.
    * Updated as needed by vmmon.  See VMK_SharedData for the ESX
    * analog, which is updated by the vmkernel.
    */
   RateConv_ParamsVolatile pseudoTSCConv;
   /* PTSC value immediately before last worldswitch. */
   VmAbsoluteTS worldSwitchPTSC;

   /*
    * PTSC value of the next MonitorPoll callback for this vcpu.  When
    * the time arrives, if a target VCPU thread is in the monitor, it
    * wants to receive a hardware interrupt (e.g., an IPI) as soon as
    * possible; if it has called up to userlevel to halt, it wants to
    * wake up as soon as possible.
    */
   VmAbsoluteTS monitorPollExpiry;

   /*
    * Location of crosspage in different address spaces.
    */
   LA32   vmm32CrossPageLA;  // where 32-bit mon has crosspage double-mapped
   LA64   vmm64CrossPageLA;  // where 64-bit mon has crosspage double-mapped
   LA64   hostCrossPageLA;   // where host has crosspage mapped

   LA32   vmm32CrossGDTLA;   // where crossGDT mapped by PT patch
                             //  32-bit host: host kernel linear address
                             //  64-bit host: machine address
   LA64   vmm64CrossGDTLA;   // where crossGDT mapped by PT patch
                             //  32-bit host: machine address
                             //  64-bit host: host kernel linear address

   /*
    * A VMXON page used to return to VMX operation when leaving the monitor,
    * if we had to leave VMX operation to enter the monitor.  These pages
    * are allocated per-PCPU, and this field must contain the VMXON page for
    * the current PCPU when performing a world-switch.
    */
   MA     rootVMCS;

   /*
    * Dummy VMCSes used to ensure that the cached state of a foreign VMCS
    * gets flushed to memory.
    */
   MA     dummyVMCS[MAX_DUMMY_VMCSES];

   /*
    * The current VMCS of a foreign hypervisor when we leave VMX operation
    * to disable paging.
    */
   MA     foreignVMCS;

   /*
    * Set if the host is in VMX operation and we need to disable paging
    * to switch between legacy mode and long mode.
    */
   uint32 inVMXOperation;

   /*
    * Set by Task_InitCrosspage to tell monitorHosted32.c what host CS to use.
    */
   uint16 hostInitial32CS;
   uint16 hostInitial64CS;

   /*
    * If an NMI happens when switching from the host to the monitor, this flag
    * gets set by the monitor and it immediately loops back to the host where
    * the host can forward the NMI to the host OS.  Ditto for MCE.
    */
   uint32 retryWorldSwitch;

   /*
    * Descriptors and interrupt tables for switchNMI handlers.
    */
   uint32 switchHostIDT[76];       // uses hostCS:hostVA

   uint32 switchMon32IDT[38];      // uses 32-bit monCS:monVA

   uint32 switchMon64IDT[76];      // uses 64-bit monCS:monVA

   uint32 switchMixIDT[76];        // uses CROSSGDT_*CS:MA

   uint32 switchNMI;         // Offset from beginning of crosspage to where
                             // switchNMI.S is loaded (near end of crosspage).
                             // Use macro SWITCHNMI to access switchNMI fields.
}
#include "vmware_pack_end.h"
VMCrossPage;

#define SWITCHNMI(crosspage) ((SwitchNMI *)((VA)(crosspage) + (crosspage)->switchNMI))

#define CROSSPAGE_VERSION    (0x17AC + WS_NMI_STRESS)  // increment by TWO

#if !defined(VMX86_SERVER) && defined(VMM)
#define CROSS_PAGE  ((VMCrossPage * const) VPN_2_VA(CROSS_PAGE_START))
#define VMM_SWITCH_SHARED_DATA CROSS_PAGE
#endif

#define NULLPAGE_LINEAR_START  (MONITOR_LINEAR_START + \
                                PAGE_SIZE * CPL0_GUARD_PAGE_START)

#define USERCALL_TIMEOUT     100  // milliseconds

#define MX_WAITINTERRUPTED     3
#define MX_WAITTIMEDOUT        2
#define MX_WAITNORMAL          1  // Must equal one; see linux module code.
#define MX_WAITERROR           0  // Use MX_ISWAITERROR() to test for error.

// Any zero or negative value denotes error.
#define MX_ISWAITERROR(e)      ((e) <= MX_WAITERROR)

#define OFF64(_off) offsetof(ContextInfo64,_off)

#endif
