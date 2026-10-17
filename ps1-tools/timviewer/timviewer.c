/*
* PS1 DEV CART TIM and VRAM viewer
* Uploaded and executed by "ps1transfer -tim file.tim [-tim file.tim ...]"
*
* The TIM files are loaded in VRAM at their own position, then 3 views (Select to change):
* - VRAM map:	the whole VRAM scaled to the screen, image and CLUT of each TIM outlined, overlaps in red
* - TIM view:	the selected TIM drawn with its color depth and CLUT, zoom/scroll, palette swatches
* - VRAM 1:1:	scroll in the raw VRAM (16-bit view) with the outlines
* Warnings: overlaps, CLUT x not multiple of 16, CLUT size not matching the color depth,
* 4/8-bit texture crossing a texture page, outside of VRAM.
*
* The viewer screen buffers and font are placed in VRAM areas not used by the TIM files when possible.
*
* Controls:	Select: view, L1/R1: TIM, D-pad: scroll (R2: faster), Triangle/Cross: zoom (TIM view),
*			L2/R2: CLUT row (TIM view), Square: draw with the CLUT of another TIM (TIM view, same CLUT width),
*			Circle: palette editor (TIM view), Start: reset scroll/zoom
* Palette editor: D-pad: entry, L2/R2: red/green/blue, Triangle/Cross: +/- (held: repeat), Square: restore the row,
*			Start: save NAME_edit.tim, Select: reload NAME.tim (edited in a paint program), Circle: exit.
*			Save/reload use the PCDRV file server: "ps1transfer -tim file.tim -c".
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
#include "devcart_ui.h"
#include "psxpcdrv.h"

// Data block written by ps1transfer, see "-tim" in ps1transfer/main.c
#define	VIEWER_DATA_ADRS	0x80100000
#define	VIEWER_MAGIC		0x564D4954	// "TIMV"
#define	VIEWER_MAX_TIM		16
#define	VIEWER_NAME_LEN		24

typedef struct
{
	uint32_t	adrs;
	uint32_t	size;
	char	name[VIEWER_NAME_LEN];
} ViewerFile;

typedef struct
{
	uint32_t		magic;
	uint32_t		count;
	ViewerFile	files[VIEWER_MAX_TIM];
} ViewerData;

#define	SCREEN_W		320
#define	SCREEN_H		240
#define	VRAM_W			1024
#define	VRAM_H			512
#define	OT_LEN			4
#define	PRIM_SIZE		(96 * 1024)
#define	PIECE			32			// VRAM is drawn by pieces of 32x32 VRAM pixels (texture page limits)
#define	MAP_Y			14			// VRAM map: 1024x512 scaled by 5/16 = 320x160 (under the title bar)
#define	MAP_INFO_Y		174			// Info panel under the map (4 lines)
#define	TIM_INFO_Y		160			// Info panel of the TIM view (5 lines)
#define	TIM_Y			20			// Image of the TIM view
#define	MAP_NUM			5
#define	MAP_DEN			16
#define	MAX_RECTS		(VIEWER_MAX_TIM * 2 + 3)

enum	{VIEW_MAP, VIEW_TIM, VIEW_VRAM, VIEW_COUNT};

typedef struct
{
	char	*name;
	int		mode;		// 0: 4-bit, 1: 8-bit, 2: 16-bit, 3: 24-bit
	RECT	img, clut;	// VRAM units, clut.w = 0 if no CLUT
	uint16_t	*clut_data;	// In the TIM file (edited in place, so a save writes the edited palette)
	uint16_t	*clut_orig;	// Copy of the original CLUT (restore)
	uint32_t	*file;		// TIM file in the data block
	uint32_t	file_size, slot_size;
	char	warn[160];
} TimInfo;

#define	CLUT_POOL		32768	// Original CLUTs copies (uint16_t)
static uint16_t		clut_pool[CLUT_POOL];
static int			clut_pool_used;
static char			status[64];		// Result of the last palette editor action

#define	PC_CHUNK		8192	// File server transfers by chunks, like the memory card tool

static void	CheckTims(void);

static TimInfo		tims[VIEWER_MAX_TIM];
static int			n_tims;
static DISPENV		disp[2];
static DRAWENV		draw[2];
static uint32_t		ot[2][OT_LEN];
static uint8_t		prims[2][PRIM_SIZE];
static uint8_t		*prim_ptr, *prim_end;
static RECT			screen_rect[2], font_rect;
static int			fnt_info, fnt_map_info;	// Text of the info panels (devcart_ui.h)
static int			viewer_overlaps;		// The viewer screens or font overwrite TIM data

static const uint8_t	colors[8][3] = {{0, 255, 0}, {0, 160, 255}, {255, 0, 255}, {255, 160, 0},
									{0, 255, 255}, {160, 255, 0}, {255, 255, 255}, {160, 120, 255}};
static const char	*mode_names[4] = {"4-bit", "8-bit", "16-bit", "24-bit"};
static const uint8_t	grey[3] = {128, 128, 128};	// Viewer screen buffers and font in the VRAM map

static int	Overlap(RECT *a, RECT *b)
{
	return (a->w && b->w && (a->x < b->x + b->w) && (b->x < a->x + a->w) && (a->y < b->y + b->h) && (b->y < a->y + a->h));
}

static void	*AllocPrim(int size)
{
	void	*p = prim_ptr;

	if (prim_ptr + size > prim_end)
		return (NULL);
	prim_ptr += size;
	return (p);
}

// TIM file: ID, flags, [CLUT block: length, x, y, w, h, data], image block: length, x, y, w, h, data. Loaded in VRAM.
static int	ParseTim(TimInfo *t)
{
	uint32_t	*p = t->file, flags = p[1];
	int		n;

	if (p[0] != 0x10)
		return (0);
	t->mode = flags & 7;
	t->clut.w = 0;
	p += 2;
	if (flags & 8)
	{
		t->clut.x = ((uint16_t *)p)[2];
		t->clut.y = ((uint16_t *)p)[3];
		t->clut.w = ((uint16_t *)p)[4];
		t->clut.h = ((uint16_t *)p)[5];
		t->clut_data = (uint16_t *)(p + 3);
		LoadImage(&t->clut, p + 3);
		n = t->clut.w * t->clut.h;
		t->clut_orig = NULL;
		if (clut_pool_used + n <= CLUT_POOL)
		{
			t->clut_orig = clut_pool + clut_pool_used;
			memcpy(t->clut_orig, t->clut_data, n * 2);
			clut_pool_used += n;
		}
		p += p[0] / 4;
	}
	t->img.x = ((uint16_t *)p)[2];
	t->img.y = ((uint16_t *)p)[3];
	t->img.w = ((uint16_t *)p)[4];
	t->img.h = ((uint16_t *)p)[5];
	LoadImage(&t->img, p + 3);
	DrawSync(0);
	return (1);
}

static void	LoadTims(ViewerData *vd)
{
	int	i;

	for (i = 0; (i < vd->count) && (i < VIEWER_MAX_TIM); i++)
	{
		TimInfo	*t = &tims[n_tims];

		t->name = vd->files[i].name;
		t->file = (uint32_t *)vd->files[i].adrs;
		t->file_size = vd->files[i].size;
		// Space for a reloaded file: up to the next file in the data block
		t->slot_size = ((i + 1 < vd->count) ? vd->files[i + 1].adrs : VIEWER_DATA_ADRS + 0xE0000) - vd->files[i].adrs;
		if (ParseTim(t))
			n_tims++;
	}
}

/****************************************/
// Palette editor: entries edited in the TIM file, the CLUT row is loaded in VRAM at once

