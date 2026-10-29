/*
* PS1 PLAZA: a small online game for up to 24 players on one internet server, a demo of what an online or MMO-style game
* can do with the PS1 DEV CART.
*
* Each player is a coloured shape (4 shapes x 6 colours, one player each) moving around a world of 4 x 3 screens. The
* tiles you move over take your colour and pattern: the player with the most tiles when the round ends wins. The list
* on the left shows everyone connected, the minimap the whole world. Quick chat on the face buttons.
*
* The computer runs "ps1transfer -r -e -f plaza.ps-exe -net play.logi26.co.uk:7020" (run.bat): the bridge between this
* program's psxcomb USB link and the game server on the internet. Protocol: plaza.h.
* The link is read and written without waiting (synchronous psxcomb calls with a wait callback that stops at once:
* what has arrived, what fits in the ring), so the game never stops for the network.
*
* Screens: connecting, name (on-screen keyboard), shape and colour (taken ones greyed, a free one preselected), game.
* Own movement is shown at once and sent ~17 times a second; the other players move smoothly to the positions the
* server sends.
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
#include <psxetc.h>
#include <psxapi.h>
#include "psyq_compat.h"
#include "psxcomb/psxcomb.h"
#include "plaza.h"

#define	SCREEN_W		320
#define	SCREEN_H		240

// Game screen: the player list on the left, the world on the right
#define	PANEL_W			128
#define	VIEW_X			PANEL_W
#define	VIEW_W			(SCREEN_W - PANEL_W)
#define	VIEW_H			SCREEN_H
#define	WORLD_PX_W		(PZ_WORLD_W * PZ_TILE)
#define	WORLD_PX_H		(PZ_WORLD_H * PZ_TILE)
#define	TILES			(PZ_WORLD_W * PZ_WORLD_H)

// Minimap: one pixel per 2 x 2 tiles, a 16-bit texture next to the frame buffers
#define	MINI_W			(PZ_WORLD_W / 2)
#define	MINI_H			(PZ_WORLD_H / 2)
#define	MINI_ROWS		48			// Rows uploaded: VRAM is loaded by DMA in blocks of 64 bytes (48 x 45 isn't)
#define	MINI_VRAM_X		320
#define	MINI_VRAM_Y		0
#define	MINI_X			(SCREEN_W - MINI_W - 4)
#define	MINI_Y			4

#define	SHAPE_R			6			// Radius of a player's shape
#define	BUBBLE_FRAMES	150			// Quick chat shown for 3 seconds (PAL)

// Colours (as the PS1 DEV CART tools)
#define	C_BG			14, 16, 28
#define	C_BAR			40, 50, 120
#define	C_PANEL			20, 24, 40
#define	C_SELECT		50, 70, 130
#define	C_WORLD			28, 32, 48
#define	C_TEXT			128, 128, 128	// (text tint: 128 = as the font)
#define	C_DIM			60, 60, 70
#define	C_YELLOW		128, 110, 30
#define	C_ERROR			230, 70, 70

// setRGB0 with a colour above (expanded into its 3 values first)
#define	SET_RGB(_p_, _c_)			_SET_RGB(_p_, _c_)
#define	_SET_RGB(_p_, _r_, _g_, _b_)	setRGB0(_p_, _r_, _g_, _b_)

static const uint8_t	colours[PZ_COLOURS][3] =
{
	{230, 60, 60}, {235, 215, 40}, {60, 205, 80}, {50, 205, 225}, {70, 100, 245}, {215, 70, 210}
};
static const char *const	colour_names[PZ_COLOURS] = {"RED", "YELLOW", "GREEN", "CYAN", "BLUE", "MAGENTA"};
static const char *const	shape_names[PZ_SHAPES] = {"CIRCLE", "SQUARE", "TRIANGLE", "DIAMOND"};
static const char *const	phrases[PZ_PHRASES] = {"HI!", "GG!", "FOLLOW ME!", "OVER HERE!"};

/****************************************/
// Drawing: one ordering table, drawn by layers (each layer above the ones before)

enum
{
	L_WORLD, L_PAINT, L_MARK, L_OUTLINE, L_SHAPE, L_TAG_BG, L_TAG, L_MINI_BG, L_MINI, L_MINI_DOT, L_PANEL, L_PANEL_FG,
	L_PANEL_TEXT, L_OVERLAY, L_OVERLAY_TEXT, LAYERS
};

#define	PRIM_BYTES		65536

static DISPENV		disp[2];
static DRAWENV		draw[2];
static uint32_t		ot[2][LAYERS];
static uint8_t		prims[2][PRIM_BYTES];
static uint8_t		*prim_ptr;
static int			side;

static void	*Alloc(int size)
{
	void	*p;

	if (prim_ptr + size > prims[side] + PRIM_BYTES - 64)
		return (NULL);
	p = prim_ptr;
	prim_ptr += size;
	return (p);
}

#define	OT(_layer_)		(&ot[side][LAYERS - 1 - (_layer_)])

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

static void	Tri(int layer, int x0, int y0, int x1, int y1, int x2, int y2, int r, int g, int b)
{
	POLY_F3	*p = Alloc(sizeof(POLY_F3));

	if (!p)
		return;
	setPolyF3(p);
	setXY3(p, x0, y0, x1, y1, x2, y2);
	setRGB0(p, r, g, b);
	addPrim(OT(layer), p);
}

