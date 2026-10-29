/*
* PS1 CO-PROCESSOR: the computer as the PlayStation's co-processor, through the PS1 DEV CART (or PCSX-Redux).
*
* Two scenes, each one either computed by the PlayStation alone or by the computer (Select switches), with the time
* the work takes shown at the bottom:
* - Swarm: 1000 units finding their way around walls to a target (path field, each unit pushed away from the ones near
*   it), 50 steps a second. With the computer, it runs the simulation and sends the positions (3 bytes a unit); the
*   PlayStation only draws them, moving them smoothly between two updates.
* - Ray tracer: spheres with reflections and shadows. With the computer, it ray traces the camera's view and sends it
*   as an MDEC bitstream, decoded by the PlayStation's MDEC; alone, the PlayStation ray traces it in fixed point, a
*   few rows a frame.
*
* The computer runs coproc_server.py and "ps1transfer -r -e -f coproc.ps-exe -net 127.0.0.1:7030" (run.bat), or
* PCSX-Redux with its serial port bridged to the server (redux.bat). Without the computer, the PS1 alone modes still
* work. Protocol: coproc.h.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxetc.h>
#include <psxapi.h>
#include <psxpress.h>
#include "psyq_compat.h"
#include "psxcomb/psxcomb.h"
#include "coproc.h"

#define	SCREEN_W		320
#define	SCREEN_H		240
#define	VIEW_H			224			// The scenes; the status bar below
#define	BAR_Y			VIEW_H

// Colours (as the PS1 DEV CART tools)
#define	C_BG			14, 16, 28
#define	C_BAR			40, 50, 120
#define	C_PANEL			20, 24, 40
#define	C_SELECT		50, 70, 130
#define	C_TEXT			128, 128, 128	// (text tint: 128 = as the font)
#define	C_DIM			70, 70, 80
#define	C_YELLOW		128, 110, 30
#define	C_GREEN			60, 128, 60
#define	C_ERROR			230, 70, 70
#define	SET_RGB(_p_, _c_)			_SET_RGB(_p_, _c_)
#define	_SET_RGB(_p_, _r_, _g_, _b_)	setRGB0(_p_, _r_, _g_, _b_)

/****************************************/
// Drawing: one ordering table, drawn by layers

enum {L_BG, L_IMAGE, L_WALL, L_UNIT, L_MARK, L_PANEL, L_TEXT, LAYERS};

#define	PRIM_BYTES		49152

static DISPENV		disp[2];
static DRAWENV		draw[2];
static uint32_t		ot[2][LAYERS];
static uint8_t		prims[2][PRIM_BYTES];
static uint8_t		*prim_ptr;
static int			side, is_pal, frame;

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

static void	CentreText(int layer, int y, const char *s, int r, int g, int b)
{
	Text(layer, (SCREEN_W - strlen(s) * 8) / 2, y, s, r, g, b);
}

// Time measured in horizontal blanks (64 us): VSync(1) counts them since the last VSync(0) (16 bits: 4 s at most)
static int	Now(void)
{
	return (VSync(1));
}

static int	HblanksToUs(int n)
{
	return ((n & 0xFFFF) * 64);
}

/****************************************/
// Link: on a console, psxcomb's USB link, bridged to the server by "ps1transfer -net"; in PCSX-Redux, its serial port
// (SIO1) in raw mode, bridged by redux.ps1 (the same bytes)

enum {LINK_NONE, LINK_CART, LINK_EMULATOR};

static int		link_kind = LINK_NONE, welcomed, last_rx_frame;
static long		link_fd = -1;
static uint8_t	tx[8192];
static int		tx_n;
static uint8_t	rx[2048];
static uint32_t	rx_bytes, rx_rate, rx_rate_bytes;

#define	PCSX_MAGIC		(*(volatile uint32_t *)0x1F802080)	// PCSX-Redux: reads "PCSX"
#define	HOOK_USB_PROCESS	(*(volatile uint32_t *)0x8000DF74)	// PS1 DEV CART kernel hook: its USB service
#define	SIO1_DATA		(*(volatile uint8_t *)0x1F801050)
#define	SIO1_STAT		(*(volatile uint32_t *)0x1F801054)
#define	SIO1_MODE		(*(volatile uint16_t *)0x1F801058)
#define	SIO1_CTRL		(*(volatile uint16_t *)0x1F80105A)
#define	SIO1_BAUD		(*(volatile uint16_t *)0x1F80105E)
#define	SIO_TXRDY		0x0001
#define	SIO_RXRDY		0x0002
#define	SIO_CTRL_ON		0x0827		// TX and RX on, DTR, RTS, receive interrupt (after each byte)
#define	SIO_ACK			0x0010
#define	SIO_RESET		0x0040
#define	SIO_RING		16384
static volatile uint8_t		sio_ring[SIO_RING];
static volatile uint32_t	sio_head, sio_tail;

static void	SioIrq(void)
{
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
	return (0);		// Stop at once: a read gives what has arrived, a write what fits
}

static void	LinkOpen(void)
{
	if (PCSX_MAGIC == 0x58534350)
	{
		// (the emulator doesn't limit the serial port to its baud rate: the fastest one, 8N1)
		link_kind = LINK_EMULATOR;
		SIO1_CTRL = SIO_RESET;
		SIO1_MODE = 0x4E;
		SIO1_BAUD = 1;
		EnterCriticalSection();
		InterruptCallback(IRQ_SIO1, SioIrq);
		ExitCriticalSection();
		SIO1_CTRL = SIO_CTRL_ON | SIO_ACK;
		return;
	}
	if (HOOK_USB_PROCESS - 0x8000C000 >= 0x2000)		// No PS1 DEV CART firmware: the PS1 alone modes only
		return;
	link_kind = LINK_CART;
	CombSetLink(COMB_LINK_USB);		// (before AddCOMB: the rings are published at once)
	AddCOMB();
	link_fd = open("sio:", O_RDWR);
	CombWaitCallback(NoWait);
}

static void	Send(int type, const void *payload, int n)
{
	if ((link_kind == LINK_NONE) || (tx_n + CP_HEADER + n > (int)sizeof(tx)))
		return;
	tx[tx_n++] = type;
	tx[tx_n++] = n;
	tx[tx_n++] = n >> 8;
	memcpy(tx + tx_n, payload, n);
	tx_n += n;
}

static void	LinkFlush(void)
{
	long	n = 0;

	if (link_kind == LINK_EMULATOR)
	{
		while (n < tx_n)
		{
			int	wait = 100000;

			while (!(SIO1_STAT & SIO_TXRDY) && --wait)
				;
			if (!wait)
				break;
			SIO1_DATA = tx[n++];
		}
	}
	else if ((link_fd >= 0) && tx_n)
		n = write(link_fd, tx, tx_n);
	if (n > 0)
	{
		memmove(tx, tx + n, tx_n - n);
		tx_n -= n;
	}
}

