/*
* PS1 DEV CART memory card backup/restore
* Uploaded and executed by "ps1transfer -mcbackup card.mcd" or "ps1transfer -mcrestore card.mcd" [-slot 2]
* (a multitap's slots too: -slot 1b, 2d...: cardserver.h CARD_SLOT)
*
* Backup:	the 1024 sectors (128KB) of the card are read and written to the computer file with the PCDRV
*			file server of ps1transfer (psxpcdrv).
* Restore:	ps1transfer uploads the card image with this program, it is written to the card after a
*			confirmation with the controller (Cross: write, Circle: cancel).
* At the end, an EOT character (4) is printed so ps1transfer stops its console/file server mode.
* Server:	stays running and reads or writes the sectors asked by ps1transfer (save commands on the card itself,
*			see cardserver.h). Writes also need a confirmation with the controller.
* Card images are raw 128KB files (.mcd / .mcr, as used by emulators).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
// Built with PSn00bSDK (open source) instead of PsyQ
#include <psxgpu.h>
#include <psxetc.h>
#include <psxapi.h>
#include "psyq_compat.h"
#include "hook_usb.h"
#include "psxpcdrv.h"
#include "cardserver.h"
#include "cardio.h"
#include "devcart_ui.h"

// Data block written by ps1transfer, see "-mcbackup"/"-mcrestore" in ps1transfer/main.c
#define	CARD_DATA_ADRS		0x80100000
#define	CARD_MAGIC			0x44524143	// "CARD"
#define	CARD_IMAGE_OFFSET	0x100

enum	{MODE_BACKUP = 1, MODE_RESTORE, MODE_SERVER};

typedef struct
{
	uint32_t	magic;
	uint32_t	mode;
	uint32_t	slot;		// CARD_SLOT(port 1 or 2, multitap slot 0-3: A-D), see cardserver.h
	char	name[64];	// Backup file name (in the ps1transfer -p directory)
} CardData;

#define	SCREEN_W		320
#define	SCREEN_H		240
#define	CARD_SECTORS	1024
#define	SECTOR_SIZE		128
#define	CARD_SIZE		(CARD_SECTORS * SECTOR_SIZE)
#define	RETRY			3
#define	PC_CHUNK		8192

static DISPENV		disp[2];
static DRAWENV		draw[2];
static int			side;
static uint8_t		card[CARD_SIZE];	// Backup buffer
static long			sw_ev[4], hw_ev[4];	// End of I/O, error, timeout, new card
static char			status[128] = "", result[128] = "";
static int			progress = -1;
static int			bios_sectors;		// Sectors done with the BIOS driver (direct access failed)

// Screen (devcart_ui.h): title bar with the mode, status panel, progress bar, controller help
static int			fnt_text, fnt_progress;
static const char	*help;		// Help bar text (confirmation), NULL: none

static void	Display(char *title)
{
	UiTitle("PS1 DEV CART MEMORY CARD TOOL", title);
	UiPanel(8, 22, 304, 98);
	FntPrint(fnt_text, "%s\n\n%s\n", status, result);
	if (progress >= 0)
	{
		UiPanel(8, 126, 304, 34);
		FntPrint(fnt_progress, "SECTOR %d/%d   %d%%", progress, CARD_SECTORS, progress * 100 / CARD_SECTORS);
		UiProgress(16, 146, 288, 6, progress, CARD_SECTORS);
	}
	UiHelp(help);
	DrawSync(0);
	HookUsbWaitFrame();	// VSync(0), the kernel hook answers the computer meanwhile (see hook_usb.h)
	side ^= 1;
	PutDispEnv(&disp[side]);
	PutDrawEnv(&draw[side]);
	UiFlush();
	FntFlush(fnt_text);
	FntFlush(fnt_progress);
}

// 0: done, 1: error, 2: timeout, 3: new card (inserted since the last access)
static int	WaitEvents(long *ev)
{
	int	i;

	while (1)
		for (i = 0; i < 4; i++)
			if (TestEvent(ev[i]) == 1)
				return (i);
}

// Sector read/write completion: software or hardware card events, whichever is signaled
static int	WaitAnyEvents(void)
{
	int	i;

	while (1)
		for (i = 0; i < 4; i++)
			if ((TestEvent(hw_ev[i]) == 1) || (TestEvent(sw_ev[i]) == 1))
				return (i);
}

static void	ClearEvents(void)
{
	int	i;

	for (i = 0; i < 4; i++)
	{
		TestEvent(sw_ev[i]);
		TestEvent(hw_ev[i]);
	}
}

// "1", "2", "1B"...: the slot shown
static const char	*SlotName(int slot)
{
	static char	name[4];

	name[0] = '0' + CARD_SLOT_PORT(slot);
	name[1] = CARD_SLOT_TAP(slot) ? 'A' + CARD_SLOT_TAP(slot) : 0;
	name[2] = 0;
	return (name);
}

// The BIOS channel of a slot (00h port 1, 10h port 2, + the multitap slot)
static long	SlotChan(int slot)
{
	return (((CARD_SLOT_PORT(slot) == 2) ? 0x10 : 0x00) | CARD_SLOT_TAP(slot));
}

// Card present and ready (a new card must be acknowledged with _card_clear)
// (3 tries: once, the first check after a reset said "no card" with a card inserted). A multitap's slot B-D: its first
// sector read directly (the BIOS driver only knows slot A)
static int	CardReady(int slot)
{
	long	chan = SlotChan(slot);
	int		ret = 1, i;

	if (CARD_SLOT_TAP(slot))
	{
		static uint8_t	sector[128];

		return (CardIoSectorRetry(slot, 0, sector, 0));
	}

	for (i = 0; (i < 3) && ret; i++)
	{
		ClearEvents();
		_card_info(chan);
		ret = WaitEvents(sw_ev);
		if (ret == 3)
		{
			ClearEvents();
			_card_clear(chan);
			ret = WaitEvents(hw_ev);
		}
	}
	return (ret == 0);
}

// Direct access first (cardio.h), the BIOS driver if it fails (not for a multitap's slot B-D)
static int	SectorAccess(int slot, int sector, uint8_t *buf, int write)
{
	long	chan = SlotChan(slot);
	int		i;

	if (CardIoSectorRetry(slot, sector, buf, write))
		return (1);
	if (CARD_SLOT_TAP(slot))
		return (0);
	bios_sectors++;
	for (i = 0; i < RETRY; i++)
	{
		ClearEvents();
		if (write ? _card_write(chan, sector, buf) : _card_read(chan, sector, buf))
			if (WaitAnyEvents() == 0)
				return (1);
	}
	return (0);
}

static int	CardAccess(int slot, uint8_t *data, int write)
{
	for (progress = 0; progress < CARD_SECTORS; progress++)
	{
		if (!SectorAccess(slot, progress, data + progress * SECTOR_SIZE, write))
		{
			sprintf(result, "ERROR %s SECTOR %d", write ? "WRITING" : "READING", progress);
			printf("Card: error %s sector %d\n", write ? "writing" : "reading", progress);
			return (0);
		}
		if ((progress & 15) == 15)
			Display(write ? "RESTORE" : "BACKUP");
		if ((progress & 255) == 255)
			printf("Card: %d/%d sectors\n", progress + 1, CARD_SECTORS);
	}
	return (1);
}

static int	SendBackup(char *name)
{
	int	fd, i, n;

	if (PCinit() != 0)
	{
		sprintf(result, "NO FILE SERVER (PS1TRANSFER -C)");
		return (0);
	}
	if ((fd = PCcreat(name, 0)) < 0)
	{
		sprintf(result, "CAN'T CREATE THE FILE");
		return (0);
	}
	for (i = 0; i < CARD_SIZE; i += n)
	{
		n = (CARD_SIZE - i > PC_CHUNK) ? PC_CHUNK : CARD_SIZE - i;
		if (PCwrite(fd, (char *)card + i, n) != n)
		{
			PCclose(fd);
			sprintf(result, "FILE WRITE ERROR");
			return (0);
		}
	}
	PCclose(fd);
	return (1);
}

/****************************************/
// Server mode

