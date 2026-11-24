"""
PS1 DEV CART GUI: Sony's later PS1 music format (1999: Ape Escape, The Legend of Dragoon, Wild Arms 2, Alundra 2...)
made into the formats the sound tester plays: the bank (.HD "SShd" + .BD) into a VAB (VH + VB), the music (.SQ "SSsq")
into a SEQ.

The layout is psx-spx's ("CDROM File Audio Other Formats"), checked on Ape Escape's 43 musics and 82 banks; what the
files showed beyond it:

SQ: header (volume, ticks per quarter note, tempo in beats per minute: 16-bit each; "SSsq" at 12), 16 channels of 16
bytes at 16 (byte 1 channel, 2 program or 0xFF, 3 volume, 4 pan), the events from 0x110. Events are MIDI's with these
differences: the delta time (MIDI's variable length) AFTER each event, a pitch bend of 1 byte (0-127, 64 the centre),
the tempo as FF 51 and 16 bits (beats per minute), the end FF 2F 00 with no delta time. Running status is used by
some games (Alundra 2: also a note off as a note on of velocity 0, and a channel event's status kept across a tempo
event) and not by others (Ape Escape: every status written, note offs as 8x). The
loop is SEQ's: controller 99 value 20 (start), controller 6 (count, 127), controller 99 value 30 (end).

HD: sizes of the HD and of its BD, "SShd" at 12, 28 offsets at 16 (0xFFFFFFFF none): 0 programs, 1 velocity table,
2 "breath" waves (the modulation's shapes), 3 and 4 sound effects. A tone plays its sample at 44100 Hz at its root
key, its fine tune ADDED in 1/16 semitones (signed; measured on the looped samples' periods); its envelope is the SPU's
two registers (ADSR1 the low 16 bits); its flags from bit 0: high priority, noise, 2 unknown, pitch bend range from
the program, modulation, breath wave from the program, reverb (bit 5 is set on exactly the tones with a breath wave).
A program of type bit 7 plays all its tones in range at once (layers), the others have key splits that don't overlap:
both are what a VAB's tones do. A program of type 0xFF is a drum kit (not only sound effects): byte 6 its first note,
byte 7 its LAST note, one tone for each note in between (its key range bytes 0, an unused note's sample 0xFFFF), the
pitch still the note against the tone's root (most roots are the note + 12: a 22050 Hz sample).

Modulation (Ape Escape's driver, SCES_015.64 8009CEF8h, run from its VSync callback: 50 updates a second on a PAL
disc, 60 on an NTSC one) becomes pitch bends written into the SEQ (to_seq with the HD): a note of a tone with flag bit
5 is modulated if its channel's depth (controller 1; the SQ header's channel byte 9) isn't 0 at its key on; each
update its phase (0 at key on) grows by the channel's speed (byte C; controller 2's value v sets 240 / (60 - 58 v /
127): a cycle of 60 updates down to 2), past 240 it starts again at half the speed, and the pitch is moved by wave[phase / 4] x depth / 255 - (depth + 1) / 2 (1/64 semitone: a semitone either
way at full depth, whatever the tone's bend range; the channel's own bend added as its tone's range has it). Controllers
1 and 2 also change the notes already sounding (1 turns modulation on for them). A SEQ's bend is the channel's, so
the channel's newest note leads (a chord's notes start together anyway); the VAB's tones with modulation and no bend
range get a range of 1 semitone.

Not converted (a VAB and a SEQ have nothing for them): the noise flag, the sound effect sequences (data 3 and 4), a
velocity table that isn't 1:1 (none in Ape Escape), a drum kit's notes after its first 16 with a sample (a VAB program
has 16 tones), controller 1 reaching a note of a tone without modulation (the driver modulates it too, with the wave
its voice last had).

Games keep these files inside their own archives, so "scan" looks for them in any file (a disc image too, 2352-byte
sectors or 2048): every SQ, the banks near it, each bank's BD found as the place after it where all its samples
start right after another sample's end. A music's bank is the one near it with a tone for the most of its notes
(Alundra 2: SQ, HD, BD in some files and HD, BD, SQ in others; Ape Escape: HD, effects HD, SQ, BD, effects BD).
The Legend of Dragoon keeps each music in a small MRG archive ("MRG", 0x1A, the number of files, each one's offset
and size in bytes) inside DRGN0.BIN (an MRG whose offsets count sectors): its bank is the one in the same MRG, the
BD sometimes split (the small MRG's last file, then DRGN0's next files, up to the BD's size); a music whose MRG has
no bank plays with one loaded before it: the nearest of the disc with a tone for all its notes, else for the most.
Some of its musics have notes for programs their bank lacks (a second bank the game has loaded: not converted).

    python sshd.py info in.hd|in.sq
    python sshd.py scan game.bin out_folder     (every music found: NN.seq, NN.vh, NN.vb)
    python sshd.py extract game.bin out_folder  (every music found as the game has it: NN.sq, NN.hd, NN.bd)
    python sshd.py vab in.hd in.bd out.vh       (and out.vb; or out.vab: one file)
    python sshd.py seq in.sq out.seq [in.hd [50]]  (with its bank: the modulation as pitch bends; 50: a PAL game's)

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import re
import struct
import sys

import numpy as np

import discimage
import vab

NONE32, NONE16 = 0xFFFFFFFF, 0xFFFF
SQ_EVENTS = 0x110
SQ_CHANNELS = 16
TONE_SIZE = 16
FLAG_PRIORITY, FLAG_NOISE, FLAG_BEND_PROGRAM, FLAG_MODULATION, FLAG_BREATH_PROGRAM, FLAG_REVERB = 1, 2, 16, 32, 64, 128
PROGRAM_KIT = 0xFF
RATE_NTSC, RATE_PAL = 60, 50        # The driver's updates a second (its VSync)
MOD_WAVE = 64                       # A breath wave's bytes (only the first 60 read: the phase stays under 240)
MOD_RELEASE = 1.0                   # A modulated note's bends go on this long after its note off (its release), seconds


class FormatError(Exception):
    """A file that isn't what it should be (the message is shown to the user)."""


