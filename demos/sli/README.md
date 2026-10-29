# PS1 SLI

Two PlayStations draw **one 3D scene across two screens**: 640 × 240 in all, each console drawing its half. It's a
flight over an endless landscape of sea, beaches, grassland, rock and snowy mountains, lit, Gouraud shaded and fading
into the sky, with a ship that either controller steers.

<p align="center">
  <img src="../../screenshots/ps1/sli_1.png" alt="The two consoles' screens side by side: open sea between two islands, across both screens." width="100%">
  <img src="../../screenshots/ps1/sli_2.png" alt="The two consoles' screens side by side: a coast on the left and the sea across the middle." width="100%">
  <img src="../../screenshots/ps1/sli_3.png" alt="The two consoles' screens side by side: a bay with beaches crossing the middle." width="100%">
</p>

These are the two consoles' screens put side by side: the left half from one PlayStation, the right half from the
other. Each runs at 50 fps. For the picture, both were stopped on the same frame (`freeze_frame` in sli.c, a test hook
set with `ps1transfer -poke`), because reading a screen takes longer than a frame.

## How it works

**One camera, two halves.**
- Both consoles use the same camera, and each projects the scene as if it were one 640-pixel-wide picture centred at
  x = 320.
- The left console's projection centre is on its right edge, and the right console's is on its left edge. On the
  PlayStation's 3D maths chip (the GTE) that's one setting, the screen offset.
- Each console then skips the polygons of the other half. So two PlayStations draw twice the polygons of one, on
  twice the screen.

**The same world on both (lockstep).**
- Both consoles run the same simulation from the same start, with no random numbers and only whole numbers, so
  they stay identical frame after frame.
- Each frame they send each other only their controller's buttons, a few bytes, and each simulates frame N only once
  it has both consoles' buttons for it.
- A button press is used 3 frames later (60 ms), which hides the link's time.
- Each packet also carries a check of the world. If the checks ever differ, both consoles start the flight again
  from the same place.

**The link cable.**
- The consoles talk to each other directly over the PlayStation link cable, with no computer in between once the
  demo has started.
- The serial port runs at about 529 kbps, with an interrupt collecting the bytes, as Net Yaroze link games do.
- Lost or damaged packets are caught by a CRC and sent again.

In a one-minute test, both consoles ran at 50 fps with no restarts, and the CRC caught the two damaged packets.

**The limit:** the two screens aren't genlocked. Each console's picture comes from its own crystal, so the two screens
refresh up to one frame apart. The worlds stay identical, but something moving fast across the join can look very
slightly sheared.

## Running it

You need two PAL (or two NTSC) PlayStations, each with a PS1 DEV CART connected to the same computer, each on its
own screen, and a link cable between the consoles. Then run:

    run.bat

It starts the demo on both consoles: the first cartridge draws the left half and the second the right half. After
that the computer isn't needed, because the consoles keep in step over the link cable. To choose which is which, give
the serial numbers (`ps1transfer -list` shows them), left one first:

    run.bat A10PVL53 A10PUCVT

| Button (either controller) | |
|---|---|
| D-pad left/right | Turn |
| D-pad up/down | Climb / dive |
| Select | Swap the two halves (if the screens are the other way round) |

When nobody steers, the ship follows a slow wandering course.

**No link cable?** `run_usb.bat` joins the two consoles through their cartridges and the computer instead (the USB
link relay, which stays open). It works, but at about 25 fps, because the relay adds latency.

**On one console,** the demo flies on its own, drawing its half and showing "WAITING FOR THE OTHER PS1".

## Building

`sli.ps-exe` is included. To build it, with PSn00bSDK set in `config/Makefile.conf` as for the PlayStation tools:

    make

It uses the repo's psxcomb (`../../psxcomb`, for the USB link) and `psn00b_tool.mk`.