static int	Connected(void)
{
	return (welcomed && (frame - last_rx_frame < 50 * 5));
}

/****************************************/
// Packets from the computer (payloads up to 32 KB: a still ray traced frame is about 24)

static uint8_t	hdr[CP_HEADER];
static int		hdr_n, type, left, got;
static uint8_t	payload[32768] __attribute__((aligned(4)));

#define	U16(_p_)	((_p_)[0] | ((_p_)[1] << 8))
#define	U32(_p_)	(U16(_p_) | ((uint32_t)U16((_p_) + 2) << 16))

static void	Packet(const uint8_t *p, int n);

static void	Parse(const uint8_t *p, int n)
{
	while (n)
	{
		if (hdr_n < CP_HEADER)
		{
			hdr[hdr_n++] = *p++;
			n--;
			if (hdr_n < CP_HEADER)
				continue;
			type = hdr[0];
			left = U16(hdr + 1);
			got = 0;
			if (!left)
			{
				Packet(payload, 0);
				hdr_n = 0;
			}
			continue;
		}
		while (n && left)
		{
			if (got < (int)sizeof(payload))
				payload[got] = *p;
			got++;
			p++;
			n--;
			left--;
		}
		if (!left)
		{
			if (got <= (int)sizeof(payload))
				Packet(payload, got);
			hdr_n = 0;
		}
	}
}

static void	LinkReceive(void)
{
	long	n;
	int		i;

	if (link_kind == LINK_EMULATOR)
	{
		while (sio_head != sio_tail)
		{
			for (n = 0; (n < (long)sizeof(rx)) && (sio_tail != sio_head); n++)
				rx[n] = sio_ring[sio_tail++ & (SIO_RING - 1)];
			last_rx_frame = frame;
			rx_bytes += n;
			Parse(rx, n);
		}
		return;
	}
	for (i = 0; (i < 32) && (link_fd >= 0); i++)
	{
		n = read(link_fd, rx, sizeof(rx));
		if (n <= 0)
			break;
		last_rx_frame = frame;
		rx_bytes += n;
		Parse(rx, n);
		if (n < (long)sizeof(rx))
			break;
	}
}

/****************************************/
// Swarm

static uint8_t	walls[CP_CELLS];				// 1: wall
static int		target_x = CP_GRID_W - 8, target_y = CP_GRID_H / 2, cursor_x = 40, cursor_y = 28;
static int16_t	dir_x[CP_CELLS], dir_y[CP_CELLS];	// Direction of each cell (256 = 1)
static int		units = CP_UNITS_MAX;
static int32_t	ux[CP_UNITS_MAX], uy[CP_UNITS_MAX];	// PS1 alone: pixels * 256
static uint16_t	px0[CP_UNITS_MAX], px1[CP_UNITS_MAX];	// With the computer: the last two positions received
static uint8_t	py0[CP_UNITS_MAX], py1[CP_UNITS_MAX];
static int		swarm_pc, swarm_wanted_pc = 1, units_frame, units_interval = 5, units_seq, units_received;
static int		sim_us, field_us, pc_step_us, units_rate, units_count_second;
static int		paint_value = -1;
static uint8_t	unit_shade[CP_UNITS_MAX];		// Each unit's colour

// The starting layout: a border, two walls with gaps, a box in the middle
static void	SwarmWalls(void)
{
	int	x, y;

	memset(walls, 0, sizeof(walls));
	for (x = 0; x < CP_GRID_W; x++)
		walls[x] = walls[(CP_GRID_H - 1) * CP_GRID_W + x] = 1;
	for (y = 0; y < CP_GRID_H; y++)
		walls[y * CP_GRID_W] = walls[y * CP_GRID_W + CP_GRID_W - 1] = 1;
	for (y = 8; y < 48; y++)
		walls[y * CP_GRID_W + 26] = walls[y * CP_GRID_W + 27] = 1;
	for (y = 1; y < 20; y++)
		walls[y * CP_GRID_W + 52] = walls[y * CP_GRID_W + 53] = 1;
	for (y = 36; y < CP_GRID_H - 1; y++)
		walls[y * CP_GRID_W + 52] = walls[y * CP_GRID_W + 53] = 1;
	for (y = 24; y < 32; y++)
		for (x = 38; x < 44; x++)
			walls[y * CP_GRID_W + x] = 1;
}

static void	SwarmSpawn(void)
{
	int	i, x, y;

	for (i = 0; i < units; i++)
	{
		do
		{
			x = 8 + rand() % 90;
			y = 8 + rand() % (VIEW_H - 16);
		} while (walls[(y / CP_CELL) * CP_GRID_W + x / CP_CELL]);
		ux[i] = x << 8;
		uy[i] = y << 8;
		unit_shade[i] = 80 + i * 175 / units;
		px0[i] = px1[i] = x;
		py0[i] = py1[i] = y;
	}
}

// Path field: distances to the target (Dijkstra with a binary heap, 10 straight, 14 diagonal, a diagonal only when
// both sides are free), then each cell's direction to its neighbour nearest to the target (as coproc_server.py)
static const int8_t	nbr[8][3] = {{1, 0, 10}, {-1, 0, 10}, {0, 1, 10}, {0, -1, 10}, {1, 1, 14}, {-1, 1, 14}, {1, -1, 14},
								 {-1, -1, 14}};
static int32_t		dist[CP_CELLS];
static uint32_t		heap[CP_CELLS * 4];

static int	Free(int x, int y)
{
	return ((x >= 0) && (x < CP_GRID_W) && (y >= 0) && (y < CP_GRID_H) && !walls[y * CP_GRID_W + x]);
}

