# Net Play: plan and notes (2026-11-13)

Goal (user): a "Net Play" page in the PS1 DEV CART Manager. It detects the disc in the drive, applies the link patch if
the game is supported (Red Alert Retaliation first, more link-cable games later, one patch per game), and lets a player
create or join a multiplayer game. **Requirement: 20 frames a second at least; below that it isn't worth playing.**

## Where the link stands (same PC, both cartridges on one computer)
- 24.3 game frames a second on average (most seconds 24-25; the cable gives 25). Morning of 2026-11-13: 21-22.
- What got it there: 95-byte USB exchanges instead of 347 (new driver layout: rx area, computer counters, console
  counters, tx area; not rings), and relay pacing (busy 100 ms, idle 2 ms). Details: `virtual/README.md` (to update).
- 25 frames a second on one PC since the payload copy (A below).
- BEN's freeze after a game was left (~45 s later, at its menu): the start of gap C (0x801548E4) was the library's
  SIO interrupt handler, registered with the kernel at boot and called when the port flags an interrupt (never in the
  emulator). Now a real handler there (vlink_head.S). BEN survived leaving + 2 minutes at its menu afterwards.

## Why internet play is the hard part
A frame of Red Alert's link game, from a trace (`ps1transfer -vlink` "trace", 2026-11-13):

| Step | Time |
|---|---|
| BILL's game work | ~15 ms |
| messages, one after another: header, header, payload 1 both ways, payload 2 both ways | ~10-25 ms |
| BEN's game work (BILL waits) | ~15 ms |

The game alternates strictly (one side sends, the other receives, then the reverse), and each console does its work
while the other waits. About 6 messages per frame wait for the one before, so each millisecond of one-way network delay
adds about 6 ms to every frame:

| Players | One-way delay | Estimated frames/s |
|---|---|---|
| Same PC (measured) | - | 24.3 |
| Same house (LAN) | 0.5-1 ms | ~21-23 |
| Same country, internet | 5-10 ms | ~10-14 |
| Further | 20 ms+ | ~6 |