static void	UploadClutRow(TimInfo *t, int row)
{
	RECT	r;

	setRECT(&r, t->clut.x, t->clut.y + row, t->clut.w, 1);
	LoadImage(&r, (uint32_t *)(t->clut_data + row * t->clut.w));
	DrawSync(0);
}

// Saved on the computer as name_edit.tim with the PCDRV file server ("ps1transfer -tim ... -c")
static void	SaveTim(TimInfo *t)
{
	char	name[VIEWER_NAME_LEN + 8], *dot;
	int		fd, i, n;

	strcpy(name, t->name);
	if ((dot = strrchr(name, '.')))
		*dot = 0;
	strcat(name, "_edit.tim");
	if ((PCinit() != 0) || ((fd = PCcreat(name, 0)) < 0))
	{
		strcpy(status, "NO FILE SERVER: USE -TIM ... -C");
		return;
	}
	for (i = 0; i < t->file_size; i += n)
	{
		n = (t->file_size - i > PC_CHUNK) ? PC_CHUNK : t->file_size - i;
		if (PCwrite(fd, (char *)t->file + i, n) != n)
			break;
	}
	sprintf(status, (i >= t->file_size) ? "SAVED %s" : "WRITE ERROR %s", name);
	PCclose(fd);
}

// Reloaded from the computer (e.g. edited in a paint program), it must fit in its place of the data block
static void	ReloadTim(TimInfo *t)
{
	int	fd, size, i, n;

	if ((PCinit() != 0) || ((fd = PCopen(t->name, 0, 0)) < 0))
	{
		strcpy(status, "NO FILE SERVER: USE -TIM ... -C");
		return;
	}
	size = PClseek(fd, 0, 2);
	PClseek(fd, 0, 0);
	if ((size <= 0) || (size > t->slot_size))
	{
		sprintf(status, "FILE TOO BIG TO RELOAD (%d)", size);
		PCclose(fd);
		return;
	}
	for (i = 0; i < size; i += n)
	{
		n = (size - i > PC_CHUNK) ? PC_CHUNK : size - i;
		if (PCread(fd, (char *)t->file + i, n) != n)
			break;
	}
	PCclose(fd);
	if (i < size)
		strcpy(status, "READ ERROR");	// The TIM in RAM is incomplete, VRAM keeps the previous one
	else if (!ParseTim(t))
		strcpy(status, "NOT A TIM FILE");
	else
	{
		t->file_size = size;
		CheckTims();
		sprintf(status, "RELOADED %s", t->name);
	}
}

