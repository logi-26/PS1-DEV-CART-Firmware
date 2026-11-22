# TOCA 2 Touring Cars (Europe, SLES_015.47): pad play over the network (research)

Disc: `Desktop\ps1usb\TOCA 2 Touring Cars` (the user's PAL disc, Italian / Spanish: language chosen at
boot). Root files and the two programs extracted to `disc/` (SLES_015.47, FRONTEND.EXE, GAME.EXE, ENDING.EXE).
Emulator: `../dd/dd_run.py t --game toca2` (folder `../dd/.redux_t`), `../dd/nav_t.sh BUTTON WAIT SHOT` (press on pad
1, `2:BUTTON` on pad 2, then a screenshot). Tools: `../tools/exetool.py`.

## The programs
- SLES_015.47 (boot, loads at 0x80012600) and GAME.EXE (0x80012600-0x800D6E00, pc 0x800CB290): the race. In a race
  the RAM's code is GAME.EXE's (99 % identical; SLES_015.47 is a different build of the same area).
- FRONTEND.EXE (0x80128000-0x801E4000): the menus, its own pad driver (0x801C0320.., buffers 0x801E50D4 / raw
  0x801E7D10).
- No BIOS InitPAD use (the library's InitPAD wrappers are never called): the game has its own SIO pad driver.

## Two players
Main menu (MENU GARA) -> CORSA SINGOLA (DOWN x3) -> GIOCATORI 2, QUALIFICHE NO -> CONTINUA (TIPO DI TEMPO is skipped)
-> name for player 1 (pad 1: X, START) -> name for player 2 (**pad 2**: X, START) -> track (Thruxton) X ->
difficulty X -> load -> IMP. AUTO (car setup, in GAME.EXE): player 1 FATTO (X), player 2 INIZIA CORSA (**pad 2** X)
-> the race starts at once (no load), split screen, 6 CPU cars. Save state of that last screen:
`../dd/.redux_t/prerace.sst`.

## The race (GAME.EXE)
- $gp = 0x800D5A60. [gp+0x408] (0x800D5E68): the frame function's state (1 in the car setup and the race).
- **Frame function 0x8004B3D0**: first thing `jal 0x800A6024` (0x8004B3DC) = read all pads; then a jump table on
  [gp+0x408]. **The hook for pad play: 0x8004B3DC.**
- 0x800A6024 calls 0x800A5D5C(port 0..7) (multitap aware): raw buffers 0x800E0374 (port 1) and 0x800E0396 (port 2,
  +0x22): status, type<<4|size (0x41 digital, 0x73 analog, 0x23 neGcon), buttons active low (+ analog bytes); decoded
  by 0x800A5B3C(type, buf+2, out) into per-pad structs at 0x800E017C + port * 0x38 (+0 type, +4 held, +8 pressed
  edges, +0x18.. analog). The SIO driver fills the raw buffers twice per vertical blank (0x800B670C), the raw replies
  at 0x800E2FB0.
- Game frames: 45 per 120 vertical blanks in the 2P race (~22 fps). Pacing 0x800A0E70: waits until VSync(-1) reaches
  [gp+0xA90]; the next target = now + [gp+0xA98] (set by 0x800A1460 from 0x8004B588: 2, or 1 when a flag is set);
  lateness in [gp+0xA94] (getter 0x800A0CAC never called).
- **Variable time step (found 2026-11-21)**: the pacer stores GetRCnt(root counter 2) - previous, scaled
  (0x800A1488), in [gp+0x12E4]; 0x8009FE90 (each frame, from 0x8004B89C / 0x8009D1A8) sets the step [gp+0xA80]
  (0x800D64E0) = clamp(0x800A14D0() = [gp+0x12E4], 0x1000..0xFF000) and the race clock [gp+0xA84] += step; 14
  readers of the step (getter 0x8009FEFC). Measured ~0x29940-0x2AA60 per frame (wobbles ~1 %).
  **Patch for lockstep: 0x800A14D0 = lui v0, 2 / jr ra / ori v0, v0, 0x8000** (a constant 0x28000 = 40 ms, two PAL
  vertical blanks; a frame drawn slower then plays slower, as in CTR).
- Random: libc rand 0x800C56E8 (state 0x800E3650), srand 0x800C5718. **Race start: 0x80074718** seeds the random
  table (0x8009E2D0(seed): srand(seed), 250 rand() into 0x800DFC48) with the halfword at 0x800F0970 (0xF214 here),
  right after player 2's INIZIA CORSA. Other seedings: 0x8004B644 / 0x8007ECD0 (rand(), or a fixed seed 0x29A /
  [gp+0x79E] when [[0x80012600]+0x3F] is set: replays?).
