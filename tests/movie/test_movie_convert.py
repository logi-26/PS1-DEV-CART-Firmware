"""
The Movie Tools' converter (gui/strmovie.py, gui/movieconv.py), its STR files checked independently (FFmpeg's psxstr
demuxer, MDEC and XA decoders; the CD sectors' EDC and ECC computed here):
- frames: the picture kept (against the source and FFmpeg's decoding), each in its sectors' budget, at every resolution;
  hard pictures (noise) at small budgets
- the frames' pace: each frame's last sector at its time (speed 1x / 2x, every frame rate, each sound format), no drift
- sectors: EDC / ECC, subheaders, the sound's interleave, the frame headers, the end of the file
- XA sound: FFmpeg decodes what the writer encoded; the reader on 2336 / 2352 / RIFF / 2048 sectors and damaged files
- whole conversions (movieconv.Job) from a test video with a flash and a beep each second: frame count, sound length,
  the flash and the beep together in the STR, trims, every option; slide shows; a conversion stopped

    python -m unittest tests/movie/test_movie_convert.py -v      (FFmpeg: the Movie Tools' download or the PATH)

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import random
import struct
import subprocess
import sys
import threading
import unittest
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from moviehost import (TMP, beep_sound, beeps, ffmpeg, ffmpeg_audio, ffmpeg_frames, make_movie, movieconv,  # noqa
                       psnr, snr_db, strmovie, test_picture, to_raw)


def need_ffmpeg(test):
    if not ffmpeg():
        test.skipTest("no FFmpeg (Movie Tools: Get FFmpeg)")


# ---------------------------------------------------------------------------------------------------- CD sectors

def _edc(data):
    """CD EDC: CRC-32, polynomial 0xD8018001 (reflected), no inversion."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xD8018001 if crc & 1 else 0)
    return crc


_F = [((i << 1) ^ (0x11D if i & 0x80 else 0)) & 0xFF for i in range(256)]
_B = [0] * 256
for _i in range(256):
    _B[_i ^ _F[_i]] = _i


def _ecc(src, major_count, minor_count, major_mult, minor_inc):
    size = major_count * minor_count
    out = bytearray(2 * major_count)
    for major in range(major_count):
        index = (major >> 1) * major_mult + (major & 1)
        a = b = 0
        for _ in range(minor_count):
            t = src[index]
            index += minor_inc
            if index >= size:
                index -= size
            a ^= t
            b ^= t
            a = _F[a]
        a = _B[_F[a] ^ b]
        out[major] = a
        out[major + major_count] = a ^ b
    return bytes(out)


def check_sector(s):
    """Problems of a 2336-byte Mode 2 sector (EDC, Form 1 ECC), [] if none."""
    sub = s[0:4]
    if sub != s[4:8]:
        return ["subheader copies differ"]
    if sub[2] & 0x20:           # Form 2: EDC over subheader + 2324 bytes
        e = struct.unpack_from("<I", s, 2332)[0]
        return [] if e in (0, _edc(s[0:2332])) else ["form 2 EDC"]
    problems = []
    if struct.unpack_from("<I", s, 2056)[0] != _edc(s[0:2056]):
        problems.append("form 1 EDC")
    src = bytes(4) + s[0:2060]
    p = _ecc(src, 86, 24, 2, 86)
    q = _ecc(src + p, 52, 43, 86, 88)
    if s[2060:2232] != p:
        problems.append("ECC P")
    if s[2232:2336] != q:
        problems.append("ECC Q")
    return problems


# ---------------------------------------------------------------------------------------------------- the writer

