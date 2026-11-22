PS1 SLI 3 SCREENS on PCSX-Redux (no console)
==========================================

The PS1 DEV CART repo's 3-screen SLI flight (demos/sli3), with three PCSX-Redux windows instead of PlayStations, to
record it on one computer. Kept outside the repo.

    run.bat                      three windows side by side on the main monitor, 640 x 480 each (1920 x 480 in all)
    run.bat fullscreen           one screen per monitor, full screen, left to right as the monitors are arranged
    run.bat --layout monitors    one screen per monitor, in a window as large as fits
    run.bat --width 600          smaller windows in the row

It opens the hub in a window of its own, then the three emulators. They start together once all three are connected:
left "1 OF 3", middle "2 OF 3", right "3 OF 3", each at 50 fps.

- Steer with the keyboard or a controller in any of the windows (the one that has the focus): D-pad left/right to
  turn, up/down to climb and dive. Nobody steering: the ship follows a slow wandering course.
- In the hub window, type "freeze" to stop all three screens on the same frame (a still), "go" to go on.
- Close the emulators to stop (the hub closes with them). In full screen: click a screen and press Alt+F4, for each.

Full screen ("run.bat fullscreen"): each emulator's window loses its title bar and border and is made to cover its
whole monitor, on top of everything (the taskbar and the hub's window are behind it). The picture is 4:3 with black
bars at the sides on a 16:9 monitor, at 50 fps. It isn't PCSX-Redux's own full screen: that switches the monitor to
60 Hz, and a full screen window that loses the focus minimises itself, while only one of the three can have it.

Recording: OBS's "Display Capture" (one per monitor, or the whole desktop) sees all three. A window covering the main
monitor exactly is shown by Windows without the desktop compositor, so an older "GDI" capture (BitBlt, also
Print Screen in some tools) shows a frozen picture on that monitor: use Display Capture.

For recording, the three windows have no menu bar: only the picture, at 0,300 to 1920,780 on the main monitor in the
default row (the title bars just above).

Files:
- sli.ps-exe      the demo program (the repo's demos/sli/sli.ps-exe, unchanged)
- sli_hub.py      the hub (the repo's demos/sli3/sli_hub.py, unchanged)
- sli_redux3.py   starts the hub and the emulators, and bridges each emulator's serial port to the hub
- .redux1-3\      each emulator's own settings, made from your usual PCSX-Redux settings each time (yours aren't
                  changed): its serial port on (6722, 6723, 6724, raw), the dynarec on, no debug windows, its window
                  placed. Each emulator asks the hub for its own place, so the windows are always in that order.

PCSX-Redux is found as in the repo's demo: PCSX_REDUX, on the PATH, the PSX.Dev extension's copy for VS Code, or a
C:\pcsx-redux-* folder (or give it: run.bat --redux C:\path\pcsx-redux.exe). Python 3, nothing to install.