// Warnings for each TIM: overlaps, CLUT position/size, texture page crossing, outside VRAM
static void	CheckTims(void)
{
	int		i, j, page_w;
	TimInfo	*t, *o;
	char	*w;

	for (i = 0; i < n_tims; i++)
	{
		t = &tims[i];
		w = t->warn;
		*w = 0;
		if ((t->img.x + t->img.w > VRAM_W) || (t->img.y + t->img.h > VRAM_H))
			w += sprintf(w, "IMAGE OUTSIDE VRAM\n");
		if (t->clut.w && Overlap(&t->img, &t->clut))
			w += sprintf(w, "IMAGE OVERLAPS ITS CLUT\n");
		for (j = 0; j < n_tims; j++)
		{
			o = &tims[j];
			if (j == i)
				continue;
			if (Overlap(&t->img, &o->img) || Overlap(&t->img, &o->clut))
				w += sprintf(w, "IMAGE OVERLAPS TIM %d %s\n", j + 1, Overlap(&t->img, &o->img) ? "IMAGE" : "CLUT");
			if (Overlap(&t->clut, &o->img) || Overlap(&t->clut, &o->clut))
				w += sprintf(w, "CLUT OVERLAPS TIM %d %s\n", j + 1, Overlap(&t->clut, &o->img) ? "IMAGE" : "CLUT");
			if (w - t->warn > sizeof(t->warn) - 40)
				break;
		}
		if (t->clut.w)
		{
			if (t->clut.x & 15)
				w += sprintf(w, "CLUT X %d NOT MULTIPLE OF 16\n", t->clut.x);
			if (((t->mode == 0) && (t->clut.w != 16)) || ((t->mode == 1) && (t->clut.w != 256)))
				w += sprintf(w, "CLUT WIDTH %d FOR %s\n", t->clut.w, mode_names[t->mode]);
		}
		else if (t->mode < 2)
			w += sprintf(w, "%s WITHOUT CLUT\n", mode_names[t->mode]);
		if (t->mode < 2)	// 4-bit: page 64 VRAM pixels wide, 8-bit: 128 (page x is a multiple of 64)
		{
			page_w = t->mode ? 128 : 64;
			if (((t->img.x & 63) + t->img.w > page_w) || ((t->img.y & 255) + t->img.h > 256))
				w += sprintf(w, "CROSSES A TEXTURE PAGE\n");
		}
		for (j = 0; j < 3; j++)
			if (Overlap(&t->img, (j < 2) ? &screen_rect[j] : &font_rect) ||
				Overlap(&t->clut, (j < 2) ? &screen_rect[j] : &font_rect))
				viewer_overlaps = 1;
	}
}

