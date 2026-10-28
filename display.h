/*
* PS1USB firmware by OrionSoft [2024]
* Minimal display functions using only Load/MoveImage to minimize memory footprint
*
* PS1 DEV CART: the menu has the colors of the tools (devcart_ui.h): dark blue background, panels, the logo on top.
* The text is drawn with sprites from the font in VRAM (black is transparent), so it can be tinted and drawn on the
* panels; panels and lines are VRAM fills, the activity log scrolls with MoveImage. One buffer, no ordering table.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "devcart_ui.h"

#define SCREEN_WIDTH	320
#define SCREEN_HEIGHT	240
#define SCREEN_BG_COLOR	UI_BG		// (R, G, B)

void	InitDisplay(void)
{
	DISPENV	disp;
	DRAWENV	draw;

	ResetGraph(0);
	SetDefDispEnv(&disp, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	if (*((char *)0xbfc7ff52) == 'E')	// Check BIOS Video Mode (J/A -> NTSC, E -> PAL)
		SetVideoMode(MODE_PAL);	// (PSn00bSDK's PutDispEnv centres the picture for PAL: no screen.y offset as with PsyQ)
	SetDispMask(1);
	PutDispEnv(&disp);
	SetDefDrawEnv(&draw, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);	// The text sprites are drawn in the displayed buffer
	draw.dfe = 1;
	PutDrawEnv(&draw);
}

void	LoadTexture(uint32_t *tim)
{
	TIM_IMAGE	tparam;

	GetTimInfo(tim, &tparam);
	LoadImage(tparam.prect, tparam.paddr);	// Load Image in VRAM
	DrawSync(0);
}

// A rectangle drawn with a TILE primitive: exact to the pixel (a VRAM fill, ClearImage, works on 16 pixel columns:
// a panel at x = 8 lost its last 8 pixels)
static TILE	fill_tile;

static void	FillRect(int x, int y, int w, int h, int r, int g, int b)
{
	setTile(&fill_tile);
	setXY0(&fill_tile, x, y);
	setWH(&fill_tile, w, h);
	setRGB0(&fill_tile, r, g, b);
	termPrim(&fill_tile);
	DrawOTag((uint32_t *)&fill_tile);
	DrawSync(0);
}

/****************************************/

#define	FONT_U					(1024-256)
#define	FONT_V					0
#define	FONT_CHAR_W				8
#define	FONT_CHAR_H				8
#define	FONT_N_CHAR_PER_LINE	32	// Must be multiple of power of 2
#define	FONT_N_FONT_LINE		3	// Must be multiple of power of 2
#define	FONT_FIRST_CHAR			'!'
#define	TEXT_MAX				40	// Characters per call

// Text colors (sprite color, 128 = the font as it is: white)
#define	TEXT_WHITE				128, 128, 128
#define	TEXT_DIM				90, 104, 150	// Labels
#define	TEXT_ACCENT				80, 160, 255

static DR_TPAGE	text_tpage;
static SPRT_8	text_sprites[TEXT_MAX];

void	DrawTextColor(int x, int y, const char *text, int r, int g, int b)
{
	void	*last = &text_tpage;
	int		n = 0;

	// 16-bit texture page of the font, drawing in the displayed area allowed
	setDrawTPage(&text_tpage, 1, 1, getTPage(2, 0, FONT_U, FONT_V));
	while (*text && (n < TEXT_MAX))
	{
		int	c = (*text++) - FONT_FIRST_CHAR;

		if ((c >= 0) && (c < (FONT_N_CHAR_PER_LINE*FONT_N_FONT_LINE)))
		{
			SPRT_8	*s = &text_sprites[n++];

			setSprt8(s);
			setXY0(s, x, y);
			setUV0(s, (c & (FONT_N_CHAR_PER_LINE-1)) * FONT_CHAR_W, (c / FONT_N_CHAR_PER_LINE) * FONT_CHAR_H);
			setRGB0(s, r, g, b);
			catPrim(last, s);
			last = s;
		}
		x += FONT_CHAR_W;
	}
	termPrim(last);
	DrawOTag((uint32_t *)&text_tpage);
	DrawSync(0);
}

#define	DrawText(_x_, _y_, _text_)	DrawTextColor(_x_, _y_, _text_, TEXT_WHITE)

/****************************************/

