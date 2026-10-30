/*
* PS1 REWIND: PEG DROP, a demo of the rewind library (rewind.h). Drop balls from the top: they bounce off the pegs and
* off each other into the bins at the bottom, which score. Hold L1 to rewind, R1 to rewind faster: every ball flies
* back up the way it came. Let go and the game goes on from there.
*
* Everything that makes up the game is in one block (Game, about 700 bytes), given to the rewind library; what's
* drawn is worked out from it.
*
* The computer runs rewind_server.py and "ps1transfer -r -e -f pegdrop.ps-exe -net 127.0.0.1:7040" (run.bat).
* Without it, the rewind goes back 10 seconds (the ring in RAM).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>
#include <psxetc.h>
#include <psxapi.h>
#include "psyq_compat.h"
#include "rewind.h"

#define	SCREEN_W		320
#define	SCREEN_H		240
#define	BAR_Y			224

/****************************************/
// Drawing: one ordering table, drawn by layers

enum {L_BG, L_FIELD, L_BALL, L_TINT, L_TEXT, LAYERS};

#define	PRIM_BYTES		32768

static DISPENV		disp[2];
static DRAWENV		draw[2];
static uint32_t		ot[2][LAYERS];
static uint8_t		prims[2][PRIM_BYTES];
static uint8_t		*prim_ptr;
static int			side;

#define	OT(_layer_)		(&ot[side][LAYERS - 1 - (_layer_)])

static void	*Alloc(int n)
{
	void	*p;

	if (prim_ptr + n > prims[side] + PRIM_BYTES - 64)
		return (NULL);
	p = prim_ptr;
	prim_ptr += n;
	return (p);
}

static void	Rect(int layer, int x, int y, int w, int h, int r, int g, int b)
{
	TILE	*t = Alloc(sizeof(TILE));

	if (!t)
		return;
	setTile(t);
	setXY0(t, x, y);
	setWH(t, w, h);
	setRGB0(t, r, g, b);
	addPrim(OT(layer), t);
}

// Text with the debug font (upper case), tinted: the sprites of FntSort with their colour, modulated
static void	Text(int layer, int x, int y, const char *s, int r, int g, int b)
{
	uint8_t	*start = prim_ptr, *end, *p;

	if (!Alloc(16 * (strlen(s) + 2)))
		return;
	end = FntSort(OT(layer), start, x, y, s);
	for (p = start; p < end; p += ((p[3] + 1) * 4))
		if ((p[7] & 0xE0) == 0x60)
		{
			p[4] = r;
			p[5] = g;
			p[6] = b;
			p[7] &= ~1;
		}
	prim_ptr = end;
}

static uint32_t	Isqrt(uint32_t v)
{
	uint32_t	r = 0, b = 1 << 30;

	while (b > v)
		b >>= 2;
	while (b)
	{
		if (v >= r + b)
		{
			v -= r + b;
			r = (r >> 1) + b;
		}
		else
			r >>= 1;
		b >>= 2;
	}
	return (r);
}

static void	Disc(int layer, int cx, int cy, int r, int cr, int cg, int cb)
{
	int	dy;

	for (dy = -r; dy <= r; dy++)
	{
		int	w = Isqrt(r * r - dy * dy);

		Rect(layer, cx - w, cy + dy, w * 2 + 1, 1, cr, cg, cb);
	}
}

/****************************************/
// The game: positions in pixels * 16, speeds in pixels a frame * 16

#define	BALLS			64
#define	BALL_R			4
#define	PEG_R			3
#define	LEFT			16
#define	RIGHT			304
#define	BIN_W			32
#define	BINS			9
#define	BIN_TOP			198			// The bins' walls
#define	LAND_Y			218			// A ball this low is in its bin
#define	DROP_Y			20
#define	GRAVITY			3
#define	MAX_V			96

typedef struct
{
	int16_t	x, y, vx, vy;
	uint8_t	alive, colour;
} Ball;

typedef struct					// Everything that makes up the game (what the rewind keeps)
{
	uint32_t	rng;
	int32_t		score;
	int16_t		dropper, cooldown;
	uint16_t	dropped, bins[BINS];
	uint8_t		flash[BINS];
	Ball		balls[BALLS];
} Game;

static Game				game;
int						auto_play, auto_rewind;		// For tests (ps1transfer -poke): plays by itself; rewinds at
													// that speed. auto_play 2: play 30 s, rewind x4 6 s, play 10 s,
													// rewind x1 15 s, play (test_waits: frames the rewind waited)
