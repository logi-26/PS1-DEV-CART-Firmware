/*
* PS1 DEV CART firmware and tools: the few PsyQ functions they used that PSn00bSDK doesn't have
* (built with PSn00bSDK, https://github.com/Lameguy64/PSn00bSDK)
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _PSYQ_COMPAT_H_
#define _PSYQ_COMPAT_H_

// Fills a VRAM rectangle with a colour (GPU fill command: x rounded down and w up to multiples of 16)
static void	ClearImage(RECT *rect, int r, int g, int b)
{
	FILL	fill;

	setFill(&fill);
	setRGB0(&fill, r, g, b);
	setXY0(&fill, rect->x, rect->y);
	setWH(&fill, rect->w, rect->h);
	DrawPrim((const uint32_t *)&fill);
	DrawSync(0);
}

// Controllers through the BIOS pad driver, read like PsyQ's PadRead(): 1 = pressed, pad 1 in the low 16 bits and pad 2
// in the high 16 bits, same bits as PsyQ's libetc.h (second byte of the pad data in the high byte, third in the low byte)
#define	PADLup		(1 << 12)
#define	PADLdown	(1 << 14)
#define	PADLleft	(1 << 15)
#define	PADLright	(1 << 13)
#define	PADRup		(1 << 4)	// Triangle
#define	PADRdown	(1 << 6)	// Cross
#define	PADRleft	(1 << 7)	// Square
#define	PADRright	(1 << 5)	// Circle
#define	PADi		(1 << 9)
#define	PADj		(1 << 10)
#define	PADk		(1 << 8)
#define	PADl		(1 << 3)
#define	PADm		(1 << 1)
#define	PADn		(1 << 2)
#define	PADo		(1 << 0)
#define	PADh		(1 << 11)
#define	PADL1		PADn
#define	PADL2		PADo
#define	PADR1		PADl
#define	PADR2		PADm
#define	PADstart	PADh
#define	PADselect	PADk

static uint8_t	pad_buffer[2][34];

static void	PadInit(int mode)
{
	InitPAD(pad_buffer[0], sizeof(pad_buffer[0]), pad_buffer[1], sizeof(pad_buffer[1]));
	// "No controller" until the BIOS has read the pads: InitPAD clears the buffers, and zeros would read as a
	// controller with all buttons pressed for the first frame
	memset(pad_buffer, 0xFF, sizeof(pad_buffer));
	StartPAD();
	ChangeClearPAD(0);	// The BIOS pad driver must not acknowledge VBlank, PSn00bSDK's VSync() needs it
}

// (volatile: the BIOS writes the buffers at VBlank, a loop calling only PadRead must read them again each time)
static uint32_t	PadReadOne(const volatile uint8_t *buf)
{
	if (buf[0])	// No controller (or not read yet)
		return (0);
	return (~((buf[2] << 8) | buf[3]) & 0xFFFF);
}

static uint32_t	PadRead(int id)
{
	return (PadReadOne(pad_buffer[0]) | (PadReadOne(pad_buffer[1]) << 16));
}

static void	PadStop(void)
{
	StopPAD();
}

// Display set-up shared by the firmware and the tools: PSn00bSDK's ResetGraph() calls ResetCallback() and installs its
// VBlank/DMA handlers only if the callbacks were not set up yet, so it must come first (no ResetCallback() before it,
// as in PsyQ programs: VSync() would never return). PSn00bSDK's PutDispEnv centres the picture for PAL (no screen.y
// offset as with PsyQ).
static void	InitDisplayPAL(DISPENV *disp, int w, int h)
{
	ResetGraph(0);
	SetDefDispEnv(disp, 0, 0, w, h);
	if (*((char *)0xbfc7ff52) == 'E')	// Check BIOS Video Mode (J/A -> NTSC, E -> PAL)
		SetVideoMode(MODE_PAL);
	SetDispMask(1);
	PutDispEnv(disp);
}

// Busy wait without interrupts (PSn00bSDK's VSync() waits for the VBlank interrupt, so it can't be used inside a
// critical section): root counter 2, which the BIOS SetRCnt sets to the system clock (33868800 Hz; PsyQ's SetRCnt
// selected system clock / 8)
#define	RCNT2_TICKS_PER_US	34
static void	DelayUs(uint32_t us)
{
	uint32_t	ticks = us * RCNT2_TICKS_PER_US + 1;

	SetRCnt(RCntCNT2, 0xFFFF, RCntMdNOINTR);
	while (ticks)
	{
		uint32_t	n = (ticks > 50000) ? 50000 : ticks;

		ResetRCnt(RCntCNT2);
		while (GetRCnt(RCntCNT2) < n);
		ticks -= n;
	}
}

#endif
