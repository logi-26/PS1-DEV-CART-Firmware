/*
* PS1 CO-PROCESSOR: what the PlayStation and the computer send each other (through "ps1transfer -net", over the psxcomb
* USB link, or PCSX-Redux's serial port). Shared by coproc.c and coproc_server.py (same values there).
*
* Packets: [type, length low, length high, payload]. Numbers are little endian.
*
* PS1 -> computer
*	'H'	hello: version (1)
*	'B'	swarm on the computer from now on: target cell x, y (1 + 1), walls (CP_WALL_BYTES: a bit per cell, rows from the
*		top, bit 0 first), unit count (2), then the units: x, y (2 + 2 each, pixels * 16)
*	'G'	swarm target moved: cell x, y (1 + 1)
*	'L'	swarm walls changed: walls (CP_WALL_BYTES)
*	'K'	swarm positions received: their sequence number (2) (the next ones are sent then: paced by the link)
*	'E'	swarm back on the PlayStation (the computer stops)
*	'R'	ray traced frame wanted: id (2), quality (1: 0 moving, 1 still), yaw, pitch (2 + 2: 4096 = a turn), distance (2,
*		1/256 units)
*	'S'	smoke on the computer, the controls (each frame): cursor x, y (2 + 2, pixels), its move since the last ones
*		(2 + 2, pixels * 16), frames since the last ones (1), flags (1: CP_SMOKE_*), colour (1: index in the palette),
*		clear count (1: the smoke cleared each time it changes)
*	'N'	smoke frame received: its sequence number (2) (frames are paced by the link)
*	'Q'	smoke back on the PlayStation (the computer stops)
* Computer -> PS1
*	'W'	welcome: version (1)
*	'U'	swarm positions: sequence (2), computer time per step (4, microseconds), steps since the last ones (2), unit count
*		(2), then the units: x (9 bits), y (8 bits) packed in 3 bytes: x & 255, x >> 8, y (pixels)
*	'V'	swarm movements since the last positions: the same header, then the units: x, y moves (1 + 1, signed pixels)
*		(when they all fit; the positions again every 2 seconds)
*	'F'	ray traced frame: id (2), width, height (2 + 2), render time (2, ms), encode time (2, ms), then the frame as an
*		MDEC bitstream (BS version 2)
*	'M'	smoke frame: sequence (2), computer time per step (2, 1/10 ms), steps since the last frame (2), width, height
*		(2 + 2), then the frame as an MDEC bitstream (BS version 2)
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _COPROC_H_
#define _COPROC_H_

#define	CP_VERSION		2

#define	CP_HELLO		'H'
#define	CP_SWARM_BEGIN	'B'
#define	CP_SWARM_GOAL	'G'
#define	CP_SWARM_WALLS	'L'
#define	CP_SWARM_ACK	'K'
#define	CP_SWARM_END	'E'
#define	CP_RAY			'R'
#define	CP_SMOKE		'S'
#define	CP_SMOKE_ACK	'N'
#define	CP_SMOKE_END	'Q'

#define	CP_WELCOME		'W'
#define	CP_UNITS		'U'
#define	CP_MOVES		'V'
#define	CP_FRAME		'F'
#define	CP_SMOKE_FRAME	'M'

#define	CP_HEADER		3

// Swarm: a field of 80 x 56 cells of 4 pixels (320 x 224), 1000 units
#define	CP_GRID_W		80
#define	CP_GRID_H		56
#define	CP_CELL			4
#define	CP_CELLS		(CP_GRID_W * CP_GRID_H)
#define	CP_WALL_BYTES	(CP_CELLS / 8)
#define	CP_UNITS_MAX	1000
#define	CP_SPEED		48		// Pixels per second
#define	CP_STEP_HZ		50		// Simulation steps per second (both sides)

// Ray tracer: frames of 160 x 112 (moving) or 320 x 224 (still), shown on 320 x 224
#define	CP_RAY_W		160
#define	CP_RAY_H		112

// Smoke: fluid on the screen (320 x 224); the PlayStation's grid has cells of 8 pixels (40 x 28), the computer's of 2
// (160 x 112, its frames). Speeds in pixels a step (50 steps a second)
#define	CP_SMOKE_PUSH	1			// Flags: the cursor pushes smoke, the wind blows
#define	CP_SMOKE_WIND	2
#define	CP_SMOKE_W		160			// The computer's frames
#define	CP_SMOKE_H		112

#endif