So with only the serial library patched (today's approach), internet play falls below the 20 frames a second minimum.
Same-house play over a LAN should stay above it. These are estimates: measure them first (the delay simulator below).

## Result 2026-11-14: internet play measured at home (simulated delay)
Symmetric exchange switched on by the relay only during a game (`sym 1`; the setup screen also uses the exchange
function and deadlocked when it was always on), no copies in that mode, and **double delivery** (each in-game message
given with its copy at once, the sender's identical repeat dropped; started at a frame's first header):

| Delay each way (ping) | Before | Symmetric only | **Symmetric + double delivery** |
|---|---|---|---|
| 0 | 25 | 25 | **25** |
| 5 ms (10 ms) | ~14 est. | 22.9 | **24.9** |
| 10 ms (20 ms) | ~10 est. | 15.7 | **24.9** |
| 15 ms (30 ms) | ~8 est. | 12.3 | **20.3** |
| 20 ms (40 ms) | ~6 est. | 9.5 | **16.9** |

So internet play meets the 20 frames a second minimum up to ~30 ms ping. No mismatched repeat in the test. Next: the
relay split over TCP (Phase 1), keeping these modes; then the Manager page.
Also found: BEN's slow laser (long USB silences, slow loading: start countdown runs out) - the relay now waits for a
silent console (usb_patient) instead of giving up after 5 s (that was what froze BEN after games were left).

## Lobby design (agreed 2026-11-14)
Goal (user): a player just chooses LINK GAME on the console; while it shows "waiting for other player" the Manager
announces the game in a lobby; every other Manager's Net Play page lists it and can join; the two consoles are then
linked as if by a cable, the player who started waiting becoming the host. Decisions: lobby **and** traffic through
a server on the NAS (no router ports for players); the game's link-page wait patched to last much longer, **always
cancellable by the player**.

How it works:
1. **Standby.** With the game at its main menu, the Manager installs the patch (as Start does now) and keeps its half
   of the relay running, connected to nobody ("Ready for Net Play" on the page).
2. **Waiting detected, no extra signal needed:** choosing LINK GAME makes the game raise RTS and hold it while it
   polls CTS (the link page); the driver already passes line changes to the relay ("sets its lines 2"). Held RTS with
   nobody connected = a game offered: the relay reports it, the Manager announces it to the lobby (player name,
   game title and disc, region/ping), and withdraws it when RTS drops (cancelled, timed out, or linked).
3. **Lobby list.** Other Managers' Net Play pages show the open games for the same game (patch package title), with
   the host's name and ping. Join connects this Manager's relay half to the host's through the server.
4. **The link handshake as on a cable.** The joiner chooses LINK GAME on the console; the host's held RTS is seen as
   CTS, so the joiner becomes the responder and the waiting host the initiator (the game's own rule). Then the setup
   screen, the start, and the game with the symmetric exchange + double delivery as now.
5. **The server (NAS).** Hardened like the PLAZA server (containers, internal network, firewall, only its port
   published; see the plaza-nas-server notes): TCP from each Manager; lobby messages (offer, withdraw, list, join)
   and, once paired, the relay frames forwarded between the two halves (the 'H'/'R'/'S' frames of VlinkNet), with
   TCP_NODELAY both ways. Ping counts to the NAS (the measured limit: 20 fps up to ~15 ms one-way end to end).
6. **The longer wait.** Find the link page's connect loop (SLES_009.49: connect 0x801186D8, retry count passed by its
   caller; to locate in SLES_013.43) and patch its retry count (several minutes). First check whether the waiting
   screen already accepts a cancel button: if it does, the longer wait keeps it; if not, the patch adds one (the pad
   checked in the loop) and the Manager page gets Cancel (a driver flag the loop checks: it gives up as on its own
   timeout).

Found (2026-11-14, SLES_013.43): the link page's wait is **connect(retries, callback) at 0x80114F20**, called once
from 0x800BF528 as connect(1500, 0x800B46C0); the 1500 is the single instruction `li a0,1500` (240405DC) at
**0x800BF52C**. Each pass: CTS checked (_sio_control(3,1)), else retries counted down and callback(retries) (the waiting
screen: it shows retries/30 as a seconds countdown, 1500 = 50 s); retries -1 = wait forever (the countdown skipped).
The loop ignores the callback's return. Longer wait: e.g. `li a0,18000` (2404 4650: 10 minutes shown). Cancel: to be
checked whether the waiting screen takes a button; if not, the patch adds one.
(d) in progress 2026-11-14: the Manager's Net Play page (gui/pages/netplay.py) has an **Online lobby** mode (now
the default): lobby server address (default 127.0.0.1:7031 until it runs on the NAS) and player name (remembered);
Start installs the patch and runs `-vlinklobby ... -vlinkname ... -vlinkgame <title> -vlinksym ... -vlinktune
<work dir>/tune.txt`; the "Open games in the lobby" list (own offer left out by name) with Join and "Leave the game"
(written to the tune file: "join N", "leave N", acted on when changed); states for offered / playing with / the other
player left / server errors. Layout: two columns in a scrolled frame (the page was taller than the window). Each
window keeps a working folder `%TEMP%\ps1devcart_netplay_<pid>` with `relay.log` (the relay's output, timed) and
`vlink_trace.txt` (written when the relay stops), to look into failures.
**Open bug (the main one now):** through the lobby (script or Manager), the host's console sometimes goes back to its
menu a few seconds after the joiner chose LINK GAME: the setup screen starts, then the host's console is given none
of the joiner's data (its half receives the joiner's records from the server but delivers no data bytes to the
console), its wait ends after 1.5 s ("waits ended"), "connection lost". Seen 3 times (script 02:16, Manager twice),
worked twice (script 02:25, earlier direct). Never seen in direct -vlinkhost / -vlinkjoin. Suspects: something left
from the unpaired standby period (records dropped at boundaries, the 'fresh' lines record, VlIncoming's empty-area
rule if the console's rx holds an unread byte), or the timing of 'P' versus the first records. Next: reproduce with
the Manager windows' logs/traces (or ps1-linkplay/lobby/tests/lobby_test.py, "trace 400") and compare a failing and a working pairing
(VlIncoming's rx_head/rx_tail, data_left, the in-stream at pairing).
(c) done 2026-11-14 (on this PC): `ps1-linkplay/lobby/netplay_server.py` (asyncio, port 7031; offers per game,
pairing, then forwarding the relay frames) and the relay's lobby mode `ps1transfer -n X -vlinklobby HOST:PORT
-vlinkname NAME -vlinkgame TITLE -vlinksym ... -vlinktune FILE` (standby with its console; "VLINK GAMES id name|..."
printed for the page; "join N" / "leave 1" read from the tune file). BEN offered by choosing LINK GAME, BILL's half
joined from the list, 25 frames a second through the server. **Open:** one earlier attempt failed at the end of the
setup screen (BEN's console given none of BILL's data, its wait ended, back to the menu); not explained yet; traces
now kept by the network half (also written when it stops).
(b) done 2026-11-14: `li a0,18000` at 0x800BF52C installed with the driver ("install_words" in the patch package):
BEN waited well past the old limit, Triangle still cancels (the game's own handling, no countdown shown). Also: line
holding (`hold`) switched off by default: it delayed the other console's line changes by 0.5 s while a console's own
lines were low, which broke the network handshake when the waiting host's partner joined (both ended up waiting).
(a) done 2026-11-14: the host carries its console from the start; held RTS = "VLINK OFFER" / "VLINK WITHDRAW".
Through the script the flow works; through two Managers BEN's wait ran out while BILL's Manager was still installing.

Order of work: (a) standby + waiting detection in the relay/page, (b) the longer wait + cancel patch, (c) the lobby
server (first on this PC, then the NAS), (d) the page's list and Join, (e) a test between two computers, then over
the internet.

## Automatic link play (2026-11-14, built; emulator-tested, NOT yet run on the consoles)
Goal (user): no controller needed to start a link game. Hosting in the Manager makes the console choose LINK GAME,
the country and color picked in the Manager, and START SETUP (the console then waits: the offer). Joining a game from
the lobby list does the same on the joiner's console once paired (the host's RTS already there: the joiner becomes the
responder, as on a cable). Country and color are chosen on the Manager page (Russia, England, Ukraine, Germany,
France / Beige, Blue, Red, Green: the game's values 0-4 / 0-3).

Found in the front end overlay (the menus, loaded from DATA.MIX at 0x80010000 by 0x801449DC, entry 0x80012590; read
from RAM dumps in Redux, `research/redux_trace/menu_auto.lua` + `menu_run.py`, savestates `.menu/menu.sst` (main menu)
and `.menu/linkpage.sst`):
- Exe: the run loop 0x800BF2D8 calls the menus (0x800B6278 -> 0x801449DC -> overlay 0x80012590), whose result 4 =
  link game (game type 1 at 0x80168CC4).
- Main menu = overlay 0x80012F10: menu struct 0x801A9A94 (+0 count 5, +4 items 0x801A9AE4 (stride 34h, +0 id, -1
  hidden; +20h value; +30h callback), +18h shown (1000h), +20h/+24h cursor), its input 0x8001A934 (pad words
  0x80024910 held / 0x80024912 pressed, X = 40h, Triangle 10h). Each frame it calls _sio_control(0,0,0) (0x8001301C,
  returning to 0x80013024) and shows item 4 (LINK GAME) only if DSR (bit 80h): the driver's DSR (the other side's
  DTR) makes it appear without a cable. The choice is kept in s4 (-1 none): s4 = 4 -> the link page 0x800189C8 after
  the menu's fade. Idle 1500 frames: the attract movie (returns 7).
- Link page = 0x800189C8: menu 0x801A9DD8, items 0x801A9E28: COUNTRY (value 0x801A9E48), COLOR (0x801A9E7C), and
  START SETUP (item 2, value 0x801A9EB0, callback 0x80019804: returns 4 on X, else the value it is given, so a value
  written there stays). Value 4 -> fade, country/color copied to 0x801A9A3C/40, RTS up (0x80018B3C), then the waiting
  loop (CTS = linked 0x80018D20, Triangle = back).
- Redux's pad override (PCSX.SIO0.slots[1].pads[1].setOverride) had no effect on this game: not used.

Built:
- Driver (`vlink_head.S` vlink_auto_entry, 8 instructions in gap B, 620 of 628 bytes now): _sio_control enters there;
  if the auto word (stats[6], 0x8000D978, written by the computer only) equals the caller's return address 0x80013024,
  it is cleared and s4 = 4. One shot, only from the main menu's call. **Emulator: armed at the main menu, s4 = 4 at the
  next call, the word cleared, the link page entered from the menu's own dispatch (ra 0x800131E0).**
- Relay (`usblink.c` VlAuto, `ps1transfer -vlinkauto STEPS`, tune "auto N COUNTRY COLOR"): runs the package's steps
  (w wait / a arm / s set, values c/k = country/color), from the furthest wait already holding, 60 s limit (armed word
  cleared on failure). Lines "VLINK AUTO name: started/step/done/failed".
- Package (`build_hw.py` "auto_steps", "auto_choices"): main menu shown, arm, link page shown, country/color, START.
- Manager page: "Your side" (Country, Color, automatic start on/off, remembered), "Host a game" (lobby: connects if
  needed, then the automatic start), Join (automatic start on "VLINK paired with"), direct create (at once) / join (on
  connection). Same-PC mode stays manual (two automatic starts would race to be host).
- Data writes alone were checked in Redux for the link page (country 2 / color 3 shown live, START -> RTS raised).

To check on the consoles: the driver changed, so both consoles reset and the game booted again first.

First console test (2026-11-14 11:23, BILL hosting, one Manager window): the first Host worked (LINK GAME, country,
color, START, "VLINK OFFER"). Problems, and fixes (relay + package + page only, the driver unchanged):
- **Second Host did nothing:** the relay started at step 3: the link page's values (menu items, callback, shown 1000h)
  stay in memory after the page, so "link page showing" held at the main menu; START went into a page not there and
  LINK GAME was never asked. Now: every run from step 1; at the main menu the page's old "shown" and START cleared
  first; then arm, then "taken" (the driver cleared the word), then the real page (shown again after its fade in).
- **Triangle on the console:** the relay saw it ("sets its lines 0", "VLINK WITHDRAW", the offer withdrawn) but the
  page said "Waiting for the other player": now "Your console left the link page ... no longer offered".
- **BEN saw no games:** the list only came from a running relay (Start). Now the page lists the lobby's games itself
  (gui/netplay.py LobbyWatcher: hello, then the 'G' lists; reconnects every 5 s) while its relay isn't running, and
  Join does it all: install, connect, join that game, the automatic start once paired.

Second console test (2026-11-14 11:44, BILL hosting, BEN joining through the page's list): both automatic starts
worked; the game ran 5 minutes at 25 fps, then both ended with "connection lost" (11:52:53). **Not a network drop**
(both on this PC through the local lobby): BEN's game stopped for 2.2 s between a frame's two headers (traces: its
"waiting" flag 0, its frame counter still, USB answering: busy outside the driver, probably its slow laser), and the
relay ended BILL's wait after VL_ABORT_MS (1.5 s). Fix (usblink.c VlPatience): past 1.5 s, a wait is kept up to
VL_STALL_MS (30 s) while the other console is only busy (its frame counter still since this wait began, not waiting in
the driver, its computer/USB still there); a console that left the game runs on (its menu), so the wait still ends in
~2-3 s. Lines "VLINK X: the other console is busy ... / is back".
Not built yet: re-establishing a dropped TCP connection (lobby or direct) mid-game. Needs the relay records numbered and
kept until acknowledged, a session key to re-pair through the lobby, and the consoles held meanwhile (VL_STALL_MS).

Third console test (2026-11-14 12:31, BEN hosting): 15 minutes of play, then BILL's game left the match (12:46:23).
Seconds before: BILL 2 game frames in a second (a stall of ~1 s), then the relay switched the symmetric exchange off,
on, off in both consoles: VlSym turned it off when a header was 1 s old, rewriting the exchange's instructions in the
middle of a frame (before VlPatience a stall that long had ended the game anyway). Fix: off only when a game ended
(VlGameLeft: a console's frame counter moving 1.5 s after its last header, or no header for 30 s, or the other side
gone). The traces had stopped at 400000 events (12:38/12:42): the trace and message recorders are now rings (the
newest kept, written at the stop; the page asks for "trace 86400").
Lobby on the page: the lobby server running at 7031 was still the old one (no ids): no "(host)", Join allowed on the
player's own game. Now the own row is also known by name while hosting; Join is disabled on it; while hosting, Join on
another game asks to stop hosting and join it (the player presses Triangle; on "VLINK WITHDRAW" the page joins);
while paired, Join is refused with a message. (Leaving the link page automatically would need another driver hook:
gap B has 8 bytes left.)

Fourth console test (2026-11-14 13:00, BILL hosting): 4 minutes, then both consoles waited for each other
(13:04:31). Traces (now rings, written by a clean "stop 1"): BILL wrote an **84-byte payload** (4 orders) twice; BEN
asked for it and was never given it: double delivery puts a message twice into the 128-byte receive area at once, so
only messages up to 64 bytes fit; the relay waited for room that never came (BEN's game: 5853 doubles of 24 bytes, 14
of 44, one of 64, then the 84). Fix (usblink.c VlIncoming): double delivery only when 2 x n <= VL_RX; longer messages
and their repeats given as usual. (Likely also the 12:46 drop's start, when orders were given; not provable, no trace.)

Fifth console test (2026-11-14 13:17): 20+ minutes, 20+ units each, no drop (the 84-byte fix).

Host's game settings (2026-11-14, user's request: chosen in the Manager before hosting like the PC version, shown in
the lobby, set on the consoles, the host's game started): the setup screen after the link uses the link page's menu
0x801A9DD8 with 11 items at 0x801A9EC4 (34h each, value at +20h): map (1-105), units (0-12), tech level (1-10),
credits (0-10000 by 1000), AI teams (0-2), bases, ore regrows, shroud regrows, crates, capture flag (0/1), and item 10
the host's start (callback 0x80019670: 4 on X; its value 0x801AA0EC is what the main menu checks). Found in two Redux
linked by the real driver (run_pair.py --hw, research/redux_trace/setup_auto.lua doing the automatic start as the
relay); then values written into the host's items and 0x801AA0EC = 4: both games started with them (map 12, $5000, no
bases). Built: package "host_options" (labels, values, codes, defaults) and "host_steps" (setup screen showing, 1.5 s,
the 10 values $2-$11, 1 s, start); relay: a second step list (-vlinkhostauto, tune "hostauto N V0..V15"), up to 16
values, "d" wait steps; page: "Game settings (when you host)", sent when paired as the host (lobby) or when the other
player connects (direct create). Not yet run on consoles.
Lobby list: every game listed (server: offers of all games to every unpaired client; a join still needs the same
game), with Host / Game ("title (disc)") / Ping, and a details panel (host, game, ping, settings). The relay's hello
carries the disc and the settings text (-vlinkdisc, -vlinkinfo). The page refuses to join a game of another title.
First-attempt connection failures (13:16:02, 13:48:28): the joiner linked (its RTS pulse) but then did no link I/O at
all (never reached the setup screen) while the host sat at the setup screen sending its 728-byte settings messages.
Not explained. The relay now logs watched game words when they change (-vlinkwatch, package "watch": main menu shown,
the page's item count, setup start, game type, game chosen) to see where the joiner's game goes next time.

14:21 test: no lobby at all: the user's lobby server (still the morning's, PID 24352) refused the relay's 5-field
hello ("update the Manager") and the page still started the console (it waited, offered nowhere). Fixes: the hello is
the 3 fields again, the details go in a separate 'D' frame (an older server ignores it; tune "details DISC<TAB>TEXT"
sends new ones while connected); the automatic start for hosting (and a join chosen before the relay ran) waits for
the server's first list ("VLINK GAMES": a refused half gets none); the watcher fills the game title when an older
server's list has none. Tested against the old server's code and the new one (scratchpad compat test).
Host settings moved (user): no longer always shown in the game's panel; Host a game opens a window with them (two per
row, remembered, Defaults / Cancel / Host); the game detected first if it wasn't.

Page simplified (2026-11-14, user): lobby only (no direct or same-PC modes on the page; the relay still has them),
the server address in the code (pages/netplay.py LOBBY_SERVER; PS1DEVCART_NETPLAY_LOBBY for tests), no connection
panel. Layout: "Your game" strip (cartridge, Detect, result, automatic start switch), the lobby full width (Host /
Join / Leave; Host, Game, Ping; "Game settings": the chosen lobby's settings, four columns), Status. Host a game's
window: name, the game's options (country, color), its settings; Join's window: the game's options.
Intro movie at the main menu (user: after 8-10 s idle the intro plays and the Manager can't start the link play): the
main menu's idle counter (s6, 1500 passes, "beq s6,v0" at 0x8001338C -> return 7) made a nop: package "keep_steps"
(the instruction and three neighbours checked, then the nop), applied by the installer and kept by the relay each
second (-vlinkkeep, VlKeep: the menus are loaded again from the disc). Redux: unpatched the timer fired after 1593
vsyncs (s6 1501); with the nop no intro in 3x as long (s6 past 4600).

Manager page redesign (2026-11-14, user's review): steps on the left (1. your game: the cartridge chosen at the top of
the window, Detect; 2. the game's own options, from its package, shown once the game is known; 3. name (the
cartridge's to begin with) and the way of playing), the lobby on the right (Host a game / Join / Leave; Host and Ping
columns; the player's own offer "(you) ... (host)"), Status under it. Generic: the package gives "options" (label,
values; their indexes $0-$3 in "auto_steps", tune "auto N V0 V1 V2 V3") and "link_play" (the console's menu path).
Lobby server: 'I' (client's id), 'Y' (server's ping, echoed: host's round trip in the 'G' lists, "id TAB name TAB
ms"), 'T' (client's own ping, echoed). The page's LobbyWatcher lists the games with ping = host's + its own round trip.

## Manager page (2026-11-14)
`gui/pages/netplay.py` + `gui/netplay.py`: Detect the games (DISC_ID), Start (installs the driver from the patch package
`gui/data/netplay/red_alert_retaliation.json`, written by build_hw.py; then `ps1transfer -vlink B -vlinksym ...`
as the app's task), state and frame rate from the relay's lines, Stop. Same PC only for now. **Worked first time.**

**Open observation:** in that first game through the page, all of BEN's colours went wrong (green ground pink, the
menu's red background blue), also after leaving the game; normal again after a console reset. Once only so far. The
link writes nothing near the GPU (driver area 0x8000D810-0x8000D97F, the library gaps, the 2 exchange words); a
stray write in BEN's game RAM can't be ruled out. If it happens again: note what was running, read the GPU's display
mode state before resetting (debug page), keep the relay's log.

## Message contents (trace with contents, 2026-11-13, 3439 messages, orders given on both consoles)
- **Every message is sent twice, identical:** each frame is header, header, payload, payload (860/860 pairs from
  BEN, 859/859 from BILL equal, payloads with orders too: 44 and 64 bytes). The second of each is the transport's
  repeat. The relay already answers the second header locally; the second payload can be answered the same way.
- **No message depends on what the console just received:** both consoles send, in the same frame, the same game-state
  CRC (FRAMEINFO record 16h, e.g. 5ADC430E both ways), computed separately, plus their own orders; the first 4 bytes
  are a checksum of the message (not a frame number). So each message is fixed before that frame's exchange.
- **No Frame field:** PC Red Alert's commands carry the frame they run on, MaxAhead frames ahead (QUEUE.CPP:
  Queue_AI_Multiplayer, Can_Advance, Generate_Real_Timing_Event), which hides network delay. The PSX version dropped
  it (record bytes 2-3 are 00 00): it is strictly lockstep per frame, so the PC's latency hiding can't be switched on.
  (In EA's released source, Queue_AI_Multiplayer's sync body is under #if (0); the design is in its comments.)
- Trace file: `research/real_trace/vlink_trace_messages_2026-11-13.txt` ("M" lines: us, console 0 BEN / 1 BILL, length, hex).

## What could make internet play reach 20+
0. **A: the second payload answered locally** (relay only, like the header copy): about 4 crossings a frame
   instead of 6. **Done 2026-11-13: 24.9 frames a second on one PC (25 almost every second; 23.8 without), reads
   waiting 3.4 ms instead of 5.4, no mismatched repeat.** Default in the relay (`pcopy 1`). Over the internet alone
   still ~13 frames a second at 10 ms one-way.
0. **B: symmetric exchange** (patch of the game's exchange function, 0x80118A1C in SLES_009.49, to be found in
   SLES_013.43): the responder sends first then reads, like the initiator, so both send together and do their game
   work at the same time. A frame: work (~17 ms) + 2 one-way delays (header, payload) + a few ms. Estimated: 5 ms
   one-way ~25, 10 ms ~23-24, 15 ms ~20. Needs: writes no longer waiting for the reader's credit (both would wait),
   and care when a game is left so unread messages can't pile up (a stale message crashed BILL before).
   **Tried 2026-11-13 (`vlink_free_write` + 2 instructions at 0x8011528C / 0x8011529C, installed by default by
   usb_link.py; `--no-sym` leaves them out):** works in the emulators, but on the consoles the **setup screen's last
   step also goes through this exchange function**: with BILL as host, after its settings both consoles wrote and
   neither read (deadlock, trace `research/real_trace/vlink_trace_symmetric_setup_deadlock_2026-11-13.txt`); with BEN
   as host the game start failed. So it must only be on during a game. Plan: install with `--no-sym`; the relay
   writes the two instructions into both consoles when both games' frame counters move, and the originals back when
   a game ends (a frame with one console switched and the other not can't deadlock; the instruction cache should take
   the change within a frame, as the exchange code is evicted every frame). Then the delay test (`delay_us`).
1. **Measure, before building:** a delay setting in the relay (`vlink_tune.txt`) that holds each message for N ms,
   to measure frames a second against delay with both consoles at home. Relay-only, no console reset.
2. **A symmetric exchange (research):** if a console's message for a frame doesn't depend on the other's message of
   the same frame, the game's own message layer (0x800BBC50, called from 0x800BBA78 <- 0x800BF724) could be patched so
   that both consoles send at once and then read. Then both do their game work at the same time instead of in turn,
   and a frame costs about one one-way delay instead of six: work (~15-20 ms) + delay + a few ms. At 10 ms one-way
   that is ~33 ms, within the 40 ms of 25 frames a second. First step: check in traces whether each message's content
   depends on what was just received (the payloads carry a CRC of the game state: FRAMEINFO 16h).
   Risks: deeper patch of the game itself (not just the library), the game's own checks; and every further game would
   need its own message-layer patch, not only the shared serial-library driver.
3. Fewer messages per frame (header + payload in one write) as part of 2.

## Plan (phases)
0. **Base:** find BEN's freeze after a game is left; update `virtual/README.md`; commit (no Claude lines in commits).
1. **Relay split over TCP** (`ps1transfer -vlink-net`): one process per cartridge; the record stream that passes
   between two threads today goes over TCP. Credits, header copies, line holding and the "gone" timing stay local to
   each half. Hello (game ID, driver and protocol versions; mismatch refused), keep-alive, "left the game" notice (so
   the other console times out quickly, as with a cable). The delay setting. Test: two cartridges on one PC through
   127.0.0.1 (must still give ~24), then with simulated delay.
2. **Patch packages + installer in the Manager:** port `usb_link.py`'s install into a GUI module on the Manager's live
   link (code and patch points checked, cheats off: the rings use the cheat table space, driver into the gaps, never
   replaced under a running game). One package per game, e.g. `gui/data/netplay/SLES_013.43.json`: disc IDs
   (Retaliation SLES_013.43 / SLES_113.43), patch points, gaps, original code checksums, driver bytes, the link
   shortcuts safe for that game (the header copy is Red Alert's). Made by `build_hw.py`.
3. **Create / join:** recommended a small Net Play server on the NAS like the PLAZA server (hardened containers; both
   players connect out, no router port needed): Create gives a room code, Join takes it, the server forwards the
   streams. Also a direct mode for a LAN (host listens, the other enters its address).
4. **The Net Play page:** console (cartridge), game (Detect from the disc name the firmware keeps, DISC_ID 0x8000DEE0,
   as the Cheats page; warnings for old firmware or a game not booted from the cartridge menu), patch (status,
   Install), session (Create with room code / Join / connected: other player, delay, frames a second, Leave = the
   relay's clean stop). The safe order guided step by step: game at its main menu, Install, connect, LINK GAME on both.
   The relay runs as a task; its once-a-second lines feed the page; the live link is closed while the relay runs.
5. **Real trials:** LAN between two PCs, internet; frames a second against delay; packaging in the Manager release.
6. **More games:** a scanner for Sony's serial library in other executables (the three functions patched in Red
   Alert); the same `vlink.c` built per game with its addresses and gaps; notes per game (consoles, frames a second,
   issues); a compatibility list on the page.

## Questions still open (asked 2026-11-13)
1. Internet: NAS server with room codes, direct addresses only, or both?
2. Keep the same-PC two-cartridge mode on the page (useful for tests)?
3. Ship with the next Manager release, or a test feature until more games work?
4. Order: Phase 0 first, or Phase 1 now? (Now also: the delay measurement and the symmetric-exchange research decide
   whether internet play is worth building at all.)

## Tools and settings today (`ps1-linkplay/virtual`)
- `python usb_link.py BEN BILL` (both games at their main menu first); `vlink_tune.txt` read each second by the relay:
  `stop 1` (clean stop), `trace S` (S seconds of events to `vlink_trace.txt`), `busy_us N`, `idle_us N`, `hold 0/1`
  (line changes held from a console at its menus), `fake 0` (keep 0: it dropped real headers).
- The relay's log `usb_link.log`: VLINK lines (turns, bytes, frames a second) and VTIME lines (exchange times, message
  crossings, how long reads wait) every second.

## 2026-11-16: lobby status, intro kept off, LINK GAME locked
- Lobby: games stay listed once paired: 'G' lines gained a 7th field, status "waiting" / "in progress" (last, so older
  Managers read them as before; an older server sends none: "waiting"). The page shows a Status column; games in
  progress are not joined (Join disabled, the server refuses them too). Tested: scratchpad lobby_status_test.py.
- The intro came back: the page had lost `-vlinkkeep` (the installer writes the nop once, but the menus are loaded
  again from the disc). Passed again.
- LINK GAME locked (research/redux_trace/menu_lock.py, menu_lock_sim.py): always shown, named "DEV CART NET PLAY"
  (English/French/German texts), skipped by the cursor (id -3; the menus' cursor skips negative ids; a negative choice,
  e.g. a mouse click, is none). Only the Manager starts a link game (the driver sets the choice s4 = 4 itself), so the
  page's "start it for me" switch is gone. In the package's keep_steps (26 words). Checked on the game's own code
  with a small MIPS interpreter (cursor down/up never on item 4, X gives 0-3, choice 4 still accepted); Redux crashed
  loading .menu/menu.sst on 2026-11-16 (the savestate's RAM is at offset 0x27 of the file: menu.ram / linkpage.ram).
  Not yet seen on a console: the text's width on screen, and the patch through the menus' reloads.
- 2026-11-17 console test, 3 tries: 2x both back to the main menu ~1 s after reaching the setup screen (2-4 bytes
  crossed), 1x the game started, then the in-game link broke 1.5 s after "symmetric exchange on" (BILL's game left
  the link 0.1 s after the handshake, BEN waited). The night before, without the keep steps, the first try played
  5 min. The keep steps' first check read 9 words (the changing one last), so once applied: 9 reads a second on both
  consoles through setup and game (the front end stays loaded in game), each stopping the game in an interrupt.
  Reordered: "w8001338C=12C20006" alone first (1 read a second), the identity words only after a reload. If the
  setup failure comes back, it is the older open bug above (setup starts, then no data): compare both traces.

## Next steps (2026-11-17)
1. Console re-test on BEN and BILL with the reordered keep steps (one read a second): if the setup screen failure
   comes back, compare both relay logs and traces at that moment (the older open bug above).
2. Commit once the re-test passes: nothing committed since 2026-11-13 (lobby list status, the page rework, the
   LINK GAME lock, the keep steps fix). No Claude lines in commits.
3. The lobby server on the NAS (like PLAZA): server changes first (limits per address, the real client address
   behind the proxy (PROXY protocol), data limits per connection), then the container scripts (netplay.sh, internal
   network + nginx + firewall as PLAZA's server/nas), the user's router rule TCP 7031 -> 192.168.0.33, the address
   (play.logi26.co.uk:7031 or a new name), and LOBBY_SERVER in gui/pages/netplay.py.

## 2026-11-21: lobby server on the NAS
- Server hardened for the internet (`lobby/netplay_server.py`): the player's address from the proxy's PROXY protocol
  line (`--proxy-from`, trusted only from that address), connections per address (8) and per minute (30), 200 at
  once, 256 KB/s per connection (2 MB burst; a relay frame can be 60 KB after a stall), a client that stops reading
  dropped at 4 MB queued, no frame for 90 s closed (clients answer the 3 s pings), names/titles/settings without
  control characters (a line break in a name would add a fake row to the lists). Protocol unchanged.
  Tests without consoles: `lobby/tests/server_test.py` (13 checks, its own servers on 7131-7134).
- Deployed like PLAZA (`lobby/nas/`: netplay.sh, nginx.conf, firewall.sh, Dockerfile.firewall) in
  `/share/CACHEDEV2_DATA/netplay-server/` (app/netplay_server.py): `netplay_server` on `netplay_internal`
  (172.31.252.0/24, --internal), `netplay_server_proxy` (nginx, the only published port 7031) on `netplay_edge`
  (172.31.253.0/24) + internal at 172.31.252.2, `netplay_firewall` (netplay-firewall:latest, the same rules as
  PLAZA's for these subnets). Verified: the server reaches nothing (internet, router, NAS); the proxy can't reach the
  LAN/NAS; the lobby from the PC over the LAN (offer, list, join, forwarding; the PC's real address in the log); the
  Manager's LobbyWatcher reads its lists. QNAP's network create failed once ("ip4_type is not static", NAT rule):
  retrying worked. (`netplay_net` on the NAS is another project's: not ours, left alone.)
  Update: copy netplay_server.py to app/ (strip CRLF), `./netplay.sh restart`.
- The page's LOBBY_SERVER is now `play.logi26.co.uk:7031` (PS1DEVCART_LOBBY overrides: 192.168.0.33:7031 on the LAN,
  127.0.0.1:7031 for a local server). **Needs the user's router rule TCP 7031 -> 192.168.0.33** (until then the
  public name is refused, also from home: NAT loopback goes through the forward).

## Next steps (2026-11-21)
1. User: router rule TCP 7031 -> 192.168.0.33; then check from outside that only 7031 is newly open.
2. Console test through the NAS lobby (BEN and BILL, then two computers, then over the internet).
3. The open lobby bug (setup screen, the host given none of the joiner's data) if it comes back: relay logs/traces.
4. Commit (nothing committed since 2026-11-13; no Claude lines).

## 2026-11-21 evening: tests through the NAS lobby (play.logi26.co.uk:7031, router rule in place)
- User's own run through the public name (Managers, BILL host, BEN joins, different sides): setup, 5 min game, ~20 fps.
- Scripted runs (scratchpad nas_test.py: BEN hosts direct, BILL joins through a 40 +/- 15 ms each-way delay proxy):
  1. **Both players Russia/Beige (the package defaults): both consoles reach the link page then fall back to the main
     menu in ~1 s; HOSTAUTO "setup screen not shown" after 60 s.** = the setup-screen failure of 11-17 (likely).
     Fix needed: the host's side in the offer, the Join window not offering it.
  2. BILL England/Blue: HOSTAUTO done, setup screen; BILL ready after ~19 s, **BEN answered only 3 min later** (no
     button pressed); then 180 s in game at 8.8 fps average (as the 11-14 table: 40 ms each way is too much).
     After the relays stopped (mid-game), **BEN crashed** on the link page (Country/Color screen): illegal instruction
     at 801B3D5C from the interrupt dispatcher (8014ED18): the handler table at 801AB2B0 overwritten by a printf
     frame of libcd's "CD timeout: %s:(%s) Sync..." message (pointers to its "NoIntr"/"DataReady" strings).
     Suspect: BEN's weak CD drive (slow laser noted 11-14) - also fits the 3 min wait (map loading). To check: same
     run with BILL hosting; BEN's disc/drive.
