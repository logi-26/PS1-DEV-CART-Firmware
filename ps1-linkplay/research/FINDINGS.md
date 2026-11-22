# Red Alert (PAL, SLES_009.49) link-cable findings

Phase 0 notes. Offline disassembly only; nothing has been captured from a real link session yet.
Addresses are RAM addresses (exe loads at 0x80010000, entry 0x80156544).

## Where the code is
- Menu text "LINK GAME" / "LIAISON PAR CABLE" is in DATA.MIX, so this release supports link play.
- No inline SIO1 register code anywhere (exe or DATA.MIX). Access is through a pointer table at 0x801B3CD0
  (0x1F801050 = SIO1, 0x1F801070 = I_STAT) used by Sony's serial library at 0x80155300-0x80156540.
  - `_sio_control(cmd, sub, value)` = 0x80155A4C
  - read = 0x801554EC (async start 0x8015548C), write = 0x801557D0 (async start 0x8015576C)
  - the `"sio:"` device driver table is at 0x801BAED0 / 0x801BBAA0
- The game's own link layer is 0x80118700-0x80118AB0 (dump in `ra_link_module.asm`).
  It opens `sio:` twice (read and write), then uses ordinary read/write on it.

## Serial settings
| Item | Value | Confidence |
|---|---|---|
| Baud | 2,073,600 (value passed is 0x1FA400, divisor 1) | fairly high |
| Mode | library default 0xCE = 8 data bits, no parity, 2 stop bits; never overridden | fairly high |
| RX interrupt threshold | 1 byte | high |
| Error checks | stat & 0x38 (parity/overrun/framing) abort a read | high |
| Handshake lines | RTS out (ctrl bit 0x20) and CTS in (stat bit 0x100). Corrected: an earlier note said DSR. DSR (stat bit 0x80) is only checked once at open. | medium |

## Link layer (read from ra_link_module.asm)
Function names are mine. Addresses are RAM.

**connect (0x801186D8)**, args: retry count, wait callback.
1. open `sio:` twice (fd at 0xB64($gp) and 0xB68($gp)), set baud 2,073,600, rx level 1, install the timeout callback.
2. If DSR is already high, close and fail with code 1.
3. If CTS is already high, the peer got there first: this console is the **responder** (role 1 stored at 0xB58($gp)).
   It raises RTS, waits one frame, then drops RTS.