static void	Quad(int layer, int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3, int r, int g, int b)
{
	POLY_F4	*p = Alloc(sizeof(POLY_F4));

	if (!p)
		return;
	setPolyF4(p);
	setXY4(p, x0, y0, x1, y1, x2, y2, x3, y3);
	setRGB0(p, r, g, b);
	addPrim(OT(layer), p);
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

// Circle: a fan of 12 triangles (unit circle * 256)
static const int16_t	circle[13][2] =
{
	{256, 0}, {222, 128}, {128, 222}, {0, 256}, {-128, 222}, {-222, 128}, {-256, 0}, {-222, -128}, {-128, -222},
	{0, -256}, {128, -222}, {222, -128}, {256, 0}
};

static void	Shape(int layer, int shape, int x, int y, int rad, int r, int g, int b)
{
	int	i;

	switch (shape)
	{
		case 0:
			for (i = 0; i < 12; i++)
				Tri(layer, x, y, x + circle[i][0] * rad / 256, y + circle[i][1] * rad / 256,
					x + circle[i + 1][0] * rad / 256, y + circle[i + 1][1] * rad / 256, r, g, b);
			break;
		case 1:
			Rect(layer, x - rad + 1, y - rad + 1, rad * 2 - 1, rad * 2 - 1, r, g, b);
			break;
		case 2:
			Tri(layer, x, y - rad - 1, x - rad - 1, y + rad, x + rad + 1, y + rad, r, g, b);
			break;
		default:
			Quad(layer, x, y - rad - 1, x - rad - 1, y, x + rad + 1, y, x, y + rad + 1, r, g, b);
			break;
	}
}

static void	ShapeOf(int layer, int id, int x, int y, int rad, int dim)
{
	const uint8_t	*c = colours[id % PZ_COLOURS];

	if (dim)
		Shape(layer, id / PZ_COLOURS, x, y, rad, C_DIM);
	else
		Shape(layer, id / PZ_COLOURS, x, y, rad, c[0], c[1], c[2]);
}

/****************************************/
// Link: on a console, psxcomb's USB link, bridged to the server by "ps1transfer -net"; in PCSX-Redux, its serial port
// (SIO1) connected to the server as a TCP client in raw mode (the same bytes, no bridge)

enum {LINK_NONE, LINK_CART, LINK_EMULATOR};

static int		link_kind = LINK_NONE;
static long		link_fd = -1;
static uint8_t	tx[2048];
static int		tx_n;
static uint8_t	rx[1024];

#define	PCSX_MAGIC		(*(volatile uint32_t *)0x1F802080)	// PCSX-Redux: reads "PCSX"
#define	HOOK_USB_PROCESS	(*(volatile uint32_t *)0x8000DF74)	// PS1 DEV CART kernel hook: its USB service

// SIO1 registers
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

// Received bytes, from the serial interrupt (the controller only holds 8)
#define	SIO_RING		4096
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
		// 8 data bits, 1 stop bit, no parity, 115200 bps (about 10 KB/s: the game needs about 3)
		link_kind = LINK_EMULATOR;
		SIO1_CTRL = SIO_RESET;
		SIO1_MODE = 0x4E;
		SIO1_BAUD = 2116800 / 115200;
		EnterCriticalSection();
		InterruptCallback(IRQ_SIO1, SioIrq);
		ExitCriticalSection();
		SIO1_CTRL = SIO_CTRL_ON | SIO_ACK;
		return;
	}
	if (HOOK_USB_PROCESS - 0x8000C000 >= 0x2000)		// No PS1 DEV CART firmware
		return;
	link_kind = LINK_CART;
	CombSetLink(COMB_LINK_USB);		// (before AddCOMB: the rings are published at once)
	AddCOMB();
	link_fd = open("sio:", O_RDWR);
	CombWaitCallback(NoWait);
}

static void	Send(int type, const void *payload, int n)
{
	if (tx_n + PZ_HEADER + n > (int)sizeof(tx))
		return;		// (the bridge isn't taking it: dropped)
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
		// (a byte takes 87 us at 115200 bps: at most 512 bytes a frame, a frame's messages are about 20)
		while ((n < tx_n) && (n < 512))
		{
			int	wait = 100000;

			while (!(SIO1_STAT & SIO_TXRDY) && --wait)
				;
			if (!wait)
				break;
			SIO1_DATA = tx[n++];
		}
	}
	else if ((link_fd < 0) || !tx_n)
		return;
	else
		n = write(link_fd, tx, tx_n);
	if (n > 0)
	{
		memmove(tx, tx + n, tx_n - n);
		tx_n -= n;
	}
}

/****************************************/
// Game state

enum {S_CONNECT, S_NAME, S_PICK, S_JOINING, S_GAME, S_LOST};

typedef struct
{
	int		present;
	char	name[PZ_NAME + 1];
	int		score;
	int		x, y;			// Last position from the server (world pixels)
	int		dx, dy;			// Shown (moves smoothly to x, y), * 16
	int		seen;			// A position has arrived
	int		chat, chat_until;
} Player;

