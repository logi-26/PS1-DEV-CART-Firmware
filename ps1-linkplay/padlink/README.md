# Pad play (CTR): the console driver

XBAND-style network play for Crash Team Racing (USA, SCUS_944.26), 2-4 consoles: only the players' buttons cross the
network; every console runs the same game in lockstep. Research: `../research/ctr/NOTES.md`.

## Files
- `padlink.c`, `padlink_head.S`, `padlink.ld`: the driver. `python build.py` -> `gui/data/netplay/crash_team_racing.json`
  (the Manager's package: the code, its gap's original SHA-1, the patched words with their originals, the Host / Join
  choices from `gui/ctr_package.py`) and `padlink_ctr.lua` (the same for the emulator).
- The computer's side is in the Manager: `gui/padrelay.py` (the relay: the lobby room, installing the driver, the
  game's setup at the main menu (`gui/ctr_package.py`), the frames, the state hash check), `gui/pages/netplay.py`
  (Host / Join / Start now for a package with `"kind": "padplay"`), `ps1transfer -xserve PORT` (usblink.c XServe: the
  kernel hook's exchange for the relay, on 127.0.0.1 only), the lobby server's rooms (`../lobby/netplay_server.py`,
  test `../lobby/tests/room_test.py`).
- Tests without consoles: `emu_relay_test.py` (lobby server + N emulators with `padlink_emu.lua`, the kernel hook's
  exchange through files + one relay each: `--players 3 --max 4 --leave 3:115 --settings JSON`), `page_test.py` (two
  Net Play pages in a stub app driving two emulators: host, join, Start now; screenshots `page_*.png`).
- `padlink.lua`: in PCSX-Redux, does what the computer does for a console: installs the driver at the main menu,
  fills the exchange area, sets the game up (`../research/ctr/ctr_setup.lua`), carries the frames (`hub.py`).
- `emu_play.py`: N emulators with the driver (`--players 2..4 --setup "{...}" --pad --extra ...`; `--stop`).

## The driver
- Code in GhostReplay_ThTick (0x80026ED8, 0x960 bytes; 2084 used): Time Trial's ghost replay, never run otherwise
  (GhostReplay_Init1 returns unless Time Trial; checked through the attract demo and every mode).
- Exchange area at 0x8000D810 (kernel hook's cheat table space; cheats stay off), 0x128 bytes:
  computer's part +0x00..+0x9F (magic "PLNK", players, me, delay, flags (1 on, 2 test buttons), seed, epoch, test
  buttons, have[4], buttons[4][16], abort), console's part +0xA0..+0xF7 (epoch_now, frame, out_upto, state 0 off /
  1 loading / 2 racing / 3 waiting, out[16], hash[8], waits, wait_loops, loads), then private (libpad's port 1 copy,
  racing, loading_seen, frame counter, rand state).
- Patches: 0x800262D0 (GAMEPAD_ProcessAnyoneVars) -> pl_frame_stub, 0x8003CB78 (a load's start) -> pl_load_stub,
  0x80078BE8 (BIOS rand) -> pl_rand, 0x80034ED4 nop (fixed frame step).
- While on: libpad's port 1 copy goes to the driver's spare buffer (pointer 0x8009ECDC), the game's buffer 0x80096AD4
  is a fake multitap with `players` pads; races start after each load (seeds, counters); each frame publishes this
  player's buttons for frame + delay, waits for the others' (calling the kernel hook's USB service, `pl_service`),
  publishes a state hash (positions, generators, clock).

## Emulator results (2026-11-17, input delay 2, 30 + 20 ms)
2P VS 900 frames; 2P Arcade (4 AI) with a pause -> Restart and a second race (1319 frames); 4P team Battle on 4
emulators (Skull Rock, 900 frames): the driver's state hash identical on every frame on all emulators.

Whole chain (lobby server, relays, driver installed through the exchange, emulators): 2P VS 1600 frames; a room for 4
started with 3 (Start now), 3P team battle, player 3 quitting at frame 613, players 1-2 on past frame 2000; the Net
Play page itself (two pages): host, join (Coco taken: not offered), Start now with 2 of 4: no desyncs anywhere.

## Not tested without consoles
`ps1transfer -xserve` over USB (the kernel hook's 'X': written then read after it, as vlink uses), the console's real
pad 1 through libpad's redirected copy, loading times differing between consoles (the state hash will tell), Sony's
BIOS. The user's running lobby server must be restarted (rooms are new). The attract demo: the relay waits for the
main menu (the player presses a button).

## Status (2026-11-18): on hold, waiting for CTR discs
Everything above is built and emulator-tested; the console test waits until the user has Crash Team Racing (USA,
SCUS_944.26) discs for two consoles. The first console test, when the discs are here:
1. Restart the Net Play lobby server on the NAS with the new `ps1-linkplay/lobby/netplay_server.py` (rooms: M/K/B/F).
2. Use the rebuilt `ps1transfer/ps1transfer.exe` (has `-xserve`) with the Manager on both computers.
3. Boot CTR on both consoles and leave it at the main menu (press a button if the attract demo is playing).
4. Manager, Net Play page: Detect the game; Host on one, Join on the other (free characters only), Start now if the
   room isn't full.
5. Logs: `%TEMP%\ps1devcart_netplay_<pid>\relay.log` on each computer (DESYNC lines = the state hash differed).
Watch for: the USB bridge, the real pad through the driver, loading times differing, Sony's BIOS, the frame rate.
