# CTR - Crash Team Racing (USA, SCUS_944.26): pad play over the network (research)

Disc: `Desktop\ps1usb\CTR_ Crash Team Racing` (one data track, 605 MB: BIGFILE.BIG 270 MB, XA music
streamed, TEST.STR video). Root files extracted to `disc/` (tools/isoextract.py; only files under 8 MB).
Tools: `../tools/exetool.py EXE info|callers|dis|refs|find`. Emulator harness: `../dd/dd_run.py a --game ctr` (both
pads connected; no pad hook: `hold(name, n)` = the emulator's pad 1 override, works in CTR; pad 2:
`PCSX.SIO0.slots[2].pads[1].setOverride`).

## Found (2026-11-17)
- Pads: Sony's libpad (no BIOS InitPAD), the driver at 0x8007C9E0-0x8007E9xx (port registers through pointers at
  0x8008C7E0/E4). Raw receive data: port 1 0x8009F60B.. (00, type 73 = Redux's analog pad, 5A, buttons x2 active
  low, 4 analog bytes), port 2 0x8009F62E.. (00, 41 digital, 5A, buttons). The driver's interrupt (0x8007E488 ->
  0x8007F204/0x8007F210) copies them each vertical blank to the library's buffer the game reads: 0x80096AD4..
  (player 1: type at 0x80096AD5, buttons 0x80096AD6), player 2 at 0x80096AF9.. (stride 0x24).
- The game's pad code: 0x80025410 (called from 0x80034B38; base 0x80096804 + 0x2D0 = the buffer) runs every
  vertical blank (600 in 600); 0x80025738.. (from 0x800262E8) runs once per game frame.
- Game frames in a 2-player battle (Nitro Court, split screen): 34-36 per 60 vertical blanks, not a fixed rate.
  In the menu: 30 (60 per 120).
- Random numbers: BIOS rand (A0h 2Fh) present but no srand; the game likely has its own generator: to find.
- Menus: main menu ADVENTURE / TIME TRIAL / ARCADE / VS. / BATTLE / HIGH SCORE (Battle = DOWN x4), players 2P/3P/4P
  (needs pad 2 connected), character select (each pad X), level, setup (START BATTLE). The main menu times out to
  the attract intro.

## The game's time (2026-11-17)
- $gp = 0x8008CF6C; the game's main structure at [$gp+0x340] (called gGT below).
- Per vertical blank, 0x80034AA4 (runs every blank): gGT+0x1CE0 / +0x1CE4 counters, GetRCnt(timer 1: lines)
  added to a total at 0x8008D988 ($gp+0xA1C), ResetRCnt; then the pad code 0x80025410.
- Root counter library: SetRCnt 0x80077B48, GetRCnt 0x80077BE4, StartRCnt 0x80077C1C, StopRCnt 0x80077C4C,
  ResetRCnt 0x80077C80.
- Time now 0x8004B3A4: (total + timer 1) * 1000 / ~5245.6 = 1/3000 s units (NTSC lines 15734/s). Time since
  0x8004B41C(then, &store).
- **The frame's step**, once per game frame at 0x80034EA8: since = time since gGT+0x1D08 (updated); step =
  since * 32 / 100 (~960 a second: 32 = 1/30 s); negative -> 32 (0x80034ED4 bgez / 0x80034EDC); > 64 -> 64; paused /
  loading (gGT+4 & 0xF) -> 32; stored at gGT+0x1D04 (67 readers in the exe; gGT+0x1D00 += step). Race start
  0x8003CB7C sets 32. The overlays (BIGFILE.BIG) never call the time functions or GetRCnt.
- **Patch for lockstep: 0x80034ED4 = 0 (nop)**: the store of 32 always follows (fixed step of 1/30 s, the same on
  both consoles; a console drawing slower than 30 fps then plays slower instead of taking bigger steps).
- gGT+0x1CE4 (blanks counted in real time) is read by 0x8002E934 (a pulsing value 100-164: looks visual),
  0x80032F94 (a 3-blank wait), 0x80031FC8 / 0x800379B8 (bookkeeping): to confirm harmless in the determinism test.

## Random numbers (2026-11-17)
- No standard generator constants in the exe. The BIOS rand (stub 0x80078BE8), never seeded (no srand), is called by:
  0x80050848 (weapon crate, battle mode flag 0x20: rand % 14, 5/8/9 remapped) and 0x800508AC (other modes: rand
  % 12); in BIGFILE.BIG at +0x688E550 and +0x688EEA8 (an overlay: an object created by 0x80030A50, its angle
  rand % 4096).
