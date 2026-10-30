/*
* PS1 REWIND: smooth rewind for a PlayStation program that opts in, through the PS1 DEV CART.
*
* The program keeps everything that makes up its game in one block of memory (positions, speeds, score, random seed:
* what's drawn is worked out from it). Each frame, the library keeps a copy of the block in a ring in RAM
* (REWIND_RING frames), and sends the computer what changed since the frame before; the computer (rewind_server.py)
* keeps minutes of them. Rewinding plays the copies back from the ring, so it never waits for the link, while older
* ones come from the computer to refill the ring behind. Releasing the button goes on from that frame (the computer
* forgets the frames after it). Without the computer, the rewind goes back REWIND_RING frames.
*
* Each frame:
*	if (!Rewind_Frame(speed))	// speed: 0 to play, 1 or more to rewind that many frames (the block is set back)
*	{
*		GameStep();				// the game moves on...
*		Rewind_Save();			// ...and the block is kept
*	}
*	Draw();
*
* Link: psxcomb's USB link (the PS1 DEV CART firmware), bridged to rewind_server.py by "ps1transfer -net". Protocol
* (packets: type, length (2), payload; numbers little endian):
*	PS1 -> computer	'H' hello (each second): version (1), block size (2)
*					'K' keyframe: frame (4), the whole block
*					'D' frame (4), what changed since the frame before: runs of skip (2), count (1), bytes XOR
*					'B' older frames wanted: the oldest the PS1 has (4), how many (2)
*					'T' the program goes on from this frame (4): the computer forgets the ones after it; the sum
*						of the block's bytes (4), checked by the computer
*	computer -> PS1	'W' welcome (each hello: it keeps the connection known): version (1), the oldest frame it has (4)
*					'O' an older frame: frame (4), what changed from the frame after it (the same runs)
*					'N' no older frames: the oldest it has (4)
*					'X' send a keyframe (the computer missed a frame)
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _REWIND_H_
#define _REWIND_H_

#include <stdint.h>

#define	REWIND_VERSION		1
#define	REWIND_RING			512		// Frames kept in RAM (about 10 seconds at 50 fps)
#define	REWIND_MAX_STATE	1024	// The block's size at most

typedef struct
{
	int		connected;		// The computer answers
	int		rewinding;
	int		ring_frames;	// Frames that can be rewound from RAM
	int		frames_back;	// Frames that can be rewound in all (with the computer's)
	int		up_bytes;		// Bytes sent and received in the last second
	int		down_bytes;
	int		waiting;		// The rewind waits for older frames from the computer
} RewindInfo;

void	Rewind_Init(void *state, int size);		// Opens the link (if the PS1 DEV CART firmware is there)
int		Rewind_Frame(int speed);				// Before the game's step: 1 if rewinding (the block was set back)
void	Rewind_Save(void);						// After the game's step: the block kept
void	Rewind_GetInfo(RewindInfo *info);

#endif
