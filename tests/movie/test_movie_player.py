"""
The console's movie tester (ps1-tools/videotest/videotest.c) run on the PC (vthost): STR movies made by the Movie
Tools' writer, played from start to end as the console plays them:
- every frame put together from its sectors and given to the MDEC intact (its bytes = the file's), each shown once,
  in order, at its pace; none dropped, late, or stalled when the decoder and the file server keep up
- the sound (XA turned into SPU blocks, the SPU ring and its interrupt) = FFmpeg's decoding of the file's XA
- the picture and the sound together (a beep at each second, the frame of that second): the gap constant, no drift,
  also after a pause, at the loop, NTSC and PAL, RAM and stream modes, 1x and 2x, each sound format
- the commands: pause, play, step, restart, loop, depth 24, scale, close and open again
- slower decoder, slower file server: frames dropped as designed, no hang, the sound kept
- 640-wide movies, 2048 / 2352 / RIFF CDXA sectors
- damaged files: no buffer or VRAM overflow, no hang

    python -m unittest tests/movie/test_movie_player.py -v

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import random
import shutil
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from moviehost import (TMP, av_offsets, crc, ffmpeg, ffmpeg_audio, make_movie, run, snr_db,  # noqa: E402
                       strmovie, to_raw)

OPEN = "open %s speed %d mode %s loop %d"


def frames_of(path):
    st = strmovie.StrFile(path)
    return st, {f["number"]: crc(st.frame_bs(f)) for f in st.frames}


class Played(unittest.TestCase):
    """A movie played to its end: the checks every playback gets."""

    def play(self, path, speed=2, mode="auto", loop=0, commands=(), **kw):
        r = run(path, [(0, OPEN % (os.path.basename(path), speed, mode, loop))] + list(commands), **kw)
        self.assertEqual([], r.problems, r.raw[-600:])
        return r

    def check_frames(self, r, path, all_shown=True, once=True):
        st, crcs = frames_of(path)
        for vs, number, size, c in r.decodes:
            self.assertEqual(crcs.get(number), c, "frame %d given to the MDEC differs from the file's" % number)
        shown = [s[1] for s in r.shows]
        if once:
            self.assertEqual(sorted(shown), shown, "frames shown in order")
            self.assertEqual(len(set(shown)), len(shown), "each frame shown once")
        if all_shown:
            self.assertEqual(list(range(1, len(st.frames) + 1)), shown, "every frame shown")
        return st

    def check_pace(self, r, fps, tolerance_frames=1.5):
        """Frames shown at their pace: frame n about (n-1)/fps after frame 1 (the shown times on the vsyncs)."""
        first = r.shows[0]
        for vs, n, _ in r.shows:
            want = (n - first[1]) / fps
            got = (vs - first[0]) * r.vsync_us / 1e6
            self.assertLess(abs(got - want), tolerance_frames / fps + 0.02,
                            "frame %d shown at %.3f s, its time %.3f s" % (n, got, want))

    def check_sync(self, r, fps, speed=2, max_gap=None, max_spread=None):
        """The picture and the sound together: each second's first frame shown after its beep by a gap within what
        the format allows (a frame is shown once its last sector is read: up to a frame's sectors, plus a vertical
        blank), the gaps spread by no more than the steps of time (a vertical blank, a sector or two), no drift."""
        sector = 1.0 / (75 * speed)
        vsync = r.vsync_us / 1e6
        max_gap = max_gap or (1.0 / fps + 2 * sector + 2 * vsync)
        max_spread = max_spread or (vsync + 2 * sector + 0.012)
        offs = av_offsets(r, fps)
        self.assertGreaterEqual(len(offs), 2, "the beeps heard")
        gaps = [o for _, o in offs]
        text = ", ".join("%.3f" % g for g in gaps)
        self.assertLess(max(abs(g) for g in gaps), max_gap, "picture / sound gap (s): %s" % text)
        self.assertLess(max(gaps) - min(gaps), max_spread, "picture / sound gaps spread (s): %s" % text)
        if len(gaps) >= 8:      # (a trend over enough seconds: the first one, the clock settling, weighs less)
            slope = np.polyfit([k for k, _ in offs], gaps, 1)[0]
            self.assertLess(abs(slope), 0.006, "picture / sound drift: %.1f ms a second (%s)" % (slope * 1e3, text))
        return gaps


class Playback(Played):

    VARIANTS = [  # name, movie options, play options
        ("2x stereo 37800", dict(), dict()),
        ("2x mono 37800", dict(stereo=False), dict()),
        ("2x stereo 18900", dict(rate=18900), dict()),
        ("2x mono 18900", dict(rate=18900, stereo=False), dict()),
        ("1x mono 18900", dict(speed=1, rate=18900, stereo=False), dict(speed=1)),
        ("1x stereo 37800", dict(speed=1), dict(speed=1)),
        ("1x stereo 18900 10 fps", dict(speed=1, rate=18900, fps=10), dict(speed=1)),
        ("2x 30 fps", dict(fps=30), dict()),
        ("2x 24 fps", dict(fps=24), dict()),
        ("2x 256x224", dict(w=256, h=224), dict()),
        ("2x 160x112 25 fps", dict(w=160, h=112, fps=25), dict()),
    ]

    def test_variants(self):
        for name, mopts, popts in self.VARIANTS:
            movie_opts = dict(dict(seconds=4), **mopts)
            path, _ = make_movie("v_%s.str" % name.replace(" ", "_"), **movie_opts)
            fps = movie_opts.get("fps", 15)
            for mode in ("ram", "stream"):
                for pal in (False, True):
                    with self.subTest(variant=name, mode=mode, pal=pal):
                        if mode == "ram" and os.path.getsize(path) > 1024 * 1024:
                            continue        # (RAM mode: the movie must fit in the tester's 1 MB)
                        r = self.play(path, mode=mode, pal=pal, **popts)
                        self.check_frames(r, path)
                        self.assertEqual(0, r.done["dropped"] + r.done["late"] + r.done["underruns"],
                                         "dropped, late, sound underruns: %s" % r.done)
                        self.assertEqual(1, r.done["ended"], "the end reached")
                        self.check_pace(r, fps)
                        self.check_sync(r, fps, popts.get("speed", 2))

    def test_sound_content(self):
        """The sound the SPU played (the XA units turned into SPU blocks) = FFmpeg's decoding of the file's XA."""
        if not ffmpeg():
            self.skipTest("no FFmpeg")
        for rate in (37800, 18900):
            for stereo in (True, False):
                with self.subTest(rate=rate, stereo=stereo):
                    path, _ = make_movie("snd_%d_%d.str" % (rate, stereo), rate=rate, stereo=stereo, seconds=3)
                    raw = to_raw(path, path + ".2352")
                    ref = ffmpeg_audio(raw, stereo)
                    r = self.play(path)
                    played, out_rate = r.sound()
                    self.assertEqual(rate, out_rate)
                    start = r.keyons[0] * r.vsync_us * rate // 1000000
                    got = played[start:start + len(ref)]
                    ref_l = ref[:, 0] if stereo else ref
                    self.assertGreater(snr_db(ref_l[:len(got)], got[:, 0]), 30, "left / mono")
                    if stereo:
                        self.assertGreater(snr_db(ref[:len(got), 1], got[:, 1]), 30, "right")

    def test_sector_formats(self):
        path, _ = make_movie("fmt.str", seconds=3)
        raw = to_raw(path, os.path.join(TMP, "fmt_raw.str"))
        riff = to_raw(path, os.path.join(TMP, "fmt_riff.str"), riff=True)
        video, _ = make_movie("fmt_2048.str", seconds=3, fmt=2048)
        for p in (raw, riff, video):
            with self.subTest(file=os.path.basename(p)):
                r = self.play(p)
                self.check_frames(r, p)
                self.assertEqual(1, r.done["ended"])
                if p != video:
                    self.check_sync(r, 15)

    def test_wide(self):
        for w, h in ((640, 240), (512, 240), (368, 240)):
            with self.subTest(w=w, h=h):
                path, _ = make_movie("wide_%d.str" % w, w=w, h=h, seconds=3)
                for depth in ("15", "24"):
                    r = self.play(path, commands=[(1, "depth %s" % depth)])
                    self.check_frames(r, path)
                    self.check_sync(r, 15)


class Commands(Played):

    def test_long_no_drift(self):
        """A 20-second movie (NTSC and PAL, 1x and 2x): no drift between the picture and the sound."""
        for speed in (1, 2):
            path, _ = make_movie("long_%d.str" % speed, w=160, h=112, seconds=20, speed=speed, rate=18900)
            for pal in (False, True):
                with self.subTest(speed=speed, pal=pal):
                    r = self.play(path, speed=speed, pal=pal, vsyncs=60 * 30)
                    self.check_frames(r, path)
                    self.check_sync(r, 15, speed)

    def test_pause_resume(self):
        """Paused 2 s at 1.3 s, then played on: the picture and the sound still together."""
        path, _ = make_movie("pause.str", seconds=5)
        r = self.play(path, commands=[(78, "pause"), (198, "play")])
        self.check_frames(r, path)
        gaps = self.check_sync(r, 15)
        self.assertGreater(r.done["vsync"] * r.vsync_us / 1e6, 6.5, "the pause took its time")

    def test_pause_twice_and_toggle(self):
        path, _ = make_movie("pause2.str", seconds=5)
        r = self.play(path, commands=[(40, "pause"), (70, "toggle"), (130, "toggle"), (160, "play")])
        self.check_frames(r, path)
        self.check_sync(r, 15)

    def test_step(self):
        path, _ = make_movie("step.str", seconds=4)
        r = self.play(path, until_end=False, vsyncs=400,
                      commands=[(60, "pause"), (90, "step"), (110, "step"), (130, "step"), (150, "play")])
        shown_paused = [s[1] for s in r.shows if 60 < s[0] < 150]
        self.assertEqual(3, len(shown_paused), "a frame per step: %s" % shown_paused)
        self.assertEqual(list(range(shown_paused[0], shown_paused[0] + 3)), shown_paused, "the next frames")
        self.check_frames(r, path, all_shown=False)

    def test_loop(self):
        path, _ = make_movie("loop.str", seconds=3)
        r = self.play(path, loop=1, until_end=False, vsyncs=int(3 * 60 * 2.6))
        self.assertGreaterEqual(sum(1 for s in r.shows if s[1] == 1), 2, "frame 1 again after the end")
        self.assertEqual([], r.problems)
        # Each pass in sync: the beeps against the frames of the same pass
        sound, rate = r.sound()
        from moviehost import beeps
        heard = beeps(sound, rate)
        firsts = [s for s in r.shows if (s[1] - 1) % 15 == 0]
        n = min(len(heard), len(firsts))
        gaps = [firsts[i][0] * r.vsync_us / 1e6 - heard[i] for i in range(n)]
        self.assertGreaterEqual(n, 5)
        self.assertLess(max(gaps) - min(gaps), 0.05, "gaps over the loop: %s" % gaps)

    def test_restart_close_reopen(self):
        path, _ = make_movie("restart.str", seconds=3)
        name = os.path.basename(path)
        r = self.play(path, until_end=True, commands=[(50, "restart"), (120, "close"),
                                                      (140, OPEN % (name, 2, "auto", 0))])
        self.assertEqual(1, r.done["ended"])
        self.assertGreaterEqual(sum(1 for s in r.shows if s[1] == 1), 3, "from the start: restart and open again")
        self.check_frames(r, path, all_shown=False, once=False)
        last = [s[1] for s in r.shows if s[0] >= 140]
        self.assertEqual(list(range(1, 46)), last, "the movie opened again: every frame")

    def test_switches_while_playing(self):
        path, _ = make_movie("switch.str", seconds=4)
        cmds = [(20, "depth 24"), (40, "scale 1x"), (60, "depth 15"), (80, "scale fit"), (100, "scale full"),
                (110, "info 0"), (120, "volume 40"), (130, "loop 0"), (140, "volume 100")]
        r = self.play(path, commands=cmds)
        st = self.check_frames(r, path, all_shown=False)
        # (a depth change frees the frames decoded ahead, in the other depth: 2 or 3 of them not shown, each time)
        self.assertGreaterEqual(len(r.shows), len(st.frames) - 3 * 2, "frames shown: %s" % [s[1] for s in r.shows])
        self.check_sync(r, 15)

    def test_open_missing(self):
        r = run(os.path.join(TMP, "x"), [(0, "open no_such_movie.str"), (30, "play"), (40, "step")], vsyncs=120,
                until_end=False)
        self.assertEqual([], r.problems)
        self.assertTrue(any("error" in l for l in r.lines), r.lines)


class Limits(Played):
    """The tester behind (slow decoder, slow file server): frames dropped as designed, the sound and the pace kept."""

    def test_slow_decoder(self):
        path, _ = make_movie("slowdec.str", fps=30, seconds=4)
        for vs in (2, 3, 6):
            with self.subTest(decode_vsyncs=vs):
                r = self.play(path, decode_vsyncs=vs)
                self.check_frames(r, path, all_shown=False)
                self.assertEqual(0, r.done["underruns"], "the sound kept")
                self.check_sync(r, 30, max_gap=0.2, max_spread=0.12)

    def test_heavy_movie(self):
        """Frames bigger than their share of the sectors (noise at 1x): shown late, the sound unbroken, no overflow."""
        path, _ = make_movie("heavy.str", w=320, h=240, fps=30, speed=1, seconds=3, kind="noise")
        r = self.play(path, speed=1, vsyncs=60 * 20)
        self.check_frames(r, path, all_shown=False)
        self.assertEqual(1, r.done["ended"])
        self.assertEqual(0, r.done["underruns"], "the sound kept: %s" % r.done)

    def test_slow_file_server(self):
        path, _ = make_movie("slowread.str", seconds=6)
        for kbps in (400, 300, 200):
            with self.subTest(read_kbps=kbps):
                r = self.play(path, mode="stream", read_kbps=kbps, vsyncs=60 * 40)
                self.check_frames(r, path, all_shown=False)
                self.assertEqual(1, r.done["ended"], "the end reached: %s" % r.done)


class Damaged(Played):
    """Damaged movies: sector headers, frame headers, BS data, sound subheaders changed, files cut: the tester must not
    write outside its buffers or the VRAM, nor hang."""

    FIELDS = [  # (name, offset in the video sector's data, size)
        ("index", 4, 2), ("count", 6, 2), ("frame", 8, 4), ("size", 12, 4), ("width", 16, 2), ("height", 18, 2),
        ("bs words", 32, 2), ("bs magic", 34, 2), ("bs scale", 36, 2), ("bs version", 38, 2), ("data", 100, 4),
    ]

    def test_damaged(self):
        n = int(os.environ.get("MOVIE_FUZZ", "60"))
        base, _ = make_movie("dmg.str", seconds=3)
        with open(base, "rb") as f:
            data = bytearray(f.read())
        st = strmovie.StrFile(base)
        video = [i for i in range(st.count) if i not in set(st.audio)]
        rng = random.Random(7)
        problems = []
        values = [0, 1, 0xFF, 0xFFFF, 0x7FFF, 0x8000, 0xFFFFFFFF, 0x7FFFFFFF, 0x10000, 640, 641, 1024, 4096]
        cases = []
        for name, off, size in self.FIELDS:
            for k in range(3):
                d = bytearray(data)
                for _ in range(rng.randint(1, 4)):
                    s = rng.choice(video)
                    v = rng.choice(values) & ((1 << (8 * size)) - 1)
                    d[s * 2336 + 8 + off:s * 2336 + 8 + off + size] = v.to_bytes(size, "little")
                cases.append(("%s #%d" % (name, k), d))
        for k in range(n):
            d = bytearray(data)
            for _ in range(rng.randint(1, 20)):
                d[rng.randrange(len(d))] = rng.randrange(256)
            cases.append(("bytes #%d" % k, d))
        for k in range(6):
            d = bytearray(data)
            for s in rng.sample(st.audio, 3):
                d[s * 2336 + 3] = d[s * 2336 + 7] = rng.choice((0x10, 0x05, 0xFF, 0x04))
            cases.append(("sound coding #%d" % k, d))
        for cut in (2336 * 10 + 100, len(data) // 2 + 7, len(data) - 1000, 2000, 50):
            cases.append(("cut at %d" % cut, data[:cut]))
        for what, d in cases:
            path = os.path.join(TMP, "dmg_case.str")
            with open(path, "wb") as f:
                f.write(d)
            for mode in ("ram", "stream"):
                r = run(path, [(0, OPEN % ("dmg_case.str", 2, mode, 0)), (100, "step"), (120, "play")],
                        vsyncs=60 * 12, timeout=60)
                if r.problems:
                    problems.append("%s (%s): %s" % (what, mode, "; ".join(r.problems)[:300]))
        self.assertEqual([], problems)


if __name__ == "__main__":
    unittest.main()