# ---------------------------------------------------------------------------------------------------- HD (bank)

class Sshd:
    """An HD file: programs {number: dict(layered, kit, vol, pan, bend, breath, tones [dict])} (kit: a drum kit, one
    tone for each of its notes), velocity (128 values or None), blocks (the numbers of the data blocks it has), waves
    (data block 2's breath waves: [bytes] of MOD_WAVE, 80h the centre)."""

    def __init__(self, d):
        if len(d) < 0x80 or d[12:16] != b"SShd":
            raise FormatError("not an HD file (no SShd)")
        self.size, self.bd_size = struct.unpack_from("<II", d, 0)
        offsets = struct.unpack_from("<28I", d, 16)
        self.blocks = [i for i, o in enumerate(offsets) if o != NONE32]
        if any(offsets[i] >= len(d) for i in self.blocks):
            raise FormatError("HD cut short (%d bytes)" % len(d))
        self.velocity = None
        if offsets[1] != NONE32 and offsets[1] + 130 <= len(d):
            self.velocity = d[offsets[1] + 2:offsets[1] + 130]
        self.waves = []
        if offsets[2] != NONE32 and offsets[2] + 2 <= len(d):  # (a count - 1, then each wave's offset from the block)
            at = offsets[2]
            count = struct.unpack_from("<H", d, at)[0] + 1
            for off in struct.unpack_from("<%dH" % count, d, at + 2) if at + 2 + count * 2 <= len(d) else ():
                w = d[at + off:at + off + MOD_WAVE]
                self.waves.append(bytes(w) if len(w) == MOD_WAVE else None)
        self.programs = {}
        base = offsets[0]
        if base == NONE32:
            return
        try:
            count = struct.unpack_from("<H", d, base)[0] + 1
            for number, off in enumerate(struct.unpack_from("<%dH" % count, d, base + 2)):
                if off == NONE16:
                    continue
                p = base + off
                kind, vol, pan, _, bend, breath, first, last = struct.unpack_from("<8B", d, p)
                kit = kind == PROGRAM_KIT
                tones = []
                for t in range(max(0, last - first + 1) if kit else (kind & 0x7F) + 1):
                    lo, hi, root, fine, adpcm, adsr, _, tvol, tpan, tbend, tbreath, flags = struct.unpack_from(
                        "<BBBbHIBBBBBB", d, p + 8 + t * TONE_SIZE)
                    if adpcm == NONE16:
                        continue                    # (an empty tone, a kit's note with no drum)
                    if kit:
                        lo = hi = first + t
                    tones.append(dict(min=lo, max=hi, root=root, fine=fine, adpcm=adpcm * 8, adsr1=adsr & 0xFFFF,
                                      adsr2=adsr >> 16, vol=tvol, pan=tpan,
                                      bend=bend if flags & FLAG_BEND_PROGRAM else tbend,
                                      breath=breath if flags & FLAG_BREATH_PROGRAM else tbreath, flags=flags))
                self.programs[number] = dict(layered=bool(kind & 0x80) and not kit, kit=kit, vol=vol, pan=pan,
                                             bend=bend, breath=breath, tones=tones)
        except struct.error:
            raise FormatError("HD cut short (%d bytes)" % len(d))

    @classmethod
    def load(cls, path):
        with open(path, "rb") as f:
            return cls(f.read())


def sample_end(bd, start):
    """The end of the sample at start in a BD: after its block with the end flag (or the BD's end)."""
    p = start
    while p + 16 <= len(bd):
        p += 16
        if bd[p - 15] & 1:
            break
    return p


def vab_bend(t):
    """A tone's bend range in the VAB (semitones): the HD's; a tone with modulation and none, 1 (its modulation is
    written as pitch bends, and a range of 0 would hold none)."""
    return min(max(t["bend"], 1) if t["flags"] & FLAG_MODULATION else t["bend"], 127)


def tune(root, fine):
    """A VAB tone's (center, shift) for a root key and a fine tune in 1/16 semitones (a VAB's shift only raises, in
    1/128 semitones: a tone tuned down has the key above as its center)."""
    center, shift = root, fine * 8
    while shift < 0:
        center, shift = center + 1, shift + 128
    while shift > 127:
        center, shift = center - 1, shift - 128
    return max(0, min(127, center)), shift


