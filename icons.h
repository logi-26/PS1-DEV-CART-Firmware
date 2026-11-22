/*
* PS1 DEV CART: the pad's buttons as icons in the menu's texts (the help bars of the PicoStation screen and the cheat
* screen): ICON_CROSS... are characters of the text, drawn by DrawTextIcons in the buttons' colors.
*
* The icons are made once in VRAM (white, tinted as sprites): x 728-791, y 448-455, beside the screensaver's logo
* (saver.h) and under the cover slots (sdbrowser.h). 64 x 8 pixels in one transfer: a whole number of 16-word DMA
* blocks (PSn00bSDK's LoadImage sends whole blocks, the words past the image reach the GPU as commands).
* ✕ ○ △ □: 8 pixels wide (7 drawn, a column of space after them), START and SELECT: 16 (15 drawn).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _ICONS_H_
#define _ICONS_H_

#define	ICON_CROSS		"\001"
#define	ICON_CIRCLE		"\002"
#define	ICON_TRIANGLE	"\003"
#define	ICON_SQUARE		"\004"
#define	ICON_START		"\005"
#define	ICON_SELECT		"\006"
#define	ICONS			6

#define	ICON_VRAM_X		728
#define	ICON_VRAM_Y		448
#define	ICON_TPAGE_X	704		// (their texture page: u = x - 704, v = y - 256)

static bool		icons_ok;		// (cleared when VRAM may have been used: the PicoStation screen's start)
static DR_TPAGE	icon_tpage;
static SPRT		icon_sprite;

// An icon's place in the image and its width (the small ones first, then START and SELECT)
#define	ICON_U(_k_)		(((_k_) < 4) ? (_k_) * 8 : 32 + ((_k_) - 4) * 16)
#define	ICON_W(_k_)		(((_k_) < 4) ? 8 : 16)

static void	IconsMake(void)
{
	static const uint16_t	bits[ICONS][8] = {	// (bit 15: the left column)
		{0xC600, 0xEE00, 0x7C00, 0x3800, 0x7C00, 0xEE00, 0xC600, 0},	// Cross
		{0x3800, 0x6C00, 0xC600, 0xC600, 0xC600, 0x6C00, 0x3800, 0},	// Circle
		{0x1000, 0x3800, 0x2800, 0x6C00, 0x4400, 0xFE00, 0xFE00, 0},	// Triangle
		{0xFE00, 0xFE00, 0xC600, 0xC600, 0xC600, 0xFE00, 0xFE00, 0},	// Square
		{0x3FFC, 0x7DFE, 0xFCFE, 0xFC7E, 0xFCFE, 0x7DFE, 0x3FFC, 0},	// START: a pill, an arrow in it
		{0, 0x3FFC, 0x7FFE, 0x7FFE, 0x7FFE, 0x3FFC, 0, 0}};			// SELECT: a flat pill
	uint32_t				img[64 * 8 / 2];
	uint16_t				*p = (uint16_t *)img;
	RECT					rect = {ICON_VRAM_X, ICON_VRAM_Y, 64, 8};
	int						k, x, y;

	for (y = 0; y < 8; y++)
		for (k = 0; k < ICONS; k++)
			for (x = 0; x < ICON_W(k); x++)
				*p++ = (bits[k][y] & (0x8000 >> x)) ? 0x7FFF : 0;
	LoadImage(&rect, img);
	DrawSync(0);
	icons_ok = true;
}

// A text's width in pixels (the icons: their width)
static int	IconTextWidth(const char *text)
{
	int	w = 0;

	for (; *text; text++)
		w += ((*text >= 1) && (*text <= ICONS)) ? ICON_W(*text - 1) : FONT_CHAR_W;
	return (w);
}

// A text with icons at x, y: the icons in their colors, the rest in r, g, b
static void	DrawTextIcons(int x, int y, const char *text, int r, int g, int b)
{
	static const uint8_t	tint[ICONS][3] = {{55, 80, 128}, {128, 45, 45}, {30, 110, 75}, {120, 60, 100},
		{85, 85, 95}, {85, 85, 95}};
	char					seg[TEXT_MAX + 1];
	int						n = 0, k;

	if (!icons_ok)
		IconsMake();
	for (;; text++)
	{
		if (((*text >= 1) && (*text <= ICONS)) || !*text)
		{
			if (n)
			{
				seg[n] = 0;
				DrawTextColor(x, y, seg, r, g, b);
				x += n * FONT_CHAR_W;
				n = 0;
			}
			if (!*text)
				break;
			k = *text - 1;
			setDrawTPage(&icon_tpage, 1, 0, getTPage(2, 0, ICON_TPAGE_X, 256));
			setSprt(&icon_sprite);
			setXY0(&icon_sprite, x, y);
			setWH(&icon_sprite, ICON_W(k), 8);
			setUV0(&icon_sprite, ICON_VRAM_X - ICON_TPAGE_X + ICON_U(k), ICON_VRAM_Y - 256);
			setRGB0(&icon_sprite, tint[k][0], tint[k][1], tint[k][2]);
			catPrim(&icon_tpage, &icon_sprite);
			termPrim(&icon_sprite);
			DrawOTag((uint32_t *)&icon_tpage);
			DrawSync(0);
			x += ICON_W(k);
		}
		else if (n < TEXT_MAX)
			seg[n++] = *text;
	}
}

#endif
