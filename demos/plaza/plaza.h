/*
* PS1 PLAZA: what the server and the PlayStation send each other (through "ps1transfer -net", over the psxcomb USB
* link). The server uses the same values.
*
* Packets: [type, length low, length high, payload]. Numbers are little endian.
*
* Player ids 0-23 are also their look: shape = id / 6, colour = id % 6 (4 shapes x 6 colours, one player each).
* Board tiles hold the owner: 0 nobody, else id + 1.
*
* PS1 -> server
*	'H'	hello: version (1), kind (1: 0 PlayStation, 1 bot)
*	'J'	join: id (1), name (text, 1-10 characters)
*	'M'	move: x (2), y (2), world pixels (the centre of the shape)
*	'C'	quick chat: phrase (1)
*	'P'	ping: time (2), sent back in 'O'
* Server -> PS1
*	'W'	welcome: version (1), players max (1), world width and height in tiles (1 + 1), tile size (1), tick rate (1)
*	'R'	player present: id (1), score (2), name (text) (each player on connect, then each join, to everyone)
*	'X'	player gone: id (1) (their tiles are cleared)
*	'A'	join accepted: id (1), x (2), y (2) where to start
*	'N'	join refused: reason (1)
*	'B'	the whole board: width x height bytes, rows from the top (after 'A')
*	'T'	painted tiles: [index (2), owner (1)] ...
*	'S'	positions: [id (1), x (2), y (2)] ... (players near you every tick, everyone a few times a second)
*	'F'	your position corrected by the server (moved too fast): x (2), y (2)
*	'K'	quick chat: id (1), phrase (1)
*	'L'	scores: [id (1), tiles (2)] ...
*	'D'	round: seconds left (2), winner of the last round (1, 0xFF none), new round (1: the board is cleared)
*	'O'	pong: time (2) of the ping
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _PLAZA_H_
#define _PLAZA_H_

#define	PZ_VERSION		1

#define	PZ_HELLO		'H'
#define	PZ_JOIN			'J'
#define	PZ_MOVE			'M'
#define	PZ_CHAT			'C'
#define	PZ_PING			'P'

#define	PZ_WELCOME		'W'
#define	PZ_PLAYER		'R'
#define	PZ_GONE			'X'
#define	PZ_ACCEPT		'A'
#define	PZ_REFUSE		'N'
#define	PZ_BOARD		'B'
#define	PZ_TILES		'T'
#define	PZ_POSITIONS	'S'
#define	PZ_FORCE		'F'
#define	PZ_SAID			'K'
#define	PZ_SCORES		'L'
#define	PZ_ROUND		'D'
#define	PZ_PONG			'O'

// Refusal reasons
#define	PZ_TAKEN		1		// Someone has that shape and colour
#define	PZ_FULL			2
#define	PZ_BAD_NAME		3
#define	PZ_OLD_VERSION	4

#define	PZ_HEADER		3
#define	PZ_PLAYERS		24
#define	PZ_SHAPES		4		// Circle, square, triangle, diamond
#define	PZ_COLOURS		6
#define	PZ_NAME			10
#define	PZ_PHRASES		4

// World: 96 x 90 tiles of 8 pixels (4 x 3 screens of the play area)
#define	PZ_WORLD_W		96
#define	PZ_WORLD_H		90
#define	PZ_TILE			8

#define	PZ_SPEED		150		// Pixels per second (the server allows a little more)

#endif
