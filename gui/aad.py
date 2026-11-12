"""
PS1 DEV CART GUI: Crystal Dynamics' music (their "AAD" sound library: Gex 2, Gex 3, Akuji, Legacy of Kain: Soul Reaver,
Walt Disney World Racing Tour...) made into the formats the sound tester plays: a bank (.SND, ID "aSND", stored as
"DNSa") and its samples (.SMP, "aSMP", stored as "PMSa") into a VAB (VH + VB), each music of the bank into a SEQ.

The layout below is the one Gex 3 uses, read from its files and checked against the decompiled library of Soul
Reaver (github.com/fmil95/soul-re: AADLIB.c, AADSEQEV.c, AADSQCMD.c, AADVOICE.c), which has the same structures
with 32-bit header fields (bank version 262 either way).

SND bank: "DNSa", header size (32-bit, 0x80), version (16-bit, 0x106), 1 byte unknown, number of programs (8-bit),
of tones, of waves, of sequences, of labels (16-bit each), reverb mode, reverb depth; then from the header size:
programs (8 bytes: number of tones, first tone, volume, pan), tones (16 bytes: priority, program, volume, pan, center
note, fine tune (signed: aadStepsPerCent's 0.76 cent at the centre note), lowest and highest note, mode (4: reverb),
pitch bend range (semitones), ADSR1, ADSR2, wave), wave offsets (32-bit, in the SMP's data), sequence offsets and
label offsets (32-bit), then the sequences. A tone plays its wave at 44100 Hz at its center note, as a VAB's tone.

Gex 2's and Akuji's banks (older): the version 16-bit at 4 (Gex 2 0x43, Akuji 0x45, as their sequences'), the
number of programs at 7 and the other numbers from 8 (the number of sequences a byte); the tables at a place fixed
by the version (0x44, 0x48: their code); their waves are SPU RAM addresses (the first: 0x3010, 0x1010). In every
layout the libraries read the number of sequences as a byte. Some of Gex 3's programs count more tones than their
bank has (those left out).

Soul Reaver's banks: the same fields 32-bit (header size 0x28, version 0x106, then the numbers), its sequences
"QSMa": after the 12 bytes, the number of tracks (16-bit, 2 bytes unused) and each one's offset (32-bit, from the
sequence's start); a label's offset from its track's start. Its BIGFILE.DAT (a directory of files by name hash,
stored as they are): the banks and samples found in it by their IDs.

Gex 2's samples (no ID): the size of the data after these 4 bytes, then the SPU data; its banks have Akuji's layout
(waves from 0x3010, sequences' version 0x43).

SMP samples: "PMSa", the size of the data after these 8 bytes, then the SPU ADPCM data (loaded as it is into SPU RAM; its first block
zero: the 8 zero bytes after the size and 8 more), the waves' offsets counted from there.

Sequence: "QESa", microseconds per quarter note (32-bit), ticks per quarter note, version (16-bit), then one track
of events: a delta time (MIDI's variable length) BEFORE each event, MIDI's events with running status (note on, a
velocity of 0 for note off; controllers 7 volume and 10 pan; program change; pitch bend), or FF, a command number,
its length and its bytes: the library's commands (78: tempo, loops, labels, variables, slots...). A bank's first
sequence is usually a control script (it starts the music in a slot, sets the game's variables), the others the
music. The music's loop: a label defined (DefineLabel, 49) and gone back to at the end (GotoLabel, 50), a label's
offset pointing just after its DefineLabel in the track; or LoopStart (46, a count, 0: endless) and LoopEnd (47).

Gex's BIGFILE.DAT: number of files, then 24 bytes for each (a hash, the size, the compressed size or 0, the offset,
2 more words), each file 2048-byte aligned, compressed with an LZ (psx-spx: "CDROM File Archive BIGFILE.DAT (Gex)").

What the console's tester does differently, made up for in the SEQ: a note starts at its channel's pitch bend here
(libsnd and the tester: unbent), so the bend is written again after a note on of a bent channel; a channel's
transpose (Soul Reaver) is written in its notes (the library picks the tone by the note before it: a transposed note
across a key split can get the other tone here). Not converted: the control sequences, the commands for the game
(variables, branches, slots, channel mutes, sound effects).

    python aad.py info file.snd|bigfile.dat
    python aad.py scan game.bin|BIGFILE.DAT out_folder      (every bank: NN.vh, NN.vb and NN_M.seq for each music)

A disc image is read where needed (discimage.py): its ISO 9660 directories, BIGFILE.DAT's file table and its sound
files only.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import math
import os
import struct
import sys

import discimage
import vab

BANK_VERSION = 0x106
OLD_TABLES = {0x43: 0x44, 0x45: 0x48}   # Older banks: where the tables start, by version (Gex 2's and Akuji's code)
SEQ_HEADER = 12
PITCH_CENTRE = 4096          # The library's pitch at a tone's centre note (aadPitchTable[60]: 44100 Hz)
STEPS_CENTRE = 150732        # aadStepsPerCent[60]: a fine tune unit is STEPS_CENTRE * 100 >> 23 of it (0.76 cent)
SMP_DATA = 8                # (the SPU data from after the ID and size, as the library loads it)

# The library's commands (FF nn): those used here
CMD_SELECT_CHANNEL, CMD_SET_TEMPO, CMD_CHANGE_TEMPO = 0, 5, 7
CMD_TRANSPOSE, CMD_IGNORE_TRANSPOSE, CMD_RESPECT_TRANSPOSE = 28, 29, 30
CMD_LOOP_START, CMD_LOOP_END, CMD_DEFINE_LABEL, CMD_GOTO_LABEL, CMD_END = 46, 47, 49, 50, 68
CMD_NAMES = {0: "select channel", 1: "select slot", 2: "assign sequence", 6: "tempo from a sequence", 8: "start slot",
             9: "stop slot", 10: "pause slot", 11: "resume slot", 14: "slot volume", 15: "slot pan",
             16: "channel volume", 17: "channel pan", 18: "channel mutes (all channels played)",
             19: "channel mutes (all channels played)", 20: "channel mutes (all channels played)",
             21: "channel mutes (all channels played)", 26: "channel program", 28: "channel transpose", 51: "variables",
             52: "variables", 53: "variables", 54: "variables", 55: "variables", 56: "variables", 57: "branches",
             58: "branches", 59: "branches", 60: "branches", 61: "branches", 62: "branches", 63: "branches",
             64: "branches", 65: "variables", 66: "variables", 67: "variables", 69: "sound effects", 70: "sound effects", 71: "sound effects", 73: "sustain update",
             74: "sustain update"}


class FormatError(Exception):
    """A file that isn't what it should be (the message is shown to the user)."""