// Menu: logo (logo.tim, 304x40 at 8,6 in cartrom/tim.tfs), accent line, Firmware and Device ID panels, USB status
// panel, activity log panel
#define	LOGO_OFFSET_Y		6
#define	LOGO_HEIGHT			40
#define	ACCENT_Y			(LOGO_OFFSET_Y + LOGO_HEIGHT + 4)
#define	INFO_PANEL_Y		(ACCENT_Y + 8)
#define	INFO_PANEL_H		20	// One line: label and value
#define	STATUS_PANEL_Y		(INFO_PANEL_Y + INFO_PANEL_H + 6)
#define	STATUS_PANEL_H		20
#define	LOG_PANEL_Y			(STATUS_PANEL_Y + STATUS_PANEL_H + 6)
#define	LOG_PANEL_H			(SCREEN_HEIGHT - 8 - LOG_PANEL_Y)

#define	CONSOLE_OFFSET_X	16
#define	CONSOLE_WIDTH		(SCREEN_WIDTH - CONSOLE_OFFSET_X * 2)
#define	CONSOLE_OFFSET_Y	(LOG_PANEL_Y + 6 + FONT_CHAR_H + 4)	// Under the "ACTIVITY" label
#define	CONSOLE_HEIGHT		(((LOG_PANEL_Y + LOG_PANEL_H - 6 - CONSOLE_OFFSET_Y) / FONT_CHAR_H) * FONT_CHAR_H)
#define	CONSOLE_N_CHAR		(CONSOLE_WIDTH/FONT_CHAR_W)
#define	CONSOLE_N_LINE		(CONSOLE_HEIGHT/FONT_CHAR_H)

int		ConsoleCurLine;

// Background, accent line and the panels (the logo is loaded on top of it, see main)
void	DrawMenuFrame(void)
{
	RECT	screen = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};

	ClearImage(&screen, UI_BG);		// (whole screen: 16 pixel columns are fine)
	DrawSync(0);
	FillRect(8, ACCENT_Y, SCREEN_WIDTH - 16, 2, UI_BAR);
	FillRect(8, INFO_PANEL_Y, 148, INFO_PANEL_H, UI_PANEL);
	FillRect(164, INFO_PANEL_Y, 148, INFO_PANEL_H, UI_PANEL);
	FillRect(8, STATUS_PANEL_Y, SCREEN_WIDTH - 16, STATUS_PANEL_H, UI_PANEL);
	FillRect(8, LOG_PANEL_Y, SCREEN_WIDTH - 16, LOG_PANEL_H, UI_PANEL);
}

// Panel labels (once the font is loaded)
void	DrawMenuLabels(void)
{
	DrawTextColor(16, INFO_PANEL_Y + 6, "FIRMWARE", TEXT_DIM);
	DrawTextColor(172, INFO_PANEL_Y + 6, "ID", TEXT_DIM);
	DrawTextColor(16, LOG_PANEL_Y + 6, "ACTIVITY", TEXT_DIM);
}

// A value in an info panel (0: left, 1: right), after its label
void	DrawInfo(int panel, const char *text)
{
	int	x = panel ? (172 + 3 * FONT_CHAR_W) : (16 + 9 * FONT_CHAR_W);	// After "ID " / "FIRMWARE "

	FillRect(x, INFO_PANEL_Y + 6, (panel ? 312 : 156) - 8 - x, FONT_CHAR_H, UI_PANEL);
	DrawText(x, INFO_PANEL_Y + 6, text);
}

// USB status: light (green/red) and text
void	DrawStatus(int ok, const char *text)
{
	FillRect(16, STATUS_PANEL_Y + 6, 288, FONT_CHAR_H, UI_PANEL);
	if (ok)
		FillRect(16, STATUS_PANEL_Y + 6, 12, FONT_CHAR_H, UI_OK);
	else
		FillRect(16, STATUS_PANEL_Y + 6, 12, FONT_CHAR_H, UI_ERROR);
	DrawText(36, STATUS_PANEL_Y + 6, text);
}

void	InitConsole(void)
{
	ConsoleCurLine = CONSOLE_OFFSET_Y;
}

void	PrintConsole(char *text)
{
	if (ConsoleCurLine >= (CONSOLE_OFFSET_Y + CONSOLE_HEIGHT))
	{
		RECT	scroll = {CONSOLE_OFFSET_X, CONSOLE_OFFSET_Y + FONT_CHAR_H, CONSOLE_WIDTH, CONSOLE_HEIGHT - FONT_CHAR_H};

		// Scroll one line up, clear the last line
		MoveImage(&scroll, CONSOLE_OFFSET_X, CONSOLE_OFFSET_Y);
		DrawSync(0);
		FillRect(CONSOLE_OFFSET_X, CONSOLE_OFFSET_Y + ((CONSOLE_N_LINE - 1) * FONT_CHAR_H), CONSOLE_WIDTH, FONT_CHAR_H,
			UI_PANEL);
		ConsoleCurLine -= FONT_CHAR_H;
	}
	DrawText(CONSOLE_OFFSET_X, ConsoleCurLine, text);
	ConsoleCurLine += FONT_CHAR_H;
}
