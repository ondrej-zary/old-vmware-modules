/*********************************************************
 * Copyright (C) 2007 VMware, Inc. All rights reserved.
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
 * vmciEvent.c --
 *
 *     VMCI Event code for host and guests.
 */

#if defined(__linux__) && !defined(VMKERNEL)
#  include "driver-config.h"
#  define EXPORT_SYMTAB
#  define __NO_VERSION__
#  include "compat_kernel.h"
#  include "compat_module.h"
#endif // __linux__
#include "vmci_defs.h"
#include "vmci_kernel_if.h"
#include "vmci_infrastructure.h"
#include "vmciEvent.h"
#ifdef VMX86_TOOLS
#  include "vmciInt.h"
#  include "vmciGuestKernelAPI.h"
#  include "vmciUtil.h"
#else
#  include "vmciDriver.h"
#endif
#ifdef VMKERNEL
#  include "vmciVmkInt.h"
#  include "vm_libc.h"
#  include "helper_ext.h"
#else
#  include "vmciHostKernelAPI.h"
#endif
#include "circList.h"  /* Must come after vmciVmkInt.h. */

#define EVENT_MAGIC 0xEABE0000


typedef struct VMCISubscription {
   VMCIId         id;
   VMCI_Event     event;
   VMCI_EventCB   callback;
   void           *callbackData;
   ListItem       subscriberListItem;
} VMCISubscription;

typedef struct VMCISubscriptionItem {
   ListItem          listItem;
   VMCISubscription  sub;
} VMCISubscriptionItem;


static VMCISubscription *VMCIEventFind(VMCIId subID);
#if defined(VMKERNEL)
static void VMCIEventHelperCB(void *data);
#endif
static void VMCIEventDeliver(VMCIEventMsg *eventMsg);
static int VMCIEventRegisterSubscription(VMCISubscription *sub, VMCI_Event event,
                                         VMCI_EventCB callback,
                                         void *callbackData);
static VMCISubscription *VMCIEventUnregisterSubscription(VMCIId subID);

/*
 * In the guest, VMCI events are dispatched from interrupt context, so
 * the locks need to be bottom half safe. In the host kernel, this
 * isn't so, and regular locks are used instead.
 */

#ifdef VMX86_TOOLS
#define VMCIEventInitLock(_lock, _name) VMCI_InitLock(_lock, _name, VMCI_LOCK_RANK_MIDDLE_BH)
#define VMCIEventGrabLock(_lock, _flags) VMCI_GrabLock_BH(_lock, _flags)
#define VMCIEventReleaseLock(_lock, _flags) VMCI_ReleaseLock_BH(_lock, _flags)
#else
#define VMCIEventInitLock(_lock, _name) VMCI_InitLock(_lock, _name, VMCI_LOCK_RANK_MIDDLE)
#define VMCIEventGrabLock(_lock, _flags) VMCI_GrabLock(_lock, _flags)
#define VMCIEventReleaseLock(_lock, _flags) VMCI_ReleaseLock(_lock, _flags)
#endif


static ListItem *subscriberArray[VMCI_EVENT_MAX] = {NULL};
static VMCILock subscriberLock;


/*
 *----------------------------------------------------------------------
 *
 * VMCIEvent_Init --
 *
 *      General init code.
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
VMCIEvent_Init(void)
{
   VMCIEventInitLock(&subscriberLock, "VMCIEventSubscriberLock");
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIEvent_Exit --
 *
 *      General exit code.
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
VMCIEvent_Exit(void)
{
   ListItem *iter, *iter2;
   VMCI_Event e;

   /* We free all memory at exit. */
   for (e = 0; e < VMCI_EVENT_MAX; e++) {
      LIST_SCAN_SAFE(iter, iter2, subscriberArray[e]) {
         VMCISubscription *cur =
            LIST_CONTAINER(iter, VMCISubscription, subscriberListItem);
         VMCI_FreeKernelMem(cur, sizeof *cur);
      }
   }
   VMCI_CleanupLock(&subscriberLock);
}

#ifdef VMX86_TOOLS
/*
 *-----------------------------------------------------------------------------
 *
 * VMCIEvent_CheckHostCapabilities --
 *
 *      Verify that the host supports the hypercalls we need. If it does not,
 *      try to find fallback hypercalls and use those instead.
 *
 * Results:
 *      TRUE if required hypercalls (or fallback hypercalls) are
 *      supported by the host, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

Bool
VMCIEvent_CheckHostCapabilities(void)
{
   /* VMCIEvent does not require any hypercalls. */
   return TRUE;
}
#endif