int						test_waits;
static const int16_t	bin_points[BINS] = {10, 20, 50, 100, 500, 100, 50, 20, 10};
static const uint8_t	colours[8][3] = {{255, 90, 70}, {255, 190, 60}, {120, 230, 90}, {70, 200, 255},
										 {160, 120, 255}, {255, 110, 200}, {240, 240, 240}, {90, 240, 200}};
static int16_t			peg_x[80], peg_y[80];
static int				pegs;

static uint32_t	Random(void)
{
	game.rng ^= game.rng << 13;
	game.rng ^= game.rng >> 17;
	game.rng ^= game.rng << 5;
	return (game.rng);
}

static void	GameReset(void)
{
	memset(&game, 0, sizeof(game));
	game.rng = 0x2545F491;
	game.dropper = 160 * 16;
}

// The pegs: rows 22 pixels apart, every other row shifted half a gap (not part of the game's block: they don't move)
static void	Pegs(void)
{
	int	r, c;

	for (r = 0; r < 7; r++)
		for (c = 0; c < ((r & 1) ? 8 : 9); c++)
		{
			peg_x[pegs] = ((r & 1) ? 48 : 32) + c * 32;
			peg_y[pegs] = 52 + r * 22;
			pegs++;
		}
}

static void	Drop(int x)
{
	int	i;

	for (i = 0; i < BALLS; i++)
		if (!game.balls[i].alive)
		{
			Ball	*b = &game.balls[i];

			b->alive = 1;
			b->x = x + (int)(Random() % 33) - 16;
			b->y = DROP_Y * 16;
			b->vx = (int)(Random() % 17) - 8;
			b->vy = 0;
			b->colour = Random() & 7;
			game.dropped++;
			return;
		}
}

// Off a circle (a peg, radius r pixels, at cx, cy): pushed out, bounced (restitution 0.6), a little random spin
static void	Bounce(Ball *b, int cx, int cy, int r)
{
	int	dx = b->x - cx, dy = b->y - cy, rr = (r + BALL_R) * 16, d2 = dx * dx + dy * dy, d, nx, ny, vn;

	if ((d2 >= rr * rr) || !d2)
		return;
	d = Isqrt(d2);
	nx = dx * 256 / d;
	ny = dy * 256 / d;
	b->x = cx + nx * rr / 256;
	b->y = cy + ny * rr / 256;
	vn = (b->vx * nx + b->vy * ny) / 256;
	if (vn < 0)
	{
		b->vx -= vn * nx * 160 / 65536;		// (1 + 0.6)
		b->vy -= vn * ny * 160 / 65536;
		b->vx += (int)(Random() % 5) - 2;
	}
}

