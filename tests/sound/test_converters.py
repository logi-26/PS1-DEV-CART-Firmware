"""
The Sound Tools' converters (gui/audioconv.py, gui/tracker.py), their results played by the console's player (seqhost)
where it applies:
- MIDI -> SEQ: the notes at their times (a MIDI reader of these tests' own), tempo changes, several tracks, the loop
  (markers, CC 111, whole, from a tick) played again
- SEQ -> MIDI -> SEQ: each test SEQ played the same after the round trip (loops too)
- WAV -> VAG -> WAV: the sound kept (signal to noise), the loop point, the header
- VAB built from sounds: each tone at its sample's pitch at its root note (rate, fine tune), key ranges, bend range
- music tracker songs: compiled, played as the reference reads them, the song's loop
- XA written and read back; the samples of the test banks and VAGs exported

    python -m unittest tests/sound/test_converters.py -v      (PS1_SOUND_CORPUS: the test files, see test_seq_player)

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import glob
import os
import struct
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import seqref                                                           # noqa: E402
from soundhost import NTSC_US, TMP, compare_voices, corpus, corpus_dirs, run, run_bytes, vabmod   # noqa: E402

import audioconv                                                        # noqa: E402  (gui, added by soundhost)
import riffs                                                            # noqa: E402
import tracker                                                          # noqa: E402


# ---------------------------------------------------------------------------------------------------- MIDI files

def vl(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.insert(0, (v & 0x7F) | 0x80)
        v >>= 7
    return bytes(out)


def midi_file(tracks, division=480, fmt=1):
    """A MIDI file of tracks: [[(tick, bytes)]] (absolute ticks), FF 2F 00 added."""
    out = b"MThd" + struct.pack(">IHHH", 6, fmt, len(tracks), division)
    for tr in tracks:
        data, last = b"", 0
        for tick, e in sorted(tr, key=lambda x: x[0]):
            data += vl(tick - last) + e
            last = tick
        data += b"\x00\xFF\x2F\x00"
        out += b"MTrk" + struct.pack(">I", len(data)) + data
    return out


def marker(text):
    return b"\xFF\x06" + vl(len(text)) + text.encode()


def tempo(us):
    return b"\xFF\x51\x03" + struct.pack(">I", us)[1:]


def read_midi_notes(data):
    """Note ons of a MIDI file: [(us, ch, note, vel)], the loop markers (us) and the end (us). Running status, every
    track merged, the tempo map applied (this test's own reader)."""
    assert data[:4] == b"MThd"
    ntracks, division = struct.unpack(">HH", data[10:14])
    p, evs = 14, []
    for _ in range(ntracks):
        assert data[p:p + 4] == b"MTrk"
        size = struct.unpack(">I", data[p + 4:p + 8])[0]
        q, end, tick, status = p + 8, p + 8 + size, 0, 0
        while q < end:
            v = 0
            while True:
                b = data[q]
                q += 1
                v = (v << 7) | (b & 0x7F)
                if not b & 0x80:
                    break
            tick += v
            s = data[q]
            if s & 0x80:
                q += 1
            else:
                s = status
            if s == 0xFF:
                kind = data[q]
                q += 1
                n = 0
                while True:
                    b = data[q]
                    q += 1
                    n = (n << 7) | (b & 0x7F)
                    if not b & 0x80:
                        break
                evs.append((tick, "meta", (kind, data[q:q + n])))
                q += n
                continue
            if s in (0xF0, 0xF7):
                n = data[q]
                q += 1 + n
                continue
            status = s
            size2 = 1 if s & 0xF0 in (0xC0, 0xD0) else 2
            evs.append((tick, "ch", bytes((s,)) + data[q:q + size2]))
            q += size2
        p = end
    evs.sort(key=lambda e: e[0])
    tempo_us, t, last, notes, marks = 500000, 0.0, 0, [], {}
    for tick, kind, d in evs:
        t += (tick - last) * tempo_us / division
        last = tick
        if kind == "meta":
            if d[0] == 0x51:
                tempo_us = int.from_bytes(d[1], "big")
            elif d[0] == 0x06:
                marks.setdefault(d[1].decode(errors="replace").lower(), t)
        elif d[0] & 0xF0 == 0x90 and d[2]:
            notes.append((t, d[0] & 15, d[1], d[2]))
    return notes, marks, t


def match(want, got, frame_us=NTSC_US, tick_us=1500, limit=3):
    """Notes in any order at the same time: each expected note (us, ch, note, vel) found among what the console read
    (a frame early at most, a tick late). [text]"""
    pool = {}
    for g in got:
        pool.setdefault((g[2], g[3], g[4]), []).append(g[0])
    problems = []
    for w in want:
        times = pool.get((w[1], w[2], w[3]), [])
        hit = next((x for x in times if w[0] - frame_us - tick_us - 1500 <= x <= w[0] + tick_us + 1500), None)
        if hit is None:
            problems.append("ch %d note %d vel %d at %.3f s not played (or not then)" % (w[1], w[2], w[3], w[0] / 1e6))
            if len(problems) >= limit:
                break
        else:
            times.remove(hit)
    extra = sum(len(v) for v in pool.values())
    if not problems and extra:
        problems.append("%d notes more than expected" % extra)
    return problems


def tmpfile(data, suffix):
    os.makedirs(TMP, exist_ok=True)
    fd, path = tempfile.mkstemp(suffix=suffix, dir=TMP)
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    return path


# ---------------------------------------------------------------------------------------------------- MIDI -> SEQ

class MidiToSeq(unittest.TestCase):

    def convert_and_play(self, mid, horizon_s, **kw):
        path = tmpfile(mid, ".mid")
        try:
            seq, info = audioconv.midi_to_seq(path, **kw)
        finally:
            os.remove(path)
        h = run_bytes(seq, play_ms=int(horizon_s * 1000), repeat=0)
        self.assertIsNone(h.problem(), h.raw[-300:])
        return seq, info, h

    def song(self, loop_marks=False):
        """Three tracks: tempo map (with a change), melody with running status, chords on ch 2, drums on ch 10."""
        t0 = [(0, tempo(500000)), (1920, tempo(400000)), (3840, tempo(600000))]
        if loop_marks:
            t0 += [(960, marker("loopStart")), (4800, marker("loopEnd"))]
        mel = [(0, b"\xC0\x05")]
        for i in range(24):
            mel += [(i * 240, bytes((0x90, 60 + i % 12, 90 + i % 30))), (i * 240 + 200, bytes((60 + i % 12, 0)))]
        chords = [(0, b"\xC1\x10"), (0, b"\xB1\x07\x64")]
        for bar in range(4):
            for n in (48, 52, 55):
                chords += [(bar * 1920, bytes((0x91, n + bar, 80))), (bar * 1920 + 1800, bytes((0x81, n + bar, 0)))]
        drums = []
        for i in range(32):
            drums += [(i * 240, bytes((0x99, 36 if i % 4 == 0 else 42, 110))), (i * 240 + 60, bytes((0x89, 36 if i % 4 == 0 else 42, 0)))]
        return midi_file([t0, mel, chords, drums])

    def test_notes_and_tempo(self):
        mid = self.song()
        want, _, end = read_midi_notes(mid)
        _, info, h = self.convert_and_play(mid, end / 1e6 + 2, loop="none")
        self.assertEqual([], match(want, h.notes))
        self.assertIsNotNone(h.stop, "played once (no loop)")

    def test_test_audio_midis(self):
        dirs = [os.path.join(d, "to-ps1", "midi") for d in corpus_dirs()]
        files = [f for d in dirs for f in glob.glob(os.path.join(d, "*.mid"))]
        if not files:
            self.skipTest("no test-audio MIDI files in PS1_SOUND_CORPUS")
        for f in files:
            with self.subTest(file=os.path.basename(f)):
                with open(f, "rb") as fh:
                    mid = fh.read()
                want, marks, end = read_midi_notes(mid)
                stop = marks.get("loopend", end)
                _, _, h = self.convert_and_play(mid, stop / 1e6 + 1, loop="none")
                self.assertEqual([], match([w for w in want if w[0] < stop - 2e4],
                                           [g for g in h.notes if g[0] < stop - 2e4]))

    def check_loop(self, mid, loop_us, end_us, **kw):
        want, _, _ = read_midi_notes(mid)
        horizon = end_us + 2.5 * (end_us - loop_us)
        seq, info, h = self.convert_and_play(mid, horizon / 1e6, **kw)
        expected = [w for w in want if w[0] < end_us]
        body = [w for w in want if loop_us <= w[0] < end_us]
        k = 1
        while end_us + (k - 1) * (end_us - loop_us) < horizon - 0.2e6:
            expected += [(w[0] + k * (end_us - loop_us),) + w[1:] for w in body]
            k += 1
        expected = [e for e in expected if e[0] < horizon - 0.2e6]
        got = [g for g in h.notes if g[0] < horizon - 0.2e6]
        self.assertEqual([], match(expected, got, tick_us=3000))

    def test_loop_markers(self):
        mid = self.song(loop_marks=True)
        _, marks, _ = read_midi_notes(mid)
        self.check_loop(mid, marks["loopstart"], marks["loopend"], loop="markers")

    def test_loop_whole_and_tick(self):
        mid = self.song()
        _, _, end = read_midi_notes(mid)
        self.check_loop(mid, 0, end, loop="whole")
        # From tick 1920 (the tempo is 500000 up to it: 2 s)
        self.check_loop(mid, 2e6, end, loop="tick", loop_tick=1920)

    def test_cc111_loop(self):
        tracks = [[(0, tempo(500000))], [(0, b"\xC0\x00")]]
        for i in range(8):
            tracks[1] += [(i * 480, bytes((0x90, 60 + i, 100))), (i * 480 + 400, bytes((0x80, 60 + i, 0)))]
        tracks[1].append((960, b"\xB0\x6F\x00"))          # RPG Maker: CC 111 at the loop start
        mid = midi_file(tracks)
        _, _, end = read_midi_notes(mid)
        self.check_loop(mid, 1e6, end, loop="markers")


# ---------------------------------------------------------------------------------------------------- SEQ -> MIDI -> SEQ

class SeqRoundTrip(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.files = corpus()
        if not cls.files:
            raise unittest.SkipTest("no PS1_SOUND_CORPUS")

    def test_round_trip(self):
        for path, _, _ in self.files:
            with open(path, "rb") as f:
                tracks = seqref.tracks(f.read())
            out = os.path.join(TMP, "rt_" + os.path.basename(path) + ".mid")
            os.makedirs(TMP, exist_ok=True)
            info = audioconv.seq_to_midi(path, out)
            for i, mid_path in enumerate(info["files"]):
                res, tmp, body = tracks[i]
                evs = seqref.events(body)
                length = seqref.length_us(res, tmp, evs)
                with self.subTest(file=os.path.basename(path), track=i):
                    seq, _ = audioconv.midi_to_seq(mid_path, loop="markers")
                    horizon = min(150e6, length * 1.6 + 3e6)
                    # (a tempo change inside an endless loop the file doesn't set back at its start: on the console
                    # the repeats at the new tempo; its MIDI is one pass, converted back the loop set as it began)
                    first_end = _tempo_in_endless_loop(res, tmp, evs)
                    if first_end:
                        horizon = first_end
                    # Both SEQs played by the console's player: the same notes (the loops followed in both)
                    a = run(path, track=i, play_ms=int(horizon / 1000), repeat=0)
                    b = run_bytes(seq, play_ms=int(horizon / 1000), repeat=0)
                    self.assertIsNone(a.problem())
                    self.assertIsNone(b.problem())
                    tick = seqref.tick_us(res, tmp, evs)
                    self.assertEqual([], match([(n[0],) + n[2:5] for n in a.notes if n[0] < horizon - 1e6],
                                               [n for n in b.notes if n[0] < horizon - 1e6], tick_us=tick))
                os.remove(mid_path)


def _tempo_in_endless_loop(res, tempo, evs):
    """The time of the end of an endless loop with a tempo change inside it (us), else None."""
    res, t, inside, count, tempo_in = res or 480, 0.0, False, -1, False
    for delta, kind, args in evs:
        t += delta * tempo / res
        if kind == "tempo":
            tempo = args[0]
            tempo_in = tempo_in or inside
        elif kind == "loopstart":
            inside, count, tempo_in = True, -1, False
        elif kind == "loopcount":
            count = args[0]
        elif kind == "loopend" and inside:
            if count == 127 and tempo_in:
                return t
            inside = False
    return None


# ---------------------------------------------------------------------------------------------------- VAG

def snr_db(ref, test):
    n = min(len(ref), len(test))
    ref, test = ref[:n].astype(np.float64), test[:n].astype(np.float64)
    noise = np.sum((ref - test) ** 2)
    return 10 * np.log10(np.sum(ref ** 2) / max(noise, 1e-9))


class Vag(unittest.TestCase):

    def sounds(self):
        rate = 22050
        t = np.arange(rate) / rate
        out = {"sine 440": np.sin(2 * np.pi * 440 * t) * 20000,
               "chord": (np.sin(2 * np.pi * 261.6 * t) + np.sin(2 * np.pi * 329.6 * t) + np.sin(2 * np.pi * 392 * t))
               * 8000,
               "decay": np.sin(2 * np.pi * 110 * t) * np.exp(-t * 3) * 30000}
        for d in corpus_dirs():
            for f in glob.glob(os.path.join(d, "to-ps1", "audio", "*.wav")) + glob.glob(os.path.join(d, "to-ps1",
                                                                                                  "drum-kit", "*.wav")):
                out[os.path.basename(f)] = audioconv.load(f, rate).astype(np.float64)
        return rate, {k: np.clip(v, -32768, 32767).astype(np.int16) for k, v in out.items()}

    def test_encode_decode(self):
        rate, sounds = self.sounds()
        for name, pcm in sounds.items():
            with self.subTest(sound=name):
                data = audioconv.to_vag(pcm, rate, name="t")
                path = tmpfile(data, ".vag")
                try:
                    adpcm, r, nm = audioconv.read_vag(path)
                finally:
                    os.remove(path)
                self.assertEqual(rate, r)
                self.assertEqual(struct.unpack(">I", data[12:16])[0], len(adpcm), "header size")
                out, loop = audioconv.decode_adpcm(adpcm)
                self.assertIsNone(loop)
                self.assertFalse(out[:28].any(), "a VAG starts with a silent block")
                out = out[28:]
                self.assertGreaterEqual(len(out), len(pcm))
                self.assertGreater(snr_db(pcm, out), 18, "signal to noise (dB)")

    def test_loop_point(self):
        rate, sounds = self.sounds()
        pcm = sounds["sine 440"]
        for start in (0.0, 0.25, 0.5013):
            with self.subTest(loop_start=start):
                data = audioconv.to_vag(pcm, rate, loop=True, loop_start=start)
                out, loop = audioconv.decode_adpcm(data[48:])
                self.assertIsNotNone(loop)
                self.assertLessEqual(abs(loop[0] - 28 - start * rate), 28, "loop start (samples, a block is 28; after"
                                     " the silent block)")
                self.assertEqual(0, loop[0] % 28)

    def test_test_files_decode(self):
        files = [f for d in corpus_dirs() for f in glob.glob(os.path.join(d, "**", "*.vag"), recursive=True)]
        files += glob.glob(os.path.join(os.path.dirname(corpus_dirs()[0]) if corpus_dirs() else "", "extracted data",
                                        "*.vag"))
        if not files:
            self.skipTest("no VAG files")
        for f in files[:60]:
            with self.subTest(file=os.path.basename(f)):
                out = os.path.join(TMP, "v.wav")
                secs, r, _ = audioconv.vag_to_audio(f, out)
                self.assertGreater(secs, 0)
                self.assertGreater(r, 0)
                os.remove(out)


# ---------------------------------------------------------------------------------------------------- VAB

class VabBuild(unittest.TestCase):

    def test_pitch_at_root(self):
        """Each tone sounds at its sample's own pitch at its root note (rate, fine tune), across the bank."""
        cases = [(44100, 60, 0), (22050, 60, 0), (11025, 48, 0), (32000, 69, 0), (22050, 60, 50), (8000, 72, -30),
                 (44100, 36, 0), (48000, 60, 0)]
        tones = []
        for i, (rate, root, cents) in enumerate(cases):
            pcm = (np.sin(np.arange(2000) * 0.05) * 10000).astype(np.int16)
            tones.append(dict(program=i, pcm=pcm, rate=rate, root=root, cents=cents, min=0, max=127, vol=100,
                              pan=64, adsr1=0x80FF, adsr2=0x5FC4, reverb=False, loop=False, loop_start=0.0,
                              bend=i % 12 + 1))
        vh, vb = audioconv.build_vab(tones)
        bank = vabmod.Vab(vh, vb)
        self.assertEqual(len(cases), bank.ps)
        self.assertEqual(sum(bank.vag_sizes()), len(vb), "VAG sizes add up to the VB")
        body = b""
        for i, (rate, root, cents) in enumerate(cases):
            body += vl(0) + bytes((0xC0 | i, i)) + vl(0) + bytes((0x90 | i, root, 100)) + vl(240) + bytes((0x80 | i,
                                                                                                         root, 0))
        seq = b"SEQp" + struct.pack(">IH", 1, 480) + struct.pack(">I", 500000)[1:] + b"\x04\x02" + body
        seq += b"\x00\xFF\x2F\x00"
        vh_p, vb_p = tmpfile(vh, ".vh"), tmpfile(vb, ".vb")
        try:
            h = run_bytes(seq, vh=vh_p, vb=vb_p, play_ms=3000, repeat=0)
        finally:
            os.remove(vh_p)
            os.remove(vb_p)
        self.assertIsNone(h.problem())
        self.assertEqual(len(cases), len(h.notes))
        for (rate, root, cents), n, i in zip(cases, h.notes, range(len(cases))):
            with self.subTest(rate=rate, root=root, cents=cents):
                self.assertEqual(1, len(n[5]), "one voice")
                pitch = n[5][0][3]
                want = 4096 * rate / 44100 * 2 ** (cents / 1200)
                self.assertLess(abs(pitch - want) / want, 0.006, "pitch %04X, expected %04X" % (pitch, round(want)))
                t = bank.tones(i)[0]
                self.assertEqual((i % 12 + 1, i % 12 + 1), (t["pbmin"], t["pbmax"]), "bend range")


# ---------------------------------------------------------------------------------------------------- tracker

class TrackerSongs(unittest.TestCase):

    def library_song(self, loop_row=0):
        song = tracker.new_song()
        song["patterns"], song["clips"] = [], []
        start = {}
        for cat, items in riffs.by_category():
            for rid, _ in items[:2]:
                pat = riffs.build(rid, song)
                song["patterns"].append(pat)
                t = {"Drums": 0, "Bass": 1, "Chords": 2, "Arpeggio": 3, "Melody": 3}[cat]
                song["clips"].append(tracker.new_clip(t, start.get(t, 0), len(song["patterns"]) - 1, pat["rows"]))
                start[t] = start.get(t, 0) + pat["rows"]
        song["loop"] = loop_row
        return song

    def test_song_plays_and_loops(self):
        song = self.library_song(loop_row=32)
        seq, info = tracker.compile_song(song)
        vh, vb = tracker.build_bank(song, tracker.InstrumentCache())[:2]
        length, loop = info["length_ms"] * 1000, info["loop_ms"] * 1000
        res, tmp, body = seqref.tracks(seq)[0]
        evs = seqref.events(body)
        self.assertAlmostEqual(length / 1e6, seqref.length_us(res, tmp, evs) / 1e6, delta=0.01)
        horizon = length + 1.5 * (length - loop)
        ref, _ = seqref.play(res, tmp, evs, horizon)
        vh_p, vb_p = tmpfile(vh, ".vh"), tmpfile(vb, ".vb")
        try:
            h = run_bytes(seq, vh=vh_p, vb=vb_p, play_ms=int(horizon / 1000))
        finally:
            os.remove(vh_p)
            os.remove(vb_p)
        self.assertIsNone(h.problem())
        self.assertEqual([], match([r[:4] for r in ref if r[0] < horizon - 1e5], [n for n in h.notes
                                                                                 if n[0] < horizon - 1e5]))
        # After the end: the notes from the loop row again
        first = [r[1:4] for r in ref if loop <= r[0] < loop + 2e6]
        again = [r[1:4] for r in ref if length <= r[0] < length + 2e6]
        self.assertEqual(sorted(first), sorted(again), "the song's loop")
        self.assertEqual([], compare_voices(ref, h.notes, vabmod.Vab(vh, vb)))

    def test_midi_export(self):
        """Export MIDI (as the page does: the SEQ for MIDI, then SEQ -> MIDI): the song's notes at their times."""
        song = self.library_song(loop_row=None)
        seq, info = tracker.compile_song(song, midi=True)
        res, tmp, body = seqref.tracks(seq)[0]
        evs = seqref.events(body)
        ref, _ = seqref.play(res, tmp, evs, 1e12)
        src = tmpfile(seq, ".seq")
        out = src + ".mid"
        try:
            audioconv.seq_to_midi(src, out)
            with open(out, "rb") as f:
                notes, _, _ = read_midi_notes(f.read())
        finally:
            os.remove(src)
            if os.path.exists(out):
                os.remove(out)
        self.assertEqual(len(ref), len(notes))
        for r, n in zip(sorted(r[:4] for r in ref), sorted(notes)):
            self.assertEqual(r[1:4], n[1:4])
            self.assertAlmostEqual(r[0], n[0], delta=1000)


# ---------------------------------------------------------------------------------------------------- XA, exports

class XaAndExports(unittest.TestCase):

    def test_xa_round_trip(self):
        for rate, stereo in ((37800, False), (18900, False), (37800, True), (18900, True)):
            with self.subTest(rate=rate, stereo=stereo):
                t = np.arange(rate * 2) / rate
                a = (np.sin(2 * np.pi * 330 * t) * 15000).astype(np.int16)
                b = (np.sin(2 * np.pi * 523 * t) * np.exp(-t) * 15000).astype(np.int16)
                songs = [np.stack([a, b], 1) if stereo else a, np.stack([b, a], 1) if stereo else b]
                path = os.path.join(TMP, "t.xa")
                os.makedirs(TMP, exist_ok=True)
                audioconv.write_xa(path, songs, rate, stereo)
                written = audioconv.xa_to_audio(path, os.path.join(TMP, "t"))
                self.assertEqual(2, len(written), "channels")
                for (ch, f, secs), src in zip(written, songs):
                    self.assertAlmostEqual(2.0, secs, delta=0.1)
                    back = audioconv.load(f, rate, stereo=stereo)
                    src_m = src if not stereo else src
                    # (the XA decoder's start may be a few samples apart: best of a few offsets)
                    best = max(snr_db(src_m[:len(back) - 64].reshape(-1), back[k:k + len(src_m) - 64].reshape(-1))
                               for k in range(0, 8))
                    self.assertGreater(best, 15, "channel %d signal to noise" % ch)
                    os.remove(f)
                os.remove(path)

    def test_export_banks(self):
        banks = [(vh, vb) for _, vh, vb in corpus() if vh]
        banks = list(dict.fromkeys(banks))
        if not banks:
            self.skipTest("no PS1_SOUND_CORPUS")
        for vh, vb in banks:
            with self.subTest(bank=os.path.basename(vh)):
                out = os.path.join(TMP, "export")
                os.makedirs(out, exist_ok=True)
                written, text = audioconv.export_vab(vh, out, vb)
                sizes = vabmod.Vab.load(vh, vb).vag_sizes()
                self.assertEqual(sum(1 for s in sizes[1:] if s), len(written) if isinstance(written, list) else written,
                                 "samples written (the empty ones skipped)")
                for f in glob.glob(os.path.join(out, "*")):
                    os.remove(f)


if __name__ == "__main__":
    unittest.main()