- **For lockstep: both consoles srand (A0h 30h) the same seed at the sync point** (the BIOS's state otherwise
  depends on power-on and every earlier crate). Any other generator of the game's own would show in the
  determinism test.

## Open
1. Frame pacing: 0x80025738.. counted 34-36 per 60 blanks in the battle, while the main loop seems to wait 2 blanks
   a frame ($gp+0x348 = 2 at 0x80037934): with the fixed step, is the battle's speed right?
2. Determinism test: pads injected per game frame (at 0x80025738's frame, into 0x80096AD6 / 0x80096AFA), the same
   battle twice with different menu timing, RAM compared.

## Determinism test (2026-11-17): passed with the fixed frame step
`ctr_det.lua` (run by `../dd/dd_run.py X --game ctr` + ctl.lua `TAG, DELAY, PATCH = ...; dofile(ctr_det.lua)`):
Battle, 2P, Nitro Court through the emulator's pads; from the battle's start (0x8003CB7C) both pads scripted by game
frame (written into each player's raw buffer at 0x80025738: a1 = 0x80096AD4 / 0x80096AF6; status 0, type 41h, +2
SELECT..LEFT, +3 L2..SQUARE, active low); RAM dumped at frames 300/600/900 at the frame's start (0x80034EA8); rand
calls logged (frame, caller). Four runs: A/B no patch, C/D patch; B/D 37 vertical blanks later at every menu step.
- rand: A vs B differ from the 32nd call (frame 236 vs 238); C vs D: all 136 calls the same (frame and caller).
- Screens (det_grid.png: rows A-D, frames 300/600/900): A vs B 31% / 67% / 63% of pixels differ; C vs D 1.2% /
  1.1% / 1.1%, only the karts' exhaust smoke (det_cd_900.png): cosmetic, probably the real-time blank counter.
- Pacing (blanks between frames 300-600-900): C 921 / 901, D 920 / 901 (A 930 / 860, B 888 / 924).
- RAM: ~150 KB differ in 0x800C0000-0x800EFFFF in both pairs (graphics packets, timing); ~18 KB elsewhere even C vs
  D (pad library state, the clock, sound...): not yet sorted into timing-only vs game state; the rand order and the
  screens say the game state matches.
- Both runs' rand sequences matched without srand here (same BIOS, same boot): on consoles srand at the sync point.

## Next steps (2026-11-17)
1. Lockstep prototype with two emulators: two Redux, each pad sent through a small Python relay to the other, with
   an artificial delay (2-3 frames) and jitter: the real sync (input delay, waiting for the other side, the seed at
   the sync point), and the user plays it to judge the delay.
2. Where lockstep starts: at the CTR main menu (the character / level menus then in step too) or at the battle
   (both consoles' choices synced another way first): the prototype shows which is simpler.
3. The console driver: in the game's spare RAM (the kernel hook is full): the frame step patch (0x80034ED4 = nop),
   srand at the sync point, and at 0x80025738 each frame waiting for both players' buttons from the computer.
4. The Manager and the lobby: pad frames instead of link traffic (Host / Join almost unchanged).
Still open from the test: sort the ~18 KB of RAM that differs C vs D into timing-only and game state.

## Lockstep prototype, two emulators (2026-11-17): works
`lockstep.lua` (each emulator; TAG, ME 1/2, DELAY, INPUT_DELAY, SOURCE 'script' or 'pad') + `relay.py` (the network:
each one's ls_out.txt to the other's ls_in.txt after a delay + jitter, in order). From the battle's start: each frame
f, this side's input for f + INPUT_DELAY sent, the other's for f waited for (emulated time stands still meanwhile),
both written into the raw pad buffers at 0x80025738; the fixed frame step patch; the first INPUT_DELAY frames empty.
Run order: emulators, then relay.py (it empties the files), then ctl.lua in both.
Run 1: both scripted, input delay 3 frames, 30 ms + up to 20 ms jitter one way, B 37 blanks later at every menu step:
- inputs applied frames 0-900: identical; rand: 139 / 139 identical; screens at 300 / 600 / 900: 0.0% differ (even
  the exhaust smoke: the emulators wait for each other every frame) (lockstep_grid.png).
- Waits: A 5.2 s at the start (B still loading), then none; B 105 frames, 1.1 s over 900 frames (30 s of play).
Not yet: someone playing (SOURCE 'pad': each emulator's pad 1; two windows on one computer, each needs its own
controller / keyboard focus), srand at the sync point (both emulators boot the same BIOS state here), where lockstep
starts (here at the battle, both menus scripted the same).
Played by someone (2026-11-17): `python play.py [--input-delay 2] [--delay 30] [--jitter 20] [--minutes 10]`
starts both emulators, the relay and lockstep.lua: A = you (its pad 1: the raw buffer 0x8009F60E/F, read at each
frame's start), B = the scripted player 2. `python play.py --stop` closes them. Checked: emulator A's pad (CROSS held
through the emulator) reached both sides' frame logs on the same frames. (ctl.lua only runs when its text changes.)

User's play test (2026-11-17): 3 frames of input delay (100 ms) was compared with 2 frames (67 ms), 30 ms + up to
20 ms jitter: **2 frames feels better**. 2 is now the default (play.py, lockstep.lua) and the target for the console
driver. With 2 frames, a round trip (both ways plus the relay) above about 67 ms makes a side wait for the other; on
longer connections the driver could choose the delay from the measured ping (2 up to ~60 ms round trip, then 3, 4).

## 2-4 players, game set up from the Manager (2026-11-17)
User's wishes: VS Single / VS Cup / Battle for 2-4 players, Arcade Single / Cup for 2 (with the AI racers), the game
set up in the Manager (host popup: mode, players, level / cup, laps, battle settings; joiners pick a free character),
and the host can start before the lobby is full (a 4-player battle started with 2 = a 2-player battle).
Sources: CTR-ModSDK (github CTR-tools/CTR-ModSDK): its build "926" = this disc (symbols/gcc-syms926.txt match, e.g.
GAMEPAD_PollVsync 0x80025410); include/namespace_Main.h (GameTracker), regionsEXE.h (data/sdata), decompile/General/230
(main menu overlay), BOTS/.
- **Fake multitap** (`ctr_mt.lua`): the game's pad system (base 0x80096804) reads port 1's buffer 0x80096AD4: byte 0 = 0
  and byte 1 = 0x80 -> multitap, 4 slots of 8 bytes from +2 (status 0, type 41h, buttons active low; status FF = none).
  Written each vblank (GAMEPAD_PollVsync 0x80025410) and each frame (0x80025738): 3P / 4P become selectable and work.
  Redux has no multitap and no other PS1 emulator is installed; the console driver fakes it the same way.
- **Game started straight from the main menu, no menus** (`ctr_setup.lua` setup{}): every menu path ends with
  sdata->ptrDesiredMenu (0x8008D924) = &data.menuQueueLoadTrack (0x80085A94), which loads gGT->currLEV. Written first:
  gGT ([0x8008D2AC] = 0x80096B20) +0 gameMode1 (BATTLE 0x20, ARCADE 0x400000, limits POINT 0x4000 / LIFE 0x8000 /
  TIME 0x10000), +8 gameMode2 (CUP_ANY 0x10, cheats cleared), +0x1CA9 players, +0x1D33 laps, +0x1EB0 level, +0x1EBC
  arcade difficulty (0x50/0xA0/0xF0), cup +0x1E58 id / +0x1E5C track / +0x1E60 points (first track from data.ArcadeCups
  0x80084148, 18 bytes each), characters data.characterIDs 0x80086E84 (8 shorts), battle: +0x1D84 time (minutes *
  0xE100, -1 = forever), +0x1D88 lives, +0x1D8C points, +0x1DA0 weapon bits (11: 0x34DE all), +0x1DA4 team per player,
  +0x1DD8 team flags, +0x1DDC teams, +0x1DF0 / +0x1DF4 weapon list (as MM_Battle_MenuProc's START BATTLE computes).
  Level IDs: races 0-17 (menu order: 3 Crash Cove, 6 Roo's Tubes, 4 Tiger Temple, 14 Coco Park, 9 Mystery Caves,
  2 Blizzard Bluff, 8 Sewer Speedway, 0 Dingo Canyon, 5 Papu's Pyramid, 1 Dragon Mines, 12 Polar Pass, 10 Cortex
  Castle, 15 Tiny Arena, 7 Hot Air Skyway, 11 N. Gin Labs, 13 Oxide Station, 16 Slide Coliseum, 17 Turbo Track),
  arenas 18-24 (Nitro Court, Rampage Ruins, Parking Lot, Skull Rock, North Bowl, Rocky Road, Lab Basement).
  Checked in Redux (screens + RAM): VS 4P 5 laps, Battle 3P life limit 6 lives 6 min teams 0/1/1 4 weapons, VS Cup 2P,
  Arcade 2P single (hard) and cup (easy) with 4 AI racers.
- **More things that must be the same on every console** (found with 2P Arcade, which went out of step):
  - gGT+0x1CEC (frames since power-on; read by BOTS_ThTick_Drive, VehPhysGeneral_JumpAndFriction) and gGT+0x1CF8
    (VehPhysCrash_AnyTwoCars): set to 0 at each load's start and each race's start.
  - The game's own generators: RngDeadCoed (0x8006C684) with states 0x8008D668 (8 bytes: bots' paths / starts /
    driving, items, potions; also advanced by the menus) and gGT+0x252C (particles, sparks); MixRNG ($gp+0x4B8 =
    0x8008D424, 16 bits: held items, bots, particles). Seeded at each load's start (the bots get their paths
    (sdata->driver_pathIndexIDs 0x8008CF78) while the level loads, BOTS_Adv_AdjustDifficulty).
  - The BIOS rand (stub 0x80078BE8): replaced (stub = jr ra / nop, v0 from our generator): Redux runs OpenBIOS, consoles
    Sony's BIOS; the driver gives its own seeded generator on any BIOS.
  - Cosmetic only (allowed to differ): exhaust smoke, water / texture animation, HUD icon flicker, speedometer needle
    (driver +0x36E), sound pointers (driver +0x300..): the real-time blank counter.
- **N-player lockstep** (`lockstep_n.lua` + `hub.py` + `play_n.py`): frame point = GAMEPAD_ProcessAnyoneVars
  0x800262D0 (once per main-loop pass: races, the pause menu and menus; the earlier 0x80034EA8 stops while paused ->
  stuck in the pause menu). Races ("epochs"): a load's start (0x8003CB7C or the LOADING flag 0x40000000) ends one and
  seeds; the first frame after the load starts the next one (frame 0, the first INPUT_DELAY frames empty), so a
  restart / the next cup race / anything that loads re-synchronises all consoles. Lines "race player frame buttons".
  Logs: ls_frames.txt, ls_hash.txt (each driver's struct hashed per frame, --hash N), statecmp.py (drivers compared in
  RAM dumps), raminfo.py (settings in a dump).
- Results (input delay 2, 30 ms + up to 20 ms, emulators starting 37 / 80 / 120 blanks apart, all players scripted),
  every driver's hash identical on every frame:
  - 4P VS (Mystery Caves) on 4 emulators, 900 frames; 3P Battle (Rampage Ruins, teams) on 3, 900 frames;
  - 2P Arcade (Coco Park, hard, 4 AI) 900 frames, also the RAM at 300/600/900 and all rand calls;
  - 2P VS with a pause -> Restart at frame 418, then race 2: 1320 frames identical across both races.
  `python play_n.py --players 3 [--setup "{...}"] [--pad] [--hash 900] [--extra "{{race, frame, player, 'START'}}"]`,
  `python play_n.py --stop`.
- Not yet: a cup's next race after a finished race (same load mechanism as Restart; scripted players don't finish
  laps); quitting to the main menu (a load: the lockstep goes on in the menus; the Manager's next setup must then be
  applied on the same frame everywhere, e.g. "setup at race e frame f"); unlocked characters / tracks differ between
  memory cards (offer only the standard 8 characters and the open tracks, or anything since the setup loads any).
- On consoles: the loading takes a different time; counters are reset when the race starts, but whatever the game
  computes from them during the load (MainInit_FinalizeInit reads gGT+0x1CEC) must be checked with the state hash.

### Next steps (2026-11-17)
1. Console driver (spare RAM): fake multitap, the setup writes, the load / race hooks (seeds, counters, fixed step,
   rand stub), a wait per frame for the other players' buttons from the computer; a per-frame state hash sent along
   (desync detection).
2. Relay / lobby: pad frames for 2-4 consoles through the lobby server ("race player frame buttons"), host-chosen
   settings, player slots (characters unique, chosen by the joiners), "Start now" for the host from 2 players
   (Arcade: 2 only; Battle: at least 2 teams among those who joined).
3. Manager page: the CTR package (modes, levels, cups, battle settings) in the host popup; the join popup with the free
   characters; the lobby list showing "Battle - 2/4 players".

## Console driver and the Manager (2026-11-17): built, emulator-tested
See `../../padlink/README.md`: the driver (Time Trial's ghost replay space, 2084 / 2400 bytes), the relay
(`gui/padrelay.py`), lobby rooms, `ps1transfer -xserve`, the Net Play page (Host / Join / Start now). Emulator tests of
the whole chain passed (no desyncs); first console test next (BEN + BILL, Crash Team Racing USA on both).