static Player	players[PZ_PLAYERS];
static uint8_t	board[TILES];
static int		state = S_CONNECT, frame, is_pal;
static int		me = -1, my_x, my_y;		// Own position * 256 (world pixels)
static int		round_left, round_winner = 0xFF, banner_until;
static int		ping_ms = -1, last_rx_frame, welcomed, server_tick;
char			name[PZ_NAME + 1];
static int		pick_cursor = -1, message_until, join_frame;
static const char	*message = "";
static int		show_names = 1, show_minimap = 1;
// For tests, set from the computer (ps1transfer -u / -poke) before the bridge starts: the name in "name" is used and
// the preselected shape and colour joined, without the controller
int				auto_join;
static int		mini_dirty = 1;

static int	Count(void)
{
	int	i, n = 0;

	for (i = 0; i < PZ_PLAYERS; i++)
		n += players[i].present;
	return (n);
}

static void	Message(const char *s)
{
	message = s;
	message_until = frame + 150;
}

static int	FreeId(void)
{
	int	i, free[PZ_PLAYERS], n = 0;

	for (i = 0; i < PZ_PLAYERS; i++)
		if (!players[i].present)
			free[n++] = i;
	return (n ? free[(frame * 7 + rand()) % n] : -1);
}

/****************************************/
// Packets from the server

static uint8_t	hdr[PZ_HEADER];
static int		hdr_n, type, left, got;
static uint8_t	payload[1024];

#define	U16(_p_)	((_p_)[0] | ((_p_)[1] << 8))

static void	SetTile(int index, int owner)
{
	if ((index < 0) || (index >= TILES) || (board[index] == owner))
		return;
	board[index] = owner;
	mini_dirty = 1;
}

static void	Packet(const uint8_t *p, int n)
{
	int	i, id;

	switch (type)
	{
		case PZ_WELCOME:
			if ((n >= 6) && (p[0] == PZ_VERSION))
			{
				// (again: a new connection, e.g. the bridge started again: the server has forgotten us, the players
				// are sent again, the shape and colour chosen again)
				welcomed = 1;
				server_tick = p[5];
				memset(players, 0, sizeof(players));
				if ((state == S_CONNECT) || ((state == S_LOST) && !name[0]))
					state = S_NAME;
				else if ((state == S_GAME) || (state == S_LOST))
				{
					state = S_PICK;
					pick_cursor = -1;
					me = -1;
				}
			}
			else
			{
				Message("THIS GAME IS TOO OLD FOR THE SERVER");
				state = S_LOST;
			}
			break;
		case PZ_PLAYER:
			if ((n >= 3) && (p[0] < PZ_PLAYERS))
			{
				Player	*pl = &players[p[0]];

				pl->present = 1;
				pl->score = U16(p + 1);
				n -= 3;
				if (n > PZ_NAME)
					n = PZ_NAME;
				memcpy(pl->name, p + 3, n);
				pl->name[n] = 0;
				pl->seen = 0;
			}
			break;
		case PZ_GONE:
			if ((n >= 1) && (p[0] < PZ_PLAYERS))
			{
				players[p[0]].present = 0;
				for (i = 0; i < TILES; i++)
					if (board[i] == p[0] + 1)
						board[i] = 0;
				mini_dirty = 1;
			}
			break;
		case PZ_ACCEPT:		// (whatever the screen: the server has us in the game)
			if ((n >= 5) && (p[0] < PZ_PLAYERS))
			{
				me = p[0];
				my_x = U16(p + 1) << 8;
				my_y = U16(p + 3) << 8;
				state = S_GAME;
			}
			break;
		case PZ_REFUSE:
			if (state == S_JOINING)
			{
				state = (p[0] == PZ_BAD_NAME) ? S_NAME : S_PICK;
				Message((p[0] == PZ_TAKEN) ? "TAKEN JUST NOW: CHOOSE ANOTHER" :
					((p[0] == PZ_FULL) ? "THE SERVER IS FULL" : "TRY ANOTHER NAME"));
				pick_cursor = -1;
			}
			break;
		case PZ_TILES:
			for (i = 0; i + 3 <= n; i += 3)
				SetTile(U16(p + i), p[i + 2]);
			break;
		case PZ_POSITIONS:
			for (i = 0; i + 5 <= n; i += 5)
			{
				Player	*pl;

				id = p[i];
				if ((id >= PZ_PLAYERS) || (id == me))
					continue;
				pl = &players[id];
				pl->x = U16(p + i + 1);
				pl->y = U16(p + i + 3);
				// Far from where it is shown (just seen, or out of view for a while): there at once
				if (!pl->seen || (abs(pl->x * 16 - pl->dx) > 96 * 16) || (abs(pl->y * 16 - pl->dy) > 96 * 16))
				{
					pl->dx = pl->x * 16;
					pl->dy = pl->y * 16;
				}
				pl->seen = 1;
			}
			break;
		case PZ_FORCE:
			if (n >= 4)
			{
				my_x = U16(p) << 8;
				my_y = U16(p + 2) << 8;
			}
			break;
		case PZ_SAID:
			if ((n >= 2) && (p[0] < PZ_PLAYERS) && (p[1] < PZ_PHRASES))
			{
				players[p[0]].chat = p[1];
				players[p[0]].chat_until = frame + BUBBLE_FRAMES;
			}
			break;
		case PZ_SCORES:
			for (i = 0; i + 3 <= n; i += 3)
				if (p[i] < PZ_PLAYERS)
					players[p[i]].score = U16(p + i + 1);
			break;
		case PZ_ROUND:
			if (n >= 4)
			{
				round_left = U16(p);
				if (p[3])		// New round: the board is cleared, the winner shown
				{
					memset(board, 0, sizeof(board));
					mini_dirty = 1;
					round_winner = p[2];
					banner_until = frame + 250;
				}
			}
			break;
		case PZ_PONG:
			if (n >= 2)
				ping_ms = ((uint16_t)(VSync(-1) - U16(p))) * (is_pal ? 20 : 17);
			break;
	}
}