// Asks for a confirmation with the controller: 1 = Cross (write), 0 = Circle (cancel)
static int	Confirm(char *title)
{
	uint32_t	pad = PadRead(1), opad;

	help = "CROSS: WRITE        CIRCLE: CANCEL";
	do
	{
		Display(title);
		opad = pad;
		pad = PadRead(1);
	} while (!((pad & ~opad) & (PADRdown | PADRright)));
	help = NULL;
	return (!(pad & PADRright));
}

static int	ServerCommand(CardServerMailbox *mb)
{
	uint8_t	*image = (uint8_t *)CARDSRV_IMAGE_ADRS;
	int		i, write = (mb->cmd == CARDSRV_WRITE), sector;

	if ((mb->count > CARDSRV_SECTORS) || ((mb->cmd != CARDSRV_READ) && !write) || !CARD_SLOT_VALID(mb->slot))
		return (CARDSRV_BAD_COMMAND);
	mb->text[CARDSRV_TEXT - 1] = 0;
	sprintf(status, "SLOT %s: %s", SlotName(mb->slot), write ? "WRITE" : "READ");
	if (!CardReady(mb->slot))
	{
		sprintf(result, "NO MEMORY CARD IN SLOT %s", SlotName(mb->slot));
		return (CARDSRV_NO_CARD);
	}
	if (write)
	{
		sprintf(status, "SLOT %s: %s\n%d SECTOR(S) TO WRITE", SlotName(mb->slot), mb->text, (int)mb->count);
		result[0] = 0;
		if (!Confirm("SERVER"))
		{
			sprintf(result, "CANCELLED, CARD NOT MODIFIED");
			return (CARDSRV_CANCELLED);
		}
	}
	for (i = 0; i < mb->count; i++)
	{
		sector = mb->sectors[i] & (CARDSRV_SECTORS - 1);
		if (!SectorAccess(mb->slot, sector, image + sector * SECTOR_SIZE, write))
		{
			mb->error_sector = sector;
			sprintf(result, "ERROR %s SECTOR %d", write ? "WRITING" : "READING", sector);
			return (CARDSRV_IO_ERROR);
		}
		if ((i & 15) == 15)
		{
			sprintf(status, "SLOT %s: %s %d/%d", SlotName(mb->slot), write ? "WRITING" : "READING", i + 1, (int)mb->count);
			Display("SERVER");
		}
	}
	sprintf(status, "SLOT %s", SlotName(mb->slot));
	sprintf(result, "%s %d SECTOR(S): OK", write ? "WROTE" : "READ", (int)mb->count);
	return (CARDSRV_OK);
}

