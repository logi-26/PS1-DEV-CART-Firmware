# Destruction Derby (SCUS_943.02, US): pad play over the network (research)

Idea (user, 2026-11-17): XBAND-style. Each console runs the game; only the pads cross the network; both consoles
apply each frame's pads a few frames later (input delay), in lockstep. No link-cable support needed.

Disc: `Desktop\ps1usb\Destruction Derby` (track 1 data, 17 MB; tracks 2-20 CD audio = the music).
Extracted to `disc/` (tools/isoextract.py). `exedis.py callers ADDR...` / `exedis.py dis FROM TO`.

## Found (static, from the executable)
- Exe: load 0x80010000, size 0xAC800, entry 0x800ABC9C. Sony libraries (C) 1994-95.
- Pads: BIOS InitPAD (B0h 12h, stub 0x800AAEBC) called once at 0x80040D8C:
  `InitPAD(0x801E72EC, 0x22, 0x801E730E, 0x22)`: port 1's buffer 0x801E72EC, port 2's 0x801E730E (34 bytes each,
  byte 0 status, byte 1 type, +2 buttons, active low). StartPAD (B0h 13h) at 0x8003F710. The BIOS fills them in the
  vertical blank interrupt.
- The game's pad routine 0x80040E18: type check (4xh digital / 2xh), buttons from +2 (inverted), held / pressed /
  remapped (the "choose your pad" layouts) into 0x800E0680.. (and 0x801D0750.. for the second path). The buffers are
  read in only a few places (0x80040E30.., 0x80040F4C.., 0x80099814, and 0x801E7330 just past port 2 at 0x80039264,
  0x8009E180, 0x8009E248: to check).
- Random numbers: the BIOS rand / srand (A0h 2Fh / 30h). The only srand: `srand(0x5427)` at 0x80023774 (a fixed
  seed). rand is called from ~35 places (0x8001xxxx, 0x80025xxx, 0x80027xxx, 0x8009xxxx...).
- REPLAYS.RAW on the disc: replays may be stored inputs (a deterministic engine) or positions: to check.

## Open questions (Redux next)
1. Does the race logic advance once per game frame (per pad read), or by the vertical blanks elapsed (a variable
   step: two consoles drawing at different speeds would then compute different races)?
2. Is rand called anywhere in a loop that depends on time (waiting for the CD, the menus' idle loops)? Its sequence
   must be the same on both consoles from the race's start.
3. Determinism test without a network: the same pads replayed twice in Redux from the same start -> the same RAM
   (cars' positions) frame by frame?
4. Where to hook: after the BIOS fills the buffers (its interrupt) and before 0x80040E18 reads them; the race's
   frame loop is the point both consoles wait for each other.

## 2026-11-17: no two-player mode on one console (user), but link-cable play
The first menu after START: one PlayStation (DOWN: two PlayStations joined by a cable = link play; X there starts
the link and the game stops reading the pads). No split screen: pad play (both pads into one game) doesn't apply.
Destruction Derby is a link-cable game instead: a candidate for the existing Net Play (vlink) path, like Red Alert.
Harness kept: dd_run.py / dd_auto.lua (pads injected at 0x80040E18, read counter, ctl.lua, shots), nav.sh, png.py,
det_test.lua (determinism run: not finished, stopped).
