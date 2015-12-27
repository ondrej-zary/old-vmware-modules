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
 * moduleloop.c --
 *
 *     Platform independent routines, private to VMCORE,  to
 *     support module calls and user calls in the module.
 *
 */

#if defined(linux)
/* Must come before any kernel header file */
#   include "driver-config.h"
#   include <linux/kernel.h>
#   include "compat_sched.h"
#endif
#include "vmware.h"
#include "modulecall.h"
#include "vmx86.h"
#include "task.h"
#include "initblock.h"
#include "vm_basic_asm.h"
#include "iocontrols.h"
#include "hostif.h"
#include "memtrack.h"
#include "driver_vmcore.h"
#include "usercalldefs.h"

/*
 *----------------------------------------------------------------------
 *
 * Vmx86_RunVM  --
 *
 *      Main interaction between the module and the monitor:
 *
 *      Run the monitor
 *      Process module calls from the monitor
 *      Make cross user calls to the main thread
 *      Return to userlevel to process normal user calls
 *      and to signal timeout or errors.
 *
 * Results:
 *      Positive: user call number.
 *      USERCALL_RESTART: (Linux only)
 *      USERCALL_VMX86ALLOCERR: error (message already output)
 *
 * Side effects:
 *      Not really, just a switch to monitor and back, that's all.
 *
 *----------------------------------------------------------------------
 */

