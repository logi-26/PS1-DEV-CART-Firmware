"""
PS1 DEV CART GUI: the music tracker's starter riffs (the Music Tracker page's library): drum beats, bass lines, chords,
arpeggios and melodies to put on the tracks as they are, or to start from. The pitched ones follow the same four bars
(C, A minor, F, G), so any of them play together. Each one brings its instruments (built-in waveforms and drums:
INSTRUMENTS), added to the song the first time one of its riffs is used.

Written at 4 rows per beat (16 rows a bar), put on the song's rows per beat (build).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import tracker

# Instruments of the riffs: name, waveform, envelope (audioconv.ENVELOPES), volume, reverb
INSTRUMENTS = {
    "kick": ("Kick", "kick", tracker.ONE_SHOT, 127, False),
    "snare": ("Snare", "snare", tracker.ONE_SHOT, 105, True),
    "clap": ("Clap", "clap", tracker.ONE_SHOT, 100, True),
    "hat": ("Hi-hat", "hat", tracker.ONE_SHOT, 70, False),
    "ohat": ("Open hat", "open hat", tracker.ONE_SHOT, 60, False),
    "tom": ("Tom", "tom", tracker.ONE_SHOT, 110, True),
    "bass": ("Bass", "triangle", 0, 127, False),
    "synbass": ("Synth bass", "saw", 3, 90, False),
    "pad": ("Strings", "strings", 2, 70, True),
    "organ": ("Organ", "organ", 0, 70, True),
    "stab": ("Stab", "pulse", 3, 75, True),
    "pluck": ("Pluck", "organ", 3, 100, True),
    "lead": ("Lead", "square", 0, 80, True),
    "soft": ("Soft lead", "triangle", 1, 110, True),
}
# Drum lines: the drum's instrument, note (where it sounds as made) and channel (the hi-hats on one: one cuts the other)
DRUMS = {"kick": (36, 0), "snare": (38, 1), "clap": (39, 1), "hat": (42, 2), "ohat": (46, 2), "tom": (45, 3)}
TOMS = {"h": 50, "m": 45, "l": 41}      # A tom line's high, mid, low notes
ACCENTS = {"X": None, "x": 100, "o": 60}    # A hit's volume: X loud (127), x normal, o soft (a ghost note)
CATEGORIES = ("Drums", "Bass", "Chords", "Arpeggio", "Melody")
# The four bars: bass root, chord (voiced close together: each chord near the one before)
CHORDS = ((48, (60, 64, 67)), (45, (60, 64, 69)), (41, (60, 65, 69)), (43, (59, 62, 67)))
MINOR = {45}                            # Roots of the minor chords


def _drums(**lines):
    """Notes of drum lines (16 rows a bar): X, x, o a hit (ACCENTS), toms: h, m, l (TOMS); each hit until the next of
    its channel."""
    notes = []
    for name, line in lines.items():
        key = "tom" if name == "toms" else name
        note, ch = DRUMS[key]
        for r, c in enumerate(line):
            if name == "toms" and c in TOMS:
                notes.append((r, ch, TOMS[c], None, key, 110))
            elif c in ACCENTS:
                notes.append((r, ch, note, None, key, ACCENTS[c]))
    return notes


def _per_bar(make):
    """Notes of the four bars: make(bar's first row, bass root, chord, next bar's root) gives each bar's."""
    notes = []
    for bar, (root, triad) in enumerate(CHORDS):
        notes += make(bar * 16, root, triad, CHORDS[(bar + 1) % len(CHORDS)][0])
    return notes


def _chords(rows, key, accent=True):
    """The chord of each bar on rows (row, length), the first of the bar louder (accent)."""
    return _per_bar(lambda s, root, triad, nxt: [
        (s + r, i, n, length, key, None if accent and j == 0 else 95 if accent else None)
        for j, (r, length) in enumerate(rows) for i, n in enumerate(triad)])


def _melody(items, key="lead"):
    return [(r, 0, n, length, key) for r, n, length in items]


def _walk(s, root, triad, nxt):
    third = root + (3 if root in MINOR else 4)
    approach = nxt + (1 if nxt < root + 7 else -1)
    return [(s, 0, root, 4, "bass"), (s + 4, 0, third, 4, "bass"), (s + 8, 0, root + 7, 4, "bass"),
            (s + 12, 0, approach, 4, "bass")]


