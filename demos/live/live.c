/*
* PS1 LIVE: a small platform game whose level is edited on the computer while it runs on the PlayStation
* (live_editor.py): tiles, enemies, the start, colours, physics and the tile set's graphics change on the TV at once.
*
* The whole level is one block in RAM at a fixed address (live.h), read by the game every frame; the editor writes it
* through the PS1 DEV CART ("ps1transfer -memlive": the memory of the running program, read and written while it runs)
* or PCSX-Redux's GDB server. The game writes where the player and the enemies are for the editor's map.
*
* Controls: Left/Right run, Cross jump, Start back to the start. Coins, spikes, springs, enemies, the goal flag.
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
#include "live.h"

#define	SCREEN_W		320
#define	SCREEN_H		240
#define	VIEW_H			(LV_H * LV_TILE)	// 224: the level; the bar below
#define	WORLD_W			(LV_W * LV_TILE)
#define	SHEET_X			640					// The tile set in VRAM
#define	SHEET_Y			0
#define	PLAYER_W		12
#define	PLAYER_H		14

static LvLevel *const	level = (LvLevel *)LV_LEVEL_ADRS;
static LvState *const	state = (LvState *)LV_STATE_ADRS;

/****************************************/
// Drawing: one ordering table, drawn by layers

enum {L_SKY, L_TILES, L_ACTORS, L_BAR, L_TEXT, LAYERS};

#define	PRIM_BYTES		32768

static DISPENV		disp[2];
static DRAWENV		draw[2];
static uint32_t		ot[2][LAYERS];
static uint8_t		prims[2][PRIM_BYTES];
static uint8_t		*prim_ptr;
static int			side, is_pal;

#define	OT(_layer_)		(&ot[side][LAYERS - 1 - (_layer_)])