static int	IsFree(RECT *r, int n_used)
{
	int	k;

	if ((r->x + r->w > VRAM_W) || (r->y + r->h > VRAM_H))
		return (0);
	for (k = 0; k < n_tims; k++)
		if (Overlap(r, &tims[k].img) || Overlap(r, &tims[k].clut))
			return (0);
	for (k = 0; k < n_used; k++)	// Screen buffers already placed
		if (Overlap(r, &screen_rect[k]))
			return (0);
	return (1);
}

// Viewer screen buffers and font in VRAM areas not used by the TIM files (search on a grid).
// With only one free area, both buffers use it (single buffer) instead of overwriting TIM data.
static void	PlaceViewer(void)
{
	int		x, y, n = 0;
	RECT	r;

	for (y = 0; (y <= VRAM_H - SCREEN_H) && (n < 2); y += 16)
		for (x = 0; (x <= VRAM_W - SCREEN_W) && (n < 2); x += 64)
		{
			setRECT(&r, x, y, SCREEN_W, SCREEN_H);
			if (IsFree(&r, n))
				screen_rect[n++] = r;
		}
	if (n == 0)		// No free area: default place, TIM data is overwritten
		setRECT(&screen_rect[n++], 0, 0, SCREEN_W, SCREEN_H);
	if (n == 1)
		screen_rect[1] = screen_rect[0];

	setRECT(&font_rect, 960, 256, 64, 130);
	for (y = VRAM_H - 130; y >= 0; y -= 16)	// Font texture and its CLUT below, from the bottom right
		for (x = VRAM_W - 64; x >= 0; x -= 64)
		{
			setRECT(&r, x, y, 64, 130);
			if (IsFree(&r, 2))
			{
				font_rect = r;
				return;
			}
		}
}

// Draw a VRAM rectangle (VRAM pixels) as textured quads, scaled by num/den, in the texture mode (0/1/2) and CLUT
static void	DrawVram(int vx, int vy, int vw, int vh, int sx, int sy, int num, int den, int mode, uint16_t clut, uint32_t *otp)
{
	int			px, py, pw, ph, tpu = 4 >> mode, u0, v0, u1, v1, x0, y0, x1, y1;
	POLY_FT4	*p;

	for (py = vy; py < vy + vh; py += ph)
	{
		ph = PIECE - (py % PIECE);
		if (py + ph > vy + vh)
			ph = vy + vh - py;
		y0 = sy + (py - vy) * num / den;
		y1 = sy + (py + ph - vy) * num / den;
		if ((y1 <= 0) || (y0 >= SCREEN_H))
			continue;
		for (px = vx; px < vx + vw; px += pw)
		{
			pw = PIECE - (px % PIECE);
			if (px + pw > vx + vw)
				pw = vx + vw - px;
			x0 = sx + (px - vx) * num / den;
			x1 = sx + (px + pw - vx) * num / den;
			if ((x1 <= 0) || (x0 >= SCREEN_W) || (x1 == x0) || (y1 == y0) || !(p = AllocPrim(sizeof(POLY_FT4))))
				continue;
			u0 = (px & 63) * tpu;
			u1 = u0 + pw * tpu;
			v0 = py & 255;
			v1 = v0 + ph;
			setPolyFT4(p);
			setRGB0(p, 128, 128, 128);
			setXY4(p, x0, y0, x1, y0, x0, y1, x1, y1);
			setUV4(p, u0, v0, (u1 > 255) ? 255 : u1, v0, u0, (v1 > 255) ? 255 : v1, (u1 > 255) ? 255 : u1, (v1 > 255) ? 255 : v1);
			p->tpage = getTPage(mode, 0, px & ~63, py & ~255);
			p->clut = clut;
			addPrim(otp, p);
		}
	}
}

static void	DrawTile(int x, int y, int w, int h, int r, int g, int b, uint32_t *otp)
{
	TILE	*t;

	if ((w <= 0) || (h <= 0) || !(t = AllocPrim(sizeof(TILE))))
		return;
	setTile(t);
	setXY0(t, x, y);
	setWH(t, w, h);
	setRGB0(t, r, g, b);
	addPrim(otp, t);
}

