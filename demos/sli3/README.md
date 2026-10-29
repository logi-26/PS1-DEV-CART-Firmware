# PS1 SLI 3 SCREENS

The [PS1 SLI](../sli/README.md) flight on **three screens**: 960 × 240 in all, each screen drawing its own third.
Here two PlayStations draw the left and middle screens, and PCSX-Redux draws the right one, all in step at 50 fps.
It works with more screens too, and with two consoles that have no link cable between them.

<p align="center">
  <img src="../../screenshots/ps1/sli3_1.png" alt="Three screens side by side: open sea, a green coast on the left and an island on the right." width="100%">
  <img src="../../screenshots/ps1/sli3_2.png" alt="Three screens side by side: an inlet winding through green land, the sea on the right." width="100%">
  <img src="../../screenshots/ps1/sli3_3.png" alt="Three screens side by side: a bay between two green coasts across all three." width="100%">
</p>

These are the three screens put side by side: the left one from the first PlayStation, the middle one from the
second, and the right one from PCSX-Redux. For the picture, the hub stopped all three on the same frame (`freeze`).

## How it works

The link cable joins only two consoles. Here every screen goes through the computer instead:
- each console over its cartridge's USB link, with a bridge to the hub (`ps1transfer -net`);
- PCSX-Redux over its serial port, bridged to the hub by `redux.ps1`.

The hub (`sli_hub.py`) keeps them all in step.
- **The picture widens with the screens.** With N screens it's N × 320 pixels wide, with the same field of view
  (about 106 degrees) spread across them. Each screen draws its own 320 columns, and skips the others' polygons.
- **The hub keeps the time.** Each screen sends the hub its buttons for frame N. Once the hub has everyone's, it
  sends every screen all the buttons together, and they all simulate frame N. A button press is used 4 frames later
  (80 ms).
- **The world checks** come to the hub too. If they ever differ, the hub starts every screen again together.

It's the same program as the two-screen demo (`../sli/sli.ps-exe`): started with the USB link, or in PCSX-Redux, it
waits for the hub instead of the other console.

**Speed.** Two consoles and PCSX-Redux ran together at 50 fps. The landscape goes a little less far than over the
link cable (20 cells instead of 22) on every screen, because serving the USB link takes a little of each frame.
PCSX-Redux needs its dynarec: with the interpreter (for example with its GDB server on) it runs at about 45 fps, and
every screen waits for it.

## Running it

You need PlayStations with a PS1 DEV CART, each connected to this computer and on its own screen, and Python 3
(the hub needs nothing else). For PCSX-Redux as a screen, it is found on the PATH, in the PSX.Dev extension for VS
Code, or with `PCSX_REDUX` set to its `pcsx-redux.exe`.

Give the screens from left to right, as cartridge serial numbers (`ps1transfer -list` shows them) and `redux` for
PCSX-Redux:

    run.bat A10PVL53 A10PUCVT redux

With no names, it uses every cartridge connected:

    run.bat

It opens the hub in its own window, starts the demo on each console with its bridge in a window of its own, and
starts PCSX-Redux. PCSX-Redux runs with its own copy of your settings (in `.redux\` here), with the serial port and
the dynarec on: your own settings aren't changed. The screens start together once all of them are connected. Close
the windows to stop.

| Button (any controller) | |
|---|---|
| D-pad left/right | Turn |
| D-pad up/down | Climb / dive |
| Select | Move every screen one place along (if the screens are in the wrong order) |

In the hub window, `freeze` stops every screen on the same frame (for a photo), and `go` goes on.

A screen that stops (its bridge closed, or the emulator quit) makes the others wait, showing "WAITING FOR THE
HUB...". They all start again together once it's back.