static void	SwarmField(void)
{
	int			n = 0, i, k, start = Now();

	for (i = 0; i < CP_CELLS; i++)
		dist[i] = 0x7FFFFFFF;
	if (Free(target_x, target_y))
	{
		dist[target_y * CP_GRID_W + target_x] = 0;
		heap[n++] = target_y * CP_GRID_W + target_x;	// (distance << 13 | cell: cells < 8192)
	}
	while (n)
	{
		uint32_t	top = heap[0], last = heap[--n];
		int			c = top & 0x1FFF, d = top >> 13, x = c % CP_GRID_W, y = c / CP_GRID_W, j = 0;

		while (1)					// Heap: the last one down from the top
		{
			int	m = j * 2 + 1;

			if (m >= n)
				break;
			if ((m + 1 < n) && (heap[m + 1] < heap[m]))
				m++;
			if (heap[m] >= last)
				break;
			heap[j] = heap[m];
			j = m;
		}
		heap[j] = last;
		if (d > dist[c])
			continue;
		for (k = 0; k < 8; k++)
		{
			int	nx = x + nbr[k][0], ny = y + nbr[k][1], nd = d + nbr[k][2];

			if (!Free(nx, ny) || (nbr[k][0] && nbr[k][1] && (!Free(nx, y) || !Free(x, ny))))
				continue;
			if ((nd < dist[ny * CP_GRID_W + nx]) && (n < (int)(sizeof(heap) / 4)))
			{
				uint32_t	v = ((uint32_t)nd << 13) | (ny * CP_GRID_W + nx);

				dist[ny * CP_GRID_W + nx] = nd;
				for (j = n++; j && (heap[(j - 1) / 2] > v); j = (j - 1) / 2)	// Up
					heap[j] = heap[(j - 1) / 2];
				heap[j] = v;
			}
		}
	}
	for (i = 0; i < CP_CELLS; i++)
	{
		int	x = i % CP_GRID_W, y = i / CP_GRID_W, best = dist[i];

		dir_x[i] = dir_y[i] = 0;
		for (k = 0; k < 8; k++)
		{
			int	nx = x + nbr[k][0], ny = y + nbr[k][1];

			if (!Free(nx, ny) || (nbr[k][0] && nbr[k][1] && (!Free(nx, y) || !Free(x, ny))))
				continue;
			if (dist[ny * CP_GRID_W + nx] < best)
			{
				best = dist[ny * CP_GRID_W + nx];
				dir_x[i] = nbr[k][0] * (nbr[k][1] ? 181 : 256);
				dir_y[i] = nbr[k][1] * (nbr[k][0] ? 181 : 256);
			}
		}
	}
	field_us = HblanksToUs(Now() - start);
}

// A step of the PS1 alone simulation (the same rules as coproc_server.py): the path field's direction, pushed away
// from the units nearer than 3 pixels (the 3 x 3 cells around, at most 16 of them), not into walls. The push of a
// unit at d: d * (R^2 - |d|^2) / (R (|d|^2 + 1)) (pixels; one division, no square root)
#define	SEP_R			(3 * 256)
#define	SEP_K			40
#define	MAX_V			(CP_SPEED * 3 / 2 * 256)
static int16_t	bucket[CP_CELLS], next_unit[CP_UNITS_MAX];

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

static void	SwarmStep(void)
{
	int	i, start = Now(), fps = is_pal ? 50 : 60;

	for (i = 0; i < CP_CELLS; i++)
		bucket[i] = -1;
	for (i = 0; i < units; i++)
	{
		int	c = (uy[i] >> 10) * CP_GRID_W + (ux[i] >> 10);

		next_unit[i] = bucket[c];
		bucket[c] = i;
	}
	for (i = 0; i < units; i++)
	{
		int	cx = ux[i] >> 10, cy = uy[i] >> 10, c = cy * CP_GRID_W + cx, bx, by, checked = 0;
		int	vx = dir_x[c] * CP_SPEED, vy = dir_y[c] * CP_SPEED, push_x = 0, push_y = 0, nx, ny;

		for (by = cy - 1; by <= cy + 1; by++)
			for (bx = cx - 1; bx <= cx + 1; bx++)
			{
				int	j;

				if ((bx < 0) || (bx >= CP_GRID_W) || (by < 0) || (by >= CP_GRID_H))
					continue;
				for (j = bucket[by * CP_GRID_W + bx]; (j >= 0) && (checked < 16); j = next_unit[j])
				{
					int	dx = ux[i] - ux[j], dy = uy[i] - uy[j], d2, k;

					if ((j == i) || (dx >= SEP_R) || (dx <= -SEP_R) || (dy >= SEP_R) || (dy <= -SEP_R))
						continue;
					checked++;
					d2 = dx * dx + dy * dy;
					if (!d2 || (d2 >= SEP_R * SEP_R))
						continue;
					k = 3 * (d2 + 65536);			// (pixels * 256 squared)
					push_x += dx * (SEP_R * SEP_R - d2) / k;
					push_y += dy * (SEP_R * SEP_R - d2) / k;
				}
			}
		vx += push_x * SEP_K;
		vy += push_y * SEP_K;
		if ((vx > MAX_V) || (vx < -MAX_V) || (vy > MAX_V) || (vy < -MAX_V))
		{
			int	m = Isqrt((uint32_t)((vx >> 8) * (vx >> 8) + (vy >> 8) * (vy >> 8))) << 8;

			if (m > MAX_V)
			{
				int	f = (MAX_V << 8) / m;		// (256: unchanged)

				vx = (vx >> 4) * f >> 4;
				vy = (vy >> 4) * f >> 4;
			}
		}
		// One axis after the other: not into a wall, not out of the field
		nx = ux[i] + vx / fps;
		if (nx < 0)							nx = 0;
		if (nx > ((SCREEN_W - 1) << 8))		nx = (SCREEN_W - 1) << 8;
		if (!walls[cy * CP_GRID_W + (nx >> 10)])
			ux[i] = nx;
		ny = uy[i] + vy / fps;
		if (ny < 0)							ny = 0;
		if (ny > ((VIEW_H - 1) << 8))		ny = (VIEW_H - 1) << 8;
		if (!walls[(ny >> 10) * CP_GRID_W + (ux[i] >> 10)])
			uy[i] = ny;
	}
	sim_us = (sim_us * 7 + HblanksToUs(Now() - start)) / 8;		// (smoothed for the bar)
}

static void	SendWalls(int type_)
{
	uint8_t	b[CP_WALL_BYTES];
	int		i;

	memset(b, 0, sizeof(b));
	for (i = 0; i < CP_CELLS; i++)
		if (walls[i])
			b[i >> 3] |= 1 << (i & 7);
	Send(type_, b, sizeof(b));
}

// To the computer: the target, the walls and the units where they are now
static void	SwarmToComputer(void)
{
	static uint8_t	b[4 + CP_WALL_BYTES + CP_UNITS_MAX * 4];
	int				i, n = 0;

	b[n++] = target_x;
	b[n++] = target_y;
	memset(b + n, 0, CP_WALL_BYTES);
	for (i = 0; i < CP_CELLS; i++)
		if (walls[i])
			b[n + (i >> 3)] |= 1 << (i & 7);
	n += CP_WALL_BYTES;
	b[n++] = units;
	b[n++] = units >> 8;
	for (i = 0; i < units; i++)
	{
		int	x = ux[i] >> 4, y = uy[i] >> 4;

		b[n++] = x;
		b[n++] = x >> 8;
		b[n++] = y;
		b[n++] = y >> 8;
	}
	Send(CP_SWARM_BEGIN, b, n);
	swarm_pc = 1;
	units_received = 0;
}

