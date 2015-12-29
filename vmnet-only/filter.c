/*********************************************************
 * Copyright (C) 2006 VMware, Inc. All rights reserved.
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

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/socket.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include "compat_skbuff.h"
#include "compat_mutex.h"
#include "compat_semaphore.h"
#include <linux/netdevice.h>
/*
 * All this makes sense only if NETFILTER support is configured in our kernel.
 */
#ifdef CONFIG_NETFILTER

#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/poll.h>

#include "vnetFilter.h"
#include "vnetFilterInt.h"
#include "vnetInt.h"
#include "vmnetInt.h"
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 2, 0)
#include <linux/export.h>
#endif

// VNet_FilterLogPacket.action for dropped packets
#define VNET_FILTER_ACTION_DRP         (1)
#define VNET_FILTER_ACTION_DRP_SHORT   (2)
#define VNET_FILTER_ACTION_DRP_MATCH   (3)
#define VNET_FILTER_ACTION_DRP_DEFAULT (4)

// VNet_FilterLogPacket.action for forwarded packets
#define VNET_FILTER_ACTION_FWD         (1<<8 | 1)
#define VNET_FILTER_ACTION_FWD_LOOP    (1<<8 | 5)
#define VNET_FILTER_ACTION_FWD_MATCH   (1<<8 | 6)
#define VNET_FILTER_ACTION_FWD_DEFAULT (1<<8 | 7)

/* netfilter hooks for filtering. */
static nf_hookfn VNetFilterHookFn;

static struct nf_hook_ops vmnet_nf_ops[] = {
   {  .hook = VNetFilterHookFn,
      compat_nf_hook_owner
      .pf = PF_INET,
      .hooknum = VMW_NF_INET_LOCAL_IN,
      .priority = NF_IP_PRI_FILTER - 1, },
   {  .hook = VNetFilterHookFn,
      compat_nf_hook_owner
      .pf = PF_INET,
      .hooknum = VMW_NF_INET_POST_ROUTING,
      .priority = NF_IP_PRI_FILTER - 1, }
};

/* track if we actually set a callback in IP's filter driver */
static Bool installedFilterCallback = FALSE;

/* rules to use for filtering */
RuleSet *ruleSetHead = NULL;  /* linked list of all rules */
int32 numRuleSets = 0;        /* number of rule sets in ruleSetHead's linked list */
RuleSet *activeRule = NULL;   /* actual rule set for filter callback to use */

/* locks to protect against concurrent accesses. */
static compat_define_mutex(filterIoctlMutex); /* serialize ioctl()s from user space. */
/*
 * user/netfilter hook concurrency lock.
 * This spinlock doesn't scale well if/when in the future the netfilter
 * callbacks can be concurrently executing on multiple threads on multiple
 * CPUs, so we should revisit locking for allowing for that in the future.
 */
DEFINE_SPINLOCK(activeRuleLock);

/*
 * Logging.
 * 
 * All logging for development build uses LOG(2, (KERN_INFO ...)) because the default
 * log level is set to 1 (vnetInt.h). All ACE logging, i.e. policy driven logging, uses
 * printk(KERN_INFO ...).
 */
static uint32 logLevel = VNET_FILTER_LOGLEVEL_NORMAL; /* the current log level */

static void LogPacket(uint16 action, void *header, void *data,
                      uint32 length, Bool drop);
static int InsertHostFilterCallback(void);
static void RemoveHostFilterCallback(void);
static RuleSet *FindRuleSetById(uint32 id, RuleSet ***prevPtr);
static int CreateRuleSet(uint32 id, uint32 defaultAction);
static void DeleteRule(Rule *rule);
static int DeleteRuleSet(uint32 id);
static int ChangeRuleSet(uint32 id, Bool enable, Bool disable, uint32 action);
static int AddIPv4Rule(uint32 id, VNet_AddIPv4Rule *rule,
                       VNet_IPv4Address *addressList,
                       VNet_IPv4Port *portList);


/*
 *----------------------------------------------------------------------
 *
 * DropPacket --
 *
 *      Function is used to record information regarding a packet
 *      being dropped.
 *
 * Results:
 *      void
 *
 * Side effects:
 *      Might store information regarding the packet.
 *
 *----------------------------------------------------------------------
 */

