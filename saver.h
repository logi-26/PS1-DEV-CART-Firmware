/*
* PS1 DEV CART: the screensavers of the menu and of the PicoStation screen (sdbrowser.h)
*
* Each one is set from the computer (the Manager's Settings page, "ps1transfer -saver"): on or off, after 3, 5 or 10
* minutes without a button pressed (or the computer's activity, on the menu), and the display turned off or the
* PS1 DEV CART logo bouncing on a black screen. The settings are saved in the cartridge flash with the device ID
* (ps1devcart.c: "SSVR" and 2 bytes after it), the default (none saved): on, 3 minutes, display off.
*
* Bouncing logo: the screen kept in VRAM meanwhile (at y 256, as the cheat screen does) and put back when it ends;
* the logo (logo.tim of the ROM, 304 x 40) made half as big (152 x 20, its background color black) in VRAM beside
* the PicoStation screen's 12 pixel font, moved one pixel each way every 3 frames, bouncing off the screen's edges.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _SAVER_H_
#define _SAVER_H_

// A screensaver's setting (saver_cfg): the timeout's index (3, 5, 10 minutes), flags
#define	SAVER_TIME		0x03
#define	SAVER_LOGO		0x04	// The bouncing logo (else: the display off)
#define	SAVER_OFF		0x08	// No screensaver
#define	SAVER_MENU		0		// saver_cfg[]: the menu's, the PicoStation screen's, then the menu's options
#define	SAVER_PICO		1

#define	SAVER_OPTS		2		// saver_cfg[SAVER_OPTS]: the menu's options, OPT_*
#define	OPT_NO_DETAILS	0x01	// The PicoStation screen's list: no discs and cheats counts (faster: nothing read)
#define	OPT_NO_EDC		0x02	// The PicoStation's EDC correction off for the games mounted (sdbrowser.h SdOptions)
#define	OPT_NO_BOOTFIX	0x04	// Its bootsector fix off

uint8_t			saver_cfg[3];	// (read from the flash: LoadDeviceId)
uint8_t			saver_rx[3];	// Received by the 'S' command, see usb.h
static bool		saver_on;
static uint8_t	saver_logo;		// (the running one: the logo, else the display off)
static int		saver_x, saver_y, saver_dx, saver_dy;

#define	SAVER_SAVE_Y	256		// The screen kept there (the cheat screen's place: not in use meanwhile)
#define	SAVER_LOGO_X	576		// The small logo in VRAM (x 320-575: the 12 pixel font, see sdbrowser.h)
#define	SAVER_LOGO_Y	448
#define	SAVER_LOGO_W	152
#define	SAVER_LOGO_H	20
#define	SAVER_LOGO_BG	0x0C41	// logo.tim's background (the menu's color): black here
#define	SAVER_STEP_FRAMES	3	// The logo moves a pixel each way every 3 frames (about 17 pixels a second on PAL)
#define	SAVER_TIM_RAM	0x80010000	// logo.tim copied there (LoadImage doesn't read the ROM), the small one after it
#define	SAVER_SMALL_RAM	0x80018000

// A screensaver's timeout in frames, 0: off
static int	SaverFrames(int which)
{
	static const uint8_t	minutes[] = {3, 5, 10, 10};
	int						c = saver_cfg[which];

	if (c & SAVER_OFF)
		return (0);
	return (minutes[c & SAVER_TIME] * 60 * ((GetVideoMode() == MODE_PAL) ? 50 : 60));
}

// logo.tim made half as big (each pixel: the average of 4, the background counted as black) into VRAM
static bool	SaverLogoMake(void)
{
	uint16_t			*src = (uint16_t *)(SAVER_TIM_RAM + 20), *out = (uint16_t *)SAVER_SMALL_RAM;
	RECT				rect = {SAVER_LOGO_X, SAVER_LOGO_Y, SAVER_LOGO_W, SAVER_LOGO_H};
	int					x, y, i;

	if (ROM_LoadFile("logo.tim", (uint8_t *)SAVER_TIM_RAM) < 20 + 2 * 4 * SAVER_LOGO_W * SAVER_LOGO_H)
		return (false);
	for (y = 0; y < SAVER_LOGO_H; y++)
		for (x = 0; x < SAVER_LOGO_W; x++)
		{
			int	r = 0, g = 0, b = 0;

			for (i = 0; i < 4; i++)
			{
				uint16_t	c = src[(2 * y + (i >> 1)) * 2 * SAVER_LOGO_W + 2 * x + (i & 1)];

				if (c != SAVER_LOGO_BG)
				{
					r += c & 31;
					g += (c >> 5) & 31;
					b += (c >> 10) & 31;
				}
			}
			*out++ = (r >> 2) | ((g >> 2) << 5) | ((b >> 2) << 10);
		}
	LoadImage(&rect, (uint32_t *)SAVER_SMALL_RAM);
	DrawSync(0);
	return (true);
}

static void	SaverStart(int which)
{
	RECT	screen = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};

	saver_on = true;
	saver_logo = (saver_cfg[which] & SAVER_LOGO) && SaverLogoMake();
	if (!saver_logo)
	{
		SetDispMask(0);
		return;
	}
	MoveImage(&screen, 0, SAVER_SAVE_Y);
	DrawSync(0);
	FillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, 0);
	saver_x = (SCREEN_WIDTH - SAVER_LOGO_W) / 2;
	saver_y = (SCREEN_HEIGHT - SAVER_LOGO_H) / 2;
	saver_dx = saver_dy = 1;
}

// Called often while it runs (the menu: many times a frame): every SAVER_STEP_FRAMES frames the logo moved (what it leaves behind cleared, then the logo copied over the rest)
static void	SaverFrame(void)
{
	RECT	logo = {SAVER_LOGO_X, SAVER_LOGO_Y, SAVER_LOGO_W, SAVER_LOGO_H};

	static int	last;	// (the frame it last moved: the menu's loop runs many times a frame, counted in frames)

	if (!saver_on || !saver_logo || (VSync(-1) - last < SAVER_STEP_FRAMES))
		return;
	last = VSync(-1);
	if ((saver_x + saver_dx < 0) || (saver_x + saver_dx > SCREEN_WIDTH - SAVER_LOGO_W))
		saver_dx = -saver_dx;
	if ((saver_y + saver_dy < 0) || (saver_y + saver_dy > SCREEN_HEIGHT - SAVER_LOGO_H))
		saver_dy = -saver_dy;
	FillRect((saver_dx > 0) ? saver_x : saver_x + SAVER_LOGO_W - 1, saver_y, 1, SAVER_LOGO_H, 0, 0, 0);
	FillRect(saver_x, (saver_dy > 0) ? saver_y : saver_y + SAVER_LOGO_H - 1, SAVER_LOGO_W, 1, 0, 0, 0);
	saver_x += saver_dx;
	saver_y += saver_dy;
	MoveImage(&logo, saver_x, saver_y);
	DrawSync(0);
}

// The screen as it was
static void	SaverStop(void)
{
	RECT	saved = {0, SAVER_SAVE_Y, SCREEN_WIDTH, SCREEN_HEIGHT};

	if (!saver_on)
		return;
	saver_on = false;
	if (!saver_logo)
	{
		SetDispMask(1);
		return;
	}
	MoveImage(&saved, 0, 0);
	DrawSync(0);
}

#endif