// The board ('B') goes straight to its place, any length; other payloads up to sizeof(payload)
static void	Parse(const uint8_t *p, int n)
{
	while (n)
	{
		if (hdr_n < PZ_HEADER)
		{
			hdr[hdr_n++] = *p++;
			n--;
			if (hdr_n < PZ_HEADER)
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
			if (type == PZ_BOARD)
			{
				if (got < TILES)
					board[got] = *p;
			}
			else if (got < (int)sizeof(payload))
				payload[got] = *p;
			got++;
			p++;
			n--;
			left--;
		}
		if (!left)
		{
			if (type == PZ_BOARD)
				mini_dirty = 1;
			else
				Packet(payload, (got < (int)sizeof(payload)) ? got : (int)sizeof(payload));
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
			Parse(rx, n);
		}
		return;
	}
	// A few reads per frame (the board arrives in about half a second)
	for (i = 0; (i < 16) && (link_fd >= 0); i++)
	{
		n = read(link_fd, rx, sizeof(rx));
		if (n <= 0)
			break;
		last_rx_frame = frame;
		Parse(rx, n);
		if (n < (long)sizeof(rx))
			break;
	}
}

/****************************************/
// Screens

static uint32_t	pad, opad;
#define	PRESSED(_b_)	((pad & (_b_)) && !(opad & (_b_)))

static void	TitleBar(const char *right)
{
	Rect(L_PANEL, 0, 0, SCREEN_W, 14, C_BAR);
	Text(L_PANEL_TEXT, 8, 4, "PS1 PLAZA", C_TEXT);
	if (right)
		Text(L_PANEL_TEXT, SCREEN_W - 8 - strlen(right) * 8, 4, right, C_TEXT);
}

static void	HelpBar(const char *s)
{
	Rect(L_PANEL, 0, SCREEN_H - 14, SCREEN_W, 14, C_PANEL);
	Text(L_PANEL_TEXT, 8, SCREEN_H - 11, s, C_TEXT);
}

static void	CentreText(int layer, int y, const char *s, int r, int g, int b)
{
	Text(layer, (SCREEN_W - strlen(s) * 8) / 2, y, s, r, g, b);
}

static void	ShowMessage(int y)
{
	if (frame < message_until)
		CentreText(L_PANEL_TEXT, y, message, C_ERROR);
}

static void	ScreenConnect(void)
{
	char	s[40];

	TitleBar(state == S_LOST ? "OFFLINE" : "CONNECTING");
	if (state == S_LOST)
	{
		CentreText(L_PANEL_TEXT, 100, "CONNECTION TO THE SERVER LOST", C_ERROR);
		if (link_kind == LINK_EMULATOR)
			CentreText(L_PANEL_TEXT, 116, "RESTART PCSX-REDUX TO REJOIN", C_TEXT);
		else
			CentreText(L_PANEL_TEXT, 116, "START RUN.BAT AGAIN TO REJOIN", C_TEXT);
		ShowMessage(140);
		return;
	}
	if (link_kind == LINK_NONE)
	{
		CentreText(L_PANEL_TEXT, 84, "NO WAY TO THE INTERNET FOUND", C_ERROR);
		CentreText(L_PANEL_TEXT, 108, "ON A PLAYSTATION: A PS1 DEV CART", C_TEXT);
		CentreText(L_PANEL_TEXT, 120, "AND RUN.BAT ON THE COMPUTER", C_TEXT);
		CentreText(L_PANEL_TEXT, 144, "IN AN EMULATOR: PCSX-REDUX WITH", C_TEXT);
		CentreText(L_PANEL_TEXT, 156, "ITS SERIAL PORT AS A RAW CLIENT", C_TEXT);
		CentreText(L_PANEL_TEXT, 168, "TO PLAY.LOGI26.CO.UK:7020", C_TEXT);
		return;
	}
	CentreText(L_PANEL_TEXT, 100, "WAITING FOR THE SERVER", C_TEXT);
	strcpy(s, "   ");
	memset(s, '.', (frame / 20) % 4);
	CentreText(L_PANEL_TEXT, 116, s, C_TEXT);
	if (link_kind == LINK_EMULATOR)
	{
		CentreText(L_PANEL_TEXT, 150, "PCSX-REDUX: SERIAL PORT (SIO1) CLIENT,", C_DIM);
		CentreText(L_PANEL_TEXT, 162, "RAW, TO PLAY.LOGI26.CO.UK:7020", C_DIM);
	}
	else
		CentreText(L_PANEL_TEXT, 150, "RUN.BAT ON THE COMPUTER", C_DIM);
}

