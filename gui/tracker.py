"""
PS1 DEV CART GUI: music tracker songs (the Music Tracker page, pages/tracker.py).

A song is riffs (patterns: rows x channels of cells) placed on tracks over time (the arrangement: clips), with
instruments made from samples (WAV, MP3... through audioconv.load) or built-in waveforms. It is compiled to what games
play with libsnd: Sony's SEQ (one MIDI channel per tracker channel) and a VAB (VH + VB: one program per instrument, its
sample over the whole keyboard).

Arrangement: tracks (lanes), each clip a riff on a track from a row of the song for a length (rows: past the riff's
end it plays again). Each track has the channels its riffs use (the first track channels 0..., the next ones after);
a clip ends its notes at its end, and a clip starting on its track cuts the one before. The song loops from the row
"loop" (None: plays once). To be played it is flattened (flatten): the clips' rows in one timeline, cut in patterns
played in order (compile_seq).

Cell: [note, instrument, volume, effect, value], None for an empty field:
- note: 0-127 (60 = C4), NOTE_OFF (the channel's note released)
- instrument: 0-127, the instrument (VAB program) the note plays (empty: the channel's last one)
- volume: 0-127, the note's velocity (empty: 127)
- effect: a letter of EFFECTS and its value 0-255, as SEQ events (what libsnd plays):
    P pan (00 left, 40 centre, 7F right)        V channel volume (00-7F)        E expression (00-7F)
    B pitch bend (00 down, 40 none, 7F up: the instrument's bend range, the notes sounding)    T tempo (BPM, 20-FF)
    G slide: the note playing glides to the cell's note (not played again) over xx rows (00: one row), with pitch
      bends: within the instrument's bend range (further: the note played)
    A arpeggio xy: the note, the note + x, the note + y semitones in turn through the row, with pitch bends within
      the bend range, else the notes played again

Each channel plays one note at a time (a new note releases the one before): the piano roll puts the notes played
together (chords) on channels of their own (add_note). The loop: SEQ loop markers (NRPN 99 = 20 / 30, as Sony's
tools), the channels' state (instrument, volume, pan, expression, bend) and the tempo set again at the loop start.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import copy
import json
import math
import os
import struct

import numpy as np

import audioconv

FORMAT = "ps1devcart-song"
VERSION = 2                     # 2: riffs on tracks (1: patterns in an order, converted when loaded)
EXTENSION = ".dcsong"
RESOLUTION = 480                # SEQ ticks per quarter note
MAX_CHANNELS = 16
MAX_ROWS = 256
MAX_INSTRUMENTS = 128
MAX_TRACKS = 16
LPB_CHOICES = (1, 2, 3, 4, 6, 8, 12, 16)    # Rows per beat (RESOLUTION divides by each)
NOTE_OFF = -1
EFFECTS = {"P": "Pan", "V": "Channel volume", "E": "Expression", "B": "Pitch bend", "T": "Tempo (BPM)",
           "G": "Slide to the note (rows)", "A": "Arpeggio (semitones x, y)"}
STEP_TICKS = RESOLUTION // 16   # A pitch bend of a slide or an arpeggio each 1/64 note (about a frame at 120 BPM)
BEND_RANGES = range(1, 13)      # Instruments' pitch bend range (semitones)
NOTE_NAMES = ("C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-")
TRACK_NAMES = ("Drums", "Bass", "Chords", "Melody")

# Built-in waveforms: one period of WAVE_PERIOD samples at 44100 Hz (787.5 Hz, so notes up to about G7 within the
# SPU's 4x pitch), an attack ramp then a loop of whole periods and SPU blocks (no click). "strings": three saws a few
# cents apart (a longer loop: they beat slowly), "organ": sines on the organ's harmonics.
# Drums: one-shot sounds (not looped), each at its General MIDI drum note (DRUM_WAVES: played there as made).
WAVEFORMS = ("square", "pulse", "saw", "triangle", "sine", "strings", "organ", "noise",
             "kick", "snare", "clap", "hat", "open hat", "tom")
DRUM_WAVES = {"kick": 36, "snare": 38, "clap": 39, "hat": 42, "tom": 45, "open hat": 46}
WAVE_RATE = 44100
WAVE_PERIOD = 56
WAVE_INTRO = 1120               # 20 periods, 40 blocks of 28 samples
WAVE_LOOP = 896                 # 16 periods, 32 blocks
STRINGS_CYCLES = 400            # The strings' loop: 400 periods (22400 samples, 800 blocks), saws of 399, 400, 401
NOISE_SAMPLES = 22050
REVERB_TYPES = 10               # The song's reverb (the sound tester's types, pages/sound.py REVERBS), 0: off


class SongError(Exception):
    pass


def note_text(note):
    if note is None:
        return "..."
    if note == NOTE_OFF:
        return "==="
    return "%s%d" % (NOTE_NAMES[note % 12], note // 12 - 1)


# ---------------------------------------------------------------------------------------------------- song

def new_instrument(name="Square", wave="square", envelope=0):
    return dict(name=name, source="wave", wave=wave, path="", root=60, cents=0, rate=22050, vol=110, pan=64,
                envelope=envelope, loop=False, loop_start=0.0, reverb=False, normalize=True, bend=2)


def new_pattern(rows=64, name="Riff"):
    return dict(name=name, rows=rows, cells={})


def new_clip(track, start, pattern, length):
    return dict(track=track, start=start, pattern=pattern, length=length)


def new_song():
    """A song with a one-bar riff on its Melody track, ready for its notes."""
    return dict(title="", bpm=120, lpb=4, channels=8, loop=0, reverb=3, depth=64, patterns=[new_pattern(16, "Riff 1")],
                tracks=[dict(name=n) for n in TRACK_NAMES], clips=[new_clip(3, 0, 0, 16)],
                instruments=[new_instrument("Square", "square"), new_instrument("Pulse", "pulse"),
                             new_instrument("Saw", "saw"), new_instrument("Triangle", "triangle"),
                             new_instrument("Sine pluck", "sine", envelope=3),
                             new_instrument("Noise hit", "noise", envelope=3)])


def song_from_order(song, order, loop_pos):
    """The arrangement of patterns played in an order (version 1 songs, MIDI import): one after the other on one
    track, the song looped from the start of the position loop_pos (None: no loop)."""
    song["tracks"] = [dict(name="Track 1")]
    song["clips"], start, song["loop"] = [], 0, None
    for i, p in enumerate(order):
        if i == loop_pos:
            song["loop"] = start
        rows = song["patterns"][p]["rows"]
        song["clips"].append(new_clip(0, start, p, rows))
        start += rows
    return song


def save(song, path):
    """The song as JSON; sample paths relative to the song's folder when they can be."""
    folder = os.path.dirname(os.path.abspath(path))
    data = dict(format=FORMAT, version=VERSION, title=song["title"], bpm=song["bpm"], lpb=song["lpb"],
                channels=song["channels"], loop=song["loop"], reverb=song["reverb"], depth=song["depth"],
                tracks=song["tracks"], clips=song["clips"],
                instruments=[], patterns=[])
    for ins in song["instruments"]:
        ins = dict(ins)
        if ins["path"]:
            try:
                ins["path"] = os.path.relpath(ins["path"], folder)
            except ValueError:          # (another drive)
                pass
        data["instruments"].append(ins)
    for pat in song["patterns"]:
        item = dict(name=pat["name"], rows=pat["rows"], cells=[[r, c] + list(cell) for (r, c), cell in
                                                                sorted(pat["cells"].items())])
        if pat.get("source"):
            item["source"] = pat["source"]
        data["patterns"].append(item)
    text = json.dumps(data, indent=1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def load(path):
    with open(path, encoding="utf-8") as f:
        try:
            data = json.load(f)
        except ValueError as e:
            raise SongError("%s: not a song file (%s)" % (os.path.basename(path), e))
    if data.get("format") != FORMAT:
        raise SongError("%s: not a song file" % os.path.basename(path))
    folder = os.path.dirname(os.path.abspath(path))
    song = new_song()
    song["reverb"] = 0                  # (songs saved before the reverb setting: as they were heard)
    for key in ("title", "bpm", "lpb", "channels", "loop", "reverb", "depth", "tracks", "clips"):
        if key in data:
            song[key] = data[key]
    song["instruments"] = []
    for ins in data.get("instruments", []):
        full = new_instrument()
        full.update(ins)
        if full["path"] and not os.path.isabs(full["path"]):
            full["path"] = os.path.normpath(os.path.join(folder, full["path"]))
        song["instruments"].append(full)
    song["patterns"] = []
    for i, pat in enumerate(data.get("patterns", [])):
        cells = {}
        for item in pat.get("cells", []):
            r, c, cell = item[0], item[1], (list(item[2:]) + [None] * 5)[:5]
            cells[(r, c)] = cell
        p = new_pattern(pat.get("rows", 64), pat.get("name") or "Pattern %02d" % i)
        p["cells"] = cells
        if pat.get("source"):
            p["source"] = pat["source"]
        song["patterns"].append(p)
    if not song["patterns"]:
        song["patterns"] = [new_pattern(16, "Riff 1")]
    if "clips" not in data:             # Version 1: patterns in an order
        order = [p for p in data.get("order", [0]) if 0 <= p < len(song["patterns"])] or [0]
        song_from_order(song, order, data.get("loop"))
    return check(song)


def check(song):
    """Values in range (a song file edited by hand, old versions)."""
    song["bpm"] = max(20, min(300, int(song["bpm"])))
    if song["lpb"] not in LPB_CHOICES:
        song["lpb"] = 4
    song["channels"] = max(1, min(MAX_CHANNELS, int(song["channels"])))
    for i, pat in enumerate(song["patterns"]):
        pat.setdefault("name", "Riff %d" % (i + 1))
    if not song.get("tracks"):
        song["tracks"] = [dict(name="Track 1")]
    song["clips"] = [c for c in song.get("clips", []) if 0 <= c["pattern"] < len(song["patterns"]) and
                     0 <= c["track"] < len(song["tracks"]) and c["start"] >= 0 and c["length"] >= 1]
    if song["loop"] is not None and (not isinstance(song["loop"], int) or song["loop"] < 0):
        song["loop"] = 0
    song["reverb"] = max(0, min(REVERB_TYPES - 1, int(song.get("reverb") or 0)))
    song["depth"] = max(0, min(127, int(song.get("depth", 64))))
    for ins in song["instruments"]:
        ins["bend"] = max(BEND_RANGES[0], min(BEND_RANGES[-1], int(ins.get("bend", 2))))
    return song


# ---------------------------------------------------------------------------------------------------- riffs

def pattern_channels(pat):
    """Channels a riff uses (the highest with something in it, + 1; 1 for an empty one)."""
    return max([c + 1 for (r, c), cell in pat["cells"].items() if any(v is not None for v in cell)] or [1])


def pattern_notes(pat):
    """The notes of a riff: [dict(row, ch, note, ins, vol, end)], end: the row where the next note or note off of
    its channel is (the riff's end if none)."""
    by_ch = {}
    for (r, c), cell in pat["cells"].items():
        if r < pat["rows"] and cell[0] is not None:
            by_ch.setdefault(c, []).append((r, cell))
    notes = []
    for c, items in by_ch.items():
        items.sort(key=lambda i: i[0])
        for i, (r, cell) in enumerate(items):
            if cell[0] != NOTE_OFF:
                end = items[i + 1][0] if i + 1 < len(items) else pat["rows"]
                notes.append(dict(row=r, ch=c, note=cell[0], ins=cell[1], vol=cell[2], end=end))
    notes.sort(key=lambda n: (n["row"], n["note"]))
    return notes


def _set_note(cells, r, c, note, ins=None, vol=None):
    """The note fields of a cell (its effect kept; the cell removed when left empty)."""
    cell = list(cells.get((r, c)) or [None] * 5)
    cell[0], cell[1], cell[2] = note, ins, vol
    if all(v is None for v in cell):
        cells.pop((r, c), None)
    else:
        cells[(r, c)] = cell


def channel_free(pat, ch, start, end):
    """No note of the channel sounds from row start to end (excluded)."""
    return not any(n["ch"] == ch and n["row"] < end and n["end"] > start for n in pattern_notes(pat))


def add_note(pat, row, length, note, ins, vol=None, channels=MAX_CHANNELS, prefer=None):
    """A note put on a riff (piano roll) on a channel with nothing sounding then (prefer: tried first, else the
    first one): its note off at its end. Returns the channel, None if every channel is taken."""
    end = min(pat["rows"], row + max(1, length))
    order = ([prefer] if prefer is not None else []) + [c for c in range(channels) if c != prefer]
    for c in order:
        if c < channels and channel_free(pat, c, row, end):
            cells = pat["cells"]
            _set_note(cells, row, c, note, ins, vol)
            nxt = cells.get((end, c))
            if end < pat["rows"] and (nxt is None or nxt[0] is None):
                _set_note(cells, end, c, NOTE_OFF)
            return c
    return None


def remove_note(pat, row, ch):
    """A note taken off a riff (piano roll), with its note off; a note of the channel ending where it started ends
    there still (a note off put)."""
    notes = pattern_notes(pat)
    this = next((n for n in notes if n["row"] == row and n["ch"] == ch), None)
    if not this:
        return
    cells = pat["cells"]
    end = cells.get((this["end"], ch))
    if end and end[0] == NOTE_OFF:
        _set_note(cells, this["end"], ch, None)
    before = any(n["ch"] == ch and n["end"] == row for n in notes)
    _set_note(cells, row, ch, NOTE_OFF if before else None)


# ---------------------------------------------------------------------------------------------------- arrangement

def track_channels(song):
    """The channels of each track: [(first channel, count)] (a track's riffs: the most channels one of them uses,
    0 for a track with no clip), and the total. SongError beyond 16."""
    widths = [0] * len(song["tracks"])
    for clip in song["clips"]:
        widths[clip["track"]] = max(widths[clip["track"]], pattern_channels(song["patterns"][clip["pattern"]]))
    out, base = [], 0
    for w in widths:
        out.append((base, w))
        base += w
    if base > MAX_CHANNELS:
        raise SongError("the tracks' riffs use %d channels together, the PS1 plays 16 (%s): fewer notes at once "
                        "(chords) or fewer tracks" % (base, ", ".join(
                            "%s %d" % (t["name"], w) for t, (b, w) in zip(song["tracks"], out) if w)))
    return out, base


def song_length(song):
    """Rows of the arrangement (its last clip's end)."""
    return max([c["start"] + c["length"] for c in song["clips"]] or [0])


def clip_ends(song):
    """Each clip's end as played: its own, or where the next clip of its track starts (it cuts it)."""
    ends = {}
    for t in range(len(song["tracks"])):
        clips = sorted((c for c in song["clips"] if c["track"] == t), key=lambda c: c["start"])
        for i, c in enumerate(clips):
            end = c["start"] + c["length"]
            if i + 1 < len(clips):
                end = min(end, clips[i + 1]["start"])
            ends[id(c)] = end
    return ends


def flatten(song, loop=True):
    """The arrangement as patterns played in order (compile_seq): a song dict (patterns, order, loop: order
    position, channels) and the first row of each of its patterns."""
    bases, total_ch = track_channels(song)
    total = song_length(song)
    if not total:
        raise SongError("the arrangement is empty: put riffs on the tracks")
    cells, ends = {}, clip_ends(song)
    for clip in sorted(song["clips"], key=lambda c: c["start"]):
        pat = song["patterns"][clip["pattern"]]
        base, width = bases[clip["track"]]
        start, end, n = clip["start"], ends[id(clip)], pat["rows"]
        for rep in range(start, end, n):
            for (r, c), cell in pat["cells"].items():
                if r < n and rep + r < end:
                    cells[(rep + r, base + c)] = list(cell)
        for c in range(base, base + width):         # (its notes end with it)
            cell = cells.get((end, c))
            if end < total and (cell is None or cell[0] is None):
                cell = cell or [None] * 5
                cell[0] = NOTE_OFF
                cells[(end, c)] = cell
    loop_row = song["loop"] if loop and song["loop"] is not None and song["loop"] < total else None
    first = loop_row or 0
    starts = list(range(0, first, MAX_ROWS)) + list(range(first, total, MAX_ROWS))
    patterns = []
    for i, s in enumerate(starts):
        e = starts[i + 1] if i + 1 < len(starts) else total
        pat = new_pattern(e - s, "")
        pat["cells"] = {(r - s, c): cell for (r, c), cell in cells.items() if s <= r < e}
        patterns.append(pat)
    flat = dict(song, patterns=patterns, order=list(range(len(patterns))), channels=max(1, total_ch),
                loop=None if loop_row is None else starts.index(loop_row))
    return flat, starts


def compile_song(song, loop=True, midi=False):
    """SEQ bytes of the arrangement and its timing (compile_seq), its rows as (ms, 0, song row)."""
    flat, starts = flatten(song, loop)
    seq, info = compile_seq(flat, None, "song" if loop else None, midi)
    info["rows"] = [(ms, 0, starts[pos] + r) for ms, pos, r in info["rows"]]
    return seq, info


def compile_riff(song, index, loop=True):
    """SEQ bytes of one riff alone (looped) and its timing (rows: (ms, 0, riff row))."""
    pat = song["patterns"][index]
    one = dict(song, patterns=[pat], order=[0], loop=0, channels=max(song["channels"], pattern_channels(pat)))
    return compile_seq(one, None, "all" if loop else None)


# ---------------------------------------------------------------------------------------------------- instruments

def drum_pcm(kind):
    """A drum sound (float numpy at WAVE_RATE, -1..1)."""
    rng = np.random.default_rng(1234)

    def env(t, decay):
        return np.exp(-t / decay)

    def noise(n):
        return rng.uniform(-1, 1, n)

    def highpass(v, times=1):       # (differences: the low frequencies removed, the hiss kept)
        for _ in range(times):
            v = np.diff(v, prepend=0.0)
        return v

    def smooth(v, k):               # (a moving average: the highest frequencies softened)
        return np.convolve(v, np.ones(k) / k, mode="same")

    def tone(t, start, end, sweep):     # (a sine falling from start to end Hz)
        f = end + (start - end) * np.exp(-t / sweep)
        return np.sin(2 * np.pi * np.cumsum(f) / WAVE_RATE)

    length = {"kick": 0.45, "snare": 0.3, "clap": 0.35, "hat": 0.12, "open hat": 0.5, "tom": 0.5}[kind]
    t = np.arange(int(length * WAVE_RATE)) / WAVE_RATE
    if kind == "kick":
        v = tone(t, 160, 48, 0.035) * env(t, 0.17) + smooth(noise(len(t)), 3) * env(t, 0.003) * 0.4
    elif kind == "snare":
        body = (np.sin(2 * np.pi * 185 * t) + 0.5 * np.sin(2 * np.pi * 330 * t)) * env(t, 0.05)
        v = body * 0.5 + smooth(highpass(noise(len(t))), 2) * env(t, 0.09) * 0.7
    elif kind == "clap":
        hits = sum(np.where(t >= s, env(np.maximum(t - s, 0), 0.006), 0) for s in (0.0, 0.011, 0.022))
        tail = np.where(t >= 0.03, env(np.maximum(t - 0.03, 0), 0.09), 0)
        v = smooth(highpass(noise(len(t))), 4) * (hits * 0.8 + tail * 0.6)
    elif kind in ("hat", "open hat"):
        metal = sum(np.sign(np.sin(2 * np.pi * f * t)) for f in (3110, 4570, 5430, 6680, 7960, 8390)) / 6
        v = (highpass(noise(len(t)), 2) * 0.6 + highpass(metal) * 0.4) * env(t, 0.025 if kind == "hat" else 0.18)
    elif kind == "tom":
        v = tone(t, 150, 110, 0.06) * env(t, 0.22) + smooth(noise(len(t)), 6) * env(t, 0.01) * 0.2
    else:
        raise SongError("unknown drum %s" % kind)
    v[-200:] *= np.linspace(1, 0, 200)     # (no click at the end)
    return v


def wave_pcm(kind):
    """A built-in waveform (int16 numpy at WAVE_RATE), its loop start (samples, None: a one-shot sound) and natural
    note (fractional)."""
    if kind == "noise":
        rng = np.random.default_rng(1234)
        pcm = (rng.uniform(-1, 1, NOISE_SAMPLES) * 0.5 * 32767).astype(np.int16)
        return pcm, 0, 60.0
    if kind in DRUM_WAVES:
        v = drum_pcm(kind)
        v = v / max(1e-9, float(np.abs(v).max())) * 0.9
        return (v * 32767).astype(np.int16), None, float(DRUM_WAVES[kind])
    harmonics = range(1, WAVE_PERIOD // 2 - 2)    # (below the sample rate's half: no aliasing at the root)
    if kind == "strings":
        n = WAVE_INTRO + WAVE_PERIOD * STRINGS_CYCLES
        i = np.arange(n)
        v = 0
        for cycles, phase in ((STRINGS_CYCLES - 1, 0.0), (STRINGS_CYCLES, 2.1), (STRINGS_CYCLES + 1, 4.2)):
            x = 2 * np.pi * cycles * i / (WAVE_PERIOD * STRINGS_CYCLES) + phase
            v = v + sum(np.sin(k * x) / k ** 1.3 for k in range(1, 16))      # (softer than a saw: warmer)
    else:
        n = WAVE_INTRO + WAVE_LOOP
    x = 2 * np.pi * (np.arange(n) % WAVE_PERIOD) / WAVE_PERIOD
    if kind == "strings":
        pass
    elif kind == "organ":
        v = sum(a * np.sin(k * x) for k, a in ((1, 1.0), (2, 0.7), (3, 0.5), (4, 0.35), (6, 0.2), (8, 0.25)))
    elif kind == "sine":
        v = np.sin(x)
    elif kind == "square":
        v = sum(np.sin(k * x) / k for k in harmonics if k % 2) * (4 / np.pi)
    elif kind == "pulse":          # 25% duty
        v = sum(np.sin(k * np.pi * 0.25) * np.cos(k * x) / k for k in harmonics) * (4 / np.pi)
    elif kind == "saw":
        v = sum(np.sin(k * x) * (-1) ** (k + 1) / k for k in harmonics) * (2 / np.pi)
    elif kind == "triangle":
        v = sum(np.sin(k * x) * (-1) ** ((k - 1) // 2) / (k * k) for k in harmonics if k % 2) * (8 / np.pi ** 2)
    else:
        raise SongError("unknown waveform %s" % kind)
    v = v / max(1e-9, float(np.abs(v).max())) * 0.55
    v *= np.minimum(1.0, np.arange(n) / 200.0)
    note = 69 + 12 * math.log2(WAVE_RATE / WAVE_PERIOD / 440.0)
    return (v * 32767).astype(np.int16), WAVE_INTRO, note


class InstrumentCache:
    """The instruments' samples (loaded or made once while their settings and files don't change)."""

    def __init__(self):
        self.items = {}

    def tone(self, ins, program, ffmpeg=""):
        a1, a2 = audioconv.ENVELOPES[max(0, min(len(audioconv.ENVELOPES) - 1, int(ins["envelope"])))][1:]
        base = dict(program=program, min=0, max=127, vol=int(ins["vol"]), pan=int(ins["pan"]), adsr1=a1, adsr2=a2,
                    reverb=bool(ins["reverb"]), bend=int(ins.get("bend", 2)))
        if ins["source"] == "wave":
            key = ("wave", ins["wave"])
            if key not in self.items:
                self.items[key] = wave_pcm(ins["wave"])
            pcm, loop_start, note = self.items[key]
            root = int(round(note))
            return dict(base, pcm=pcm, rate=WAVE_RATE, root=root, cents=int(ins["cents"]) - (note - root) * 100,
                        loop=loop_start is not None, loop_start=((loop_start or 0) + 0.5) / WAVE_RATE)
        path = ins["path"]
        if not path or not os.path.isfile(path):
            raise SongError("instrument %d (%s): the sample file %s is missing" % (program, ins["name"], path or "?"))
        rate = int(ins["rate"]) or 22050
        key = ("file", path, os.path.getmtime(path), rate, bool(ins["normalize"]))
        if key not in self.items:
            self.items[key] = audioconv.load(path, rate, False, ffmpeg, normalize=bool(ins["normalize"]))
        return dict(base, pcm=self.items[key], rate=rate, root=int(ins["root"]), cents=int(ins["cents"]),
                    loop=bool(ins["loop"]), loop_start=float(ins["loop_start"]))


def build_bank(song, cache, ffmpeg=""):
    """VH and VB bytes of the song's instruments (program n = instrument n)."""
    if not song["instruments"]:
        raise SongError("add an instrument")
    tones = [cache.tone(ins, i, ffmpeg) for i, ins in enumerate(song["instruments"][:MAX_INSTRUMENTS])]
    vh, vb = audioconv.build_vab(tones)
    if len(vb) > audioconv.SPU_SAMPLES_MAX:
        raise SongError("the samples take %d KB, the SPU RAM has %d KB for them (lower the sample rates)" % (
            len(vb) // 1024, audioconv.SPU_SAMPLES_MAX // 1024))
    return vh, vb


# ---------------------------------------------------------------------------------------------------- SEQ

def _varlen(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append(0x80 | (v & 0x7F))
        v >>= 7
    return bytes(reversed(out))


def play_path(song, order, loop_pos):
    """The rows played, in time order: [(order position, row)], and the index in it the song loops back to (None:
    no loop)."""
    path, first = [], {}
    for pos, p in enumerate(order):
        first[pos] = len(path)
        path += [(pos, r) for r in range(song["patterns"][p]["rows"])]
    return path, first.get(loop_pos) if loop_pos is not None else None


def compile_seq(song, order=None, loop="song", midi=False):
    """SEQ bytes of patterns played in order (a flattened song: flatten; or one riff) and its timing: dict rows (ms,
    order position, row) at each row's start, length_ms, loop_ms (None: no loop), notes. song: patterns, order (or
    the order given), loop (order position), channels, lpb, bpm, instruments. loop: "song" (the song's loop), "all"
    (from the start), None (plays once). midi: for a MIDI file (seq_to_midi), the
    instruments' bend ranges as RPN 0 (a SEQ can't have them: libsnd takes CC 6 as a loop count)."""
    order = song["order"] if order is None else order
    loop_pos = song["loop"] if loop == "song" else (0 if loop == "all" else None)
    if loop_pos is not None and not 0 <= loop_pos < len(order):
        loop_pos = None
    path, loop_index = play_path(song, order, loop_pos)
    if loop is None:
        loop_index = None
    elif loop == "all" and loop_index is None:
        loop_index = 0
    lpb, channels = song["lpb"], song["channels"]
    tpr = RESOLUTION // lpb
    tempo = int(round(60e6 / song["bpm"]))
    instruments = song["instruments"]
    events = []                     # (tick, priority, sequence, bytes)
    seq = [0]

    def add(tick, prio, data):
        seq[0] += 1
        events.append((tick, prio, seq[0], data))

    default = dict(prog=None, vol=127, pan=64, expr=127, bend=8192)
    state = [dict(default) for _ in range(channels)]
    last_ins = [0] * channels
    sounding = [None] * channels    # Note keyed on (the one its note off releases)
    velocity = [127] * channels
    ranges = [2] * channels         # (midi: the bend range set on each channel, MIDI's default 2)
    sliding = [None] * channels     # (bend before, bends) of a slide (cut by the channel's next note)
    used = set()
    for pos, r in path:
        for c in range(channels):
            cell = song["patterns"][order[pos]]["cells"].get((r, c))
            if cell and any(v is not None for v in cell):
                used.add(c)

    def bend_value(semis, c):
        """Pitch bend (0-16383) of semitones with the channel's instrument, None: beyond its bend range."""
        ins = last_ins[c]
        rng = int(instruments[ins].get("bend", 2)) if ins < len(instruments) else 2
        if abs(semis) > rng:
            return None
        return max(0, min(16383, int(round(8192 + semis * 8192 / rng))))

    def bend(tick, prio, c, value):
        state[c]["bend"] = value
        add(tick, prio, bytes((0xE0 | c, value & 0x7F, value >> 7)))
        return events[-1]

    def cut_slide(tick, c):
        """The bends of a slide from a time on removed (the channel's bend: where the slide got to)."""
        value, bends = sliding[c]
        for e in bends:
            if e[0] >= tick:
                events.remove(e)
            else:
                value = e[3][1] | (e[3][2] << 7)
        state[c]["bend"] = value
        sliding[c] = None

    def channel_state(tick, prio):
        for c in sorted(used):
            s = state[c]
            if s["prog"] is not None:
                add(tick, prio, bytes((0xC0 | c, s["prog"])))
            add(tick, prio, bytes((0xB0 | c, 7, s["vol"])))
            add(tick, prio, bytes((0xB0 | c, 10, s["pan"])))
            add(tick, prio, bytes((0xB0 | c, 11, s["expr"])))
            add(tick, prio, bytes((0xE0 | c, s["bend"] & 0x7F, s["bend"] >> 7)))

    rows, tick, ms, notes, loop_ms = [], 0, 0.0, 0, None
    for index, (pos, r) in enumerate(path):
        pat = song["patterns"][order[pos]]
        if index == loop_index:
            loop_ms = ms
            add(tick, 0, b"\xB0\x63\x14")           # Loop start (NRPN 99 = 20), count 127: for good
            add(tick, 0, b"\xB0\x62\x7F")
            add(tick, 1, b"\xFF\x51" + struct.pack(">I", tempo)[1:])
            channel_state(tick, 2)
        elif tick == 0:
            channel_state(tick, 2)
        for c in range(channels):
            cell = pat["cells"].get((r, c))
            if not cell:
                continue
            note, ins, vol, fx, val = cell
            s = state[c]
            if fx == "T" and val:
                tempo = int(round(60e6 / max(20, min(255, val))))
                add(tick, 1, b"\xFF\x51" + struct.pack(">I", tempo)[1:])
            elif fx in ("P", "V", "E") and val is not None:
                v = min(127, val)
                s[{"P": "pan", "V": "vol", "E": "expr"}[fx]] = v
                add(tick, 5, bytes((0xB0 | c, {"P": 10, "V": 7, "E": 11}[fx], v)))
            elif fx == "B" and val is not None:
                bend(tick, 5, c, min(127, val) << 7)
            playable = note is not None and note != NOTE_OFF
            if note is not None and sliding[c]:
                cut_slide(tick, c)
            if playable and ins is not None:
                last_ins[c] = ins
            # Slide: the note sounding glides to this one (pitch bends from where it is), not played again
            if fx == "G" and playable and sounding[c] is not None:
                target = bend_value(note - sounding[c], c)
                if target is not None:
                    length = max(1, val or 1) * tpr
                    steps = max(1, length // STEP_TICKS)
                    start = s["bend"]
                    sliding[c] = (start, [bend(tick + i * length // steps, 5, c, start + (target - start) * i // steps)
                                          for i in range(1, steps + 1)])
                    continue
            if note is not None:
                if sounding[c] is not None:
                    add(tick, 3, bytes((0x90 | c, sounding[c], 0)))
                    sounding[c] = None
                if note == NOTE_OFF:
                    continue
                if s["bend"] != 8192:           # (left bent by a slide: the new note from the centre)
                    bend(tick, 5, c, 8192)
                if s["prog"] != last_ins[c]:
                    s["prog"] = last_ins[c]
                    add(tick, 4, bytes((0xC0 | c, s["prog"])))
                    rng = int(instruments[s["prog"]].get("bend", 2)) if s["prog"] < len(instruments) else 2
                    if midi and rng != ranges[c]:
                        ranges[c] = rng
                        for cc, v in ((101, 0), (100, 0), (6, rng), (38, 0)):
                            add(tick, 4, bytes((0xB0 | c, cc, v)))
                velocity[c] = 127 if vol is None else max(1, min(127, vol))
                add(tick, 6, bytes((0x90 | c, note, velocity[c])))
                sounding[c] = note
                notes += 1
            # Arpeggio: the note, + x, + y semitones in turn through the row (bends, else the notes played again)
            if fx == "A" and val and sounding[c] is not None:
                base, offsets = sounding[c], (0, val >> 4, val & 15)
                bends = [bend_value(o, c) for o in offsets]
                for i, t in enumerate(range(tick + STEP_TICKS, tick + tpr, STEP_TICKS), 1):
                    if None not in bends:
                        bend(t, 5, c, bends[i % 3])
                    else:
                        add(t, 3, bytes((0x90 | c, sounding[c], 0)))
                        sounding[c] = max(0, min(127, base + offsets[i % 3]))
                        add(t, 6, bytes((0x90 | c, sounding[c], velocity[c])))
                if None not in bends:
                    bend(tick + tpr, 2, c, 8192)        # (back on the note at the next row)
        rows.append((int(ms), pos, r))
        tick += tpr
        ms += tpr * tempo / RESOLUTION / 1000.0
    for c in range(channels):
        if sounding[c] is not None:
            add(tick, 3, bytes((0x90 | c, sounding[c], 0)))
    if loop_index is not None:
        add(tick, 9, b"\xB0\x63\x1E")                   # Loop end (NRPN 99 = 30)
    events.sort(key=lambda e: (e[0], e[1], e[2]))
    tempo0 = int(round(60e6 / song["bpm"]))
    out = bytearray(b"pQES" + struct.pack(">IH", 1, RESOLUTION) + struct.pack(">I", tempo0)[1:] + bytes((4, 2)))
    last, status = 0, 0
    for t, _, _, data in events:
        out += _varlen(t - last)
        last = t
        if data[0] == 0xFF:
            out += data
            status = 0              # (after a meta event: the next status written again)
        elif data[0] == status:
            out += data[1:]         # Running status
        else:
            out += data
            status = data[0]
    out += _varlen(tick - last) + b"\xFF\x2F\x00"
    return bytes(out), dict(rows=rows, length_ms=int(ms), loop_ms=None if loop_ms is None else int(loop_ms),
                            notes=notes)


def row_at(rows, ms):
    """(order position, row) playing at a time (rows of compile_seq)."""
    lo, hi = 0, len(rows) - 1
    if hi < 0:
        return None
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if rows[mid][0] <= ms:
            lo = mid
        else:
            hi = mid - 1
    return rows[lo][1], rows[lo][2]


def row_ms(rows, pos, row):
    """The time a row of the order starts (rows of compile_seq), None if not in it."""
    for ms, p, r in rows:
        if p == pos and r == row:
            return ms
    return None


def clone_pattern(pat, name=None):
    """A copy of a riff (name: a new riff of its own, not the library's any more)."""
    new = copy.deepcopy(pat)
    if name:
        new["name"] = name[:32]
        new.pop("source", None)
    return new


# ---------------------------------------------------------------------------------------------------- MIDI import

# General MIDI program families (8 programs each) -> a built-in waveform and envelope (a start: samples replace them)
GM_FAMILIES = (("Piano", "triangle", 3), ("Chromatic", "organ", 3), ("Organ", "organ", 0), ("Guitar", "pulse", 3),
               ("Bass", "triangle", 0), ("Strings", "strings", 0), ("Ensemble", "strings", 0), ("Brass", "saw", 0),
               ("Reed", "square", 0), ("Pipe", "sine", 0), ("Synth lead", "square", 0), ("Synth pad", "strings", 2),
               ("Synth effect", "pulse", 0), ("Ethnic", "pulse", 3), ("Percussive", "tom", 4),
               ("Sound effect", "noise", 0))
MIDI_DRUMS = 9                  # MIDI channel 10: drums
ONE_SHOT = 4                    # The envelope of drums (audioconv.ENVELOPES)


def gm_drum(note):
    """The built-in drum (DRUM_WAVES) of a General MIDI drum note (cymbals: the open hat, other percussion: the
    hi-hat)."""
    if note <= 36:
        return "kick"
    if note in (37, 38, 40):
        return "snare"
    if note == 39:
        return "clap"
    if note in (41, 43, 45, 47, 48, 50):
        return "tom"
    if note in (46, 49, 51, 52, 53, 55, 57, 59):
        return "open hat"
    return "hat"
IMPORT_LPB = (4, 8, 3, 6, 12, 16)


def import_midi(path, rows=64):
    """A song from a standard MIDI file: its notes on the rows (rows per beat chosen to fit them), each MIDI channel
    on as many tracker channels as it plays notes at once (up to 16 in all), an instrument per MIDI program (a
    built-in waveform of its General MIDI family), the tempo changes (T), volume and pan (V, P). Returns the song
    and a dict: notes, dropped (notes: no channel left), lost_fx (tempo, volume, pan: no effect column left), lpb,
    channels, patterns."""
    division, events = audioconv.read_midi(path)
    # Notes: (start tick, end tick, MIDI channel, note, velocity, program)
    notes, playing, program = [], {}, [0] * 16
    controls, tempos = [], []           # (tick, MIDI channel, "V"/"P", value), (tick, us per quarter note)
    last, markers = 0, {}
    for tick, _, kind, data in events:
        last = max(last, tick)
        if kind == "marker" and data in ("loopstart", "loopend"):
            markers.setdefault(data, tick)
        if kind == "tempo":
            tempos.append((tick, data))
            continue
        if kind != "ch":
            continue
        st, ch = data[0] & 0xF0, data[0] & 15
        if st == 0xC0:
            program[ch] = data[1]
        elif st == 0x90 and data[2]:
            key = (ch, data[1])
            if key in playing:          # (the same note again: the first one ends)
                s = playing.pop(key)
                notes.append((s[0], tick) + s[1:])
            playing[key] = (tick, ch, data[1], data[2], program[ch])
        elif st == 0x80 or st == 0x90:
            s = playing.pop((ch, data[1]), None)
            if s:
                notes.append((s[0], tick) + s[1:])
        elif st == 0xB0 and data[1] in (7, 10):
            controls.append((tick, ch, "V" if data[1] == 7 else "P", data[2]))
    for s in playing.values():
        notes.append((s[0], last) + s[1:])
    if not notes:
        raise SongError("%s: no notes" % os.path.basename(path))
    notes.sort(key=lambda n: (n[0], n[2], n[3]))

    # Rows per beat: the fewest with the notes' starts on rows (within 1/8 row)
    lpb = IMPORT_LPB[-1]
    for choice in IMPORT_LPB:
        tpr = division / choice
        near = sum(1 for n in notes if abs(n[0] / tpr - round(n[0] / tpr)) < 0.125)
        if near >= 0.95 * len(notes):
            lpb = choice
            break
    tpr = division / lpb

    def row_of(tick):
        return int(round(tick / tpr))

    song = new_song()
    song["title"] = os.path.splitext(os.path.basename(path))[0][:40]
    song["lpb"] = lpb
    first_tempo = next((t for tk, t in tempos if tk == 0), tempos[0][1] if tempos else 500000)
    song["bpm"] = max(20, min(300, int(round(60e6 / first_tempo))))
    song["instruments"] = []
    ins_of = {}

    def instrument(ch, prog, note):
        key = gm_drum(note) if ch == MIDI_DRUMS else prog
        if key not in ins_of and len(song["instruments"]) < MAX_INSTRUMENTS:
            if ch == MIDI_DRUMS:
                ins = new_instrument(key.capitalize(), key, envelope=ONE_SHOT)
            else:
                name, wave, env = GM_FAMILIES[prog // 8]
                ins = new_instrument("%s %d" % (name, prog), wave, envelope=env)
            ins_of[key] = len(song["instruments"])
            song["instruments"].append(ins)
        return ins_of.get(key, 0)

    total = max(row_of(n[1]) for n in notes) + 1
    loop_row = row_of(markers["loopstart"]) if "loopstart" in markers else 0
    if "loopend" in markers and loop_row < row_of(markers["loopend"]) < total:
        total = row_of(markers["loopend"])     # (what comes after the loop is never heard)
    cells = {}                          # (song row, tracker channel): cell
    lanes = {}                          # MIDI channel: [tracker channel]
    free_at = [0] * MAX_CHANNELS        # Song row each tracker channel's last note ends at
    used, dropped, lost_fx = 0, 0, 0
    for start, end, ch, note, vel, prog in notes:
        r, e = row_of(start), max(row_of(start) + 1, row_of(end))
        lane = None
        for c in lanes.get(ch, []):
            if free_at[c] <= r:         # (its last note ended: free, or a note off there replaced)
                lane = c
                break
        if lane is None:
            if used >= MAX_CHANNELS:
                dropped += 1
                continue
            lane = used
            used += 1
            lanes.setdefault(ch, []).append(lane)
        cells[(r, lane)] = [note, instrument(ch, prog, note), None if vel >= 127 else vel, None, None]
        if (e, lane) not in cells:
            cells[(e, lane)] = [NOTE_OFF, None, None, None, None]
        free_at[lane] = e

    def effect(r, chans, fx, val):
        for c in chans:
            cell = cells.setdefault((r, c), [None] * 5)
            if cell[3] is None:
                cell[3], cell[4] = fx, val
                return True
        return False

    for tick, us in tempos:
        if tick and not effect(row_of(tick), range(used), "T", max(20, min(255, int(round(60e6 / us))))):
            lost_fx += 1
    now = {}                            # (MIDI channel, effect): its value (the same again: no effect)
    for tick, ch, fx, val in controls:
        if ch not in lanes or now.get((ch, fx), 127 if fx == "V" else 64) == val:
            continue
        now[(ch, fx)] = val
        if not effect(row_of(tick), lanes[ch], fx, val):
            lost_fx += 1

    # Patterns of `rows` rows, played in order (one starts at the loop start: the song loops from it)
    loop_row = min(loop_row, total - 1)
    starts = list(range(0, loop_row, rows)) + list(range(loop_row, total, rows))
    song["channels"] = max(1, used)
    song["patterns"] = []
    for i, first in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else total
        pat = new_pattern(end - first, "Part %d" % (i + 1))
        for (r, c), cell in cells.items():
            if first <= r < end:
                pat["cells"][(r - first, c)] = cell
        song["patterns"].append(pat)
    song_from_order(song, list(range(len(song["patterns"]))), starts.index(loop_row))
    return check(song), dict(notes=len(notes) - dropped, dropped=dropped, lost_fx=lost_fx, lpb=lpb, channels=used,
                             patterns=len(song["patterns"]))
