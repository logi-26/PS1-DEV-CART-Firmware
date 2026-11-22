# Virtual link cable (Red Alert Retaliation)

Link play without a link cable: the game's serial library is patched to a small driver (`vlink.c`) that passes the
link traffic through memory rings, and the rings are carried between the two consoles by something outside them: on
real consoles the computer, over each PS1 DEV CART's USB (`ps1transfer -vlink`); in the emulator a Lua script in each
PCSX-Redux.

Status (2026-11-13): works on BEN and BILL (firmware 1.4): link page, setup, game, leaving a link game (the other
console gets "connection lost" and returns to the menu). 21-22 game frames a second with some seconds at 25 (cable:
25). Discs: BEN Soviet (SLES_113.43), BILL Allies (SLES_013.43): the same code.
**Goal of the next session: 25 frames a second** (see Next).

## Running it on the consoles
1. Boot Red Alert on both consoles (from the cartridge menu) and wait for the game's main menu. **Nothing of this
   should be running while a console boots** (it was suspected of disturbing BILL's boot).
2. `python usb_link.py BEN BILL` (from this folder). It installs the driver in each game (checks first: the game's
   code, the driver's places, cheats off), then runs `ps1transfer -n BEN -vlink BILL` (`ps1transfer/ps1transfer.exe`
   of this repository, built with `buildtools/build_ps1transfer.sh`: the release's has no -vlink).
3. LINK GAME on both consoles. Ctrl+C stops it. Log: `usb_link.log` (each console's turns, bytes and game frames a
   second, every second).
- A new driver version (`vlink.c` changed) needs both consoles reset and the game booted again: it is never replaced
  under a running game (a game waiting inside it hung BILL). Changes to the relay only (`usblink.c`) need no reset:
  stop the script, rebuild ps1transfer, start it again.

## The driver (`vlink.c`, built by `build_hw.py SLES_013.43` into `vlink_hw.json`)
- Patch points (SLES_013.43; every serial access of the exe and of the DATA.MIX overlays goes through them): a `j`
  replaces the first two words of `_sio_control` (0x80154354), the sync read (0x80153DF4) and the sync write
  (0x801540D8) of Sony's serial library. (`build_hw.py` needs the exe extracted from the disc: `research/tools/
  isoextract.py`.)
- Its code goes in three gaps of that library the patch makes dead: 0x80153DFC (read), 0x801540E0 (write),
  0x801548E4 (the library's SIO interrupt handler, never called: its check needs a SIO interrupt, not enabled here).
  Its rings (128 bytes each) at 0x8000D810, the kernel hook's cheat table space after its magic word (cheats stay off).
  **No RAM of the game is used**: RAM that read zero during the game wasn't free (0x801F3000: BILL's screen went
  blank at once, twice).
- Layout (`VLinkBuf`): the computer's part (rx_head, tx_tail, ln_head, credit_in, abort, ln_ring[16], rx[128]) then
  the console's part (rx_tail, tx_head, ln_tail, my_lines, saved_lines, peer_lines, peer_seen, credit_used, waiting,
  tx[128], stats[8]): `-vlink` writes the first and reads the second (without stats: 183 bytes, exactly 3 USB packets)
  in one exchange.
- Records in the rings: `[0, len lo, len hi, bytes]` data, `[1, lines]` DTR/RTS change, `[2, n lo, n hi]` credit (a
  read wants n bytes), `[3, n]` credit taken back.
- Like the cable: a write sends only what the other side's reads asked for (credit: the setup screen sends its
  settings every frame; without it the copies piled up and were read as game messages: "connection lost"); line
  changes are given one at a time, each after the game has looked (the link page's one-frame RTS pulse).
- On the console it runs the kernel hook's USB service itself (pointer at 0x8000DF74, interrupts off) while the game
  waits, so a request is answered at once rather than at the next interrupt.
- Timeouts: a wait ends by the game's own poll callback, or when the computer raises `abort` (it measures real time:
  a console waiting 1.5 s with nothing coming, then 0.1 s while the other side stays silent: the game retries many
  times on its way out). Not by a console timer: GPUSTAT bit 31 changes every scanline on a PS1 in 240-line mode
  (every frame in Redux), so each wait gave up at once and BILL started a game on garbage.

## The relay (`ps1transfer -vlink`, `ps1transfer/usblink.c`)
One thread per cartridge, one 'X' exchange per turn (the kernel hook's: one USB write, answered without waiting).
- Second headers: each game frame both consoles exchange a 6-byte header (`BD DA len AF DE`) twice, then the payload:
  6 one-way messages in turn, each crossing the computer. The second header is always the first's copy (checked in
  every recorded game). When a console asks for 6 bytes just after the other's header came, it gets the copy at once
  (its request still goes to the other console, whose second header is then dropped, checked equal): almost 4
  crossings a frame. Only on request: given in advance, a copy nobody asked for (leaving a link game) shifted every
  message after it, and BILL, reading garbage, wrote over its own interrupt callbacks (crash returning to the menu).
- Pacing: turns one after another while the console waits in the driver (its `waiting` flag seen in the last 20 ms)
  or something is to be given to it, else one every 5 ms: each exchange answered in an interrupt stops the game.

## Speed (game frames a second; cable 25)
| Change | Frames/s |
|---|---|
| Python carrying with `-memlive` (it sleeps ~15 ms between commands on Windows) | 1.8 |
| `-vlink`, 512-byte rings (1.1 KB a turn) | 12 |
| 128-byte rings | 19 |
| Second-header shortcut | 21-22, some 25 |
| Two exchanges always on their way | 18 (worse: more exchanges answered in interrupts) |
| Pacing by the console + the copy only on request | built, emulator-tested, **not yet run on the consoles** |

Why not 25 yet: each game frame (40 ms at 25 a second: 2 vertical blanks) needs about 4 one-way crossings, each at
least one USB turn on each side (about 2 ms, turns of 1.4-2.2 ms measured); a frame that misses its 40 ms takes 60.

## Next: 25 frames a second
1. Run the paced relay on the consoles and measure (BILL needs a reset after its last crash; BEN doesn't).
2. Measure where a frame's time goes on the console (the driver's stats, or a timer in the driver: time spent
   waiting per frame vs the game's own work) to know the slack there is.
3. Shorter turns: the console answers the exchange byte by byte (CRC per byte): a faster copy loop in the driver's
   own service (it can read the FT245 FIFO itself), or exchanges sized to the data actually waiting.
4. Answer the payload's crossing sooner: the relay thread waking the other cartridge's thread at once (an event
   instead of its next turn).
5. The game's own timing: if it waits for a vertical blank after the exchange, the exchange finishing before the
   blank is what counts (find where it waits: 0x800B20B8's loop).

## Emulator (`vlink.lua`, `run_pair.py`)
- `python run_pair.py --hw` starts two Redux with the console build (`--only A`: one; plain: the older RAM build at
  0x8000C000); `python run_pair.py --stop` closes them. Each window is `pcsx-redux.main`, a child of
  `pcsx-redux.exe`: both are recorded and closed (killing only the launcher left the windows open).
- Logs `.vlink_a/vlink.log`, `.vlink_b/vlink.log`: stats, game frames, where the CPU is each vertical blank, calls
  into the library's other entries. A `ctl.lua` next to a log is run once each time it changes.
- Redux: keep the handle `PCSX.addBreakpoint` returns (garbage-collected: the breakpoint goes); a frame is run in a
  burst then it sleeps, so the script holds the emulator in real time while the game waits (15 ms a frame at most);
  it doesn't emulate the I-cache, GPUSTAT bit 31 per scanline, or interrupt costs: console behaviour can differ.

## Files
`vlink.c`, `vlink_head.S`, `vlink_hw.ld`, `build_hw.py` (console build), `build.sh` (emulator RAM build),
`vlink.lua`, `run_pair.py`, `usb_link.py`; ps1transfer: `usblink.c` (VlinkRelay), `main.c` (`-vlink`,
`USB_ExchangeSend/Recv`), `usblink.h`. None of it is committed yet.