class Frames(unittest.TestCase):

    PICTURES = [("gradient", 30), ("smooth", 28), ("edges", 18), ("black", 40), ("white", 40), ("colours", 22)]
    # (colours: saturated colour edges, the colour at half resolution as the MDEC has it)

    def picture(self, kind, w, h):
        yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
        if kind == "gradient":
            return test_picture(w, h, 3)
        if kind == "smooth":
            img = np.stack((128 + 100 * np.sin(xx / 23.0), 128 + 100 * np.cos(yy / 17.0),
                            128 + 100 * np.sin((xx + yy) / 31.0)), -1)
        elif kind == "edges":
            img = np.where(((xx // 8 + yy // 8) % 2)[..., None] > 0, 230, 20) * np.ones(3)
        elif kind == "black":
            img = np.zeros((h, w, 3))
        elif kind == "white":
            img = np.full((h, w, 3), 255.0)
        else:
            img = np.stack((np.where(xx < w / 2, 255, 0), np.where(yy < h / 2, 255, 0),
                            np.where((xx + yy) % 64 < 32, 255, 0)), -1).astype(np.float32)
        return np.clip(img, 0, 255).astype(np.uint8)

    def test_quality_every_resolution(self):
        """Each picture at each resolution in 8 sectors: the writer's own decoding = FFmpeg's, close to the source."""
        need_ffmpeg(self)
        for res, _ in movieconv.RESOLUTIONS:
            w, h = (int(v) for v in res.split("x"))
            for kind, min_psnr in self.PICTURES:
                with self.subTest(res=res, picture=kind):
                    img = self.picture(kind, w, h)
                    path = os.path.join(TMP, "q.str")
                    wr = strmovie.StrWriter(path, w, h, 15, 2)
                    budget = 8 * strmovie.VIDEO_PAYLOAD
                    bs, scale, _ = strmovie.encode_frame(img, budget)
                    self.assertLessEqual(len(bs), budget)
                    wr.add_frame(bs, 8)
                    wr.close()
                    own = strmovie.decode_bs(bs, w, h)
                    ff = ffmpeg_frames(to_raw(path, path + ".2352"), w, h)
                    self.assertEqual(1, len(ff), "FFmpeg decodes the frame")
                    self.assertGreater(psnr(own, ff[0]), 38, "the writer's decoding and FFmpeg's agree")
                    self.assertGreater(psnr(img, ff[0]), min_psnr if w * h <= 320 * 256 else min_psnr - 4,
                                       "the picture kept (scale %d)" % scale)

    def test_hard_frames_small_budgets(self):
        """Noise (the hardest picture) at the smallest budgets: too big for its share even at the coarsest scale, a
        frame takes the sectors it needs (counted), the next ones smaller: the movie back on time after them."""
        for w, h, fps, speed in ((640, 240, 30, 1), (320, 240, 60, 1), (512, 240, 30, 1), (320, 256, 30, 2)):
            with self.subTest(w=w, h=h, fps=fps, speed=speed):
                path = os.path.join(TMP, "hard.str")
                audio = (beep_sound(1, 37800, True), 37800, True)
                wr = strmovie.StrWriter(path, w, h, fps, speed, audio)
                for i in range(6):
                    n, budget = wr.frame_budget()
                    bs, scale, _ = strmovie.encode_frame(test_picture(w, h, i, "noise"), budget)
                    wr.add_frame(bs, n)
                hard = wr.over_budget
                black = strmovie.encode_frame(test_picture(w, h, 0, "black"))[0]
                share = wr.video_rate() / fps
                catch_up = len(black) <= int(share) * strmovie.VIDEO_PAYLOAD
                for i in range(int(fps * 4)):       # (then simple pictures: the movie catches up if they fit)
                    n, budget = wr.frame_budget()
                    bs, scale, _ = strmovie.encode_frame(test_picture(w, h, i, "black"), budget)
                    wr.add_frame(bs, n)
                wr.close()
                st = strmovie.StrFile(path)
                self.assertEqual([], st.problems)
                vr = wr.video_rate()
                behind = wr.video_sectors - int(len(st.frames) * vr / fps)
                if catch_up:
                    self.assertEqual(0, behind, "back on time (%d frames were too big)" % hard)
                else:   # (even a black frame bigger than a frame's share: this size at this rate can't keep up)
                    self.assertGreater(behind, 0)
                for f, (scale, size, sectors) in zip(st.frames, wr.stats):
                    self.assertLessEqual(size, sectors * strmovie.VIDEO_PAYLOAD)
                    self.assertEqual(sectors, f["sectors"])

    def test_pace(self):
        """Each frame's last sector (when the console shows it) at its time: frame f at f / fps (+ a constant)."""
        for speed in (1, 2):
            for fps in (10, 12, 15, 20, 23.976, 24, 25, 29.97, 30):
                for audio in (None, (18900, False), (37800, False), (18900, True), (37800, True)):
                    with self.subTest(speed=speed, fps=fps, audio=audio):
                        path = os.path.join(TMP, "pace.str")
                        a = (beep_sound(4, audio[0], audio[1]), audio[0], audio[1]) if audio else None
                        wr = strmovie.StrWriter(path, 16, 16, fps, speed, a)
                        frame = strmovie.encode_frame(np.zeros((16, 16, 3), np.uint8))[0]
                        frames = int(fps * 8)
                        for _ in range(frames):
                            n, _ = wr.frame_budget()
                            wr.add_frame(frame, n)
                        wr.close()
                        st = strmovie.StrFile(path)
                        self.assertEqual(frames, len(st.frames))
                        t = [(f["last"] + 1) / (75.0 * speed) for f in st.frames]
                        off = [t[i] - i / fps for i in range(len(t))]
                        sector = 1 / (75.0 * speed)
                        self.assertLess(max(off) - min(off), 2 * sector + 1e-6,
                                        "frames off their time by %.1f to %.1f ms" % (min(off) * 1e3, max(off) * 1e3))

    def test_sectors(self):
        """EDC / ECC of every sector, subheaders, the interleave, the frame headers, the end of the file."""
        for rate, stereo, speed in ((37800, True, 2), (18900, False, 1), (37800, False, 2), (18900, True, 2)):
            with self.subTest(rate=rate, stereo=stereo, speed=speed):
                path, stats = make_movie("sec_%d_%d_%d.str" % (rate, stereo, speed), w=160, h=112, seconds=2,
                                         rate=rate, stereo=stereo, speed=speed)
                with open(path, "rb") as f:
                    d = f.read()
                self.assertEqual(0, len(d) % 2336)
                n = len(d) // 2336
                bad = []
                audio, video = [], []
                for i in range(n):
                    s = d[i * 2336:(i + 1) * 2336]
                    p = check_sector(s)
                    if p:
                        bad.append((i, p))
                    sub = s[0:4]
                    self.assertEqual((1, 1), (sub[0], sub[1]), "file and channel")
                    if sub[2] & 0x04:
                        audio.append(i)
                        self.assertEqual(0x64, sub[2] & 0x7E, "audio: form 2, real time, audio")
                        self.assertEqual((1 if stereo else 0) | (4 if rate == 18900 else 0), sub[3], "coding")
                    else:
                        video.append(i)
                        self.assertEqual(0x48, sub[2] & 0x7E, "video: data, real time")
                    if i == n - 1:
                        self.assertEqual(0x81, sub[2] & 0x81, "the last sector: end of record and file")
                self.assertEqual([], bad[:5])
                gaps = {b - a for a, b in zip(audio, audio[1:])}
                self.assertEqual(1, len(gaps), "a sound sector every N sectors: %s" % gaps)
                self.assertEqual(strmovie.xa_interleave(rate, stereo, speed), gaps.pop())
                st = strmovie.StrFile(path)
                self.assertEqual(list(range(1, len(stats) + 1)), [f["number"] for f in st.frames])
                for f, (scale, size, sectors) in zip(st.frames, stats):
                    self.assertEqual((size, sectors, 160, 112, scale, 2), (f["size"], f["sectors"], f["w"], f["h"],
                                                                           f["scale"], f["version"]))

    def test_sound(self):
        """XA: FFmpeg decodes what the writer encoded; the writer's decoder agrees; the sound's length."""
        need_ffmpeg(self)
        for rate in (37800, 18900):
            for stereo in (True, False):
                with self.subTest(rate=rate, stereo=stereo):
                    t = np.arange(rate * 3) / rate
                    tone = np.sin(2 * np.pi * 440 * t) * 12000 + np.sin(2 * np.pi * 1250 * t) * 6000
                    src = (np.stack([tone, -tone], 1) if stereo else tone).astype(np.int16)
                    path = os.path.join(TMP, "xa.str")
                    wr = strmovie.StrWriter(path, 16, 16, 15, 2, (src, rate, stereo))
                    frame = strmovie.encode_frame(np.zeros((16, 16, 3), np.uint8))[0]
                    for _ in range(45):
                        wr.add_frame(frame, wr.frame_budget()[0])
                    wr.close()
                    ff = ffmpeg_audio(to_raw(path, path + ".2352"), stereo)
                    own = strmovie.StrFile(path).decode_audio()
                    self.assertGreaterEqual(len(ff), len(src) - 4032)
                    self.assertGreater(snr_db(own, ff), 40, "the writer's XA decoding and FFmpeg's agree")
                    self.assertGreater(snr_db(src, ff), 20, "the sound kept")

    def test_reader(self):
        path, _ = make_movie("rd.str", w=160, h=112, seconds=2)
        st = strmovie.StrFile(path)
        raw = strmovie.StrFile(to_raw(path, os.path.join(TMP, "rd_raw.str")))
        riff = strmovie.StrFile(to_raw(path, os.path.join(TMP, "rd_riff.str"), riff=True))
        video, _ = make_movie("rd_2048.str", w=160, h=112, seconds=2, fmt=2048)
        v = strmovie.StrFile(video)
        for other in (raw, riff):
            self.assertEqual([f["size"] for f in st.frames], [f["size"] for f in other.frames])
            self.assertEqual(st.audio_info(), other.audio_info())
            self.assertTrue(np.array_equal(st.decode_audio(), other.decode_audio()))
        self.assertEqual(len(st.frames), len(v.frames))
        self.assertIsNone(v.audio_info())

    def test_reader_damaged(self):
        """A damaged file: read with its problems listed, or ValueError (what the page shows), nothing else."""
        path, _ = make_movie("rdd.str", w=160, h=112, seconds=2)
        with open(path, "rb") as f:
            data = f.read()
        rng = random.Random(3)
        failures = []
        for k in range(150):
            d = bytearray(data)
            if k % 3 == 0:
                d = d[:rng.randrange(1, len(d))]
            for _ in range(rng.randint(1, 30)):
                if len(d):
                    d[rng.randrange(len(d))] = rng.randrange(256)
            p = os.path.join(TMP, "rdd_case.str")
            with open(p, "wb") as f:
                f.write(d)
            try:
                st = strmovie.StrFile(p)
                for n in range(len(st.frames)):
                    try:
                        st.decode_frame(n)
                    except ValueError:
                        pass
                if st.audio_info():
                    st.decode_audio()
            except ValueError:
                pass
            except Exception as e:      # noqa: BLE001
                failures.append("case %d: %s: %s" % (k, type(e).__name__, e))
        self.assertEqual([], failures[:8])


# ---------------------------------------------------------------------------------------------------- conversions

def make_source_video(path, seconds=6, fps=30, w=640, h=480, sound=True):
    """A test video (MP4): a gradient that moves, white at the first frame of each second; a beep then."""
    wav = path + ".wav"
    if sound:
        s = beep_sound(seconds, 48000, True)
        with wave.open(wav, "wb") as f:
            f.setnchannels(2)
            f.setsampwidth(2)
            f.setframerate(48000)
            f.writeframes(s.tobytes())
    cmd = [ffmpeg(), "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (w, h), "-r", str(fps),
           "-i", "-"]
    if sound:
        cmd += ["-i", wav]
    cmd += ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-g", "15"]
    if sound:
        cmd += ["-c:a", "aac", "-b:a", "192k"]
    cmd.append(path)
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    for i in range(seconds * fps):
        img = test_picture(w, h, i)
        if i % fps < 3:                 # (0.1 s: a frame of it at any frame rate made of it)
            img = np.full_like(img, 255)
        p.stdin.write(img.tobytes())
    p.stdin.close()
    err = p.stderr.read()
    if p.wait():
        raise RuntimeError(err.decode(errors="replace"))
    return path


def convert(opts):
    """movieconv.Job run to its end: (error, result, job)."""
    out = {}
    ev = threading.Event()

    def done(err, res):
        out["err"], out["res"] = err, res
        ev.set()
    job = movieconv.Job(opts, lambda *a: None, done)
    if not ev.wait(600):
        job.stop()
        raise RuntimeError("the conversion didn't end")
    return out["err"], out["res"], job


class Conversions(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        if not ffmpeg():
            raise unittest.SkipTest("no FFmpeg")
        os.makedirs(TMP, exist_ok=True)
        cls.video = make_source_video(os.path.join(TMP, "source.mp4"))
        cls.silent = make_source_video(os.path.join(TMP, "silent.mp4"), sound=False, seconds=3)

    def opts(self, **kw):
        o = dict(source="video", path=self.video, w=320, h=240, fps=15, aspect="4:3", fit="fit", speed=2, fmt=2336,
                 quality="fit", scale=1, sound="video", sound_path="", rate=37800, stereo=True, gain=1.0, start=0,
                 duration=0, seconds=2, fade=0.5, out=os.path.join(TMP, "conv.str"), ffmpeg=ffmpeg())
        o.update(kw)
        return o

    CASES = [
        ("defaults", {}),
        ("1x 18900 mono", dict(speed=1, rate=18900, stereo=False)),
        ("2x 18900 stereo 30 fps", dict(rate=18900, fps=30)),
        ("PAL 320x256 25 fps", dict(h=256, fps=25)),
        ("640x240", dict(w=640, h=240, fps=15)),
        ("160x112 crop", dict(w=160, h=112, fit="crop")),
        ("256x224 stretch 16:9", dict(w=256, h=224, fit="stretch", aspect="16:9")),
        ("fixed quality", dict(quality="fixed", scale=4)),
        ("trimmed 1.5 s + 3 s", dict(start=1.5, duration=3)),
        ("no sound", dict(sound="none")),
        ("video only, 2048", dict(fmt=2048)),
        ("24 fps", dict(fps=24)),
        ("29.97 fps", dict(fps=29.97)),
    ]

    def test_conversions(self):
        for name, kw in self.CASES:
            with self.subTest(case=name):
                o = self.opts(**kw)
                err, res, _ = convert(o)
                self.assertIsNone(err)
                length = o["duration"] or (6 - o["start"])
                self.assertLessEqual(abs(res["frames"] - length * o["fps"]), 1.5, "frames: %d" % res["frames"])
                st = strmovie.StrFile(o["out"])
                self.assertEqual([], st.problems)
                self.assertEqual(res["frames"], len(st.frames))
                for f, (scale, size, sectors) in zip(st.frames, res["stats"]):
                    self.assertLessEqual(size, sectors * strmovie.VIDEO_PAYLOAD)
                self.assertEqual((o["w"], o["h"]), (st.frames[0]["w"], st.frames[0]["h"]))
                if o["fmt"] == 2048 or o["sound"] == "none":
                    self.assertIsNone(st.audio_info())
                    continue
                self.assertEqual((o["rate"], o["stereo"]), st.audio_info())
                raw = to_raw(o["out"], o["out"] + ".2352")
                frames = ffmpeg_frames(raw, o["w"], o["h"])
                self.assertEqual(len(st.frames), len(frames), "FFmpeg decodes every frame")
                pcm = ffmpeg_audio(raw, o["stereo"])
                self.assertGreater(len(pcm) / o["rate"], length - 0.1, "the sound's length")
                # The flash (white frame) and the beep of each second: together in the STR
                bright = [i for i, f in enumerate(frames) if f.mean() > 200]
                heard = beeps(pcm, o["rate"])
                flashes = [i / o["fps"] for i in bright if i == 0 or (i - 1) not in bright]
                self.assertGreaterEqual(len(flashes), int(length) - 1, "the flashes: %s" % flashes)
                pairs = [(f, min(heard, key=lambda b: abs(b - f))) for f in flashes if heard]
                self.assertTrue(all(abs(f - b) <= 1.0 / o["fps"] + 0.03 for f, b in pairs),
                                "flash / beep: %s" % ["%.3f/%.3f" % p for p in pairs])

    def test_sound_from_file(self):
        o = self.opts(path=self.silent, sound="file", sound_path=self.video + ".wav")
        err, res, _ = convert(o)
        self.assertIsNone(err)
        st = strmovie.StrFile(o["out"])
        self.assertEqual((37800, True), st.audio_info())
        self.assertGreater(len(st.decode_audio()) / 37800.0, 2.9)

    def test_slide_show(self):
        from PIL import Image
        pics = []
        for i, col in enumerate(((255, 0, 0), (0, 255, 0), (0, 0, 255))):
            p = os.path.join(TMP, "slide%d.png" % i)
            Image.new("RGB", (400 + i * 100, 300), col).save(p)
            pics.append(p)
        for seconds, fade in ((2, 0.5), (1, 0), (0, 0)):
            with self.subTest(seconds=seconds, fade=fade):
                o = self.opts(source="pictures", paths=pics, seconds=seconds, fade=fade, sound="none")
                err, res, _ = convert(o)
                self.assertIsNone(err)
                want = 3 * max(1, int(round(seconds * 15))) if seconds else 3
                self.assertEqual(want, res["frames"])
                st = strmovie.StrFile(o["out"])
                first = st.decode_frame(0).mean(axis=(0, 1))
                self.assertGreater(first[0], 150, "the first picture red: %s" % first)

    def test_stop(self):
        out = {}
        ev = threading.Event()
        job = movieconv.Job(self.opts(out=os.path.join(TMP, "stopped.str")), lambda *a: None,
                            lambda err, res: (out.update(err=err), ev.set()))
        job.stop()
        self.assertTrue(ev.wait(120))
        self.assertEqual("stopped", out["err"])


if __name__ == "__main__":
    unittest.main()