// On-screen keyboard: 4 rows of 10 keys, then DELETE and OK
static const char	keys[4][11] = {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-_. ", "0123456789"};
static int			key_x, key_y;

static void	NameDone(void)
{
	int	n = strlen(name);

	while (n && (name[n - 1] == ' '))
		name[--n] = 0;
	if (!n)
	{
		Message("TYPE A NAME FIRST");
		return;
	}
	state = S_PICK;
	pick_cursor = -1;
}

static void	ScreenName(void)
{
	int		x, y, n = strlen(name);
	char	s[2] = {0, 0}, shown[PZ_NAME + 2];

	if (PRESSED(PADLleft))	key_x = (key_y == 4) ? 0 : (key_x + 9) % 10;
	if (PRESSED(PADLright))	key_x = (key_y == 4) ? 1 : (key_x + 1) % 10;
	// (the last row has two keys: DELETE on the left half, OK on the right)
	if (PRESSED(PADLup) || PRESSED(PADLdown))
	{
		int	from = key_y;

		key_y = (key_y + (PRESSED(PADLup) ? 4 : 1)) % 5;
		if (key_y == 4)
			key_x = (key_x >= 5);
		else if (from == 4)
			key_x = key_x ? 7 : 2;
	}
	if (PRESSED(PADRup) && n)
		name[n - 1] = 0;
	if (PRESSED(PADRdown))
	{
		if (key_y < 4)
		{
			if (n < PZ_NAME)
			{
				name[n] = keys[key_y][key_x];
				name[n + 1] = 0;
			}
		}
		else if (key_x == 0)
		{
			if (n)
				name[n - 1] = 0;
		}
		else
			NameDone();
	}
	if (PRESSED(PADstart) || (auto_join && name[0]))
		NameDone();

	TitleBar("YOUR NAME");
	Rect(L_PANEL, 70, 30, 180, 22, C_PANEL);
	strcpy(shown, name);
	if ((frame / 15) & 1)
		strcat(shown, "_");
	Text(L_PANEL_TEXT, 82, 37, shown, C_YELLOW);
	for (y = 0; y < 4; y++)
		for (x = 0; x < 10; x++)
		{
			int	sx = 40 + x * 24, sy = 66 + y * 24;

			Rect(L_PANEL, sx, sy, 20, 20, ((x == key_x) && (y == key_y)) ? 50 : 20,
				((x == key_x) && (y == key_y)) ? 70 : 24, ((x == key_x) && (y == key_y)) ? 130 : 40);
			s[0] = keys[y][x];
			Text(L_PANEL_TEXT, sx + ((s[0] == ' ') ? 2 : 6), sy + 6, (s[0] == ' ') ? "SP" : s, C_TEXT);
		}
	Rect(L_PANEL, 40, 166, 116, 20, (key_y == 4) && !key_x ? 50 : 20, (key_y == 4) && !key_x ? 70 : 24,
		(key_y == 4) && !key_x ? 130 : 40);
	Text(L_PANEL_TEXT, 74, 172, "DELETE", C_TEXT);
	Rect(L_PANEL, 164, 166, 116, 20, (key_y == 4) && key_x ? 50 : 20, (key_y == 4) && key_x ? 70 : 24,
		(key_y == 4) && key_x ? 130 : 40);
	Text(L_PANEL_TEXT, 214, 172, "OK", C_TEXT);
	ShowMessage(196);
	HelpBar("X:TYPE  TRIANGLE:DELETE  START:OK");
}

// Shape and colour: 4 rows (shapes) x 6 columns (colours), taken ones greyed with their player's name below
static void	ScreenPick(void)
{
	int		i, x, y, count = Count();
	char	s[48];

	if ((pick_cursor < 0) || (pick_cursor >= PZ_PLAYERS))
		pick_cursor = FreeId();
	if (pick_cursor < 0)
		pick_cursor = 0;
	x = pick_cursor % PZ_COLOURS;
	y = pick_cursor / PZ_COLOURS;
	if (PRESSED(PADLleft))	x = (x + PZ_COLOURS - 1) % PZ_COLOURS;
	if (PRESSED(PADLright))	x = (x + 1) % PZ_COLOURS;
	if (PRESSED(PADLup))	y = (y + PZ_SHAPES - 1) % PZ_SHAPES;
	if (PRESSED(PADLdown))	y = (y + 1) % PZ_SHAPES;
	if ((state == S_JOINING) && (frame - join_frame > 50 * 5))	// No answer (e.g. sent to a connection now closed)
		state = S_PICK;
	if (state == S_JOINING)		// (waiting for the server's answer)
		x = pick_cursor % PZ_COLOURS, y = pick_cursor / PZ_COLOURS;
	pick_cursor = y * PZ_COLOURS + x;
	if (state == S_JOINING)
		;
	else if (PRESSED(PADRright))
		state = S_NAME;
	else if (PRESSED(PADRdown) || PRESSED(PADstart) || (auto_join && !players[pick_cursor].present))
	{
		if (players[pick_cursor].present)
			Message("TAKEN: CHOOSE A BRIGHT ONE");
		else
		{
			uint8_t	j[1 + PZ_NAME];

			j[0] = pick_cursor;
			memcpy(j + 1, name, strlen(name));
			Send(PZ_JOIN, j, 1 + strlen(name));
			state = S_JOINING;
			join_frame = frame;
		}
	}

	sprintf(s, "%d/%d PLAYING", count, PZ_PLAYERS);
	TitleBar(s);
	CentreText(L_PANEL_TEXT, 22, "CHOOSE YOUR SHAPE AND COLOUR", C_TEXT);
	for (i = 0; i < PZ_PLAYERS; i++)
	{
		int	sx = 28 + (i % PZ_COLOURS) * 44, sy = 36 + (i / PZ_COLOURS) * 36;

		Rect(L_PANEL, sx, sy, 40, 32, (i == pick_cursor) ? 50 : 20, (i == pick_cursor) ? 70 : 24,
			(i == pick_cursor) ? 130 : 40);
		ShapeOf(L_PANEL_FG, i, sx + 20, sy + 16, 9, players[i].present);
	}
	if (players[pick_cursor].present)
		sprintf(s, "TAKEN BY %s", players[pick_cursor].name);
	else
		sprintf(s, "%s %s: FREE", colour_names[pick_cursor % PZ_COLOURS], shape_names[pick_cursor / PZ_COLOURS]);
	CentreText(L_PANEL_TEXT, 186, s, players[pick_cursor].present ? 100 : 128, players[pick_cursor].present ? 100 : 110,
		players[pick_cursor].present ? 100 : 30);
	if (state == S_JOINING)
		CentreText(L_PANEL_TEXT, 200, "JOINING...", C_TEXT);
	ShowMessage(200);
	HelpBar("X:JOIN  CIRCLE:NAME  D-PAD:CHOOSE");
}

/****************************************/
// Game

static uint16_t	mini[MINI_ROWS][MINI_W];

static void	MinimapUpdate(void)
{
	RECT	r = {MINI_VRAM_X, MINI_VRAM_Y, MINI_W, MINI_ROWS};
	int		x, y;

	if (!mini_dirty || (frame & 7))
		return;
	mini_dirty = 0;
	for (y = 0; y < MINI_H; y++)
		for (x = 0; x < MINI_W; x++)
		{
			int	o = board[(y * 2) * PZ_WORLD_W + x * 2];

			if (o)
			{
				const uint8_t	*c = colours[(o - 1) % PZ_COLOURS];

				mini[y][x] = 0x8000 | ((c[0] * 5 / 8) >> 3) | (((c[1] * 5 / 8) >> 3) << 5) | (((c[2] * 5 / 8) >> 3) << 10);
			}
			else
				mini[y][x] = 0x8000 | (28 >> 3) | ((32 >> 3) << 5) | ((48 >> 3) << 10);
		}
	LoadImage(&r, (const uint32_t *)mini);
}

static void	DrawMinimap(void)
{
	SPRT		*s;
	DR_TPAGE	*t;
	int			i;

	Rect(L_MINI_BG, MINI_X - 1, MINI_Y - 1, MINI_W + 2, MINI_H + 2, 120, 120, 140);
	s = Alloc(sizeof(SPRT));
	t = Alloc(sizeof(DR_TPAGE));
	if (!s || !t)
		return;
	setSprt(s);
	setShadeTex(s, 1);
	setXY0(s, MINI_X, MINI_Y);
	setWH(s, MINI_W, MINI_H);
	setUV0(s, 0, 0);
	s->clut = 0;
	addPrim(OT(L_MINI), s);
	setDrawTPage(t, 0, 0, getTPage(2, 0, MINI_VRAM_X, MINI_VRAM_Y));
	addPrim(OT(L_MINI), t);		// (added last: drawn before the sprite)
	for (i = 0; i < PZ_PLAYERS; i++)
	{
		int	x, y;

		if (!players[i].present || ((i != me) && !players[i].seen))
			continue;
		x = (i == me) ? (my_x >> 8) : (players[i].dx >> 4);
		y = (i == me) ? (my_y >> 8) : (players[i].dy >> 4);
		if (i == me)
			Rect(L_MINI_DOT, MINI_X + x / 16 - 1, MINI_Y + y / 16 - 1, 3, 3, 255, 255, 255);
		else
			Rect(L_MINI_DOT, MINI_X + x / 16, MINI_Y + y / 16, 2, 2, colours[i % PZ_COLOURS][0],
				colours[i % PZ_COLOURS][1], colours[i % PZ_COLOURS][2]);
	}
}

// A painted tile: the colour, with the owner's shape as a pattern (4 players share each colour)
static void	DrawTile(int sx, int sy, int owner)
{
	const uint8_t	*c = colours[owner % PZ_COLOURS];
	int				r = c[0] * 9 / 16, g = c[1] * 9 / 16, b = c[2] * 9 / 16;
	int				mr = c[0] * 5 / 16, mg = c[1] * 5 / 16, mb = c[2] * 5 / 16;

	Rect(L_PAINT, sx, sy, PZ_TILE, PZ_TILE, r, g, b);
	switch (owner / PZ_COLOURS)
	{
		case 1:	Rect(L_MARK, sx + 2, sy + 2, 4, 4, mr, mg, mb);										break;
		case 2:	Tri(L_MARK, sx + 8, sy, sx, sy + 8, sx + 8, sy + 8, mr, mg, mb);					break;
		case 3:	Quad(L_MARK, sx + 4, sy + 1, sx + 1, sy + 4, sx + 7, sy + 4, sx + 4, sy + 7, mr, mg, mb);	break;
	}
}

static void	GameUpdate(void)
{
	int	dx = 0, dy = 0, speed, i, x, y;
	const volatile uint8_t	*pb = pad_buffer[0];
	static int	sent_x = -1, sent_y = -1, last_send;

	if (pad & PADLleft)		dx = -1;
	if (pad & PADLright)	dx = 1;
	if (pad & PADLup)		dy = -1;
	if (pad & PADLdown)		dy = 1;
	speed = PZ_SPEED * 256 / (is_pal ? 50 : 60);
	if (!dx && !dy && !pb[0] && (pb[1] == 0x73))	// Analog (DualShock in analog mode): the left stick
	{
		int	ax = pb[6] - 128, ay = pb[7] - 128;

		if ((abs(ax) > 32) || (abs(ay) > 32))
		{
			my_x += speed * ax / 128;
			my_y += speed * ay / 128;
		}
	}
	else if (dx && dy)
	{
		my_x += dx * speed * 181 / 256;		// (diagonal: / sqrt 2)
		my_y += dy * speed * 181 / 256;
	}
	else
	{
		my_x += dx * speed;
		my_y += dy * speed;
	}
	if (my_x < (SHAPE_R << 8))					my_x = SHAPE_R << 8;
	if (my_x > ((WORLD_PX_W - SHAPE_R - 1) << 8))	my_x = (WORLD_PX_W - SHAPE_R - 1) << 8;
	if (my_y < (SHAPE_R << 8))					my_y = SHAPE_R << 8;
	if (my_y > ((WORLD_PX_H - SHAPE_R - 1) << 8))	my_y = (WORLD_PX_H - SHAPE_R - 1) << 8;
	x = my_x >> 8;
	y = my_y >> 8;
	// Own tile painted at once (the server sends it back)
	SetTile((y / PZ_TILE) * PZ_WORLD_W + x / PZ_TILE, me + 1);
	// Position sent ~17 times a second when it changed
	if (((x != sent_x) || (y != sent_y)) && (frame - last_send >= 3))
	{
		uint8_t	m[4] = {x, x >> 8, y, y >> 8};

		Send(PZ_MOVE, m, 4);
		sent_x = x;
		sent_y = y;
		last_send = frame;
	}
	// Quick chat
	for (i = 0; i < PZ_PHRASES; i++)
	{
		static const uint32_t	buttons[PZ_PHRASES] = {PADRup, PADRright, PADRleft, PADRdown};

		if (PRESSED(buttons[i]))
		{
			uint8_t	c = i;

			Send(PZ_CHAT, &c, 1);
			players[me].chat = i;
			players[me].chat_until = frame + BUBBLE_FRAMES;
		}
	}
	if (PRESSED(PADselect))	show_names ^= 1;
	if (PRESSED(PADstart))	show_minimap ^= 1;
	// The others move smoothly to their last position
	for (i = 0; i < PZ_PLAYERS; i++)
		if (players[i].present && players[i].seen && (i != me))
		{
			players[i].dx += (players[i].x * 16 - players[i].dx) / 4;
			players[i].dy += (players[i].y * 16 - players[i].dy) / 4;
		}
}

static void	ScreenGame(void)
{
	int		cam_x, cam_y, tx0, ty0, tx, ty, i, n, row;
	char	s[40];

	GameUpdate();
	cam_x = (my_x >> 8) - VIEW_W / 2;
	cam_y = (my_y >> 8) - VIEW_H / 2;
	if (cam_x < 0)							cam_x = 0;
	if (cam_x > WORLD_PX_W - VIEW_W)		cam_x = WORLD_PX_W - VIEW_W;
	if (cam_y < 0)							cam_y = 0;
	if (cam_y > WORLD_PX_H - VIEW_H)		cam_y = WORLD_PX_H - VIEW_H;

	// World and its painted tiles in view
	Rect(L_WORLD, VIEW_X, 0, VIEW_W, VIEW_H, C_WORLD);
	tx0 = cam_x / PZ_TILE;
	ty0 = cam_y / PZ_TILE;
	for (ty = ty0; (ty <= ty0 + VIEW_H / PZ_TILE) && (ty < PZ_WORLD_H); ty++)
		for (tx = tx0; (tx <= tx0 + VIEW_W / PZ_TILE) && (tx < PZ_WORLD_W); tx++)
		{
			int	o = board[ty * PZ_WORLD_W + tx];

			if (o)
				DrawTile(VIEW_X + tx * PZ_TILE - cam_x, ty * PZ_TILE - cam_y, o - 1);
		}

	// Players (the others, then yourself with a white outline), name tags, quick chat
	for (n = 0; n < 2; n++)
		for (i = 0; i < PZ_PLAYERS; i++)
		{
			Player	*pl = &players[i];
			int		x, y, len;

			if (!pl->present || ((i == me) != n) || ((i != me) && !pl->seen))
				continue;
			x = VIEW_X + ((i == me) ? (my_x >> 8) : (pl->dx >> 4)) - cam_x;
			y = ((i == me) ? (my_y >> 8) : (pl->dy >> 4)) - cam_y;
			if ((x < VIEW_X - 40) || (x > SCREEN_W + 40) || (y < -30) || (y > SCREEN_H + 30))
				continue;
			if (i == me)
				Shape(L_OUTLINE, i / PZ_COLOURS, x, y, SHAPE_R + 2, 255, 255, 255);
			ShapeOf(L_SHAPE, i, x, y, SHAPE_R, 0);
			len = strlen(pl->name) * 8;
			if (show_names)
				Text(L_TAG, x - len / 2, y - SHAPE_R - 11, pl->name, (i == me) ? 128 : 110, (i == me) ? 128 : 110,
					(i == me) ? 128 : 120);
			if (frame < pl->chat_until)
			{
				const char	*t = phrases[pl->chat];
				int			w = strlen(t) * 8 + 6, by = y - SHAPE_R - (show_names ? 25 : 15);

				Rect(L_TAG_BG, x - w / 2, by - 2, w, 12, 240, 240, 240);
				Text(L_TAG, x - w / 2 + 3, by, t, 10, 10, 20);
			}
		}

	if (show_minimap)
	{
		MinimapUpdate();
		DrawMinimap();
	}

	// Player list
	Rect(L_PANEL, 0, 0, PANEL_W, SCREEN_H, C_PANEL);
	sprintf(s, "PLAYERS %d/%d", Count(), PZ_PLAYERS);
	Text(L_PANEL_TEXT, 4, 4, s, C_TEXT);
	for (i = 0, row = 0; i < PZ_PLAYERS; i++)
	{
		int	y = 16 + row * 8;

		if (!players[i].present)
			continue;
		if (i == me)
			Rect(L_PANEL_FG, 0, y - 1, PANEL_W, 9, C_SELECT);
		ShapeOf(L_PANEL_TEXT, i, 6, y + 3, 3, 0);
		Text(L_PANEL_TEXT, 13, y, players[i].name, (i == me) ? 128 : 110, (i == me) ? 128 : 110, (i == me) ? 128 : 120);
		sprintf(s, "%3d", players[i].score * 100 / TILES);
		Text(L_PANEL_TEXT, 100, y, s, C_YELLOW);
		row++;
	}
	Rect(L_PANEL_FG, 0, 214, PANEL_W, 1, C_SELECT);
	sprintf(s, "ROUND  %d:%02d", round_left / 60, round_left % 60);
	Text(L_PANEL_TEXT, 4, 219, s, C_TEXT);
	if (ping_ms >= 0)
		sprintf(s, "PING   %d MS", ping_ms);
	else
		strcpy(s, "PING   -");
	Text(L_PANEL_TEXT, 4, 229, s, C_TEXT);

	// Round over: the winner
	if (frame < banner_until)
	{
		const char	*w = ((round_winner < PZ_PLAYERS) && players[round_winner].present) ? players[round_winner].name : "NOBODY";
		int			cx = VIEW_X + VIEW_W / 2;

		Rect(L_OVERLAY, VIEW_X + 16, 96, VIEW_W - 32, 40, C_BAR);
		Text(L_OVERLAY_TEXT, cx - 40, 104, "ROUND OVER", C_TEXT);
		sprintf(s, "%s WINS!", w);
		Text(L_OVERLAY_TEXT, cx - strlen(s) * 4, 120, s, C_YELLOW);
	}
	if (frame - last_rx_frame > 50 * 6)
		Text(L_OVERLAY_TEXT, VIEW_X + 8, 200, "NO NEWS FROM THE SERVER...", C_ERROR);
}

/****************************************/

int	main(void)
{
	int	i, last_ping = 0;

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
	srand(*(volatile uint16_t *)0x1F801100);	// (root counter 0)

	LinkOpen();
	{
		uint8_t	h[2] = {PZ_VERSION, 0};

		Send(PZ_HELLO, h, 2);
	}

	while (1)
	{
		LinkReceive();

		opad = pad;
		pad = PadRead(1) & 0xFFFF;
		prim_ptr = prims[side];
		ClearOTagR(ot[side], LAYERS);

		if (welcomed && (state != S_LOST) && (frame - last_rx_frame > 50 * 15))
		{
			state = S_LOST;		// (the server pings back every second: nothing for 15 s, the bridge has stopped)
			Message("");
		}
		switch (state)
		{
			case S_CONNECT:
			case S_LOST:	ScreenConnect();	break;
			case S_NAME:	ScreenName();		break;
			case S_PICK:
			case S_JOINING:	ScreenPick();		break;
			case S_GAME:	ScreenGame();		break;
		}
		if (frame - last_ping >= 50)
		{
			int		v = VSync(-1);
			uint8_t	t[2] = {v, v >> 8};

			Send(PZ_PING, t, 2);
			last_ping = frame;
			// PCSX-Redux connects its serial port when it starts, before this program: the server's welcome may have
			// come too early. The hello again each second until it is answered (the server answers each hello)
			if ((link_kind == LINK_EMULATOR) && !welcomed)
			{
				uint8_t	h[2] = {PZ_VERSION, 0};

				Send(PZ_HELLO, h, 2);
			}
		}
		LinkFlush();

		// This frame drawn while the next one is built (in the other ordering table and primitive buffer)
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
