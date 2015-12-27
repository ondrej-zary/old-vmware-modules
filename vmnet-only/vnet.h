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

#ifndef _VNET_H
#define _VNET_H

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_MODULE
#define INCLUDE_ALLOW_VMK_MODULE
#define INCLUDE_ALLOW_VMMEXT
#include "includeCheck.h"
#include "vm_basic_types.h"
#include "vm_atomic.h"

#define VNET_PVN_ABI_ID_LEN    (256 / 8)  // bytes used on ioctl()
#define VNET_PVN_ID_LEN        (160 / 8)  // actual length used

#define VNET_BIND_VERSION  0x1
#define VNET_BIND_TO_VNET  0x1
#define VNET_BIND_TO_PVN   0x2

typedef struct VNet_Bind {
   uint32 version;                 // VNET_BIND_VERSION
   uint32 bindType;                // VNET_BIND_TO_xxx
   int32 number;                   // used for VNET_BIND_TO_VNET
   uint8  id[VNET_PVN_ABI_ID_LEN]; // used for VNET_BIND_TO_PVN
} VNet_Bind;

/*
 * We define customized ioctl commands by adding 0x1000
 * to the standard Linux definitions.
 *
 * See comments in iocontrols.h
 */

#define VNET_FIRST_CMD     0x99F2

// #define SIOCSKEEP          0x99F0  // not used
// #define SIOCGKEEP          0x99F1  // not used
#define SIOCSLADRF         0x99F2
#define SIOCPORT           0x99F3
#define SIOCBRIDGE         0x99F4
#define SIOCNETIF          0x99F5

#define SIOCSETMACADDR     0x99F6
#define SIOCSSWITCHMAP     0x99F7
#define SIOCSETNOTIFY      0x99F8
#define SIOCUNSETNOTIFY    0x99F9
// #define SIOCSETCLUSTERSIZE 0x99FA  // obsolete
#define SIOCSETNOTIFY2     0x99FB
#define SIOCGETAPIVERSION  0x99FC

#define VNET_LAST_CMD      0x99FC

#if defined __linux__ || defined __APPLE__
#define SIOCGETAPIVERSION2 _IOWR(0x99, 0xE0, uint32)
#define SIOCGBRSTATUS	    _IOR(0x99, 0xFD, uint32)
#define SIOCSPEER	    _IOW(0x99, 0xFE, char[8])
#define SIOCSPEER2          _IOW(0x99, 0xFE, char[32])
#define SIOCSBIND           _IOW(0x99, 0xFF, VNet_Bind)
#define SIOCSFILTERRULES    _IOW(0x99, 0xE1, VNet_RuleHeader)
#define SIOCSUSERLISTENER   _IOW(0x99, 0xE2, VNet_SetUserListener)
#endif

#if defined __linux__
#define VNET_BRFLAG_FORCE_SMAC    0x00000001

typedef
#include "vmware_pack_begin.h"
struct VNet_BridgeParams {
   char   name[32];
   uint32 flags;
}
#include "vmware_pack_end.h"
VNet_BridgeParams;

#define SIOCSPEER3         _IOW(0x99, 0xE4, VNet_BridgeParams)
#endif

#ifdef __APPLE__

#define VMNET_KEXT_NAME_BASE "com.vmware.kext.vmnet"

#ifdef VMX86_DEVEL
#define VMNET_KEXT_NAME VMNET_KEXT_NAME_BASE ".devel"
#else
#define VMNET_KEXT_NAME VMNET_KEXT_NAME_BASE
#endif

/*
 * We use [gs]etsockopt on Mac OS instead of ioctls for operations on vmnet
 */
enum VMNetSockOpt {
   VMNET_SO_APIVERSION = 0,     // Must come first, should never change
   VMNET_SO_BRSTATUS,
   VMNET_SO_PEER,
   VMNET_SO_BINDTOHUB,
   VMNET_SO_IFADDR,
   VMNET_SO_NETIFCREATE,
   VMNET_SO_IFFLAGS,
   VMNET_SO_LADRF,
   VMNET_SO_BRCREATE,
   VMNET_SO_SETNOTIFY,
   VMNET_SO_READDATA,
   VMNET_SO_UNSETNOTIFY,
   VMNET_SO_SETUSERLISTENER
};

/*
 * This magic value is populated in VNet_Notify.actMask and VNet_Notify.pollMask
 * to request the driver to clear the Notify pollPtr if the receive queue is empty.
 */