static void	DrawOutline(RECT *rc, int sx, int sy, int vx, int vy, int num, int den, const uint8_t *c, uint32_t *otp)
{
	int	x0, y0, x1, y1;

	if (!rc->w)
		return;
	x0 = sx + (rc->x - vx) * num / den;
	y0 = sy + (rc->y - vy) * num / den;
	x1 = sx + (rc->x + rc->w - vx) * num / den;
	y1 = sy + (rc->y + rc->h - vy) * num / den;
	if (x1 <= x0)
		x1 = x0 + 1;
	if (y1 <= y0)
		y1 = y0 + 1;
	DrawTile(x0, y0, x1 - x0, 1, c[0], c[1], c[2], otp);
	DrawTile(x0, y1 - 1, x1 - x0, 1, c[0], c[1], c[2], otp);
	DrawTile(x0, y0, 1, y1 - y0, c[0], c[1], c[2], otp);
	DrawTile(x1 - 1, y0, 1, y1 - y0, c[0], c[1], c[2], otp);
}

// Outlines of all TIM images/CLUTs (selected one in yellow), overlap areas in red
static void	DrawAllOutlines(int sx, int sy, int vx, int vy, int num, int den, int selected, uint32_t *otp)
{
	static const uint8_t	yellow[3] = {255, 255, 0}, red[3] = {255, 0, 0};
	int					i, j;
	RECT				*a, *b, r;

	for (i = 0; i < n_tims; i++)
	{
		const uint8_t	*c = (i == selected) ? yellow : colors[i & 7];

		DrawOutline(&tims[i].img, sx, sy, vx, vy, num, den, c, otp);
		DrawOutline(&tims[i].clut, sx, sy, vx, vy, num, den, c, otp);
	}
	for (i = 0; i < n_tims * 2; i++)
		for (j = i + 1; j < n_tims * 2; j++)
		{
			a = (i & 1) ? &tims[i / 2].clut : &tims[i / 2].img;
			b = (j & 1) ? &tims[j / 2].clut : &tims[j / 2].img;
			if (!Overlap(a, b))
				continue;
			r.x = (a->x > b->x) ? a->x : b->x;
			r.y = (a->y > b->y) ? a->y : b->y;
			r.w = ((a->x + a->w < b->x + b->w) ? a->x + a->w : b->x + b->w) - r.x;
			r.h = ((a->y + a->h < b->y + b->h) ? a->y + a->h : b->y + b->h) - r.y;
			DrawOutline(&r, sx, sy, vx, vy, num, den, red, otp);
		}
}

// Palette entry cursor: outline around the swatch
static void	DrawCursor(int x, int y, int size, uint32_t *otp)
{
	DrawTile(x - 1, y - 1, size + 2, 1, 255, 255, 255, otp);
	DrawTile(x - 1, y + size, size + 2, 1, 255, 255, 255, otp);
	DrawTile(x - 1, y, 1, size, 255, 255, 255, otp);
	DrawTile(x + size, y, 1, size, 255, 255, 255, otp);
}

// Next TIM with a CLUT of the same width (-1: the TIM's own CLUT)
static int	NextSwap(int sel, int swap)
{
	int	i, k;

	for (k = 1; k <= n_tims; k++)
	{
		i = ((swap < 0) ? sel : swap) + k;
		i %= n_tims;
		if (i == sel)
			return (-1);
		if (tims[i].clut.w && (tims[i].clut.w == tims[sel].clut.w))
			return (i);
	}
	return (-1);
}

static void	PrintTimInfo(int i, int fnt)
{
	TimInfo	*t = &tims[i];
	int		width = (t->mode == 3) ? t->img.w * 2 / 3 : t->img.w * (4 >> t->mode);	// Pixels

	FntPrint(fnt, "TIM %d/%d %s\n", i + 1, n_tims, t->name);
	FntPrint(fnt, "IMAGE %s %dx%d AT %d,%d\n", mode_names[t->mode & 3], width, t->img.h, t->img.x, t->img.y);
	if (t->clut.w)
		FntPrint(fnt, "CLUT  %dx%d AT %d,%d\n", t->clut.w, t->clut.h, t->clut.x, t->clut.y);
	FntPrint(fnt, "%s", t->warn[0] ? t->warn : "NO PROBLEM FOUND\n");
}

