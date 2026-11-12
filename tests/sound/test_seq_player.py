"""
The sound tester's SEQ / SEP player (ps1-tools/soundtest/soundtest.c), run on the PC (seqhost) and compared with the
reference player (seqref.py): notes and their times (a frame at a time, NTSC and PAL), tempo changes, loops (counted,
endless, several), the header variants, SEPs, seeking, the effects (transpose, tempo, mutes), the voices (pitch,
volume, layered tones) against the bank; files cut short or damaged (no endless loop, no read past the data).

    python -m unittest tests/sound/test_seq_player.py -v
    PS1_SOUND_CORPUS=dir1;dir2   the game / SDK SEQ, SEP and VAB files to play too (not in the repository)
    SOUND_FUZZ=N                 damaged copies of each of them tried (default 40)

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import random
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import seqref                                             # noqa: E402
from soundhost import (NTSC_US, PAL_US, compare_notes, compare_voices, corpus, ev, run, run_bytes,  # noqa: E402
                       seq_file, sep_file, vabmod)

ON, OFF, CC, PC, BEND = 0x90, 0x80, 0xB0, 0xC0, 0xE0
TEMPO = (0xFF, 0x51)


def loop_start(count=None, ch=0, count_cc=6):
    out = ev(0, CC | ch, 99, 20)
    if count is not None:
        out += ev(0, CC | ch, count_cc, count)
    return out


def loop_end(delta=0, ch=0):
    return ev(delta, CC | ch, 99, 30)


def scale(n=8, step=240, ch=0, first=60):
    """n notes a step apart (each released before the next), running status."""
    out = ev(0, ON | ch, first, 100)
    for i in range(n):
        out += ev(step, first + i, 0)               # (running status: note on, velocity 0)
        if i + 1 < n:
            out += ev(0, first + i + 1, 100)
    return out


class Played:
    """A SEQ (bytes) played by both players: host result, reference notes, end."""

    def __init__(self, data, frame_us=NTSC_US, horizon_ms=60000, track=0, **opts):
        self.data, self.frame_us = data, frame_us
        self.host = run_bytes(data, frame_us=frame_us, play_ms=horizon_ms, track=track, **opts)
        res, tempo, body = seqref.tracks(data)[track]
        self.events = seqref.events(body)
        self.ref, self.end = seqref.play(res, tempo, self.events, horizon_ms * 1000)
        self.res, self.tempo = res, tempo
        self.horizon = horizon_ms * 1000


class SynthTests(unittest.TestCase):
    """SEQs written for the tests, each at NTSC and PAL frames."""

    def check(self, data, horizon_ms=60000, frame_us=None, **opts):
        out = []
        for f in ([frame_us] if frame_us else [NTSC_US, PAL_US]):
            p = Played(data, f, horizon_ms, **opts)
            self.assertIsNone(p.host.problem(), p.host.raw[-500:])
            self.assertEqual([], compare_notes(p.ref, p.host.notes, f, p.horizon, tempo_pct=opts.get("tempo", 100),
                                           tick_us=seqref.tick_us(p.res, p.tempo, p.events)), "frame %d us" % f)
            out.append(p)
        return out[0]

    def test_notes_running_status(self):
        p = self.check(seq_file(scale(16)))
        self.assertEqual(16, len(p.host.notes))

    def test_tempo_changes(self):
        body = scale(4) + ev(0, *TEMPO, 0x03, 0xD0, 0x90) + scale(4, first=70)   # 250000 us
        body += ev(0, *TEMPO, 0x0F, 0x42, 0x40) + scale(4, first=80)              # 1 s per quarter note
        self.check(seq_file(body))

    def test_tempo_changes_add_up(self):
        # Many tempo changes inside frames, played again by an endless loop (as Alundra 2): the frame's ticks after
        # each change at the new tempo, no part of a tick lost (it was: notes 5-14 ms late after 2 minutes)
        body = loop_start(127)
        for i in range(40):
            us = (523417, 611111, 487213)[i % 3]
            body += ev(7, *TEMPO, us >> 16, (us >> 8) & 255, us & 255) + ev(11, ON, 60 + i % 12, 100)
            body += ev(13, OFF, 60 + i % 12, 0)
        body += loop_end(5)
        self.check(seq_file(body), horizon_ms=150000)

    def test_tempo_meta_running_status(self):
        # Sony: FF 51 ttt <delta> 51 ttt (a meta event again with running status), then a note with its own status
        body = ev(0, *TEMPO, 0x03, 0xD0, 0x90) + ev(480, 0x51, 0x0F, 0x42, 0x40) + scale(4)
        p = self.check(seq_file(body))
        self.assertAlmostEqual(0.25, p.ref[0][0] / 1e6, places=3)     # (the second tempo at 480 ticks of the first)
        self.assertAlmostEqual(0.25 + 0.5, p.ref[1][0] / 1e6, places=3)

    def test_header_variants(self):
        for kw in (dict(ident=b"pQES"), dict(version16=True), dict(res=96), dict(res=0, tempo=0)):
            with self.subTest(**{k: str(v) for k, v in kw.items()}):
                self.check(seq_file(scale(6), **kw))

    def test_extreme_tempos(self):
        for tempo in (1, 100, 0xFFFFFF):
            with self.subTest(tempo=tempo):
                p = Played(seq_file(scale(4)), NTSC_US, 30000 if tempo > 1000000 else 5000, tempo=tempo)
                self.assertIsNone(p.host.problem())

    def test_counted_loops(self):
        for cc in (6, 98):
            with self.subTest(count_cc=cc):
                body = scale(2) + loop_start(2, count_cc=cc) + scale(3, first=70) + loop_end() + scale(2, first=50)
                p = self.check(seq_file(body), repeat=0)
                self.assertEqual(2 + 3 * 3 + 2, len(p.host.notes))
                self.assertIsNotNone(p.host.stop)

    def test_endless_loop(self):
        body = scale(2) + loop_start(127) + scale(3, first=70) + loop_end() + scale(2, first=50)
        p = self.check(seq_file(body), horizon_ms=20000)
        self.assertGreater(len(p.host.notes), 30)
        self.assertIsNone(p.host.restart)

    def test_loop_end_without_count_or_start(self):
        self.check(seq_file(scale(2) + loop_start(None) + scale(3) + loop_end() + scale(2)), repeat=0)
        self.check(seq_file(scale(2) + loop_end() + scale(2)), repeat=0)

    def test_two_loops(self):
        body = loop_start(1) + scale(2) + loop_end() + loop_start(2) + scale(2, first=70) + loop_end() + scale(1)
        p = self.check(seq_file(body), repeat=0)
        self.assertEqual(2 * 2 + 2 * 3 + 1, len(p.host.notes))

    def test_loop_running_status(self):
        # The loop's first events use the running status of the loop start (a controller): restored at each jump
        body = ev(0, ON, 60, 100) + loop_start(2) + ev(10, 7, 100) + ev(0, ON, 62, 100) + ev(240, 62, 0)
        body += loop_end(240) + ev(0, 60, 0)
        p = self.check(seq_file(body), repeat=0)
        self.assertEqual(4, len(p.host.notes))

    def test_endless_loop_without_time(self):
        """An endless loop with nothing to wait for inside: the frame must still end (no hang)."""
        body = scale(2) + loop_start(127) + ev(0, ON, 64, 100) + ev(0, 64, 0) + loop_end() + scale(1)
        for count in (127, 3):
            with self.subTest(count=count):
                body = scale(2) + loop_start(count) + ev(0, ON, 64, 100) + ev(0, 64, 0) + loop_end() + scale(1)
                h = run_bytes(seq_file(body), play_ms=3000, timeout=20)
                self.assertIsNone(h.problem(), h.raw[-300:])
                self.assertEqual([60, 61, 64, 64, 60], [n[3] for n in h.notes[:5]])

    def test_no_notes(self):
        p = Played(seq_file(ev(0, *TEMPO, 0x07, 0xA1, 0x20) + ev(960, CC, 7, 100)), NTSC_US, 5000)
        self.assertIsNone(p.host.problem())
        self.assertIsNotNone(p.host.stop, "a SEQ without notes stops (not started again each frame)")

    def test_cut_short(self):
        data = seq_file(scale(8), end=False)
        for cut in (0, 1, 2, 3, 5):
            with self.subTest(cut=cut):
                d = data[:len(data) - cut]
                p = Played(d, NTSC_US, 10000, repeat=0, guard=True)
                self.assertIsNone(p.host.problem(), p.host.raw[-300:])

    def test_repeat(self):
        p = Played(seq_file(scale(4)), NTSC_US, 10000, repeat=1)
        self.assertIsNotNone(p.host.restart)
        self.assertAlmostEqual(p.end / 1e6, p.host.restart / 1e6, delta=0.05)

    def test_sep(self):
        seqs = [(480, 500000, scale(4)), (96, 250000, scale(6, step=48, first=70)), (480, 600000, scale(2, first=40))]
        data = sep_file(seqs)
        for i in range(3):
            with self.subTest(track=i):
                p = self.check(data, track=i)
                self.assertEqual(3, len(p.host.tracks))

    def test_sep_size_past_end(self):
        data = sep_file([(480, 500000, scale(4)), (480, 500000, scale(4))])
        data = data[:-6]                                    # the last SEQ cut, its size too big
        for i in range(2):
            p = Played(data, NTSC_US, 5000, track=i, guard=True)
            self.assertIsNone(p.host.problem(), p.host.raw[-300:])

    def test_controllers_kept(self):
        body = ev(0, PC, 5) + ev(0, CC, 7, 90) + ev(0, CC, 10, 20) + ev(0, CC, 11, 70) + ev(0, CC, 64, 127)
        body += scale(4) + ev(0, CC, 64, 0) + ev(0, CC, 123, 0) + ev(0, 0xD0, 50) + ev(0, 0xA0, 60, 40) + scale(2)
        self.check(seq_file(body))

    def test_effects(self):
        data = seq_file(scale(8, ch=0) + scale(8, ch=1, first=48))
        p = Played(data, NTSC_US, 20000, tempo=200)
        self.assertEqual([], compare_notes(p.ref, p.host.notes, NTSC_US, p.horizon, tempo_pct=200))
        p = Played(data, NTSC_US, 20000, mute=2)
        self.assertEqual(16, len(p.host.notes), "muted notes are still read")

    def test_seek(self):
        body = ev(0, PC, 3) + ev(0, CC, 7, 80) + scale(4) + ev(0, CC, 10, 30) + ev(0, *TEMPO, 0x03, 0xD0, 0x90)
        body += ev(0, BEND, 0, 0x50) + scale(8, first=70)
        data = seq_file(body)
        res, tempo, b = seqref.tracks(data)[0]
        evs = seqref.events(b)
        length = seqref.length_us(res, tempo, evs)
        for ms in (1, 250, 499, 500, 1000, 1500, 1999, int(length / 1000) + 100):
            with self.subTest(seek_ms=ms):
                h = run_bytes(data, seek=ms, play_ms=10000, repeat=0)
                self.assertIsNone(h.problem())
                if ms * 1000 >= length:         # (past the end: from the start)
                    ms = 0
                want = seqref.state_at(res, tempo, evs, ms * 1000)
                got = h.state[1]
                for c in range(16):
                    w = want[c]
                    self.assertEqual((w["prog"], w["volume"], w["expression"], w["pan"], w["bend"], w["sustain"]),
                                     got[c], "channel %d after a seek to %d ms" % (c, ms))
                ref, _ = seqref.play(res, tempo, evs, 1e12, loops=False, start_us=ms * 1000)
                shifted = [(n[0] - ms * 1000,) + n[1:] for n in ref]
                self.assertEqual([], compare_notes(shifted, h.notes, NTSC_US, 1e12))

    def test_seek_on_a_note(self):
        """A seek to the time of a note (0: the start, as Play from the start sends) plays that note."""
        data = seq_file(ev(0, ON, 60, 100) + ev(0, ON | 1, 64, 100) + ev(480, OFF, 60, 0) + ev(0, ON, 62, 100) +
                        ev(480, 62, 0))
        for ms in (0, 500):
            with self.subTest(seek_ms=ms):
                h = run_bytes(data, seek=ms, play_ms=3000, repeat=0)
                first = [n[3] for n in h.notes[:2]]
                self.assertEqual([60, 64] if ms == 0 else [62], first[:2 if ms == 0 else 1],
                                 "notes played after a seek to %d ms: %s" % (ms, [n[3] for n in h.notes]))

    def test_seek_in_loop(self):
        body = scale(2) + loop_start(2) + scale(4, first=70) + loop_end() + scale(2, first=50)
        data = seq_file(body)
        h = run_bytes(data, seek=1000, play_ms=20000, repeat=0)
        self.assertIsNone(h.problem())
        # From 1 s: the loop's last 2 notes, its 2 jumps back (4 notes each), the 2 after it
        self.assertEqual(2 + 2 * 4 + 2, len(h.notes), "the loop is played after a seek into it")

    def test_many_voices(self):
        body = b"".join(ev(0, ON | (i % 16), 40 + i, 100) for i in range(40)) + ev(960, CC, 123, 0)
        p = Played(seq_file(body), NTSC_US, 5000)
        self.assertIsNone(p.host.problem())


class CorpusTests(unittest.TestCase):
    """The game and SDK files of PS1_SOUND_CORPUS: every SEQ of each, played for up to 3 minutes."""

    @classmethod
    def setUpClass(cls):
        cls.files = corpus()
        if not cls.files:
            raise unittest.SkipTest("no PS1_SOUND_CORPUS")

    def test_play(self):
        for path, vh, vb in self.files:
            with open(path, "rb") as f:
                data = f.read()
            bank = vabmod.Vab.load(vh, vb) if vh else None
            for i, (res, tempo, body) in enumerate(seqref.tracks(data)):
                for frame in (NTSC_US, PAL_US):
                    with self.subTest(file=os.path.basename(path), track=i, frame=frame):
                        evs = seqref.events(body)
                        length = seqref.length_us(res, tempo, evs)
                        horizon = min(180e6, length * 2 + 5e6)
                        h = run(path, vh, vb, track=i, frame_us=frame, play_ms=int(horizon / 1000))
                        self.assertIsNone(h.problem(), h.raw[-400:])
                        self.assertAlmostEqual(length / 1e6, h.scan[0] / 1e6, delta=0.01 + length * 1e-5 / 1e6,
                                               msg="length")
                        used = sum(1 << c for c in seqref.channels_used(evs))
                        self.assertEqual(used, h.scan[1], "channels with notes")
                        ref, _ = seqref.play(res, tempo, evs, horizon)
                        self.assertEqual([], compare_notes(ref, h.notes, frame, horizon, tick_us=seqref.tick_us(res, tempo, evs)))
                        if bank and frame == NTSC_US:
                            self.assertEqual([], compare_voices(ref, h.notes, bank))

    def test_effects(self):
        for path, vh, vb in self.files[:3]:
            bank = vabmod.Vab.load(vh, vb) if vh else None
            with open(path, "rb") as f:
                res, tempo, body = seqref.tracks(f.read())[0]
            evs = seqref.events(body)
            ref, _ = seqref.play(res, tempo, evs, 30e6)
            for opts in (dict(transpose=5), dict(transpose=-12), dict(tempo=150), dict(tempo=50), dict(mute=0x0F0F)):
                with self.subTest(file=os.path.basename(path), **opts):
                    h = run(path, vh, vb, play_ms=30000, **opts)
                    self.assertIsNone(h.problem())
                    self.assertEqual([], compare_notes(ref, h.notes, NTSC_US, 30e6, tempo_pct=opts.get("tempo", 100)))
                    if bank and "tempo" not in opts:
                        self.assertEqual([], compare_voices(ref, h.notes, bank, transpose=opts.get("transpose", 0),
                                                            mute=opts.get("mute", 0)))

    def test_seek(self):
        for path, vh, vb in self.files:
            with open(path, "rb") as f:
                data = f.read()
            for i, (res, tempo, body) in enumerate(seqref.tracks(data)):
                evs = seqref.events(body)
                length = seqref.length_us(res, tempo, evs)
                for frac in (0.0, 0.1, 0.37, 0.5, 0.83, 0.99):
                    ms = int(length * frac / 1000)
                    with self.subTest(file=os.path.basename(path), track=i, seek_ms=ms):
                        h = run(path, vh, vb, track=i, seek=ms, play_ms=8000)
                        self.assertIsNone(h.problem(), h.raw[-300:])
                        want = seqref.state_at(res, tempo, evs, ms * 1000)
                        for c in range(16):
                            w = want[c]
                            self.assertEqual((w["prog"], w["volume"], w["expression"], w["pan"], w["bend"],
                                              w["sustain"]), h.state[1][c], "channel %d" % c)
                        # The notes after it, up to the next loop end (read straight on: no jump yet)
                        ref, _ = seqref.play(res, tempo, evs, 1e12, loops=False, start_us=ms * 1000)
                        until = _next_loop_end(res, tempo, evs, ms * 1000)
                        window = min(8e6, until - ms * 1000)
                        shifted = [(n[0] - ms * 1000,) + n[1:] for n in ref]
                        self.assertEqual([], compare_notes(shifted, h.notes, NTSC_US, window,
                                                                   tick_us=seqref.tick_us(res, tempo, evs)))


def _next_loop_end(res, tempo, evs, after):
    res, tempo, t = res or 480, tempo or 500000, 0.0
    for delta, kind, args in evs:
        t += delta * tempo / res
        if kind == "tempo":
            tempo = args[0]
        if kind in ("loopend", "end") and t > after:
            return t
    return t


class RobustnessTests(unittest.TestCase):
    """Damaged copies of the corpus files (cut short, bytes changed): the player must not hang or read past the data
    (a read past it on the console: whatever is after the file played as music)."""

    @classmethod
    def setUpClass(cls):
        cls.files = corpus()
        if not cls.files:
            raise unittest.SkipTest("no PS1_SOUND_CORPUS")

    def test_damaged(self):
        n = int(os.environ.get("SOUND_FUZZ", "40"))
        rng = random.Random(1)
        hangs, reads = [], []
        for path, _, _ in self.files:
            with open(path, "rb") as f:
                data = f.read()
            name = os.path.basename(path)
            cases = [("cut at %d" % c, data[:c]) for c in sorted(rng.sample(range(16, len(data)), min(n // 2,
                                                                                                   len(data) - 16)))]
            for k in range(n // 2):
                d = bytearray(data)
                for _ in range(rng.randint(1, 8)):
                    d[rng.randrange(16, len(d))] = rng.randrange(256)
                cases.append(("bytes changed #%d" % k, bytes(d)))
            for what, d in cases:
                h = run_bytes(d, name=name, play_ms=20000, guard=True, timeout=30)
                if h.hang:
                    hangs.append("%s %s: %s" % (name, what, h.hang))
                elif h.crash or h.problem():
                    reads.append("%s %s: %s" % (name, what, h.crash or h.problem()))
        self.assertEqual([], hangs, "hangs")
        self.assertEqual([], reads, "reads past the data / crashes")


if __name__ == "__main__":
    unittest.main()