def to_vab(hd, bd):
    """An HD (Sshd or its bytes) and its BD as a VAB: (VH bytes, VB bytes, info). The samples aren't encoded again:
    the same ADPCM blocks. info: programs, tones, samples, and what a VAB can't hold (skipped: list of texts)."""
    if not isinstance(hd, Sshd):
        hd = Sshd(hd)
    if not hd.programs:
        raise FormatError("no programs in this HD (a sound effects bank: not converted)")
    skipped = []
    if hd.velocity is not None and bytes(hd.velocity) != bytes(range(128)):
        skipped.append("the velocity table (not 1:1)")
    programs = {}
    for n, p in hd.programs.items():
        tones = p["tones"]
        # A drum kit, or a program of one key a tone used as one (Arc the Lad III: up to 23 tones, some keys with
        # two layered): the first 16 tones kept, a VAB program has no more
        if len(tones) > vab.TONES_PER_PROGRAM and (p["kit"] or all(t["min"] == t["max"] for t in tones)):
            skipped.append("program %d (%s): the notes %s" % (n, "drum kit" if p["kit"] else "a tone for each key",
                                                               ", ".join(str(t["min"]) for t in
                                                                         tones[vab.TONES_PER_PROGRAM:])))
            tones = tones[:vab.TONES_PER_PROGRAM]
        if tones:
            programs[n] = dict(p, tones=tones)
    starts = sorted({t["adpcm"] for p in programs.values() for t in p["tones"]})
    if any(s >= len(bd) for s in starts):
        raise FormatError("a sample starts after the end of the BD (%d bytes): not this HD's BD?" % len(bd))
    if len(starts) > 254:
        raise FormatError("%d samples: a VAB has 254 at most" % len(starts))
    samples = [bd[s:sample_end(bd, s)] for s in starts]
    number = {s: i + 1 for i, s in enumerate(starts)}
    for n, p in programs.items():
        if n >= vab.PROGRAMS:
            raise FormatError("program %d: a VAB's programs are 0-127" % n)
        if len(p["tones"]) > vab.TONES_PER_PROGRAM:
            raise FormatError("program %d: %d tones, a VAB program has 16 at most" % (n, len(p["tones"])))
    ps, tones_total = len(programs), sum(len(p["tones"]) for p in programs.values())
    tones_at = vab.HEADER_SIZE + vab.PROGRAMS * vab.PROG_SIZE
    sizes_at = tones_at + ps * vab.TONES_PER_PROGRAM * vab.TONE_SIZE
    vh = bytearray(sizes_at + 512)
    vb = b"".join(samples)
    struct.pack_into("<4sIII4H4BI", vh, 0, b"pBAV", 7, 0, len(vh) + len(vb), 0xEEEE, ps, tones_total, len(samples),
                     127, 64, 0, 0, NONE32)
    noise = 0
    for block, n in enumerate(sorted(programs)):
        p = programs[n]
        struct.pack_into("<6BH", vh, vab.HEADER_SIZE + n * vab.PROG_SIZE, len(p["tones"]), min(p["vol"], 127), 0, 0,
                         min(p["pan"], 127), 0, 0)
        for i, t in enumerate(p["tones"]):
            center, shift = tune(t["root"], t["fine"])
            noise += bool(t["flags"] & FLAG_NOISE)
            struct.pack_into("<8B6B2xHHhh", vh, tones_at + (block * vab.TONES_PER_PROGRAM + i) * vab.TONE_SIZE,
                             127 if t["flags"] & FLAG_PRIORITY else 0, 4 if t["flags"] & FLAG_REVERB else 0,
                             min(t["vol"], 127), min(t["pan"], 127), center, shift, min(t["min"], 127),
                             min(t["max"], 127), 0, 0, 0, 0, vab_bend(t), vab_bend(t), t["adsr1"],
                             t["adsr2"], n, number[t["adpcm"]])
    for i, data in enumerate(samples):
        struct.pack_into("<H", vh, sizes_at + (i + 1) * 2, len(data) // 8)
    if noise:
        skipped.append("noise on %d tones (they play their sample)" % noise)
    return bytes(vh), vb, dict(programs=sorted(programs), tones=tones_total, samples=len(samples), skipped=skipped)


# ---------------------------------------------------------------------------------------------------- SQ (music)

def _varlen(d, p):
    v = 0
    while True:
        if p >= len(d):
            raise FormatError("SQ cut short (%d bytes)" % len(d))
        b = d[p]
        p += 1
        v = (v << 7) | (b & 0x7F)
        if not b & 0x80:
            return v, p


def _write_varlen(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append(0x80 | (v & 0x7F))
        v >>= 7
    return bytes(reversed(out))


class Ssq:
    """An SQ file: volume, ppqn, tempo (beats per minute), channels [dict(channel, program or None, vol, pan, depth,
    speed)] (depth, speed: the modulation's, bytes 9 and C), events
    [(tick, kind, data)]: kind "ev" (the MIDI bytes of a channel event, its pitch bend still 1 byte) or "tempo" (beats
    per minute), ticks (the length), ended (the end event was there), size (the file's bytes, to its end event)."""

    def __init__(self, d):
        if len(d) < SQ_EVENTS or d[12:16] != b"SSsq":
            raise FormatError("not an SQ file (no SSsq)")
        self.volume, self.ppqn, self.tempo = struct.unpack_from("<HHH", d, 0)
        if not self.ppqn or not self.tempo:
            raise FormatError("SQ with no tempo or no ticks per quarter note")
        self.channels = []
        for c in range(SQ_CHANNELS):
            _, ch, prog, vol, pan = struct.unpack_from("<5B", d, 16 + c * 16)
            self.channels.append(dict(channel=ch & 15, program=None if prog == 0xFF else prog, vol=vol, pan=pan,
                                      depth=d[16 + c * 16 + 9], speed=d[16 + c * 16 + 12]))
        self.events, self.ended = [], False
        p, tick, running = SQ_EVENTS, 0, 0
        while p < len(d):
            status = d[p]
            if status < 0x80 and running:                   # Running status: the last channel event's
                size = 2 if running < 0xC0 else 1
                if p + size > len(d):
                    raise FormatError("SQ cut short (%d bytes)" % len(d))
                self.events.append((tick, "ev", bytes((running,)) + d[p:p + size]))
                p += size
            elif status == 0xFF:
                if p + 1 >= len(d):
                    raise FormatError("SQ cut short (%d bytes)" % len(d))
                if d[p + 1] == 0x2F:
                    self.ended = True
                    p += 3
                    break
                if d[p + 1] != 0x51 or p + 4 > len(d):
                    raise FormatError("SQ: event FF %02X at byte %d (not known)" % (d[p + 1], p))
                bpm = struct.unpack_from("<H", d, p + 2)[0]
                if bpm:
                    self.events.append((tick, "tempo", bpm))
                p += 4
            elif 0x80 <= status < 0xF0:
                size = 3 if status < 0xC0 else 2            # (a pitch bend: 1 byte)
                if p + size > len(d):
                    raise FormatError("SQ cut short (%d bytes)" % len(d))
                self.events.append((tick, "ev", d[p:p + size]))
                p += size
                running = status
            else:
                raise FormatError("SQ: byte %02X at %d where an event starts" % (status, p))
            delta, p = _varlen(d, p)
            tick += delta
        self.ticks, self.size = tick, min(p, len(d))

    @classmethod
    def load(cls, path):
        with open(path, "rb") as f:
            return cls(f.read())

    def seconds(self):
        total, bpm, last = 0.0, self.tempo, 0
        for tick, kind, v in self.events:
            if kind == "tempo":
                total += (tick - last) * 60.0 / bpm / self.ppqn
                bpm, last = v, tick
        return total + (self.ticks - last) * 60.0 / bpm / self.ppqn


def _tones_played(hd, program, key):
    """The tones of an HD a note plays: a drum kit's for its note, a layered program's in range, else the first in
    range."""
    p = hd.programs.get(program) if program is not None else None
    if not p:
        return []
    if p["kit"]:
        return [t for t in p["tones"] if t["min"] == key][:1]
    tones = [t for t in p["tones"] if t["min"] <= key <= t["max"]]
    return tones if p["layered"] else tones[:1]


def mod_speed(value):
    """The modulation speed controller 2's value sets (the driver: a cycle of 60 - 58 x value / 127 updates, so from 1
    a second on NTSC down to 2 updates)."""
    return 240 // (60 - 58 * value // 127)


class _ModVoice:
    """A note as the driver's voice has it for its modulation (see the module's text)."""

    def __init__(self, key, tone, wave, on, depth, speed, start):
        self.key, self.wave, self.on, self.depth, self.speed = key, wave, on, depth, speed
        self.range, self.vab_range = tone["bend"], vab_bend(tone)
        self.phase, self.value, self.start, self.released, self.quirk = 0, None, start, None, False

    def step(self, phase):
        phase += self.speed & 0xFFF
        return (self.speed & 0xFFF) >> 1 if phase >= 240 else phase

    def mod(self):
        """The modulation now (64 none): wave x depth / 255 - (depth + 1) / 2 + 64 (before its first update: the value
        that update gives, its key on and that update being at the same time on the console)."""
        if self.value is not None:
            return self.value
        return self.wave[self.step(self.phase) >> 2] * self.depth // 255 - ((self.depth + 1) >> 1) + 64

    def update(self):
        if self.on:
            self.phase = self.step(self.phase)
            self.value = self.wave[self.phase >> 2] * self.depth // 255 - ((self.depth + 1) >> 1) + 64

    def bend(self, channel_bend):
        """The SEQ bend (0-16383) giving this note the driver's pitch over its VAB tone's range: the channel's bend
        (its tone's range) and the modulation (1 semitone at 64 from the centre), in 1/16 semitones as the driver."""
        x = (channel_bend - 64) * self.range
        if not self.on:
            if self.vab_range == self.range:
                return channel_bend << 7                    # (as the SQ has it)
            return max(0, min(16383, 8192 + int(round(x * 128.0 / self.vab_range))))
        sixteenths = (x // 4 if x >= 0 else -(-x // 4)) + ((self.mod() - 64) >> 2)
        return max(0, min(16383, 8192 + int(round(sixteenths * 512.0 / self.vab_range))))


def to_seq(sq, hd=None, rate=RATE_NTSC):
    """An SQ (Ssq or its bytes) as a SEQ: (bytes, info). Each channel's program, volume and pan of the SQ's header
    are set at the start (the SQ's own events after them); a pitch bend is MIDI's 14 bits; the loop is the same
    controllers. With its HD (Sshd or bytes): the modulation (see the module's text) as pitch bends, the driver
    updating it rate times a second (RATE_PAL, RATE_NTSC). info: ppqn, tempo, notes, channels, seconds, loop,
    loop_end, volume, modulated (notes), bends (written for the modulation), skipped (texts)."""
    if not isinstance(sq, Ssq):
        sq = Ssq(sq)
    if hd is not None and not isinstance(hd, Sshd):
        hd = Sshd(hd)
    usec = lambda bpm: max(1, min(0xFFFFFF, int(round(60000000.0 / bpm))))
    out = bytearray(b"pQES" + struct.pack(">IH", 1, sq.ppqn) + usec(sq.tempo).to_bytes(3, "big") + bytes((4, 2)))
    body = []
    for c in sq.channels:
        if c["program"] is not None:
            ch = c["channel"]
            body += [(0, bytes((0xC0 | ch, c["program"] & 127))), (0, bytes((0xB0 | ch, 7, min(c["vol"], 127)))),
                     (0, bytes((0xB0 | ch, 10, min(c["pan"], 127))))]
    # The modulation: each channel's state as the driver's (program, bend, depth, speed: the header's at the start)
    mod = hd is not None and any(t["flags"] & FLAG_MODULATION for p in hd.programs.values() for t in p["tones"])
    state = [dict(program=None, bend=64, depth=0, speed=0, voices=[], sent=8192) for _ in range(16)]
    for c in sq.channels:
        state[c["channel"]].update(program=c["program"], depth=c["depth"], speed=c["speed"])
    modulated = bends = quirks = layered = 0

    def lead(s):
        return max(s["voices"], key=lambda v: v.start) if s["voices"] else None

    def target(s):
        v = lead(s)
        return v.bend(s["bend"]) if v else s["bend"] << 7

    def send(tick, ch, value, force=False):
        nonlocal bends
        s = state[ch]
        if value != s["sent"] or force:
            body.append((tick, bytes((0xE0 | ch, value & 127, value >> 7))))
            s["sent"] = value
            bends += 1

    seg_tick, seg_time, bpm, frame, last_tick = 0, 0.0, sq.tempo, 1, 0

    def frames_until(time, limit_tick):
        """The driver's updates before time: each modulated note's phase, the channels' bends where they moved. Update
        n is at the end of the nth frame (n / rate): the frame plays its events up to that time, then updates its
        voices (a note keyed in it has its first update then: the value its key on was given)."""
        nonlocal frame, last_tick
        while frame / float(rate) < time:
            t = frame / float(rate)
            # (the tick the frame has reached, as a player counts them: down, so a bend isn't a frame late)
            tick = min(limit_tick, max(last_tick, seg_tick + int((t - seg_time) * bpm * sq.ppqn / 60.0 + 1e-6)))
            for ch, s in enumerate(state):
                if not s["voices"]:
                    continue
                s["voices"] = [v for v in s["voices"] if v.released is None or t - v.released < MOD_RELEASE]
                for v in s["voices"]:
                    v.update()
                if any(v.on or v.vab_range != v.range for v in s["voices"]) or s["sent"] != s["bend"] << 7:
                    send(tick, ch, target(s))
            frame += 1

    notes, chans, loop = 0, set(), [None, None]
    for tick, kind, v in sq.events:
        time = seg_time + (tick - seg_tick) * 60.0 / (bpm * sq.ppqn)
        if mod:
            frames_until(time, tick)
        last_tick = tick
        if kind == "tempo":
            body.append((tick, b"\xFF\x51" + usec(v).to_bytes(3, "big")))
            seg_tick, seg_time, bpm = tick, time, v
            continue
        hi, ch = v[0] & 0xF0, v[0] & 15
        s = state[ch]
        if hi == 0xE0:
            s["bend"] = v[1] & 127
            if mod:
                value = target(s)
                body.append((tick, bytes((v[0], value & 127, value >> 7))))
                s["sent"] = value
                continue
            v = bytes((v[0], 0, v[1] & 127))
        elif hi == 0x90 and v[2]:
            notes += 1
            chans.add(ch)
            if mod:
                s["voices"] = [x for x in s["voices"] if x.released is None]
                tones = _tones_played(hd, s["program"], v[1])
                tone = next((t for t in tones if t["flags"] & FLAG_MODULATION), tones[0] if tones else None)
                if tone:
                    wave = None
                    if tone["flags"] & FLAG_MODULATION and tone["breath"] < len(hd.waves):
                        wave = hd.waves[tone["breath"]]
                    voice = _ModVoice(v[1], tone, wave, wave is not None and s["depth"] != 0, s["depth"], s["speed"],
                                      time)
                    s["voices"].append(voice)
                    modulated += voice.on
                    # (layers each with its own wave, or one without: a channel's bend moves them all as the first)
                    layered += voice.on and len({t["breath"] if t["flags"] & FLAG_MODULATION else None
                                                 for t in tones}) > 1
                    body.append((tick, bytes(v)))
                    if voice.on or voice.vab_range != voice.range:
                        send(tick, ch, target(s), force=True)   # (a SEQ player's new note: unbent until a bend)
                    else:
                        send(tick, ch, target(s))
                    continue
        elif hi == 0x80 or hi == 0x90:
            if mod:
                held = [x for x in s["voices"] if x.key == v[1] and x.released is None]
                if held:
                    held[0].released = time
        elif hi == 0xC0:
            s["program"], s["bend"] = v[1], 64             # (the driver: a program change sets the bend back)
        elif hi == 0xB0 and v[1] in (1, 2) and mod:         # (also the notes sounding: their depth, their speed)
            if v[1] == 1:
                s["depth"] = v[2]
            else:
                s["speed"] = mod_speed(v[2])
            for x in s["voices"]:
                if v[1] == 2:
                    x.speed = s["speed"]
                elif x.wave is not None:
                    modulated += not x.on and x.value is None
                    x.on, x.depth = True, v[2]
                elif v[2] and not x.quirk:
                    x.quirk = True
                    quirks += 1
        elif hi == 0xB0 and v[1] == 99 and v[2] in (20, 30):
            loop[v[2] == 30] = tick
        body.append((tick, bytes(v)))
    if mod:
        frames_until(seg_time + (sq.ticks - seg_tick) * 60.0 / (bpm * sq.ppqn), sq.ticks)
    last = 0
    for tick, data in body:
        out += _write_varlen(tick - last) + data
        last = tick
    out += _write_varlen(sq.ticks - last) + b"\xFF\x2F\x00"
    skipped = []
    if quirks:
        skipped.append("the modulation of %d notes of tones without it (controller 1 turning it on: the game uses the "
                       "wave their voice last had)" % quirks)
    if layered:
        skipped.append("the modulation of the other layers of %d notes (layers on different waves: a channel's bend "
                       "moves them all as its first)" % layered)
    return bytes(out), dict(ppqn=sq.ppqn, tempo=sq.tempo, notes=notes, channels=sorted(chans), seconds=sq.seconds(),
                            loop=loop[0], loop_end=loop[1], volume=sq.volume, modulated=modulated, bends=bends,
                            skipped=skipped)


# ---------------------------------------------------------------------------------------------------- scan

SCAN_BD = 0x100000                  # A bank's BD is looked for this far after its HD
SCAN_NEAR = 0x100000                # A music's bank: one that starts this near it
SCAN_NEXT = 0x1000                  # ... one with less than this between it and the music, first
SCAN_SQ_MAX = 0x80000
SCAN_GROUP = 0x100000               # A small MRG archive (a music with its bank): at most this long
SECTOR_SYNC = b"\x00" + b"\xFF" * 10 + b"\x00"


def disc_data(d):
    """The data of a disc image of 2352-byte sectors (the 2048 bytes of each: a file's bytes follow each other); any
    other file as it is."""
    if len(d) >= 2352 * 17 and d[:12] == SECTOR_SYNC and d[2352:2364] == SECTOR_SYNC:
        a = np.frombuffer(d, np.uint8, len(d) // 2352 * 2352).reshape(-1, 2352)
        return a[:, 24:24 + 2048].tobytes()
    return d


def _bd_places(d, start, stop, hd, starts):
    """The places (4-byte aligned) from start where a BD of the HD would end by stop: each of its samples starts with
    an ADPCM block's first 2 bytes (a filter of 0-4, flags of 0-7) after a block with the end flag (the first sample:
    at the BD's start), and the BD's last block ends a sample."""
    window = np.frombuffer(d[start:stop], np.uint8)
    count = (len(window) - hd.bd_size) // 4 + 1
    if count <= 0:
        return []
    cands = np.arange(count) * 4
    ok = (window[cands + (hd.bd_size - 15)] & 0xF9) == 1
    for s in starts:
        ok &= (window[cands + s] < 0x50) & (window[cands + (s + 1)] < 8)
        if s >= 16:
            ok &= (window[cands + (s - 15)] & 0xF9) == 1
    return [start + int(h) * 4 for h in np.nonzero(ok)[0]]


def find_bd(d, hd_at, hd):
    """Where the BD of the HD at hd_at starts in d (see _bd_places), the nearest to the HD within SCAN_BD: after it
    (Ape Escape, Alundra 2, Dragoon: HD then BD) or before it (Arc the Lad III: BD, the music, then HD). None: not
    found."""
    starts = sorted({t["adpcm"] for p in hd.programs.values() for t in p["tones"]})
    if not starts or hd.bd_size < 32 or starts[-1] >= hd.bd_size:
        return None
    best = None
    first = (hd_at + hd.size + 3) & ~3
    if first < len(d):
        after = _bd_places(d, first, first + min(len(d) - first, SCAN_BD + hd.bd_size), hd, starts)
        if after:
            best = (after[0] - first, after[0])
    low = max(0, hd_at - SCAN_BD - hd.bd_size) & ~3
    before = _bd_places(d, low, hd_at, hd, starts)
    if before and (best is None or hd_at - (before[-1] + hd.bd_size) < best[0]):
        best = (hd_at - (before[-1] + hd.bd_size), before[-1])
    return best[1] if best else None


def _needs(sq):
    """An SQ's notes as {(program, key): count} (each note's program: its channel's last)."""
    prog, need = {c["channel"]: c["program"] for c in sq.channels}, {}
    for _, kind, v in sq.events:
        if kind != "ev":
            continue
        if v[0] & 0xF0 == 0xC0:
            prog[v[0] & 15] = v[1]
        elif v[0] & 0xF0 == 0x90 and v[2]:
            key = (prog.get(v[0] & 15), v[1])
            need[key] = need.get(key, 0) + 1
    return need


def _ranges(hd):
    """An HD's key ranges by program: {program: [(lowest, highest)]}."""
    return {p: [(t["min"], t["max"]) for t in v["tones"]] for p, v in hd.programs.items()}


def _cover(need, ranges):
    """The part of the notes (_needs) with a tone in a bank (_ranges) (0-1)."""
    total = sum(need.values())
    if not total:
        return 0.0
    hit = sum(n for (p, key), n in need.items() if p in ranges and any(lo <= key <= hi for lo, hi in ranges[p]))
    return hit / total


def _coverage(sq, hd):
    """The part of an SQ's notes that have a tone in an HD (0-1)."""
    return _cover(_needs(sq), _ranges(hd))


def _archive_entries(d):
    """The files of MRG archives whose offsets count 2048-byte sectors (The Legend of Dragoon's DRGN0.BIN), in their
    table's order: [[(start, size)]] for each archive."""
    out = []
    for at in d.find(b"MRG\x1a"):
        n = struct.unpack("<I", d[at + 4:at + 8].ljust(4, b"\0"))[0]
        if not 1 <= n <= 65536:
            continue
        table = d[at + 8:at + 8 + n * 8]
        if len(table) < n * 8:
            continue
        ents = [struct.unpack_from("<II", table, i * 8) for i in range(n)]
        used = [e for e in ents if e[1]]                # (empty entries: places kept for files not there)
        # (sectors: as bytes, its first file would be inside its own table)
        if not used or used[0][0] >= 8 + n * 8 or used[0][0] * 2048 < 8 + n * 8:
            continue
        if sum(b[0] < a[0] for a, b in zip(used, used[1:])) > len(used) // 20:
            continue
        out.append([(at + s * 2048, size) for s, size in ents])
    return out


def split_bd(d, bank_at, hd, group, archives):
    """A BD split over files (The Legend of Dragoon: the last file of the music's small MRG, then the big archive's
    files after it, each loaded after the other): its bytes, or None (not split, or the samples don't fit)."""
    n = struct.unpack("<I", d[group[0] + 4:group[0] + 8])[0]
    table = d[group[0] + 8:group[0] + 8 + n * 8]
    off, size = struct.unpack_from("<II", table, (n - 1) * 8)
    if group[0] + off <= bank_at:
        return None
    data = bytearray(d[group[0] + off:group[0] + off + size])
    for ents in archives:
        k = next((i for i, (s, sz) in enumerate(ents) if s <= group[0] < s + sz), None)
        if k is None:
            continue
        for s, sz in ents[k + 1:]:
            if len(data) >= hd.bd_size:
                break
            if sz:
                data += d[s:s + sz]
        break
    if len(data) < hd.bd_size:
        return None
    data = bytes(data[:hd.bd_size])
    starts = sorted({t["adpcm"] for p in hd.programs.values() for t in p["tones"]})
    if all(data[s] < 0x50 and data[s + 1] < 8 and (s < 16 or data[s - 15] & 0xF9 == 1) for s in starts):
        return data
    return None


def _groups(d):
    """Small MRG archives ("MRG", 0x1A, the number of files, then each one's offset and size in bytes: The Legend of
    Dragoon keeps a music with its bank in one): [(start, end)]. (An MRG whose offsets count sectors, a disc's big
    archive, isn't one: its offsets fall inside its own table.)"""
    out = []
    for at in d.find(b"MRG\x1a"):
        n = struct.unpack("<I", d[at + 4:at + 8].ljust(4, b"\0"))[0]
        if not 1 <= n <= 256:
            continue
        table = d[at + 8:at + 8 + n * 8]
        if len(table) < n * 8:
            continue
        ents = [struct.unpack_from("<II", table, i * 8) for i in range(n)]
        if any(off < 8 + n * 8 or size > SCAN_GROUP for off, size in ents):
            continue
        end = max(off + size for off, size in ents)
        if end <= SCAN_GROUP:
            out.append((at, at + end))
    return out


SYSTEM_CNF_BOOT = re.compile(rb"BOOT\s*=\s*cdrom:\\*([A-Za-z]{4})[_-]")
PAL_SERIALS = ("SCES", "SLES", "SCED", "SLED", "SCEE")


def disc_rate(d):
    """A disc's driver update rate (its VSync): RATE_PAL for a European serial in its SYSTEM.CNF (read in the first
    megabyte), else RATE_NTSC (an archive, not a disc: NTSC)."""
    m = SYSTEM_CNF_BOOT.search(bytes(d[0:0x100000]))
    return RATE_PAL if m and m.group(1).decode("ascii").upper() in PAL_SERIALS else RATE_NTSC


def scan(d, progress=None):
    """The musics in a game's file (a path, a discimage.Image or bytes: a disc image's sectors seen as their data, see
    discimage; read where needed, never whole): [dict(at, sq (Ssq), sq_bytes, hd_at, hd (Sshd), hd_bytes,
    bd_bytes, coverage, rate)] in file order, each music with its bank once (games keep copies); hd None: no bank
    found for the music; rate: the disc's driver update rate (disc_rate), for to_seq.

    A music's bank: one next to it (its HD or BD within SCAN_NEXT of the SQ) before any other, then the one with a
    tone for the most of its notes. A copy of a music with no bank next to it (a file of the SQ alone, played with a
    bank already loaded) is left out when another copy has one. Where the banks before and after it are as good
    (files packed one after the other: is the bank before its music, or after?), the side the nearest music with
    only one good bank has its bank on.

    A music in a small MRG archive (The Legend of Dragoon): the bank in the same archive; if it has none (the game
    plays it with a bank loaded before): a copy of it packed with a bank that has all its notes; else the first bank
    after it with a tone for all its notes, its copies packed with a poorer bank left out (DRGN0.BIN's entries
    696-701: the game loads one beside each battle set's music, 696 with set 702 and 698 with 712, and their copies
    at 5905 and 6225-6260 are packed with banks; when the game plays them, and so with which bank, isn't known); else
    the bank of the whole file with a tone for the most of its notes, the nearest."""
    if not isinstance(d, discimage.Image):
        with discimage.Image(d) as img:
            return scan(img, progress)
    rate = disc_rate(d)
    banks = []
    for at in d.find(b"SShd"):
        at -= 12
        if at < 0:
            continue
        size = struct.unpack("<I", d[at:at + 4])[0]
        if not 0x80 <= size < 0x40000:                          # (not an HD: not read)
            continue
        try:
            hd = Sshd(d[at:at + size])
        except FormatError:
            continue
        if hd.programs:
            banks.append([at, hd, False, None, _ranges(hd), None])  # (at, HD, its BD looked for, where, key
            #                                                              ranges, its bytes if split)
    groups = _groups(d)
    archives = None

    def bd_of(bank):
        nonlocal archives
        if not bank[2]:
            bank[2], bank[3] = True, find_bd(d, bank[0], bank[1])
            group = min((g for g in groups if g[0] <= bank[0] < g[1]), key=lambda g: g[1] - g[0], default=None)
            if bank[3] is None and group:               # (its BD split over the archive's files)
                if archives is None:
                    archives = _archive_entries(d)
                data = split_bd(d, bank[0], bank[1], group, archives)
                if data is not None:
                    bank[3], bank[5] = group[0], data
        return bank[3]

    musics = []                                                 # [at, Ssq, candidates, chosen, side or None]
    places = [at - 12 for at in d.find(b"SSsq") if at >= 12]
    for i, at in enumerate(places):
        if progress:
            progress(i, len(places))
        try:
            sq = Ssq(d[at:at + SCAN_SQ_MAX])
        except FormatError:
            continue
        if not sq.ended or not any(k == "ev" and v[0] & 0xF0 == 0x90 for _, k, v in sq.events):
            continue
        end, cands, need = at + sq.size, [], _needs(sq)
        for bank in banks:
            if not at - SCAN_NEAR < bank[0] < end + SCAN_NEAR:
                continue
            cover = _cover(need, bank[4])
            if cover <= 0 or bd_of(bank) is None:
                continue
            gap = min(max(at - (s + n), s - end, 0) for s, n in ((bank[0], bank[1].size),
                                                                 (bank[3], bank[1].bd_size)))
            cands.append((cover, gap, bank[0] < at, bank))
        group = min((g for g in groups if g[0] <= at < g[1]), key=lambda g: g[1] - g[0], default=None)
        if group:
            same = [c for c in cands if group[0] <= c[3][0] < group[1]]
            if same:
                cands = same
            else:                                               # (its bank loaded before it)
                # The first bank after it with a tone for all its notes (Dragoon's battle musics: seen in the game
                # with the battle sets that follow them), else the one with the most, the nearest
                after = next((b for b in sorted(banks, key=lambda b: b[0]) if b[0] > at and
                              _cover(need, b[4]) >= 0.999 and bd_of(b) is not None), None)
                if after:
                    c = (_cover(need, after[4]), SCAN_NEXT, False, after)
                    musics.append([at, sq, [c], c, None, True])
                    continue
                best = None
                for bank in banks:
                    cover = _cover(need, bank[4])
                    if cover > 0 and (best is None or (cover, -abs(bank[0] - at)) > best[0]) and \
                            bd_of(bank) is not None:
                        best = ((cover, -abs(bank[0] - at)), bank)
                if best:
                    c = (best[0][0], SCAN_NEXT, best[1][0] < at, best[1])
                    musics.append([at, sq, [c], c, None, False])
                else:
                    musics.append([at, sq, [], None, None, False])
                continue
        near = [c for c in cands if c[1] < SCAN_NEXT]
        if any(c[0] >= 0.5 for c in near):
            cands = near
        top = [c for c in cands if c[0] >= max(x[0] for x in cands) - 0.02]
        sides = {c[2] for c in top}
        chosen = max(top, key=lambda c: (c[0], -c[1])) if len(sides) == 1 else None
        musics.append([at, sq, top, chosen, chosen[2] if chosen and len({c[2] for c in cands}) == 2 else None,
                       False])
    sure = [(m[0], m[4]) for m in musics if m[4] is not None]
    found, seen = [], set()
    # A music played with a loaded bank: a copy packed with a bank that has all its notes before anything else, else
    # the first bank after it with all its notes (its copies packed with a bank that lacks some are left out)
    own_best = {}
    for m in musics:
        if not m[5] and m[3]:
            k = d[m[0]:m[0] + m[1].size]
            own_best[k] = max(own_best.get(k, 0.0), m[3][0])
    loaded = {k for k in (d[m[0]:m[0] + m[1].size] for m in musics if m[5]) if own_best.get(k, 0.0) < 0.999}
    paired = {d[m[0]:m[0] + m[1].size] for m in musics if any(c[1] < SCAN_NEXT for c in m[2])} - loaded
    for at, sq, top, chosen, _, full in musics:
        k = d[at:at + sq.size]
        if k in paired and not any(c[1] < SCAN_NEXT for c in top):
            continue
        if (k in loaded) != full:
            continue
        if not chosen and top:
            side = min(sure, key=lambda s: abs(s[0] - at))[1] if sure else None
            chosen = max(top, key=lambda c: (c[2] == side, c[0], -c[1]))
        entry = dict(at=at, sq=sq, sq_bytes=d[at:at + sq.size], hd_at=None, hd=None, hd_bytes=b"", bd_bytes=b"",
                     coverage=0.0, rate=rate)
        if chosen:
            hd_at, hd, _, bd_at, _, bd_split = chosen[3]
            entry.update(hd_at=hd_at, hd=hd, hd_bytes=d[hd_at:hd_at + hd.size],
                         bd_bytes=bd_split or d[bd_at:bd_at + hd.bd_size], coverage=chosen[0])
        key = (entry["sq_bytes"], entry["hd_bytes"], entry["bd_bytes"])
        if key not in seen:
            seen.add(key)
            found.append(entry)
    return found


def write_original(m, base):
    """A music found by scan() as the game has it: base.sq, and its bank base.hd + base.bd (not changed: a BD split
    over an archive's files put back together, a music played with a bank loaded before it given that bank).
    [(path, size)] written."""
    files = [(base + ".sq", m["sq_bytes"])]
    if m["hd"]:
        files += [(base + ".hd", m["hd_bytes"]), (base + ".bd", m["bd_bytes"])]
    for path, data in files:
        with open(path, "wb") as f:
            f.write(data)
    return [(p, len(d)) for p, d in files]


# ---------------------------------------------------------------------------------------------------- command line

def main():
    args = sys.argv[1:]
    try:
        if len(args) == 2 and args[0] == "info":
            with open(args[1], "rb") as f:
                d = f.read()
            if d[12:16] == b"SSsq":
                sq = Ssq(d)
                print("SQ: %.1f s, %d events, tempo %d, %d ticks per quarter note, volume %d" % (
                    sq.seconds(), len(sq.events), sq.tempo, sq.ppqn, sq.volume))
                for c in sq.channels:
                    if c["program"] is not None:
                        print("  channel %2d: program %3d, volume %3d, pan %3d" % (c["channel"], c["program"],
                                                                                  c["vol"], c["pan"]))
            else:
                hd = Sshd(d)
                print("HD: %d bytes, BD %d bytes, data blocks %s, %d programs" % (hd.size, hd.bd_size, hd.blocks,
                                                                                  len(hd.programs)))
                for n, p in sorted(hd.programs.items()):
                    print("  program %3d: volume %3d, pan %3d%s" % (
                        n, p["vol"], p["pan"], ", drum kit" if p["kit"] else ", layered" if p["layered"] else ""))
                    for t in p["tones"]:
                        print("    keys %3d-%3d, root %3d %+3d/16, sample at 0x%05X, ADSR %04X %04X, volume %3d, "
                              "pan %3d, bend %2d, flags %02X" % (t["min"], t["max"], t["root"], t["fine"], t["adpcm"],
                                                                 t["adsr1"], t["adsr2"], t["vol"], t["pan"],
                                                                 t["bend"], t["flags"]))
            return 0
        if len(args) == 4 and args[0] == "vab":
            with open(args[1], "rb") as f:
                hd = f.read()
            with open(args[2], "rb") as f:
                bd = f.read()
            vh, vb, info = to_vab(hd, bd)
            if args[3].lower().endswith(".vab"):
                with open(args[3], "wb") as f:
                    f.write(vh + vb)
            else:
                with open(args[3], "wb") as f:
                    f.write(vh)
                with open(os.path.splitext(args[3])[0] + ".vb", "wb") as f:
                    f.write(vb)
            print("%s: VH %d bytes, VB %d bytes, %s" % (args[3], len(vh), len(vb), info))
            return 0
        if len(args) == 3 and args[0] == "scan":
            os.makedirs(args[2], exist_ok=True)
            musics = scan(args[1])
            for n, m in enumerate(musics):
                base = os.path.join(args[2], "%02d" % n)
                data, info = to_seq(m["sq"], m["hd"], m["rate"])
                with open(base + ".seq", "wb") as f:
                    f.write(data)
                text = "no bank found"
                if m["hd"]:
                    vh, vb, vinfo = to_vab(m["hd"], m["bd_bytes"])
                    with open(base + ".vh", "wb") as f:
                        f.write(vh)
                    with open(base + ".vb", "wb") as f:
                        f.write(vb)
                    text = "%d programs, %d samples, %d KB, a tone for %.1f%% of the notes%s" % (
                        len(vinfo["programs"]), vinfo["samples"], len(vb) // 1024, m["coverage"] * 100,
                        "".join("; not converted: " + s for s in vinfo["skipped"] + info["skipped"]))
                    if info["modulated"]:
                        text += "; %d notes modulated (%d bends)" % (info["modulated"], info["bends"])
                print("%02d at 0x%08X: %5.1f s, %5d notes; %s" % (n, m["at"], info["seconds"], info["notes"], text))
            print("%d musics in %s" % (len(musics), args[2]))
            return 0
        if len(args) == 3 and args[0] == "extract":
            os.makedirs(args[2], exist_ok=True)
            musics = scan(args[1])
            for n, m in enumerate(musics):
                files = write_original(m, os.path.join(args[2], "%02d" % n))
                print("%02d at 0x%08X: %s" % (n, m["at"], ", ".join("%s %d bytes" % (os.path.basename(p), s)
                                                                    for p, s in files)))
            print("%d musics in %s" % (len(musics), args[2]))
            return 0
        if 3 <= len(args) <= 5 and args[0] == "seq":
            hd = None
            if len(args) >= 4:
                with open(args[3], "rb") as f:
                    hd = f.read()
            with open(args[1], "rb") as f:
                data, info = to_seq(f.read(), hd, int(args[4]) if len(args) == 5 else RATE_NTSC)
            with open(args[2], "wb") as f:
                f.write(data)
            print("%s: %d bytes, %s" % (args[2], len(data), info))
            return 0
    except (FormatError, OSError) as e:
        print("error: %s" % e)
        return 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
