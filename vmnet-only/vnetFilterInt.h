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

/*
 * vnetFilterInt.h
 *      This file defines platform-independent structures and limits that are
 *      internally used in the vmnet driver for packet filtering.
 */

#ifndef _VNETFILTERINT_H_
#define _VNETFILTERINT_H_

#define MAX_RULE_SETS      32    /* maximum rule sets to allow */
#define MAX_RULES_PER_SET  64    /* maximum rules for each rule set */
#define MAX_ADDR_PER_RULE  64    /* maximum IP addresses for one rule */
#define MAX_PORT_PER_RULE  64    /* maximum ports for one rule */

typedef struct RuleAddr {
   uint32 ipv4Addr; /* remote entity's address (dst on outbound, src on inbound) */
   uint32 ipv4Mask; /* remote entity's mask    (dst on outbound, src on inbound) */
} RuleAddr;

typedef struct RulePort {
   uint32 localPortLow;    /* ~0 is don't care, otherwise low local range (inclusive)  */
   uint32 localPortHigh;   /* ~0 is don't care, otherwise high local range (inclusive) */
   uint32 remotePortLow;   /* ~0 is don't care, otherwise low remote range (inclusive) */
   uint32 remotePortHigh;  /* ~0 is don't care, otherwise low remote range (inclusive) */
} RulePort;

typedef struct Rule {
   struct Rule *next;	/* used for linked list */

   uint16 action;	/* VNET_FILTER_RULE_BLOCK, or VNET_FILTER_RULE_ALLOW */
   uint16 direction;	/* VNET_FILTER_DIRECTION_IN,
			   VNET_FILTER_DIRECTION_OUT,
			   VNET_FILTER_DIRECTION_BOTH */

   uint8 addressListLen;   /* items in addressList (0 means don't care about address) */
   uint8 portListLen;	   /* items in portList (0 means don't care about port) */

   uint16 proto;	   /* IP protocol that rule applies to (e.g., TCP or UDP) */
                           /* ~0 mean don't care, in which case "portList" is ignored */

   RuleAddr *addressList;  /* list of IP addresses for rule */

   RulePort *portList;	   /* list of port ranges for rule (if proto is TCP or UDP) */
} Rule;

typedef struct RuleSet {
   struct RuleSet *next; /* next item in linked list */
   uint32 id;		 /* id provided to user-mode application */
   uint16 enabled;	 /* bool: tracks if used enabled to take effect for filtering */
   uint16 action;	 /* default action to use for rule */
			 /* VNET_FILTER_RULE_BLOCK, or VNET_FILTER_RULE_ALLOW */
   struct Rule *list;	 /* first rule in rule set */
   struct Rule **tail;	 /* used to quickly add element to end of list */
   uint32 numRules;	 /* number of rules in 'list' */
} RuleSet;

#endif // _VNETFILTERINT_H_