# ---------------------------------------------------------------------------------------------------- bank

class Snd:
    """An SND bank: programs [dict(tones (count), first, vol, pan)], tones [dict], waves [offset], sequences
    [(offset, bytes)], labels [offset], reverb (mode, depth)."""

    def __init__(self, d):
        if len(d) < 0x20 or d[:4] != b"DNSa":
            raise FormatError("not a Crystal Dynamics bank (no DNSa)")
        if struct.unpack_from("<I", d, 8)[0] == BANK_VERSION:       # Soul Reaver: 32-bit fields
            size, np = struct.unpack_from("<II", d, 4)[0], struct.unpack_from("<I", d, 0xC)[0]
            nt, nw, ns, nl, rmode, rdepth = struct.unpack_from("<6I", d, 0x10)
            if max(np, nt, nw, ns, nl) > 0xFFFF:
                raise FormatError("bank with %d programs, %d tones" % (np, nt))
        else:
            if struct.unpack_from("<H", d, 8)[0] == BANK_VERSION:   # Gex 3: the same fields packed
                size, np, counts = struct.unpack_from("<I", d, 4)[0], d[0xB], 0xC
            else:                                                   # Older (Gex 2, Akuji): the version at 4
                version = struct.unpack_from("<H", d, 4)[0]
                size = OLD_TABLES.get(version, (version + 3) & ~3)  # (a version not known: the tables found below)
                np, counts = d[7], 8
            nt, nw, ns, nl, rmode, rdepth = struct.unpack_from("<6H", d, counts)
            ns &= 0xFF                                  # (the libraries read the number of sequences as a byte)
        if not self._chain(d, size, np, nt, False):
            size = next((s for s in range(0x10, min(0x400, len(d) - np * 8), 4) if self._chain(d, s, np, nt, True)),
                        None)
            if size is None:
                raise FormatError("bank of a layout not known (no program table found)")
        tones_at = size + np * 8
        waves_at = tones_at + nt * 16
        seqs_at = waves_at + nw * 4
        labels_at = seqs_at + ns * 4
        base = labels_at + nl * 4
        if base > len(d):
            raise FormatError("bank cut short (%d bytes, %d in its tables)" % (len(d), base))
        self.reverb = (rmode, rdepth)
        self.programs = [dict(zip(("tones", "first", "vol", "pan"), struct.unpack_from("<HHBB", d, size + p * 8)))
                         for p in range(np)]
        self.tones = []
        for t in range(nt):
            (prio, prog, vol, pan, center, fine, lo, hi, mode, bend, adsr1, adsr2,
             wave) = struct.unpack_from("<BBBBBbBBBBHHh", d, tones_at + t * 16)
            self.tones.append(dict(priority=prio, program=prog, vol=vol, pan=pan, center=center, fine=fine, min=lo,
                                   max=hi, mode=mode, bend=bend, adsr1=adsr1, adsr2=adsr2, wave=wave))
        self.waves = list(struct.unpack_from("<%dI" % nw, d, waves_at))
        offsets = struct.unpack_from("<%dI" % ns, d, seqs_at)
        self.labels = list(struct.unpack_from("<%dI" % nl, d, labels_at))
        ends = sorted(set(base + o for o in offsets) | {len(d)})
        self.sequences = []
        for o in offsets:
            at = base + o
            if at + SEQ_HEADER > len(d) or d[at:at + 4] not in (b"QESa", b"QSMa"):
                raise FormatError("sequence %d: no QESa or QSMa at %d" % (len(self.sequences), at))
            end = min(e for e in ends if e > at)
            self.sequences.append(Sequence(d[at:end], self.labels))
        # (its length: to its last sequence's end command; the bank found inside a larger file)
        self.size = max([base] + [base + o + s.size for o, s in zip(offsets, self.sequences)])

    @staticmethod
    def _chain(d, at, np, nt, exact):
        """Whether np programs at `at` have their tones one after the other (exact: nt in all; some of Gex 3's banks'
        programs count more tones than the bank has, those left out)."""
        if not np or at + np * 8 > len(d):
            return False
        first = 0
        for p in range(np):
            n, f = struct.unpack_from("<HH", d, at + p * 8)
            if f != first or n > 16:
                return False
            first += n
        return first == nt or not exact

    def program_tones(self, p):
        """Program p's tones (those with a wave of the bank)."""
        prog = self.programs[p]
        return [t for t in self.tones[prog["first"]:prog["first"] + prog["tones"]] if 0 <= t["wave"] < len(self.waves)]


def sample_end(data, start):
    """The end of the sample at start: after its block with the end flag (or the data's end)."""
    p = start
    while p + 16 <= len(data):
        p += 16
        if data[p - 15] & 1:
            break
    return p


def is_sized_samples(d):
    """Gex 2's samples: the size of the data after it (32-bit), then the SPU data (its first block zero)."""
    return len(d) >= 36 and struct.unpack_from("<I", d, 0)[0] == len(d) - 4 and not any(d[4:20])


def is_smp(d):
    """Whether a file is a bank's samples: PMSa, or Gex 2's (is_sized_samples)."""
    return d[:4] == b"PMSa" or is_sized_samples(d)


def smp_data(d):
    """The SPU ADPCM data of an SMP file (PMSa, Gex 2's size and data, or the data alone)."""
    if d[:4] == b"PMSa":
        return d[SMP_DATA:]
    return d[4:] if is_sized_samples(d) else d


def wave_base(snd, data):
    """What to take off the bank's waves for their place in an SMP's data, or None if the SMP doesn't have them: each
    wave inside the data with an ADPCM block's first bytes, and (all but a few: a tone can start inside another's
    sample, one of Gex 2's) after a block with the end flag (the first at 0). Gex 3's waves are offsets in the data
    (0), Akuji's and Gex 2's SPU RAM addresses (the first wave's: 0x1010, 0x3010)."""
    used = sorted({snd.waves[t["wave"]] for t in snd.tones if 0 <= t["wave"] < len(snd.waves)})
    if not used:
        return None
    for base in sorted({0, min(snd.waves)}):
        if used[0] < base or used[-1] - base + 16 > len(data):
            continue
        if not all(not (w - base) % 16 and data[w - base] < 0x50 and data[w - base + 1] < 8 for w in used):
            continue
        inside = sum(1 for w in used if w != base and not data[w - base - 15] & 1)
        if inside <= len(used) // 20:
            return base
    return None