# Riffs: id: (category, name, rows, notes: (row, channel, note, length (None: until the next), instrument[, volume]))
RIFFS = {
    # Drums
    "beat": ("Drums", "Basic beat", 16, _drums(kick="X.......X.......", snare="....X.......X...",
                                               hat="X.x.X.x.X.x.X.x.")),
    "rock": ("Drums", "Rock beat", 16, _drums(kick="X.......X.X.....", snare="....X.......X...",
                                              ohat="x.o.x.o.x.o.x.o.")),
    "four": ("Drums", "Four on the floor", 16, _drums(kick="X...X...X...X...", clap="....X.......X...",
                                                      hat="x...x...x...x...", ohat="..X...X...X...X.")),
    "disco": ("Drums", "Disco", 16, _drums(kick="X...X...X...X...", snare="....X..o....X..o",
                                           hat="XoxoXoxoXoxoXoxo")),
    "break": ("Drums", "Breakbeat", 16, _drums(kick="X.....x...x.....", snare="....X..o.o..X..o",
                                               hat="X.x.X.x.X.x.X.x.")),
    "half": ("Drums", "Half-time", 16, _drums(kick="X.........x.....", snare="........X.......",
                                              hat="X.x.X.x.X.x.X.xo")),
    "busy": ("Drums", "Sixteenth hats", 16, _drums(kick="X.......X.x.....", snare="....X.......X...",
                                                   hat="Xoxoxoxoxoxoxoxo")),
    "shuffle": ("Drums", "Shuffle", 16, _drums(kick="X.....x.X.......", snare="....X..o....X...",
                                               hat="X..xX..xX..xX..x")),
    "onedrop": ("Drums", "One drop", 16, _drums(kick="........X.......", snare="........X.......",
                                                hat="..x...x...x...x.")),
    "rolling": ("Drums", "Rolling hats", 16, _drums(kick="X......X..X.....", clap="........X.......",
                                                    hat="x.x.x.x.x.xoxoxo")),
    "fill": ("Drums", "Snare fill", 16, _drums(kick="X...X...X.......", snare="....X..ooxoxXxXX")),
    "tomfill": ("Drums", "Tom fill", 16, _drums(kick="X...........X...", snare="..o.X...........",
                                                toms="......hhmmll....", ohat="............X...")),
    # Bass
    "bass8": ("Bass", "Root eighths", 64, _per_bar(lambda s, root, t, nxt: [
        (s + r, 0, root, 1, "bass", None if r % 4 == 0 else 100) for r in range(0, 16, 2)])),
    "octave": ("Bass", "Octave bounce", 64, _per_bar(lambda s, root, t, nxt: [
        (s + r, 0, root + (12 if r % 4 else 0), 2, "bass") for r in range(0, 16, 2)])),
    "long": ("Bass", "Long notes", 64, _per_bar(lambda s, root, t, nxt: [(s, 0, root, 16, "bass")])),
    "synco": ("Bass", "Syncopated", 64, _per_bar(lambda s, root, t, nxt: [
        (s, 0, root, 3, "bass"), (s + 3, 0, root, 3, "bass"), (s + 6, 0, root + 12, 2, "bass"),
        (s + 10, 0, root, 2, "bass"), (s + 12, 0, root + 7, 4, "bass")])),
    "walk": ("Bass", "Walking", 64, _per_bar(_walk)),
    "pump": ("Bass", "Off-beat pump", 64, _per_bar(lambda s, root, t, nxt: [
        (s + r, 0, root, 2, "synbass") for r in (2, 6, 10, 14)])),
    "synth": ("Bass", "Synth sixteenths", 64, _per_bar(lambda s, root, t, nxt: [
        (s + r, 0, root + (12 if r % 4 == 2 else 0), 1, "synbass", None if r % 4 == 0 else 85) for r in range(16)])),
    "funk": ("Bass", "Funky", 64, _per_bar(lambda s, root, t, nxt: [
        (s, 0, root, 2, "synbass"), (s + 3, 0, root + 12, 1, "synbass", 90), (s + 6, 0, root, 1, "synbass", 80),
        (s + 7, 0, root + 10, 2, "synbass"), (s + 10, 0, root + 7, 2, "synbass"),
        (s + 12, 0, root + 12, 1, "synbass", 90), (s + 14, 0, root + 10, 2, "synbass", 80)])),
    # Chords
    "pad": ("Chords", "String chords", 64, _chords(((0, 16),), "pad", accent=False)),
    "organ": ("Chords", "Organ rhythm", 64, _chords(((0, 3), (3, 3), (6, 4), (10, 2), (12, 4)), "organ")),
    "half2": ("Chords", "Two a bar", 64, _chords(((0, 8), (8, 8)), "pluck")),
    "stabs": ("Chords", "Chord stabs", 64, _chords(((0, 2), (6, 2), (8, 2), (14, 2)), "stab")),
    "offbeat": ("Chords", "Off-beat chords", 64, _chords(((2, 2), (6, 2), (10, 2), (14, 2)), "stab")),
    "eighths": ("Chords", "Eighth chords", 64, _chords([(r, 1) for r in range(0, 16, 2)], "stab")),
    # Arpeggios
    "arpup": ("Arpeggio", "Arpeggio up", 64, _per_bar(lambda s, root, triad, nxt: [
        (s + r, 0, (triad + (triad[0] + 12,))[r % 4], 1, "pluck", None if r % 4 == 0 else 90) for r in range(16)])),
    "arpdown": ("Arpeggio", "Arpeggio down", 64, _per_bar(lambda s, root, triad, nxt: [
        (s + r, 0, (triad[0] + 12, triad[2], triad[1], triad[0])[r % 4], 1, "pluck", None if r % 4 == 0 else 90)
        for r in range(16)])),
    "arpupdown": ("Arpeggio", "Arpeggio up and down", 64, _per_bar(lambda s, root, triad, nxt: [
        (s + r, 0, (triad[0], triad[1], triad[2], triad[0] + 12, triad[2], triad[1])[r % 6], 1, "pluck")
        for r in range(16)])),
    "climb": ("Arpeggio", "Two-octave climb", 64, _per_bar(lambda s, root, triad, nxt: [
        (s + r, 0, (triad + tuple(n + 12 for n in triad) + (triad[1] + 12, triad[0] + 12))[r % 8], 1, "pluck",
         None if r % 8 == 0 else 90) for r in range(16)])),
    "broken": ("Arpeggio", "Broken eighths", 64, _per_bar(lambda s, root, triad, nxt: [
        (s + r, 0, (triad[0] - 12, triad[2] - 12, triad[0], triad[2] - 12)[r // 2 % 4], 2, "pluck")
        for r in range(0, 16, 2)])),
    # Melodies
    "tune": ("Melody", "Simple tune", 64, _melody([
        (0, 76, 4), (4, 79, 2), (6, 76, 2), (8, 74, 4), (12, 72, 4),
        (16, 72, 4), (20, 69, 2), (22, 72, 2), (24, 76, 6), (30, 74, 2),
        (32, 72, 4), (36, 69, 4), (40, 65, 2), (42, 69, 2), (44, 72, 4),
        (48, 71, 4), (52, 74, 4), (56, 79, 6), (62, 77, 2)])),
    "answer": ("Melody", "Call and answer", 64, _melody([
        (0, 67, 2), (2, 72, 2), (4, 76, 4), (8, 74, 2), (10, 72, 2), (12, 74, 4),
        (16, 72, 4), (20, 69, 4), (24, 64, 8),
        (32, 65, 2), (34, 69, 2), (36, 72, 4), (40, 74, 2), (42, 72, 2), (44, 69, 4),
        (48, 67, 4), (52, 71, 4), (56, 74, 8)])),
    "hook": ("Melody", "Hook", 64, _melody([
        (0, 76, 2), (2, 76, 2), (4, 79, 4), (8, 76, 2), (10, 74, 2), (12, 72, 4),
        (16, 72, 2), (18, 72, 2), (20, 76, 4), (24, 74, 2), (26, 72, 2), (28, 69, 4),
        (32, 69, 2), (34, 72, 2), (36, 77, 4), (40, 76, 2), (42, 74, 2), (44, 72, 4),
        (48, 71, 2), (50, 74, 2), (52, 79, 6), (58, 77, 2), (60, 74, 4)])),
    "slow": ("Melody", "Slow and high", 64, _melody([
        (0, 79, 8), (8, 84, 8), (16, 81, 12), (28, 79, 4), (32, 77, 8), (40, 81, 8), (48, 79, 12), (60, 74, 4)],
        "soft")),
    "bounce": ("Melody", "Bouncy", 64, _per_bar(lambda s, root, t, nxt: [
        (s + r, 0, n, length, "lead") for r, n, length in (
            (0, t[0] + 12, 2), (2, t[1] + 12, 1), (3, t[2] + 12, 2), (6, t[1] + 12, 2), (8, t[0] + 12, 2),
            (10, t[2], 1), (11, t[0] + 12, 2), (14, t[1] + 12, 2))])),
}


def by_category():
    """[(category, [(riff id, name)])]"""
    return [(cat, [(rid, r[1]) for rid, r in RIFFS.items() if r[0] == cat]) for cat in CATEGORIES]


def instrument_for(song, key):
    """The song's instrument of a riff instrument (added the first time, found again by its name and waveform)."""
    name, wave, envelope, vol, reverb = INSTRUMENTS[key]
    for i, ins in enumerate(song["instruments"]):
        if ins["name"] == name and ins["source"] == "wave" and ins["wave"] == wave:
            return i
    if len(song["instruments"]) >= tracker.MAX_INSTRUMENTS:
        raise tracker.SongError("the song has %d instruments, the most a VAB has" % tracker.MAX_INSTRUMENTS)
    ins = tracker.new_instrument(name, wave, envelope)
    ins["vol"] = vol
    ins["reverb"] = reverb
    song["instruments"].append(ins)
    return len(song["instruments"]) - 1


def build(rid, song):
    """A riff of the library as a pattern of the song (its rows per beat), its instruments added to the song."""
    cat, name, rows, notes = RIFFS[rid]
    lpb = song["lpb"]

    def scale(r):
        return int(round(r * lpb / 4.0))

    pat = tracker.new_pattern(max(1, scale(rows)), name)
    pat["source"] = rid
    cells = pat["cells"]
    for item in sorted(notes, key=lambda n: n[0]):
        r, ch, note, length, key = item[:5]
        vol = item[5] if len(item) > 5 else None
        row = scale(r)
        if row >= pat["rows"]:
            continue
        cells[(row, ch)] = [note, instrument_for(song, key), vol, None, None]
        if length:
            end = max(row + 1, scale(r + length))
            if end < pat["rows"] and (end, ch) not in cells:
                cells[(end, ch)] = [tracker.NOTE_OFF, None, None, None, None]
    return pat