/*
 *-----------------------------------------------------------------------------
 *
 * VMCIEventFind --
 *
 *      Find entry. Assumes lock is held.
 *
 * Results:
 *      Entry if found, NULL if not.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static VMCISubscription *
VMCIEventFind(VMCIId subID)  // IN
{
   ListItem *iter;
   VMCI_Event e;

   for (e = 0; e < VMCI_EVENT_MAX; e++) {
      LIST_SCAN(iter, subscriberArray[e]) {
         VMCISubscription *cur =
            LIST_CONTAINER(iter, VMCISubscription, subscriberListItem);
         if (cur->id == subID) {
            return cur;
         }
      }
   }
   return NULL;
}


#if defined(VMKERNEL)
/*
 *----------------------------------------------------------------------------
 *
 * VMCIEventHelperCB --
 *
 *      Invoked in the helper world on ESX to deliver the event.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The event specified in eventMsg is delivered and eventMsg is freed.
 *
 *----------------------------------------------------------------------------
 */

static void
VMCIEventHelperCB(void *data)  // IN
{
   VMCIEventMsg *eventMsg;

   ASSERT(data);

   eventMsg = (VMCIEventMsg *)data;
   VMCIEventDeliver(eventMsg);
   VMCI_FreeKernelMem(eventMsg, VMCI_DG_SIZE(&eventMsg->hdr));
}
#endif


/*
 *----------------------------------------------------------------------------
 *
 * VMCIEventDeliver --
 *
 *      Actually delivers the events to the subscribers.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The callback function for each subscriber is invoked.
 *
 *----------------------------------------------------------------------------
 */

