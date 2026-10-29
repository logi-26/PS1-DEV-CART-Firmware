/*
* PS1 SLI: two PlayStations draw one 3D scene across two screens, 640 x 240 in all, each one half of it.
*
* A flight over an endless landscape (water, sand, grass, rock, snow; lit, Gouraud shaded, fading into the sky), with
* a ship that either controller steers. Both consoles run the same world in lockstep: each frame they exchange only
* their controller's buttons over the link cable (psxcomb, 8-byte packets), and each one simulates frame N once it has
* both consoles' buttons for it. Each draws its half of the view: the same camera, the projection's centre on the right
* edge of the left screen and on the left edge of the right one (the GTE's screen offset), and the polygons of the
* other half left out. Twice the polygons of one PlayStation, on twice the screen.
*
* Two consoles: run.bat ("ps1transfer -n CART -link -f sli.ps-exe": the BIOS tty kept, the link cable): the two talk
* to each other. Two, three or more screens (PS1 SLI 3 SCREENS): ../sli3/run.bat, each console through its cartridge
* and the computer ("-link usb" and "ps1transfer -net" to sli_hub.py), PCSX-Redux as a screen too (its serial port
* bridged to the hub):
* the hub gives each screen its place and sends every console each frame's buttons of all of them. Until then, it
* flies alone (its own part). Places: set by the scripts (SIDE_ADRS), else chosen when they meet; Select moves them.
*
* Controls (either console): D-pad turn and climb, Select swap the sides.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include <psxetc.h>
#include <psxapi.h>
#include "psyq_compat.h"
#include "psxcomb/psxcomb.h"

#define	SCREEN_W		320
#define	SCREEN_H		240
// The picture: "screens" screens of 320 pixels side by side, the projection distance growing with them (the same
// view, about 106 degrees, spread over more screens)
static int		screens = 2;
#define	FULL_W			(SCREEN_W * screens)
#define	PROJECTION		(120 * screens)
#define	HORIZON_Y		90			// Screen line of the projection's centre

#define	SIDE_ADRS		0x801BF000	// The scripts write here 0x534C4900 | place (0: the leftmost screen)
#define	SIDE_MAGIC		0x534C4900
#define	HOOK_TTY_MODE	(*((volatile uint8_t *)0x8000DF71) & 0x7F)	// The execute's tty mode: 0 USB link

/****************************************/
// Drawing: an ordering table of depths, the text in its first entries (drawn last)

#define	OT_LEN			1024
#define	PRIM_BYTES		98304

static DISPENV		disp[2];
static DRAWENV		draw[2];
static uint32_t		ot[2][OT_LEN];
static uint8_t		prims[2][PRIM_BYTES];
static uint8_t		*prim_ptr;
static int			db;

static void	*Alloc(int size)
{
	void	*p;

	if (prim_ptr + size > prims[db] + PRIM_BYTES - 64)
		return (NULL);
	p = prim_ptr;
	prim_ptr += size;
	return (p);
}