static void	GameStep(uint32_t pad, uint32_t pressed)
{
	int	i, j;

	// The dropper: D-pad, Cross drops (held: a stream), Square a row of eight
	if (pad & PADLleft)		game.dropper -= 40;
	if (pad & PADLright)	game.dropper += 40;
	if (game.dropper < (LEFT + 12) * 16)	game.dropper = (LEFT + 12) * 16;
	if (game.dropper > (RIGHT - 12) * 16)	game.dropper = (RIGHT - 12) * 16;
	if (game.cooldown)
		game.cooldown--;
	if ((pad & PADRdown) && !game.cooldown)
	{
		Drop(game.dropper);
		game.cooldown = 5;
	}
	if (pressed & PADRleft)
		for (i = 0; i < 8; i++)
			Drop((LEFT + 18 + i * 38) * 16);
	if (pressed & PADRup)
		GameReset();

	for (i = 0; i < BALLS; i++)
	{
		Ball	*b = &game.balls[i];
		int		k, row;

		if (!b->alive)
			continue;
		b->vy += GRAVITY;
		if (b->vx > MAX_V)	b->vx = MAX_V;
		if (b->vx < -MAX_V)	b->vx = -MAX_V;
		if (b->vy > MAX_V)	b->vy = MAX_V;
		b->x += b->vx;
		b->y += b->vy;
		// The pegs of the rows near it, the walls, the bins' walls
		row = ((b->y >> 4) - 52 + 11) / 22;
		for (k = 0; k < pegs; k++)
			if ((peg_y[k] - 52) / 22 >= row - 1 && (peg_y[k] - 52) / 22 <= row + 1)
				Bounce(b, peg_x[k] * 16, peg_y[k] * 16, PEG_R);
		if (b->x < (LEFT + BALL_R) * 16)
		{
			b->x = (LEFT + BALL_R) * 16;
			b->vx = -b->vx / 2;
		}
		if (b->x > (RIGHT - BALL_R) * 16)
		{
			b->x = (RIGHT - BALL_R) * 16;
			b->vx = -b->vx / 2;
		}
		if (b->y > BIN_TOP * 16)
			for (k = 1; k < BINS; k++)
			{
				int	wx = (LEFT + k * BIN_W) * 16;

				if ((b->x > wx - (BALL_R + 1) * 16) && (b->x < wx + (BALL_R + 1) * 16))
				{
					b->x = (b->x < wx) ? wx - (BALL_R + 1) * 16 : wx + (BALL_R + 1) * 16;
					b->vx = -b->vx / 2;
				}
			}
		if (b->y >= LAND_Y * 16)		// In a bin: it scores
		{
			k = ((b->x >> 4) - LEFT) / BIN_W;
			if (k < 0)		k = 0;
			if (k >= BINS)	k = BINS - 1;
			game.bins[k]++;
			game.score += bin_points[k];
			game.flash[k] = 20;
			b->alive = 0;
		}
	}
	// The balls off each other: pushed apart, their speeds along the line between them swapped (restitution 0.8)
	for (i = 0; i < BALLS; i++)
		for (j = i + 1; game.balls[i].alive && (j < BALLS); j++)
		{
			Ball	*a = &game.balls[i], *b = &game.balls[j];
			int		dx = b->x - a->x, dy = b->y - a->y, rr = BALL_R * 2 * 16, d2, d, nx, ny, push, vn;

			if (!b->alive || (dx >= rr) || (dx <= -rr) || (dy >= rr) || (dy <= -rr))
				continue;
			d2 = dx * dx + dy * dy;
			if ((d2 >= rr * rr) || !d2)
				continue;
			d = Isqrt(d2);
			nx = dx * 256 / d;
			ny = dy * 256 / d;
			push = (rr - d) / 2 + 1;
			a->x -= nx * push / 256;
			a->y -= ny * push / 256;
			b->x += nx * push / 256;
			b->y += ny * push / 256;
			vn = ((b->vx - a->vx) * nx + (b->vy - a->vy) * ny) / 256;
			if (vn < 0)
			{
				int	imp = vn * 230 / 256;		// ((1 + 0.8) / 2)

				a->vx += imp * nx / 256;
				a->vy += imp * ny / 256;
				b->vx -= imp * nx / 256;
				b->vy -= imp * ny / 256;
			}
		}
	for (i = 0; i < BINS; i++)
		if (game.flash[i])
			game.flash[i]--;
}

/****************************************/

static void	DrawGame(int rewinding)
{
	int		i;
	char	s[48];

	// Walls, the pegs, the bins (their walls, values, lit when a ball lands), the dropper, the balls
	Rect(L_FIELD, LEFT - 3, 0, 3, BAR_Y, 70, 80, 110);
	Rect(L_FIELD, RIGHT, 0, 3, BAR_Y, 70, 80, 110);
	for (i = 0; i < pegs; i++)
		Disc(L_FIELD, peg_x[i], peg_y[i], PEG_R, 150, 160, 190);
	for (i = 0; i < BINS; i++)
	{
		int	x = LEFT + i * BIN_W, f = game.flash[i] * 6;

		if (i)
			Rect(L_FIELD, x - 1, BIN_TOP, 2, BAR_Y - BIN_TOP, 70, 80, 110);
		Rect(L_BG, x + 1, BIN_TOP + 4, BIN_W - 2, BAR_Y - BIN_TOP - 4, 20 + f, 24 + f, 44 + f / 2);
		sprintf(s, "%d", bin_points[i]);
		Text(L_TEXT, x + (BIN_W - strlen(s) * 8) / 2, BIN_TOP + 10, s, 100, 100, 110);
	}
	{
		int	x = game.dropper >> 4;

		Rect(L_FIELD, x - 10, 8, 21, 3, 230, 200, 80);
		Rect(L_FIELD, x - 2, 11, 5, 4, 230, 200, 80);
	}
	for (i = 0; i < BALLS; i++)
	{
		Ball	*b = &game.balls[i];

		if (b->alive)
			Disc(L_BALL, b->x >> 4, b->y >> 4, BALL_R, colours[b->colour][0], colours[b->colour][1],
				 colours[b->colour][2]);
	}
	sprintf(s, "SCORE %d", (int)game.score);
	Text(L_TEXT, LEFT + 4, 24, s, 128, 128, 128);
	sprintf(s, "BALLS %d", game.dropped);
	Text(L_TEXT, RIGHT - 4 - strlen(s) * 8, 24, s, 128, 128, 128);

	// While rewinding: the screen tinted blue
	if (rewinding)
	{
		TILE	*t = Alloc(sizeof(TILE));
		DR_TPAGE	*tp = Alloc(sizeof(DR_TPAGE));

		if (t && tp)
		{
			setTile(t);
			setSemiTrans(t, 1);
			setXY0(t, 0, 0);
			setWH(t, SCREEN_W, BAR_Y);
			setRGB0(t, 0, 20, 70);
			addPrim(OT(L_TINT), t);
			setDrawTPage(tp, 0, 1, getTPage(0, 1, 0, 0));		// (semi-transparency: background + foreground)
			addPrim(OT(L_TINT), tp);
		}
	}
}