#define VNET_NOTIFY_CLR_MAGIC   0xDECAFBAD

typedef struct VNet_NetIf {
   char name[16];               // The BSD name of the interface
   uint8 instance;              // The "unit number" of the interface
} VNet_NetIf;

typedef
#include "vmware_pack_begin.h"
struct {
   char name[16]; // IN: BSD name of the interface to bridge.
   int media;     // IN: Media of the interface to bridge.
}
#include "vmware_pack_end.h"
VNet_Bridge;

#ifdef LATER
typedef struct VNet_Read {
   VA uAddr;            // Buffer to read into
   size_t len;          // Max number of bytes to read
} VNet_Read;
#endif

#endif

/*
 * VMnet driver version.
 *
 * Increment major version when you make an incompatible change.
 * Compatibility goes both ways (old driver with new executable
 * as well as new driver with old executable).
 */

#ifdef linux
#define VNET_API_VERSION		(3 << 16 | 0)
#elif defined __APPLE__
#define VNET_API_VERSION                (6 << 16 | 0)
#else
#define VNET_API_VERSION                (5 << 16 | 0)
#endif
#define VNET_API_VERSION_MAJOR(v)	((uint32) (v) >> 16)
#define VNET_API_VERSION_MINOR(v)	((uint16) (v))

/* version 1 structure */

typedef struct VNet_SetMacAddrIOCTL {
   int             version;
   unsigned char   addr[6];
   unsigned        flags;
} VNet_SetMacAddrIOCTL;

typedef struct VNet_Notify {
   uint32           version;
   uint32           padding;        /* Make gcc 64bit abi padding explicit */
   VA64             actPtr;         /* User VA of an Atomic_uint32 */
   VA64             pollPtr;        /* User VA of a volatile uint32 */
   VA64             recvClusterPtr; /* User VA of a uint32 */
   uint32           actMask;
   uint32           pollMask;
} VNet_Notify;

#define VNET_SETMACADDRF_UNIQUE      0x01
/*
 * The latest 802.3 standard sort of says that the length field ought to
 * be less than 1536 (for VLAN tagging support). I am choosing 1532
 * as our max VNET_MTU size, as I'd rather keep it a multiple of 4 and
 * VLAN tagging uses only upto 1518 bytes.
 */
#define VNET_MTU                     1532


#define VNET_BUF_TOO_SMALL           (-1)

/*
 *  vlan switch stuff
 */


#define VNET_MAX_VLANS     255

struct VNetSwitchMap {
   int  trunk;
   int  vlan;
   int  connect;
   int  vnet;
};

/*
 *----------------------------------------------------------------------------
 * VNetEvent
 *----------------------------------------------------------------------------
 */

/* the current version */
#define VNET_EVENT_VERSION         1

/* event classes */
#define VNET_EVENT_CLASS_BRIDGE    1

/* event types */
#define VNET_EVENT_TYPE_LINK_STATE 0

/* parameter for SIOCSUSERLISTENER */
typedef
#include "vmware_pack_begin.h"
struct VNet_SetUserListener {
   uint32 version;
   uint32 classMask;
}
#include "vmware_pack_end.h"
VNet_SetUserListener;

/* the event header */
typedef
#include "vmware_pack_begin.h"
struct VNet_EventHeader {
   uint32 size;
   uint32 senderId;
   uint32 eventId;
   uint32 classSet;
   uint32 type;
}
#include "vmware_pack_end.h"
VNet_EventHeader;

/*
 * the link state event
 * header = { sizeof(VNet_LinkStateEvent), ?, ?, VNET_EVENT_CLASS_BRIDGE,
 *            VNET_EVENT_TYPE_LINK_STATE }
 */
typedef
#include "vmware_pack_begin.h"
struct VNet_LinkStateEvent {
   VNet_EventHeader header;
   uint32 adapter;
   Bool up;
   char _pad[3];
}
#include "vmware_pack_end.h"
VNet_LinkStateEvent;

/*
 *----------------------------------------------------------------------------
 */

#if defined __APPLE__ && !defined KERNEL

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/socket.h>
#include <sys/sys_domain.h>
#include "str.h"