static void	SwarmToPs1(void)
{
	int	i;

	if (swarm_pc)
		Send(CP_SWARM_END, NULL, 0);
	swarm_pc = 0;
	if (units_received)
		for (i = 0; i < units; i++)
		{
			ux[i] = px1[i] << 8;
			uy[i] = py1[i] << 8;
		}
}

// Positions ('U', 3 bytes a unit) or the moves since the last ones ('V', 2 bytes)
static void	UnitsPacket(const uint8_t *p, int n, int moves)
{
	int	i, count, size = moves ? 2 : 3, frac;

	if ((n < 10) || !swarm_pc || (moves && !units_received))
		return;
	units_seq = U16(p);
	pc_step_us = U32(p + 2);
	count = U16(p + 8);
	if (count > units)
		count = units;
	if (n < 10 + count * size)
		return;
	{
		int	t = frame - units_frame, d = units_interval;

		if (t > d)
			t = d;
		frac = t * 256 / d;
	}
	for (i = 0; i < count; i++)
	{
		const uint8_t	*u = p + 10 + i * size;
		int				x, y;

		// (the one shown now becomes the start of the next movement)
		if (units_received)
		{
			px0[i] = px0[i] + (((px1[i] - px0[i]) * frac) >> 8);
			py0[i] = py0[i] + (((py1[i] - py0[i]) * frac) >> 8);
		}
		if (moves)
		{
			x = px1[i] + (int8_t)u[0];
			y = py1[i] + (int8_t)u[1];
		}
		else
		{
			x = u[0] | (u[1] << 8);
			y = u[2];
		}
		px1[i] = x;
		py1[i] = y;
		if (!units_received)
		{
			px0[i] = px1[i];
			py0[i] = py1[i];
		}
	}
	units_interval = frame - units_frame;
	if (units_interval < 1)
		units_interval = 1;
	if (units_interval > 25)
		units_interval = 25;
	units_frame = frame;
	units_received = 1;
	units_count_second++;
	{
		uint8_t	a[2] = {units_seq, units_seq >> 8};

		Send(CP_SWARM_ACK, a, 2);
	}
}

/****************************************/
// Ray tracer: the camera, the computer's frames (MDEC) or the PS1's own (fixed point)

#define	TEX_X			640			// The frame in VRAM (320 x 224 at most, 16 bits)
#define	TEX_Y			0
#define	RLE_WORDS		32768

static int		cam_yaw = 300, cam_pitch = 260, cam_dist = 6 * 256 + 128, cam_moved_frame;
static int		req_yaw = -1, req_pitch, req_dist;		// Camera of the last request
static int		ray_wanted_pc = 1, ray_pc, ray_inflight, ray_id, ray_quality, ray_full, ray_w, ray_h, ray_frames, ray_rate;
static int		ray_render_ms, ray_encode_ms, ray_bytes, ray_decode_us, ray_request_frame;
static uint32_t	rle[RLE_WORDS + 64];
static uint32_t	frame_buf[320 * 224 / 2];
static uint32_t	bs_buf[32768 / 4];

static void	MdecInit(void)
{
	int	i;

	DecDCTReset(1);
	for (i = 0; i < 1000; i++)
		__asm__ volatile ("nop");
	*(volatile uint32_t *)0x1F801824 = 0x60000000;	// (the DMA request enable after the reset, see videostreamer)
	DecDCTPutEnv(NULL, 0);
}

// A frame of the computer: decompressed (DecDCTvlc), decoded by the MDEC, its 16-pixel strips uploaded
static void	FramePacket(const uint8_t *p, int n)
{
	VLC_Context	ctx;
	RECT		rect;
	int			w, h, x, i, codes, padded, start = Now();

	if (n < 10)
		return;
	w = U16(p + 2);
	h = U16(p + 4);
	if ((w > 320) || (h > 224) || (w & 15) || (h & 15))
		return;
	if (U16(p) == ray_id)
		ray_inflight = 0;
	ray_render_ms = U16(p + 6);
	ray_encode_ms = U16(p + 8);
	ray_bytes = n - 10;
	ray_frames++;
	if (!ray_pc)
		return;
	memcpy(bs_buf, p + 10, n - 10);
	if (DecDCTvlcStart(&ctx, rle, RLE_WORDS, bs_buf) != 0)
		return;
	if (*(volatile uint32_t *)0x1F801824 & (1 << 29))	// MDEC still busy (not expected): reset
		MdecInit();
	codes = rle[0] & 0xFFFF;
	padded = (codes + 31) & ~31;			// (fed in 32-word chunks: zero after the codes, see videostreamer)
	for (i = codes; i < padded; i++)
		rle[1 + i] = 0;
	DecDCTin(rle, DECDCT_MODE_16BPP_BIT15);
	DecDCTout(frame_buf, w * h / 2);
	if (DecDCToutSync(0))
	{
		MdecInit();
		return;
	}
	for (x = 0; x < w; x += 16)
	{
		setRECT(&rect, TEX_X + x, TEX_Y, 16, h);
		LoadImage(&rect, frame_buf + x * h / 2);
	}
	DrawSync(0);
	ray_w = w;
	ray_h = h;
	if ((U16(p) == ray_id) && ray_quality)
		ray_full = 1;
	ray_decode_us = HblanksToUs(Now() - start);
}

// PS1 alone: the same scene as coproc_server.py, in fixed point (16.16)
typedef struct { int32_t x, y, z; } V3;

#define	FONE				65536
#define	FX(_f_)			((int32_t)((_f_) * 65536))
#define	MUL(_a_, _b_)	((int32_t)(((int64_t)(_a_) * (_b_)) >> 16))

// a / b in 16.16 with 32-bit division only (no 64-bit division here): the whole part, then the fraction bit by bit
static int32_t	Div(int32_t a, int32_t b)
{
	uint32_t	ua = (a < 0) ? -a : a, ub = (b < 0) ? -b : b, q, rem;
	int			i;

	if (!ub)
		return (0x7FFFFFFF);
	q = ua / ub;
	rem = ua % ub;
	for (i = 0; i < 16; i++)
	{
		rem <<= 1;
		q <<= 1;
		if (rem >= ub)
		{
			rem -= ub;
			q |= 1;
		}
	}
	return (((a < 0) != (b < 0)) ? -(int32_t)q : (int32_t)q);
}

