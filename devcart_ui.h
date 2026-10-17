/*
* PS1 DEV CART tools: common screen style (the look of the SPU synth)
*
* Dark blue background, a title bar with the tool name (and a status on the right), panels, a help line at the
* bottom. The tools draw their own picture/text as before; the bars and panels are drawn after it, then the title and
* help text (debug font of PSn00bSDK, FntLoad first).
*
*	UiInit(draw):		background color of both drawing environments, title/help text streams (open them first:
*						the tool's own text stream is opened after, its FntPrint/FntFlush use its id)
*	UiTitle(name, right)	title bar text, right: shown on the right (NULL: nothing)
*	UiHelp(text)		help line at the bottom (NULL: no help bar)
*	UiPanel(x, y, w, h)	a panel (behind the text), UiTile(...): a colored rectangle (e.g. a progress bar)
*	UiFlush()			once per frame after PutDrawEnv (and after the tool's DrawOTag): bars, panels, title, help
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _DEVCART_UI_H_
#define _DEVCART_UI_H_

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>

// Colors (r, g, b), as in the synth
#define	UI_BG			14, 16, 28		// Background
#define	UI_BAR			40, 50, 120		// Title bar
#define	UI_PANEL		20, 24, 40		// Panels, help bar
#define	UI_SELECT		50, 70, 130		// Selected line
#define	UI_ACCENT		80, 160, 255	// Progress, levels
#define	UI_TRACK		60, 60, 60		// Empty part of a progress bar
#define	UI_OK			80, 220, 80
#define	UI_ERROR		230, 70, 70

// setRGB0 with a color above (two levels: the color is expanded into its 3 values first)
#define	UI_SET_RGB(_p_, _c_)			_UI_SET_RGB(_p_, _c_)
#define	_UI_SET_RGB(_p_, _r_, _g_, _b_)	setRGB0(_p_, _r_, _g_, _b_)

#define	UI_SCREEN_W		320
#define	UI_TITLE_H		14				// Title bar, text at (8, 4)
#define	UI_HELP_Y		226				// Help bar, text at (8, UI_HELP_Y + 3)
#define	UI_HELP_H		14
#define	UI_BODY_Y		20				// First text line of the body
#define	UI_CHARS		38				// Characters per line in the bars (304 pixels)

// PS1 DEV CART version: the firmware (shown on the menu) and the PS1 tools
#define	DEVCART_VERSION	"1.0"

#define	UI_TILES		64				// Bars, panels and tiles per frame

static TILE		ui_tiles[2][UI_TILES];
static uint32_t	ui_ot[2][UI_TILES];
static int		ui_side, ui_count, ui_fnt_title, ui_fnt_help, ui_help;

static void	UiInit(DRAWENV *draw)
{
	int	i;

	for (i = 0; i < 2; i++)
	{
		draw[i].isbg = 1;
		UI_SET_RGB(&draw[i], UI_BG);
	}
	ui_fnt_title = FntOpen(8, 4, 304, 8, 0, 48);
	ui_fnt_help = FntOpen(8, UI_HELP_Y + 3 - 8, 304, 16, 0, 96);	// (2 lines: the help bar grows up)
	ClearOTagR(ui_ot[ui_side], UI_TILES);
}

// Panels with text: the text stream of a panel at (x, y), width w, "lines" lines of text (8 pixels), with the same
// margins everywhere. UiTextPanel draws the panel (each frame), the text goes to the stream with FntPrint.
#define	UI_PANEL_H(_lines_)		((_lines_) * 8 + 12)

static int	UiOpenText(int x, int y, int w, int lines)
{
	// (the character count includes the line ends and the characters cut at the right: twice the visible ones)
	return (FntOpen(x + 8, y + 6, w - 16, lines * 8, 0, lines * (w - 16) / 4 + 16));
}

// A colored rectangle, drawn in the order of the calls (before the text)
static void	UiTile(int x, int y, int w, int h, int r, int g, int b)
{
	TILE	*t;

	if (ui_count >= UI_TILES)
		return;
	t = &ui_tiles[ui_side][ui_count];
	setTile(t);
	setXY0(t, x, y);
	setWH(t, w, h);
	setRGB0(t, r, g, b);
	addPrim(&ui_ot[ui_side][UI_TILES - 1 - ui_count], t);	// (the ordering table is drawn from its last entry)
	ui_count++;
}

static void	UiPanel(int x, int y, int w, int h)
{
	UiTile(x, y, w, h, UI_PANEL);
}

static void	UiTextPanel(int x, int y, int w, int lines)
{
	UiTile(x, y, w, UI_PANEL_H(lines), UI_PANEL);
}

// Progress bar: done/total of the width
static void	UiProgress(int x, int y, int w, int h, int done, int total)
{
	UiTile(x, y, w, h, UI_TRACK);
	if (total > 0)
		UiTile(x, y, (done >= total) ? w : w * done / total, h, UI_ACCENT);
}

static void	UiTitle(const char *name, const char *right)
{
	char	line[UI_CHARS + 1];
	int		n = strlen(name), r = right ? strlen(right) : 0;

	UiTile(0, 0, UI_SCREEN_W, UI_TITLE_H, UI_BAR);
	if (n > UI_CHARS)
		n = UI_CHARS;
	memset(line, ' ', UI_CHARS);
	memcpy(line, name, n);
	if (r && (n + 1 + r <= UI_CHARS))
		memcpy(line + UI_CHARS - r, right, r);
	line[(r && (n + 1 + r <= UI_CHARS)) ? UI_CHARS : n] = 0;
	FntPrint(ui_fnt_title, "%s", line);
}

// One or two lines ("\n" between them)
static void	UiHelp(const char *text)
{
	int	two;

	ui_help = (text != NULL);
	if (!text)
		return;
	two = (strchr(text, '\n') != NULL);
	UiTile(0, UI_HELP_Y - (two ? 8 : 0), UI_SCREEN_W, UI_HELP_H + (two ? 8 : 0), UI_PANEL);
	FntPrint(ui_fnt_help, two ? "%s" : "\n%s", text);
}

// Colored text (the debug font streams are white): the sprites of FntSort, tinted (128, 128, 128 = white), drawn
// after the panels. One line, at x, y.
#define	UI_TEXT_PRIMS	4096			// Bytes of sprites per frame
#define	UI_YELLOW		128, 110, 30	// Tint for UiColorText: names, titles

static uint8_t	ui_text_prims[2][UI_TEXT_PRIMS];
static uint32_t	ui_text_ot[2];
static uint8_t	*ui_text_ptr;

static void	UiColorText(int x, int y, const char *text, int r, int g, int b)
{
	uint8_t	*start, *end, *p;

	if (!ui_text_ptr)
	{
		ui_text_ptr = ui_text_prims[ui_side];
		ClearOTagR(&ui_text_ot[ui_side], 1);
	}
	if (ui_text_ptr + 16 * (strlen(text) + 2) > ui_text_prims[ui_side] + UI_TEXT_PRIMS)
		return;
	start = ui_text_ptr;
	end = FntSort(&ui_text_ot[ui_side], start, x, y, text);
	// The sprites (codes 0x64-0x7F) get the color, and the texture is modulated by it (bit 0 of the code cleared: FntSort
	// makes "raw texture" sprites, drawn white whatever the color); the texture page settings are kept
	for (p = start; p < end; p += ((p[3] + 1) * 4))
		if ((p[7] & 0xE0) == 0x60)
		{
			p[4] = r;
			p[5] = g;
			p[6] = b;
			p[7] &= ~1;
		}
	ui_text_ptr = end;
}

static void	UiFlush(void)
{
	DrawOTag(&ui_ot[ui_side][UI_TILES - 1]);
	if (ui_text_ptr)
		DrawOTag(&ui_text_ot[ui_side]);
	FntFlush(ui_fnt_title);
	FntFlush(ui_fnt_help);
	ui_side ^= 1;					// The other buffer for the next frame (this one may still be drawn)
	ui_count = 0;
	ui_text_ptr = NULL;
	ClearOTagR(ui_ot[ui_side], UI_TILES);
}

#endif