int	main(void)
{
	ViewerData	*vd = (ViewerData *)VIEWER_DATA_ADRS;
	uint32_t		pad, opad = 0;
	int			side = 0, i, j, view = VIEW_MAP, sel = 0, zoom = 1, clut_row = 0, is_pal, speed;
	int			scroll_x = 0, scroll_y = 0;
	int			swap = -1, edit = 0, entry = 0, comp = 0, held = 0;
	static const char	*comp_names[3] = {"RED", "GREEN", "BLUE"};

	// Same video mode as the console region, like the PS1 DEV CART menu (BIOS version string: 'E' = Europe = PAL)
	is_pal = (*((char *)0xBFC7FF52) == 'E');
	ResetGraph(0);	// (first: PSn00bSDK's ResetGraph sets up the callbacks, see psyq_compat.h)
	SetVideoMode(is_pal ? MODE_PAL : MODE_NTSC);
	PadInit(0);

	if (vd->magic == VIEWER_MAGIC)
		LoadTims(vd);
	PlaceViewer();
	CheckTims();
	// Short text only: with the original firmware, printf blocks (and the PS1 freezes) when more than the 256 bytes
	// USB FIFO is printed while no "ps1transfer -c" reads it. The details are on the screen.
	for (i = j = 0; i < n_tims; i++)
		j += (tims[i].warn[0] != 0);
	printf("TIM viewer: %d TIM, %d with warnings%s\n", n_tims, j, viewer_overlaps ? ", viewer overwrites TIM data" : "");

	for (i = 0; i < 2; i++)
	{
		// Drawing in one buffer while the other is displayed
		SetDefDispEnv(&disp[i], screen_rect[i ^ 1].x, screen_rect[i ^ 1].y, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&draw[i], screen_rect[i].x, screen_rect[i].y, SCREEN_W, SCREEN_H);
	}
	FntLoad(font_rect.x, font_rect.y);
	UiInit(draw);
	fnt_map_info = UiOpenText(8, MAP_INFO_Y, 304, 4);
	fnt_info = UiOpenText(8, TIM_INFO_Y, 304, 5);
	SetDispMask(1);

	while (1)
	{
		pad = PadRead(1);
#define	PRESSED(_b_)	((pad & (_b_)) && !(opad & (_b_)))
		speed = (pad & PADR2) ? 8 : 2;

		if (edit)
		{
			// Palette editor: the entry (5-bit components, bit 15 = semi-transparency kept) is edited in the TIM file
			TimInfo	*t = &tims[sel];
			uint16_t	*c = t->clut_data + clut_row * t->clut.w;
			int		v, shift = comp * 5;

			if (PRESSED(PADLleft) && (entry > 0))					entry--;
			if (PRESSED(PADLright) && (entry < t->clut.w - 1))		entry++;
			if (PRESSED(PADLup) && (entry >= 16))					entry -= 16;
			if (PRESSED(PADLdown) && (entry + 16 < t->clut.w))		entry += 16;
			if (PRESSED(PADL2))		comp = (comp + 2) % 3;
			if (PRESSED(PADR2))		comp = (comp + 1) % 3;
			// Triangle/Cross: once when pressed, then repeated after 15 frames while held
			held = (pad & (PADRup | PADRdown)) ? held + 1 : 0;
			if ((held == 1) || ((held > 15) && !(held & 1)))
			{
				v = ((c[entry] >> shift) & 31) + ((pad & PADRup) ? 1 : -1);
				if ((v >= 0) && (v <= 31))
				{
					c[entry] = (c[entry] & ~(31 << shift)) | (v << shift);
					UploadClutRow(t, clut_row);
				}
			}
			if (PRESSED(PADRleft) && t->clut_orig)
			{
				memcpy(c, t->clut_orig + clut_row * t->clut.w, t->clut.w * 2);
				UploadClutRow(t, clut_row);
				strcpy(status, "ROW RESTORED");
			}
			if (PRESSED(PADstart))
				SaveTim(t);
			if (PRESSED(PADselect))
			{
				ReloadTim(t);
				if (clut_row >= t->clut.h)
					clut_row = 0;
				if (!t->clut.w)		// Reloaded without CLUT
					edit = 0;
				else if (entry >= t->clut.w)
					entry = 0;
			}
			if (PRESSED(PADRright))
				edit = 0;
		}
		else
		{
			if (PRESSED(PADselect))
			{
				view = (view + 1) % VIEW_COUNT;
				scroll_x = scroll_y = 0;
			}
			if (PRESSED(PADR1) && n_tims)	{sel = (sel + 1) % n_tims; clut_row = 0; swap = -1; scroll_x = scroll_y = 0;}
			if (PRESSED(PADL1) && n_tims)	{sel = (sel + n_tims - 1) % n_tims; clut_row = 0; swap = -1; scroll_x = scroll_y = 0;}
			if (pad & PADLleft)		scroll_x -= speed;
			if (pad & PADLright)	scroll_x += speed;
			if (pad & PADLup)		scroll_y -= speed;
			if (pad & PADLdown)		scroll_y += speed;
			if (PRESSED(PADRup) && (zoom < 8))		zoom *= 2;
			if (PRESSED(PADRdown) && (zoom > 1))	zoom /= 2;
			if ((view == VIEW_TIM) && n_tims)
			{
				TimInfo	*ct = &tims[(swap < 0) ? sel : swap];

				if (PRESSED(PADR2) && (clut_row < ct->clut.h - 1))	clut_row++;
				if (PRESSED(PADL2) && (clut_row > 0))				clut_row--;
				if (PRESSED(PADRleft) && tims[sel].clut.w)
				{
					swap = NextSwap(sel, swap);
					clut_row = 0;
					strcpy(status, (swap < 0) ? "OWN CLUT" : "CLUT OF ANOTHER TIM");
				}
				if (PRESSED(PADRright) && tims[sel].clut.w)
				{
					edit = 1;
					swap = -1;
					if (clut_row >= tims[sel].clut.h)
						clut_row = 0;
					entry = 0;
					status[0] = 0;
				}
			}
			if (PRESSED(PADstart))
			{
				scroll_x = scroll_y = 0;
				zoom = 1;
			}
		}
		opad = pad;

		side ^= 1;
		ClearOTagR(ot[side], OT_LEN);
		prim_ptr = prims[side];
		prim_end = prims[side] + PRIM_SIZE;

		if (view == VIEW_MAP)
		{
			DrawVram(0, 0, VRAM_W, VRAM_H, 0, MAP_Y, MAP_NUM, MAP_DEN, 2, 0, &ot[side][OT_LEN - 1]);
			DrawAllOutlines(0, MAP_Y, 0, 0, MAP_NUM, MAP_DEN, sel, &ot[side][1]);
			for (i = 0; i < 3; i++)	// Viewer screen buffers and font in grey
				DrawOutline((i < 2) ? &screen_rect[i] : &font_rect, 0, MAP_Y, 0, 0, MAP_NUM, MAP_DEN, grey, &ot[side][2]);
			UiTitle("PS1 DEV CART TIM VIEWER", "VRAM MAP");
			UiTextPanel(8, MAP_INFO_Y, 304, 4);
			if (n_tims)
				PrintTimInfo(sel, fnt_map_info);
			else
				FntPrint(fnt_map_info, "NO TIM FILE.\nUSE: PS1TRANSFER -TIM FILE.TIM");
			UiHelp(viewer_overlaps ? "SELECT:VIEW  L1/R1:TIM\nWARNING: VIEWER OVERWRITES TIM DATA" :
				"SELECT:VIEW  L1/R1:TIM");
		}
		else if (view == VIEW_TIM && n_tims)
		{
			TimInfo	*t = &tims[sel], *ct = &tims[(swap < 0) ? sel : swap];	// ct: TIM of the CLUT used
			int		mode = (t->mode > 2) ? 2 : t->mode, tpu = 4 >> mode;	// 24-bit: shown as raw 16-bit
			uint16_t	clut = ct->clut.w ? getClut(ct->clut.x, ct->clut.y + clut_row) : 0;

			// Image at zoom: VRAM pixels * texels per VRAM pixel * zoom
			DrawVram(t->img.x, t->img.y, t->img.w, t->img.h, 16 - scroll_x, TIM_Y - scroll_y, tpu * zoom, 1, mode, clut, &ot[side][OT_LEN - 1]);
			// Palette swatches (16 per line)
			if (ct->clut.w)
			{
				uint16_t	*c = ct->clut_data + clut_row * ct->clut.w;
				int		size = (ct->clut.w > 16) ? (edit ? 6 : 4) : 8;	// Bigger swatches to edit 256 colors
				int		x0 = SCREEN_W - 16 * size - 6, y0 = TIM_Y;

				DrawTile(x0 - 2, y0 - 2, 16 * size + 4, ((ct->clut.w + 15) / 16) * size + 4, 80, 80, 80, &ot[side][1]);
				for (i = 0; i < ct->clut.w; i++)
					DrawTile(x0 + (i & 15) * size, y0 + (i / 16) * size, size, size,
						(c[i] & 31) << 3, ((c[i] >> 5) & 31) << 3, ((c[i] >> 10) & 31) << 3, &ot[side][0]);
				if (edit)
					DrawCursor(x0 + (entry & 15) * size, y0 + (entry / 16) * size, size, &ot[side][0]);
			}
			if (edit)
			{
				uint16_t	v = t->clut_data[clut_row * t->clut.w + entry];

				UiTitle("PS1 DEV CART TIM VIEWER", "PALETTE EDITOR");
				UiTextPanel(8, TIM_INFO_Y, 304, 5);
				FntPrint(fnt_info, "%s  ROW %d/%d  ENTRY %d\n", t->name, clut_row + 1, t->clut.h, entry);
				FntPrint(fnt_info, "R %d  G %d  B %d%s  [%s]\n", v & 31, (v >> 5) & 31, (v >> 10) & 31,
					(v & 0x8000) ? "  STP" : (v ? "" : "  TRANSPARENT"), comp_names[comp]);
				UiHelp("D-PAD:ENTRY L2/R2:COLOR TRI/X:+/-\nSQR:RESTORE START:SAVE SEL:LOAD O:EXIT");
			}
			else
			{
				char	right[16];

				sprintf(right, "TIM %d/%d", sel + 1, n_tims);
				UiTitle("PS1 DEV CART TIM VIEWER", right);
				UiTextPanel(8, TIM_INFO_Y, 304, 5);
				FntPrint(fnt_info, "ZOOM %d  CLUT ROW %d/%d", zoom, clut_row + 1, ct->clut.h ? ct->clut.h : 1);
				if (swap >= 0)
					FntPrint(fnt_info, "  CLUT OF TIM %d", swap + 1);
				FntPrint(fnt_info, "\n");
				PrintTimInfo(sel, fnt_info);
				UiHelp("SELECT:VIEW L1/R1:TIM D-PAD:SCROLL\nTRI/X:ZOOM L2/R2:CLUT SQR:SWAP O:EDIT");
			}
			if (status[0])
				FntPrint(fnt_info, "%s\n", status);
		}
		else
		{
			// Raw VRAM 1:1 (16-bit view), outlines on top
			if (scroll_x < 0)	scroll_x = 0;
			if (scroll_y < 0)	scroll_y = 0;
			if (scroll_x > VRAM_W - SCREEN_W)	scroll_x = VRAM_W - SCREEN_W;
			if (scroll_y > VRAM_H - (SCREEN_H - 16))	scroll_y = VRAM_H - (SCREEN_H - 16);
			DrawVram(scroll_x, scroll_y, SCREEN_W, SCREEN_H - 16, 0, 16, 1, 1, 2, 0, &ot[side][OT_LEN - 1]);
			DrawAllOutlines(0, 16, scroll_x, scroll_y, 1, 1, sel, &ot[side][1]);
			{
				char	right[24];

				sprintf(right, "VRAM %d,%d", scroll_x, scroll_y);
				UiTitle("PS1 DEV CART TIM VIEWER", right);
			}
			UiHelp("SELECT:VIEW  D-PAD:SCROLL  R2:FAST");
		}

		DrawSync(0);
		VSync(0);
		PutDispEnv(&disp[side]);
		PutDrawEnv(&draw[side]);
		DrawOTag(&ot[side][OT_LEN - 1]);
		UiFlush();
		FntFlush(fnt_map_info);
		FntFlush(fnt_info);
	}

	return (0);
}