static void	Text(int x, int y, const char *s, int r, int g, int b)
{
	uint8_t	*start = prim_ptr, *end, *p;

	if (!Alloc(16 * (strlen(s) + 2)))
		return;
	end = FntSort(&ot[db][0], start, x, y, s);
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

static void	Shade(int x, int y, int w, int h)	// A dark band behind text
{
	TILE		*t = Alloc(sizeof(TILE));
	DR_TPAGE	*m = Alloc(sizeof(DR_TPAGE));

	if (!t || !m)
		return;
	setTile(t);
	setSemiTrans(t, 1);
	setXY0(t, x, y);
	setWH(t, w, h);
	setRGB0(t, 64, 64, 64);
	addPrim(&ot[db][1], t);
	setDrawTPage(m, 0, 0, getTPage(0, 2, 0, 0));	// (subtractive: darker)
	addPrim(&ot[db][1], m);
}

/****************************************/
// The landscape: a 64 x 64 height map repeated without end, made the same way on both consoles

#define	MAP				64
#define	CELL			320			// World units
#define	VIEW_CELLS		22			// How far the landscape goes (cells, at most)
static int		view_cells = VIEW_CELLS;	// (20 with the hub: serving the USB link takes a little of a frame)
#define	SEA				0

static int16_t	height[MAP][MAP];
static uint8_t	colour[MAP][MAP][3];		// Lit colour of each point (before the fog)

static uint32_t	seed = 12345;

static int	Random(void)
{
	seed = seed * 1103515245 + 12345;
	return ((seed >> 16) & 0x7FFF);
}

// Value noise on a lattice of "step" points (repeating over the map), smoothly interpolated
static void	Octave(int32_t *acc, int step, int amp)
{
	static int16_t	lat[MAP][MAP];
	int				n = MAP / step, x, y;

	for (y = 0; y < n; y++)
		for (x = 0; x < n; x++)
			lat[y][x] = Random() % (amp * 2 + 1) - amp;
	for (y = 0; y < MAP; y++)
		for (x = 0; x < MAP; x++)
		{
			int	x0 = x / step, y0 = y / step, x1 = (x0 + 1) % n, y1 = (y0 + 1) % n;
			int	fx = (x % step) * 256 / step, fy = (y % step) * 256 / step;
			int	sx = fx * fx * (768 - 2 * fx) >> 16, sy = fy * fy * (768 - 2 * fy) >> 16;	// (smooth step)
			int	a = lat[y0][x0] + ((lat[y0][x1] - lat[y0][x0]) * sx >> 8);
			int	b = lat[y1][x0] + ((lat[y1][x1] - lat[y1][x0]) * sx >> 8);

			acc[y * MAP + x] += a + ((b - a) * sy >> 8);
		}
}

static void	MakeLandscape(void)
{
	static int32_t	acc[MAP * MAP];
	int				x, y;

	memset(acc, 0, sizeof(acc));
	Octave(acc, 16, 520);
	Octave(acc, 8, 240);
	Octave(acc, 4, 90);
	Octave(acc, 2, 30);
	for (y = 0; y < MAP; y++)
		for (x = 0; x < MAP; x++)
		{
			int	h = acc[y * MAP + x] + 40;

			height[y][x] = (h > 0) ? h + h * h / 900 : h;	// (sharper peaks)
		}
	// Colours: by height (sand, grass, rock, snow), the water by its depth; lit from the left and above
	for (y = 0; y < MAP; y++)
		for (x = 0; x < MAP; x++)
		{
			int	h = height[y][x], dx = height[y][(x + 1) % MAP] - height[y][(x + MAP - 1) % MAP];
			int	dz = height[(y + 1) % MAP][x] - height[(y + MAP - 1) % MAP][x];
			int	light = 190 - dx / 3 + dz / 6, r, g, b;

			if (light < 60)		light = 60;
			if (light > 255)	light = 255;
			if (h <= SEA)
			{
				int	d = -h / 6;

				if (d > 70)
					d = 70;
				r = 30 - d / 3;
				g = 120 - d;
				b = 200 - d / 2;
				light = 230;
			}
			else if (h < 50)	{r = 215; g = 200; b = 135;}
			else if (h < 560)	{r = 60 + h / 12; g = 165 - h / 7; b = 50;}
			else if (h < 1000)	{r = 130; g = 118; b = 105;}
			else				{r = 245; g = 245; b = 255;}
			colour[y][x][0] = r * light / 255;
			colour[y][x][1] = g * light / 255;
			colour[y][x][2] = b * light / 255;
		}
}

static int	Height(int i, int j)			// (map point, the water's surface for the sea)
{
	int	h = height[i & (MAP - 1)][j & (MAP - 1)];

	return ((h < SEA) ? SEA : h);
}

static int	Floor(int v)					// v / CELL rounded down
{
	return ((v >= 0) ? v / CELL : -((-v + CELL - 1) / CELL));
}

static int	HeightAt(int x, int z)			// (world units: between the points, interpolated)
{
	int	j = Floor(x), i = Floor(z), fx = x - j * CELL, fz = z - i * CELL;
	int	a = Height(i, j) + (Height(i, j + 1) - Height(i, j)) * fx / CELL;
	int	b = Height(i + 1, j) + (Height(i + 1, j + 1) - Height(i + 1, j)) * fx / CELL;

	return (a + (b - a) * fz / CELL);
}

/****************************************/
// The world: the same on both consoles (changed only by the simulation, from both controllers' buttons)

typedef struct
{
	int32_t		x, y, z;		// Camera, world units * 16
	int32_t		yaw, pitch;		// 4096: a turn
	int32_t		turn, climb;	// Steering (smoothed)
	uint32_t	frame;
	uint16_t	last_buttons;
	int16_t		swapped;		// Select: the sides swapped
} World;

static World	world;

static void	WorldInit(void)
{
	memset(&world, 0, sizeof(world));
	world.x = 20 * CELL * 16;
	world.z = 20 * CELL * 16;
	world.y = 1500 * 16;
	world.pitch = 110;
}

static void	WorldStep(uint16_t buttons)
{
	int	want_turn = 0, want_climb = 0, ground, ahead_x, ahead_z, speed = 44 * 16;

	if ((buttons & PADselect) && !(world.last_buttons & PADselect))
		world.swapped ^= 1;
	world.last_buttons = buttons;
	if (buttons & PADLleft)		want_turn = -20;
	if (buttons & PADLright)	want_turn = 20;
	if (buttons & PADLup)		want_climb = -40;
	if (buttons & PADLdown)		want_climb = 40;
	if (!(buttons & (PADLleft | PADLright)))	// (no steering: a slow wandering course)
		want_turn = (isin((world.frame * 3) & 4095) * 9) >> 12;
	world.turn += (want_turn - world.turn) / 8;
	world.climb += (want_climb - world.climb) / 8;
	world.yaw = (world.yaw + world.turn) & 4095;
	world.x += (isin(world.yaw) * speed) >> 12;
	world.z += (icos(world.yaw) * speed) >> 12;
	// Height: steered, kept above the ground ahead
	ahead_x = (world.x >> 4) + ((isin(world.yaw) * 600) >> 12);
	ahead_z = (world.z >> 4) + ((icos(world.yaw) * 600) >> 12);
	ground = HeightAt(world.x >> 4, world.z >> 4);
	if (HeightAt(ahead_x, ahead_z) > ground)
		ground = HeightAt(ahead_x, ahead_z);
	world.y -= world.climb * 16;
	if (world.y < (ground + 700) * 16)
		world.y += ((ground + 700) * 16 - world.y) / 5;
	if (world.y > 2600 * 16)
		world.y = 2600 * 16;
	world.pitch = 100 + world.climb;
	world.frame++;
}

static uint8_t	WorldCheck(void)		// (sent with the buttons: a different world shows "OUT OF STEP")
{
	uint32_t	v = world.x ^ (world.y << 3) ^ (world.z << 7) ^ (world.yaw << 11) ^ world.frame;

	return ((v ^ (v >> 8) ^ (v >> 16) ^ (v >> 24)) & 0xFF);
}

/****************************************/
// Drawing the world: the sky, the landscape around the camera, the ship

static int			side;					// This console's place, 0: the leftmost screen (before Select)
static int			polys;
typedef struct { int16_t x, y; uint16_t z; uint8_t c[3], ok; } Point;
#define	GRID		(VIEW_CELLS * 2 + 2)
static Point		pts[GRID][GRID];
static uint32_t		pts_stamp[GRID][GRID], stamp;
static MATRIX		cam;

#define	FOG_NEAR	1500
#define	FOG_FAR		(view_cells * CELL)
static const uint8_t	fog[3] = {178, 200, 228};
static int8_t			ripple[64];			// The water's shine (a sine table)

static int	Side(void)
{
	return ((side + world.swapped) % screens);
}

static Point	*Project(int gi, int gj, int wi, int wj, int cx, int cy, int cz)
{
	Point	*p = &pts[gi][gj];

	if (pts_stamp[gi][gj] != stamp)
	{
		SVECTOR	v;
		int32_t	sxy, sz;
		int		mi = wi & (MAP - 1), mj = wj & (MAP - 1), f, i, water = (height[mi][mj] <= SEA);

		pts_stamp[gi][gj] = stamp;
		v.vx = wj * CELL - cx;
		v.vy = -(Height(wi, wj) - cy);
		v.vz = wi * CELL - cz;
		gte_ldv0(&v);
		gte_rtps();
		gte_stsxy(&sxy);
		gte_stsz(&sz);
		p->x = (int16_t)sxy;
		p->y = (int16_t)(sxy >> 16);
		p->z = sz;
		p->ok = (sz > 96);
		// Fog: towards the sky's colour with the distance; the water's shine moves
		f = (sz - FOG_NEAR) * 256 / (FOG_FAR - FOG_NEAR);
		if (f < 0)		f = 0;
		if (f > 256)	f = 256;
		for (i = 0; i < 3; i++)
		{
			int	c = colour[mi][mj][i];

			if (water)
				c += ripple[((world.frame * 60 + wj * 700 + wi * 1100) >> 6) & 63];
			if (c < 0)		c = 0;
			if (c > 255)	c = 255;
			p->c[i] = c + (((int)fog[i] - c) * f >> 8);
		}
	}
	return (p);
}

static void	DrawLandscape(void)
{
	int	cx = world.x >> 4, cy = world.y >> 4, cz = world.z >> 4, gi, gj;
	int	ci = Floor(cz), cj = Floor(cx), fx = isin(world.yaw), fz = icos(world.yaw);
	int	wedge_lo = SCREEN_W * Side() - FULL_W / 2, wedge_hi = wedge_lo + SCREEN_W;

	wedge_lo -= (wedge_lo < 0) ? -wedge_lo / 8 + 40 : 40;		// (a little wider)
	wedge_hi += (wedge_hi > 0) ? wedge_hi / 8 + 40 : 40;

	stamp++;
	for (gi = 0; gi < GRID - 1; gi++)
		for (gj = 0; gj < GRID - 1; gj++)
		{
			int			wi = ci + gi - VIEW_CELLS, wj = cj + gj - VIEW_CELLS;

			if ((gi < VIEW_CELLS - view_cells) || (gi > VIEW_CELLS + view_cells) ||
				(gj < VIEW_CELLS - view_cells) || (gj > VIEW_CELLS + view_cells))
				continue;
			int			dx = wj * CELL + CELL / 2 - cx, dz = wi * CELL + CELL / 2 - cz;
			int			ahead = (dx * fx + dz * fz) >> 12, across = (dx * fz - dz * fx) >> 12, otz;
			Point		*p0, *p1, *p2, *p3;
			POLY_G4		*q;

			// Only the cells in this console's wedge of the view, within the view distance: its screen's columns of
			// the whole picture, from lo to hi pixels off the centre (a little wider: the cells are tested by their
			// centre)
			if ((ahead < -CELL) || (dx * dx + dz * dz > FOG_FAR * FOG_FAR))
				continue;
			if ((across * PROJECTION < wedge_lo * ahead - CELL * PROJECTION) ||
				(across * PROJECTION > wedge_hi * ahead + CELL * PROJECTION))
				continue;
			p0 = Project(gi, gj, wi, wj, cx, cy, cz);
			p1 = Project(gi, gj + 1, wi, wj + 1, cx, cy, cz);
			p2 = Project(gi + 1, gj, wi + 1, wj, cx, cy, cz);
			p3 = Project(gi + 1, gj + 1, wi + 1, wj + 1, cx, cy, cz);
			if (!p0->ok || !p1->ok || !p2->ok || !p3->ok)
				continue;
			// Off this console's screen (the other half, below), or too big for the GPU
			if ((p0->x < 0) && (p1->x < 0) && (p2->x < 0) && (p3->x < 0))
				continue;
			if ((p0->x >= SCREEN_W) && (p1->x >= SCREEN_W) && (p2->x >= SCREEN_W) && (p3->x >= SCREEN_W))
				continue;
			if ((p0->y >= SCREEN_H) && (p1->y >= SCREEN_H) && (p2->y >= SCREEN_H) && (p3->y >= SCREEN_H))
				continue;
			if ((p0->x < -900) || (p0->x > 1200) || (p1->x < -900) || (p1->x > 1200) || (p2->x < -900) ||
				(p2->x > 1200) || (p3->x < -900) || (p3->x > 1200) || (p0->y < -400) || (p3->y > 700))
				continue;
			q = Alloc(sizeof(POLY_G4));
			if (!q)
				return;
			setPolyG4(q);
			setXY4(q, p0->x, p0->y, p1->x, p1->y, p2->x, p2->y, p3->x, p3->y);
			setRGB0(q, p0->c[0], p0->c[1], p0->c[2]);
			setRGB1(q, p1->c[0], p1->c[1], p1->c[2]);
			setRGB2(q, p2->c[0], p2->c[1], p2->c[2]);
			setRGB3(q, p3->c[0], p3->c[1], p3->c[2]);
			otz = (p0->z + p1->z + p2->z + p3->z) >> 4;
			if (otz < 8)			otz = 8;
			if (otz >= OT_LEN - 1)	otz = OT_LEN - 2;
			addPrim(&ot[db][otz], q);
			polys++;
		}
}

// The ship: in front of the camera, banking with the turns (flat shaded)
static SVECTOR			ship_v[8] =
{
	{0, 0, 260}, {-230, 30, -90}, {230, 30, -90}, {0, -70, -110}, {0, 40, -120}, {-70, -10, 60}, {70, -10, 60},
	{0, -40, -30}
};
static const uint8_t	ship_f[10][4] =		// Faces: 3 points, shade
{
	{0, 3, 1, 230}, {0, 2, 3, 200}, {0, 1, 4, 120}, {0, 4, 2, 100}, {1, 3, 4, 150}, {2, 4, 3, 130},
	{0, 5, 7, 255}, {0, 7, 6, 255}, {5, 6, 7, 200}, {1, 2, 4, 90}
};

static void	DrawShip(void)
{
	MATRIX	m;
	SVECTOR	r = {0, 0, 0, 0};
	VECTOR	t;
	int		i;

	r.vz = world.turn * 14;
	r.vx = -world.climb * 3;
	t.vx = 0;
	t.vy = 150 + world.climb * 2;
	t.vz = 950;
	RotMatrix(&r, &m);
	TransMatrix(&m, &t);
	gte_SetRotMatrix(&m);
	gte_SetTransMatrix(&m);
	for (i = 0; i < 10; i++)
	{
		POLY_F3	*f;
		int32_t	s0, s1, s2, z0, z1, z2, order;
		int		k = ship_f[i][3];

		gte_ldv3(&ship_v[ship_f[i][0]], &ship_v[ship_f[i][1]], &ship_v[ship_f[i][2]]);
		gte_rtpt();
		gte_stsxy3(&s0, &s1, &s2);
		gte_stsz3(&z0, &z1, &z2);
		if (!(f = Alloc(sizeof(POLY_F3))))
			break;
		setPolyF3(f);
		setXY3(f, (int16_t)s0, s0 >> 16, (int16_t)s1, s1 >> 16, (int16_t)s2, s2 >> 16);
		// (colours that stand out from the landscape: a white hull, red wings, a yellow canopy)
		if (i >= 6 && i <= 8)
			setRGB0(f, 255 * k / 255, 220 * k / 255, 60 * k / 255);
		else if ((i == 2) || (i == 3) || (i == 9))
			setRGB0(f, 230 * k / 255 + 20, 50 * k / 255, 40 * k / 255);
		else
			setRGB0(f, 235 * k / 255 + 20, 235 * k / 255 + 20, 245 * k / 255 + 10);
		// (in front of the landscape; its own faces ordered by depth, the far ones first: no back face culling)
		order = 3 + ((z0 + z1 + z2) / 3 - 900) / 40;
		if (order < 3)		order = 3;
		if (order > 7)		order = 7;
		addPrim(&ot[db][order], f);
		polys++;
	}
	gte_SetRotMatrix(&cam);
	gte_SetTransMatrix(&cam);
}

static void	DrawSky(void)
{
	POLY_G4	*s = Alloc(sizeof(POLY_G4));

	if (!s)
		return;
	setPolyG4(s);
	setXY4(s, 0, 0, SCREEN_W, 0, 0, HORIZON_Y + 30, SCREEN_W, HORIZON_Y + 30);
	setRGB0(s, 30, 60, 150);
	setRGB1(s, 30, 60, 150);
	setRGB2(s, fog[0], fog[1], fog[2]);
	setRGB3(s, fog[0], fog[1], fog[2]);
	addPrim(&ot[db][OT_LEN - 1], s);
}

static void	DrawWorld(void)
{
	MATRIX	ry, rx;
	SVECTOR	a = {0, 0, 0, 0}, b = {0, 0, 0, 0};
	VECTOR	t = {0, 0, 0};

	// The camera: the world turned by the yaw, then the pitch; the projection centred on the 640-pixel picture
	a.vy = -world.yaw;
	b.vx = world.pitch;
	RotMatrix(&a, &ry);
	RotMatrix(&b, &rx);
	MulMatrix0(&rx, &ry, &cam);
	TransMatrix(&cam, &t);
	gte_SetRotMatrix(&cam);
	gte_SetTransMatrix(&cam);
	gte_SetGeomOffset(FULL_W / 2 - SCREEN_W * Side(), HORIZON_Y);
	gte_SetGeomScreen(PROJECTION);
	polys = 0;
	DrawSky();
	DrawLandscape();
	DrawShip();
}

/****************************************/
// Lockstep: each frame's buttons of both consoles, over the link cable (a plain serial driver: the bytes received by
// the interrupt into a ring, sent when the transmitter is ready, as Net Yaroze link games do) or the USB link (psxcomb
// through the computer, read and written without waiting)

// Frames between a button press and the frame it is for (the link's time): 3 on the link cable, 4 with the hub
#define	DELAY			delay
static int		delay = 3;
#define	RING			64
#define	SYNC			0xA5
#define	HELLO			0xFFFF
#define	RESTART			0xFFFE		// The worlds differ (a corrupted packet got through): both start again
#define	START			0xFFFD		// From the hub: buttons = screens << 8 | place, the world from frame 0
#define	PCSX_MAGIC		(*(volatile uint32_t *)0x1F802080)	// PCSX-Redux: reads "PCSX"
#define	PKT				10			// Packet bytes

typedef struct { uint16_t buttons; uint8_t check, valid; } Input;

static Input	mine[RING], theirs[RING];
// For screenshots, set from the computer (ps1transfer -poke) on both consoles: the world stops at this frame (both on
// the same frame: the two halves of one picture), until it is set further again
uint32_t		freeze_frame = 0xFFFFFFFF;
static uint8_t	checks[RING];				// World check after each frame
static uint32_t	sim_frame, made_frame, peer_need;	// Next frame to simulate, next input to make, the peer's wait
static int		linked, my_id, out_of_step, usb, errors, restart_send, restarts;
static int		hub;						// The hub (sli_hub.py) gives the frames, else the other console
static uint32_t	hub_sent;					// Next input to send to the hub
static int		hub_paused;					// The hub's "freeze" (a hello from it): no waiting message
static uint32_t	last_rx_vsync, stalls;
static long		link_fd = -1;

#define	SIO1_DATA		(*(volatile uint8_t *)0x1F801050)
#define	SIO1_STAT		(*(volatile uint32_t *)0x1F801054)
#define	SIO1_MODE		(*(volatile uint16_t *)0x1F801058)
#define	SIO1_CTRL		(*(volatile uint16_t *)0x1F80105A)
#define	SIO1_BAUD		(*(volatile uint16_t *)0x1F80105E)
#define	SIO_TXRDY		0x0001
#define	SIO_RXRDY		0x0002
#define	SIO_ERRORS		0x0038		// Parity, overrun, framing
#define	SIO_CTRL_ON		0x0827		// TX and RX on, DTR, RTS (the other's CTS), receive interrupt after each byte
#define	SIO_ACK			0x0010
#define	SIO_RESET		0x0040
#define	SIO_RING		4096
static volatile uint8_t		sio_ring[SIO_RING];
static volatile uint32_t	sio_head, sio_tail;

static void	SioIrq(void)
{
	if (SIO1_STAT & SIO_ERRORS)
		errors++;
	while (SIO1_STAT & SIO_RXRDY)
	{
		uint8_t	c = SIO1_DATA;

		if (sio_head - sio_tail < SIO_RING)
			sio_ring[sio_head++ & (SIO_RING - 1)] = c;
	}
	SIO1_CTRL |= SIO_ACK;
}

static long	NoWait(long rw, long count)
{
	return (0);
}

static void	LinkOpen(void)
{
	// "-link usb": the USB link through the computer, else the link cable; PCSX-Redux: its serial port (bridged to
	// the hub), whatever the cartridge's tty mode reads there (no cartridge)
	usb = (HOOK_TTY_MODE == 0) && (PCSX_MAGIC != 0x58534350);
	hub = usb || (PCSX_MAGIC == 0x58534350);
	if (hub)
	{
		delay = 4;			// (a round trip through the bridges and the hub: a few frames)
		view_cells = 20;	// (the same on every screen, PCSX-Redux too)
	}
	if (usb)
	{
		CombSetLink(COMB_LINK_USB);
		AddCOMB();
		link_fd = open("sio:", O_RDWR);
		CombWaitCallback(NoWait);
		return;
	}
	// 8 data bits, 1 stop bit, no parity, 2116800 / 4 bps (about 529 kbps, the same on both)
	SIO1_CTRL = SIO_RESET;
	SIO1_MODE = 0x4E;
	SIO1_BAUD = 4;
	EnterCriticalSection();
	InterruptCallback(IRQ_SIO1, SioIrq);
	ExitCriticalSection();
	SIO1_CTRL = SIO_CTRL_ON | SIO_ACK;
}

static void	LinkWrite(const uint8_t *p, int n)
{
	if (usb)
	{
		if (link_fd >= 0)
			write(link_fd, (void *)p, n);		// (into the ring: the relay takes it)
		return;
	}
	while (n--)
	{
		int	wait = 20000;			// (about 2 ms: the other console's CTS off, it isn't there)

		while (!(SIO1_STAT & SIO_TXRDY) && --wait)
			;
		if (!wait)
			return;
		SIO1_DATA = *p++;
	}
}

static int	LinkRead(uint8_t *p, int max)
{
	int	n = 0;

	if (usb)
		return ((link_fd >= 0) ? (int)read(link_fd, p, max) : 0);
	while ((n < max) && (sio_tail != sio_head))
		p[n++] = sio_ring[sio_tail++ & (SIO_RING - 1)];
	return (n);
}

static uint16_t	Crc(const uint8_t *p)		// CRC-16 (CCITT) of a packet's first 8 bytes
{
	uint16_t	c = 0xFFFF;
	int			i, b;

	for (i = 0; i < PKT - 2; i++)
	{
		c ^= p[i] << 8;
		for (b = 0; b < 8; b++)
			c = (c & 0x8000) ? (c << 1) ^ 0x1021 : (c << 1);
	}
	return (c);
}

// A packet: sync, frame (2), buttons (2), world check, the frame the sender waits for (2), CRC (2)
static void	Packet(uint8_t *p, uint16_t f, uint16_t buttons, uint8_t check)
{
	uint16_t	c;

	p[0] = SYNC;
	p[1] = f;
	p[2] = f >> 8;
	p[3] = buttons;
	p[4] = buttons >> 8;
	p[5] = check;
	p[6] = sim_frame;
	p[7] = sim_frame >> 8;
	c = Crc(p);
	p[8] = c;
	p[9] = c >> 8;
}

// Each frame: the newest input, and the oldest ones the other console still waits for (a lost packet is sent again);
// a hello every half second (the other chooses its side by it), only that while alone
static void	Send(void)
{
	uint8_t		buf[PKT * 5];
	int			n = 0;
	uint32_t	f;

	if (!(VSync(-1) % 25))
	{
		uint32_t	forced = *(volatile uint32_t *)SIDE_ADRS;

		// (to the hub: the place the script asked for, 0xFF: any)
		Packet(buf, HELLO, my_id, ((forced >> 8) == (SIDE_MAGIC >> 8)) ? (forced & 0xFF) : 0xFF);
		n = PKT;
	}
	if (hub)					// The new inputs, once each (the link to the hub doesn't lose any)
	{
		while (linked && (hub_sent < made_frame) && (n < (int)sizeof(buf)))
		{
			Packet(buf + n, hub_sent, mine[hub_sent % RING].buttons, mine[hub_sent % RING].check);
			n += PKT;
			hub_sent++;
		}
		if (n)
			LinkWrite(buf, n);
		return;
	}
	if (restart_send)			// (a few times: the other console starts again too)
	{
		Packet(buf + n, RESTART, 0, 0);
		n += PKT;
		restart_send--;
	}
	if (linked)
	{
		for (f = peer_need; (f < made_frame) && (f < peer_need + 2); f++)
		{
			Packet(buf + n, f, mine[f % RING].buttons, mine[f % RING].check);
			n += PKT;
		}
		if (made_frame && (made_frame - 1 >= peer_need + 2))
		{
			f = made_frame - 1;
			Packet(buf + n, f, mine[f % RING].buttons, mine[f % RING].check);
			n += PKT;
		}
	}
	if (n)
		LinkWrite(buf, n);
}

static void	LinkStart(void)		// Both consoles: the same world from frame 0
{
	memset(mine, 0, sizeof(mine));
	memset(theirs, 0, sizeof(theirs));
	for (sim_frame = 0; sim_frame < DELAY; sim_frame++)	// (the first frames: no buttons, known on both sides)
		mine[sim_frame].valid = theirs[sim_frame].valid = 1;
	sim_frame = 0;
	made_frame = DELAY;
	hub_sent = DELAY;
	peer_need = 0;
	WorldInit();
	checks[0] = WorldCheck();
	linked = 1;
}

static void	Packet8(const uint8_t *p)
{
	uint16_t	f = p[1] | (p[2] << 8);

	last_rx_vsync = VSync(-1);
	if (hub)
	{
		if (f == START)					// Our place and the number of screens: the world from frame 0
		{
			screens = p[4] ? p[4] : 1;
			side = p[3];
			LinkStart();
		}
		else if (f == HELLO)
			hub_paused = 1;
		else if (linked && (f < HELLO - 2))	// A frame: everyone's buttons together
		{
			uint32_t	full = sim_frame + (int16_t)(f - (uint16_t)sim_frame);

			hub_paused = 0;
			if (full - sim_frame < RING)
			{
				theirs[full % RING].buttons = p[3] | (p[4] << 8);
				theirs[full % RING].valid = 1;
			}
		}
		return;
	}
	if (f == RESTART)
	{
		if (linked && (sim_frame > 50))		// (not again for the copies of the same restart)
		{
			restarts++;
			LinkStart();
		}
	}
	else if (f == HELLO)
	{
		int	id = p[3];

		if (id == my_id)					// (the same id by chance: another one)
			my_id = ((my_id * 97 + VSync(-1)) & 0xFF) | 1;
		else if (id)
		{
			if (*(volatile uint32_t *)SIDE_ADRS >> 8 != SIDE_MAGIC >> 8)
				side = (my_id > id);
			if (!linked)
				LinkStart();
		}
	}
	else
	{
		uint32_t	full;

		if (!linked)
			LinkStart();
		// (the frame number: 16 bits, near ours; the other console's wait: its low byte, near ours too)
		full = sim_frame + (int16_t)(f - (uint16_t)sim_frame);
		if (full - sim_frame < RING)
		{
			Input	*in = &theirs[full % RING];

			in->buttons = p[3] | (p[4] << 8);
			in->check = p[5];
			in->valid = 1;
		}
		peer_need = sim_frame + (int16_t)((p[6] | (p[7] << 8)) - (uint16_t)sim_frame);
	}
}

// The bytes received: whole packets (sync, sum) taken; out of step, a byte dropped until a packet starts
static void	Receive(void)
{
	static uint8_t	acc[PKT * 8];
	static int		acc_n;
	int				n, i;

	while ((n = LinkRead(acc + acc_n, sizeof(acc) - acc_n)) > 0)
	{
		acc_n += n;
		i = 0;
		while (acc_n - i >= PKT)
		{
			if ((acc[i] == SYNC) && (Crc(acc + i) == (acc[i + 8] | (acc[i + 9] << 8))))
			{
				Packet8(acc + i);
				i += PKT;
			}
			else
				i++;
		}
		memmove(acc, acc + i, acc_n - i);
		acc_n -= i;
	}
}

// One display frame: the new input made, then the frames both consoles have the buttons of (at most 2: catching up)
static void	Lockstep(uint16_t buttons)
{
	int	steps;

	if (!linked)
	{
		WorldStep(buttons);			// (alone: flying on our own until the other console answers)
		return;
	}
	if (made_frame < sim_frame + DELAY)
	{
		Input	*in = &mine[made_frame % RING];

		in->buttons = buttons;
		in->check = checks[(made_frame - DELAY) % RING];
		in->valid = 1;
		made_frame++;
	}
	for (steps = 0; (steps < 2) && (sim_frame < freeze_frame); steps++)
	{
		Input	*a = &mine[sim_frame % RING], *b = &theirs[sim_frame % RING];

		if (hub)						// (the hub's frame: everyone's buttons; the hub compares the checks)
		{
			if (!b->valid)
			{
				if (!steps)
					stalls++;
				break;
			}
			WorldStep(b->buttons);
			b->valid = a->valid = 0;
			theirs[(sim_frame + RING / 2) % RING].valid = 0;
			sim_frame++;
			checks[sim_frame % RING] = WorldCheck();
			continue;
		}
		if (!a->valid || !b->valid)
		{
			if (!steps)
				stalls++;
			break;
		}
		if ((sim_frame >= DELAY) && (b->check != checks[(sim_frame - DELAY) % RING]))
		{
			// The worlds differ: both start again from the same place (the flight goes on)
			out_of_step++;
			restarts++;
			restart_send = 3;
			LinkStart();
			return;
		}
		WorldStep(a->buttons | b->buttons);
		a->valid = b->valid = 0;
		mine[(sim_frame + RING / 2) % RING].valid = 0;
		theirs[(sim_frame + RING / 2) % RING].valid = 0;
		sim_frame++;
		checks[sim_frame % RING] = WorldCheck();
	}
}

/****************************************/

int	main(void)
{
	uint32_t	pad = 0, last_second = 0, frames = 0, fps = 0;
	int			i;
	char		s[64];

	ResetGraph(0);	// (first: PSn00bSDK's ResetGraph sets up the callbacks, see psyq_compat.h)
	SetVideoMode((*((char *)0xBFC7FF52) == 'E') ? MODE_PAL : MODE_NTSC);
	PadInit(0);
	for (i = 0; i < 2; i++)
	{
		SetDefDispEnv(&disp[i], 0, i ? 0 : SCREEN_H, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&draw[i], 0, i ? SCREEN_H : 0, SCREEN_W, SCREEN_H);
		draw[i].isbg = 1;
		setRGB0(&draw[i], fog[0], fog[1], fog[2]);
	}
	FntLoad(960, 256);
	SetDispMask(1);
	InitGeom();
	*(volatile uint32_t *)SIDE_ADRS = 0;		// (run.bat writes the side after the start)
	// An id for choosing the sides: from the time the console has run (different on each)
	my_id = ((*(volatile uint16_t *)0x1F801100) ^ VSync(-1) ^ (VSync(-1) >> 8)) & 0xFF;
	if (!my_id)
		my_id = 1;
	MakeLandscape();
	for (i = 0; i < 64; i++)
		ripple[i] = (isin(i * 64) * 18) >> 12;
	WorldInit();
	LinkOpen();

	while (1)
	{
		uint32_t	forced = *(volatile uint32_t *)SIDE_ADRS;

		if (((forced >> 8) == (SIDE_MAGIC >> 8)) && !hub)
			side = forced & 1;
		Receive();
		pad = PadRead(1) & 0xFFFF;
		Lockstep(pad);
		Send();
		// The other console stopped: alone again (with the hub: waiting for it, the world kept, the screens all on
		// the same frame)
		if (linked && !hub && (VSync(-1) - last_rx_vsync > 50 * 3))
			linked = 0;

		prim_ptr = prims[db];
		ClearOTagR(ot[db], OT_LEN);
		DrawWorld();
		// The label: in the picture's top corners (the middle screens: in their middle)
		{
			int		x = (Side() == 0) ? 6 : ((Side() == screens - 1) ? SCREEN_W - 150 : 88);
			char	label[24];

			if (screens == 2)
				strcpy(label, Side() ? "PS1 SLI  RIGHT" : "PS1 SLI  LEFT");
			else
				sprintf(label, "PS1 SLI  %d OF %d", Side() + 1, screens);
			Shade(x - 6, 0, 156, 24);
			Text(x, 4, label, 128, 110, 30);
		}
		if (!linked)
			sprintf(s, "ALONE %d POLYS %dFPS", polys, (int)fps);
		else
			sprintf(s, "%d POLYS %dFPS%s", polys, (int)fps, restarts ? " RESYNCED" : "");
		Text((Side() == 0) ? 6 : ((Side() == screens - 1) ? SCREEN_W - 150 : 88), 13, s, 100, 110, 120);
		if (linked && hub && !hub_paused && (VSync(-1) - last_rx_vsync > 50))
		{
			Shade(64, 200, 192, 14);
			Text(76, 203, "WAITING FOR THE HUB...", 128, 128, 128);
		}
		if (!linked)
		{
			Shade(40, 200, 240, 14);
			Text(52, 203, hub ? "WAITING FOR THE OTHER SCREENS" : "WAITING FOR THE OTHER PS1...", 128, 128, 128);
		}

		frames++;
		if (VSync(-1) - last_second >= 50)
		{
			fps = frames * 50 / (VSync(-1) - last_second);
			frames = 0;
			last_second = VSync(-1);
		}
		DrawSync(0);
		VSync(0);
		PutDispEnv(&disp[db]);
		PutDrawEnv(&draw[db]);
		DrawOTag(&ot[db][OT_LEN - 1]);
		db ^= 1;
	}

	return (0);
}
