/*
* PS1 DEV CART bus timing test
* Uploaded and executed by "ps1transfer -timingtest" (ps1transfer/timingtest.c): sets the cartridge port timings the
* computer asks for, fills and checks the test data the computer reads and writes through the kernel hook with them,
* and reports what the console is (BIOS, CPU, GPU, the bus timings the BIOS set). See timingtest.h.
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
#include <psxspu.h>
#include <hwregs_c.h>
#include "psyq_compat.h"
#include "devcart_ui.h"
#include "timingtest.h"

#define	SCREEN_W		320
#define	SCREEN_H		240

#define	REG32(_a_)		(*(volatile uint32_t *)(_a_))
#define	EXP1_DELAY		REG32(0x1F801008)
#define	COM_DELAY		REG32(0x1F801020)

static DISPENV		disp[2];
static DRAWENV		draw[2];
static int			fnt_info, fnt_state;
static uint32_t		boot_exp1, boot_com;

static uint32_t	Prid(void)
{
	uint32_t	v;

	__asm__ volatile ("mfc0 %0, $15" : "=r" (v));
	return (v);
}

static void	SetTiming(volatile TtMailbox *mb, uint32_t exp1, uint32_t com)
{
	if (!exp1)
	{
		exp1 = boot_exp1;
		com = boot_com;
	}
	EnterCriticalSection();
	COM_DELAY = com;
	EXP1_DELAY = exp1;
	ExitCriticalSection();
	mb->exp1_now = EXP1_DELAY;
	mb->com_now = COM_DELAY;
}

// The buffer compared with the pattern (made again after the buffer): the first bad bytes, and whether a byte is
// missing or added at the first one
static int32_t	Verify(volatile TtMailbox *mb, uint32_t seed)
{
	const uint8_t	*buf = (const uint8_t *)TT_BUFFER_ADRS;
	uint8_t			*pat = (uint8_t *)(TT_BUFFER_ADRS + TT_BUFFER_SIZE);
	uint32_t		i, n = 0;
	int32_t			bad = 0;

	memset((void *)mb->bad, 0, sizeof(mb->bad));
	mb->verify_shift = 0;
	TtPattern(pat, TT_BUFFER_SIZE, seed);
	for (i = 0; i < TT_BUFFER_SIZE; i++)
	{
		if (buf[i] == pat[i])
			continue;
		if (!bad && (i + 17 < TT_BUFFER_SIZE))
		{
			if (!memcmp(buf + i, pat + i + 1, 16))
				mb->verify_shift = 1;
			else if (!memcmp(buf + i + 1, pat + i, 16))
				mb->verify_shift = 2;
		}
		if (n < TT_MAX_BAD)
		{
			volatile TtBadByte	*b = &mb->bad[n++];

			b->offset = i;
			b->expected = pat[i];
			b->got = buf[i];
			b->next_expected = (i + 1 < TT_BUFFER_SIZE) ? pat[i + 1] : 0;
			b->next_got = (i + 1 < TT_BUFFER_SIZE) ? buf[i + 1] : 0;
		}
		bad++;
	}
	return (bad);
}

int	main(void)
{
	volatile TtMailbox	*mb = (volatile TtMailbox *)TT_MAILBOX_ADRS;
	int			i, side = 0, is_pal, result;
	uint32_t	last_beat = 0, beat_frame = 0, cmds = 0;
	RECT		load_rect = {320, 0, 384, 512};	// (384KB: beside the screens, not over the font)

	// The console as the BIOS (and the firmware) left it, before anything changes
	boot_exp1 = EXP1_DELAY;
	boot_com = COM_DELAY;
	memset((void *)mb, 0, sizeof(*mb));
	for (i = 0; i < 9; i++)
		mb->memctrl[i] = REG32(0x1F801000 + i * 4);
	mb->ram_size = REG32(0x1F801060);
	mb->cop0_prid = Prid();
	mb->bios_date = REG32(0xBFC00100);
	for (i = 0; i < 47; i++)
	{
		char	c = *(volatile char *)(0xBFC7FF32 + i);

		mb->bios_version[i] = ((c >= 32) && (c < 127)) ? c : (c ? '.' : 0);
		if (!c)
			break;
	}

	is_pal = (*((char *)0xBFC7FF52) == 'E');
	ResetGraph(0);
	GPU_GP1 = 0x10000007;
	mb->gpu_version = GPU_GP0;
	SetVideoMode(is_pal ? MODE_PAL : MODE_NTSC);
	for (i = 0; i < 2; i++)
	{
		SetDefDispEnv(&disp[i], 0, i ? 0 : SCREEN_H, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&draw[i], 0, i ? SCREEN_H : 0, SCREEN_W, SCREEN_H);
	}
	SpuInit();
	FntLoad(960, 256);
	UiInit(draw);
	fnt_info = UiOpenText(8, 28, 304, 5);
	fnt_state = UiOpenText(8, 92, 304, 6);

	mb->exp1_now = boot_exp1;
	mb->com_now = boot_com;
	mb->version = TT_VERSION;
	mb->magic = TT_MAGIC;
	SetDispMask(1);

	while (1)
	{
		uint32_t	frame = VSync(-1);

		mb->frames = frame;
		if (mb->cmd_seq != mb->done_seq)
		{
			switch (mb->cmd)
			{
				case TT_SET_TIMING:
					SetTiming(mb, mb->arg0, mb->arg1);
					result = 0;
					break;
				case TT_FILL:
					TtPattern((uint8_t *)TT_BUFFER_ADRS, TT_BUFFER_SIZE, mb->arg0);
					result = 0;
					break;
				case TT_VERIFY:
					result = Verify(mb, mb->arg0);
					break;
				case TT_LOAD:
					mb->load = mb->arg0;
					mb->load_loops = 0;
					result = 0;
					break;
				default:
					result = -1;
			}
			cmds++;
			mb->result = result;
			mb->done_seq = mb->cmd_seq;
			last_beat = mb->heartbeat;
			beat_frame = frame;
		}

		// Watchdog: no heartbeat from the computer for a while with another timing set: the console's own again
		if (mb->heartbeat != last_beat)
		{
			last_beat = mb->heartbeat;
			beat_frame = frame;
		}
		else if (((mb->exp1_now != boot_exp1) || (mb->com_now != boot_com)) &&
				 (frame - beat_frame > TT_WATCHDOG_FRAMES))
		{
			SetTiming(mb, 0, 0);
			mb->reverted++;
			beat_frame = frame;
		}

		// DMA traffic as a game makes: long GPU and SPU transfers started again as soon as they end, so they go on
		// while the kernel hook serves the computer (this loop doesn't run then)
		if (mb->load)
		{
			if (SpuIsTransferCompleted(0))
			{
				SpuSetTransferStartAddr(0x1010);
				SpuWrite((const uint32_t *)0x80010000, 0x7E000);
				mb->load_loops++;
			}
			if (!DrawSync(1))
			{
				LoadImage(&load_rect, (const uint32_t *)0x80010000);
				mb->load_loops++;
			}
		}

		UiTitle("PS1 DEV CART BUS TIMING TEST", NULL);
		UiTextPanel(8, 28, 304, 5);
		UiTextPanel(8, 92, 304, 6);
		FntPrint(fnt_info, "THE COMPUTER TESTS THE USB LINK WITH\nEACH TIMING OF THE CARTRIDGE PORT.\n\n"
				 "BIOS TIMING  %08X %08X\n%s", (unsigned)boot_exp1, (unsigned)boot_com, (const char *)mb->bios_version);
		FntPrint(fnt_state, "TIMING NOW   %08X %08X\nDMA LOAD     %s\nCOMMANDS     %u\nSET BACK     %u\n\n%s",
				 (unsigned)mb->exp1_now, (unsigned)mb->com_now, mb->load ? "ON" : "OFF", (unsigned)cmds,
				 (unsigned)mb->reverted);
		UiHelp("RESET FROM THE COMPUTER TO LEAVE");

		DrawSync(0);
		VSync(0);
		side ^= 1;
		PutDispEnv(&disp[side]);
		PutDrawEnv(&draw[side]);
		UiFlush();
		FntFlush(fnt_info);
		FntFlush(fnt_state);
	}
	return (0);
}
