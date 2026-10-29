# PS1 LIVE

A small platform game that you edit on the computer **while it runs on the PlayStation**. Paint tiles, move enemies,
change colours and physics, or redraw the graphics in a paint program, and the change is on the TV at once. There's
no rebuilding and no restarting: the game keeps running, with you still in it.

This is what a development cartridge is for. A normal cartridge can load a program, but it can't reach into a game
while it runs. The PS1 DEV CART reads and writes the running game's memory over USB (`ps1transfer -memlive`). It
also works with the PCSX-Redux emulator, through its GDB server, for people without a cartridge.

<p align="center">
  <img src="../../screenshots/demos/live_editor.png" alt="The PS1 LIVE editor: the level map with the player and enemies where they are in the game, the tile palette, enemies, colours, physics and the tile graphics." width="100%">
</p>

<p align="center">
  <img src="../../screenshots/ps1/live_before.png" alt="The game as it starts." width="260">
  <img src="../../screenshots/ps1/live_after.png" alt="A moment later, edited while running: a new sky, a row of blocks and coins, an enemy moved next to the player." width="260">
  <img src="../../screenshots/ps1/live_hotreload.png" alt="The tile set changed: the grass is purple (hot reload)." width="260">
</p>

These three pictures came off the PlayStation one after the other, without restarting the game:
1. **As it starts.**
2. **Edited:** a new sky, a row of blocks and coins, and an enemy moved next to the player.
3. **The tile set changed** (hot reload): the grass is now purple.

## Running it

You need Python 3 with Tkinter, which comes with the usual Windows installer. Run:

    editor.bat

then press:
- **Start on the PS1 DEV CART:** sends the game to the PlayStation and connects. With more than one cartridge, type
  its serial number first (`ps1transfer -list` shows them).
- **Attach (game running):** connects to the game if it's already running.
- **Start in PCSX-Redux:** no cartridge needed. The editor finds PCSX-Redux on the PATH or in the PSX.Dev
  extension for VS Code, or asks where it is.

When it connects, the editor reads the level from the game. After that, every change you make is written straight
into the running game.

## What you can change

| | |
|---|---|
| **Tiles** | Paint with the left button and erase with the right: ground, brick, block, spikes, coin, goal flag, spring. |
| **Enemies** | Click to add one or pick one, drag to move it, right-click to delete it. Set its patrol length, speed and colour. |
| **Player start** | Click where the player should start. |
| **Colours** | The sky's two colours, the player's tint and each tile type's tint. |
| **Physics** | Gravity, jump strength, running speed, fastest fall. Try a low gravity while you play. |
| **Tile graphics** | **Export the tile set PNG**, edit it in any paint program, and **each time you save it**, the editor sends it to the game (hot reload). It's 256 × 16 pixels: 16 tiles of 16 × 16, with the player as the 9th and the enemy as the 10th. |
| **The TV** | "Show the TV here" shows the PlayStation's picture in the editor every 2 seconds, read from its video memory while the game runs (PS1 DEV CART). |

The map shows the player and the enemies where they are in the game, read from the PlayStation 10 times a second,
with your coins, deaths and goals. **Save level** and **Load level** keep levels as files on the computer.

**On the PlayStation:** Left/Right to run, Cross to jump, Start to go back to the start. Collect the coins, avoid
the spikes, the enemies and the pits, bounce on springs, and reach the flag.

## How it works

The whole level is one block in the PlayStation's RAM at a fixed address (`0x801C0000`), described in
[live.h](live.h):
- the tiles, enemies, start, colours and physics;
- the tile graphics, which the game copies to video memory when they change.

**The game** ([live.c](live.c)) reads the block every frame, so a change shows in the very next frame. It writes
where the player and the enemies are to a second block, for the editor's map. It has no link code at all: the
cartridge's firmware does the work while the game runs, the same as for any other program.

**The editor** ([live_editor.py](live_editor.py)) writes only the bytes that changed, then a version number:
- **The version** tells the game the enemies have new places.
- **A second version** tells the game when the tile graphics change.

A change takes a few milliseconds on the PS1 DEV CART, or about 70 ms in PCSX-Redux (the emulator is paused for a
moment to write its memory).

You can use the same idea in your own game: put the values you want to tune in one place in RAM, and change them
with `ps1transfer -memlive` (or the PS1 Dev Cart Manager's Memory page) while the game runs.

## Building

`live.ps-exe` is included. To build it, with PSn00bSDK set in `config/Makefile.conf` as for the PlayStation tools:

    make
