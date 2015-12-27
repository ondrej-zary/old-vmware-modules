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
 *  Versioned atomic synchronization:
 *
 *    These synchronization macros allow single-writer/many-reader
 *    access to data, based on Leslie Lamport's paper "Concurrent
 *    Reading and Writing", Communications of the ACM, November 1977.
 *
 *    many-writer/many-reader can be implemented on top of versioned
 *    atomics by using an additional spin lock to synchronize
 *    writers. This is preferable for cases where readers are expected to
 *    greatly outnumber writers.
 */

#ifndef _VERSIONED_ATOMIC_H
#define _VERSIONED_ATOMIC_H
 
#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMKERNEL
#define INCLUDE_ALLOW_VMCORE
#define INCLUDE_ALLOW_VMMON
#include "includeCheck.h"

typedef struct {
   volatile uint32 v0;
   volatile uint32 v1;
} VersionedAtomic;

/*
 *-----------------------------------------------------------------------------
 *
 * VersionedAtomic_BeginWrite --
 *    Called by a writer to indicate that the data protected by
 *    a given atomic version is about to change. Effectively locks out
 *    all readers until EndWrite is called.
 * 
 * Results:
 *      .
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
VersionedAtomic_BeginWrite(VersionedAtomic *versions)
{
   versions->v0++;
   COMPILER_MEM_BARRIER();
}

/*
 *-----------------------------------------------------------------------------
 *
 * VersionedAtomic_EndWrite --
 *    Called by a writer after it is done updating shared data. Lets
 *    pending and new readers proceed on shared data.
 * 
 * Results:
 *      .
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
static INLINE void
VersionedAtomic_EndWrite(VersionedAtomic *versions)
{
   COMPILER_MEM_BARRIER();
   versions->v1 = versions->v0;
}

/*
 *-----------------------------------------------------------------------------
 *
 * VersionedAtomic_BeginTryRead --
 *    Called by a reader before it tried to read shared data.
 * 
 * Results:
 *    Returns a version number to the reader. This version number
 *    is required to confirm validity of the read operation when reader
 *    calls EndTryRead.
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
static INLINE uint32
VersionedAtomic_BeginTryRead(const VersionedAtomic *versions)
{
   uint32 readVersion;

   readVersion = versions->v1;
   COMPILER_MEM_BARRIER();

   return readVersion;
}

/*
 *-----------------------------------------------------------------------------
 *
 * VersionedAtomic_EndTryRead --
 *    Called by a reader after it finishes reading shared data, to confirm
 *    validity of the data that was just read (IOW, to make sure that a
 *    writer did not intervene while the read was in progress).
 * 
 * Results:
 *    TRUE if the data read between BeginTryRead() and this call is
 *    valid. FALSE otherwise.
 *
 * Side effects:
 *
 *
 *-----------------------------------------------------------------------------
 */
static INLINE Bool
VersionedAtomic_EndTryRead(const VersionedAtomic *versions,
                           uint32 readVersion)
{
   COMPILER_MEM_BARRIER();
   return (versions->v0 == readVersion);
}

#endif //_VERSIONED_ATOMIC_H
