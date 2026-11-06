/*
* PS1 DEV CART disc reader
* Uploaded and executed by "ps1transfer -disc ..." (ps1transfer/disclive.c): reads the sectors of the disc in the drive
* that ps1transfer asks for, through a mailbox in RAM (discserver.h), and shows what it does. The file system is read by
* the computer: only the sectors needed are read (a directory, one file), not the whole disc.
*
* The drive is driven with polled commands (no BIOS or PSn00bSDK CD driver, as the firmware's disc boot, cdread.h):
* - INIT: the drive ready, the disc identified (GetID), the drive unlocked if it refuses the disc (the firmware's
*   unlock, for burned discs on a console without a mod chip);
* - READ: data sectors (2048 bytes), whole XA sectors (2336 bytes: XA audio and STR movies keep their subheaders) or
*   CD audio (2352 bytes). Each data or XA sector is read with its header and checked: a sector lost (the drive goes
*   on while the interrupt of the USB link is served) is read again, so double speed stays exact;
* - TOC: the tracks (CD audio after the data track).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <psxgpu.h>
#include <psxetc.h>
#include <psxapi.h>
#include <hwregs_c.h>
#include "psyq_compat.h"
#include "devcart_ui.h"
#include "discserver.h"

#define	SCREEN_W		320
#define	SCREEN_H		240

// The polled drive commands of the firmware (cdread.h): its sector read isn't used (2048-byte sectors, single speed)
#define	CD_DELAY_US(_us_)		DelayUs(_us_)
#define	CD_CRITICAL_ENTER()		EnterCriticalSection()
#define	CD_CRITICAL_EXIT()		ExitCriticalSection()
#include "cdread.h"

#define	RAW_SIZE		2340	// Sector with the size bit of Setmode: header (4) + subheader (8) + data, no sync
#define	CDDA_SIZE		2352
#define	MAX_RETRIES		8		// Reads started again at a sector at most (then DISCSRV_READ_ERROR): the computer is
								// told in seconds, never minutes, and reads the others (a bad sector: left out)
#define	SLOW_AFTER		3		// Started again more than this: at single speed
#define	SECTOR_WAIT		15000	// A sector awaited 1.5 s at most (100 us steps; spin-up after a seek: well within)
#define	MAX_BEHIND		150		// Sectors before the one wanted skipped at most in one go (the drive late)

static DISPENV		disp[2];
static DRAWENV		draw[2];
static int			fnt_info, fnt_last;
static uint8_t		raw[CDDA_SIZE + 16];

// What the screen shows
static char			drive_text[40] = "NOT CHECKED YET";
static char			last_text[48] = "WAITING FOR THE COMPUTER";
static uint32_t		total_sectors, total_retries, total_cmds;

// Drive regions (test command 19h,22h) and the strings of the unlock commands 50h-56h (as the firmware, ps1devcart.c)
static const char	*cd_region_id[] = {"for Europe", "for U/C", "for NETEU", "for NETNA"};
static const char	*cd_region_unlock[] = {"(Europe)", "of America", "World wide", "World wide"};
static const char	*cd_unlock_strings[] = {NULL, "Licensed by", "Sony", "Computer", "Entertainment"};

static void	MsfOf(int lba, uint8_t *loc)
{
	int	pos = lba + 150;	// (2 seconds of lead-in)

	loc[0] = CD_BCD(pos / 4500);
	loc[1] = CD_BCD((pos / 75) % 60);
	loc[2] = CD_BCD(pos % 75);
}

static void	Unlock(void)
{
	uint8_t	test = 0x22, resp[16];
	int		i, region;

	if (CD_RawCommand(0x19, &test, 1, resp, sizeof(resp)) != 3)
		return;
	for (region = 0; region < 4; region++)
		if (!strcmp((char *)resp, cd_region_id[region]))
			break;
	if (region == 4)	// Japanese and debug drives: nothing to unlock
		return;
	for (i = 0; i < 7; i++)
	{
		const char	*s = (i == 5) ? cd_region_unlock[region] : (i < 5) ? cd_unlock_strings[i] : NULL;

		CD_RawCommand(0x50 + i, s, s ? strlen(s) : 0, NULL, 0);
	}
}

// GetID: the interrupt of its second answer (2: a licensed disc, 5: unlicensed, an audio CD, or no disc), 0 if none
static int	GetId(uint8_t *id)
{
	uint8_t	resp[8];
	int		irq;

	memset(id, 0, 8);
	if (CD_RawCommand(0x1A, NULL, 0, resp, sizeof(resp)) != 3)
		return (0);
	irq = CD_RawResponse(id, 8, 50000);
	return (irq);
}

static int	CmdRead(DiscServerMailbox *mb);

static int	CmdInit(DiscServerMailbox *mb)
{
	uint8_t	resp[8];
	int		irq = 0, i;

	CD_CRITICAL_ENTER();
	// The drive spun up and the disc identified (a few seconds after a disc change)
	for (i = 0; (i < 20) && ((irq = GetId(mb->id)) != 2) && (irq != 5); i++)
	{
		if (i == 2)
			CD_RawCommand(0x07, NULL, 0, resp, sizeof(resp)), CD_RawResponse(resp, sizeof(resp), 50000);	// MotorOn
		CD_DELAY_US(500000);
	}
	// Refused (unlicensed flag, a disc there): unlocked, then identified again
	if ((irq == 5) && (mb->id[1] & 0x80) && !(mb->id[1] & 0x40))
	{
		Unlock();
		irq = GetId(mb->id);
	}
	CD_CRITICAL_EXIT();
	mb->status = mb->id[0];
	// A data disc: sector 16 read. Not read (after a game that read the disc and was reset, the drive kept seeking: no
	// sector came, see the firmware's CD_Unlock): the drive reset, unlocked and identified again
	if ((irq == 2) || ((irq == 5) && !(mb->id[1] & 0x50)))
	{
		DiscServerMailbox	test = *mb;

		test.lba = 16;
		test.count = 1;
		test.mode = DISCSRV_DATA;
		if (CmdRead(&test) != 1)
		{
			strcpy(drive_text, "RESETTING THE DRIVE...");
			CD_CRITICAL_ENTER();
			CD_RawCommand(0x1C, NULL, 0, resp, sizeof(resp));		// Reset
			CD_DELAY_US(500000);
			for (i = 0; (i < 40) && ((irq = GetId(mb->id)) != 2) && (irq != 5); i++)
				CD_DELAY_US(500000);
			if ((irq == 5) && (mb->id[1] & 0x80) && !(mb->id[1] & 0x40))
			{
				Unlock();
				irq = GetId(mb->id);
			}
			CD_CRITICAL_EXIT();
		}
	}
	if (!irq)
	{
		strcpy(drive_text, "THE DRIVE DOESN'T ANSWER");
		return (DISCSRV_NO_DRIVE);
	}
	if ((irq == 5) && (mb->id[1] & 0x40))
	{
		strcpy(drive_text, "NO DISC");
		return (DISCSRV_NO_DISC);
	}
	if (mb->id[1] & 0x10)
		strcpy(drive_text, "AUDIO CD");
	else if (irq == 2)
		sprintf(drive_text, "DISC %c%c%c%c", mb->id[4] ? mb->id[4] : ' ', mb->id[5] ? mb->id[5] : ' ',
				mb->id[6] ? mb->id[6] : ' ', mb->id[7] ? mb->id[7] : ' ');
	else
		strcpy(drive_text, "UNLICENSED DISC (READ ANYWAY)");
	return (irq);
}

static int	CmdToc(DiscServerMailbox *mb)
{
	DiscTrack	*t = (DiscTrack *)DISCSRV_BUFFER_ADRS;
	uint8_t		resp[8], n;
	int			first, last, i;

	CD_CRITICAL_ENTER();
	i = CD_RawCommand(0x13, NULL, 0, resp, sizeof(resp));	// GetTN
	CD_CRITICAL_EXIT();
	if (i != 3)
		return (DISCSRV_NO_DISC);
	mb->status = resp[0];
	first = (resp[1] >> 4) * 10 + (resp[1] & 15);
	last = (resp[2] >> 4) * 10 + (resp[2] & 15);
	if ((first < 1) || (last > 99) || (last < first))
		return (DISCSRV_NO_DISC);
	for (i = first; i <= last + 1; i++)	// (the lead out last: GetTD 0)
	{
		n = (i > last) ? 0 : CD_BCD(i);
		CD_CRITICAL_ENTER();
		if (CD_RawCommand(0x14, &n, 1, resp, sizeof(resp)) != 3)
			resp[1] = resp[2] = 0;
		CD_CRITICAL_EXIT();
		t->track = (i > last) ? 0xAA : n;
		t->mm = resp[1];
		t->ss = resp[2];
		t->pad = 0;
		t++;
	}
	sprintf(last_text, "TRACKS %d-%d", first, last);
	return (last - first + 1);
}

// Pause (a read going on is stopped), then the mode
static void	ReadSetup(uint8_t mode)
{
	uint8_t	resp[8];

	if (CD_RawCommand(0x09, NULL, 0, resp, sizeof(resp)) == 3)
		CD_RawResponse(resp, sizeof(resp), 50000);
	CD_RawCommand(0x0E, &mode, 1, resp, sizeof(resp));
}

// One sector of the read going on: its bytes in raw[] (count in *got), the interrupt (1: a sector, 0: timeout)
static int	NextSector(int *got, int timeout)
{
	int		irq, i;
	uint8_t	c;

	CD_REG(0) = 1;
	for (; !(irq = CD_REG(3) & 7) && timeout; timeout--)	// (in 100us steps)
		CD_DELAY_US(100);
	if (irq != 1)
	{
		while (CD_REG(0) & 0x20)
			c = CD_REG(1);
		CD_REG(0) = 1;
		CD_REG(3) = 0x1F;
		IRQ_STAT = ~(1 << 2);
		return (irq);
	}
	while (CD_REG(0) & 0x20)	// Response FIFO (status)
		c = CD_REG(1);
	CD_REG(0) = 0;
	CD_REG(3) = 0;
	CD_REG(3) = 0x80;	// Want data
	for (timeout = 1000; !(CD_REG(0) & 0x40) && timeout; timeout--)	// Data FIFO not empty
		CD_DELAY_US(10);
	for (i = 0; (i < CDDA_SIZE) && (CD_REG(0) & 0x40); i++)
		raw[i] = CD_REG(2);
	*got = i;
	CD_REG(3) = 0;
	CD_REG(0) = 1;
	CD_REG(3) = 0x1F;	// Acknowledge: the next sector
	IRQ_STAT = ~(1 << 2);
	(void)c;
	return (1);
}

static int	CmdRead(DiscServerMailbox *mb)
{
	uint8_t		*dst = (uint8_t *)DISCSRV_BUFFER_ADRS, loc[3], resp[8];
	int			kind = mb->mode & DISCSRV_MODE_MASK, lba = mb->lba, count = mb->count, done = 0, retries = 0;
	int			size = (kind == DISCSRV_DATA) ? CD_SECTOR : (kind == DISCSRV_XA) ? 2336 : CDDA_SIZE;
	int			irq, got, tries = 0, pos;
	bool		cdda = (kind == DISCSRV_CDDA), ok;

	if ((kind > DISCSRV_CDDA) || (count < 1) || ((uint32_t)count * size > DISCSRV_BUFFER_SIZE))
		return (DISCSRV_BAD_COMMAND);
	mb->progress = 0;
	sprintf(last_text, "%s %d +%d%s", cdda ? "AUDIO" : (kind == DISCSRV_XA) ? "XA" : "DATA", lba, count,
			(mb->mode & DISCSRV_DOUBLE) ? " 2X" : "");
	mb->sector_size = size;
	// Whole sectors (size bit), CD audio allowed for audio


	uint8_t	setmode = ((mb->mode & DISCSRV_DOUBLE) ? 0x80 : 0) | ((mb->mode & DISCSRV_NO_SIZE) ? 0 : 0x20) |
		((mb->mode & DISCSRV_IGNORE) ? 0x10 : 0) | (cdda ? 0x01 : 0);
	int		behind;

	ReadSetup(setmode);
	while (done < count)
	{
		if (tries++ > (cdda ? 4 : MAX_RETRIES))	// (CD audio: interrupts off, not for long)
		{
			mb->error_lba = lba + done;
			break;
		}
		if ((tries == SLOW_AFTER + 1) && (setmode & 0x80))	// (read again and again: at single speed)
			ReadSetup(setmode &= ~0x80);
		MsfOf(lba + done, loc);
		// Setloc, ReadN. CD audio: SeekP first (ReadN's own seek is a data seek: it looks for the sector headers, which
		// CD audio hasn't got), then ReadN with no Setloc pending reads from there
		ok = (CD_RawCommand(0x02, loc, 3, resp, sizeof(resp)) == 3);
		if (ok && cdda && !(mb->mode & DISCSRV_NO_SEEKP))
			ok = (CD_RawCommand(0x16, NULL, 0, resp, sizeof(resp)) == 3) &&
				((mb->pad[0] = CD_RawResponse(resp, sizeof(resp), 40000)) == 2);
		ok = ok && (CD_RawCommand((mb->mode & DISCSRV_READS) ? 0x1B : 0x06, NULL, 0, resp, sizeof(resp)) == 3);
		if (!ok)
		{
			mb->pad[1] = resp[1];
			retries++;
			continue;
		}
		for (behind = 0; done < count; )
		{
			if ((irq = NextSector(&got, SECTOR_WAIT)) != 1)
			{
				mb->pad[0] = 0x80 | irq;
				mb->pad[1] = raw[1];
				retries++;
				break;
			}
			if (cdda)
			{
				if (got > CDDA_SIZE - 12)
					size = mb->sector_size = got;
				memcpy(dst + done * size, raw, size);
				done++;
				tries = 0;
				continue;
			}
			// The sector expected? (an earlier one is skipped, a later one: lost sectors, read again)
			pos = lba + done + 150;
			if ((raw[0] == CD_BCD(pos / 4500)) && (raw[1] == CD_BCD((pos / 75) % 60)) && (raw[2] == CD_BCD(pos % 75)))
			{
				memcpy(dst + done * size, raw + ((kind == DISCSRV_DATA) ? 12 : 4), size);
				mb->progress = ++done;
				tries = 0;
				continue;
			}
			retries++;
			{
				int	m = ((raw[0] >> 4) * 10 + (raw[0] & 15)) * 4500 + ((raw[1] >> 4) * 10 + (raw[1] & 15)) * 75 +
						(raw[2] >> 4) * 10 + (raw[2] & 15);

				if ((m < pos) && (m > pos - 50) && (behind++ < MAX_BEHIND))	// (behind: the next ones are coming)
					continue;
			}
			break;
		}
	}
	ReadSetup(0x00);	// Paused, single speed


	mb->retries = retries;
	total_sectors += done;
	total_retries += retries;
	return ((done == count) ? done : DISCSRV_READ_ERROR);
}

// CD audio through the SPU (the drive gives no CD audio sectors to ReadN on the consoles tested): the track is played
// at single speed into the SPU (its CD input on, the output muted), which writes what it receives into its capture
// buffers: 0x000-0x3FF left, 0x400-0x7FF right, 512 samples each at 44100 Hz, the SPU status telling which half it
// writes. Each half is copied as soon as the SPU has moved to the other one (256 samples: 5.8 ms). The SPU RAM is
// read by DMA only, 16 words at a time, the first halfword of each one stale (as the debug monitor reads it, see
// monitor.c SpuRead): 56 bytes kept per DMA. The kernel hook's interrupts stay on (the computer doesn't ask anything
// during a capture).

#define	SPU_R16(_a_)		(*((volatile uint16_t *)(_a_)))
#define	SPU_MAIN_L			SPU_R16(0x1F801D80)
#define	SPU_MAIN_R			SPU_R16(0x1F801D82)
#define	SPU_CD_L			SPU_R16(0x1F801DB0)
#define	SPU_CD_R			SPU_R16(0x1F801DB2)
#define	CAP_XFER_ADDR		SPU_R16(0x1F801DA6)
#define	CAP_SPU_CTRL			SPU_R16(0x1F801DAA)
#define	CAP_XFER_CTRL		SPU_R16(0x1F801DAC)
#define	CAP_SPU_STATUS			SPU_R16(0x1F801DAE)
#define	DMA4_MADR			(*((volatile uint32_t *)0x1F8010C0))
#define	DMA4_BCR			(*((volatile uint32_t *)0x1F8010C4))
#define	DMA4_CHCR			(*((volatile uint32_t *)0x1F8010C8))
#define	DMA_CTRL			(*((volatile uint32_t *)0x1F8010F0))
#define	SPU_CHUNK			56
#define	CAPTURE_HALF		0x200	// Bytes of a half capture buffer (256 samples)

static uint16_t	spu_block[16 * 2 + 2];

static void	SpuXferMode(uint16_t mode)
{
	CAP_SPU_CTRL = (CAP_SPU_CTRL & ~0x30) | mode;
	while ((CAP_SPU_STATUS & 0x30) != mode)
		;
}

static uint16_t	spu_check[16 * 2 + 2];

static void	SpuBlock(uint32_t adrs, uint16_t *buf)
{
	SpuXferMode(0);
	CAP_XFER_ADDR = adrs >> 3;
	SpuXferMode(0x30);				// DMA read
	DMA4_MADR = (uint32_t)buf & 0xFFFFFF;
	DMA4_BCR = (1 << 16) | 16;
	DMA4_CHCR = 0x01000200;			// SPU to RAM, by blocks, start
	while (DMA4_CHCR & (1 << 24))
		;
}

// size bytes (multiple of 2) of SPU RAM from adrs (multiple of 8) to dst. Each block read until two reads agree: the
// SPU writing the capture buffers meanwhile spoils a halfword of a DMA now and then (always the same places of a half,
// the timing being the same)
static void	SpuCopy(uint32_t adrs, uint8_t *dst, int size)
{
	int	n, tries;

	while (size > 0)
	{
		n = (size < SPU_CHUNK) ? size : SPU_CHUNK;
		SpuBlock(adrs, spu_block);
		for (tries = 0; tries < 4; tries++)
		{
			SpuBlock(adrs, spu_check);
			if (!memcmp((uint8_t *)spu_block + 2, (uint8_t *)spu_check + 2, n))
				break;
			memcpy(spu_block, spu_check, sizeof(spu_check));
		}
		memcpy(dst, (uint8_t *)spu_block + 2, n);
		dst += n;
		size -= n;
		adrs += SPU_CHUNK;
	}
	SpuXferMode(0);
}

// The drive's position while it plays: GetlocP (11h) sent without waiting, its answer read later (track, index, the
// relative and the absolute time). The drive's own reports (Setmode bit 2) came too rarely: none in 5 seconds at times.
static bool	loc_pending;

static void	PositionAsk(void)
{
	if (CD_REG(0) & 0x80)	// (busy)
		return;
	CD_REG(0) = 1;
	CD_REG(3) = 0x1F;
	CD_REG(3) = 0x40;
	CD_REG(0) = 0;
	CD_REG(1) = 0x11;
	loc_pending = true;
}

// The absolute sector of the answer, -1 for an error, -2 if it hasn't come yet
static int	PositionAnswer(void)
{
	uint8_t	r[8];
	int		n, irq;

	CD_REG(0) = 1;
	if (!(irq = CD_REG(3) & 7))
		return (-2);
	for (n = 0; CD_REG(0) & 0x20; n++)
	{
		uint8_t	c = CD_REG(1);

		if (n < 8)
			r[n] = c;
	}
	CD_REG(0) = 1;
	CD_REG(3) = 0x1F;
	IRQ_STAT = ~(1 << 2);
	if (irq != 3)
		return (-2);	// (something else: still waiting)
	loc_pending = false;
	if (n < 8)
		return (-1);
#define	BCD2(_b_)	((((_b_) >> 4) * 10) + ((_b_) & 15))
	return (BCD2(r[5]) * 4500 + BCD2(r[6]) * 75 + BCD2(r[7]) - 150);
}

static uint8_t	cap_l[CAPTURE_HALF], cap_r[CAPTURE_HALF];

static int	CmdCapture(DiscServerMailbox *mb)
{
	uint32_t	*dst = (uint32_t *)DISCSRV_BUFFER_ADRS, frames = 0, first_frame = 0;
	uint16_t	ctrl = CAP_SPU_CTRL, main_l = SPU_MAIN_L, main_r = SPU_MAIN_R;
	uint8_t		loc[3], resp[8], mode = 0x00;	// (single speed)
	int			half, i, sector, first_sector = -1, last_sector = -1;
	uint32_t	last_frame = 0;

	if ((mb->count < 1) || (mb->count > DISCSRV_CAPTURE_MAX))
		return (DISCSRV_BAD_COMMAND);
	sprintf(last_text, "AUDIO CAPTURE %d", (int)mb->lba);
	mb->sector_size = 4;

	// The SPU: on, CD input on at full volume, nothing heard; DMA channel 4 on; the drive's audio to the SPU (left to
	// left, right to right at 100%), not muted
	SPU_MAIN_L = SPU_MAIN_R = 0;
	SPU_CD_L = SPU_CD_R = 0x7FFF;
	CAP_SPU_CTRL = (ctrl & ~0x30) | 0xC001;
	CAP_XFER_CTRL = 4;
	DMA_CTRL |= 8 << 16;
	CD_CRITICAL_ENTER();
	CD_REG(0) = 2;
	CD_REG(2) = 0x80;	// Left to left
	CD_REG(3) = 0x00;	// Left to right
	CD_REG(0) = 3;
	CD_REG(1) = 0x80;	// Right to right
	CD_REG(2) = 0x00;	// Right to left
	CD_REG(3) = 0x20;	// Applied
	ReadSetup(mode);	// (paused, single speed: Play at 2x would play twice as fast)
	CD_RawCommand(0x0C, NULL, 0, resp, sizeof(resp));	// Demute
	MsfOf(mb->lba, loc);
	if ((CD_RawCommand(0x02, loc, 3, resp, sizeof(resp)) != 3) || (CD_RawCommand(0x03, NULL, 0, resp, sizeof(resp)) != 3))
	{
		CD_CRITICAL_EXIT();
		SPU_MAIN_L = main_l;
		SPU_MAIN_R = main_r;
		return (DISCSRV_READ_ERROR);
	}
	CD_CRITICAL_EXIT();

	// Each half once the SPU writes the other one (bit 11 of the status: the half written)
	half = (CAP_SPU_STATUS >> 11) & 1;
	while (frames < mb->count)
	{
		int	now = (CAP_SPU_STATUS >> 11) & 1;

		// The position, asked again and again until the drive goes on to the next sector between two answers close in
		// time (the frame of that sector's start, to half a buffer: 256 frames): once it plays from where it was asked
		if (first_sector < 0)
		{
			if (!loc_pending)
				PositionAsk();
			else if ((sector = PositionAnswer()) != -2)
			{
				// (not further than if it had played at once: before its seek, the drive answers where it was)
				if ((sector >= (int)mb->lba) && (sector <= (int)(mb->lba + frames / 588) + 2) && (last_sector >= 0) &&
					(sector == last_sector + 1) && (frames - last_frame <= 256))
				{
					first_sector = sector;
					first_frame = frames;
				}
				last_sector = sector;
				last_frame = frames;
			}
		}
		if (now == half)
			continue;
		SpuCopy(half * CAPTURE_HALF, cap_l, CAPTURE_HALF);
		SpuCopy(0x400 + half * CAPTURE_HALF, cap_r, CAPTURE_HALF);
		half = now;
		for (i = 0; (i < CAPTURE_HALF / 2) && (frames < mb->count); i++)
		{
			dst[frames++] = cap_l[i * 2] | (cap_l[i * 2 + 1] << 8) | (cap_r[i * 2] << 16) | ((uint32_t)cap_r[i * 2 + 1] << 24);
		}
	}

	CD_CRITICAL_ENTER();
	if (loc_pending)	// (its answer)
		CD_RawResponse(resp, sizeof(resp), 1000);
	loc_pending = false;
	// Where the drive is now against the frames captured since its position was known: a half buffer missed (not
	// copied within its 5.8 ms) makes the frames fewer than the time played
	mb->lost = 0;
	if (first_sector >= 0)
	{
		uint8_t	r[8];

		if ((CD_RawCommand(0x11, NULL, 0, r, sizeof(r)) == 3))
		{
			int	now = BCD2(r[5]) * 4500 + BCD2(r[6]) * 75 + BCD2(r[7]) - 150;

			mb->lost = (now - first_sector) * 588 - (int)(frames - first_frame);
		}
	}
	CD_RawCommand(0x0B, NULL, 0, resp, sizeof(resp));	// Mute
	ReadSetup(0x00);	// Paused
	CD_CRITICAL_EXIT();
	SPU_MAIN_L = main_l;
	SPU_MAIN_R = main_r;
	SPU_CD_L = SPU_CD_R = 0;
	mb->retries = first_frame;
	mb->error_lba = (uint32_t)first_sector;
	total_sectors += frames / 588;
	return (frames);
}

// DISCSRV_STREAM: as CmdCapture, each half sent to the computer as soon as it is copied (1KB: about 2 ms at the USB's
// 600KB/s, the next half comes 5.8 ms later), so the whole track is taken as it plays. A half missed (the copy late:
// more than 1.5 halves of time since the previous one, horizontal lines counted by root counter 1) is counted.
#define	USB_STAT			(*((volatile uint8_t *)0x1F060000))	// (inverted: 0 = yes) bit 1 can send, bit 2 USB ready
#define	USB_DATA			(*((volatile uint8_t *)0x1F020000))
#define	RCNT1_COUNT			(*((volatile uint16_t *)0x1F801110))
#define	RCNT1_MODE			(*((volatile uint16_t *)0x1F801114))
#define	HALF_LINES_MAX		135		// Horizontal lines (about 91 a half at 15.7 kHz): more, a half was missed
#define	HOOK_ENABLED		(*((volatile uint8_t *)0x8000DF70))	// Kernel hook params: is_enabled (see kernel_hook.c)

static uint8_t	stream_block[8 + STREAM_FRAMES * 4 + 1];

static bool	StreamSend(const uint8_t *p, int n)
{
	int	timeout;

	while (n--)
	{
		for (timeout = 2000000; USB_STAT & 2; )		// (the computer reading: room in the FIFO)
			if (!timeout-- || (USB_STAT & 4))
				return (false);
		USB_DATA = *p++;
	}
	return (true);
}

static bool	StreamBlock(int type, uint32_t index, const uint8_t *data, int n)
{
	uint8_t	sum = 0;
	int		i;

	stream_block[0] = 'S';
	stream_block[1] = 'D';
	stream_block[2] = type;
	stream_block[3] = 0;
	memcpy(stream_block + 4, &index, 4);
	memcpy(stream_block + 8, data, n);
	for (i = 0; i < n; i++)
		sum += data[i];
	stream_block[8 + n] = sum;
	return (StreamSend(stream_block, 9 + n));
}

static int	CmdStream(DiscServerMailbox *mb)
{
	uint32_t	frames = 0, first_frame = 0, last_frame = 0, end[6], misses = 0, first_miss = 0, pcm[STREAM_FRAMES];
	uint16_t	ctrl = CAP_SPU_CTRL, main_l = SPU_MAIN_L, main_r = SPU_MAIN_R, line = 0;
	uint8_t		loc[3], resp[8], mode = 0x00, hook;
	int			half, i, sector, first_sector = -1, last_sector = -1, lost = 0;
	uint32_t	limit = (mb->count + 75 * 10) * 588;	// (at most: 10 seconds more than asked)
	bool		ok = true, started = false;

	sprintf(last_text, "AUDIO STREAM %d", (int)mb->lba);
	SPU_MAIN_L = SPU_MAIN_R = 0;
	SPU_CD_L = SPU_CD_R = 0x7FFF;
	CAP_SPU_CTRL = (ctrl & ~0x30) | 0xC001;
	CAP_XFER_CTRL = 4;
	DMA_CTRL |= 8 << 16;
	CD_CRITICAL_ENTER();
	CD_REG(0) = 2;
	CD_REG(2) = 0x80;
	CD_REG(3) = 0x00;
	CD_REG(0) = 3;
	CD_REG(1) = 0x80;
	CD_REG(2) = 0x00;
	CD_REG(3) = 0x20;
	ReadSetup(mode);
	CD_RawCommand(0x0C, NULL, 0, resp, sizeof(resp));	// Demute
	MsfOf(mb->lba, loc);
	if ((CD_RawCommand(0x02, loc, 3, resp, sizeof(resp)) != 3) || (CD_RawCommand(0x03, NULL, 0, resp, sizeof(resp)) != 3))
		ok = false;
	CD_CRITICAL_EXIT();
	RCNT1_MODE = 0x0100;	// Root counter 1: horizontal lines, free running
	// The kernel hook doesn't serve the USB meanwhile: a byte from the computer (STREAM_STOP) is read here, not taken
	// for a command (the computer asks nothing else during a stream)
	hook = HOOK_ENABLED;
	HOOK_ENABLED = 0;

	half = (CAP_SPU_STATUS >> 11) & 1;
	while (ok && (frames < limit))
	{
		int	now = (CAP_SPU_STATUS >> 11) & 1;

		if (first_sector < 0)
		{
			if (!loc_pending)
				PositionAsk();
			else if ((sector = PositionAnswer()) != -2)
			{
				if ((sector >= (int)mb->lba) && (sector <= (int)(mb->lba + frames / 588) + 2) && (last_sector >= 0) &&
					(sector == last_sector + 1) && (frames - last_frame <= 256))
				{
					first_sector = sector;
					first_frame = frames;
				}
				last_sector = sector;
				last_frame = frames;
			}
			if ((first_sector < 0) && (frames > 44100 * 8))	// (the drive doesn't play)
				break;
		}
		else if ((frames - first_frame) >= (mb->lba + mb->count - first_sector) * 588 + 588 * 10)
			break;		// (all asked for, and a bit more)
		if (!(USB_STAT & 1) && (USB_DATA == STREAM_STOP))	// (the computer: stop)
			break;
		if (now == half)
			continue;
		{
			uint16_t	t = RCNT1_COUNT;

			if (started && ((uint16_t)(t - line) > HALF_LINES_MAX) && (first_sector >= 0))
			{
				if (!misses++)
					first_miss = frames;
			}
			line = t;
			started = true;
		}
		SpuCopy(half * CAPTURE_HALF, cap_l, CAPTURE_HALF);
		SpuCopy(0x400 + half * CAPTURE_HALF, cap_r, CAPTURE_HALF);
		half = now;
		for (i = 0; i < STREAM_FRAMES; i++)
			pcm[i] = cap_l[i * 2] | (cap_l[i * 2 + 1] << 8) | (cap_r[i * 2] << 16) | ((uint32_t)cap_r[i * 2 + 1] << 24);
		ok = StreamBlock(STREAM_AUDIO, frames, (uint8_t *)pcm, sizeof(pcm));
		frames += STREAM_FRAMES;
	}

	CD_CRITICAL_ENTER();
	if (loc_pending)
		CD_RawResponse(resp, sizeof(resp), 1000);
	loc_pending = false;
	if (first_sector >= 0)
	{
		uint8_t	r[8];

		if (CD_RawCommand(0x11, NULL, 0, r, sizeof(r)) == 3)
			lost = (BCD2(r[5]) * 4500 + BCD2(r[6]) * 75 + BCD2(r[7]) - 150 - first_sector) * 588 - (int)(frames - first_frame);
	}
	CD_RawCommand(0x0B, NULL, 0, resp, sizeof(resp));	// Mute
	ReadSetup(0x00);
	CD_CRITICAL_EXIT();
	SPU_MAIN_L = main_l;
	SPU_MAIN_R = main_r;
	SPU_CD_L = SPU_CD_R = 0;
	end[0] = frames;
	end[1] = (uint32_t)first_sector;
	end[2] = first_frame;
	end[3] = misses;
	end[4] = first_miss;
	end[5] = (uint32_t)lost;
	if (ok)
		StreamBlock(STREAM_END, frames, (uint8_t *)end, sizeof(end));
	HOOK_ENABLED = hook;
	mb->retries = misses;
	total_sectors += frames / 588;
	return (ok ? (int)frames : DISCSRV_READ_ERROR);
}

static int	CmdStop(void)
{
	uint8_t	resp[8];

	CD_CRITICAL_ENTER();
	if (CD_RawCommand(0x08, NULL, 0, resp, sizeof(resp)) == 3)	// Stop (motor off)
		CD_RawResponse(resp, sizeof(resp), 50000);
	CD_CRITICAL_EXIT();
	strcpy(last_text, "MOTOR STOPPED");
	return (0);
}

int	main(void)
{
	volatile DiscServerMailbox	*mb = (volatile DiscServerMailbox *)DISCSRV_MAILBOX_ADRS;
	int		i, side = 0, is_pal, result;

	is_pal = (*((char *)0xBFC7FF52) == 'E');
	ResetGraph(0);
	SetVideoMode(is_pal ? MODE_PAL : MODE_NTSC);
	for (i = 0; i < 2; i++)
	{
		SetDefDispEnv(&disp[i], 0, i ? 0 : SCREEN_H, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&draw[i], 0, i ? SCREEN_H : 0, SCREEN_W, SCREEN_H);
	}
	FntLoad(960, 256);
	UiInit(draw);
	fnt_info = UiOpenText(8, 28, 304, 4);
	fnt_last = UiOpenText(8, 84, 304, 4);

	// The CD interrupt stays masked: the drive is polled (the kernel hook's USB link keeps its interrupts)
	EnterCriticalSection();
	IRQ_MASK &= ~(1 << 2);
	CD_REG(0) = 1;
	CD_REG(2) = 0x1F;	// (the drive's interrupts on, as the BIOS leaves them: only the CPU side is masked)
	CD_REG(3) = 0x1F;
	IRQ_STAT = ~(1 << 2);
	ExitCriticalSection();

	memset((void *)mb, 0, sizeof(*mb));
	mb->version = DISCSRV_VERSION;
	mb->magic = DISCSRV_MAGIC;
	SetDispMask(1);

	while (1)
	{
		if (mb->cmd_seq != mb->done_seq)
		{
			DiscServerMailbox	*m = (DiscServerMailbox *)mb;

			m->retries = 0;
			m->error_lba = 0;
			switch (m->cmd)
			{
				case DISCSRV_INIT:	result = CmdInit(m); break;
				case DISCSRV_READ:	result = CmdRead(m); break;
				case DISCSRV_TOC:	result = CmdToc(m); break;
				case DISCSRV_STOP:	result = CmdStop(); break;
				case DISCSRV_CAPTURE:	result = CmdCapture(m); break;
				case DISCSRV_STREAM:	result = CmdStream(m); break;
				default:			result = DISCSRV_BAD_COMMAND;
			}
			if (result == DISCSRV_READ_ERROR)
				sprintf(last_text, "READ ERROR AT SECTOR %u", (unsigned)m->error_lba);
			total_cmds++;
			m->result = result;
			m->done_seq = m->cmd_seq;
		}

		UiTitle("PS1 DEV CART DISC READER", NULL);
		UiTextPanel(8, 28, 304, 4);
		UiTextPanel(8, 84, 304, 4);
		FntPrint(fnt_info, "DRIVE    %s\n\nTHE COMPUTER READS THE FILES IT\nNEEDS (PS1 DEV CART MANAGER)", drive_text);
		FntPrint(fnt_last, "LAST     %s\nSECTORS  %u\nRETRIES  %u\nCOMMANDS %u", last_text, (unsigned)total_sectors,
				 (unsigned)total_retries, (unsigned)total_cmds);
		UiHelp("RESET FROM THE COMPUTER TO LEAVE");

		DrawSync(0);
		VSync(0);
		side ^= 1;
		PutDispEnv(&disp[side]);
		PutDrawEnv(&draw[side]);
		UiFlush();
		FntFlush(fnt_info);
		FntFlush(fnt_last);
	}
	return (0);
}