static int32_t	Sqrt(int32_t a)		// 16.16
{
	uint64_t	v = (uint64_t)a << 16, r = 0, b = (uint64_t)1 << 62;

	if (a <= 0)
		return (0);
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
	return ((int32_t)r);
}

static int32_t	Dot(V3 a, V3 b)
{
	return (MUL(a.x, b.x) + MUL(a.y, b.y) + MUL(a.z, b.z));
}

static V3	Add(V3 a, V3 b)				{ V3 r = {a.x + b.x, a.y + b.y, a.z + b.z}; return (r); }
static V3	Sub(V3 a, V3 b)				{ V3 r = {a.x - b.x, a.y - b.y, a.z - b.z}; return (r); }
static V3	Scale(V3 a, int32_t s)		{ V3 r = {MUL(a.x, s), MUL(a.y, s), MUL(a.z, s)}; return (r); }

static V3	Normalize(V3 a)
{
	int32_t	l = Sqrt(Dot(a, a));

	if (l)
	{
		a.x = Div(a.x, l);
		a.y = Div(a.y, l);
		a.z = Div(a.z, l);
	}
	return (a);
}

static V3	Cross(V3 a, V3 b)
{
	V3	r = {MUL(a.y, b.z) - MUL(a.z, b.y), MUL(a.z, b.x) - MUL(a.x, b.z), MUL(a.x, b.y) - MUL(a.y, b.x)};

	return (r);
}

typedef struct { V3 c; int32_t r; int col[3]; int32_t refl; } Sphere;

static const Sphere	spheres[4] =
{
	{{FX(0.0), FX(1.0), FX(0.0)}, FX(1.0), {230, 60, 60}, FX(0.25)},
	{{FX(2.2), FX(0.7), FX(1.0)}, FX(0.7), {60, 200, 90}, FX(0.1)},
	{{FX(-2.0), FX(0.9), FX(0.8)}, FX(0.9), {230, 230, 240}, FX(0.8)},
	{{FX(0.8), FX(0.45), FX(-1.8)}, FX(0.45), {230, 190, 60}, FX(0.3)},
};
static V3	light;
static V3	cam_o, cam_f, cam_r, cam_u;

// Nearest hit of a ray: -1 none, 0 floor, 1+ sphere; its distance in *t
static int	Hit(V3 o, V3 d, int32_t *t)
{
	int	i, h = -1;

	*t = 0x7FFFFFFF;
	if (d.y < -64)
	{
		int32_t	tf = Div(-o.y, d.y);

		if ((tf > 8) && (tf < *t))
		{
			*t = tf;
			h = 0;
		}
	}
	for (i = 0; i < 4; i++)
	{
		V3		oc = Sub(o, spheres[i].c);
		int32_t	b = Dot(oc, d), disc = MUL(b, b) - (Dot(oc, oc) - MUL(spheres[i].r, spheres[i].r)), sq, ts;

		if (disc <= 0)
			continue;
		sq = Sqrt(disc);
		ts = -b - sq;
		if (ts <= 8)
			ts = -b + sq;
		if ((ts > 8) && (ts < *t))
		{
			*t = ts;
			h = i + 1;
		}
	}
	return (h);
}

static void	Trace(V3 o, V3 d, int depth, int *rgb)
{
	int32_t	t, diffuse, spec, refl, up;
	int		h = Hit(o, d, &t), i, base[3];
	V3		p, n, so, rd;

	if (h < 0)
	{
		up = d.y * 3 / 2;
		if (up < 0)		up = 0;
		if (up > FONE)	up = FONE;
		rgb[0] = 150 + MUL(40 - 150, up);
		rgb[1] = 180 + MUL(60 - 180, up);
		rgb[2] = 230 + MUL(140 - 230, up);
		return;
	}
	p = Add(o, Scale(d, t));
	if (!h)
	{
		n.x = n.z = 0;
		n.y = FONE;
		i = ((p.x >> 16) + (p.z >> 16)) & 1;
		base[0] = i ? 40 : 200;
		base[1] = i ? 50 : 200;
		base[2] = i ? 90 : 210;
		refl = FX(0.15);
	}
	else
	{
		const Sphere	*s = &spheres[h - 1];

		n = Scale(Sub(p, s->c), Div(FONE, s->r));
		base[0] = s->col[0];
		base[1] = s->col[1];
		base[2] = s->col[2];
		refl = s->refl;
	}
	diffuse = Dot(n, light);
	so = Add(p, Scale(n, 64));
	if (diffuse > 0)
		for (i = 0; i < 4; i++)				// Shadow
		{
			V3		oc = Sub(so, spheres[i].c);
			int32_t	b = Dot(oc, light), disc = MUL(b, b) - (Dot(oc, oc) - MUL(spheres[i].r, spheres[i].r));

			if ((disc > 0) && (-b - Sqrt(disc) > 0))
			{
				diffuse = 0;
				break;
			}
		}
	if (diffuse < 0)
		diffuse = 0;
	rd = Sub(d, Scale(n, 2 * Dot(d, n)));
	spec = 0;
	if (diffuse > 0)
	{
		spec = Dot(rd, light);
		if (spec < 0)
			spec = 0;
		for (i = 0; i < 5; i++)				// ^32
			spec = MUL(spec, spec);
		spec = MUL(spec, FX(0.6));
	}
	for (i = 0; i < 3; i++)
		rgb[i] = (MUL(base[i] << 16, FX(0.15) + MUL(FX(0.85), diffuse)) + spec * 255) >> 16;
	if (!depth && (refl > 0))
	{
		int	b[3];

		Trace(so, rd, 1, b);
		for (i = 0; i < 3; i++)
			rgb[i] = (MUL(rgb[i] << 16, FONE - refl) + MUL(b[i] << 16, refl)) >> 16;
	}
}

static void	RayCamera(void)
{
	int32_t	cy = icos(cam_yaw) << 4, sy = isin(cam_yaw) << 4, cp = icos(cam_pitch) << 4, sp = isin(cam_pitch) << 4;
	int32_t	d = cam_dist << 8;
	V3		target = {0, FX(0.8), 0}, up = {0, FONE, 0};

	cam_o.x = MUL(MUL(d, cp), sy);
	cam_o.y = target.y + MUL(d, sp);
	cam_o.z = MUL(MUL(d, cp), cy);
	cam_f = Normalize(Sub(target, cam_o));
	cam_r = Normalize(Cross(cam_f, up));
	cam_u = Cross(cam_r, cam_f);
}

static uint16_t	row_pix[CP_RAY_W];
static int		alone_row, alone_start_frame, alone_frame_s, alone_done;

static void	RayAloneRestart(void)
{
	alone_row = 0;
	alone_done = 0;
	alone_start_frame = frame;
	RayCamera();
}