def smp_fits(snd, data):
    """Whether an SMP's data has the bank's waves (see wave_base)."""
    return wave_base(snd, data) is not None


# The bank's reverb: libspu's SpuSetReverbModeType and SpuSetReverbModeDepth (both sides) as the library sets them
# (aadInitReverb; the same calls in Gex 3's code)
REVERB_TYPES = ("off", "room", "studio (small)", "studio (medium)", "studio (large)", "hall", "space", "echo",
                "delay", "pipe")


def reverb_text(snd):
    """The bank's reverb as the tester's settings would name it: "studio (medium), depth 31%"."""
    mode, depth = snd.reverb
    name = REVERB_TYPES[mode & 0xFF] if (mode & 0xFF) < len(REVERB_TYPES) else "type %d" % mode
    return name if not mode & 0xFF else "%s, depth %d%%" % (name, round((depth & 0x7FFF) * 100 / 0x7FFF))


def tune(center, fine):
    """A VAB tone's (center, shift (1/128 semitones, up only)) for a center note and fine tune: the pitch the library
    plays at the centre note (its pitch table plus aadStepsPerCent * fine * 100 >> 23, the same tables and code in
    Gex 2, Gex 3, Akuji and Soul Reaver), in 1/128 semitones."""
    step = (STEPS_CENTRE * abs(fine) * 100) >> 23
    pitch = PITCH_CENTRE + (step if fine >= 0 else -step)
    shift = int(round(1200 * math.log2(pitch / PITCH_CENTRE) * 128 / 100.0))
    center = int(center)
    while shift < 0:
        center, shift = center + 1, shift + 128
    while shift > 127:
        center, shift = center - 1, shift - 128
    return max(0, min(127, center)), shift