static INLINE void
DropPacket(uint16 action,  // IN: reason code
           void *header,   // IN: packet header
           void *data,     // IN: packet data
           uint32 length)  // IN: packet length
{
   LogPacket(action, header, data, length, TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * ForwardPacket --
 *
 *      Function is used to record information regarding a packet
 *      being forwarded.
 *
 * Results:
 *      void
 *
 * Side effects:
 *      Might store information regarding the packet.
 *
 *----------------------------------------------------------------------
 */

static INLINE void
ForwardPacket(uint16 action,  // IN: reason code
              void *header,   // IN: packet header
              void *data,     // IN: packet data
              uint32 length)  // IN: packet length
{
#ifdef DBG
   LogPacket(action, header, data, length, FALSE);
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * VNetFilterHookFn --
 *
 *      Function is registered as a callback function with the host's
 *      IP stack.  This function can be used to filter on specified protocols
 *      IP addresses, and/or local and remote ports. It makes use of the Linux
 *      netfilter infrastructure, by inserting this function in netfilter at a
 *      priority 1 higher than iptables, so that we don't have to worry about
 *      any existing iptables based firewall rules on the Linux hosts.
 *
 * Results:
 *      NF_ACCEPT or NF_DROP.
 *
 * Side effects:
 *      None besides those described above.
 *
 *----------------------------------------------------------------------
 */

#define DEBUG_HOST_FILTER 0

#if DEBUG_HOST_FILTER
#define HostFilterPrint(a) printk a
#else
#define HostFilterPrint(a)
#endif

static unsigned int
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 13, 0)
VNetFilterHookFn(const struct nf_hook_ops *ops,        // IN:
#else
VNetFilterHookFn(unsigned int hooknum,                 // IN:
#endif
#ifdef VMW_NFHOOK_USES_SKB
                 struct sk_buff *skb,                  // IN:
#else
                 struct sk_buff **pskb,                // IN:
#endif
                 const struct net_device *in,          // IN:
                 const struct net_device *out,         // IN:
                 int (*okfn)(struct sk_buff *))        // IN:
{
#ifndef VMW_NFHOOK_USES_SKB
   struct sk_buff *skb = *pskb;
#endif
   struct iphdr *ip;
   uint32 remoteAddr;
   uint16 localPort;
   uint16 remotePort;
   uint8 *packet;
   uint8 *packetHeader;
   int packetLength;
   RuleSet *currRuleSet;
   Bool blockByDefault;
   Bool transmit; /* TRUE if transmitting, FALSE is receiving */
   Rule *currRule;
   unsigned int verdict = NF_ACCEPT;
   unsigned long flags;


   /* Early checks to see  we should even care. */
   if (skb->protocol != htons(ETH_P_IP)) {
      return verdict;
   }

   spin_lock_irqsave(&activeRuleLock, flags);

   currRuleSet = activeRule;
   // ASSERT(currRuleSet);

   /*
    * Function uses a local copy of ruleSetHead so that we're
    * not adversely affected by any rule changes that might occur
    * while this function is running.
    */

   blockByDefault = currRuleSet->action == VNET_FILTER_RULE_BLOCK;


   /* When the host transmits, hooknum is VMW_NF_INET_POST_ROUTING. */
   /* When the host receives, hooknum is VMW_NF_INET_LOCAL_IN. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 13, 0)
   transmit = (ops->hooknum == VMW_NF_INET_POST_ROUTING);
#else
   transmit = (hooknum == VMW_NF_INET_POST_ROUTING);
#endif

   packetHeader = compat_skb_network_header(skb);
   ip = (struct iphdr*)packetHeader;

   if (transmit) {
      /* skb all set up for us. */
      packet = compat_skb_transport_header(skb);
   } else {
      /* skb hasn't had a chance to be processed by TCP yet. */
      packet = compat_skb_network_header(skb) + (ip->ihl << 2);
   }

   HostFilterPrint(("PacketFilter: IP ver %d ihl %d tos %d len %d id %d\n"
                    "              offset %d ttl %d proto %d xsum %d\n"
                    "              src 0x%08x dest 0x%08x %s\n",
                    ip->version, ip->ihl, ip->tos, ip->tot_len, ip->id,
		    ip->frag_off, ip->ttl, ip->protocol, ip->check,
                    ip->saddr, ip->daddr, transmit ? "OUTGOING":"INCOMING"));

   /*
    * For incoming packets, there should be a skb->dev associated with it, with
    * a populated L2 address length.
    */
   if (skb->dev && skb->dev->hard_header_len) {
      packetLength = skb->len - skb->dev->hard_header_len - (ip->ihl << 2);
   } else {
      /*
       * In certain cases, compat_skb_mac_header() has been observed to be NULL. Don't
       * know why, but in such cases, this calculation will lead to a negative
       * packetLength, and the packet to be dropped.
       */
      packetLength = skb->len - 
                     (compat_skb_network_header(skb) - compat_skb_mac_header(skb)) - 
                     (ip->ihl << 2);
   }

   if (packetLength < 0) {
      HostFilterPrint(("PacketFilter: ill formed packet for IPv4\n"));
      HostFilterPrint(("skb: len %d h.raw %p nh.raw %p mac.raw %p, packetLength %d\n",
                       skb->len, compat_skb_transport_header(skb),
                       compat_skb_network_header(skb),
                       compat_skb_mac_header(skb), packetLength));
      verdict = NF_DROP;
      DropPacket(VNET_FILTER_ACTION_DRP_SHORT, packetHeader, packet, 0);
      goto out_unlock;
   }

   remoteAddr = transmit ? ip->daddr : ip->saddr;

   /* always allow 127/8. */
   if ((remoteAddr & 0xff) == 127) {
      HostFilterPrint(("PacketFilter: allowing %s loopback 0x%08x\n",
                       transmit ? "outgoing" : "incoming",
                       remoteAddr));
      ForwardPacket(VNET_FILTER_ACTION_FWD_LOOP,
                    packetHeader, packet, packetLength);
      goto out_unlock;
   }

   /* If we're dealing with TCP or UDP, then extract the port information */
   if (ip->protocol == IPPROTO_TCP || ip->protocol == IPPROTO_UDP) {
      uint16 srcPort, dstPort; /* used to extract port information from packet */

      if (packetLength < 4) {
         HostFilterPrint(("PacketFilter: payload too short for "
                          "TCP or UDP: %d\n", packetLength));
         verdict = NF_DROP;
         DropPacket(VNET_FILTER_ACTION_DRP_SHORT,
                    packetHeader, packet, packetLength);
         goto out_unlock;
      }

      /* Retrieve UDP/TCP port info */
      srcPort = *((uint16*)&packet[0]);
      dstPort = *((uint16*)&packet[2]);

      if (transmit) { /* transmit */
         localPort = ntohs(srcPort);
         remotePort = ntohs(dstPort);
      } else { /* receive */
         localPort = ntohs(dstPort);
         remotePort = ntohs(srcPort);
      }

      HostFilterPrint(("PacketFilter: got local port %d remote port %d\n",
                       localPort, remotePort));
   } else {
      /* these mostly exist to silence compiler warning about uninit variables */
      localPort = 0;
      remotePort = 0;
   }

   currRule = currRuleSet->list;

   /* traverse all the rules in the rule set */
   while (currRule != NULL) {
      uint32 i;
      Bool matchedAddress;

      /* if direction doesn't match rule, then skip */
      if ((currRule->direction == VNET_FILTER_DIRECTION_IN && transmit) ||
          (currRule->direction == VNET_FILTER_DIRECTION_OUT && !transmit)) {
         HostFilterPrint(("PacketFilter: didn't match direction\n"));
         /* wrong direction */
         goto skipRule;
      }

      /*
       * Check if the packet's address matches the rule.  If the list is empty
       * then this means we don't care about address and it's considered a match.
       */

      matchedAddress = (currRule->addressListLen == 0); /* empty list means don't care */
      for (i = 0; i < currRule->addressListLen; ++i) {
         if ((remoteAddr & currRule->addressList[i].ipv4Mask) ==
             currRule->addressList[i].ipv4Addr) {
            matchedAddress = TRUE;
            HostFilterPrint(("PacketFilter: rule matched ip addr %u: "
                             "0x%08x == 0x%08x\n", i, remoteAddr,
                             currRule->addressList[i].ipv4Addr));
            break;
         } else {
            HostFilterPrint(("PacketFilter: rule not match ip addr %u: "
                             "0x%08x != 0x%08x\n", i, remoteAddr,
                             currRule->addressList[i].ipv4Addr));
         }
      }
      if (!matchedAddress) {
         HostFilterPrint(("PacketFilter: rule didn't match ip addr 0x%08x\n",
                          remoteAddr));
         /* ip addr doesn't match */
         goto skipRule;
      }

      /*
       * Check the protocol. ~0 (0xffff) means we don't care about the
       * protocol and it's considered a match.
       */

      if (currRule->proto != 0xffff && currRule->proto != ip->protocol) {
         HostFilterPrint(("PacketFilter: didn't match protocol: %u != %u\n",
                          ip->protocol, currRule->proto));
         /* protocol doesn't match */
         goto skipRule;
      }

      /*
       * If the protocol is TCP or UDP then check the port list.  If the list is empty
       * then this means we don't care about ports and it's considered a match.
       */

      if (currRule->proto == IPPROTO_TCP || currRule->proto == IPPROTO_UDP) {

         /* An empty list means the rule don't care about port numbers*/
         Bool matchedPort = (currRule->portListLen == 0);

         for (i = 0; i < currRule->portListLen; ++i) {
            RulePort *portRule = currRule->portList + i;
            Bool matchedLocal, matchedRemote; /* improves readability */

            /*
             * It's presumed that if portRule->localPortLow == ~0 then
             * portRule->localPortHigh == ~0.  Similiar story for the
             * remote ports.
             */
            matchedLocal = (localPort >= portRule->localPortLow &&
                            localPort <= portRule->localPortHigh) ||
                           portRule->localPortLow == ~0;
            matchedRemote = (remotePort >= portRule->remotePortLow &&
                             remotePort <= portRule->remotePortHigh) ||
                            portRule->remotePortLow == ~0;

            if (matchedLocal && matchedRemote) {
               HostFilterPrint(("PacketFilter: matched rule's "
                                "port element %u\n", i));
               matchedPort = TRUE;
               break;
            }
            HostFilterPrint(("PacketFilter: didn't match rule's "
                             "port element %u\n", i));
            HostFilterPrint(("-- local  %4u not in range [%4u, %4u] or \n",
                             localPort, portRule->localPortLow,
                             portRule->localPortHigh));
            HostFilterPrint(("-- remote %4u not in range [%4u, %4u]\n",
                             remotePort, portRule->remotePortLow,
                             portRule->remotePortHigh));
         }
         if (!matchedPort) {
            HostFilterPrint(("PacketFilter: rule didn't match port "
                             "(local %u remote %u)\n", localPort, remotePort));
            /* port doesn't match */
            goto skipRule;
         }
      }

      /* rule matches so follow orders */

      if (currRule->action == VNET_FILTER_RULE_ALLOW) {
         HostFilterPrint(("PacketFilter: found match, forwarding\n"));
         ForwardPacket(VNET_FILTER_ACTION_FWD_MATCH,
                       packetHeader, packet, packetLength);
         goto out_unlock;
      } else {
         HostFilterPrint(("PacketFilter: found match, dropping\n"));
         verdict = NF_DROP;
         DropPacket(VNET_FILTER_ACTION_DRP_MATCH,
                    packetHeader, packet, packetLength);
         goto out_unlock;
      }

skipRule:
      currRule = currRule->next;
   }

   /* Forward or drop packet based on the default rule */
   HostFilterPrint(("PacketFilter: Didn't find match for %s "
                    "%u.%u.%u.%u, %s packet\n",
                    transmit ? "outgoing" : "incoming",
                    remoteAddr & 0xff, (remoteAddr >> 8) & 0xff,
                    (remoteAddr >> 16) & 0xff, (remoteAddr >> 24) & 0xff,
                    blockByDefault ? "drop" : "forward"));

   if (blockByDefault) {
      verdict = NF_DROP;
      DropPacket(VNET_FILTER_ACTION_DRP_DEFAULT,
                 packetHeader, packet, packetLength);
   } else {
      ForwardPacket(VNET_FILTER_ACTION_FWD_DEFAULT,
                    packetHeader, packet, packetLength);
   }
out_unlock:
   spin_unlock_irqrestore(&activeRuleLock, flags);
   return verdict;
}


/*
 *----------------------------------------------------------------------
 *
 * InsertHostFilterCallback --
 *
 *      Function registers a hook in the host's IP stack.
 *
 * Results:
 *      0 on success (or if hook already installed),
 *      errno on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
InsertHostFilterCallback(void)
{
   uint32 i;
   int retval = 0;

   LOG(2, (KERN_INFO "vnet filter inserting callback\n"));

   if (installedFilterCallback) {
      LOG(2, (KERN_INFO "vnet filter callback already registered\n"));
      goto end;
   }

   /* Register netfilter hooks. */
   for (i = 0; i < ARRAY_SIZE(vmnet_nf_ops); i++) {
      if ((retval = nf_register_hook(&vmnet_nf_ops[i])) >= 0) {
         continue;
      }
      /* Encountered an error, back out. */
      LOG(2, (KERN_INFO "vnet filter failed to register callback %d: %d\n",
              i, retval));
      while (i--) {
         nf_unregister_hook(&vmnet_nf_ops[i]);
      }
      goto end;
   }
   installedFilterCallback = TRUE;
   LOG(2, (KERN_INFO "Successfully set packet filter function\n"));

end:
   return retval;
}


/*
 *----------------------------------------------------------------------
 *
 * RemoveHostFilterCallback --
 *
 *      Function deregisters a hook in the host's IP stack.
 *
 * Results:
 *      void
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
RemoveHostFilterCallback(void)
{
   int i;

   LOG(2, (KERN_INFO "vnet filter removing callback\n"));

   if (installedFilterCallback) {
      LOG(2, (KERN_INFO "filter callback was installed: removing filter\n"));
      for (i = ARRAY_SIZE(vmnet_nf_ops) - 1; i >= 0; i--) {
         nf_unregister_hook(&vmnet_nf_ops[i]);
      }
      installedFilterCallback = FALSE;
   }
   LOG(2, (KERN_INFO "vnet filter remove callback done\n"));
}


/*
 *----------------------------------------------------------------------
 *
 * FindRuleSetById --
 *
 *      Function is given an ID for a rule set, and returns a
 *      pointer to the ruleset with that ID.  The function can
 *      optionally report what pointer is pointing to this item
 *      (suitable for removing the item from the linked list -- the
 *      result might be the prior item's next pointer, or the head).
 *
 * Results:
 *      NULL if rule set not found, otherwise pointer to rule set.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static RuleSet *
FindRuleSetById(uint32 id,          // IN: id to locate
                RuleSet ***prevPtr) // OUT: pointer to the ->next pointer
                                    //      (or head) that points to the
                                    //      returned item (optional)
{
   RuleSet *curr;
   RuleSet **prev = NULL;
   // ASSERT(id != 0);

   curr = ruleSetHead;
   prev = &ruleSetHead;
   while (curr != NULL) {
      if (curr->id == id) {
         LOG(2, (KERN_INFO "Found id %u at %p\n", id, curr));
         if (prevPtr != NULL) {
            *prevPtr = prev;
         }
         return curr;
      }
      prev = &curr->next;
      curr = curr->next;
   }
   LOG(2, (KERN_INFO "Didn't find ruleset with id %u\n", id));
   /* won't overwrite *prevPtr with NULL */
   return NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * CreateRuleSet --
 *
 *      Function creates a new rule set with a specified ID and
 *      default action.  Call will fail if failed to alloc memory,
 *      or if ID is already in use, or if maximum number of
 *      rule sets have already been created.
 *
 * Results:
 *      Returns 0 on success, and otherwise returns errno.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
CreateRuleSet(uint32 id,            // IN: requested ID for new rule set
              uint32 defaultAction) // IN: default action for rule set
{
   RuleSet *newRuleSet;
   RuleSet *curr;

   /* check if too many rule sets already exist */
   if (numRuleSets >= MAX_RULE_SETS) {
      LOG(2, (KERN_INFO "filter already has all rules (%u of %u) allocated\n",
              numRuleSets, MAX_RULE_SETS));
      return -EOVERFLOW;
   }

   /* check if ID is already in use */
   curr = FindRuleSetById(id, NULL);
   if (curr != NULL) {
      LOG(2, (KERN_INFO "filter already has id %u\n", id));
      return -EEXIST;
   }

   /* allocate and init new rule set */
   newRuleSet = kmalloc(sizeof *newRuleSet, GFP_USER);
   if (newRuleSet == NULL) {
      LOG(2, (KERN_INFO "filter mem alloc failed\n"));
      return -ENOMEM;
   }

   memset(newRuleSet, 0, sizeof *newRuleSet);
   newRuleSet->next = ruleSetHead;
   newRuleSet->id = id;
   newRuleSet->enabled = FALSE;
   newRuleSet->action = (uint16)defaultAction;
   newRuleSet->list = NULL;
   newRuleSet->numRules = 0;
   newRuleSet->tail = &newRuleSet->list;

   /* add new rule set to head of linked list */
   numRuleSets++;
   ruleSetHead = newRuleSet;
   LOG(2, (KERN_INFO "filter created ruleset with id %u\n", id));
   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * DeleteRule --
 *
 *      Function frees the memory in a Rule object.  This function
 *      frees the arrays in the Rule, but not an elements that
 *      are chained on the linked-list via 'next'.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
DeleteRule(Rule *rule)          // IN: Rule to delete.
{
   // ASSERT(rule);

   if (!rule) {
      return;
   }
   if (rule->addressList) {
      kfree(rule->addressList);
      rule->addressList = NULL;
   }
   if (rule->portList) {
      kfree(rule->portList);
      rule->portList = NULL;
   }
   kfree(rule);
}


/*
 *----------------------------------------------------------------------
 *
 * DeleteRuleSet --
 *
 *      Function deletes a rule set with a specified ID. Call will fail
 *      if ID not found or if the current rule set is being used for
 *      filtering.
 *
 * Results:
 *      Returns 0 on success, errno on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
DeleteRuleSet(uint32 id) // IN: ID of new rule set to delete
{
   RuleSet **prev = NULL;
   RuleSet *curr;
   Rule *currRule;

   /* locate the ruleset with the specified ID */
   curr = FindRuleSetById(id, &prev);
   if (curr == NULL) {
      LOG(2, (KERN_INFO "filter did not find id %u to delete\n", id));
      return -ESRCH;
   }

   LOG(2, (KERN_INFO "found id %u\n", id));

   /* check if in use */
   if (curr->enabled) {
      LOG(2, (KERN_INFO "Can't delete id %u since enabled\n", id));
      return -EBUSY;
   }

   /* remove item from linked list */
   *prev = curr->next;

   /* free rules in rule set */
   currRule = curr->list;
   curr->list = NULL; /* help mitigate any bugs or races */
   while (currRule) {
      Rule *temp = currRule->next;
      currRule->next = NULL; /* help mitigate any bugs or races */
      DeleteRule(currRule);
      currRule = temp;
   }

   kfree(curr);
   numRuleSets--;
   // ASSERT(numRuleSets >= 0);
   return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * ChangeRuleSet --
 *
 *      This function is used to specify which rule set is to be used
 *      for filtering (or stop using for filtering).  If another
 *      rule set is currently used for filtering then the specified
 *      rule set will replace it.  This funciton can also be used to
 *      change the default action for any rule set, but this option
 *      should not be used when disabling a rule set.
 *
 *      Call will fail if ID can't be found, or when attempting to
 *      disable a rule set that's not enabled.
 *
 * Results:
 *      Returns 0 on success, errno on failure.
 *
 * Side effects:
 *      May add/remove filter callback.
 *
 *----------------------------------------------------------------------
 */

static int
ChangeRuleSet(uint32 id,     // IN: requested ID of rule set
              Bool enable,   // IN: TRUE says start using this rule for filtering
              Bool disable,  // IN: TRUE says stop using this rule for filtering
              uint32 action) // IN: default action for rule set
{
   RuleSet *curr;
   int retval;
   unsigned long flags;

   // ASSERT(!enable || !disable); /* at most one can be set */

   LOG(2, (KERN_INFO "changeruleset %d enable %d disable %d action %x\n", id,
           enable, disable, action));
   /* locate the specified rule set */
   curr = FindRuleSetById(id, NULL);
   if (curr == NULL) {
      LOG(2, (KERN_INFO "vnet filter can't find ruleset: %u\n", id));
      return -ESRCH;
   }

   if (enable) {
      RuleSet *oldActive;

      if (action != VNET_FILTER_RULE_NO_CHANGE) {
         LOG(2, (KERN_INFO "vnet filter changing default action "
                 "of active rule set: %u (id %u)\n", action, id));
         curr->action = (uint16)action;
      }

      /* enable new rule */
      curr->enabled = TRUE;

      /* Grab activeRule spinlock. */
      spin_lock_irqsave(&activeRuleLock, flags);

      LOG(2, (KERN_INFO "changing active rule from "
              "%p (%u) to %p (%u)\n", activeRule,
              activeRule ? activeRule->id : 0,
              curr, curr->id));

      /* make rule active */
      oldActive = activeRule;
      activeRule = curr;

      /* Safe to release activeRule spinlock now. */
      spin_unlock_irqrestore(&activeRuleLock, flags);

      /*
       * Mark old rule as not enabled, except if it's the same
       * as the newly enabled rule set.
       */

      if (oldActive == NULL) {
         // 1) activate (no current active)
         LOG(2, (KERN_INFO "No prior rule was active\n"));
      } else if (oldActive == curr) {
         // 2) activate (current active, and same as this one)
         LOG(2, (KERN_INFO "Activated rule that was already active\n"));
      } else { /* oldActive != NULL && oldActive != curr */
         // 3) activate (current active, and different than this one)
         LOG(2, (KERN_INFO "Deactivating old rule: %p (id %u)\n",
                 oldActive, oldActive->id));
         oldActive->enabled = FALSE;
      }
      if ((retval = InsertHostFilterCallback()) != 0) {
         LOG(2, (KERN_INFO "Failed to insert filter in IP\n"));
      }

   } else if (disable) {

      if (!curr->enabled) {
         // 4) deactive (but not currently active)
         LOG(2, (KERN_INFO "vnet filter tried to deactive a "
                 "non-active rule: %u\n", id));
         if (activeRule) {
            // ASSERT(activeRule != curr);
            LOG(2, (KERN_INFO "-- current active is %p (id %u)\n",
                    activeRule, activeRule->id));
         } else {
            LOG(2, (KERN_INFO "-- no rule is currently active\n"));
         }
         /* in this case we'll also not change the default action */
         return -EINVAL;
      }

      // 5) deactive (and currently active)
      LOG(2, (KERN_INFO "vnet filter deactivating %p (id %u)\n",
              curr, id));

      RemoveHostFilterCallback();

      // ASSERT(activeRule == curr);
      /* Grab activeRule spinlock. */
      spin_lock_irqsave(&activeRuleLock, flags);
      activeRule = NULL;
      /* Safe to release activeRule spinlock now. */
      spin_unlock_irqrestore(&activeRuleLock, flags);
      curr->enabled = FALSE;
      if (action != VNET_FILTER_RULE_NO_CHANGE) {
         LOG(2, (KERN_INFO "vnet filter changing default action: "
                 "%u (id %u)\n", action, id));
         curr->action = (uint16)action;
      }
      retval = 0;

   } else { /* !enable && !disable */

      if (action == VNET_FILTER_RULE_NO_CHANGE) {
         // 6) no activate change (and default not changed)
         LOG(2, (KERN_INFO "vnet filter got nothing to change\n"));
         retval = 0;
      }

      // 7) no activate change (but default action changed)
      curr->action = (uint16)action;
      LOG(2, (KERN_INFO "vnet filter changed action: %u\n", action));
      retval = 0;
   }

   return retval;
}


/*
 *----------------------------------------------------------------------
 *
 * AddIPv4Rule --
 *
 *      Function is used to add an IPv4 rule to a rule set.
 *      Call will fail if failed to alloc memory, or if specified
 *      ID was not found.  The actual rule is not sanity checked,
 *      as it's presumed the caller did this.
 *
 * Results:
 *      Returns 0 on success, errno on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
AddIPv4Rule(uint32 id,                       // IN: requested ID of rule set
            VNet_AddIPv4Rule *rule,          // IN: rule to add
            VNet_IPv4Address *addressList,   // IN: list of addresses
            VNet_IPv4Port *portList)         // IN: list of ports
{
   Rule *newRule;
   RuleSet *curr;

   // ASSERT(rule && addressList && portList);

   /* locate the rule set with the specified ID */
   curr = FindRuleSetById(id, NULL);
   if (curr == NULL) {
      LOG(2, (KERN_INFO "vnet filter can't find ruleset: %u\n", id));
      return -ESRCH;
   }

   /* make sure that we don't have too many rules already */
   if (curr->numRules >= MAX_RULES_PER_SET) {
      LOG(2, (KERN_INFO "vnet filter has too many rules in ruleset: %u >= %u\n",
              curr->numRules, MAX_RULES_PER_SET));
      return -EOVERFLOW;
   }

   /* allocate and init rule */
   newRule = kmalloc(sizeof *newRule, GFP_USER);
   if (newRule == NULL) {
      LOG(2, (KERN_INFO "vnet filter mem alloc failed for rule\n"));
      return -ENOMEM;
   }
   memset(newRule, 0, sizeof *newRule);

   newRule->action = (uint16)rule->action;
   newRule->direction = (uint16)rule->direction;
   newRule->proto = (uint16)rule->proto;

   // ASSERT(rule->addressListLen <= 255); /* double-check for data truncation */
   newRule->addressListLen = (uint8)rule->addressListLen;
   if (newRule->addressListLen == 1 &&
       addressList[0].ipv4RemoteAddr == 0 &&
       addressList[0].ipv4RemoteMask == 0) {
      newRule->addressListLen = 0;
      LOG(2, (KERN_INFO "vnet filter address has single don't care rule\n"));
   }

   // ASSERT(rule->portListLen <= 255); /* double-check for data truncation */
   newRule->portListLen = (uint8)rule->portListLen;
   if (newRule->portListLen == 1 &&
       portList[0].localPortLow == ~0 &&
       portList[0].localPortHigh == ~0 &&
       portList[0].remotePortLow == ~0 &&
       portList[0].remotePortHigh == ~0) {
      newRule->portListLen = 0;
      LOG(2, (KERN_INFO "vnet filter port has single don't care rule\n"));
   }

   if (newRule->addressListLen > 0) {
      uint32 i;

      newRule->addressList =
         kmalloc(sizeof(*newRule->addressList) * newRule->addressListLen,
                 GFP_USER);
      if (newRule->addressList == NULL) {
         LOG(2, (KERN_INFO "vnet filter mem alloc failed for rule address\n"));
         DeleteRule(newRule);
         return -ENOMEM;
      }

      /* could use memcpy(), but this insulates against API changes */
      for (i = 0; i < newRule->addressListLen; ++i) {
         newRule->addressList[i].ipv4Addr = addressList[i].ipv4RemoteAddr;
         newRule->addressList[i].ipv4Mask = addressList[i].ipv4RemoteMask;
      }
   }

   if (newRule->portListLen > 0) {
      uint32 i;

      newRule->portList =
         kmalloc(sizeof(*newRule->portList) * newRule->portListLen, GFP_USER);
      if (newRule->portList == NULL) {
         LOG(2, (KERN_INFO "vnet filter mem alloc failed for rule port\n"));
         DeleteRule(newRule);
         return -ENOMEM;
      }

      /* could use memcpy(), but this insulates against API changes */
      for (i = 0; i < newRule->portListLen; ++i) {
         newRule->portList[i].localPortLow   = portList[i].localPortLow;
         newRule->portList[i].localPortHigh  = portList[i].localPortHigh;
         newRule->portList[i].remotePortLow  = portList[i].remotePortLow;
         newRule->portList[i].remotePortHigh = portList[i].remotePortHigh;
      }
   }

   LOG(2, (KERN_INFO "adding rule with %u addresses and %u ports\n",
           newRule->addressListLen, newRule->portListLen));

   /* add rule to rule set */
   newRule->next = NULL;
   *(curr->tail) = newRule;
   curr->tail = &(newRule->next);
   ++curr->numRules;

   LOG(2, (KERN_INFO "Added rule %p to set %p, count now %u\n",
           newRule, curr, curr->numRules));

   return 0;
}


/*
 *----------------------------------------------------------------------------
 *
 * VNetFilter_HandleUserCall --
 *
 *      Handle the subcommands from the SIOCSFILTERRULES ioctl command.
 *      We end up copying the VNet_RuleHeader bytes twice from userland,
 *      once from the calling function, and once here after we've figured out
 *      what sub-command we are dealing with.
 *
 * Returns:
 *      0 on success.
 *      errno on failure.
 *
 * Side effects:
 *      May add/remove filter callback.
 *
 *----------------------------------------------------------------------------
 */

int
VNetFilter_HandleUserCall(VNet_RuleHeader *ruleHeader,      // IN: command header
                          unsigned long ioarg)              // IN: ptr to user data
{
   int retval = 0;

   /* Serialize all ioctl()s. */
   retval = compat_mutex_lock_interruptible(&filterIoctlMutex);
   if (retval != 0) {
      return retval;
   }

   switch (ruleHeader->type) {

      case VNET_FILTER_CMD_CREATE_RULE_SET: {
         VNet_CreateRuleSet createRequest;
         if (copy_from_user(&createRequest, (void *)ioarg, sizeof createRequest)) {
            retval = -EFAULT;
            goto out_unlock;
         }
         /* Validate size. */
         if (createRequest.header.len != sizeof createRequest) {
            LOG(2, (KERN_INFO "invalid length %d/%zd for create filter "
                    "request\n", createRequest.header.len,
                    sizeof createRequest));
            retval = -EINVAL;
            goto out_unlock;
         }
         if (createRequest.ruleSetId == 0) {
            LOG(2, (KERN_INFO "invalid id %u for create filter request\n",
                    createRequest.ruleSetId));
            retval = -EINVAL;
            goto out_unlock;
         }
         if (createRequest.defaultAction != VNET_FILTER_RULE_BLOCK &&
             createRequest.defaultAction != VNET_FILTER_RULE_ALLOW) {
            LOG(2, (KERN_INFO "invalid action %u for create filter request\n",
                    createRequest.defaultAction));
            retval = -EINVAL;
            goto out_unlock;
         }
         retval = CreateRuleSet(createRequest.ruleSetId,
                                createRequest.defaultAction);
         goto out_unlock;
      }

      case VNET_FILTER_CMD_DELETE_RULE_SET: {
         VNet_DeleteRuleSet deleteRequest;
         if (copy_from_user(&deleteRequest, (void *)ioarg, sizeof deleteRequest)) {
            retval = -EFAULT;
            goto out_unlock;
         }
         /* Validate size. */
         if (deleteRequest.header.len != sizeof deleteRequest) {
            LOG(2, (KERN_INFO "invalid length %d/%zd for delete filter "
                    "request\n", deleteRequest.header.len,
                    sizeof deleteRequest));
            retval = -EINVAL;
            goto out_unlock;
         }
         if (deleteRequest.ruleSetId == 0) {
            LOG(2, (KERN_INFO "invalid id %u for delete filter request\n",
                    deleteRequest.ruleSetId));
            retval = -EINVAL;
            goto out_unlock;
         }
         retval = DeleteRuleSet(deleteRequest.ruleSetId);
         goto out_unlock;
      }

      case VNET_FILTER_CMD_CHANGE_RULE_SET: {
         VNet_ChangeRuleSet changeRequest;

         if (copy_from_user(&changeRequest, (void *)ioarg, sizeof changeRequest)) {
            retval = -EFAULT;
            goto out_unlock;
         }
         /* Validate size. */
         if (changeRequest.header.len != sizeof changeRequest) {
            LOG(2, (KERN_INFO "invalid length %d/%zd for change filter "
                    "request\n", changeRequest.header.len,
                    sizeof changeRequest));
            retval = -EINVAL;
            goto out_unlock;
         }
         if (changeRequest.ruleSetId == 0) {
            LOG(2, (KERN_INFO "invalid id %u for change filter request\n",
                    changeRequest.ruleSetId));
            retval = -EINVAL;
            goto out_unlock;
         }
         if (changeRequest.defaultAction != VNET_FILTER_RULE_NO_CHANGE &&
             changeRequest.defaultAction != VNET_FILTER_RULE_BLOCK &&
             changeRequest.defaultAction != VNET_FILTER_RULE_ALLOW) {
            LOG(2, (KERN_INFO "invalid default action %u for change "
                    "filter request\n", changeRequest.defaultAction));
            retval = -EINVAL;
            goto out_unlock;
         }
         if (changeRequest.activate != VNET_FILTER_STATE_NO_CHANGE &&
             changeRequest.activate != VNET_FILTER_STATE_ENABLE &&
             changeRequest.activate != VNET_FILTER_STATE_DISABLE) {
            LOG(2, (KERN_INFO "invalid activate %u for change filter "
                    "request\n", changeRequest.activate));
            retval = -EINVAL;
            goto out_unlock;
         }
         retval = ChangeRuleSet(changeRequest.ruleSetId,
                                changeRequest.activate == VNET_FILTER_STATE_ENABLE,
                                changeRequest.activate == VNET_FILTER_STATE_DISABLE,
                                changeRequest.defaultAction);
         goto out_unlock;

      }

      case VNET_FILTER_CMD_ADD_IPV4_RULE: {
         VNet_AddIPv4Rule *addRequest;
         VNet_IPv4Address *addressList = NULL;
         VNet_IPv4Port *portList = NULL;
         int error = -EINVAL;
         uint32 i;

         /* Validate size. */
         if (ruleHeader->len < sizeof *addRequest) {
            LOG(2, (KERN_INFO "short length %d/%zd for add filter rule "
                    "request\n", ruleHeader->len,
                    sizeof *addRequest));
            retval = -EINVAL;
            goto out_unlock;
         }
         if (ruleHeader->len > (sizeof *addRequest +
                                (sizeof *addressList * MAX_ADDR_PER_RULE) +
                                (sizeof *portList * MAX_PORT_PER_RULE))) {
            LOG(2, (KERN_INFO "long length %d for add filter rule "
                    "request\n", ruleHeader->len));
            retval = -EINVAL;
            goto out_unlock;
         }
         addRequest = kmalloc(ruleHeader->len, GFP_USER);
         if (!addRequest) {
            LOG(2, (KERN_INFO "couldn't allocate memory to add filter rule\n"));
            retval = -ENOMEM;
            goto out_unlock;
         }

         if (copy_from_user(addRequest, (void *)ioarg, ruleHeader->len)) {
            error = -EFAULT;
            goto out_error;
         }
         if (addRequest->addressListLen <= 0 ||
             addRequest->addressListLen > MAX_ADDR_PER_RULE) {
            LOG(2, (KERN_INFO "add filter rule: invalid addr list length: %u\n",
                    addRequest->addressListLen));
            goto out_error;
         }
         if (addRequest->portListLen <= 0 ||
             addRequest->portListLen > MAX_PORT_PER_RULE) {
            LOG(2, (KERN_INFO "add filter rule: invalid port list length: %u\n",
                    addRequest->portListLen));
            goto out_error;
         }
         if (addRequest->header.len !=
             (sizeof *addRequest +
              addRequest->addressListLen * sizeof(VNet_IPv4Address) +
              addRequest->portListLen * sizeof(VNet_IPv4Port))) {
            LOG(2, (KERN_INFO "add filter rule: invalid length: %u != %zu\n",
                    addRequest->header.len, sizeof *addRequest +
                    addRequest->addressListLen * sizeof(VNet_IPv4Address) +
                    addRequest->portListLen * sizeof(VNet_IPv4Port)));
            goto out_error;
         }

         /*
          * The address list comes after initial struct, and port
          * list follows the address list.
          */
         addressList = (VNet_IPv4Address *)(addRequest + 1);
         portList = (VNet_IPv4Port *)(addressList + addRequest->addressListLen);

         if (addRequest->ruleSetId == 0) {
            LOG(2, (KERN_INFO "add filter rule: invalid request id %u\n",
                    addRequest->ruleSetId));
            goto out_error;
         }
         if (addRequest->action != VNET_FILTER_RULE_BLOCK &&
             addRequest->action != VNET_FILTER_RULE_ALLOW) {
            LOG(2, (KERN_INFO "add filter rule: invalid action %u\n",
                    addRequest->action));
            goto out_error;
         }

         if (addRequest->direction != VNET_FILTER_DIRECTION_IN &&
             addRequest->direction != VNET_FILTER_DIRECTION_OUT &&
             addRequest->direction != VNET_FILTER_DIRECTION_BOTH) {
            LOG(2, (KERN_INFO "add filter rule: invalid direction %u\n",
                    addRequest->direction));
            goto out_error;
         }

         /*
          * Make sure addr is sane for given mask.  Also verify that the address
          * and mask, if both zero, are in the first element and the array only
          * has one element. This also means that a 0 mask is not allowed in any
          * element besides the first.
          */
         for (i = 0; i < addRequest->addressListLen; i++) {
            if (addressList[i].ipv4RemoteAddr !=
                (addressList[i].ipv4RemoteAddr & addressList[i].ipv4RemoteMask)) {
               LOG(2, (KERN_INFO "add filter rule got address 0x%08x mask "
                       "0x%08x for %u\n", addressList[i].ipv4RemoteAddr,
                       addressList[i].ipv4RemoteMask, i));
               addressList[i].ipv4RemoteAddr &= addressList[i].ipv4RemoteMask;
               LOG(2, (KERN_INFO "-- changed address to 0x%08x\n",
                       addressList[i].ipv4RemoteAddr));
            }

            /*
             * If addr==mask==0, then it must be in the first element of the
             * address list, and the address list should have only one element.
             */
            if (addressList[i].ipv4RemoteAddr == 0 &&
                addressList[i].ipv4RemoteMask == 0 &&
                (i > 0 || addRequest->addressListLen > 1)) {
               LOG(2, (KERN_INFO "add filter rule got violation for zero IP "
                       "addr/mask\n"));
               goto out_error;
            }
         }

         if (addRequest->proto > 0xFF && addRequest->proto != (uint16)~0) {
            LOG(2, (KERN_INFO "add filter rule got invalid proto %u\n",
                    addRequest->proto));
            goto out_error;
         }

         if (addRequest->proto == IPPROTO_TCP ||
             addRequest->proto == IPPROTO_UDP) {

            for (i = 0; i < addRequest->portListLen; i++) {

               if (portList[i].localPortLow > 0xFFFF &&
                   portList[i].localPortLow != ~0) {
                  LOG(2, (KERN_INFO "add filter rule invalid localPortLow %u\n",
                          portList[i].localPortLow));
                  goto out_error;
               }
               if (portList[i].localPortHigh > 0xFFFF &&
                   portList[i].localPortHigh != ~0) {
                  LOG(2, (KERN_INFO "add filter rule invalid localPortHigh %u\n",
                          portList[i].localPortHigh));
                  goto out_error;
               }
               if (portList[i].remotePortLow > 0xFFFF &&
                   portList[i].remotePortLow != ~0) {
                  LOG(2, (KERN_INFO "add filter rule invalid remotePortLow %u\n",
                          portList[i].remotePortLow));
                  goto out_error;
               }
               if (portList[i].remotePortHigh > 0xFFFF &&
                   portList[i].remotePortHigh != ~0) {
                  LOG(2, (KERN_INFO "add filter rule invalid remotePortHigh %u\n",
                          portList[i].remotePortHigh));
                  goto out_error;
               }

               /*
                * Make sure both low and high ports of a port range specify don't
                * care ports.
                */
               if ((portList[i].localPortLow   == ~0 && portList[i].localPortHigh  != ~0) ||
                   (portList[i].localPortLow   != ~0 && portList[i].localPortHigh  == ~0) ||
                   (portList[i].remotePortLow  == ~0 && portList[i].remotePortHigh != ~0) ||
                   (portList[i].remotePortLow  != ~0 && portList[i].remotePortHigh == ~0)) {
                  LOG(2, (KERN_INFO "add filter rule mismatch in don't care "
                          "status of ports\n"));
                  LOG(2, (KERN_INFO " -- srcLow %u srcHigh %u dstLow %u dstHigh %u\n",
                          portList[i].localPortLow, portList[i].localPortHigh,
                          portList[i].remotePortLow, portList[i].remotePortHigh));
                  goto out_error;
               }
               if (portList[i].localPortHigh  < portList[i].localPortLow ||
                   portList[i].remotePortHigh < portList[i].remotePortLow) {
                  LOG(2, (KERN_INFO "add filter rule high < low on ports\n"));
                  LOG(2, (KERN_INFO " -- srcLow %u srcHigh %u dstLow %u dstHigh %u\n",
                          portList[i].localPortLow,  portList[i].localPortHigh,
                          portList[i].remotePortLow, portList[i].remotePortHigh));
                  goto out_error;
               }
               /*
                * Only allow a don't care on port ranges when it is the only port
                * range specified.
                */
               if (portList[i].localPortLow   == ~0 && portList[i].localPortHigh  == ~0 &&
                   portList[i].remotePortLow  == ~0 && portList[i].remotePortHigh == ~0 &&
                   (i > 0 || addRequest->portListLen > 1)) {
                  LOG(2, (KERN_INFO "add filter rule incorrect don't "
                          "care on port list\n"));
                  goto out_error;
               }
            }

         } else {                  // proto not TCP or UDP
            if (addRequest->portListLen != 1 ||
                (portList[0].localPortLow   !=  0 &&
                 portList[0].localPortLow   != ~0) ||
                (portList[0].localPortHigh  !=  0 &&
                 portList[0].localPortHigh  != ~0) ||
                (portList[0].remotePortLow  !=  0 &&
                 portList[0].remotePortLow  != ~0) ||
                (portList[0].remotePortHigh !=  0 &&
                 portList[0].remotePortHigh != ~0)) {
               LOG(2, (KERN_INFO "add filter rule missing/unnecessary port "
                       "information\n"));
               for (i = 0; i < addRequest->portListLen; i++) {
                  LOG(2, (KERN_INFO " -- srcLow %u srcHigh %u dstLow %u dstHigh %u\n",
                          portList[i].localPortLow,  portList[i].localPortHigh,
                          portList[i].remotePortLow, portList[i].remotePortHigh));
               }
               goto out_error;
            }
         }
         retval = AddIPv4Rule(addRequest->ruleSetId, addRequest,
                              addressList, portList);
         goto out_unlock;
out_error:
         kfree(addRequest);
         retval = error;
         goto out_unlock;
      }

      case VNET_FILTER_CMD_ADD_IPV6_RULE:
         LOG(2, (KERN_INFO "add filter rule IPv6 not supported\n"));
         retval = -EPROTONOSUPPORT;
         goto out_unlock;
         
      case VNET_FILTER_CMD_SET_LOG_LEVEL: {
         VNet_SetLogLevel setLogLevel;
         
         if (copy_from_user(&setLogLevel, (void *)ioarg, sizeof setLogLevel)) {
            retval = -EFAULT;
         } else if (setLogLevel.header.len != sizeof setLogLevel) {
            LOG(2, (KERN_INFO "set log level invalid header length %u\n",
                    setLogLevel.header.len));
            retval = -EINVAL;
         } else if (VNET_FILTER_LOGLEVEL_NONE > setLogLevel.logLevel ||
                    setLogLevel.logLevel > VNET_FILTER_LOGLEVEL_MAXIMUM) {
            LOG(2, (KERN_INFO "set log level invalid value %u\n",
                    setLogLevel.logLevel));
            retval = -EINVAL;
         } else {
            logLevel = setLogLevel.logLevel;
         }
         goto out_unlock;
      }

      default:
         LOG(2, (KERN_INFO "add filter rule invalid command %u\n",
                 ruleHeader->type));
         retval = -EINVAL;
         goto out_unlock;
   }
out_unlock:
   compat_mutex_unlock(&filterIoctlMutex);
   return retval;
}


/*
 *----------------------------------------------------------------------
 *
 * VNetFilter_Shutdown --
 *
 *      Function is called when the driver is being unloaded.
 *      This function is responsible for removing the callback
 *      function from the IP stack and deallocating any remaining
 *      state.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

void
VNetFilter_Shutdown(void)
{
   LOG(2, (KERN_INFO "shutting down vnet filter\n"));

   RemoveHostFilterCallback();

   if (activeRule != NULL) {
      LOG(2, (KERN_INFO "disabling the active rule %u\n", activeRule->id));
      ChangeRuleSet(activeRule->id, FALSE, TRUE, VNET_FILTER_RULE_NO_CHANGE);
      // ASSERT(activeRule == NULL);
   }
   while (ruleSetHead != NULL) {
      LOG(2, (KERN_INFO "Deleteing rule set %u\n", ruleSetHead->id));
      DeleteRuleSet(ruleSetHead->id);
   }
   // ASSERT(numRuleSets == 0);

   LOG(2, (KERN_INFO "shut down vnet filter\n"));
}

/*
 *----------------------------------------------------------------------
 *
 * LogPacket --
 *
 *      This function logs a dropped or forwarded packet.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

#define LOGPACKET_HEADER_LEN (20) /* presumed length of 'header': IP (20) */
#define LOGPACKET_DATA_LEN   (28) /* TCP/UDP header (20) + 8 payload = 28 */

static void
LogPacket(uint16 action,  // IN: reason for packet drop/forward
          void *header,   // IN: packet header
          void *data,     // IN: packet data
          uint32 length,  // IN: packet length (of 'data', not including 'header')
          Bool drop)      // IN: drop versus forward
{
   char packet[(LOGPACKET_HEADER_LEN + LOGPACKET_DATA_LEN) * 3 + 1];
   int i, n;
   
   /* something to do? */
   if (VNET_FILTER_LOGLEVEL_VERBOSE > logLevel) {
      return;
   }
   
   /* cap packet length */
   if (length > LOGPACKET_DATA_LEN) {
      length = LOGPACKET_DATA_LEN;
   }
   
   /* build packet string */
   n = 0;
   if (header) {
      for (i = 0; i < LOGPACKET_HEADER_LEN; i++) {
         sprintf(&packet[n], "%02x ", ((uint8 *)header)[i]);
         n += 3;
      }
   }
   for (i = 0; i < length; i++) {
      sprintf(&packet[n], "%02x ", ((uint8 *)data)[i]);
      n += 3;
   }
   
   /* log packet */
   printk(KERN_INFO "packet %s: %s\n", drop ? "dropped" : "forwarded", packet);
}

#endif // CONFIG_NETFILTER