static void
VMCIEventDeliver(VMCIEventMsg *eventMsg)  // IN
{
   ListItem *iter;
   VMCILockFlags flags;

   ASSERT(eventMsg);

   VMCIEventGrabLock(&subscriberLock, &flags);
   LIST_SCAN(iter, subscriberArray[eventMsg->eventData.event]) {
      uint8 eventPayload[sizeof(VMCIEventData_Max)];
      VMCI_EventData *ed;
      VMCISubscription *cur = LIST_CONTAINER(iter, VMCISubscription,
                                             subscriberListItem);
      ASSERT(cur && cur->event == eventMsg->eventData.event);

      /* We set event data before each callback to ensure isolation. */
      memset(eventPayload, 0, sizeof eventPayload);
      memcpy(eventPayload, VMCI_DG_PAYLOAD(eventMsg),
             (size_t)eventMsg->hdr.payloadSize);
      ed = (VMCI_EventData *)eventPayload;
      cur->callback(cur->id, ed, cur->callbackData);
   }
   VMCIEventReleaseLock(&subscriberLock, flags);
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIEvent_Dispatch --
 *
 *      Dispatcher for the VMCI_EVENT_RECEIVE datagrams. Calls all
 *      subscribers for given event.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VMCIEvent_Dispatch(VMCIDatagram *msg)  // IN
{
   VMCIEventMsg *eventMsg = (VMCIEventMsg *)msg;

   ASSERT(msg &&
          msg->src.context == VMCI_HYPERVISOR_CONTEXT_ID &&
          msg->dst.resource == VMCI_EVENT_HANDLER);

   if (msg->payloadSize < sizeof(VMCI_Event) ||
       msg->payloadSize > sizeof(VMCIEventData_Max)) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   if (eventMsg->eventData.event >= VMCI_EVENT_MAX) {
      return VMCI_ERROR_EVENT_UNKNOWN;
   }

#if defined(VMKERNEL)
   /*
    * In the vmkernel we defer delivery of events to a helper world.  This
    * makes the event delivery more consistent across hosts and guests with
    * regard to which locks are held.
    */
   {
      VMCIEventMsg *newEventMsg;

      newEventMsg = VMCI_AllocKernelMem(VMCI_DG_SIZE(&eventMsg->hdr),
                                        VMCI_MEMORY_NONPAGED);
      if (newEventMsg == NULL) {
         return VMCI_ERROR_NO_MEM;
      }

      memcpy(newEventMsg, eventMsg, VMCI_DG_SIZE(&eventMsg->hdr));

      if (Helper_Request(vmciHelperQueue,
                         VMCIEventHelperCB, newEventMsg) != VMK_OK) {
         VMCI_FreeKernelMem(newEventMsg, VMCI_DG_SIZE(&eventMsg->hdr));
         return VMCI_ERROR_GENERIC;
      }
   }
#else
   VMCIEventDeliver(eventMsg);
#endif

   return VMCI_SUCCESS;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIEventRegisterSubscription --
 *
 *      Initialize and add subscription to subscriber list.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
VMCIEventRegisterSubscription(VMCISubscription *sub,   // IN
                              VMCI_Event event,        // IN
                              VMCI_EventCB callback,   // IN
                              void *callbackData)      // IN
{
#  define VMCI_EVENT_MAX_ATTEMPTS 10
   static VMCIId subscriptionID = 0;
   VMCILockFlags flags;
   uint32 attempts = 0;
   int result;
   Bool success;

   ASSERT(sub);

   if (event >= VMCI_EVENT_MAX || callback == NULL) {
      VMCI_DEBUG_LOG(
         ("VMCIEvent: Failed to subscribe to event %d cb %p data %p.\n",
          event, callback, callbackData));
      return VMCI_ERROR_INVALID_ARGS;
   }

   sub->event = event;
   sub->callback = callback;
   sub->callbackData = callbackData;

   VMCIEventGrabLock(&subscriberLock, &flags);
   for (success = FALSE, attempts = 0;
        success == FALSE && attempts < VMCI_EVENT_MAX_ATTEMPTS;
        attempts++) {

      /*
       * We try to get an id a couple of time before claiming we are out of
       * resources.
       */
      sub->id = ++subscriptionID;

      /* Test for duplicate id. */
      if (VMCIEventFind(sub->id) == NULL) {
         /* We succeeded if we didn't find a duplicate. */
         success = TRUE;
      }
   }

   if (success) {
      LIST_QUEUE(&sub->subscriberListItem, &subscriberArray[event]);
      result = VMCI_SUCCESS;
   } else {
      result = VMCI_ERROR_NO_RESOURCES;
   }
   VMCIEventReleaseLock(&subscriberLock, flags);

   return result;
#  undef VMCI_EVENT_MAX_ATTEMPTS
}



/*
 *----------------------------------------------------------------------
 *
 * VMCIEventUnregisterSubscription --
 *
 *      Remove subscription from subscriber list.
 *
 * Results:
 *      VMCISubscription when found, NULL otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static VMCISubscription *
VMCIEventUnregisterSubscription(VMCIId subID)    // IN
{
   VMCILockFlags flags;
   VMCISubscription *s;

   VMCIEventGrabLock(&subscriberLock, &flags);
   s = VMCIEventFind(subID);
   if (s != NULL) {
      LIST_DEL(&s->subscriberListItem, &subscriberArray[s->event]);
   }
   VMCIEventReleaseLock(&subscriberLock, flags);

   return s;
}


/*
 *----------------------------------------------------------------------
 *
 * VMCIEventSubscribe --
 *
 *      Subscribe to given event.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VMCIEventSubscribe(VMCI_Event event,        // IN
                   VMCI_EventCB callback,   // IN
                   void *callbackData,      // IN
                   VMCIId *subscriptionID)  // OUT
{
   int retval;
   VMCISubscription *s = NULL;

   if (subscriptionID == NULL) {
      VMCI_DEBUG_LOG(("VMCIEvent: Invalid arguments.\n"));
      return VMCI_ERROR_INVALID_ARGS;
   }

   s = VMCI_AllocKernelMem(sizeof *s, VMCI_MEMORY_NONPAGED);
   if (s == NULL) {
      return VMCI_ERROR_NO_MEM;
   }

   retval = VMCIEventRegisterSubscription(s, event, callback, callbackData);
   if (retval < VMCI_SUCCESS) {
      VMCI_FreeKernelMem(s, sizeof *s);
      return retval;
   }

   *subscriptionID = s->id;
   return retval;
}


#ifndef VMKERNEL
/*
 *----------------------------------------------------------------------
 *
 * VMCIEvent_Subscribe --
 *
 *      Subscribe to given event.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

#if defined(__linux__)
EXPORT_SYMBOL(VMCIEvent_Subscribe);
#endif

int
VMCIEvent_Subscribe(VMCI_Event event,        // IN
                    VMCI_EventCB callback,   // IN
                    void *callbackData,      // IN
                    VMCIId *subscriptionID)  // OUT
{
   return VMCIEventSubscribe(event, callback, callbackData, subscriptionID);
}
#endif	/* !VMKERNEL  */


/*
 *----------------------------------------------------------------------
 *
 * VMCIEventUnsubscribe --
 *
 *      Unsubscribe to given event. Removes it from list and frees it.
 *      Will return callbackData if requested by caller.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
VMCIEventUnsubscribe(VMCIId subID)   // IN
{
   VMCISubscription *s;

   /*
    * Return subscription. At this point we know noone else is accessing
    * the subscription so we can free it.
    */
   s = VMCIEventUnregisterSubscription(subID);
   if (s == NULL) {
      return VMCI_ERROR_NOT_FOUND;

   }
   VMCI_FreeKernelMem(s, sizeof *s);

   return VMCI_SUCCESS;
}


#ifndef VMKERNEL
/*
 *----------------------------------------------------------------------
 *
 * VMCIEvent_Unsubscribe --
 *
 *      Unsubscribe to given event. Removes it from list and frees it.
 *      Will return callbackData if requested by caller.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

#if defined(__linux__)
EXPORT_SYMBOL(VMCIEvent_Unsubscribe);
#endif

int
VMCIEvent_Unsubscribe(VMCIId subID)   // IN
{
   return VMCIEventUnsubscribe(subID);
}

#endif /* !VMKERNEL  */