def to_vab(snd, smp):
    """A bank (Snd or its bytes) and its SMP (file or data) as a VAB: (VH bytes, VB bytes, info). The samples aren't
    encoded again. info: programs, tones, samples, skipped (texts)."""
    if not isinstance(snd, Snd):
        snd = Snd(snd)
    data = smp_data(smp)
    skipped = []
    programs = {}
    bad = 0
    for p in range(len(snd.programs)):
        tones = snd.program_tones(p)
        bad += snd.programs[p]["tones"] - len(tones)
        if tones:
            programs[p] = tones[:vab.TONES_PER_PROGRAM]
    if bad:
        skipped.append("%d tones with no wave" % bad)
    if not programs:
        raise FormatError("no programs with tones in this bank")
    if max(programs) >= vab.PROGRAMS:
        raise FormatError("program %d: a VAB's programs are 0-127" % max(programs))
    base = wave_base(snd, data)
    if base is None:
        raise FormatError("the samples (%d bytes) don't have this bank's waves: not its SMP?" % len(data))
    starts = sorted({snd.waves[t["wave"]] - base for ts in programs.values() for t in ts})
    if len(starts) > 254:
        raise FormatError("%d samples: a VAB has 254 at most" % len(starts))
    samples = [data[s:sample_end(data, s)] for s in starts]
    number = {s: i + 1 for i, s in enumerate(starts)}
    ps, total = len(programs), sum(len(t) for t in programs.values())
    tones_at = vab.HEADER_SIZE + vab.PROGRAMS * vab.PROG_SIZE
    sizes_at = tones_at + ps * vab.TONES_PER_PROGRAM * vab.TONE_SIZE
    vh = bytearray(sizes_at + 512)
    vb = b"".join(samples)
    struct.pack_into("<4sIII4H4BI", vh, 0, b"pBAV", 7, 0, len(vh) + len(vb), 0xEEEE, ps, total, len(samples), 127,
                     64, 0, 0, 0xFFFFFFFF)
    for block, p in enumerate(sorted(programs)):
        prog = snd.programs[p]
        struct.pack_into("<6BH", vh, vab.HEADER_SIZE + p * vab.PROG_SIZE, len(programs[p]), min(prog["vol"], 127), 0,
                         0, min(prog["pan"], 127), 0, 0)
        for i, t in enumerate(programs[p]):
            center, shift = tune(t["center"], t["fine"])
            struct.pack_into("<8B6B2xHHhh", vh, tones_at + (block * vab.TONES_PER_PROGRAM + i) * vab.TONE_SIZE,
                             min(t["priority"], 127), 4 if t["mode"] == 4 else 0, min(t["vol"], 127),
                             min(t["pan"], 127), center, shift, min(t["min"], 127), min(t["max"], 127), 0, 0, 0, 0,
                             min(t["bend"], 127), min(t["bend"], 127), t["adsr1"], t["adsr2"], p,
                             number[snd.waves[t["wave"]] - base])
    for i, s in enumerate(samples):
        struct.pack_into("<H", vh, sizes_at + (i + 1) * 2, len(s) // 8)
    return bytes(vh), vb, dict(programs=sorted(programs), tones=total, samples=len(samples), skipped=skipped,
                               reverb=reverb_text(snd))


# ---------------------------------------------------------------------------------------------------- sequences

class Sequence:
    """A sequence: tempo (us per quarter note), ppqn, version, tracks (1: "QESa"; "QSMa": up to 16, Soul Reaver),
    events [(tick, kind, data, offset, track)] of all its tracks in time order: kind "ev" (MIDI bytes, full status),
    "cmd" (number, its bytes), offset from its track's start (a label's place); ticks (to its end), ended (each track
    to its end command), notes, size (its bytes, to its last track's end)."""

    def __init__(self, d, labels=()):
        if len(d) < SEQ_HEADER or d[:4] not in (b"QESa", b"QSMa"):
            raise FormatError("not a sequence (no QESa or QSMa)")
        self.tempo, self.ppqn, self.version = struct.unpack_from("<IHH", d, 4)
        self.labels = list(labels)
        if d[:4] == b"QSMa":                            # Several tracks: their number, then their offsets
            if len(d) < 0x14:
                raise FormatError("sequence cut short")
            count = struct.unpack_from("<H", d, 12)[0]
            if not 1 <= count <= 16 or 16 + count * 4 > len(d):
                raise FormatError("sequence of %d tracks" % count)
            starts = list(struct.unpack_from("<%dI" % count, d, 16))
            if any(not 16 + count * 4 <= s < len(d) for s in starts):
                raise FormatError("a track outside its sequence")
        else:
            starts = [SEQ_HEADER]
        bounds = sorted(set(starts) | {len(d)})
        events, self.ended, self.notes, self.ticks, self.size = [], True, 0, 0, 0
        for track, start in enumerate(starts):
            end = min(b for b in bounds if b > start)
            evs, ended, ticks, used = self._track(d[start:end], track)
            events += evs
            self.ended &= ended
            self.ticks = max(self.ticks, ticks)
            self.size = max(self.size, start + used)
            self.notes += sum(1 for e in evs if e[1] == "ev" and e[2][0] & 0xF0 == 0x90 and e[2][2])
        self.tracks = len(starts)
        self.events = sorted(events, key=lambda e: e[0])   # (stable: a tick's events in track order)

    @staticmethod
    def _track(d, track):
        """One track's events, whether it reached its end command, its ticks, the bytes read."""
        events, ended, p, tick, running = [], False, 0, 0, 0
        try:
            while p < len(d):
                at = p                  # (where the event starts, its delta time: a label's offset points there)
                v = d[p]
                p += 1
                if v & 0x80:
                    v &= 0x7F
                    while True:
                        c = d[p]
                        p += 1
                        v = (v << 7) | (c & 0x7F)
                        if not c & 0x80:
                            break
                tick += v
                if d[p] == 0xFF:
                    cmd, n = d[p + 1], d[p + 2]
                    if p + 3 + n > len(d):
                        raise IndexError
                    events.append((tick, "cmd", (cmd, bytes(d[p + 3:p + 3 + n])), at, track))
                    p += 3 + n
                    if cmd == CMD_END:
                        ended = True
                        break
                    continue
                if d[p] & 0x80:
                    running = d[p]
                    p += 1
                if not running:
                    raise FormatError("sequence: data byte %02X with no status at %d" % (d[p], p))
                n = (2, 2, 2, 2, 1, 1, 2, 2)[(running >> 4) & 7]
                if p + n > len(d):
                    raise IndexError
                events.append((tick, "ev", bytes((running,)) + bytes(d[p:p + n]), at, track))
                p += n
        except IndexError:
            pass                                        # (cut short: what was read is kept)
        return events, ended, tick, min(p, len(d))

    def label_ticks(self, track=0):
        """{label: tick} of the labels a track defines (their offset just after their DefineLabel)."""
        return {data[1][0]: tick for tick, kind, data, _, tr in self.events
                if tr == track and kind == "cmd" and data[0] == CMD_DEFINE_LABEL and data[1]}


def coverage(seq, snd):
    """The part of a music's notes (0-1) that have a tone in the bank: its channel's program (its last program
    change) has a tone with a wave over the note."""
    prog, notes, with_tone = {}, 0, 0
    for e in seq.events:
        kind, data = e[1], e[2]
        if kind != "ev":
            continue
        if data[0] & 0xF0 == 0xC0:
            prog[data[0] & 15] = data[1]
        elif data[0] & 0xF0 == 0x90 and data[2]:
            notes += 1
            p = prog.get(data[0] & 15, 0)
            if p < len(snd.programs):
                with_tone += any(t["min"] <= data[1] <= t["max"] for t in snd.program_tones(p))
    return with_tone / notes if notes else 0.0


def _varlen(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append(0x80 | (v & 0x7F))
        v >>= 7
    return bytes(reversed(out))


MAX_EVENTS = 400000             # A track played no further than this many events (a sequence's whole time unrolled)
MAX_MINUTES = 15                # Tracks not lining up again before this: played straight through to it, no loop
LOOP_NEST = 4                   # The library's loops inside loops


def _play_track(evs, labels, horizon):
    """One track played as the library plays it: [(tick, kind, data)] up to its end command or the horizon (ticks),
    its loops (LoopStart n / LoopEnd: n times, 0 endless; at most LOOP_NEST inside each other) and jumps (GotoLabel:
    to the bank's label table's offset in the track) followed. Returns (events, end tick or None, repeat): repeat
    (start tick, period, first event of it) when the track comes back to a place it has played in the same loop state
    (it then repeats for ever), else None."""
    out, stack, i, tick = [], [], 0, 0
    by_offset = {e[3]: k for k, e in enumerate(evs)}
    seen = {}                                   # (event, loop state) -> (tick, its place in out)
    while i < len(evs) and len(out) < MAX_EVENTS:
        at_tick, kind, data, _, _ = evs[i]
        # (a place: before its delta time, where a label points; the loop from there)
        state = (i, tuple(tuple(s) for s in stack))
        if state in seen:
            start, k = seen[state]
            return out, None, (start, tick - start, k)
        seen[state] = (tick, len(out))
        tick += at_tick - (evs[i - 1][0] if i else 0)       # (its delta time)
        if tick > horizon:
            return out, None, None
        jump = None
        if kind == "cmd":
            cmd, args = data
            if cmd == CMD_END:
                return out, tick, None
            if cmd == CMD_LOOP_START:
                if len(stack) < LOOP_NEST:
                    stack.append([i + 1, args[0] if args else 0])
            elif cmd == CMD_LOOP_END and stack:
                top = stack[-1]
                if top[1] == 0:
                    jump = top[0]
                else:
                    top[1] -= 1
                    if top[1]:
                        jump = top[0]
                    else:
                        stack.pop()
            elif cmd == CMD_GOTO_LABEL and args and args[0] < len(labels):
                jump = by_offset.get(labels[args[0]])
        out.append((tick, kind, data))
        i = jump if jump is not None else i + 1    # (read on from there: its delta time, then the event)
    return out, (tick if i >= len(evs) else None), None


def _lcm(a, b):
    return a // math.gcd(a, b) * b


def to_seq(seq):
    """A music sequence as a SEQ: (bytes, info). Each track played as the library plays it (its own loops and jumps:
    Soul Reaver's tracks loop over different lengths), then merged: where every track with notes repeats from (the
    latest of their repeat starts, after the least common multiple of their periods) the SEQ's endless loop, if the
    sequence lasts that long (its first end command ends it) and it comes before MAX_MINUTES; else the tracks played
    straight through to the sequence's end or MAX_MINUTES, with no loop."""
    tempo = seq.tempo or 500000
    ppqn = seq.ppqn or 480
    cap = int(MAX_MINUTES * 60e6 / tempo * ppqn)
    tracks = [[e for e in seq.events if e[4] == tr] for tr in range(seq.tracks)]
    played = [_play_track(evs, seq.labels, cap) for evs in tracks]
    ends = [end for _, end, _ in played if end is not None]
    seq_end = min(ends) if ends else None                 # (a track's end command ends the sequence)
    is_note = lambda k, d: k == "ev" and d[0] & 0xF0 == 0x90 and d[2]
    musical = [(out, rep) for out, _, rep in played if any(is_note(k, d) for _, k, d in out)]
    reps = [rep for _, rep in musical if rep]
    loop = [None, None]
    if reps:                                            # (the tracks with notes that repeat: one loop for all)
        # (a period a tick or two off a whole part of the longest one, Soul Reaver's 92161 against 92160: taken as
        # that part, the track put back in step at each loop where the game lets it drift a tick, not heard)
        longest = max(p for _, p, _ in reps)
        period = longest
        for _, p, _ in reps:
            k = max(1, round(longest / max(1, p)))
            if abs(k * p - longest) > 2 * k:
                period = _lcm(period, max(1, p))
        start = max(s for s, _, _ in reps)
        # (a track with notes that doesn't repeat: over before the loop, an intro)
        over = all(rep or max([t for t, k, d in out if is_note(k, d)] or [0]) <= start for out, rep in musical)
        if over and start + period <= min(cap, seq_end if seq_end is not None else cap):
            loop = [start, start + period]
    if loop[0] is not None:
        end_tick = loop[1]
    elif reps:
        end_tick = min(cap, seq_end if seq_end is not None else cap)
    elif seq_end is not None:
        end_tick = min(cap, seq_end)
    else:
        end_tick = min(cap, max([t for out, _, _ in played for t, _, _ in out] or [0]))
    events = []
    for tr, (out, _, rep) in enumerate(played):
        evs = [(t, tr, n, k, d) for n, (t, k, d) in enumerate(out)]
        if rep:                                         # (its repeating part written again up to the end)
            start, period, first = rep
            block = out[first:]
            r, n = 1, len(out)
            while start + r * period < end_tick and period and len(evs) < MAX_EVENTS:
                for t, k, d in block:
                    evs.append((t + r * period, tr, n, k, d))
                    n += 1
                r += 1
        events += [e for e in evs if e[0] <= end_tick]
    events.sort(key=lambda e: (e[0], e[1], e[2]))
    body, skipped, notes, chans, bend = [], {}, 0, set(), {}
    # A channel's transpose (SetChannelTranspose for the channel SelectChannel chose: a byte, 0xF4 = -12; ignored
    # for a channel after IgnoreChannelTranspose): its notes written transposed, each note off as its note on was
    selected, transpose, ignored, sounding, transposed = 0, [0] * 16, 0, {}, 0
    for tick, _, _, kind, data in events:
        if kind == "ev":
            hi, ch = data[0] & 0xF0, data[0] & 15
            if hi == 0xB0 and data[1] not in (7, 10):
                continue                                # (bank select, unused controllers: the library ignores them)
            if hi in (0xA0, 0xD0, 0xF0):
                continue
            if hi in (0x80, 0x90):
                shift = 0 if ignored >> ch & 1 else transpose[ch]
                if hi == 0x90 and data[2]:
                    note = max(0, min(127, data[1] + shift))
                    sounding[(ch, data[1])] = note
                    transposed += note != data[1]
                else:
                    note = sounding.pop((ch, data[1]), max(0, min(127, data[1] + shift)))
                data = bytes((data[0], note, data[2]))
            body.append((tick, data))
            if hi == 0xE0:
                bend[ch] = data
            elif hi == 0x90 and data[2]:
                notes += 1
                chans.add(ch)
                b = bend.get(ch)
                if b and (b[1] | (b[2] << 7)) != 8192:
                    body.append((tick, b))              # (the new note bent as the channel is)
            continue
        cmd, args = data
        if cmd == CMD_SET_TEMPO and len(args) >= 3:
            tempo_now = (args[0] << 16) | (args[1] << 8) | args[2]
            if tempo_now:
                body.append((tick, b"\xFF\x51" + tempo_now.to_bytes(3, "big")))
                if tick == 0:
                    tempo = tempo_now
        elif cmd == CMD_SELECT_CHANNEL and args:
            if args[0] < 16:
                selected = args[0]
        elif cmd == CMD_TRANSPOSE and args:
            transpose[selected] = args[0] - 256 if args[0] >= 128 else args[0]
        elif cmd == CMD_IGNORE_TRANSPOSE:
            ignored |= 1 << selected
        elif cmd == CMD_RESPECT_TRANSPOSE:
            ignored &= ~(1 << selected)
        elif cmd not in (CMD_DEFINE_LABEL, CMD_END, CMD_LOOP_START, CMD_LOOP_END, CMD_GOTO_LABEL):
            name = CMD_NAMES.get(cmd, "command %d" % cmd)
            skipped[name] = skipped.get(name, 0) + 1
    if loop[0] is not None:
        body += [(loop[0], b"\xB0\x63\x14"), (loop[0], b"\xB0\x06\x7F"), (loop[1], b"\xB0\x63\x1E")]
    # (the loop markers first at their tick: the loop's start before the notes there, its end after them)
    order = {b"\xB0\x63\x14": 0, b"\xB0\x06": 1}
    body.sort(key=lambda e: (e[0], order.get(e[1][:3], order.get(e[1][:2], 2)) if e[1][:2] != b"\xB0\x63" or
                             e[1][2] != 0x1E else 3))
    out = bytearray(b"pQES" + struct.pack(">IH", 1, ppqn) + tempo.to_bytes(3, "big") + bytes((4, 2)))
    last = 0
    for tick, d in body:
        out += _varlen(tick - last) + d
        last = tick
    out += _varlen(end_tick - last) + b"\xFF\x2F\x00"
    seconds, us, last = 0.0, tempo, 0
    for tick, d in body:
        if d[:2] == b"\xFF\x51":
            seconds += (tick - last) * us / 1e6 / ppqn
            us, last = int.from_bytes(d[2:5], "big"), tick
    seconds += (end_tick - last) * us / 1e6 / ppqn
    return bytes(out), dict(ppqn=seq.ppqn, tempo=tempo, notes=notes, channels=sorted(chans), seconds=seconds,
                            loop=loop[0], loop_end=loop[1], tracks=seq.tracks, transposed=transposed,
                            skipped=["%s (%d)" % (k, v) for k, v in sorted(skipped.items())])


# ---------------------------------------------------------------------------------------------------- files

def unlz(src, size):
    """Gex's BIGFILE compression: 16-bit flags (low bit first), 0 a byte as it is, 1 a copy of 1-16 bytes from up to
    4095 back (2 bytes: length - 1 in the low 4 bits, the distance in the other 12)."""
    dst, s, flags = bytearray(), 0, 0
    try:
        while len(dst) < size:
            if flags <= 1:
                flags = src[s] | (src[s + 1] << 8) | 0x10000
                s += 2
            bit = flags & 1
            flags >>= 1
            if not bit:
                dst.append(src[s])
                s += 1
            else:
                n, dist = (src[s] & 15) + 1, ((src[s] & 0xF0) << 4) | src[s + 1]
                s += 2
                if not dist or dist > len(dst):
                    raise FormatError("damaged compressed file")
                for _ in range(n):
                    dst.append(dst[-dist])
    except IndexError:
        raise FormatError("compressed file cut short")
    return bytes(dst[:size])


MAX_SOUND_FILE = 4 << 20     # A bank or its samples are read only up to this size (Gex 3's: 50 KB and 500 KB)
Image = discimage.Image         # (a game's file read where needed, never whole)
MEDIA = (".STR", ".XA", ".DA")  # Streamed movies and sound: not searched for banks


def bigfile(img, start, size):
    """The sound files of a Gex-style BIGFILE.DAT at start in the image: [(number, place in the image, bytes)]
    (decompressed), or None if
    it isn't one. Only the files that begin as a bank or samples are read."""
    if size < 0x800:
        return None
    n = struct.unpack("<I", img.read(start, 4))[0]
    if not 0 < n < 20000 or 4 + n * 24 > min(size, 0x80000):
        return None
    table = img.read(start + 4, n * 24)
    entries = [struct.unpack_from("<6I", table, i * 24) for i in range(n)]
    if any(e[3] % 0x800 or e[3] < 4 + n * 24 or e[3] + (e[2] or e[1]) > size or (e[2] and e[2] > e[1] * 2 + 16)
           for e in entries):
        return None
    files = []
    for i, (_, length, packed, off, _, _) in enumerate(entries):
        if length > MAX_SOUND_FILE:
            continue
        head = img.read(start + off, 6)
        # (a bank or samples start with their ID: after the first flags of a compressed file, its first 4 bytes)
        sized = not packed and length >= 36 and struct.unpack_from("<I", head.ljust(4, b"\0"))[0] == length - 4
        if packed and head[2:6] not in (b"DNSa", b"PMSa") or \
                not packed and head[:4] not in (b"DNSa", b"PMSa") and not sized:
            continue
        try:
            files.append((i, start + off, unlz(img.read(start + off, packed), length) if packed else
                          img.read(start + off, length)))
        except FormatError:
            pass
    return files


def iso_files(img):
    """The files of an ISO 9660 image (Image): [(path, start, size)]."""
    if img.size < 17 * 2048 or img.read(16 * 2048 + 1, 5) != b"CD001":
        return []
    root = img.read(16 * 2048 + 156, 34)
    out, todo, seen = [], [("", struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0])], set()
    while todo and len(seen) < 10000:
        path, lba, size = todo.pop()
        if lba in seen or lba * 2048 + size > img.size or size > 1 << 20:
            continue
        seen.add(lba)
        d = img.read(lba * 2048, size)
        p = 0
        while p < len(d):
            n = d[p]
            if not n:
                p = (p // 2048 + 1) * 2048
                continue
            if p + 33 > len(d):
                break
            ext, length, flags, nlen = struct.unpack_from("<I", d, p + 2)[0], struct.unpack_from("<I", d, p + 10)[0], \
                d[p + 25], d[p + 32]
            name = d[p + 33:p + 33 + nlen].decode("ascii", "replace").split(";")[0]
            if name not in ("\x00", "\x01"):
                full = path + "/" + name
                if flags & 2:
                    todo.append((full, ext, length))
                elif ext * 2048 < img.size:
                    out.append((full, ext * 2048, min(length, img.size - ext * 2048)))
            p += n
    return out


def embedded(img, start=0, size=None, prefix=""):
    """Banks and samples stored as they are in a file or a part of one (a game's archive, a RAM snapshot): [(name,
    place, bytes)], name prefix + "at 0x..." (from start). A bank's length: to its last sequence's end; an SMP's: 8 +
    its header's size."""
    stop = img.size if size is None else min(img.size, start + size)
    out = []
    for magic in (b"DNSa", b"PMSa"):
        for at in img.find(magic, start, stop):
            if magic == b"PMSa":
                n = struct.unpack("<I", img.read(at + 4, 4).ljust(4, b"\0"))[0]
                if 32 <= n <= MAX_SOUND_FILE and at + 8 + n <= stop:        # (the size: of the data after it)
                    out.append((at, "%sat 0x%X" % (prefix, at - start), img.read(at, 8 + n)))
                continue
            try:
                snd = Snd(img.read(at, min(MAX_SOUND_FILE, stop - at)))
            except (FormatError, struct.error):
                continue
            # (only a whole bank: its sequences read to their end command; "DNSa" in other data, or a damaged copy,
            # can pass the header's checks)
            if not snd.sequences or not all(s.ended for s in snd.sequences):
                continue
            out.append((at, "%sat 0x%X" % (prefix, at - start), img.read(at, snd.size)))
    return [(name, at, data) for at, name, data in sorted(out)]


def collect(img):
    """The sound files a game keeps its music in (Image): a disc image's (BIGFILE.DAT's unpacked, those stored as
    they are in its other data files: Soul Reaver's BIGFILE.DAT), a BIGFILE.DAT's, else those stored in the file
    (embedded). [(name, place in the image (an archive's file: where it is stored), bytes)]"""
    files = iso_files(img)
    if not files:
        inner = bigfile(img, 0, img.size)
        if inner:
            return [("%03d" % i, at, f) for i, at, f in inner]
        return embedded(img)
    out = []
    for name, start, size in files:
        if img.read(start, 4) in (b"DNSa", b"PMSa"):
            if size <= MAX_SOUND_FILE:
                out.append((name, start, img.read(start, size)))
            continue
        inner = bigfile(img, start, size)
        if inner:
            out += [("%s/%03d" % (name.lstrip("/"), i), at, f) for i, at, f in inner]
        elif not name.upper().endswith(MEDIA):
            out += embedded(img, start, size, name.lstrip("/") + " ")
    return out


def scan(source, progress=None):
    """The banks of a game's file (a path or bytes: disc image, BIGFILE.DAT, an SND, other data): [dict(name, at,
    snd (Snd), data (its bytes), smp (data or None), smp_file (the SMP's bytes), smp_name)], each bank once; its SMP:
    the nearest one after it (else before) that has all its waves. name: a disc's path, an archive's file number, or
    "at 0x..." (found in data); at: its place in the file (an archive's file: where it is stored)."""
    img = Image(source)
    try:
        files = collect(img)
    finally:
        img.close()
    smps = [(i, name, smp_data(f), f) for i, (name, _, f) in enumerate(files) if is_smp(f)]
    found, seen = [], set()
    banks = [(i, name, at, f) for i, (name, at, f) in enumerate(files) if f[:4] == b"DNSa"]
    for k, (i, name, at, f) in enumerate(banks):
        if progress:
            progress(k, len(banks))
        if f in seen:
            continue
        seen.add(f)
        try:
            snd = Snd(f)
        except FormatError:
            continue
        fits = [s for s in smps if smp_fits(snd, s[2])]
        fits.sort(key=lambda s: (s[0] < i, abs(s[0] - i)))
        found.append(dict(name=name, at=at, snd=snd, data=f, smp=fits[0][2] if fits else None,
                          smp_file=fits[0][3] if fits else None, smp_name=fits[0][1] if fits else None))
    return found


# ---------------------------------------------------------------------------------------------------- sound effects

SFX_VERSION = 0x100
SFX_ATTR = 24
SFX_DEMO_GAP = 480              # Ticks between two effects in the demo SEQ (a quarter note at 120 BPM: 0.5 s)


class SfxSet:
    """A set of sound effects (Soul Reaver; the library's "dynamic sound effects"): its SNF ("aSNF", stored "FNSa":
    ID, version 100h, unique ID, a word the library fills, the number of effects, 8 bytes the library fills, the
    effects' IDs, 16-bit each) and its SMF ("aSMF", stored "FMSa": ID, version 100h, the same unique ID, the number
    of effects, 6 bytes; then for each effect 24 bytes (effect ID, wave ID, a tone as a bank's, the wave's size) and
    the wave: SPU ADPCM, sent as it is to SPU RAM). effects: [dict(sfx, wave, tone (dict, as Snd.tones), data)]."""

    def __init__(self, snf, smf):
        if snf[:4] != b"FNSa" or smf[:4] != b"FMSa":
            raise FormatError("not a sound effect set (no FNSa and FMSa)")
        version, uid, _, count = struct.unpack_from("<4H", snf, 4)
        sversion, suid, scount = struct.unpack_from("<3H", smf, 4)
        if version != SFX_VERSION or sversion != SFX_VERSION:
            raise FormatError("sound effect set version %X (%X known)" % (version, SFX_VERSION))
        if (uid, count) != (suid, scount):
            raise FormatError("these SNF and SMF files aren't a pair (IDs %d/%d, %d/%d effects)" % (
                uid, suid, count, scount))
        self.uid, self.ids = uid, list(struct.unpack_from("<%dH" % count, snf, 20))
        self.effects, p = [], 16
        for _ in range(count):
            if p + SFX_ATTR > len(smf):
                raise FormatError("SMF cut short (%d bytes)" % len(smf))
            sfx, wave = struct.unpack_from("<HH", smf, p)
            (prio, prog, vol, pan, center, fine, lo, hi, mode, bend, adsr1, adsr2,
             _) = struct.unpack_from("<BBBBBbBBBBHHh", smf, p + 4)
            size = struct.unpack_from("<I", smf, p + 20)[0]
            data = smf[p + SFX_ATTR:p + SFX_ATTR + size]
            if len(data) < size:
                raise FormatError("SMF cut short (%d bytes)" % len(smf))
            self.effects.append(dict(sfx=sfx, wave=wave, data=data, tone=dict(
                priority=prio, program=prog, vol=vol, pan=pan, center=center, fine=fine, min=lo, max=hi, mode=mode,
                bend=bend, adsr1=adsr1, adsr2=adsr2)))
            p += SFX_ATTR + size
        self.snf_size, self.smf_size = 20 + 2 * count, p

    @staticmethod
    def smf_size(smf):
        """An SMF's length (its effects' sizes added up), or None."""
        try:
            count, p = struct.unpack_from("<H", smf, 8)[0], 16
            for _ in range(count):
                p += SFX_ATTR + struct.unpack_from("<I", smf, p + 20)[0]
            return p if p <= len(smf) else None
        except struct.error:
            return None


def sfx_to_vab(sfx):
    """A sound effect set as a VAB: program n = effect n (its tone, its wave; a wave several effects share once).
    Returns (VH, VB, info: effects, samples, the effects' IDs by program)."""
    if len(sfx.effects) > vab.PROGRAMS:
        raise FormatError("%d effects: a VAB has 128 programs" % len(sfx.effects))
    waves = {}
    for e in sfx.effects:
        waves.setdefault(e["wave"], e["data"])
    order = list(waves)
    if len(order) > 254:
        raise FormatError("%d samples: a VAB has 254 at most" % len(order))
    ps = len(sfx.effects)
    tones_at = vab.HEADER_SIZE + vab.PROGRAMS * vab.PROG_SIZE
    sizes_at = tones_at + ps * vab.TONES_PER_PROGRAM * vab.TONE_SIZE
    vh = bytearray(sizes_at + 512)
    vb = b"".join(waves[w] for w in order)
    struct.pack_into("<4sIII4H4BI", vh, 0, b"pBAV", 7, 0, len(vh) + len(vb), 0xEEEE, ps, ps, len(order), 127, 64,
                     0, 0, 0xFFFFFFFF)
    for n, e in enumerate(sfx.effects):
        t = e["tone"]
        struct.pack_into("<6BH", vh, vab.HEADER_SIZE + n * vab.PROG_SIZE, 1, 127, 0, 0, 64, 0, 0)
        center, shift = tune(t["center"], t["fine"])
        struct.pack_into("<8B6B2xHHhh", vh, tones_at + n * vab.TONES_PER_PROGRAM * vab.TONE_SIZE,
                         min(t["priority"], 127), 4 if t["mode"] == 4 else 0, min(t["vol"], 127),
                         min(t["pan"], 127), center, shift, 0, 127, 0, 0, 0, 0, min(t["bend"], 127),
                         min(t["bend"], 127), t["adsr1"], t["adsr2"], n, order.index(e["wave"]) + 1)
    for i, w in enumerate(order):
        struct.pack_into("<H", vh, sizes_at + (i + 1) * 2, len(waves[w]) // 8)
    return bytes(vh), vb, dict(effects=ps, samples=len(order), ids=[e["sfx"] for e in sfx.effects])


def sfx_demo_seq(sfx):
    """A SEQ that plays each effect once, one after the other (program n at its tone's lowest note, the note held
    for its sample's length or SFX_DEMO_GAP), to hear the set in the tester. (bytes, seconds)"""
    out = bytearray(b"pQES" + struct.pack(">IH", 1, 480) + (500000).to_bytes(3, "big") + bytes((4, 2)))
    body = b""
    for n, e in enumerate(sfx.effects):
        note = e["tone"]["min"] if e["tone"]["min"] <= e["tone"]["max"] else 60
        body += _varlen(0) + bytes((0xC0, n)) + _varlen(0) + bytes((0x90, note, 100))
        body += _varlen(SFX_DEMO_GAP) + bytes((0x90, note, 0))
    out += body + _varlen(SFX_DEMO_GAP // 2) + b"\xFF\x2F\x00"
    return bytes(out), (len(sfx.effects) * SFX_DEMO_GAP + SFX_DEMO_GAP // 2) / 960.0


def scan_sfx(source, progress=None):
    """The sound effect sets of a game's file (a path or bytes; read where needed): [dict(name, at, sfx (SfxSet),
    snf, smf)] (each SNF with the SMF of the same unique ID and number of effects that follows it), each set once."""
    img = Image(source)
    found, seen = [], set()
    try:
        snfs = list(img.find(b"FNSa"))
        smfs = list(img.find(b"FMSa"))
        for k, at in enumerate(snfs):
            if progress:
                progress(k, len(snfs))
            head = img.read(at, 20)
            if len(head) < 20:
                continue
            version, uid, _, count = struct.unpack_from("<4H", head, 4)
            if version != SFX_VERSION or not count:
                continue
            snf = img.read(at, 20 + 2 * count)
            for sat in (s for s in smfs if s > at):
                shead = img.read(sat, 10)
                if struct.unpack_from("<3H", shead, 4) != (SFX_VERSION, uid, count):
                    continue
                smf = img.read(sat, MAX_SOUND_FILE)
                size = SfxSet.smf_size(smf)
                if size is None:
                    continue
                smf = smf[:size]
                if smf in seen:
                    break
                seen.add(smf)
                try:
                    found.append(dict(name="at 0x%X" % at, at=at, sfx=SfxSet(snf, smf), snf=snf, smf=smf))
                except FormatError:
                    pass
                break
    finally:
        img.close()
    return found


def write_sfx(entry, folder, base):
    """A sound effect set written in a folder: base.vh, base.vb and base.seq (each effect once). Returns (seq path,
    info)."""
    vh, vb, info = sfx_to_vab(entry["sfx"])
    seq, seconds = sfx_demo_seq(entry["sfx"])
    for ext, data in ((".vh", vh), (".vb", vb), (".seq", seq)):
        with open(os.path.join(folder, base + ext), "wb") as f:
            f.write(data)
    info["seconds"] = seconds
    return os.path.join(folder, base + ".seq"), info


# ---------------------------------------------------------------------------------------------------- command line

def write_bank(entry, folder, base):
    """A bank found by scan written in a folder: base.vh, base.vb, base_N.seq for each music. Returns [(sequence
    number, path, info)], vab info (None: no SMP found, only the SEQs)."""
    vinfo = None
    if entry["smp"] is not None:
        vh, vb, vinfo = to_vab(entry["snd"], entry["smp"])
        with open(os.path.join(folder, base + ".vh"), "wb") as f:
            f.write(vh)
        with open(os.path.join(folder, base + ".vb"), "wb") as f:
            f.write(vb)
    musics = []
    for n, seq in enumerate(entry["snd"].sequences):
        if not seq.notes:
            continue
        data, info = to_seq(seq)
        path = os.path.join(folder, "%s_%d.seq" % (base, n))
        with open(path, "wb") as f:
            f.write(data)
        musics.append((n, path, info))
    return musics, vinfo


def main():
    args = sys.argv[1:]
    try:
        if len(args) >= 2 and args[0] in ("info", "scan"):
            banks = scan(args[1])
            if args[0] == "scan" and len(args) == 3:
                os.makedirs(args[2], exist_ok=True)
            for b, e in enumerate(banks):
                snd = e["snd"]
                print("%02d %s: %d programs, %d tones, %d waves, %d sequences, samples %s, reverb %s" % (
                    b, e["name"], len(snd.programs), len(snd.tones), len(snd.waves), len(snd.sequences),
                    e["smp_name"] or "not found", reverb_text(snd)))
                if args[0] == "scan" and len(args) == 3:
                    musics, vinfo = write_bank(e, args[2], "%02d" % b)
                    for n, path, info in musics:
                        print("    %s: %.1f s, %d notes, loop %s%s" % (
                            os.path.basename(path), info["seconds"], info["notes"],
                            "from tick %d" % info["loop"] if info["loop"] is not None else "none",
                            "; not converted: " + ", ".join(info["skipped"]) if info["skipped"] else ""))
                else:
                    for n, seq in enumerate(snd.sequences):
                        print("    sequence %d: %d notes, %d ticks, %d us per quarter note" % (
                            n, seq.notes, seq.ticks, seq.tempo))
            print("%d banks" % len(banks))
            return 0
    except (FormatError, OSError) as e:
        print("error: %s" % e)
        return 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