/*
 *----------------------------------------------------------------------------
 *
 * VMNetOpen --
 *
 *      Create a socket connected to the vmnet kernel control extension, bind
 *      it to a vmnet hub. Optionally make the socket non-blocking. Optionally
 *      set the interface MAC address. Optionally set interface flags.
 *
 * Results:
 *      Connected and bound socket on success.
 *      -1 on failure, returns error message in the "error" parameter.
 *
 * Side Effects:
 *      Allocates memory for returning "error" message to caller. Caller should
 *      remember to free(error).
 *
 *----------------------------------------------------------------------------
 */

static INLINE int
VMNetOpen(int hubNum,                   // IN: hub number to bind to
          Bool nonBlocking,             // IN: make socket non-blocking
          VNet_SetMacAddrIOCTL *ifAddr, // IN: optional MAC address
          uint32 flags,                 // IN: optional interface flags
          char **error)                 // OUT: error message on failures
{
   int fd;
   struct sockaddr_ctl addr;
   struct ctl_info info;
   socklen_t optlen;
   uint32 apiVersion;

   fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
   if (fd == -1) {
      if (error) {
         *error = Str_Asprintf(NULL, "Failed to create control socket: "
                               "errno %d\n", errno);
      }
      return -1;
   }

   bzero(&addr, sizeof addr);
   addr.sc_len = sizeof addr;
   addr.sc_family = AF_SYSTEM;
   addr.ss_sysaddr = AF_SYS_CONTROL;

   memset(&info, 0, sizeof info);
   strncpy(info.ctl_name, VMNET_KEXT_NAME, sizeof info.ctl_name);
   if (ioctl(fd, CTLIOCGINFO, &info)) {
      if (error) {
         *error = Str_Asprintf(NULL, "ioctl(CTLIOCGINFO) failed: errno %d\n",
                               errno);
      }
      goto exit_failure;
   }

   addr.sc_id = info.ctl_id;

   if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
      if (error) {
         *error = Str_Asprintf(NULL, "Connect to vmnet kext failed: errno %d\n",
                               errno);
      }
      goto exit_failure;
   }

   /* Optionally make socket non-blocking */
   if (nonBlocking) {
      int fFlags;
      fFlags = fcntl(fd, F_GETFL);
      if (fFlags == -1 || fcntl(fd, F_SETFL, fFlags | O_NONBLOCK) < 0) {
         if (error) {
            *error = Str_Asprintf(NULL, "Couldn't make socket non-blocking: "
                                  "errno %d\n", errno);
         }
         goto exit_failure;
      }
   }

   optlen = sizeof apiVersion;
   if (getsockopt(fd, SYSPROTO_CONTROL, VMNET_SO_APIVERSION, &apiVersion,
                  &optlen) < 0) {
      if (error) {
         *error = Str_Asprintf(NULL, "getsockopt(VMNET_SO_APIVERSION) failed: "
                               "errno %d\n", errno);
      }
      goto exit_failure;
   }

   if (VNET_API_VERSION_MAJOR(apiVersion) !=
       VNET_API_VERSION_MAJOR(VNET_API_VERSION)) {
      if (error) {
         *error = Str_Asprintf(NULL, "Module version mismatch. Please update "
                               "host.\n");
      }
      goto exit_failure;
   }

   if (setsockopt(fd, SYSPROTO_CONTROL, VMNET_SO_BINDTOHUB, &hubNum,
                  sizeof hubNum) < 0) {
      if (error) {
         *error = Str_Asprintf(NULL, "Could not bind to hub %d: errno %d\n",
                               hubNum, errno);
      }
      goto exit_failure;
   }

   /* Optionally set MAC address */
   if (ifAddr) {
      if (setsockopt(fd, SYSPROTO_CONTROL, VMNET_SO_IFADDR, ifAddr,
                     sizeof (*ifAddr)) < 0) {
         if (error) {
            *error = Str_Asprintf(NULL, "Could not set MAC address: errno %d\n",
                                  errno);
         }
         goto exit_failure;
      }
   }

   /* Optionally set interface flags */
   if (flags) {
      if (setsockopt(fd, SYSPROTO_CONTROL, VMNET_SO_IFFLAGS, &flags,
                     sizeof flags) < 0) {
         if (error) {
            *error = Str_Asprintf(NULL, "Could not set interface flags to 0x%x: "
                                  "errno %d\n", flags, errno);
         }
         goto exit_failure;
      }
   }

   /* Return success */
   return fd;

exit_failure:
   /* Return failure */
   close(fd);
   return -1;
}

#endif // __APPLE__ && ! KERNEL

#endif