// Runs the commands of ps1transfer, never returns
static void	Server(void)
{
	CardServerMailbox	*mb = (CardServerMailbox *)CARDSRV_MAILBOX_ADRS;
	uint32_t			seq;

	memset(mb, 0, sizeof(CardServerMailbox));
	mb->version = CARDSRV_VERSION;

	// (the kernel hook answers the computer while Display() waits for the next frame, see hook_usb.h; the 1kHz timer
	// interrupt used before made the BIOS memory card driver time out with PSn00bSDK)
	mb->magic = CARDSRV_MAGIC;
	sprintf(status, "WAITING FOR PS1TRANSFER");
	while (1)
	{
		Display("SERVER");
		seq = *((volatile uint32_t *)&mb->cmd_seq);
		if (seq == mb->done_seq)
			continue;
		mb->result = ServerCommand(mb);
		mb->done_seq = seq;
	}
}

int	main(void)
{
	CardData	*cd = (CardData *)CARD_DATA_ADRS;
	int			i, ok = 0, is_pal;
	char		*title;

	// Same video mode as the console region, like the PS1 DEV CART menu (BIOS version string: 'E' = Europe = PAL)
	is_pal = (*((char *)0xBFC7FF52) == 'E');
	ResetGraph(0);	// (first: PSn00bSDK's ResetGraph sets up the callbacks, see psyq_compat.h)
	SetVideoMode(is_pal ? MODE_PAL : MODE_NTSC);
	for (i = 0; i < 2; i++)
	{
		SetDefDispEnv(&disp[i], 0, i ? 0 : SCREEN_H, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&draw[i], 0, i ? SCREEN_H : 0, SCREEN_W, SCREEN_H);
	}
	FntLoad(960, 256);
	UiInit(draw);
	fnt_text = FntOpen(16, 30, 288, 86, 0, 384);
	fnt_progress = FntOpen(16, 134, 288, 8, 0, 48);
	SetDispMask(1);

	PadInit(0);
	EnterCriticalSection();
	sw_ev[0] = OpenEvent(SwCARD, EvSpIOE, EvMdNOINTR, NULL);
	sw_ev[1] = OpenEvent(SwCARD, EvSpERROR, EvMdNOINTR, NULL);
	sw_ev[2] = OpenEvent(SwCARD, EvSpTIMOUT, EvMdNOINTR, NULL);
	sw_ev[3] = OpenEvent(SwCARD, EvSpNEW, EvMdNOINTR, NULL);
	hw_ev[0] = OpenEvent(HwCARD, EvSpIOE, EvMdNOINTR, NULL);
	hw_ev[1] = OpenEvent(HwCARD, EvSpERROR, EvMdNOINTR, NULL);
	hw_ev[2] = OpenEvent(HwCARD, EvSpTIMOUT, EvMdNOINTR, NULL);
	hw_ev[3] = OpenEvent(HwCARD, EvSpNEW, EvMdNOINTR, NULL);
	for (i = 0; i < 4; i++)
	{
		EnableEvent(sw_ev[i]);
		EnableEvent(hw_ev[i]);
	}
	ExitCriticalSection();
	InitCARD(1);	// 1: controllers are also used
	StartCARD();
	_bu_init();
	ChangeClearPAD(0);	// (PSn00bSDK: StartCARD lets the BIOS acknowledge VBlank again, then VSync() never returns)

	if ((cd->magic == CARD_MAGIC) && (cd->mode == MODE_SERVER))
		Server();
	if ((cd->magic != CARD_MAGIC) || ((cd->mode != MODE_BACKUP) && (cd->mode != MODE_RESTORE)))
	{
		title = "NO COMMAND";
		sprintf(result, "USE: PS1TRANSFER -MCBACKUP FILE\n  OR PS1TRANSFER -MCRESTORE FILE");
		goto end;
	}
	if (!CARD_SLOT_VALID(cd->slot))
		cd->slot = 1;
	title = (cd->mode == MODE_BACKUP) ? "BACKUP" : "RESTORE";
	sprintf(status, "SLOT %s", SlotName(cd->slot));
	printf("Card: %s, slot %s\n", (cd->mode == MODE_BACKUP) ? "backup" : "restore", SlotName(cd->slot));

	if (!CardReady(cd->slot))
	{
		sprintf(result, "NO MEMORY CARD IN SLOT %s", SlotName(cd->slot));
		goto end;
	}

	if (cd->mode == MODE_BACKUP)
	{
		if (CardAccess(cd->slot, card, 0))
		{
			sprintf(status, "SLOT %s READ, SENDING TO PC...", SlotName(cd->slot));
			Display(title);
			if ((ok = SendBackup(cd->name)))
				sprintf(result, "BACKUP SAVED: %s", cd->name);
		}
	}
	else
	{
		// The card is overwritten: confirmation with the controller
		sprintf(status, "SLOT %s: OVERWRITE THE CARD ?", SlotName(cd->slot));
		printf("Card: waiting for confirmation on the PS1 (Cross: write, Circle: cancel)\n");
		if (!Confirm(title))
			sprintf(result, "CANCELLED, CARD NOT MODIFIED");
		else
		{
			sprintf(status, "SLOT %s: WRITING...", SlotName(cd->slot));
			if ((ok = CardAccess(cd->slot, (uint8_t *)cd + CARD_IMAGE_OFFSET, 1)))
				sprintf(result, "CARD RESTORED");
		}
	}

end:
	if (bios_sectors)
		printf("Card: %d sector(s) done with the BIOS driver\n", bios_sectors);
	printf("Card: %s\n", result);
	printf("%c", 4);	// EOT: end of ps1transfer console mode
	if (ok)
		sprintf(status, "SLOT %s: DONE", SlotName(cd->slot));
	while (1)
		Display(title);

	return (0);
}
