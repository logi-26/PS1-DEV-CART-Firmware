/*
* PS1 DEV CART disc reader: definitions shared by discread.c and ps1transfer (disclive.c, "-disc")
*
* ps1transfer starts the disc reader (or uses it if it already runs), then talks to it through a mailbox in RAM, with
* the memory access of the kernel hook ('D'/'U' while a program runs, PS1 DEV CART firmware), as the memory card tool
* does (ps1-tools/cardtool/cardserver.h):
* - the computer writes the command fields, then increments cmd_seq;
* - the disc reader runs the command, writes the sectors (or the table of contents) into the buffer, then sets result,
*   sector_size and done_seq;
* - the computer reads them from the buffer.
* The disc reader only reads sectors: the file system (ISO 9660 directories, the files' XA attributes) is read by the
* computer from them.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _DISCSERVER_H_
#define _DISCSERVER_H_

#include <stdint.h>

#define	DISCSRV_MAILBOX_ADRS	0x80100000
#define	DISCSRV_BUFFER_ADRS		0x80100100
#define	DISCSRV_BUFFER_SIZE		(0x801E0000 - DISCSRV_BUFFER_ADRS)	// (the RAM under the firmware: 896KB)
#define	DISCSRV_MAX_SECTORS		(DISCSRV_BUFFER_SIZE / 2048)		// A read at most (that many sectors of its size fit)
#define	DISCSRV_MAGIC			0x56525344		// "DSRV", set once the reader is ready
#define	DISCSRV_VERSION			4		// (4: a stream stopped by the computer; 3: STREAM; 2: reads up to the buffer size, progress; an older reader running is restarted)

// Commands
#define	DISCSRV_INIT			1		// Drive ready (unlocked if needed): id[] = GetID answer, result = its interrupt
#define	DISCSRV_READ			2		// count sectors from lba (the disc's sector numbers: 0 = 00:02:00)
#define	DISCSRV_TOC				3		// Buffer: DiscTrack[], result = tracks (the lead out after them)
#define	DISCSRV_STOP			4		// Motor off
#define	DISCSRV_CAPTURE			5		// CD audio played from lba and captured by the SPU: count frames (L, R 16-bit) in the
										// buffer (at most DISCSRV_CAPTURE_MAX). The drive's first report of its position
										// (absolute): error_lba = its sector, retries = the frame captured then (the frames
										// before it: what the drive gave before playing, not the disc's)
#define	DISCSRV_CAPTURE_MAX		((0x801E0000 - DISCSRV_BUFFER_ADRS) / 4)	// (the RAM under the firmware: 5.2 s)
#define	DISCSRV_STREAM			6		// CD audio played from lba for count sectors, captured by the SPU and sent at once
										// to the computer as it plays (raw USB, the computer only reads meanwhile):
										// blocks "SD", type, 0, frame index (32-bit), data, sum of the data bytes (8-bit).
										// Type 1: 256 frames (L, R 16-bit); type 2, the end: frames, the drive's first
										// position (sector, its frame), halves missed, the first one's frame, lost
#define	STREAM_AUDIO			1
#define	STREAM_END				2
#define	STREAM_FRAMES			256
#define	STREAM_STOP				'X'		// Sent by the computer during a stream: it ends (its end block still sent)

// Read modes (mode field), the sectors in the buffer one after the other:
#define	DISCSRV_DATA			0		// 2048 bytes: the user data of a Mode 2 Form 1 (or Mode 1) sector
#define	DISCSRV_XA				1		// 2336 bytes: subheader (twice) + user data + EDC/ECC (XA audio, STR movies,
										// what mkpsxiso takes for "xa"/"str" files)
#define	DISCSRV_CDDA			2		// 2352 bytes: CD audio (44100 Hz stereo, 16-bit little endian)
#define	DISCSRV_MODE_MASK		3
#define	DISCSRV_DOUBLE			0x100	// Double speed
// CD audio read variants (tests of the drive): no SeekP first, ReadS instead of ReadN, no whole-sector size bit
#define	DISCSRV_NO_SEEKP		0x200
#define	DISCSRV_READS			0x400
#define	DISCSRV_NO_SIZE			0x800
#define	DISCSRV_IGNORE			0x1000	// (Setmode bit 4)
// DATA and XA sectors are read whole (2340 bytes, the header too): a sector whose header isn't the one expected (a
// sector lost, the drive late) is read again, so a read at double speed with interrupts on stays exact.

// Results (negative)
#define	DISCSRV_NO_DISC			-1
#define	DISCSRV_READ_ERROR		-2		// error_lba: the sector
#define	DISCSRV_BAD_COMMAND		-3
#define	DISCSRV_NO_DRIVE		-4

typedef struct
{
	uint8_t		track;		// BCD as the drive gives it (lead out: 0xAA)
	uint8_t		mm, ss;		// Start (BCD, absolute time)
	uint8_t		pad;
} DiscTrack;

typedef struct
{
	uint32_t	magic;
	uint32_t	version;
	uint32_t	cmd_seq;		// Incremented by the computer once the fields below are written
	uint32_t	done_seq;		// = cmd_seq when the command has ended
	int32_t		result;			// Sectors read, tracks, or an error (DISCSRV_...)
	uint32_t	error_lba;		// READ_ERROR: the sector not read (the ones before it are in the buffer)
	uint32_t	sector_size;	// Bytes per sector in the buffer
	uint32_t	retries;		// Sectors read again (lost or with an error) in the last read
	uint32_t	cmd;
	uint32_t	lba;
	uint32_t	count;
	uint32_t	mode;
	uint8_t		id[8];			// GetID answer (INIT): stat, flags, type, atip, "SCEx"
	uint8_t		status;			// Drive status of the last answer
	uint8_t		pad[3];
	uint32_t	progress;		// READ: the sectors in the buffer so far (the computer shows it while it reads)
	int32_t		lost;			// CAPTURE: frames the capture missed (the drive further on than the frames say; a half
								// buffer not copied in time), about 0 when all is well
} DiscServerMailbox;

#endif