4. Otherwise it raises RTS and polls CTS once per frame (VSync, or the caller's wait callback) for up to the retry
   count. If CTS rises it is the **initiator** (role 0). If the retries run out it closes and fails with code 2.

**sync (0x801188C8)**: a second RTS/CTS rendezvous. Raise RTS and wait for CTS to change, for up to 150 ticks of a
counter at 0x888($gp). The counter is reset here but I have not found what increments it, so the real time limit
is not known. On timeout it releases RTS and fails with code 2.

**exchange (0x80118A1C)**: exchange(send_buf, send_len, recv_buf, recv_len). Strict alternation:
- initiator: send, then receive.
- responder: receive, then send.
Each side passes its own send and receive buffers. Both lengths are known to the caller in advance.
After the transfer, if the timeout flag at 0xB60($gp) is set it calls `_sio_control(2,0,0)` and returns 2.

**Message layer (0x800BBB24-0x800BBBD0)**, one turn is:
1. A 6-byte header exchange: three 16-bit fields, one of which is the magic 0xDEAF and one the payload length
   (read back from 0xAD6/0xAD8($gp)). It is exchanged twice (retry or acknowledge).
2. A payload exchange of the length the peer put in its header.
3. On error it returns codes 2, 3 or 4 to the caller.
The header magic and the lengths mean a relay could frame messages without understanding the payload.

**Implication:** the game blocks inside `read` while waiting for the peer, for up to about 1.9 million polls. An
adapter can therefore hold the console in that wait until the remote data arrives over the internet, instead of
faking replies. Whether that is fast enough depends on how often turns happen, which needs a captured trace.

## Timeout callback (0x80118AD8)
The library polls the status register and calls a game callback on every iteration (a0 = 1 for read,
2 for write, a1 = poll count). The callback returns 0 (abort) once the count passes 0x1CFDDF
(about 1.9 million polls), and sets a flag at 0xB60($gp). Each poll is a few dozen instructions, so
this is very roughly a second or more, not microseconds. That suggests a lot of tolerance for delay,
but it needs measuring on real hardware or in an emulator.

## Consequences for the design
1. A raw byte bridge cannot work at 2 Mbaud; the adapter must be a local fake peer.
2. The adapter needs RTS and DSR as well as TX/RX.
3. A PC-side prototype needs a USB-serial dongle that supports 2 Mbaud and modem lines (FT232R does).

## Still unknown
- Packet format, rate, lockstep or not, master/slave pairing.
- Real timeout length, handshake order.

## Next
- Read the rest of `ra_link_module.asm` (handshake order and the game's packet layer above read/write).
- Run two PCSX-Redux instances over SIO1 (TCP) and log the bytes.

## Tools (research/tools)
- `isoextract.py <bin> <outdir>` lists the root directory and extracts small files (SYSTEM.CNF, the exe).
  Large files such as DATA.MIX are skipped; extract them with their LBA and size from the listing.
- `mipsdis.py <start> <end>` disassembles a RAM range of the extracted exe (needs `pip install capstone`).
  Path to the exe is hard-coded as `ra/SLES_009.49`; edit it for your layout.
- `callers.py` lists callers of the serial library functions.

## Live capture on real consoles (2026-11-11, Retaliation, SLES_013.43 / SLES_113.43)
Read with `real_trace/sample.py` (ps1transfer -memlive on both dev carts; read-only, the game keeps running).
Logs: `real_trace/samples_idle.log` (30 s, both consoles, no orders), `real_trace/samples_orders.log` (180 s,
BEN only: BILL's reader produced nothing, orders given).

- Every turn each console sends a 6-byte header `BD DA <len16> AF DE` (magic DABDh, payload length, magic DEAFh)
  and the payload. The payload: 4 bytes (differ between the two directions of the same turn: tx = rx + 4 in its
  third byte on BEN), then **20-byte event records**, `type, house, 00 00, data...`. Length = 4 + 20 x n:
  24 when idle (one record), 44 or 64 with orders.
- House byte: 0Ch from BILL, 0Dh from BEN = HOUSE_MULTI1 / HOUSE_MULTI2 of Westwood's PC Red Alert
  (REDALERT/DEFINES.H, EA's GPL source release: github.com/electronicarts/CnC_Remastered_Collection).
- Record 16h is in every message: its bytes 4-7 are the same in both directions of a turn and change every turn: a
  game CRC (PC: EventType 22 = FRAMESYNC, data FrameInfo {CRC, CommandCount, Delay}). The layout matches the PC
  EventClass (REDALERT/EVENT.H).
- Order records seen: 07h, 0Ah, 0Ch (same data as 0Ah: 11h, 0628h, FFh), 10h + 02h together (64-byte message).
  Their numbering doesn't match the PC EventType list exactly (0Ah/0Ch fit PRODUCE/ABANDON and 10h/02h fit
  ANIMATION + MEGAMISSION only if shifted by one, which 16h = FRAMESYNC contradicts): to settle with controlled
  captures, one known action at a time.
- Turn rate: at least 25 a second (the payload changed on half of 459 reads 22 ms apart: -memlive is too slow to see
  every turn). The game is lockstep: neither side goes on without the other's message.

## Order records identified (2026-11-11, `real_trace/samples_actions.log`, one action at a time on BEN)
| Record | Action | PC Red Alert EventType |
|---|---|---|
| 0Ah, object ID (11h power plant, 15h barracks) | start or resume building | PRODUCE (11) |
| 0Bh, object ID | put on hold | SUSPEND (12) |
| 0Ch, object ID | cancel (sent after a 0Bh) | ABANDON (13) |
| 07h, a map position | place a building | PLACE (8) |
| 10h + 02h in one message | move a unit | ANIMATION (17, the click flash) + MEGAMISSION (2) |
| 12h | sell a building | SELL (19) |
| 16h, CRC | every turn | FRAMEINFO (25): the per-turn heartbeat with the game CRC |
| 05h, house byte CDh | switching between the repair and sell tools (PSX only) | none on the PC |

The PSX list is the PC's with one entry missing between 02h and 07h (MEGAMISSION_F, formation moves, fits), and two
more missing before FRAMEINFO (for 16h to be it). The receiver gets exactly the bytes the sender wrote, at the same
moments: a relay can forward whole messages without understanding them.

## Turn rate (2026-11-11, measured on BEN)
- Game frame counter: 0x801B4EB8 (gp + 65Ch, gp = 801B485Ch), 32-bit; the PAL VSync count at 0x801B5388, a half-rate
  counter at 0x801B5298. Over 8 s the frame counter rose by exactly 1 every 2 VSyncs: **25 game frames a second**.
- One message each way per game frame: the sent payload (and its CRC) changes with every frame (read together with
  the counter, 185 reads 40 ms apart). So **25 exchanges a second, 40 ms per frame**.
- The payload's first 4 bytes don't count up with the frame (a checksum, not a frame number), and no record carries
  a frame to execute on: unlike the PC game (commands scheduled MaxAhead frames ahead), the PSX game looks strictly
  lockstep, each frame waiting for the other console's message for that frame.
- Message layer: 0x800BBC50 (writes DABDh / DEAFh, the buffers above), called from 0x800BBA78 <- 0x800BF724 (twice)
  <- 0x800BF28C <- 0x800B20B8.
- Consequence for an internet relay: each frame needs a full round trip. Within 40 ms the game keeps full speed; a
  60 ms round trip would slow it to about 16 frames a second (two thirds speed), 100 ms to 10.

## Link play without a cable (2026-11-13)
Done with a patch of the serial library and the dev carts' USB: see `../virtual/README.md` (design, how to run,
speed, and what went wrong on the consoles). Protocol facts found there: each game frame both consoles exchange the
6-byte header twice (the second always the first's copy) then the payload, strictly alternating; the game's own read
timeout counts poll loop passes (about 1.9 million).