// Rows of the PS1's own frame until the next vertical blank (at least one a frame)
static void	RayAloneRows(void)
{
	int32_t	tanv = FX(0.57735), aspect = FX(320.0 / 224.0);
	int		v = VSync(-1), x, rgb[3];
	RECT	rect;

	if (alone_done)
		return;
	do
	{
		int32_t	py = ((alone_row * 2 + 1) << 16) / CP_RAY_H - FONE;

		for (x = 0; x < CP_RAY_W; x++)
		{
			int32_t	pxf = ((x * 2 + 1) << 16) / CP_RAY_W - FONE;
			V3		d = Add(cam_f, Sub(Scale(cam_r, MUL(MUL(pxf, tanv), aspect)), Scale(cam_u, MUL(py, tanv))));

			Trace(cam_o, Normalize(d), 0, rgb);
			if (rgb[0] > 255)	rgb[0] = 255;
			if (rgb[1] > 255)	rgb[1] = 255;
			if (rgb[2] > 255)	rgb[2] = 255;
			if (rgb[0] < 0)		rgb[0] = 0;
			if (rgb[1] < 0)		rgb[1] = 0;
			if (rgb[2] < 0)		rgb[2] = 0;
			row_pix[x] = 0x8000 | (rgb[0] >> 3) | ((rgb[1] >> 3) << 5) | ((rgb[2] >> 3) << 10);
		}
		setRECT(&rect, TEX_X, TEX_Y + alone_row, CP_RAY_W, 1);
		LoadImage(&rect, (const uint32_t *)row_pix);
		DrawSync(0);
		if (++alone_row >= CP_RAY_H)
		{
			alone_done = 1;
			alone_frame_s = (frame - alone_start_frame) * 10 / (is_pal ? 50 : 60);	// (tenths of a second)
			break;
		}
	} while (VSync(-1) == v);
	ray_w = CP_RAY_W;
	ray_h = CP_RAY_H;
}

/****************************************/
// Screens

static uint32_t	pad, opad;
#define	PRESSED(_b_)	((pad & (_b_)) && !(opad & (_b_)))

enum {S_MENU, S_SWARM, S_RAY};
static int	screen = S_MENU, menu_item, fps, fps_count, second_frame;
// For tests, set from the computer (ps1transfer -poke): 1 starts the swarm, 2 the ray tracer, as if chosen on the menu
int			auto_start, auto_orbit;		// (auto_orbit: the ray tracer's camera turned by this much a frame)

static void	Bar(const char *left, int mode_pc)
{
	Rect(L_PANEL, 0, BAR_Y, SCREEN_W, SCREEN_H - BAR_Y, C_BAR);
	Rect(L_PANEL, 0, BAR_Y, 56, SCREEN_H - BAR_Y, mode_pc ? 40 : 90, mode_pc ? 110 : 50, mode_pc ? 60 : 50);
	Text(L_TEXT, 4, BAR_Y + 4, mode_pc ? "PS1+PC" : "PS1   ", C_TEXT);
	Text(L_TEXT, 60, BAR_Y + 4, left, C_TEXT);
}

static const char	*LinkName(void)
{
	if (link_kind == LINK_NONE)
		return ("NO PS1 DEV CART, NOT IN PCSX-REDUX");
	if (!Connected())
		return ("COMPUTER NOT CONNECTED (RUN.BAT)");
	return ((link_kind == LINK_CART) ? "COMPUTER CONNECTED (USB)" : "COMPUTER CONNECTED (PCSX-REDUX)");
}

static void	ScreenMenu(void)
{
	static const char *const	items[2] = {"SWARM: 1000 UNITS", "RAY TRACER"};
	int							i;

	if (PRESSED(PADLup) || PRESSED(PADLdown))
		menu_item ^= 1;
	Rect(L_PANEL, 0, 0, SCREEN_W, 14, C_BAR);
	Text(L_TEXT, 8, 4, "PS1 CO-PROCESSOR", C_TEXT);
	CentreText(L_TEXT, 28, "THE COMPUTER DOES THE HEAVY WORK,", C_TEXT);
	CentreText(L_TEXT, 38, "THE PLAYSTATION SHOWS IT", C_TEXT);
	for (i = 0; i < 2; i++)
	{
		Rect(L_PANEL, 70, 64 + i * 28, 180, 22, (i == menu_item) ? 50 : 20, (i == menu_item) ? 70 : 24,
			(i == menu_item) ? 130 : 40);
		CentreText(L_TEXT, 71 + i * 28, items[i], C_TEXT);
	}
	CentreText(L_TEXT, 136, menu_item ? "SPHERES, REFLECTIONS AND SHADOWS" : "PATHS AROUND WALLS, IN A CROWD", C_DIM);
	CentreText(L_TEXT, 164, "SELECT IN A SCENE: PS1 ALONE / PS1+PC", C_YELLOW);
	CentreText(L_TEXT, 192, LinkName(), Connected() ? 80 : 128, Connected() ? 128 : 70, Connected() ? 80 : 70);
	Rect(L_PANEL, 0, SCREEN_H - 14, SCREEN_W, 14, C_PANEL);
	Text(L_TEXT, 8, SCREEN_H - 11, "UP/DOWN:CHOOSE  X:START", C_TEXT);
	if (auto_start)
		menu_item = auto_start - 1;
	if (PRESSED(PADRdown) || PRESSED(PADstart) || auto_start)
	{
		auto_start = 0;
		screen = menu_item ? S_RAY : S_SWARM;
		if (screen == S_SWARM)
		{
			SwarmWalls();
			SwarmSpawn();
			SwarmField();
			swarm_pc = 0;
			if (swarm_wanted_pc && Connected())
				SwarmToComputer();
		}
		else
		{
			ray_full = 0;
			ray_inflight = 0;
			ray_quality = 0;
			req_yaw = -1;
			ray_w = 0;
			cam_moved_frame = frame;
			RayAloneRestart();
		}
	}
}

