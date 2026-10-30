# PS1 REWIND

Smooth rewind on a real PlayStation, like on an emulator: hold a button and the game runs backwards, frame by frame,
for minutes. Let go and the game goes on from there.

It works for programs that opt in, with a small library ([rewind.h](rewind.h)). The program keeps everything that
makes up its game in one block of memory, and the library does the rest. A whole-console snapshot would be too slow
for this (about 3.5 MB each, seconds over the link); the game's own block is a few hundred bytes.

<p align="center">
  <img src="../../screenshots/ps1/rewind_play.png" alt="PEG DROP while it plays: 38 seconds of history on the computer, 10 in the PlayStation's RAM." width="320">
  <img src="../../screenshots/ps1/rewind_rewinding.png" alt="Rewinding: the balls go back up through the pegs." width="320">
</p>

## PEG DROP

The demo game. Drop balls from the top: they bounce off the pegs and off each other into the bins at the bottom,
which score. Bouncing balls are chaotic, so they're a good test: when you rewind, every ball flies back up exactly
the way it came.

| Button | |
|---|---|
| D-pad left/right | Move the dropper |
| Cross (hold) | Drop balls |
| Square | Drop a row of eight |
| **L1 (hold)** | **Rewind** |
| **R1 (hold)** | **Rewind 4 times as fast** |
| Triangle | New game |

The bar at the bottom shows how far back you can go, and what goes over the link.

## How it works

- **Each frame,** the library keeps a copy of the block in a ring in the PlayStation's RAM: 512 frames, about
  10 seconds. It sends the computer only what changed since the frame before.
- **The computer** ([rewind_server.py](rewind_server.py)) rebuilds every frame from that and keeps them: 10 minutes
  by default.
- **Rewinding plays the copies back from the RAM ring,** so it never waits for the link. Meanwhile the library asks
  the computer for the frames older than the oldest it has, 64 at a time, and they refill the ring behind. So the
  rewind goes on smoothly past the 10 seconds in RAM.
- **Letting go** goes on from that frame. The computer forgets the frames after it, and checks that its copy of the
  frame is the same as the PlayStation's.
- **Without the computer,** the rewind still goes back 10 seconds, from the RAM ring.

Measured on a PAL PlayStation with a PS1 DEV CART:

| | |
|---|---|
| The game's block | 684 bytes (64 balls, the score, the bins, the random seed) |
| To the computer | about 8 KB/s while playing |
| Rewinding at ×4 for 6 seconds | 24.0 seconds back, past the 10 seconds in RAM, without waiting once |
| Rewinding at ×1 for 15 seconds | 15.0 seconds back, without waiting |
| After each rewind | the PlayStation's block was the same as the computer's |

## Using it in your own program

```c
#include "rewind.h"

Rewind_Init(&game, sizeof(game));      // the block: everything that makes up the game (1 KB at most)

while (1)
{
    int speed = (pad & PADL1) ? 1 : 0; // 0: play, 1 or more: rewind that many frames

    if (!Rewind_Frame(speed))          // rewinding: the block was set back
    {
        GameStep();                    // the game moves on...
        Rewind_Save();                 // ...and the block is kept
    }
    Draw();                            // drawn from the block
}
```

- **What goes in the block:** everything the next frame depends on, including the random seed. Pointers into the
  block don't survive a rewind well; use indexes.
- **What stays out:** what doesn't change (the level, the graphics) and what's worked out from the block (what's
  drawn).
- **Sound:** sounds started while playing aren't played backwards; a game can stay silent while rewinding.
- **The link:** psxcomb's USB link, with the PS1 DEV CART firmware. Add `rewind.c` and `../../psxcomb/psxcomb.c` to
  the program's sources.

## Running it

On the computer the cartridge is connected to (Python 3, nothing else needed):

    run.bat

It opens the rewind server in its own window, sends the game to the PlayStation and starts it, then stays open as the
bridge between the cartridge and the server. With more than one cartridge, add `-n SERIAL`.

## Building

`pegdrop.ps-exe` is included. To build it, with PSn00bSDK set in `config/Makefile.conf` as for the PlayStation tools:

    make
