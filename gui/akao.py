"""
PS1 DEV CART GUI: Square's AKAO music (Final Fantasy VII, VIII, IX, Vagrant Story, Chrono Cross, Legend of Mana...) made
into SEQ + VAB for the sound tester.

An AKAO sequence: "AKAO", its ID, its size, then a header (14h bytes in version 1, 20h in version 2, 40h in version 3)
with a bit for each track and each track's offset; 48 ticks a beat. Each track is a byte code: notes, ties and rests
(00h-99h: (key x 11) + one of 11 lengths; version 3 also F0h-FDh: a key, then the length in the next byte), and
commands A0h-FFh (volume, pan, octave, tempo, loops, jumps, program changes, effects). The commands are as VGMTrans
(the VGMTrans Team, zlib licensed: src/main/formats/Akao) describes them; versions:

- 1.0: Final Fantasy VII. Instruments: the articulations of SOUND/INSTR.ALL (samples) + INSTR.DAT (64 bytes each).
- 1.1, 1.2, 2: SaGa Frontier, Front Mission 2, Chocobo's Mysterious Dungeon, Parasite Eve.
- 3.0, 3.1: Another Mind, Chocobo Dungeon 2, Final Fantasy VIII, Chocobo Racing, SaGa Frontier 2.
- 3.2: Legend of Mana, Front Mission 3, Chrono Cross, Vagrant Story, Final Fantasy IX.

A sample set ("AKAO" with a size of 0): its ID, the SPU address it is loaded at, the size of its samples, the ID of its
first articulation and how many; the articulations (version 3.1+: 16 bytes: sample, loop, fine tune, root key, the SPU
ADSR registers), then the samples (SPU ADPCM).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import bisect
import math
import re
import struct

MAGIC = b"AKAO"
PPQN = 48
LENGTHS = (192, 96, 48, 24, 12, 6, 3, 32, 16, 8, 4)        # A note's length: (its byte % 11)

V1_0, V1_1, V1_2, V2, V3_0, V3_1, V3_2 = "1.0", "1.1", "1.2", "2", "3.0", "3.1", "3.2"
VERSIONS = (V1_0, V1_1, V1_2, V2, V3_0, V3_1, V3_2)


class FormatError(ValueError):
    pass


def u16(d, at):
    return struct.unpack_from("<H", d, at)[0]


def s16(d, at):
    return struct.unpack_from("<h", d, at)[0]


def u32(d, at):
    return struct.unpack_from("<I", d, at)[0]


# ---------------------------------------------------------------------------------------------------- commands

# Each command: (name, number of argument bytes). Names used by the converter: see Sequence.events.
_COMMON = {
    0xA0: ("end", 0), 0xA1: ("program", 1), 0xA2: ("next_length", 1), 0xA3: ("volume", 1),
    0xA4: ("pitch_slide", 2), 0xA5: ("octave", 1), 0xA6: ("octave_up", 0), 0xA7: ("octave_down", 0),
    0xA8: ("expression", 1), 0xA9: ("expression_fade", 2), 0xAA: ("pan", 1), 0xAB: ("pan_fade", 2),
    0xAC: ("noise_clock", 1), 0xAD: ("attack_rate", 1), 0xAE: ("decay_rate", 1), 0xAF: ("sustain_level", 1),
    0xB0: ("decay_sustain", 2), 0xB1: ("sustain_rate", 1), 0xB2: ("release_rate", 1), 0xB3: ("reset_adsr", 0),
    0xB4: ("vibrato", 3), 0xB5: ("vibrato_depth", 1), 0xB6: ("vibrato_off", 0), 0xB7: ("attack_mode", 1),
    0xB8: ("tremolo", 3), 0xB9: ("tremolo_depth", 1), 0xBA: ("tremolo_off", 0), 0xBB: ("sustain_mode", 1),
    0xBC: ("pan_lfo", 2), 0xBD: ("pan_lfo_depth", 1), 0xBE: ("pan_lfo_off", 0), 0xBF: ("release_mode", 1),
    0xC0: ("transpose", 1), 0xC1: ("transpose_rel", 1), 0xC2: ("reverb_on", 0), 0xC3: ("reverb_off", 0),
    0xC4: ("noise_on", 0), 0xC5: ("noise_off", 0), 0xC6: ("fm_on", 0), 0xC7: ("fm_off", 0),
    0xC8: ("loop_start", 0), 0xC9: ("loop_until", 1), 0xCA: ("loop_again", 0), 0xCB: ("reset_effects", 0),
    0xCC: ("slur_on", 0), 0xCD: ("slur_off", 0), 0xCE: ("noise_toggle", 1), 0xCF: ("noise_delay", 1),
    0xD0: ("legato_on", 0), 0xD1: ("legato_off", 0), 0xD2: ("fm_toggle", 1), 0xD3: ("fm_delay", 1),
    0xD4: ("pitch_chain_on", 0), 0xD5: ("pitch_chain_off", 0), 0xD6: ("volume_chain_on", 0),
    0xD7: ("volume_chain_off", 0), 0xD8: ("tuning", 1), 0xD9: ("tuning_rel", 1), 0xDA: ("portamento", 1),
    0xDB: ("portamento_off", 0), 0xDC: ("fixed_length", 1), 0xDD: ("vibrato_depth_fade", 2),
    0xDE: ("tremolo_depth_fade", 2), 0xDF: ("pan_lfo_fade", 2),
}
_V1_TOP = {
    0xE8: ("tempo", 2), 0xE9: ("tempo_fade", 3), 0xEA: ("reverb_depth", 2), 0xEB: ("reverb_depth_fade", 3),
    0xEC: ("drum_on_v1", 2), 0xED: ("drum_off", 0), 0xEE: ("jump", 2), 0xEF: ("cond_jump", 3),
    0xF0: ("loop_branch", 3), 0xF1: ("loop_break", 3), 0xF2: ("program_no_attack", 1), 0xF3: ("f3", 0),
    0xF4: ("overlay_on", 2), 0xF5: ("overlay_off", 0), 0xF6: ("overlay_balance", 1), 0xF7: ("overlay_fade", 2),
    0xF8: ("alternate_on", 1), 0xF9: ("alternate_off", 0), 0xFD: ("time_signature", 2), 0xFE: ("measure", 2),
}
_SUB = {
    0x00: ("tempo", 2), 0x01: ("tempo_fade", 3), 0x02: ("reverb_depth", 2), 0x03: ("reverb_depth_fade", 3),
    0x04: ("drum_on", 0), 0x05: ("drum_off", 0), 0x06: ("jump", 2), 0x07: ("cond_jump", 3),
    0x08: ("loop_branch", 3), 0x09: ("loop_break", 3), 0x0A: ("program_no_attack", 1),
    0x10: ("alloc_voices", 1), 0x11: ("free_voices", 0), 0x12: ("volume_fade", 2), 0x14: ("program_split", 1),
    0x15: ("time_signature", 2), 0x16: ("measure", 2), 0x19: ("expression_fade_note", 2), 0x1C: ("fe1c", 1),
    0x1D: ("use_voices", 0), 0x1E: ("use_no_voices", 0),
}


def command_table(version):
    """{byte: (name, argument bytes)}, and the FCh / FEh sub-commands: {byte: (name, argument bytes)}."""
    top = dict(_COMMON)
    sub = {}
    if version in (V1_0, V1_1):
        top.update(_V1_TOP)
        if version == V1_1:
            for b in (0xF4, 0xF5, 0xF6, 0xF7):
                top.pop(b)
            top[0xFC] = ("program_split_v1", 2)
    elif version in (V1_2, V2):
        top[0xE0] = ("e0", 0)
        sub = dict(_SUB)
        sub[0x04] = ("drum_on_v1", 2)
        sub[0x0C] = ("fc0c", 2)
        sub[0x0D] = ("fc0d", 0)
        sub[0x0E] = ("fc0e", 1)
        sub[0x0F] = ("fc0f", 2)
        sub[0x10] = ("fc10", 1)
        sub[0x11] = ("fc11", 0)
        sub[0x14] = ("program_split_v1", 2)
        for b in (0x19, 0x1C, 0x1D, 0x1E):
            sub.pop(b)
    else:
        top[0xE0] = ("e0", 0)
        sub = dict(_SUB)
        if version == V3_2:
            top.update({0xE1: ("e1", 1), 0xE2: ("e2", 0), 0xE4: ("vibrato_rate_fade", 2),
                        0xE5: ("tremolo_rate_fade", 2), 0xE6: ("pan_lfo_rate_fade", 2)})
            sub[0x0E] = ("pattern", 2)
            sub[0x0F] = ("pattern_end", 0)
            sub[0x13] = ("fe13", 0)
            sub[0x1F] = ("fe1f", 0)          # (Vagrant Story: no arguments, the next command straight after)
        else:
            sub[0x0E] = ("fc0e", 1)
            sub[0x0F] = ("fc0f", 2)
    return top, sub


# ---------------------------------------------------------------------------------------------------- sequences

HEADER = {V1_0: 0x14, V1_1: 0x14, V1_2: 0x14, V2: 0x20, V3_0: 0x40, V3_1: 0x40, V3_2: 0x40}


def header_layout(version):
    """(where the track bits are, where the track offsets start, whether an offset counts from after itself, the
    size field's base (the size counts from there))."""
    if version in (V3_0, V3_1, V3_2):
        return 0x20, 0x40, False, 0
    return 0x10, HEADER[version], True, 0x10


class Sequence:
    """An AKAO sequence (data from its "AKAO"; it may run on after its end). version: one of VERSIONS, or None to
    find it (the first under which every track reads to its end)."""

    def __init__(self, data, version=None):
        if data[:4] != MAGIC:
            raise FormatError("not AKAO")
        if u16(data, 6) == 0:
            raise FormatError("an AKAO sample set, not a sequence")
        self.id = u16(data, 4)
        if version is None:
            errors = []
            for v in (V3_2, V3_1, V1_0, V2, V1_2, V1_1):
                try:
                    self._read(data, v)
                    break
                except FormatError as e:
                    errors.append("%s: %s" % (v, e))
            else:
                raise FormatError("no AKAO version reads it (%s)" % "; ".join(errors))
        else:
            self._read(data, version)

    def _read(self, data, version):
        bits_at, table, after, size_base = header_layout(version)
        if len(data) < table:
            raise FormatError("header cut")
        size = size_base + u16(data, 6)
        if not table < size <= len(data):
            raise FormatError("size %X" % size)
        bits = u32(data, bits_at)
        n = bin(bits).count("1")
        if not 1 <= n <= 32 or (version not in (V3_0, V3_1, V3_2) and bits >> 24):
            raise FormatError("track bits %X" % bits)
        if version in (V3_0, V3_1, V3_2) and (u32(data, 0x2C) or u32(data, 0x38) or u32(data, 0x3C)):
            # (28h: Vagrant Story has a value there)
            raise FormatError("version 3 header's zero words")
        if table + 2 * n > size:
            raise FormatError("track table cut")
        offsets = []
        for i in range(n):
            p = table + 2 * i
            at = (p + 2 if after else p) + u16(data, p)
            if not table + 2 * n <= at < size:
                raise FormatError("track %d at %X" % (i, at))
            offsets.append(at)
        self.version, self.data, self.size, self.channels = version, data[:size], size, bits
        self.offsets = offsets
        self.top, self.sub = command_table(version)
        self.instruments_at = self.drums_at = None
        self.reverb = u16(data, 8)
        self.sample_set = None
        if version in (V3_0, V3_1, V3_2):
            self.sample_set = u16(data, 0x14)
            if u32(data, 0x30):
                self.instruments_at = 0x30 + u32(data, 0x30)
            if u32(data, 0x34):
                self.drums_at = 0x34 + u32(data, 0x34)
        self.tracks = [self._commands(at) for at in offsets]

    # A track's commands, read once each (jumps and loops followed later): {offset: (name, args, next offset)}
    def _commands(self, start):
        d, size, v3 = self.data, self.size, self.version in (V3_0, V3_1, V3_2)
        out, todo = {}, [start]
        while todo:
            at = todo.pop()
            while at not in out:
                if not 0 <= at < size:
                    raise FormatError("track runs out at %X" % at)
                b = d[at]
                if b <= 0x99 or (v3 and 0xF0 <= b <= 0xFD):
                    n = 1 if b <= 0x99 else 2
                    if at + n > size:
                        raise FormatError("note cut at %X" % at)
                    out[at] = ("note", tuple(d[at:at + n]), at + n)
                    at += n
                    continue
                if 0x9A <= b <= 0x9F:
                    raise FormatError("byte %02X at %X" % (b, at))
                if b in (0xFC, 0xFE) and self.sub and (b == 0xFE) == v3:
                    if at + 1 >= size:
                        raise FormatError("cut at %X" % at)
                    op = self.sub.get(d[at + 1])
                    if op is None:
                        raise FormatError("command %02X %02X at %X" % (b, d[at + 1], at))
                    head = 2
                else:
                    op = self.top.get(b)
                    if op is None:
                        raise FormatError("command %02X at %X" % (b, at))
                    head = 1
                name, n = op
                if at + head + n > size:
                    raise FormatError("%s cut at %X" % (name, at))
                args = d[at + head:at + head + n]
                nxt = at + head + n
                out[at] = (name, args, nxt)
                if name == "end":
                    break
                if name in ("jump", "cond_jump", "loop_branch", "loop_break", "pattern"):
                    rel = s16(args, len(args) - 2)
                    dest = (nxt if not v3 else nxt - 2) + rel
                    if not 0 <= dest < size:
                        raise FormatError("%s to %X at %X" % (name, dest, at))
                    out[at] = (name, args, nxt, dest)
                    if name == "jump":
                        todo.append(dest)
                        break
                    todo.append(dest)
                if name in ("pattern_end", "loop_again"):      # (loop again: back to its loop's start, always)
                    break
                at = nxt
        return out


# ---------------------------------------------------------------------------------------------------- playing a track

MAX_EVENTS = 200000             # A track played no further than this many commands
MAX_MINUTES = 15                # Tracks not lining up again before this: played straight through to it, no loop
FADE_STEP = 3                   # A fade written as a step every this many ticks
LOOP_SLACK = 4                  # Tracks' periods this many ticks apart (a repeat) taken as the same (the driver drifts)
_ADSR = {"attack_rate": "ar", "decay_rate": "dr", "sustain_level": "sl", "sustain_rate": "sr", "release_rate": "rr",
         "attack_mode": "am", "sustain_mode": "sm", "release_mode": "rm"}
_SKIPPED = {"vibrato_rate_fade": "LFO rate fades", "tremolo_rate_fade": "LFO rate fades",
            "pan_lfo_rate_fade": "LFO rate fades", "portamento": "portamento", "noise_on": "noise",
            "noise_toggle": "noise", "fm_on": "pitch modulation", "fm_toggle": "pitch modulation",
            "alternate_on": "alternate voices"}


def tempo_us(version, raw):
    """A tempo command's value as microseconds a beat: the driver adds it to a 16-bit counter each timer tick (the
    root counter at 33868800 / 8 Hz, divided by 43D1h in version 1.0, 44E8h after), a tick of the music each wrap."""
    if not raw:
        return 60000000
    div = 0x43D1 if version == V1_0 else 0x44E8
    ticks_per_s = raw / 65536.0 * (33868800 / 8) / div
    return int(round(PPQN * 1e6 / ticks_per_s))


def _signed(b):
    return b - 256 if b >= 128 else b


class Track:
    """A track played as the driver plays it. events: [tick, kind, value]: "note" ([key, end tick, its settings: a
    dict of the instrument and the voice's settings when it starts]), "volume", "expression", "pan" (0-127; fades as
    steps), "tempo" (us a beat). end: the tick of its end command (None: it doesn't end); repeat: (tick, period,
    first event index) when it comes to a place again in the same state (it then repeats for ever), else None."""

    def __init__(self, seq, n, horizon):
        self.seq, self.events, self.skipped = seq, [], {}
        self.end = self.repeat = None
        self._play(seq.tracks[n], seq.offsets[n], horizon)

    def _skip(self, what):
        self.skipped[what] = self.skipped.get(what, 0) + 1

    def _fade(self, kind, tick, length, start, target):
        for k in range(FADE_STEP, length + 1, FADE_STEP):
            self.events.append([tick + k, kind, int(round(start + (target - start) * k / length))])
        if length % FADE_STEP:
            self.events.append([tick + length, kind, target])

    def _play(self, cmds, pc, horizon):
        v = self.seq.version
        v3 = v in (V3_0, V3_1, V3_2)
        tick, octave, transpose, tuning = 0, 4, 0, 0
        layer, loop_at, loop_n = 0, [0] * 4, [0] * 4
        pattern_ret = None
        next_len = fixed = 0
        slur = legato = False
        instr, drum = None, None
        adsr, reverb = {}, False
        vol, expr, pan = 127, 127, 64
        last_note, last_len = None, 0
        vib, vib_depth, slide = None, 0, None             # (vibrato: delay, rate, waveform; its depth byte; a slide)
        trem, trem_depth, plfo, plfo_depth = None, 0, None, 0     # (tremolo: delay, rate, waveform; pan LFO: rate, wave)
        overlay, balance = None, 0                        # (a second articulation on the notes, and their balance)
        seen = {}
        # (a loop repeating for ever (loop again) counts its passes; only a branch compares that count, so it is
        # kept at one past the largest branch count: the loop comes back to the same state)
        cap = max([c[1][0] or 256 for c in cmds.values() if c[0] in ("loop_branch", "loop_break")] or [0]) + 1
        for _ in range(MAX_EVENTS):
            if tick > horizon:
                return
            # (a place and the loop state: met again, the track repeats from there)
            state = (pc, layer, tuple(loop_at), tuple(loop_n), pattern_ret, octave)
            if state in seen:
                start, k = seen[state]
                self.repeat = (start, tick - start, k)
                return
            seen[state] = (tick, len(self.events))
            c = cmds.get(pc)
            if c is None:
                return
            name, args, nxt = c[0], c[1], c[2]
            dest = c[3] if len(c) > 3 else None
            jump = None
            if name == "note":
                b = args[0] if len(args) == 1 else (args[0] - 0xF0) * 11
                length = args[1] if len(args) == 2 else 0
                if next_len:
                    length, next_len = next_len, 0
                if fixed:
                    length = fixed
                if not length:
                    length = LENGTHS[b % 11]
                dur = length if (v3 or slur or legato) else max(length - 2, 0)
                last_len = length
                if b < 0x84:                                   # (a key: 12 keys x 11 lengths, 00h-83h)
                    key = octave * 12 + b // 11
                    settings = dict(instr=instr, drum=drum, adsr=tuple(sorted(adsr.items())), reverb=reverb,
                                    tuning=tuning, rel=b // 11, vib=vib, slide=slide, trem=trem, plfo=plfo,
                                    overlay=(overlay, balance) if overlay is not None else None)
                    slide = None                                # (a slide: the next note's only)
                    last_note = [tick, "note", [key + transpose if drum is None else key, tick + dur, settings]]
                    self.events.append(last_note)
                elif b < 0x8F and last_note is not None:       # (a tie: the note before goes on)
                    last_note[2][1] = tick + dur
                tick += length
            elif name == "end":
                self.end = tick
                return
            elif name == "vibrato":
                vib = (args[0], args[1] or 256, args[2] & 0x0F)
            elif name == "vibrato_depth":
                vib_depth = args[0]
                self.events.append([tick, "vib_depth", vib_depth])
            elif name == "vibrato_off":
                vib = None
                self.events.append([tick, "vib_depth", None])
            elif name == "vibrato_depth_fade":
                length, target = args[0] or 256, args[1]
                for k in range(FADE_STEP, length + 1, FADE_STEP):
                    d = (vib_depth & 0x7F) + ((target & 0x7F) - (vib_depth & 0x7F)) * k // length
                    self.events.append([tick + k, "vib_depth", (target & 0x80) | d])
                vib_depth = target
                self.events.append([tick + length, "vib_depth", vib_depth])
            elif name == "tremolo":
                trem = (args[0], args[1] or 256, args[2] & 0x0F)
            elif name == "tremolo_depth":
                trem_depth = args[0]
                self.events.append([tick, "trem_depth", trem_depth])
            elif name == "tremolo_off":
                trem = None
                self.events.append([tick, "trem_depth", None])
            elif name == "tremolo_depth_fade":
                self._fade("trem_depth", tick, args[0] or 256, trem_depth, args[1])
                trem_depth = args[1]
            elif name == "pan_lfo":
                plfo = (args[0] or 256, args[1] & 0x0F)
            elif name == "pan_lfo_depth":
                plfo_depth = args[0]
                self.events.append([tick, "plfo_depth", plfo_depth])
            elif name == "pan_lfo_off":
                plfo = None
                self.events.append([tick, "plfo_depth", None])
            elif name == "pan_lfo_fade":
                self._fade("plfo_depth", tick, args[0] or 256, plfo_depth, args[1])
                plfo_depth = args[1]
            elif name == "overlay_on":                  # (F4h: the program, and a second articulation with it)
                instr, adsr, overlay = ("art", args[0]), {}, args[1]
            elif name == "overlay_off":
                overlay = None
            elif name in ("overlay_balance", "overlay_fade"):
                balance = args[-1] & 0x7F
            elif name == "pitch_slide":
                slide = (args[0] or 256, _signed(args[1]))
            elif name in ("program", "program_no_attack"):
                instr, adsr = ("art", args[0]), {}
            elif name == "program_split":
                instr, adsr = ("split", args[0]), {}
            elif name == "drum_on_v1":
                drum = ("kit_at", nxt + s16(args, 0))
            elif name == "drum_on":
                drum = ("kit", None)
            elif name == "drum_off":
                drum = None
            elif name == "next_length":
                next_len = last_len = args[0]
            elif name == "fixed_length":
                fixed = min(max(last_len + _signed(args[0]), 1), 255)
            elif name == "octave":
                octave = args[0] & 0x0F
            elif name == "octave_up":
                octave = (octave + 1) & 0x0F
            elif name == "octave_down":
                octave = (octave - 1) & 0x0F
            elif name == "transpose":
                transpose = _signed(args[0])
            elif name == "transpose_rel":
                transpose += _signed(args[0])
            elif name == "tuning":
                tuning = _signed(args[0])
            elif name == "tuning_rel":
                tuning = max(-128, min(127, tuning + _signed(args[0])))
            elif name == "volume":
                vol = args[0] & 0x7F
                self.events.append([tick, "volume", vol])
            elif name == "volume_fade":
                self._fade("volume", tick, args[0] or 256, vol, args[1] & 0x7F)
                vol = args[1] & 0x7F
            elif name == "expression":
                expr = args[0] & 0x7F
                self.events.append([tick, "expression", expr])
            elif name in ("expression_fade", "expression_fade_note"):
                self._fade("expression", tick, args[0] or 256, expr, args[1] & 0x7F)
                expr = args[1] & 0x7F
            elif name == "pan":
                pan = args[0] & 0x7F
                self.events.append([tick, "pan", pan])
            elif name == "pan_fade":
                self._fade("pan", tick, args[0] or 256, pan, args[1] & 0x7F)
                pan = args[1] & 0x7F
            elif name == "tempo":
                self.events.append([tick, "tempo", tempo_us(v, u16(args, 0))])
            elif name == "tempo_fade":
                self.events.append([tick + (args[0] or 256), "tempo", tempo_us(v, u16(args, 1))])
            elif name in _ADSR:
                adsr[_ADSR[name]] = args[0]
            elif name == "decay_sustain":
                adsr["dr"], adsr["sl"] = args[0], args[1]
            elif name == "reset_adsr":
                adsr = {}
            elif name == "reverb_on":
                reverb = True
            elif name == "reverb_off":
                reverb = False
            elif name in ("slur_on", "slur_off"):
                slur = name == "slur_on"
            elif name in ("legato_on", "legato_off"):
                legato = name == "legato_on"
            elif name == "loop_start":
                layer = (layer + 1) & 3
                loop_at[layer], loop_n[layer] = nxt, 0
            elif name == "loop_until":
                loop_n[layer] += 1
                if loop_n[layer] == (args[0] or 256):
                    layer = (layer - 1) & 3
                else:
                    jump = loop_at[layer]
            elif name == "loop_again":
                loop_n[layer] = min(loop_n[layer] + 1, cap)
                jump = loop_at[layer]
            elif name == "loop_branch":
                if loop_n[layer] + 1 == (args[0] or 256):
                    jump = dest
            elif name == "loop_break":
                if loop_n[layer] + 1 == (args[0] or 256):
                    jump = dest
                layer = (layer - 1) & 3
            elif name == "jump":
                jump = dest
            elif name == "cond_jump":
                if args[0] == 0:                    # (the game's condition: 0, as when the music starts)
                    jump = dest
            elif name == "pattern":
                pattern_ret, jump = nxt, dest
            elif name == "pattern_end":
                jump, pattern_ret = pattern_ret, None
            elif name in _SKIPPED:
                self._skip(_SKIPPED[name])
            pc = jump if jump is not None else nxt


# ---------------------------------------------------------------------------------------------------- instruments

def adsr_regs(ar, dr, sl, sr, rr, am, sm, rm):
    """The SPU's ADSR registers from AKAO's rates and modes (a mode's bit 2: exponential; the sustain mode's bit 1:
    decreasing)."""
    a1 = (am & 4) << 13 | (ar & 0x7F) << 8 | (dr & 0x0F) << 4 | (sl & 0x0F)
    a2 = (sm & 4) << 13 | (sm & 2) << 13 | (sr & 0x7F) << 6 | (rm & 4) << 3 | (rr & 0x1F)
    return a1, a2


def with_adsr(a1, a2, changes):
    """ADSR registers with AKAO's ADSR commands applied (changes: {"ar": rate...})."""
    f = dict(am=a1 >> 13 & 4, ar=a1 >> 8 & 0x7F, dr=a1 >> 4 & 0x0F, sl=a1 & 0x0F, sm=a2 >> 13 & 6,
             sr=a2 >> 6 & 0x7F, rm=a2 >> 3 & 4, rr=a2 & 0x1F)
    f.update(changes)
    return adsr_regs(f["ar"], f["dr"], f["sl"], f["sr"], f["rr"], f["am"], f["sm"], f["rm"])


class Articulation:
    """A sample with its loop, root key (fractional: the key its sample plays at as it is) and ADSR registers."""

    def __init__(self, samples, start, loop, root, adsr1, adsr2):
        self.samples, self.start, self.loop = samples, start, loop      # (loop: from the sample's start, or None)
        self.root, self.adsr1, self.adsr2 = root, adsr1, adsr2


def _semitones(ratio):
    return 12 * math.log2(ratio) if ratio > 0 else 0.0


class SampleSet:
    """An AKAO sample set (version 3.1 and later: 16-byte articulations): id, spu (where it is loaded), first (its
    first articulation's ID), count, data (its samples), arts {ID: Articulation}."""

    def __init__(self, data):
        if data[:4] != MAGIC or u16(data, 6) or len(data) < 0x40:
            raise FormatError("not an AKAO sample set")
        self.id, self.spu, size = u16(data, 4), u32(data, 0x10), u32(data, 0x14)
        self.first, self.count = u32(data, 0x18), u32(data, 0x1C)
        if not 1 <= self.count <= 256 or self.first > 1024 or u32(data, 0x40) != 0:
            raise FormatError("not an AKAO sample set (version 3.1+)")
        at = 0x40 + self.count * 16
        self.data = data[at:at + size]
        self.size = size
        self.arts = {}
        for i in range(self.count):
            a = 0x40 + 16 * i
            start, loop = u32(data, a), u32(data, a + 4)
            fine, key = s16(data, a + 8), u16(data, a + 10)
            if start >= len(self.data):
                continue
            ratio = 1 + fine / 32768.0 if fine >= 0 else (fine & 0xFFFF) / 65536.0
            self.arts[self.first + i] = Articulation(self.data, start, loop - start if loop >= start else None,
                                                     key - _semitones(ratio), u16(data, a + 12), u16(data, a + 14))


def instr_all(all_data, dat_data, first=0):
    """Final Fantasy VII's SOUND/INSTR.ALL (its SPU address, its samples' size, 8 more bytes, the samples) and
    INSTR.DAT (64 bytes an articulation: the sample's and its loop's SPU addresses, AR, DR, SL, SR, RR, the attack,
    sustain and release modes, then the pitch of each of the 12 keys, the first (C) of key 72): {ID: Articulation}."""
    spu, size = u32(all_data, 0), u32(all_data, 4)
    samples = all_data[0x10:0x10 + size]
    arts = {}
    for i in range(len(dat_data) // 64):
        a = 64 * i
        start, loop = u32(dat_data, a), u32(dat_data, a + 4)
        if not spu <= start <= loop or start - spu >= len(samples):
            continue
        ar, dr, sl, sr, rr, am, sm, rm = dat_data[a + 8:a + 16]
        pitch = u32(dat_data, a + 16)
        a1, a2 = adsr_regs(ar, dr, sl, sr, rr, am, sm, rm)
        arts[first + i] = Articulation(samples, start - spu, loop - start, 72 - _semitones(pitch / 4096.0), a1, a2)
    return arts


class Region:
    """A tone of an instrument: an articulation over keys lo-hi, with its ADSR changes, volume (0-127), pan (None:
    the channel's) and, for a drum, the key its sample is played at (None: the note's)."""

    def __init__(self, art, lo, hi, adsr=(), vol=127, pan=None, play_key=None, reverb=None):
        self.art, self.lo, self.hi, self.adsr = art, lo, hi, dict(adsr)
        self.vol, self.pan, self.play_key, self.reverb = vol, pan, play_key, reverb


def _region_adsr(ar, sr, sm, rr):
    """A region's ADSR bytes (version 3): attack rate, sustain rate, sustain mode, release rate."""
    return dict(ar=ar & 0x7F, sr=sr & 0x7F, sm=(sm & 6), rr=rr & 0x1F)


def instruments(seq):
    """A version 3 sequence's own instruments (FEh 14h: 16 at most) and drum kit (FEh 04h: one region a key):
    ({number: [Region]}, {key: Region})."""
    d, out, kit = seq.data, {}, {}
    if seq.instruments_at is not None:
        base = seq.instruments_at
        for i in range(16):
            if base + 2 * i + 2 > seq.size:
                break
            p = u16(d, base + 2 * i)
            if p == 0xFFFF or (p == 0 and i):
                continue
            at, regs = base + 0x20 + p, []
            while at + 8 <= seq.size and u32(d, at):
                art, lo, hi, ar, sr, sm, rr, vol = d[at:at + 8]
                if not regs or (hi > regs[-1].hi and lo > regs[-1].hi):
                    lo = 0 if not regs else regs[-1].hi + 1
                    regs.append(Region(art, lo, hi, _region_adsr(ar, sr, sm, rr), (vol or 128) * 127 // 128))
                at += 8
            if regs:
                regs[-1].hi = 127
                out[i] = regs
    if seq.drums_at is not None:
        for key in range(128):
            at = seq.drums_at + 8 * key
            if at + 8 > seq.size:
                break
            if not u32(d, at) and not u32(d, at + 4):
                continue
            art, rel, ar, sr, sm, rr, vol, panrev = d[at:at + 8]
            kit[key] = Region(art, key, key, _region_adsr(ar, sr, sm, rr), (vol or 128) * 127 // 128,
                              panrev & 0x7F, rel, bool(panrev & 0x80))
    return out, kit


def kit_v1(seq, at):
    """A version 1 drum kit (ECh): 12 regions of 5 bytes (version 2: 6), one a key of the octave: articulation, the
    key it is played at, volume (16-bit, 7F00h full), pan: {key 0-11: Region}."""
    size = 6 if seq.version == V2 else 5
    kit = {}
    for k in range(12):
        p = at + k * size
        if p + size > seq.size:
            break
        e = seq.data[p:p + size]
        if not any(e):
            continue
        vol = min(127, u16(e, 2) * 127 // (127 * 128)) if u16(e, 2) else 127
        kit[k] = Region(e[0], k, k, (), vol, e[4] & 0x7F, e[1])
    return kit


# ---------------------------------------------------------------------------------------------------- SEQ + VAB

def _lcm(a, b):
    return a // math.gcd(a, b) * b


def _varlen(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append(0x80 | (v & 0x7F))
        v >>= 7
    return bytes(reversed(out))


def _tune(root):
    """A VAB tone's center note and shift (1/128 semitones up) for a root key (fractional)."""
    center = math.ceil(root - 1e-9)
    shift = int(round((center - root) * 128))
    if shift >= 128:
        center, shift = center - 1, shift - 128
    return max(0, min(127, center)), max(0, min(127, shift))


def _vag(art):
    """An articulation's sample as a VAB's VAG: from its start to its block with the end flag; the loop flags set
    from its loop (the driver sets the SPU's loop address itself)."""
    d = art.samples
    end = art.start
    while end + 16 <= len(d):
        end += 16
        if d[end - 15] & 1:
            break
    v = bytearray(d[art.start:end])
    if len(v) < 16:
        v = bytearray(32)
    for p in range(0, len(v), 16):
        v[p + 1] &= ~7
    loop = art.loop
    if loop is not None and 0 <= loop < len(v) and not loop % 16:
        v[loop + 1] |= 4
        v[-15] |= 3
    else:
        v[-15] |= 1
    return bytes(v)


CHANNELS = 16
BEND_TICKS = 2                  # A vibrato or slide written as a pitch bend every this many ticks (when it moved)
BEND_CENTS = 1.0                # ... by this many cents at least


def _sine(steps, amp):
    return [int(round(amp * math.sin(2 * math.pi * k / steps))) for k in range(steps)]


def _triangle(amp):
    q = [0, amp // 4, amp // 2, amp * 3 // 4, amp, amp * 3 // 4, amp // 2, amp // 4]
    return q + [-x for x in q]


def _random(n, seed, negative_only=False):
    out, x = [], seed
    for _ in range(n):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        v = (x >> 15) & 0xFFFF
        out.append(-(v >> 1) if negative_only else v - 32768)
    return out


def lfo_waves(version):
    """The driver's 16 LFO waveforms (vibrato, tremolo, pan LFO): (values, the length of the part that repeats) each.
    The same shapes as its tables (the same in FF7, FF8 and Vagrant Story but 13-15): steps, square and pulse waves,
    sines of 39, 20 and 10 steps, triangles of 16, four cycles growing to their full size, random values."""
    full = 32767
    grow = lambda wave: [v * k // 4 for k in range(1, 5) for v in wave]
    w = [([8191, -8191, 16383, -16383, 24575, -24575, full, -full], 2), ([full, -full], 2),
         ([8191, 0, 16383, 0, 24575, 0, full], 2), ([full, 0, full], 2),
         ([-8191, 0, -16383, 0, -24575, 0, -full], 2), ([-full, 0, -full], 2),
         (grow(_sine(39, full)), 39), (_sine(39, full), 39), (grow(_triangle(full)), 16), (_triangle(full), 16),
         (grow(_sine(20, full)), 20), (_sine(20, full), 20), (_random(256, 1), 256)]
    if version in (V3_0, V3_1, V3_2):
        w += [(_random(256, 2, True), 256), (grow(_sine(10, full)), 10), (_sine(10, full), 10)]
    else:
        w += [w[9], w[8], w[9]]
    return w


_WAVES = {}


def _wave_at(version, kind, i):
    v3 = version in (V3_0, V3_1, V3_2)
    if v3 not in _WAVES:
        _WAVES[v3] = lfo_waves(version)
    waves = _WAVES[v3]
    values, loop = waves[kind & 15]
    if i < len(values):
        return values[i]
    return values[len(values) - loop + (i - len(values)) % loop]


def driver_hz(version):
    """The driver's updates a second (its timer): LFOs step at this rate."""
    return 33868800 / 8.0 / (0x43D1 if version == V1_0 else 0x44E8)


def pitch_curve(version, start, end, settings, depth_at, tempo):
    """A note's pitch, in cents from its own, as the driver moves it: [(tick, cents)] from its start, every
    BEND_TICKS when it moved by BEND_CENTS (empty: it doesn't move).

    - A slide (A4h: length, semitones): the driver steps the SPU pitch in a straight line (in Hz) from the note's to
      the note's plus the semitones, over the length (ticks).
    - Vibrato (B4h: delay, rate, waveform; B5h: depth): from the note's start plus the delay (ticks), the waveform's
      next value every rate driver updates (driver_hz); the pitch moved by depth / 128 of it (bit 7 of the depth
      clear: 15/256 of that), upward values doubled (the driver's pitch is linear: an octave up doubles it)."""
    slide, vib = settings.get("slide"), settings.get("vib")
    us_tick = tempo / float(PPQN)
    hz = driver_hz(version)
    out, last = [], None
    for tick in list(range(start, end, BEND_TICKS)) + [end - 1]:
        cents = 0.0
        if slide:
            length, semis = slide
            part = min(1.0, (tick - start) / float(length))
            cents += 1200 * math.log2(1 + (2 ** (semis / 12.0) - 1) * part)
        if vib:
            delay, rate, kind = vib
            depth = depth_at(tick)
            if depth and depth & 0x7F and tick - start >= delay:
                secs = (tick - start - delay) * us_tick / 1e6
                w = _wave_at(version, kind, int(secs * hz / rate))
                ratio = (depth & 0x7F) / 128.0 * (1.0 if depth & 0x80 else 15 / 256.0)
                off = ratio * w / 65536.0 * (2 if w > 0 else 1)
                cents += 1200 * math.log2(max(0.01, 1 + off))
        if last is None or abs(cents - last) >= BEND_CENTS or (tick == end - 1 and cents != last and not out):
            out.append((tick, cents))
            last = cents
    if all(abs(c) < 0.5 for _, c in out):
        return []
    return out


def lfo_curve(version, kind, start, end, lfo, depth_at, base_at, tempo):
    """A note's expression (kind "trem") or pan ("pan") as the driver's tremolo or pan LFO moves it: [(tick, value)]
    from its start, every BEND_TICKS when the value changed (empty: it doesn't move). Read in FF7's driver (its update
    at 8002EE20h-8002EFACh, B8h at 80032804h, BCh at 8003298Ch):

    - Tremolo (B8h: delay, rate, waveform; B9h: depth 0-255): from the note's start plus the delay (ticks), the
      waveform's next value every rate driver updates; the volume moved by volume x depth / 128 x value / 32768 (up as
      much as down): here the track's expression, scaled the same.
    - Pan LFO (BCh: rate, waveform; BDh: depth): no delay; the pan moved by depth / 2 x value / 32768."""
    us_tick = tempo / float(PPQN)
    hz = driver_hz(version)
    out, last, moved = [], None, False
    if kind == "trem":
        delay, rate, wave = lfo
    else:
        (rate, wave), delay = lfo, 0
    for tick in list(range(start, end, BEND_TICKS)) + [end - 1]:
        base = base_at(tick)
        value = base
        depth = depth_at(tick)
        if depth and tick - start >= delay:
            secs = (tick - start - delay) * us_tick / 1e6
            w = _wave_at(version, wave, int(secs * hz / rate))
            if kind == "trem":
                value = base * (1 + depth / 128.0 * w / 32768.0)
            else:
                value = base + (depth >> 1) * w / 32768.0
        value = max(0, min(127, int(round(value))))
        moved = moved or value != base
        if value != last:
            out.append((tick, value))
            last = value
    return out if moved else []


def _bend_value(cents, bend_range):
    """A SEQ pitch bend (0-16383, 8192 none) for cents over a bend range (semitones)."""
    return max(0, min(16383, 8192 + int(round(cents / (bend_range * 100.0) * 8192))))


TESTER_ROOM = 880 * 1024        # The sound tester's room for SEQ + VH + VB (896 KB, less its header)
# Ways to fit it, tried in turn: (ticks between two effect steps, minutes a song that doesn't loop is played)
FIT_PLANS = ((2, 15), (4, 15), (8, 15), (8, 10), (8, 5))


def convert(seq, arts):
    """convert_once() made to fit the sound tester (TESTER_ROOM): if the files are bigger, the effects (vibrato,
    slides, tremolo, pan LFO) are written in coarser steps, then a song that doesn't loop is played for less time;
    info["fitted"] says what was done."""
    global BEND_TICKS, MAX_MINUTES
    saved = BEND_TICKS, MAX_MINUTES
    try:
        for step, minutes in FIT_PLANS:
            BEND_TICKS, MAX_MINUTES = step, minutes
            out = convert_once(seq, arts)
            if sum(len(x) for x in out[:3]) <= TESTER_ROOM:
                break
        if (step, minutes) != FIT_PLANS[0]:
            out[3]["fitted"] = "effects every %d ticks%s, to fit the tester's %d KB" % (
                step, "" if minutes == 15 else ", played %d minutes" % minutes, TESTER_ROOM // 1024)
            out[3]["skipped"].append("simplified: " + out[3]["fitted"] + " (1)")
        return out
    finally:
        BEND_TICKS, MAX_MINUTES = saved


def convert_once(seq, arts):
    """A sequence and the articulations it plays ({ID: Articulation}) as a SEQ and a VAB: (seq bytes, vh, vb, info).

    The tracks are played as the driver plays them (their own loops and jumps) and merged with one loop where all
    the tracks with notes repeat together (as aad.to_seq), if within MAX_MINUTES. Each note becomes a tone of the VAB
    with the instrument, the ADSR changes, the reverb and the tuning it starts with (a program for each set of them);
    the notes are given SEQ channels as they start (AKAO has up to 32 tracks, a SEQ 16 channels): a track keeps its
    channel while it is free; the channel's volume, expression and pan set before its note. Vibrato and pitch slides
    become pitch bends (pitch_curve), tremolo and pan LFO expression and pan changes (lfo_curve), such a note having a
    channel of its own; the VAB's bend range set for the widest. Overlay voices: a second tone in the program.
    Not converted: portamento, noise, pitch modulation, alternate voices."""
    own, kit3 = instruments(seq) if seq.version in (V3_0, V3_1, V3_2) else ({}, {})
    first_tempo = None
    probe = [Track(seq, n, PPQN * 8) for n in range(len(seq.tracks))]
    for t in probe:
        for e in t.events:
            if e[1] == "tempo" and e[0] == 0:
                first_tempo = e[2]
    tempo = first_tempo or 500000
    cap = int(MAX_MINUTES * 60e6 / tempo * PPQN)
    played = [Track(seq, n, cap) for n in range(len(seq.tracks))]
    ends = [t.end for t in played if t.end is not None]
    seq_end = max(ends) if ends and all(t.end is not None or not t.events for t in played) else None
    musical = [t for t in played if any(e[1] == "note" for e in t.events)]
    # (a track whose repeating part has no notes is over before it: as a track that ends, its notes must end before
    # the loop)
    looping = [t for t in musical if t.repeat and any(e[1] == "note" for e in t.events[t.repeat[2]:])]
    over = [t for t in musical if t not in looping]
    reps = [t.repeat for t in looping]
    loop = [None, None]
    if reps:
        # (a period a few ticks off a whole part of the longest one: taken as that part, as aad.to_seq)
        longest = max(p for _, p, _ in reps)
        period = longest
        for _, p, _ in reps:
            k = max(1, round(longest / max(1, p)))
            if abs(k * p - longest) > LOOP_SLACK * k:
                period = _lcm(period, max(1, p))
        start = max(s for s, _, _ in reps)
        last_over = max([e[2][1] for t in over for e in t.events if e[1] == "note"] or [0])
        if last_over > start:                           # (the loop later, by whole periods, after those notes)
            start += -(-(last_over - start) // period) * period
        if start + period <= cap:
            loop = [start, start + period]
    if loop[0] is not None:
        end_tick = loop[1]
    elif reps:                                          # (tracks that don't line up: played through to the cap)
        end_tick = min(cap, seq_end if seq_end is not None else cap)
    elif seq_end is not None:
        end_tick = min(cap, seq_end)
    else:
        last = [max([e[2][1] if e[1] == "note" else e[0] for e in t.events] or [0]) for t in played]
        end_tick = min(cap, max(last or [0]))
    # Each track's events up to the end (its repeating part written again)
    actions, skipped = [], {}
    for t in played:
        for k, v in t.skipped.items():
            skipped[k] = skipped.get(k, 0) + v
    for tr, t in enumerate(played):
        evs = [list(e) for e in t.events]
        if t.repeat and t.repeat[1]:
            start, period, first = t.repeat
            block, r = t.events[first:], 1
            while start + r * period < end_tick and len(evs) < MAX_EVENTS * 4:
                for e in block:
                    if e[1] == "note":
                        evs.append([e[0] + r * period, "note", [e[2][0], e[2][1] + r * period, e[2][2]]])
                    else:
                        evs.append([e[0] + r * period, e[1], e[2]])
                r += 1
        for e in evs:
            if e[0] < end_tick or (e[0] == end_tick and e[1] != "note"):
                actions.append((e[0], tr, e[1], e[2]))
    # The notes' tones: a program for each instrument (or drum kit part) with the same settings
    programs, tone_of, missing = {}, {}, {}
    order = []

    def tone(settings, key):
        instr, drum = settings["instr"], settings["drum"]
        changes = dict(settings["adsr"])
        if drum is not None:
            kit = kit3 if drum[0] == "kit" else kit_v1(seq, drum[1])
            idx = key if drum[0] == "kit" else settings["rel"]
            reg = kit.get(idx)
            if reg is None:
                return None, "drum keys with no drum"
            pk = ("kit", drum, settings["adsr"], settings["reverb"], settings["tuning"])
            regs = [Region(reg.art, key, key, reg.adsr, reg.vol, reg.pan, reg.play_key, reg.reverb)]
        elif instr is None:
            return None, "notes before a program change"
        elif instr[0] == "art":
            ov = settings.get("overlay")
            pk = ("art", instr[1], settings["adsr"], settings["reverb"], settings["tuning"], ov)
            if ov:                                      # (F4h: both articulations, their volumes by the balance)
                regs = [Region(instr[1], 0, 127, vol=max(1, 127 - ov[1])), Region(ov[0], 0, 127, vol=max(1, ov[1]))]
            else:
                regs = [Region(instr[1], 0, 127)]
        else:
            if instr[1] not in own:
                return None, "an instrument the sequence doesn't have"
            pk = ("split", instr[1], settings["adsr"], settings["reverb"], settings["tuning"])
            regs = own[instr[1]]
        made = []
        for reg in regs:
            art = arts.get(reg.art)
            if art is None:
                return None, "articulations not in the sample sets"
            adsr = dict(reg.adsr)
            adsr.update(changes)
            root = art.root
            if reg.play_key is not None:                    # (a drum: its sample played at its own key)
                root += key - reg.play_key
            t = settings["tuning"]
            if t:
                root -= _semitones(1 + t / (128.0 if t > 0 else 256.0))
            reverb = settings["reverb"] if reg.reverb is None else reg.reverb
            made.append((reg.art, reg.lo, reg.hi, with_adsr(art.adsr1, art.adsr2, adsr), reg.vol,
                         64 if reg.pan is None else reg.pan, _tune(root), reverb))
        if pk[0] == "kit":                                   # (a kit's keys: 16 a program)
            chunk = 0
            while True:
                tones = programs.setdefault(pk + (chunk,), [])
                if made[0] in tones:
                    return pk + (chunk,), None
                if len(tones) < 16:
                    tones.append(made[0])
                    if pk + (chunk,) not in order:
                        order.append(pk + (chunk,))
                    return pk + (chunk,), None
                chunk += 1
        if pk not in programs:
            programs[pk] = made[:16]
            order.append(pk)
        return pk, None

    notes = []
    for tick, tr, kind, val in actions:
        if kind != "note":
            continue
        key, end, settings = val
        if end <= tick:
            continue
        pk, why = tone(settings, key)
        if pk is None:
            missing[why] = missing.get(why, 0) + 1
            continue
        notes.append((tick, tr, key, end, pk, settings))
    if len(order) > 128:
        skipped["programs over 128 (their notes)"] = len(order) - 128
    number = {pk: i for i, pk in enumerate(order[:128])}
    # The notes' pitch curves (vibrato, slides): the tempo and each track's vibrato depth over time
    tempos = sorted((tick, val) for tick, tr, kind, val in actions if kind == "tempo")
    tempo_ticks = [x[0] for x in tempos]
    depths = {}
    for tick, tr, kind, val in sorted((a for a in actions if a[2] == "vib_depth"), key=lambda a: (a[1], a[0])):
        depths.setdefault(tr, ([], []))
        depths[tr][0].append(tick)
        depths[tr][1].append(val)

    def tempo_at(tick):
        i = bisect.bisect_right(tempo_ticks, tick) - 1
        return tempos[i][1] if i >= 0 else tempo

    def depth_at(tr, tick):
        d = depths.get(tr)
        i = bisect.bisect_right(d[0], tick) - 1 if d else -1
        return d[1][i] if i >= 0 else 0

    curves, widest = {}, 0.0
    for n, (tick, tr, key, end, pk, settings) in enumerate(notes):
        if pk in number and (settings.get("vib") or settings.get("slide")):
            c = pitch_curve(seq.version, tick, min(end, end_tick), settings, lambda t, tr=tr: depth_at(tr, t),
                            tempo_at(tick))
            if c:
                curves[n] = c
                widest = max([widest] + [abs(x) for _, x in c])
    bend_range = max(2, min(96, int(math.ceil(widest / 100.0 + 0.01))))
    # Tremolo and pan LFO: the notes' expression and pan as they move (the track's own, moved by the LFO)
    timelines = {}
    for tick, tr, kind, val in sorted((a for a in actions if a[2] in ("trem_depth", "plfo_depth", "expression",
                                                                       "pan")), key=lambda a: (a[1], a[2], a[0])):
        tl = timelines.setdefault((tr, kind), ([], []))
        tl[0].append(tick)
        tl[1].append(val)

    def value_at(tr, kind, tick, default):
        tl = timelines.get((tr, kind))
        i = bisect.bisect_right(tl[0], tick) - 1 if tl else -1
        return tl[1][i] if i >= 0 else default

    cc_curves = {}                                      # note -> {11 or 10: [(tick, value)]}
    for n, (tick, tr, key, end, pk, settings) in enumerate(notes):
        if pk not in number:
            continue
        for cc, lfo, kind, depth_kind, base_kind, base0 in ((11, settings.get("trem"), "trem", "trem_depth",
                                                             "expression", 127),
                                                            (10, settings.get("plfo"), "pan", "plfo_depth", "pan", 64)):
            if lfo:
                c = lfo_curve(seq.version, kind, tick, min(end, end_tick), lfo,
                              lambda t, tr=tr, k=depth_kind: value_at(tr, k, t, 0),
                              lambda t, tr=tr, k=base_kind, b=base0: value_at(tr, k, t, b), tempo_at(tick))
                if c:
                    cc_curves.setdefault(n, {})[cc] = c
    # The SEQ: notes given channels as they start
    body = []
    track_state = {}
    chans = [dict(owner=None, sounding={}, volume=None, expression=None, pan=None, prog=None, bend=8192, bent=set(),
                  lfo={}) for _ in range(CHANNELS)]
    track_chan = {}
    timeline = []
    for tick, tr, kind, val in actions:
        if kind in ("volume", "expression", "pan"):
            timeline.append((tick, 1, tr, kind, val))
        elif kind == "tempo":
            timeline.append((tick, 1, tr, kind, val))
    for n, (tick, tr, key, end, pk, _) in enumerate(notes):
        if pk in number:
            timeline.append((tick, 2, tr, "on", (key, pk, n)))
            timeline.append((min(end, end_tick), 0, tr, "off", (key, n)))      # (cut at the SEQ's end)
            for bt, cents in curves.get(n, [])[1:]:
                if bt < min(end, end_tick):
                    timeline.append((bt, 3, tr, "bend", (key, n, _bend_value(cents, bend_range))))
            for cc, c in cc_curves.get(n, {}).items():
                for bt, value in c[1:]:
                    if bt < min(end, end_tick):
                        timeline.append((bt, 3, tr, "cc", (key, n, cc, value)))
    timeline.sort(key=lambda e: (e[0], e[1], e[2]))
    shared = dropped = unbent = 0
    for tick, _, tr, kind, val in timeline:
        st = track_state.setdefault(tr, dict(volume=127, expression=127, pan=64))
        if kind == "tempo":
            body.append((tick, b"\xFF\x51" + val.to_bytes(3, "big")))
            continue
        if kind in ("volume", "expression", "pan"):
            st[kind] = val
            c = track_chan.get(tr)
            cc = {"volume": 7, "expression": 11, "pan": 10}[kind]
            if c is not None and any(cc in s for s in chans[c]["lfo"].values()):
                continue                                # (an LFO moves it: its curve has the track's value in it)
            if c is not None and chans[c]["sounding"] and all(t == tr for t, _ in chans[c]["sounding"].values()):
                if chans[c][kind] != val:
                    body.append((tick, bytes((0xB0 | c, {"volume": 7, "expression": 11, "pan": 10}[kind], val))))
                    chans[c][kind] = val
            continue
        if kind == "off":
            key, n = val
            c = track_chan.get(("note", n))
            if c is not None and chans[c]["sounding"].get(key, (None, None))[1] == n:
                del chans[c]["sounding"][key]
                chans[c]["bent"].discard(n)
                chans[c]["lfo"].pop(n, None)
                body.append((tick, bytes((0x80 | c, key, 0))))
            continue
        if kind == "cc":
            key, n, cc, value = val
            c = track_chan.get(("note", n))
            name = {11: "expression", 10: "pan"}[cc]
            if c is not None and cc in chans[c]["lfo"].get(n, ()) and chans[c][name] != value:
                body.append((tick, bytes((0xB0 | c, cc, value))))
                chans[c][name] = value
            continue
        if kind == "bend":
            key, n, value = val
            c = track_chan.get(("note", n))
            if c is not None and n in chans[c]["bent"] and chans[c]["bend"] != value:
                body.append((tick, bytes((0xE0 | c, value & 0x7F, value >> 7))))
                chans[c]["bend"] = value
            continue
        key, pk, n = val
        prog = number[pk]
        curve = curves.get(n)
        lfos = cc_curves.get(n, {})
        moving = curve or lfos                          # (bent, or its expression or pan moved: a channel of its own)

        def fits(c, others_ok):
            s = chans[c]["sounding"]
            if key in s and s[key][0] != tr:
                return False
            if all(t == tr for t, _ in s.values()):
                return True
            # (a channel with another track's notes: never for a bent note, nor while one is bent there)
            return (others_ok and not moving and not chans[c]["bent"] and not chans[c]["lfo"] and
                    all(track_state[t] == st for t, _ in s.values() if t != tr))
        own_c = track_chan.get(tr)
        c = own_c if own_c is not None and fits(own_c, False) else None
        if c is None:
            free = [i for i in range(CHANNELS) if not chans[i]["sounding"]]
            if not free and moving:                     # (no channel of its own: played without its bends, LFOs)
                curve, lfos, moving, unbent = None, {}, None, unbent + 1
            if free:
                # (a channel no track has used, else the one with the closest settings)
                c = min(free, key=lambda i: (chans[i]["owner"] is not None,
                                             sum(chans[i][k] != st[k] for k in ("volume", "expression", "pan")),
                                             chans[i]["prog"] != prog))
            else:
                same = [i for i in range(CHANNELS) if fits(i, True)]
                if same:
                    c, shared = same[0], shared + 1
                else:
                    near = [i for i in range(CHANNELS) if key not in chans[i]["sounding"]]
                    if not near:
                        dropped += 1
                        continue
                    c, shared = near[0], shared + 1
        ch = chans[c]
        for k, cc in (("volume", 7), ("expression", 11), ("pan", 10)):
            want = lfos[cc][0][1] if cc in lfos else st[k]
            if ch[k] != want:
                body.append((tick, bytes((0xB0 | c, cc, want))))
                ch[k] = want
        if lfos:
            ch["lfo"][n] = set(lfos)
        if ch["prog"] != prog:
            body.append((tick, bytes((0xC0 | c, prog))))
            ch["prog"] = prog
        value = _bend_value(curve[0][1], bend_range) if curve else 8192
        if ch["bend"] != value and not (ch["bent"] - {n}):
            body.append((tick, bytes((0xE0 | c, value & 0x7F, value >> 7))))
            ch["bend"] = value
        if curve:
            ch["bent"].add(n)
        ch["owner"] = tr
        ch["sounding"][key] = (tr, n)
        track_chan[tr] = c
        track_chan[("note", n)] = c
        body.append((tick, bytes((0x90 | c, key, 127))))
    if shared:
        skipped["notes sharing a channel with another track's (its volume and pan)"] = shared
    if unbent:
        skipped["notes played without their vibrato, slide, tremolo or pan LFO (no channel of their own)"] = unbent
    if dropped:
        skipped["notes with no channel free"] = dropped
    for k, v in missing.items():
        skipped["silent notes: " + k] = v
    # (in the order made: each channel's settings and program before its note; the loop's start first at its tick,
    # its end last)
    body.sort(key=lambda e: e[0])
    if loop[0] is not None:
        at = next((i for i, e in enumerate(body) if e[0] >= loop[0]), len(body))
        body[at:at] = [(loop[0], b"\xB0\x63\x14"), (loop[0], b"\xB0\x06\x7F")]
        at = next((i for i, e in enumerate(body) if e[0] > loop[1]), len(body))
        body.insert(at, (loop[1], b"\xB0\x63\x1E"))
    out = bytearray(b"pQES" + struct.pack(">IH", 1, PPQN) + tempo.to_bytes(3, "big") + bytes((4, 2)))
    last = 0
    for tick, d in body:
        out += _varlen(tick - last) + d
        last = tick
    out += _varlen(max(0, end_tick - last)) + b"\xFF\x2F\x00"
    seconds, us, last = 0.0, tempo, 0
    for tick, d in body:
        if d[:2] == b"\xFF\x51":
            seconds += (tick - last) * us / 1e6 / PPQN
            us, last = int.from_bytes(d[2:5], "big"), tick
    seconds += (end_tick - last) * us / 1e6 / PPQN
    vh, vb, vinfo = _vab([programs[pk] for pk in order[:128]], arts, bend_range)
    info = dict(version=seq.version, id=seq.id, tracks=len(seq.tracks), notes=sum(1 for e in body if e[1][0] & 0xF0 == 0x90),
                seconds=seconds, loop=loop[0], loop_end=loop[1], tempo=tempo, programs=len(order[:128]),
                tones=vinfo["tones"], samples=vinfo["samples"], sample_set=seq.sample_set, bend_range=bend_range,
                bent=len(curves), tremolo=sum(1 for c in cc_curves.values() if 11 in c),
                pan_lfo=sum(1 for c in cc_curves.values() if 10 in c),
                channels=sorted({e[1][0] & 15 for e in body if e[1][0] & 0xF0 == 0x90}),
                skipped=["%s (%d)" % (k, v) for k, v in sorted(skipped.items())])
    return bytes(out), vh, vb, info


def _vab(programs, arts, bend_range=2):
    """The VAB of the programs (each a list of tones: (articulation, lo, hi, (adsr1, adsr2), vol, pan, (center,
    shift), reverb)): (vh, vb, info)."""
    import vab
    vags, number = [], {}
    for tones in programs:
        for t in tones:
            art = arts[t[0]]
            k = (id(art.samples), art.start, art.loop)
            if k not in number:
                if len(vags) >= 254:
                    raise FormatError("more than 254 samples: a VAB has 254 at most")
                vags.append(_vag(art))
                number[k] = len(vags)
    ps, total = len(programs), sum(len(t) for t in programs)
    tones_at = vab.HEADER_SIZE + vab.PROGRAMS * vab.PROG_SIZE
    sizes_at = tones_at + ps * vab.TONES_PER_PROGRAM * vab.TONE_SIZE
    vh = bytearray(sizes_at + 512)
    vb = b"".join(vags)
    struct.pack_into("<4sIII4H4BI", vh, 0, b"pBAV", 7, 0, len(vh) + len(vb), 0xEEEE, ps, total, len(vags), 127, 64,
                     0, 0, 0xFFFFFFFF)
    for p, tones in enumerate(programs):
        struct.pack_into("<6BH", vh, vab.HEADER_SIZE + p * vab.PROG_SIZE, len(tones), 127, 0, 0, 64, 0, 0)
        for i, (art_id, lo, hi, (a1, a2), vol, pan, (center, shift), reverb) in enumerate(tones):
            art = arts[art_id]
            struct.pack_into("<8B6B2xHHhh", vh, tones_at + (p * vab.TONES_PER_PROGRAM + i) * vab.TONE_SIZE,
                             8, 4 if reverb else 0, min(vol, 127), min(pan, 127), center, shift, lo, hi, 0, 0, 0, 0,
                             bend_range, bend_range, a1, a2, p, number[(id(art.samples), art.start, art.loop)])
    for i, s in enumerate(vags):
        struct.pack_into("<H", vh, sizes_at + (i + 1) * 2, len(s) // 8)
    return bytes(vh), vb, dict(tones=total, samples=len(vags))


# ---------------------------------------------------------------------------------------------------- files

def unlzs(d):
    """Final Fantasy VII's LZS: the compressed size (32-bit), then flag bytes (bit 0 first; 1 a byte as it is, 0 a
    copy of (4 bits + 3) bytes from a 12-bit place in a 4 KB ring that starts at FEEh). None if d isn't one."""
    if len(d) < 8:
        return None
    n = u32(d, 0)
    if not len(d) - 2052 < n <= len(d) - 4:
        return None
    out, ring, r, i, end = bytearray(), bytearray(4096), 0xFEE, 4, 4 + n
    while i < end:
        flags = d[i]
        i += 1
        for b in range(8):
            if i >= end:
                break
            if flags >> b & 1:
                c = d[i]
                i += 1
                out.append(c)
                ring[r] = c
                r = (r + 1) & 0xFFF
            else:
                if i + 1 >= end:
                    return bytes(out)
                lo, hi = d[i], d[i + 1]
                i += 2
                off = lo | (hi & 0xF0) << 4
                for k in range((hi & 0x0F) + 3):
                    c = ring[(off + k) & 0xFFF]
                    out.append(c)
                    ring[r] = c
                    r = (r + 1) & 0xFFF
    return bytes(out)


# "AKAO" stored as it is in LZS data: its 4 bytes, a flag byte perhaps between two of them
_LZS_AKAO = re.compile(b"A[\\x00-\\xff]?K[\\x00-\\xff]?A[\\x00-\\xff]?O")
MEDIA = (".STR", ".XA", ".DA", ".MOV", ".IKI")
MAX_FILE = 8 << 20              # A file read whole up to this size (bigger: searched in place, FF8's archive)
SEQ_WINDOW = 0x10000            # A sequence read up to this size from its "AKAO"
COVERED = 0.999


def _blobs(img, start, size, name):
    """A file's places to search: (name, bytes) for the file and, if it is LZS (FF7), its data decompressed."""
    d = img.read(start, size)
    yield name, d
    if d[:4] != MAGIC and len(d) > 8 and _LZS_AKAO.search(d):
        u = unlzs(d)
        if u and MAGIC in u:
            yield name + " (LZS)", u


def collect(source, progress=None):
    """The AKAO sequences and sample sets of a game's file (a path, discimage.Image or bytes: a disc image is read
    where needed, never whole): (sequences [(name, Sequence)], sample sets [(name, SampleSet)], FF7's articulations
    {ID: Articulation} or None), each once."""
    import discimage
    if not isinstance(source, discimage.Image):
        with discimage.Image(source) as img:
            return collect(img, progress)
    img = source
    import aad
    try:
        files = aad.iso_files(img)
    except Exception:
        files = []
    if not files:
        files = [("", 0, len(img))]
    seqs, sets, seen_seq, seen_set = [], [], set(), set()
    instr = {}

    def take(name, d, p):
        if d[p + 6:p + 8] == b"\0\0":
            try:
                n, size = u32(d, p + 0x1C), u32(d, p + 0x14)
                if not 1 <= n <= 256 or size > 0x80000:
                    return
                s = SampleSet(d[p:p + 0x40 + n * 16 + size])
            except (FormatError, struct.error):
                return
            k = (s.id, s.first, s.count, s.spu, s.size)
            if k not in seen_set:
                seen_set.add(k)
                sets.append((name, s))
            return
        try:
            s = Sequence(d[p:p + SEQ_WINDOW])
        except (FormatError, struct.error):
            return
        if s.data not in seen_seq:
            seen_seq.add(s.data)
            seqs.append((name, s))

    for i, (name, start, size) in enumerate(files):
        if progress:
            progress(i, len(files))
        base = name.rsplit("/", 1)[-1].upper()
        if base.endswith(MEDIA) or not size:
            continue
        if base in ("INSTR.ALL", "INSTR.DAT"):
            instr[base] = img.read(start, size)
            continue
        if size > MAX_FILE:                                 # (an archive: searched in place)
            for p in img.find(MAGIC, start, start + size):
                d = img.read(p, SEQ_WINDOW)
                if len(d) >= 0x40:
                    take("%s +%X" % (name, p - start), d, 0)
            continue
        for nm, d in _blobs(img, start, size, name):
            p = d.find(MAGIC)
            while p >= 0:
                if len(d) - p >= 0x40:
                    take(nm if not p else "%s +%X" % (nm, p), d, p)
                p = d.find(MAGIC, p + 1)
    arts = instr_all(instr["INSTR.ALL"], instr["INSTR.DAT"]) if len(instr) == 2 else None
    return seqs, sets, arts


def articulations_for(seq, sets, ff7=None):
    """The articulations a sequence plays ({ID: Articulation}) and where they are from (text): version 1.0: FF7's
    INSTR.ALL; version 3: the sample set it names (its ID) over the sets the game keeps loaded (those with lower
    articulation IDs, in SPU RAM below it and not overlapping each other: Vagrant Story's WAVE0000 and WAVE0005,
    FF8's sets 0 and 5), as the driver's table of articulations is filled. Empty if none is found."""
    if seq.version == V1_0:
        return (ff7 or {}), ("INSTR.ALL" if ff7 else "")
    all_sets = [s for _, s in sets]
    named = [s for s in all_sets if s.id == seq.sample_set]
    song = max(named, key=lambda s: s.first) if named else None
    top = song.first if song else min([s.first for s in all_sets if s.first > 0] or [64])
    lo, hi = (song.spu, song.spu + song.size) if song else (0, 0)
    chosen = []
    for s in sorted(all_sets, key=lambda x: (x.first, x.spu)):
        if s.first >= top or s is song or any(c.first == s.first for c in chosen):
            continue
        if song and not (s.spu + s.size <= lo or s.spu >= hi):
            continue
        if any(not (s.spu + s.size <= c.spu or s.spu >= c.spu + c.size) for c in chosen):
            continue
        chosen.append(s)
    arts = {}
    for s in chosen + ([song] if song else []):
        arts.update(s.arts)
    names = ["set %d" % s.id for s in chosen + ([song] if song else [])]
    return arts, ", ".join(names)


def scan(source, progress=None):
    """The musics of a game's file: [dict(name, seq (Sequence), arts, sets (text))] in the order found."""
    seqs, sets, ff7 = collect(source, progress)
    out = []
    for name, s in seqs:
        arts, where = articulations_for(s, sets, ff7)
        out.append(dict(name=name, seq=s, arts=arts, sets=where))
    return out


def write_music(m, folder, base):
    """A scanned music written as base.seq, base.vh, base.vb in folder: (paths, info); info["coverage"]: the part
    of its notes with a tone."""
    import os
    data, vh, vb, info = convert(m["seq"], m["arts"])
    paths = [os.path.join(folder, base + e) for e in (".vh", ".vb", ".seq")]
    for p, d in zip(paths, (vh, vb, data)):
        with open(p, "wb") as f:
            f.write(d)
    silent = sum(int(k.rsplit("(", 1)[1].rstrip(")")) for k in info["skipped"] if k.startswith("silent notes"))
    info["coverage"] = info["notes"] / float(info["notes"] + silent) if info["notes"] + silent else 0.0
    return paths, info


def main(argv=None):
    import argparse
    import os
    ap = argparse.ArgumentParser(description="Square's AKAO music (FF7, FF8, FF9, Vagrant Story, Chrono Cross...) "
                                             "found in a game's disc image or files, as SEQ + VAB")
    ap.add_argument("source", help="disc image (.bin, .cue, .iso, .ecm) or a game file")
    ap.add_argument("out", nargs="?", help="folder for the SEQ + VH + VB files (default: list them only)")
    a = ap.parse_args(argv)
    musics = scan(a.source)
    if a.out:
        os.makedirs(a.out, exist_ok=True)
    for n, m in enumerate(musics):
        s = m["seq"]
        line = "%3d  %-40s version %s, id %d, %d tracks, instruments: %s" % (n, m["name"][:40], s.version, s.id,
                                                                            len(s.tracks), m["sets"] or "none found")
        if a.out:
            _, info = write_music(m, a.out, "akao%03d" % n)
            line += " -> %d:%02d, %d notes%s" % (int(info["seconds"]) // 60, int(info["seconds"]) % 60, info["notes"],
                                                ", loops" if info["loop"] is not None else "")
        print(line)
    print("%d musics" % len(musics))


if __name__ == "__main__":
    main()