- **Time-mixed random 0x8008F6B8 = VSync(-1) + rand()** (callers 0x8008E620, 0x8008E768, 0x8008EB78, 0x8008EBC0,
  0x8008F278): must be made deterministic (VSync(-1) -> 0 or the frame number).
- Seeds from VSync at loads: 0x800A4258..0x800A4270, 0x800A4AC0..0x800A4AD8, 0x800A4EB8..0x800A4ED0 (srand(rand ^
  VSync(1) ^ VSync(-1))).
- [gp+0x400] / [gp+0x404]: a flashing colour (120..255, +-10 a frame): cosmetic. 0x800E53F0: root counter 2 copy
  of the SIO pad driver: harmless.
- Pause menu (pad 1 START): ESCI DA CORSA -> SI -> VUOI CORRERE ANCORA? SI -> back to the car setup (its seeding
  0x8007ECD0 = rand()) -> FATTO / INIZIA CORSA starts a new race (seeding 0x80074718 again): races can be repeated in
  one session.
- Menu key repeat 0x80049F68..0x8004A150 uses VSync(-1) (pause menu): harmless unless menus are in lockstep.

## Determinism test (2026-11-21): `toca2_det.lua`, `det_run.py`
Load prerace.sst, start the race at once (A) or 37 vertical blanks later (B); race seed forced to 0x1234; pads
scripted per game frame from the race's start (raw buffers written at 0x800A6024); 0x8008F6B8's VSync given 0; rand
calls logged; page hashes 0x800D5000-0x800FFFFF each frame to 320; shots / RAM at 300, 600, 900.
- First result: no rand call before frame ~324; the screens differ slightly at frame 300 (speeds 61 / 62, 7 / 10):
  something depends on timing. Page hashes: 0x800D5000, D6000, E5000, FD000 differ from frame 0 (timing counters?),
  D7000, DE000, F0000, F5000 from frame 3.
- Redux closes (crash dump in .sentry-native) loading prerace.sst most of the time (from the intro, the menus, at
  once after start; it worked ~10 times from inside a race, then not at all): save states abandoned.
- Same session, same delay, back to back: A / A2 identical to frame 900 (RAM); but a run's result also depended on
  the run before it (B vs B2 differed): the time step below.
- A vs B (different delays) without the patch: the step [gp+0xA80] 0x43000 / 0x44000 at frame 16 etc.

## Determinism test with the fixed step (2026-11-21): `toca2_cycle.lua` (no save states)
From a running 2P race (`../dd/nav_toca2_race.sh`), each run: pause -> leave -> race again -> car setup -> wait
DELAY -> INIZIA CORSA (pad 2); race seed 0x1234, pads scripted per frame, 0x8008F6B8 VSync 0, the step patch.
Runs A (30), B (67), A2 (30), B2 (67), one after the other:
- **B, A2, B2 identical**: rand state at frames 600 / 900 the same (5cc07ee1 / 76614b25), screens at 900 the same
  cars, positions, speeds, race clock (only small colour shading differs: lighting / flashing colour).
