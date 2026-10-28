/*
* PS1 DEV CART control tester
* Executed by "ps1transfer -padtest" (the PS1 Dev Cart Manager's Remote Controller page: Run the control tester), with
* the remote controller option: the controllers of both ports as the program sees them through the BIOS pad driver
* (InitPAD/StartPAD), real ones or emulated by the cartridge (digital pad, DualShock, analog joystick, NeGcon, mouse,
* GunCon: the Remote Controller page chooses the device).
*
* Each port: the device type (the ID byte of its reply) and the buttons held, the sticks / NeGcon twist and analog
* buttons / mouse motion / GunCon position, drawn on a small diagram. The mouse moves a pointer on the screen, the
* GunCon aims a sight (the X/Y range the manager uses: the picture's edges at X 77-461, Y 25-248). The trigger (and
* the mouse's left button) leaves a mark where it was shot.
*
* Nothing is printed with printf (the screen is all there is to see), the TTY is not needed.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <psxgpu.h>
#include <psxetc.h>
#include <psxapi.h>
#include "psyq_compat.h"
#include "devcart_ui.h"

#define	SCREEN_W		320
#define	SCREEN_H		240
#define	PANEL_W			148
#define	TEXT_Y			20
#define	TEXT_LINES		7
#define	DIAG_Y			(TEXT_Y + UI_PANEL_H(TEXT_LINES) + 4)
#define	DIAG_H			(UI_HELP_Y - 8 - 4 - DIAG_Y)
#define	GUN_X0			77			// GunCon X/Y at the picture's edges (as the manager's defaults)
#define	GUN_X1			461
#define	GUN_Y0			25
#define	GUN_Y1			248
#define	MARKS			8			// Shots kept
#define	TILES			160			// Our tiles and lines per frame (the UI's are for the panels)
#define	LINES			32

// Device IDs (the second byte of the reply, see monitor/monitor.h)
#define	ID_DIGITAL		0x41
#define	ID_MOUSE		0x12
#define	ID_NEGCON		0x23
#define	ID_ANALOG_JOY	0x53
#define	ID_ANALOG		0x73
#define	ID_GUNCON		0x63

// Buttons, as PsyQ's bits (PadRead): ~(byte 2 << 8 | byte 3)
#define	B_SELECT		(1 << 8)
#define	B_L3			(1 << 9)
#define	B_R3			(1 << 10)
#define	B_START			(1 << 11)
#define	B_UP			(1 << 12)
#define	B_RIGHT			(1 << 13)
#define	B_DOWN			(1 << 14)
#define	B_LEFT			(1 << 15)
#define	B_L2			(1 << 0)
#define	B_R2			(1 << 1)
#define	B_L1			(1 << 2)
#define	B_R1			(1 << 3)
#define	B_TRIANGLE		(1 << 4)
#define	B_CIRCLE		(1 << 5)
#define	B_CROSS			(1 << 6)
#define	B_SQUARE		(1 << 7)
#define	MOUSE_LEFT		(1 << 3)	// Byte 3 (the bits of the low byte)
#define	MOUSE_RIGHT		(1 << 2)
#define	GUN_TRIGGER		B_CIRCLE	// (the GunCon's buttons at the pad's bits)
#define	GUN_A			B_START
#define	GUN_B			B_CROSS

#define	C_ON			UI_OK
#define	C_OFF			UI_TRACK
#define	C_PORT1			80, 200, 255
#define	C_PORT2			255, 170, 60

typedef struct
{
	uint8_t		id;			// ID byte, 0: no controller
	uint16_t	bits, obits;
	uint8_t		axes[4];
	int			mouse_x, mouse_y;	// Mouse pointer on the screen
	int			last_id;
} Port;

typedef struct
{
	int16_t		x, y;
	uint8_t		port;
} Mark;

static DISPENV	disp[2];
static DRAWENV	draw[2];
static int		fnt_port[2];
static Port		ports[2];
static Mark		marks[MARKS];
static int		n_marks, next_mark;
volatile uint32_t	frames;			// Frames drawn (a debugger or a test sees the program running)

// Our primitives: tiles and lines over the panels, drawn after them in the order of the calls (an ordering table
// entry each, from its last entry: the pointers, drawn last, are over the diagrams)
#define	OT_LEN			(TILES + LINES)

static TILE		tiles[2][TILES];
static LINE_F2	lines[2][LINES];
static uint32_t	ot[2][OT_LEN];
static int		side, n_tiles, n_lines;

static void	Tile(int x, int y, int w, int h, int r, int g, int b)
{
	TILE	*t;
	int		i = OT_LEN - 1 - (n_tiles + n_lines);	// (addPrim is a macro: its arguments are used twice)

	if (n_tiles >= TILES)
		return;
	t = &tiles[side][n_tiles++];
	setTile(t);
	setXY0(t, x, y);
	setWH(t, w, h);
	setRGB0(t, r, g, b);
	addPrim(&ot[side][i], t);
}

static void	Line(int x0, int y0, int x1, int y1, int r, int g, int b)
{
	LINE_F2	*l;
	int		i = OT_LEN - 1 - (n_tiles + n_lines);

	if (n_lines >= LINES)
		return;
	l = &lines[side][n_lines++];
	setLineF2(l);
	setXY2(l, x0, y0, x1, y1);
	setRGB0(l, r, g, b);
	addPrim(&ot[side][i], l);
}

#define	LIT(_on_)		((_on_) ? 80 : 60), ((_on_) ? 220 : 60), ((_on_) ? 80 : 60)

static void	Button(int x, int y, int w, int h, int on)
{
	Tile(x, y, w, h, LIT(on));
}

// A value 0-255 as a bar (centered: from the middle, for the NeGcon's twist)
static void	Bar(int x, int y, int w, int v, int centered)
{
	int	f;

	Tile(x, y, w, 5, C_OFF);
	if (centered)
	{
		f = (v - 128) * (w / 2) / 128;
		if (f >= 0)
			Tile(x + w / 2, y, f + 1, 5, UI_ACCENT);
		else
			Tile(x + w / 2 + f, y, -f, 5, UI_ACCENT);
	}
	else
		Tile(x, y, v * w / 255, 5, UI_ACCENT);
}

static void	Stick(int x, int y, int ax, int ay)
{
	Tile(x, y, 36, 36, C_OFF);
	Tile(x + 17, y, 2, 36, 40, 40, 40);
	Tile(x, y + 17, 36, 2, 40, 40, 40);
	Tile(x + ax * 30 / 255, y + ay * 30 / 255, 6, 6, C_ON);
}

static const char	*DeviceName(uint8_t id)
{
	switch (id)
	{
		case 0:				return ("NO CONTROLLER");
		case ID_DIGITAL:	return ("DIGITAL PAD");
		case ID_ANALOG:		return ("DUALSHOCK");
		case ID_ANALOG_JOY:	return ("ANALOG JOY");
		case ID_NEGCON:		return ("NEGCON");
		case ID_MOUSE:		return ("MOUSE");
		case ID_GUNCON:		return ("GUNCON");
	}
	return ("UNKNOWN");
}

// The GunCon's position on the screen, 0 if it points off the screen
static int	GunScreen(Port *p, int *x, int *y)
{
	int	gx = p->axes[0] | (p->axes[1] << 8), gy = p->axes[2] | (p->axes[3] << 8);

	if ((gx < GUN_X0) || (gx > GUN_X1) || (gy < GUN_Y0) || (gy > GUN_Y1))
		return (0);
	*x = (gx - GUN_X0) * SCREEN_W / (GUN_X1 - GUN_X0);
	*y = (gy - GUN_Y0) * SCREEN_H / (GUN_Y1 - GUN_Y0);
	return (1);
}

static void	AddMark(int x, int y, int port)
{
	marks[next_mark].x = x;
	marks[next_mark].y = y;
	marks[next_mark].port = port;
	next_mark = (next_mark + 1) % MARKS;
	if (n_marks < MARKS)
		n_marks++;
}

// The reply of a port read from the BIOS buffer: device, buttons, axes; the mouse pointer moved
static void	ReadPort(int n)
{
	volatile uint8_t	*buf = pad_buffer[n];
	Port				*p = &ports[n];
	int					i, x, y;

	p->obits = p->bits;
	p->id = buf[0] ? 0 : buf[1];
	p->bits = p->id ? (~((buf[2] << 8) | buf[3]) & 0xFFFF) : 0;
	for (i = 0; i < 4; i++)
		p->axes[i] = p->id ? buf[4 + i] : 0;
	if (p->id == ID_MOUSE)
	{
		if (p->last_id != ID_MOUSE)		// A new mouse: the pointer at the center
		{
			p->mouse_x = SCREEN_W / 2;
			p->mouse_y = SCREEN_H / 2;
		}
		p->bits &= MOUSE_LEFT | MOUSE_RIGHT;
		p->mouse_x += (int8_t)buf[4];
		p->mouse_y += (int8_t)buf[5];
		p->mouse_x = (p->mouse_x < 0) ? 0 : ((p->mouse_x >= SCREEN_W) ? SCREEN_W - 1 : p->mouse_x);
		p->mouse_y = (p->mouse_y < 0) ? 0 : ((p->mouse_y >= SCREEN_H) ? SCREEN_H - 1 : p->mouse_y);
		if ((p->bits & MOUSE_LEFT) && !(p->obits & MOUSE_LEFT))
			AddMark(p->mouse_x, p->mouse_y, n);
	}
	if ((p->id == ID_GUNCON) && (p->bits & GUN_TRIGGER) && !(p->obits & GUN_TRIGGER) && GunScreen(p, &x, &y))
		AddMark(x, y, n);
	p->last_id = p->id;
}

static const struct
{
	uint16_t	bit;
	const char	*name;
} names[] = {
	{B_UP, "UP"}, {B_DOWN, "DOWN"}, {B_LEFT, "LEFT"}, {B_RIGHT, "RIGHT"}, {B_TRIANGLE, "TRI"}, {B_CIRCLE, "CIRCLE"},
	{B_CROSS, "CROSS"}, {B_SQUARE, "SQUARE"}, {B_L1, "L1"}, {B_L2, "L2"}, {B_R1, "R1"}, {B_R2, "R2"},
	{B_SELECT, "SELECT"}, {B_START, "START"}, {B_L3, "L3"}, {B_R3, "R3"}
};

// The buttons held, as names, wrapped on the panel's lines (16 characters)
static void	PrintButtons(int fnt, Port *p)
{
	char		line[24];
	const char	*name;
	int			i, len = 0, any = 0;
	uint16_t	bit;

	line[0] = 0;
	for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
	{
		bit = names[i].bit;
		name = names[i].name;
		if (p->id == ID_MOUSE)			// (other names for these devices' buttons)
		{
			if (i > 1)
				break;
			bit = i ? MOUSE_RIGHT : MOUSE_LEFT;
			name = i ? "RIGHT" : "LEFT";
		}
		else if (p->id == ID_GUNCON)
		{
			if (i > 2)
				break;
			bit = (i == 0) ? GUN_TRIGGER : ((i == 1) ? GUN_A : GUN_B);
			name = (i == 0) ? "TRIGGER" : ((i == 1) ? "A" : "B");
		}
		else if (p->id == ID_NEGCON)
		{
			if (bit == B_CIRCLE)
				name = "A";
			else if (bit == B_TRIANGLE)
				name = "B";
			else if (bit == B_R1)
				name = "R";
			else if (!(bit & (B_UP | B_DOWN | B_LEFT | B_RIGHT | B_START)))
				continue;
		}
		if (!(p->bits & bit))
			continue;
		if (len + strlen(name) + 1 > 16)
		{
			FntPrint(fnt, "%s\n", line);
			len = 0;
			line[0] = 0;
		}
		len += sprintf(line + len, "%s%s", len ? " " : "", name);
		any = 1;
	}
	FntPrint(fnt, "%s\n", any ? line : "-");
}

// A port's panel: text, then its diagram
static void	ShowPort(int n)
{
	Port	*p = &ports[n];
	int		x = 8 + n * (PANEL_W + 8), y = DIAG_Y, fnt = fnt_port[n], gx, gy, on;

	UiTextPanel(x, TEXT_Y, PANEL_W, TEXT_LINES);
	UiPanel(x, DIAG_Y, PANEL_W, DIAG_H);
	Tile(x, TEXT_Y, PANEL_W, 3, n ? 255 : 80, n ? 170 : 200, n ? 60 : 255);	// The port's color
	FntPrint(fnt, "PORT %d\n%s\n", n + 1, DeviceName(p->id));
	if (p->id)
		FntPrint(fnt, "ID %02X\n", p->id);
	else
		FntPrint(fnt, "\n");
	if (!p->id)
		return;
	PrintButtons(fnt, p);

	switch (p->id)
	{
		case ID_DIGITAL:
		case ID_ANALOG:
		case ID_ANALOG_JOY:
		case ID_NEGCON:
			if (p->id != ID_NEGCON)
			{
				Button(x + 8, y + 6, 26, 6, p->bits & B_L2);
				Button(x + 8, y + 14, 26, 6, p->bits & B_L1);
				Button(x + 114, y + 6, 26, 6, p->bits & B_R2);
			}
			Button(x + 114, y + 14, 26, 6, p->bits & B_R1);
			Button(x + 24, y + 28, 8, 10, p->bits & B_UP);
			Button(x + 24, y + 48, 8, 10, p->bits & B_DOWN);
			Button(x + 12, y + 39, 10, 8, p->bits & B_LEFT);
			Button(x + 34, y + 39, 10, 8, p->bits & B_RIGHT);
			Button(x + 116, y + 28, 10, 10, p->bits & B_TRIANGLE);
			Button(x + 128, y + 39, 10, 10, p->bits & B_CIRCLE);
			if (p->id != ID_NEGCON)
			{
				Button(x + 116, y + 50, 10, 10, p->bits & B_CROSS);
				Button(x + 104, y + 39, 10, 10, p->bits & B_SQUARE);
				Button(x + 56, y + 41, 14, 6, p->bits & B_SELECT);
			}
			Button(x + 78, y + 41, 14, 6, p->bits & B_START);
			if ((p->id == ID_ANALOG) || (p->id == ID_ANALOG_JOY))
			{
				FntPrint(fnt, "L %3d,%3d\nR %3d,%3d", p->axes[2], p->axes[3], p->axes[0], p->axes[1]);
				Stick(x + 30, y + 66, p->axes[2], p->axes[3]);
				Stick(x + 82, y + 66, p->axes[0], p->axes[1]);
				Button(x + 30, y + 104, 36, 4, p->bits & B_L3);
				Button(x + 82, y + 104, 36, 4, p->bits & B_R3);
			}
			if (p->id == ID_NEGCON)
			{
				FntPrint(fnt, "TWIST %3d I %3d\nII %3d   L %3d", p->axes[0], p->axes[1], p->axes[2], p->axes[3]);
				Bar(x + 14, y + 68, 120, p->axes[0], 1);
				Bar(x + 14, y + 80, 120, p->axes[1], 0);
				Bar(x + 14, y + 90, 120, p->axes[2], 0);
				Bar(x + 14, y + 100, 120, p->axes[3], 0);
			}
			break;

		case ID_MOUSE:
			FntPrint(fnt, "MOVE %4d %4d\nAT %3d,%3d", (int8_t)p->axes[0], (int8_t)p->axes[1], p->mouse_x,
				p->mouse_y);
			Button(x + 40, y + 20, 30, 40, p->bits & MOUSE_LEFT);
			Button(x + 78, y + 20, 30, 40, p->bits & MOUSE_RIGHT);
			Tile(x + 40, y + 70, 71, 39, C_OFF);
			Tile(x + 40 + (p->mouse_x * 68 / SCREEN_W), y + 70 + (p->mouse_y * 36 / SCREEN_H), 3, 3, C_ON);
			break;

		case ID_GUNCON:
			on = GunScreen(p, &gx, &gy);
			FntPrint(fnt, "X %3d  Y %3d\n%s", p->axes[0] | (p->axes[1] << 8), p->axes[2] | (p->axes[3] << 8),
				on ? "ON SCREEN" : "OFF SCREEN");
			Button(x + 20, y + 10, 34, 10, p->bits & GUN_TRIGGER);
			Button(x + 60, y + 10, 30, 10, p->bits & GUN_A);
			Button(x + 96, y + 10, 30, 10, p->bits & GUN_B);
			Tile(x + 34, y + 30, 82, 62, C_OFF);
			if (on)
				Tile(x + 34 + gx * 80 / SCREEN_W, y + 30 + gy * 60 / SCREEN_H, 3, 3, C_ON);
			break;
	}
}

// The mouse pointers and GunCon sights over the screen, and the shots
static void	Pointers(void)
{
	int	n, i, x, y;

	for (i = 0; i < n_marks; i++)
	{
		if (marks[i].port)
			Tile(marks[i].x - 2, marks[i].y - 2, 5, 5, C_PORT2);
		else
			Tile(marks[i].x - 2, marks[i].y - 2, 5, 5, C_PORT1);
	}
	for (n = 0; n < 2; n++)
	{
		Port	*p = &ports[n];
		int		r = n ? 255 : 80, g = n ? 170 : 200, b = n ? 60 : 255;

		if (p->id == ID_MOUSE)			// Arrow: a corner and its diagonal
		{
			x = p->mouse_x;
			y = p->mouse_y;
			Line(x, y, x + 10, y + 10, r, g, b);
			Line(x, y, x, y + 12, r, g, b);
			Line(x, y, x + 8, y, r, g, b);
			Line(x + 1, y + 1, x + 9, y + 11, r, g, b);
		}
		else if ((p->id == ID_GUNCON) && GunScreen(p, &x, &y))	// Sight: a cross in a square
		{
			int	c = (p->bits & GUN_TRIGGER) ? 0 : 1;

			Line(x - 12, y, x - 3, y, r, g * c, b * c);
			Line(x + 3, y, x + 12, y, r, g * c, b * c);
			Line(x, y - 12, x, y - 3, r, g * c, b * c);
			Line(x, y + 3, x, y + 12, r, g * c, b * c);
			Line(x - 7, y - 7, x + 7, y - 7, r, g, b);
			Line(x + 7, y - 7, x + 7, y + 7, r, g, b);
			Line(x + 7, y + 7, x - 7, y + 7, r, g, b);
			Line(x - 7, y + 7, x - 7, y - 7, r, g, b);
		}
	}
}

int	main(void)
{
	int	i, is_pal;

	is_pal = (*((char *)0xBFC7FF52) == 'E');
	ResetGraph(0);
	SetVideoMode(is_pal ? MODE_PAL : MODE_NTSC);
	PadInit(0);
	for (i = 0; i < 2; i++)
	{
		SetDefDispEnv(&disp[i], 0, i ? 0 : SCREEN_H, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&draw[i], 0, i ? SCREEN_H : 0, SCREEN_W, SCREEN_H);
	}
	FntLoad(960, 256);
	UiInit(draw);
	for (i = 0; i < 2; i++)
		fnt_port[i] = UiOpenText(8 + i * (PANEL_W + 8), TEXT_Y, PANEL_W, TEXT_LINES);
	SetDispMask(1);

	while (1)
	{
		ClearOTagR(ot[side], OT_LEN);
		n_tiles = n_lines = 0;
		ReadPort(0);
		ReadPort(1);
		UiTitle("PS1 DEV CART CONTROL TESTER", "");
		ShowPort(0);
		ShowPort(1);
		Pointers();
		UiHelp("CONTROLLERS: REAL ONES, OR EMULATED BY\nTHE MANAGER (REMOTE CONTROLLER PAGE)");

		DrawSync(0);
		VSync(0);
		side ^= 1;
		PutDispEnv(&disp[side]);
		PutDrawEnv(&draw[side]);
		UiFlush();
		DrawOTag(&ot[side ^ 1][OT_LEN - 1]);	// (the primitives of the frame just built: diagrams, then the pointers)
		FntFlush(fnt_port[0]);
		FntFlush(fnt_port[1]);
		frames++;
	}

	return (0);
}
