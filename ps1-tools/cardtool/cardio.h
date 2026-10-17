/*
* PS1 DEV CART memory card tool: direct sector access (controller/memory card port registers)
*
* The BIOS card driver moves one byte per interrupt and starts each sector on a frame: about 40ms per 128 bytes
* sector, 43.6s for a whole card (12.5s with this). Here a sector is transferred at once with the interrupts off (a few ms), so the
* controllers and the display keep working between sectors (the BIOS reads the controllers on the VBlank interrupt).
* Commands as described in the nocash PSX specifications ("Memory Card Read/Write Commands"):
*	read:	81 52 00 00 MSB LSB 00 00 00 00 [128 x 00] 00 00	answers: FLAG 5A 5D - - 5C 5D MSB LSB [data] CHK 47
*	write:	81 57 00 00 MSB LSB [128 data] CHK 00 00 00			answers: FLAG 5A 5D - - - 5C 5D 47
* The card acknowledges each byte (/ACK), except the last one. CHK = MSB ^ LSB ^ each data byte, 47 ('G') = good.
* A sector that fails is done again by the caller with the BIOS (cardtool.c).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _CARDIO_H_
#define _CARDIO_H_

#include <stdint.h>

#define	JOY_DATA	(*((volatile uint8_t *)0x1F801040))
#define	JOY_STAT	(*((volatile uint32_t *)0x1F801044))
#define	JOY_MODE	(*((volatile uint16_t *)0x1F801048))
#define	JOY_CTRL	(*((volatile uint16_t *)0x1F80104A))
#define	JOY_BAUD	(*((volatile uint16_t *)0x1F80104E))
#define	CARDIO_IRQ_STAT	(*((volatile uint32_t *)0x1F801070))

#define	JOY_STAT_RX_READY	(1 << 1)
#define	JOY_STAT_ACK_LOW	(1 << 7)	// /ACK input low (the card acknowledges)
#define	JOY_STAT_IRQ		(1 << 9)	// /ACK received (with JOY_CTRL_ACK_IRQ)
#define	JOY_CTRL_TX_ENABLE	(1 << 0)
#define	JOY_CTRL_SELECT		(1 << 1)	// /JOYn output low
#define	JOY_CTRL_IRQ_RESET	(1 << 4)
#define	JOY_CTRL_ACK_IRQ	(1 << 12)
#define	JOY_CTRL_PORT2		(1 << 13)
#define	IRQ_CONTROLLER		(1 << 7)

// Loops for a byte or its /ACK (about 8ms; usually 5 to 30 loops, about 5100 for the /ACK after 5C when the card
// reads its flash, measured on an official card)
#define	CARDIO_WAIT			40000

// Sends a byte and returns the answer, -1 on timeout. ack: waits for the /ACK of the card
static int	CardIoByte(uint8_t out, int ack)
{
	int	i, in;

	JOY_DATA = out;
	for (i = 0; !(JOY_STAT & JOY_STAT_RX_READY); i++)
		if (i > CARDIO_WAIT)
			return (-1);
	in = JOY_DATA;
	if (ack)
	{
		for (i = 0; !(JOY_STAT & JOY_STAT_IRQ); i++)
			if (i > CARDIO_WAIT)
				return (-1);
		// The end of the /ACK pulse, then the flag is cleared (cleared during the pulse, it is set again at once and
		// the next byte wouldn't wait: the card answers 5C, then nothing while it reads its flash)
		for (i = 0; JOY_STAT & JOY_STAT_ACK_LOW; i++)
			if (i > CARDIO_WAIT)
				return (-1);
		JOY_CTRL |= JOY_CTRL_IRQ_RESET;
	}
	return (in);
}

// One command with the interrupts off: write (buf -> card) or read (card -> buf). 1: done, 0: failed
static int	CardIoSector(int slot, int sector, uint8_t *buf, int write)
{
	uint8_t	msb = sector >> 8, lsb = sector, chk = msb ^ lsb, data[128];
	int		i, ok = 0, c;

	EnterCriticalSection();
	JOY_MODE = 0x000D;		// 8 bits, baud rate x1
	JOY_BAUD = 0x0088;		// 250 kHz
	JOY_CTRL = JOY_CTRL_TX_ENABLE | JOY_CTRL_SELECT | JOY_CTRL_ACK_IRQ | JOY_CTRL_IRQ_RESET |
		((slot == 2) ? JOY_CTRL_PORT2 : 0);
	for (i = 0; i < 100; i++)	// (a short delay after the selection)
		__asm__ volatile ("nop");

	if ((CardIoByte(0x81, 1) < 0) || (CardIoByte(write ? 0x57 : 0x52, 1) < 0) || (CardIoByte(0, 1) != 0x5A) ||
		(CardIoByte(0, 1) != 0x5D) || (CardIoByte(msb, 1) < 0) || (CardIoByte(lsb, 1) < 0))
		goto out;
	if (write)
	{
		for (i = 0; i < 128; i++)
		{
			chk ^= buf[i];
			if (CardIoByte(buf[i], 1) < 0)
				goto out;
		}
		if ((CardIoByte(chk, 1) < 0) || (CardIoByte(0, 1) != 0x5C) || (CardIoByte(0, 1) != 0x5D) ||
			(CardIoByte(0, 0) != 'G'))
			goto out;
	}
	else
	{
		if ((CardIoByte(0, 1) != 0x5C) || (CardIoByte(0, 1) != 0x5D) || (CardIoByte(0, 1) != msb) ||
			(CardIoByte(0, 1) != lsb))
			goto out;
		for (i = 0; i < 128; i++)
		{
			if ((c = CardIoByte(0, 1)) < 0)
				goto out;
			data[i] = c;
			chk ^= c;
		}
		if ((CardIoByte(0, 1) != chk) || (CardIoByte(0, 0) != 'G'))
			goto out;
		memcpy(buf, data, 128);		// (the caller's buffer is only changed by a good sector)
	}
	ok = 1;

out:
	JOY_CTRL = 0;					// Deselected
	CARDIO_IRQ_STAT = ~IRQ_CONTROLLER;		// (the /ACK interrupts of this transfer aren't for the BIOS drivers)
	ExitCriticalSection();
	return (ok);
}

// With retries: after a write, the card doesn't answer while it programs its flash (the next command right after a
// write fails, measured on an official card). 1: done, 0: failed (the caller uses the BIOS driver)
#define	CARDIO_TRIES		4
#define	CARDIO_RETRY_DELAY	20000		// Loops (about 2ms) between tries, interrupts on

static int	CardIoSectorRetry(int slot, int sector, uint8_t *buf, int write)
{
	int	i, j;

	for (i = 0; i < CARDIO_TRIES; i++)
	{
		if (CardIoSector(slot, sector, buf, write))
			return (1);
		for (j = 0; j < CARDIO_RETRY_DELAY; j++)
			__asm__ volatile ("nop");
	}
	return (0);
}

#endif
