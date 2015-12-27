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
 * speaker_reg.h --
 *
 *      PC speaker register
 */

#ifndef _SPEAKER_REG_H_
#define _SPEAKER_REG_H_

#define INCLUDE_ALLOW_USERLEVEL
#define INCLUDE_ALLOW_VMMEXT
#define INCLUDE_ALLOW_VMMON
#define INCLUDE_ALLOW_VMCORE
#include "includeCheck.h"

#define SPEAKER_PORT			0x61

#define SPEAKER_RAM_PARITY_ERROR	0x80	// read/write
#define SPEAKER_IO_PARITY_ERROR		0x40	// read
#define SPEAKER_CLEAR_IRQ0_TIMER	0x40	// write
#define SPEAKER_TIMER2_OUT		0x20	// read
#define SPEAKER_REFRESH_CLOCK_DIV2	0x10	// read
#define SPEAKER_ENABLE_IO_PARITY	0x08	// read/write
#define SPEAKER_ENABLE_RAM_PARITY	0x04	// read/write
#define SPEAKER_ENABLE_SPEAKER		0x02	// read/write
#define SPEAKER_TIMER2_GATE		0x01	// read/write

#define SPEAKER_READ_ONLY (SPEAKER_RAM_PARITY_ERROR | \
			   SPEAKER_IO_PARITY_ERROR | \
			   SPEAKER_TIMER2_OUT | \
			   SPEAKER_REFRESH_CLOCK_DIV2)

#endif	// _SPEAKER_REG_H_
