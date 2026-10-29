/*
* PS1 LIVE: the level in the PlayStation's RAM, edited by the computer while the game runs (live_editor.py, through
* "ps1transfer -memlive" or PCSX-Redux's GDB server). The same layout in live_editor.py.
*
* The game reads the level block every frame: tiles, colours and physics change at once. When "version" changes (the
* editor writes it last, after the data), the enemies start their patrols again from their new places, and the tile
* graphics are uploaded to VRAM when "tiles_version" changes (hot reload of the tile set).
* The game writes the state block (where the player and the enemies are) for the editor's map.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _LIVE_H_
#define _LIVE_H_

#include <stdint.h>

// Fixed addresses (above the program and its data, below the stack): the editor needs no symbols
#define	LV_LEVEL_ADRS	0x801C0000
#define	LV_STATE_ADRS	0x801BF000

#define	LV_MAGIC		0x4556494C		// "LIVE"
#define	LV_W			64				// Tiles (16 x 16 pixels): 1024 x 224 pixels
#define	LV_H			14
#define	LV_TILE			16
#define	LV_ENEMIES		16
#define	LV_TILE_TYPES	8
#define	LV_SHEET_W		256				// Tile set: 16 tiles of 16 x 16, 16-bit colour (0x0000: transparent)
#define	LV_SHEET_H		16

// Tiles
enum {T_EMPTY, T_GROUND, T_BRICK, T_BLOCK, T_SPIKES, T_COIN, T_GOAL, T_SPRING};
#define	LV_SPRITE_PLAYER	8			// Tile set entries of the sprites
#define	LV_SPRITE_ENEMY		9

typedef struct
{
	int16_t		x, y;			// Pixels (top left), at the start of its patrol
	int16_t		range;			// Patrol to the right of x, pixels
	int16_t		speed;			// Pixels a frame * 256
	uint8_t		r, g, b, pad;	// Tint (128: as drawn)
} LvEnemy;

typedef struct
{
	uint32_t	magic;
	uint32_t	version;					// Written last by the editor after a change
	uint8_t		tiles[LV_H][LV_W];
	int16_t		start_x, start_y;			// The player's start, pixels
	uint8_t		enemy_count, pad[3];
	LvEnemy		enemies[LV_ENEMIES];
	uint8_t		sky_top[4], sky_bottom[4];	// RGB (and a pad byte)
	uint8_t		player_tint[4];
	uint8_t		tint[LV_TILE_TYPES][4];		// Tint of each tile type (128: as drawn)
	int16_t		gravity, jump, run, max_fall;	// Pixels a frame * 256 (per frame for gravity)
	uint32_t	tiles_version;				// Written after the tile set's pixels
	uint16_t	sheet[LV_SHEET_H][LV_SHEET_W];
} LvLevel;

typedef struct
{
	uint32_t	magic;
	uint32_t	frame;
	int16_t		player_x, player_y;
	uint16_t	coins, deaths, wins, pad;
	int16_t		enemy_x[LV_ENEMIES];
	uint32_t	version_seen;				// The level version the game uses
} LvState;

#endif
