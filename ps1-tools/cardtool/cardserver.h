/*
* PS1 DEV CART memory card tool, server mode: definitions shared by cardtool.c and ps1transfer (save commands on the
* card itself: "-mclist slot1", "-mcexport slot1 SAVE out.mcs", "-mcimport slot1 in.mcs", "-mcdelete slot1 SAVE")
*
* ps1transfer starts the card tool in server mode (or uses it if it already runs), then talks to it through a
* mailbox in RAM, with the memory access of the kernel hook ('D'/'U' while a program runs, PS1 DEV CART firmware):
* - Read: the computer writes the sector list and the command, increments cmd_seq; the card tool reads the sectors
*   into the image buffer (sector n at n * 128), sets done_seq and result; the computer reads them from the buffer.
* - Write: the computer writes the sectors to the image buffer, the list, a text for the confirmation and the
*   command; the card tool asks for a confirmation with the controller, then writes them.
* Only the sectors needed are read or written (e.g. the directory, the blocks of one save).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _CARDSERVER_H_
#define _CARDSERVER_H_

#include <stdint.h>

#define	CARDSRV_MAILBOX_ADRS	0x80101000
#define	CARDSRV_IMAGE_ADRS		0x80110000		// 128KB, card image layout
#define	CARDSRV_MAGIC			0x56525343		// "CSRV", set once the server is ready
#define	CARDSRV_VERSION			1
#define	CARDSRV_SECTORS			1024
#define	CARDSRV_TEXT			64

// Commands
#define	CARDSRV_READ			1
#define	CARDSRV_WRITE			2		// After a confirmation with the controller

// Results
#define	CARDSRV_OK				0
#define	CARDSRV_NO_CARD			-1
#define	CARDSRV_IO_ERROR		-2		// error_sector: the sector
#define	CARDSRV_CANCELLED		-3		// Write refused with the controller
#define	CARDSRV_BAD_COMMAND		-4

typedef struct
{
	uint32_t	magic;
	uint32_t	version;
	uint32_t	cmd_seq;					// Incremented by the computer once the fields below are written
	uint32_t	done_seq;					// = cmd_seq when the command has ended
	int32_t		result;
	uint32_t	error_sector;
	uint32_t	cmd;
	uint32_t	slot;						// 1 or 2
	uint32_t	count;						// Sectors in the list
	char		text[CARDSRV_TEXT];			// Write: what is written, shown for the confirmation
	uint16_t	sectors[CARDSRV_SECTORS];
} CardServerMailbox;

#endif