- **A differs** (the first race after the patch went in, its car setup reached from a race played with the
  emulator's pads and the variable step): something from before the race start carries into it. To find: what the
  car setup / race start keeps from earlier (the car setup's seeding 0x8007ECD0 = rand(), AI / grid state?).
  The page hashes can't show it (every race starts with the previous race's leftovers in RAM).

## History tests (2026-11-21/22): what a race keeps from before its start
`toca2_cycle.lua` options per run: patch = false (original step code back), pads = false (nobody drives),
frames = N; SEED_SETUP (the car setup's seeding 0x8009E2D0 from 0x8007ECD0 given 0x4321), transplant = {lo, hi}
(RAM from ref_f0.ram at frame 0), every rand call logged ('r frame ra state'), callbacks protected (a Lua error in a
breakpoint pauses Redux). Reference result (seed 0x1234, setup seed rand(), the scripted pads): rand state
5cc07ee1 at frame 600, 76614b25 at 900.
- Without SEED_SETUP, F1 (unpatched, idle) -> M1 / M2 / F2 (patched, idle) -> M3 / F3 (unpatched, scripted) -> M4:
  M1, M2, M3 all different; M4 = M2 (e4051834). The earlier B / A2 / B2 match was partly luck.
- With SEED_SETUP: M3 (after a patched idle race) = the reference; M1 (after an unpatched idle race) and M2
  differed; a repeat of F1 -> M1 matched the reference at 600 (then Redux closed): not reproducible = something
  outside the game's frame logic.
- Order of rand / srand through a restart (`rand_order.txt`): car setup: setup seeding, then srand(rand ^ VSync(1) ^
  VSync(-1)) at 0x800A4ED8 and 0x800A4278 (loads), ... race seeding 0x80074728 later: the race's sequence starts
  clean. All rand calls run inside the frame function (0x8004B3D0, returns to 0x8006BCE4) with interrupts on.
- Other real-time reads: GetRCnt at 0x8005499C / 0x80062944 / 0x8007AC90 only store CPU time values (0x800D5F30/34,
  0x800DAC70, never used: profiling); the other VSync(-1) calls are library timeouts. No other leak found statically.
- **Found: the sound module uses the game's rand** (GAME.EXE 0x8008B000-0x80092000): e.g. 0x8008CA4C asks whether
  an SPU voice has ended (0x800ABD88: voice table 0x800E0C75, 7 bytes x 24 voices) and only then calls rand()
  (0x8008CA60) to pick sound effect 0x10B + rand % 3 (0x800ABDC0 plays it). When a sound ends follows the audio,
  not the frames, so the game's random sequence moves at audio-dependent moments and the race (AI etc.) diverges.
  rand calls in that module: 0x8008B28C, 0x8008C73C, 0x8008CA60, 0x8008CAE8, 0x8008D0C0, 0x8008D19C, 0x8008DF54,
  0x8008E028, 0x8008F6CC (in 0x8008F6B8 = VSync + rand), 0x8008F9A0, 0x80091DA0, 0x80091F88 (the region assumed to
  be all sound: to confirm).
  **Fix (written, NOT yet tested): SOUND_RAND** in toca2_cycle.lua: those jal rand -> a copy of rand with its own
  state (test stub at 0x8000D000, state 0x8000D040: lui t0, 0x8001 / lw v0, -0x2FC0(t0) / ... ).
- Crash-proof runner `cyc_driver.py GROUPS.json --lua "..."`: each group in a freshly booted Redux driven into a 2P
  race (`../dd/nav_toca2_race.sh`), retried when Redux closes; results in `../dd/.redux_t/groups/<group>/`.
  groups1.json (FIXED_STEP + SEED_SETUP, no SOUND_RAND): **G1 = G2** (fresh sessions, same history: identical,
  618ac5e9 at 600, e0d4fc63 at 900: the emulator itself repeats). G3 / G4 (an extra patched idle race first) were
  stopped unfinished (the user asked to stop: it was taking too long).

## Next (when resumed)
1. Run groups1.json again with `--lua "FIXED_STEP = true; SEED_SETUP = true; SOUND_RAND = true"` (~20 min): G1-G4
   should then all match whatever the history. If not, compare the runs' 'r' lines (first rand call that differs).
2. Confirm the 0x8008B000-0x80092000 region is only sound (look at each of its rand callers).
3. The driver (CTR's padlink.c as the model), patches: the step (0x800A14D0), the sound rand (own generator), the
   race and car setup seedings (same seed on both consoles), 0x8008F6B8; hook 0x8004B3DC (read all pads) for the
   2 pads at 0x800E0374 / 0x800E0396; find spare room in GAME.EXE; the menus' setup (2 players, track, names).
4. Redux: save-state loads crash in this game (don't use them); Lua runs crash it every few races (the driver retries).