static void	DrawBar(int speed)
{
	RewindInfo	info;
	char		s[64];

	Rewind_GetInfo(&info);
	Rect(L_TINT, 0, BAR_Y, SCREEN_W, SCREEN_H - BAR_Y, 40, 50, 120);
	if (info.rewinding)
		sprintf(s, "%s %s %d.%dS BACK", info.waiting ? "WAIT" : "<<", (speed > 1) ? "X4" : "X1",
			info.frames_back / 50, (info.frames_back % 50) / 5);
	else if (info.connected)
		sprintf(s, "PC: %d.%dS BACK  RAM %d.%dS  UP %dKB/S", info.frames_back / 50, (info.frames_back % 50) / 5,
			info.ring_frames / 50, (info.ring_frames % 50) / 5, (info.up_bytes + 512) / 1024);
	else
		sprintf(s, "NO PC: %d.%dS BACK (RAM)  L1/R1 REWIND", info.frames_back / 50, (info.frames_back % 50) / 5);
	Text(L_TEXT, 6, BAR_Y + 4, s, 128, 128, 128);
}

int	main(void)
{
	int			i, is_pal = (*((char *)0xBFC7FF52) == 'E');
	uint32_t	pad = 0, opad;

	ResetGraph(0);	// (first: PSn00bSDK's ResetGraph sets up the callbacks, see psyq_compat.h)
	SetVideoMode(is_pal ? MODE_PAL : MODE_NTSC);
	PadInit(0);
	for (i = 0; i < 2; i++)
	{
		SetDefDispEnv(&disp[i], 0, i ? 0 : SCREEN_H, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&draw[i], 0, i ? SCREEN_H : 0, SCREEN_W, SCREEN_H);
		draw[i].isbg = 1;
		setRGB0(&draw[i], 10, 12, 22);
	}
	FntLoad(960, 256);
	SetDispMask(1);
	Pegs();
	GameReset();
	Rewind_Init(&game, sizeof(game));

	while (1)
	{
		int	speed;

		opad = pad;
		pad = PadRead(1) & 0xFFFF;
		prim_ptr = prims[side];
		ClearOTagR(ot[side], LAYERS);

		// L1: rewind, R1: rewind 4 times as fast; else the game goes on
		speed = (pad & PADR1) ? 4 : (pad & PADL1) ? 1 : auto_rewind;
		if (auto_play == 2)
		{
			static int	script;

			script++;
			if ((script > 1500) && (script <= 1800))
				speed = 4;
			else if ((script > 2300) && (script <= 3050))
				speed = 1;
		}
		if (auto_play && !speed)		// (the dropper sweeps, a ball every 5 frames, a row of eight now and then)
		{
			static int	t;

			t++;
			pad = (((t / 100) & 1) ? PADLleft : PADLright) | PADRdown | (((t % 400) == 0) ? PADRleft : 0);
		}
		if (!Rewind_Frame(speed))
		{
			GameStep(pad, pad & ~opad);
			Rewind_Save();
		}
		{
			RewindInfo	info;

			Rewind_GetInfo(&info);
			test_waits += info.waiting;
		}
		DrawGame(speed > 0);
		DrawBar(speed);

		DrawSync(0);
		VSync(0);
		PutDispEnv(&disp[side]);
		PutDrawEnv(&draw[side]);
		DrawOTag(&ot[side][LAYERS - 1]);
		side ^= 1;
	}
	return (0);
}