int
Vmx86_RunVM(VMDriver *vm,   // IN:
            Vcpuid vcpuid)  // IN:
{
   uint32 retval = MODULECALL_USERRETURN;
   VMCrossPage *crosspage = vm->crosspage[vcpuid];
   int bailValue = 0;

   ASSERT(crosspage);

   /*
    * Check if we were interrupted by signal.
    */
   if (crosspage->moduleCallInterrupted) {
      crosspage->moduleCallInterrupted = FALSE;
      goto skipTaskSwitch;
   }

   for (;;) {
      /*
       * Task_Switch changes the world to the monitor.
       * The monitor is waiting in the BackToHost routine.
       */
      UCTIMESTAMP(crosspage, SWITCHING_TO_MONITOR);
      Task_Switch(vm, vcpuid);
      UCTIMESTAMP(crosspage, SWITCHED_TO_MODULE);
      if (crosspage->yieldVCPU &&
          (crosspage->moduleCallType != MODULECALL_YIELD)) {
         HostIF_YieldCPU(0);
      }


skipTaskSwitch:;

      retval = MODULECALL_USERRETURN;

      if (crosspage->userCallType != MODULECALL_USERCALL_NONE) {
         /*
          * This is the main user call path.
          *
          * There are two kinds of user calls.  Normal ones are handled by the
          * calling VCPU thread itself.  We just return from here (back to
          * userlevel) in that case.
          *
          * Calls marked userCallCross are handled by the main VMX thread.  In
          * this case, the userCallRequest field indicates to the VMX that
          * this VCPU wants to make a user call.  This field may be consulted
          * by the VMX at any time (specifically when the VMX is awakened by
          * another VCPU), so it must be set after the other user call
          * arguments.  The VMX is responsible for resetting this field and
          * awakening the VCPU when the user call is complete, via the
          * ACK_USER_CALL and COMPLETE_USER_CALL ioctl.  The latter implies
          * the former.
          *
          * When and how to use ACK_USER_CALL and COMPLETE_USER_CALL are at
          * the discretion of the VMX.  In particular, COMPLETE_USER_CALL
          * does not imply that the requested operation has fully completed,
          * only that the VCPU can continue.  See the comment in
          * MonitorLoopCrossUserCallPoll() for use scenarios.
          *
          * -- edward
          */

         if (!crosspage->userCallCross) {
            ASSERT(!crosspage->userCallRestart);
            bailValue = crosspage->userCallType;
            crosspage->retval = retval;
            goto bailOut;
         }

         if (!crosspage->userCallRestart) {
            ASSERT(crosspage->userCallRequest == MODULECALL_USERCALL_NONE);
            crosspage->userCallRequest = crosspage->userCallType;
            UCTIMESTAMP(crosspage, AWAKENING_VMX);
            HostIF_UserCall(vm, vcpuid);
         }

         UCTIMESTAMP(crosspage, GOING_TO_SLEEP);
         if (HostIF_UserCallWait(vm, vcpuid, USERCALL_TIMEOUT)) {
            ASSERT(crosspage->userCallRequest == MODULECALL_USERCALL_NONE);
         } else {
            retval = MODULECALL_USERTIMEOUT;
         }
         UCTIMESTAMP(crosspage, AWAKE);
      }

      switch (crosspage->moduleCallType) {
      case MODULECALL_NONE:
         break;

      case MODULECALL_INTR:    // Already done in task.c
         break;

      case MODULECALL_GET_RECYCLED_PAGE: {
         MPN32 mpn;
    
         retval = (Vmx86_AllocLockedPages(vm, PtrToVA64(&mpn), 1, TRUE) == 1) ?
                   mpn : INVALID_MPN;
         break;
      }

      case MODULECALL_SEMAWAIT: {
         retval = HostIF_SemaphoreWait(vm, vcpuid, crosspage->args);

         if (retval == MX_WAITINTERRUPTED) {
            crosspage->moduleCallInterrupted = TRUE;
            bailValue = USERCALL_RESTART;
            goto bailOut;
         }
         break;
      }

      case MODULECALL_SEMASIGNAL: {
         retval = HostIF_SemaphoreSignal(crosspage->args);

         if (retval == MX_WAITINTERRUPTED) {
             crosspage->moduleCallInterrupted = TRUE;
             bailValue = USERCALL_RESTART;
             goto bailOut;
         }
         break;
      }

      case MODULECALL_SEMAFORCEWAKEUP: {
         HostIF_SemaphoreForceWakeup(vm, (Vcpuid) crosspage->args[0]);
         break;
      }

      case MODULECALL_IPI: {
         Bool didBroadcast;
         retval = HostIF_IPI(vm, (VCPUSet)(uintptr_t)crosspage->args[0], 
                             TRUE, &didBroadcast);
         break;
      }

      case MODULECALL_RELEASE_ANON_PAGES: {
         unsigned count = 1;
         MPN32 mpns[3];
         mpns[0] = (MPN32)crosspage->args[0];
         mpns[1] = (MPN32)crosspage->args[1];
         mpns[2] = (MPN32)crosspage->args[2];
         if (mpns[1] != INVALID_MPN) {
            count++;
            if (mpns[2] != INVALID_MPN) {
               count++;
            }
         }
         retval = Vmx86_FreeLockedPages(vm, PtrToVA64(mpns), count, TRUE);
         break;
      }

      case MODULECALL_IS_ANON_PAGE: {
         MPN32 mpn = (MPN32)crosspage->args[0];
         retval = Vmx86_IsAnonPage(vm, mpn);
         break;
      }

      case MODULECALL_SWITCH_TO_PEER: {
         crosspage->runVmm64 = !crosspage->runVmm64;
         break;
      }

      case MODULECALL_YIELD: {
         HostIF_YieldCPU(0);
         break;
      }

      case MODULECALL_START_VMX_OP: {
         int numVMCSes;
         int i;

         numVMCSes = crosspage->args[0];
         ASSERT(numVMCSes >= 0);
         ASSERT(numVMCSes <= MAX_DUMMY_VMCSES);
         for (i = 0; i < numVMCSes; i++) {
            MPN dummyVMCS = Task_GetDummyVMCS(i);

            if (dummyVMCS == INVALID_MPN) {
               bailValue = USERCALL_VMX86ALLOCERR;
               goto bailOut;
            }
            crosspage->dummyVMCS[i] = MPN_2_MA(dummyVMCS);
         }

         crosspage->inVMXOperation = 1;
         /*
          * PR 454299: Preserve previous crosspage->retval.
          */
         retval = crosspage->retval;
      } break;

      case MODULECALL_ALLOC_VMX_PAGE: {
         if (Task_GetRootVMCS(crosspage->args[0]) == INVALID_MPN) {
            crosspage->inVMXOperation = 0;

            bailValue = USERCALL_VMX86ALLOCERR;
            goto bailOut;
         }

         retval = crosspage->retval;
      } break;

      default:
         Warning("ModuleCall %d not supported\n", crosspage->moduleCallType);
      }

      crosspage->retval = retval;

#if defined(linux)
      cond_resched(); // Other kernels are preemptable
#endif
   }

bailOut:
   return bailValue;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Vmx86_CompleteUserCall --
 *
 *      Take actions on completion of a cross usercall, which may or
 *      may not have been acknowledged.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      VCPU thread continues.
 *
 *-----------------------------------------------------------------------------
 */

void
Vmx86_CompleteUserCall(VMDriver *vm,  // IN
                       Vcpuid vcpuid) // IN
{
   VMCrossPage *crosspage = vm->crosspage[vcpuid];

   if (crosspage->userCallRequest != MODULECALL_USERCALL_NONE) {
      crosspage->userCallRequest = MODULECALL_USERCALL_NONE;
      HostIF_AckUserCall(vm, vcpuid);
   }
   HostIF_AwakenVcpu(vm, vcpuid);
}