static void	*Alloc(int size)
{
	void	*p;

	if (prim_ptr + size > prims[side] + PRIM_BYTES - 64)
		return (NULL);
	p = prim_ptr;
	prim_ptr += size;
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

// A 16 x 16 entry of the tile set, tinted (128: as drawn)
static void	Sprite(int layer, int x, int y, int entry, const uint8_t *tint)
{
	SPRT_16	*s = Alloc(sizeof(SPRT_16));

	if (!s)
		return;
	setSprt16(s);
	setXY0(s, x, y);
	setUV0(s, entry * 16, 0);
	setRGB0(s, tint[0], tint[1], tint[2]);
	s->clut = 0;
	addPrim(OT(layer), s);
}

static void	TexturePage(int layer)	// (added last in a layer: drawn before its sprites)
{
	DR_TPAGE	*t = Alloc(sizeof(DR_TPAGE));

	if (!t)
		return;
	setDrawTPage(t, 0, 0, getTPage(2, 0, SHEET_X, SHEET_Y));
	addPrim(OT(layer), t);
}

/****************************************/
// The default level and tile set (the editor reads them back, then changes them)

static uint16_t	Rgb(int r, int g, int b)
{
	return (0x8000 | (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

static void	SheetPixel(int entry, int x, int y, int r, int g, int b)
{
	level->sheet[y][entry * 16 + x] = Rgb(r, g, b);
}

static void	DefaultSheet(void)
{
	int	x, y;

	memset(level->sheet, 0, sizeof(level->sheet));
	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++)
		{
			int	edge = (x == 0) || (y == 0) || (x == 15) || (y == 15);

			// Ground: grass on top of earth
			if (y < 4)
				SheetPixel(T_GROUND, x, y, 60, 190 - ((x * 7 + y * 3) & 31), 60);
			else
				SheetPixel(T_GROUND, x, y, 150 - ((x * 5 + y * 11) & 31), 100, 60);
			// Brick
			SheetPixel(T_BRICK, x, y, ((y & 7) == 7) || ((x + ((y >> 3) & 1) * 8) % 16 == 15) ? 110 : 190, 80, 60);
			// Block: a bevelled square
			if (edge)
				SheetPixel(T_BLOCK, x, y, 240, 200, 90);
			else
				SheetPixel(T_BLOCK, x, y, 210 - (x + y) * 3, 150, 50);
			// Spikes: triangles at the bottom
			if ((y >= 6) && (((x & 7) >= (15 - y) / 2) && ((x & 7) <= 7 - (15 - y) / 2)))
				SheetPixel(T_SPIKES, x, y, 200 + y * 3, 200 + y * 3, 220);
			// Coin: a disc
			if ((x - 7) * (x - 7) * 2 + (y - 7) * (y - 7) < 30)
				SheetPixel(T_COIN, x, y, 250, 210 - ((x + y) & 7) * 8, 40);
			// Goal: a pole and a flag
			if (x == 3)
				SheetPixel(T_GOAL, x, y, 220, 220, 220);
			else if ((x > 3) && (x < 14) && (y > 1) && (y < 8))
				SheetPixel(T_GOAL, x, y, 60, 230, 90);
			// Spring: a coil
			if ((y > 6) && ((y & 1) || (y == 15)) && (x > 1) && (x < 14))
				SheetPixel(T_SPRING, x, y, 230, 80, 80);
			// Player: a body, two eyes
			if ((x >= 2) && (x < 14) && (y >= 2))
				SheetPixel(LV_SPRITE_PLAYER, x, y, 70, 140, 255);
			if (((x == 6) || (x == 10)) && (y >= 5) && (y <= 7))
				SheetPixel(LV_SPRITE_PLAYER, x, y, 255, 255, 255);
			// Enemy: a round body, angry eyes
			if ((x - 7) * (x - 7) + (y - 9) * (y - 9) < 44)
				SheetPixel(LV_SPRITE_ENEMY, x, y, 220, 60, 200);
			if (((x == 5) || (x == 10)) && ((y == 7) || (y == 8)))
				SheetPixel(LV_SPRITE_ENEMY, x, y, 255, 255, 120);
		}
}

static void	Fill(int x0, int y0, int x1, int y1, int t)
{
	int	x, y;

	for (y = y0; y <= y1; y++)
		for (x = x0; x <= x1; x++)
			level->tiles[y][x] = t;
}

static void	SetColour(uint8_t *c, int r, int g, int b)
{
	c[0] = r;
	c[1] = g;
	c[2] = b;
	c[3] = 0;
}

static void	AddEnemy(int x, int y, int range, int speed)
{
	LvEnemy	*e = &level->enemies[level->enemy_count++];

	e->x = x * LV_TILE;
	e->y = y * LV_TILE;
	e->range = range * LV_TILE;
	e->speed = speed;
	e->r = e->g = e->b = 128;
}

static void	DefaultLevel(void)
{
	int	i;

	memset(level, 0, sizeof(LvLevel) - sizeof(level->sheet));
	Fill(0, 12, 63, 13, T_GROUND);
	Fill(14, 12, 16, 13, T_EMPTY);			// A gap
	Fill(15, 13, 15, 13, T_SPIKES);
	Fill(8, 8, 11, 8, T_BRICK);
	Fill(9, 7, 10, 7, T_COIN);
	Fill(20, 9, 23, 9, T_BLOCK);
	Fill(21, 8, 22, 8, T_COIN);
	Fill(27, 11, 27, 11, T_SPRING);
	Fill(29, 5, 34, 5, T_BRICK);
	Fill(30, 4, 33, 4, T_COIN);
	Fill(38, 10, 39, 11, T_BLOCK);
	Fill(42, 11, 45, 11, T_SPIKES);
	Fill(41, 7, 46, 7, T_BRICK);
	Fill(50, 9, 53, 9, T_BLOCK);
	Fill(55, 11, 55, 11, T_SPRING);
	Fill(58, 6, 60, 6, T_BRICK);
	Fill(61, 11, 61, 11, T_GOAL);
	level->start_x = 2 * LV_TILE;
	level->start_y = 10 * LV_TILE;
	AddEnemy(18, 11, 6, 192);
	AddEnemy(30, 4, 3, 128);
	AddEnemy(47, 11, 6, 256);
	AddEnemy(52, 8, 1, 96);
	SetColour(level->sky_top, 40, 70, 160);
	SetColour(level->sky_bottom, 160, 200, 240);
	SetColour(level->player_tint, 128, 128, 128);
	for (i = 0; i < LV_TILE_TYPES; i++)
		SetColour(level->tint[i], 128, 128, 128);
	level->gravity = 90;				// 0.35 pixel a frame per frame
	level->jump = 1600;				// 6.25 pixels a frame
	level->run = 512;				// 2 pixels a frame
	level->max_fall = 1800;
	level->version = 1;
	level->tiles_version = 1;
	level->magic = LV_MAGIC;
}

/****************************************/
// Game

static int	px, py, vx, vy, on_ground, coins, deaths, wins, banner_until, frame, cam_x;
static int	enemy_x[LV_ENEMIES], enemy_dir[LV_ENEMIES];
static uint8_t	taken[LV_H][LV_W];			// Coins taken (the level itself isn't changed: it's the editor's)
static uint32_t	version_seen, tiles_seen;
static uint32_t	pad, opad;
#define	PRESSED(_b_)	((pad & (_b_)) && !(opad & (_b_)))

static int	TileAt(int x, int y)	// (pixels)
{
	if ((x < 0) || (x >= WORLD_W))
		return (T_BLOCK);			// (the sides: solid; above the top and below the bottom: open, falling out dies)
	if ((y < 0) || (y >= VIEW_H))
		return (T_EMPTY);
	return (level->tiles[y / LV_TILE][x / LV_TILE]);
}

static int	Solid(int t)
{
	return ((t == T_GROUND) || (t == T_BRICK) || (t == T_BLOCK) || (t == T_SPRING));
}

static int	SolidBox(int x, int y, int w, int h)
{
	return (Solid(TileAt(x, y)) || Solid(TileAt(x + w - 1, y)) || Solid(TileAt(x, y + h - 1)) ||
		Solid(TileAt(x + w - 1, y + h - 1)) || Solid(TileAt(x, y + h / 2)) || Solid(TileAt(x + w - 1, y + h / 2)));
}

static void	Respawn(void)
{
	px = level->start_x << 8;
	py = level->start_y << 8;
	vx = vy = 0;
}

static void	ResetEnemies(void)
{
	int	i;

	for (i = 0; i < LV_ENEMIES; i++)
	{
		enemy_x[i] = level->enemies[i].x << 8;
		enemy_dir[i] = 1;
	}
}

static void	Restart(void)
{
	memset(taken, 0, sizeof(taken));
	ResetEnemies();
	Respawn();
	coins = 0;
}

static void	Die(void)
{
	deaths++;
	Respawn();
}

static void	Update(void)
{
	int	x, y, i, tx, ty, steps;

	// The editor changed the level: the enemies from their new places; the player out of a new wall
	if (level->version != version_seen)
	{
		version_seen = level->version;
		ResetEnemies();
		for (steps = 0; (steps < 64) && SolidBox(px >> 8, py >> 8, PLAYER_W, PLAYER_H); steps++)
			py -= LV_TILE << 8 >> 2;
	}
	if (PRESSED(PADstart))
		Restart();

	// Running and jumping
	vx = 0;
	if (pad & PADLleft)		vx = -level->run;
	if (pad & PADLright)	vx = level->run;
	if (PRESSED(PADRdown) && on_ground)
		vy = -level->jump;
	vy += level->gravity;
	if (vy > level->max_fall)
		vy = level->max_fall;

	// Moved a pixel at a time, one axis after the other, stopped by solid tiles
	steps = (vx < 0) ? -vx : vx;
	for (i = 0; i < steps; i += 256)
	{
		int	nx = px + ((vx < 0) ? -1 : 1) * ((steps - i < 256) ? steps - i : 256);

		if (SolidBox(nx >> 8, py >> 8, PLAYER_W, PLAYER_H))
			break;
		px = nx;
	}
	on_ground = 0;
	steps = (vy < 0) ? -vy : vy;
	for (i = 0; i < steps; i += 256)
	{
		int	ny = py + ((vy < 0) ? -1 : 1) * ((steps - i < 256) ? steps - i : 256);

		if (SolidBox(px >> 8, ny >> 8, PLAYER_W, PLAYER_H))
		{
			if (vy > 0)
			{
				on_ground = 1;
				// A spring under the player: up again, higher
				if ((TileAt((px >> 8) + PLAYER_W / 2, (ny >> 8) + PLAYER_H) == T_SPRING))
				{
					vy = -level->jump * 3 / 2;
					on_ground = 0;
					break;
				}
			}
			vy = 0;
			break;
		}
		py = ny;
	}
	if ((py >> 8) > VIEW_H)
		Die();

	// Touching: spikes, coins, the goal
	x = px >> 8;
	y = py >> 8;
	for (ty = y / LV_TILE; ty <= (y + PLAYER_H - 1) / LV_TILE; ty++)
		for (tx = x / LV_TILE; tx <= (x + PLAYER_W - 1) / LV_TILE; tx++)
		{
			int	t;

			if ((tx < 0) || (tx >= LV_W) || (ty < 0) || (ty >= LV_H))
				continue;
			t = level->tiles[ty][tx];
			if ((t == T_SPIKES) && (y + PLAYER_H - 1 >= ty * LV_TILE + 6))
				Die();
			else if ((t == T_COIN) && !taken[ty][tx])
			{
				taken[ty][tx] = 1;
				coins++;
			}
			else if ((t == T_GOAL) && (frame > banner_until))
			{
				wins++;
				banner_until = frame + 100;
				Restart();
			}
		}

	// Enemies: back and forth over their range, turning at walls; touching one starts again
	for (i = 0; i < level->enemy_count && i < LV_ENEMIES; i++)
	{
		LvEnemy	*e = &level->enemies[i];
		int		nx = enemy_x[i] + enemy_dir[i] * e->speed, ex;

		if ((nx >> 8) > e->x + e->range || (nx >> 8) < e->x ||
			SolidBox(nx >> 8, e->y + 2, 16, 14) || ((nx >> 8) < 0) || ((nx >> 8) > WORLD_W - 16))
			enemy_dir[i] = -enemy_dir[i];
		else
			enemy_x[i] = nx;
		ex = enemy_x[i] >> 8;
		if ((x < ex + 14) && (x + PLAYER_W > ex + 2) && (y < e->y + 16) && (y + PLAYER_H > e->y + 3))
			Die();
	}

	// Camera, state for the editor
	cam_x = x + PLAYER_W / 2 - SCREEN_W / 2;
	if (cam_x < 0)						cam_x = 0;
	if (cam_x > WORLD_W - SCREEN_W)		cam_x = WORLD_W - SCREEN_W;
	state->frame = frame;
	state->player_x = x;
	state->player_y = y;
	state->coins = coins;
	state->deaths = deaths;
	state->wins = wins;
	for (i = 0; i < LV_ENEMIES; i++)
		state->enemy_x[i] = enemy_x[i] >> 8;
	state->version_seen = version_seen;
	state->magic = LV_MAGIC;
}

static void	Draw(void)
{
	POLY_G4	*sky = Alloc(sizeof(POLY_G4));
	int		x, y, i, x0 = cam_x / LV_TILE;
	char	s[48];

	if (sky)
	{
		setPolyG4(sky);
		setXY4(sky, 0, 0, SCREEN_W, 0, 0, VIEW_H, SCREEN_W, VIEW_H);
		setRGB0(sky, level->sky_top[0], level->sky_top[1], level->sky_top[2]);
		setRGB1(sky, level->sky_top[0], level->sky_top[1], level->sky_top[2]);
		setRGB2(sky, level->sky_bottom[0], level->sky_bottom[1], level->sky_bottom[2]);
		setRGB3(sky, level->sky_bottom[0], level->sky_bottom[1], level->sky_bottom[2]);
		addPrim(OT(L_SKY), sky);
	}
	for (y = 0; y < LV_H; y++)
		for (x = x0; (x <= x0 + SCREEN_W / LV_TILE) && (x < LV_W); x++)
		{
			int	t = level->tiles[y][x];

			if ((t == T_EMPTY) || (t >= LV_TILE_TYPES) || ((t == T_COIN) && taken[y][x]))
				continue;
			Sprite(L_TILES, x * LV_TILE - cam_x, y * LV_TILE, t, level->tint[t]);
		}
	TexturePage(L_TILES);
	for (i = 0; i < level->enemy_count && i < LV_ENEMIES; i++)
		Sprite(L_ACTORS, (enemy_x[i] >> 8) - cam_x, level->enemies[i].y, LV_SPRITE_ENEMY, &level->enemies[i].r);
	Sprite(L_ACTORS, (px >> 8) - 2 - cam_x, (py >> 8) - 2, LV_SPRITE_PLAYER, level->player_tint);
	TexturePage(L_ACTORS);

	Rect(L_BAR, 0, VIEW_H, SCREEN_W, SCREEN_H - VIEW_H, 40, 50, 120);
	sprintf(s, "COINS %d  DEATHS %d  GOALS %d", coins, deaths, wins);
	Text(L_TEXT, 6, VIEW_H + 4, s, 128, 128, 128);
	Text(L_TEXT, SCREEN_W - 6 - 9 * 8, VIEW_H + 4, "PS1 LIVE", 128, 110, 30);
	if (frame < banner_until)
	{
		Rect(L_BAR, 80, 90, 160, 30, 40, 50, 120);
		Text(L_TEXT, 104, 101, "GOAL! AGAIN...", 128, 110, 30);
	}
}

// The tile set to VRAM when the editor has sent a new one (hot reload)
static void	SheetUpload(void)
{
	RECT	r = {SHEET_X, SHEET_Y, LV_SHEET_W, LV_SHEET_H};

	if (level->tiles_version == tiles_seen)
		return;
	tiles_seen = level->tiles_version;
	DrawSync(0);
	LoadImage(&r, (const uint32_t *)level->sheet);
	DrawSync(0);
}

int	main(void)
{
	int	i;

	is_pal = (*((char *)0xBFC7FF52) == 'E');
	ResetGraph(0);	// (first: PSn00bSDK's ResetGraph sets up the callbacks, see psyq_compat.h)
	SetVideoMode(is_pal ? MODE_PAL : MODE_NTSC);
	PadInit(0);
	for (i = 0; i < 2; i++)
	{
		SetDefDispEnv(&disp[i], 0, i ? 0 : SCREEN_H, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&draw[i], 0, i ? SCREEN_H : 0, SCREEN_W, SCREEN_H);
		draw[i].isbg = 1;
		setRGB0(&draw[i], 0, 0, 0);
	}
	FntLoad(960, 256);
	SetDispMask(1);

	// The default level (the editor reads it when it connects, or sends its own after starting the game)
	DefaultSheet();
	DefaultLevel();
	tiles_seen = level->tiles_version - 1;
	version_seen = level->version;
	Restart();

	while (1)
	{
		opad = pad;
		pad = PadRead(1) & 0xFFFF;
		SheetUpload();
		Update();
		prim_ptr = prims[side];
		ClearOTagR(ot[side], LAYERS);
		Draw();
		DrawSync(0);
		VSync(0);
		PutDispEnv(&disp[side]);
		PutDrawEnv(&draw[side]);
		DrawOTag(&ot[side][LAYERS - 1]);
		side ^= 1;
		frame++;
	}

	return (0);
}