static void	ScreenSwarm(void)
{
	int		i, x, y, step = (pad & PADR1) ? 2 : 1, changed_walls = 0;
	char	s[48];

	// Controls
	if (!(frame & 1) || PRESSED(PADLleft | PADLright | PADLup | PADLdown))
	{
		if (pad & PADLleft)		cursor_x -= step;
		if (pad & PADLright)	cursor_x += step;
		if (pad & PADLup)		cursor_y -= step;
		if (pad & PADLdown)		cursor_y += step;
	}
	if (cursor_x < 1)				cursor_x = 1;
	if (cursor_x > CP_GRID_W - 3)	cursor_x = CP_GRID_W - 3;
	if (cursor_y < 1)				cursor_y = 1;
	if (cursor_y > CP_GRID_H - 3)	cursor_y = CP_GRID_H - 3;
	if (PRESSED(PADRdown) && !walls[cursor_y * CP_GRID_W + cursor_x])
	{
		target_x = cursor_x;
		target_y = cursor_y;
		SwarmField();
		if (swarm_pc)
		{
			uint8_t	g[2] = {target_x, target_y};

			Send(CP_SWARM_GOAL, g, 2);
		}
	}
	if (pad & PADRleft)						// Square: walls drawn (2 x 2), or rubbed out if the first one was a wall
	{
		if (paint_value < 0)
			paint_value = !walls[cursor_y * CP_GRID_W + cursor_x];
		for (y = cursor_y; y < cursor_y + 2; y++)
			for (x = cursor_x; x < cursor_x + 2; x++)
				if ((walls[y * CP_GRID_W + x] != paint_value) && !((x == target_x) && (y == target_y)))
				{
					walls[y * CP_GRID_W + x] = paint_value;
					changed_walls = 1;
				}
	}
	else
		paint_value = -1;
	if (changed_walls)
	{
		SwarmField();
		if (swarm_pc)
			SendWalls(CP_SWARM_WALLS);
	}
	if (PRESSED(PADRup))
	{
		SwarmWalls();
		if (swarm_pc)
			SwarmToPs1();
		SwarmSpawn();
		SwarmField();
		if (swarm_wanted_pc && Connected())
			SwarmToComputer();
	}
	if (PRESSED(PADselect))
	{
		swarm_wanted_pc = !swarm_wanted_pc;
		if (swarm_pc)
			SwarmToPs1();
	}
	if (swarm_pc && !Connected())
		SwarmToPs1();
	else if (!swarm_pc && swarm_wanted_pc && Connected())	// (the computer connected meanwhile)
		SwarmToComputer();
	if (PRESSED(PADstart))
	{
		if (swarm_pc)
			SwarmToPs1();
		screen = S_MENU;
		return;
	}

	if (!swarm_pc)
		SwarmStep();

	// Walls (runs of a row), the target, the cursor, the units
	for (y = 0; y < CP_GRID_H; y++)
		for (x = 0; x < CP_GRID_W; x++)
			if (walls[y * CP_GRID_W + x])
			{
				int	x0 = x;

				while ((x < CP_GRID_W) && walls[y * CP_GRID_W + x])
					x++;
				Rect(L_WALL, x0 * CP_CELL, y * CP_CELL, (x - x0) * CP_CELL, CP_CELL, 90, 100, 130);
			}
	Rect(L_MARK, target_x * CP_CELL - 2, target_y * CP_CELL - 2, CP_CELL + 4, CP_CELL + 4, 255, 230, 60);
	Rect(L_MARK, cursor_x * CP_CELL - 1, cursor_y * CP_CELL - 1, 2 * CP_CELL + 2, 1, 255, 255, 255);
	Rect(L_MARK, cursor_x * CP_CELL - 1, cursor_y * CP_CELL + 2 * CP_CELL, 2 * CP_CELL + 2, 1, 255, 255, 255);
	Rect(L_MARK, cursor_x * CP_CELL - 1, cursor_y * CP_CELL, 1, 2 * CP_CELL, 255, 255, 255);
	Rect(L_MARK, cursor_x * CP_CELL + 2 * CP_CELL, cursor_y * CP_CELL, 1, 2 * CP_CELL, 255, 255, 255);
	{
		// (1000 tiles a frame: made here directly, the fraction of the movement worked out once)
		int		t = frame - units_frame, d = units_interval, f;
		TILE	*q = Alloc(sizeof(TILE) * units);

		if (t > d)
			t = d;
		f = t * 256 / d;
		for (i = 0; q && (i < units); i++, q++)
		{
			int	r = unit_shade[i], ux_, uy_;

			if (swarm_pc)
			{
				ux_ = px0[i] + (((px1[i] - px0[i]) * f) >> 8);
				uy_ = py0[i] + (((py1[i] - py0[i]) * f) >> 8);
			}
			else
			{
				ux_ = ux[i] >> 8;
				uy_ = uy[i] >> 8;
			}
			setTile(q);
			setXY0(q, ux_, uy_);
			setWH(q, 2, 2);
			setRGB0(q, r, 255 - r / 2, 120 + r / 3);
			addPrim(OT(L_UNIT), q);
		}
	}

	if (swarm_pc)
		sprintf(s, "PC %d.%dMS %d/S %dKB/S %dFPS", pc_step_us / 1000, (pc_step_us / 100) % 10, units_rate,
			(int)(rx_rate / 1024), fps);
	else
		sprintf(s, "SIM %d.%dMS PATHS %dMS %dFPS", sim_us / 1000, (sim_us / 100) % 10, field_us / 1000, fps);
	Bar(s, swarm_pc);
}

static void	ScreenRay(void)
{
	int		moved = 0, spd = (pad & PADR1) ? 64 : 24;
	char	s[48];

	if (auto_orbit)			{cam_yaw += auto_orbit; moved = 1;}
	if (pad & PADLleft)		{cam_yaw -= spd; moved = 1;}
	if (pad & PADLright)	{cam_yaw += spd; moved = 1;}
	if (pad & PADLup)		{cam_pitch += spd / 2; moved = 1;}
	if (pad & PADLdown)		{cam_pitch -= spd / 2; moved = 1;}
	if (pad & PADRup)		{cam_dist -= 24; moved = 1;}
	if (pad & PADRdown)		{cam_dist += 24; moved = 1;}
	if (cam_pitch < 60)		cam_pitch = 60;
	if (cam_pitch > 900)	cam_pitch = 900;
	if (cam_dist < 3 * 256)	cam_dist = 3 * 256;
	if (cam_dist > 12 * 256)	cam_dist = 12 * 256;
	if (moved)
	{
		cam_moved_frame = frame;
		ray_full = 0;
		RayAloneRestart();
	}
	if (PRESSED(PADselect))
	{
		ray_wanted_pc = !ray_wanted_pc;
		ray_full = 0;
		ray_inflight = 0;
		ray_quality = 0;
		req_yaw = -1;
		RayAloneRestart();
	}
	if (PRESSED(PADstart))
	{
		screen = S_MENU;
		return;
	}
	if (ray_pc != (ray_wanted_pc && Connected()))		// (the computer connected or gone meanwhile)
	{
		ray_pc = ray_wanted_pc && Connected();
		ray_full = 0;
		ray_inflight = 0;
		ray_quality = 0;
		req_yaw = -1;
		RayAloneRestart();
	}

	if (ray_pc)
	{
		// One request at a time, with the camera of now: a new view while it moves (160 x 112), the still frame once
		// it has stopped (320 x 224, 4 rays a pixel)
		if (ray_inflight && (frame - ray_request_frame > 50 * 3))
			ray_inflight = 0;
		if (!ray_inflight)
		{
			int		changed = (cam_yaw != req_yaw) || (cam_pitch != req_pitch) || (cam_dist != req_dist);
			int		still = frame - cam_moved_frame > 15, quality = -1;
			uint8_t	r[9];

			if (changed)
				quality = still;
			else if (still && !ray_quality && !ray_full)
				quality = 1;
			if (quality >= 0)
			{
				ray_id = (ray_id + 1) & 0xFFFF;
				ray_quality = quality;
				req_yaw = cam_yaw;
				req_pitch = cam_pitch;
				req_dist = cam_dist;
				still = quality;
				r[0] = ray_id;
				r[1] = ray_id >> 8;
				r[2] = still;
				r[3] = cam_yaw;
				r[4] = cam_yaw >> 8;
				r[5] = cam_pitch;
				r[6] = cam_pitch >> 8;
				r[7] = cam_dist;
				r[8] = cam_dist >> 8;
				Send(CP_RAY, r, 9);
				ray_inflight = 1;
				ray_request_frame = frame;
			}
		}
	}
	else
		RayAloneRows();

	// The frame: scaled up when it is 160 x 112; a 320 x 224 one in two texture pages (256 + 64 pixels)
	if (ray_w)
	{
		int	half = (ray_w == CP_RAY_W), part, parts = half ? 1 : 2;

		for (part = 0; part < parts; part++)
		{
			POLY_FT4	*q = Alloc(sizeof(POLY_FT4));
			int			u0 = part * 256, w = half ? ray_w : (part ? 64 : 256);
			int			x0 = half ? 0 : u0, x1 = half ? SCREEN_W : u0 + w, y1 = VIEW_H;
			int			th = half ? ray_h : ray_h;

			if (!q)
				break;
			setPolyFT4(q);
			setShadeTex(q, 1);
			setXY4(q, x0, 0, x1, 0, x0, y1, x1, y1);
			setUV4(q, 0, 0, w - 1, 0, 0, th - 1, w - 1, th - 1);
			q->tpage = getTPage(2, 0, TEX_X + u0, TEX_Y);
			q->clut = 0;
			addPrim(OT(L_IMAGE), q);
		}
	}
	if (!ray_pc && !alone_done)
	{
		sprintf(s, "ROW %d/%d", alone_row, CP_RAY_H);
		Rect(L_PANEL, 0, alone_row * 2, SCREEN_W, 1, 255, 230, 60);
	}

	if (ray_pc && (ray_w == 320))			// (the still frame)
		sprintf(s, "PC %dMS 320X224 %dKB 4 RAYS/PX", ray_render_ms + ray_encode_ms, ray_bytes / 1024);
	else if (ray_pc)
		sprintf(s, "PC %dMS %dX%d %dKB %dFR/S", ray_render_ms + ray_encode_ms, ray_w, ray_h, ray_bytes / 1024, ray_rate);
	else if (alone_done)
		sprintf(s, "160X112 IN %d.%d S  %dFPS", alone_frame_s / 10, alone_frame_s % 10, fps);
	else
		sprintf(s, "RAY TRACING ROW %d/%d  %dFPS", alone_row, CP_RAY_H, fps);
	Bar(s, ray_pc);
}

static void	Packet(const uint8_t *p, int n)
{
	switch (type)
	{
		case CP_WELCOME:
			if ((n >= 1) && (p[0] == CP_VERSION))
				welcomed = 1;
			break;
		case CP_UNITS:
		case CP_MOVES:
			UnitsPacket(p, n, type == CP_MOVES);
			break;
		case CP_FRAME:
			FramePacket(p, n);
			break;
	}
}

/****************************************/

int	main(void)
{
	int	i, last_hello = -100;

	is_pal = (*((char *)0xBFC7FF52) == 'E');
	ResetGraph(0);	// (first: PSn00bSDK's ResetGraph sets up the callbacks, see psyq_compat.h)
	SetVideoMode(is_pal ? MODE_PAL : MODE_NTSC);
	PadInit(0);
	for (i = 0; i < 2; i++)
	{
		SetDefDispEnv(&disp[i], 0, i ? 0 : SCREEN_H, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&draw[i], 0, i ? SCREEN_H : 0, SCREEN_W, SCREEN_H);
		draw[i].isbg = 1;
		SET_RGB(&draw[i], C_BG);
	}
	FntLoad(960, 256);
	SetDispMask(1);
	InitGeom();		// (the MDEC bitstream decompressor uses the GTE)
	MdecInit();
	DecDCTvlcCopyTableV2((VLC_TableV2 *)0x1F800000);	// Its table in the scratchpad (faster)
	srand(*(volatile uint16_t *)0x1F801100);
	{
		V3	l = {FX(-0.4), FX(1.0), FX(0.5)};

		light = Normalize(l);
	}

	LinkOpen();

	while (1)
	{
		LinkReceive();
		opad = pad;
		pad = PadRead(1) & 0xFFFF;
		prim_ptr = prims[side];
		ClearOTagR(ot[side], LAYERS);

		switch (screen)
		{
			case S_MENU:	ScreenMenu();	break;
			case S_SWARM:	ScreenSwarm();	break;
			case S_RAY:		ScreenRay();	break;
		}
		// Hello each second: the welcome back keeps the connection known (and PCSX-Redux connects before the program
		// runs: the first welcome may be missed)
		if ((link_kind != LINK_NONE) && (frame - last_hello >= 50))
		{
			uint8_t	h[1] = {CP_VERSION};

			Send(CP_HELLO, h, 1);
			last_hello = frame;
		}
		LinkFlush();

		// Every second: frame rate, link rate, updates a second
		fps_count++;
		if (frame - second_frame >= (is_pal ? 50 : 60))
		{
			fps = fps_count * (is_pal ? 50 : 60) / (frame - second_frame);
			fps_count = 0;
			rx_rate = rx_bytes - rx_rate_bytes;
			rx_rate_bytes = rx_bytes;
			units_rate = units_count_second;
			units_count_second = 0;
			ray_rate = ray_frames;
			ray_frames = 0;
			second_frame = frame;
		}

		DrawSync(0);
		VSync(0);
		PutDispEnv(&disp[side]);
		PutDrawEnv(&draw[side]);
		DrawOTag(&ot[side][LAYERS - 1]);
		side ^= 1;
		frame = VSync(-1);		// (frames counted in vertical blanks: a slow frame counts as several)
	}

	return (0);
}
