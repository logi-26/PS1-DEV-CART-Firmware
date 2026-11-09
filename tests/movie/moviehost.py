"""
Helpers of the movie tests: test movies made with strmovie (a picture that changes each frame, a beep at the start of
each second), the console's movie tester run on the PC (vthost, build_host.py), FFmpeg as an independent decoder of
STR files (its psxstr demuxer, MDEC and XA decoders), the picture / sound synchronisation measured.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import struct
import subprocess
import sys
import wave
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "gui"))

import build_host                       # noqa: E402
import movieconv                        # noqa: E402
import strmovie                         # noqa: E402

TMP = os.path.join(HERE, "build", "tmp")
NTSC_VSYNC_US, PAL_VSYNC_US = 16715, 20096
BEEP_HZ, BEEP_S = 1000, 0.05


def ffmpeg():
    return movieconv.find_ffmpeg()


# ---------------------------------------------------------------------------------------------------- test movies

def test_picture(w, h, i, kind="gradient"):
    """A frame of a test movie (RGB h x w): it changes each frame, so a frame shown twice or out of order is seen."""
    yy, xx = np.mgrid[0:h, 0:w]
    if kind == "noise":
        rng = np.random.default_rng(i)
        return rng.integers(0, 256, (h, w, 3), dtype=np.uint8)
    if kind == "black":
        return np.zeros((h, w, 3), np.uint8)
    img = np.stack((xx * 255 // max(1, w - 1), (yy + i * 4) % 256, np.full_like(xx, (i * 16) % 256)), -1)
    return img.astype(np.uint8)


def beep_sound(seconds, rate, stereo, amp=20000):
    """A beep of BEEP_S at the start of each second (the picture / sound check), silence between."""
    t = np.arange(int(rate * seconds)) / rate
    s = ((t % 1.0) < BEEP_S) * np.sin(2 * np.pi * BEEP_HZ * t) * amp
    return np.stack([s, s], 1).astype(np.int16) if stereo else s.astype(np.int16)


def make_movie(name, w=320, h=240, fps=15, speed=2, seconds=4, rate=37800, stereo=True, fmt=2336, kind="gradient",
               sound=True, scale=None):
    """A test STR in TMP (made once for these parameters): its path and the writer's stats."""
    os.makedirs(TMP, exist_ok=True)
    path = os.path.join(TMP, name)
    audio = (beep_sound(seconds, rate, stereo), rate, stereo) if sound and fmt == 2336 else None
    wr = strmovie.StrWriter(path, w, h, fps, speed, audio, fmt)
    for i in range(int(round(fps * seconds))):
        n, budget = wr.frame_budget()
        bs, _, _ = strmovie.encode_frame(test_picture(w, h, i, kind), None if scale else budget, scale or 1)
        wr.add_frame(bs, n)
    wr.close()
    return path, wr.stats


def to_raw(src, dst, riff=False):
    """2336-byte sectors -> 2352 (sync + header, as a raw CD track: what FFmpeg's psxstr reads), in a RIFF CDXA file
    (riff: as Windows copies a CD track)."""
    with open(src, "rb") as f:
        d = f.read()
    out = bytearray()
    for i in range(len(d) // 2336):
        lba = i + 150
        msf = bytes(((lba // 4500) // 10 * 16 + (lba // 4500) % 10, (lba // 75 % 60) // 10 * 16 + (lba // 75 % 60) % 10,
                     (lba % 75) // 10 * 16 + (lba % 75) % 10))
        out += b"\x00" + b"\xff" * 10 + b"\x00" + msf + b"\x02" + d[i * 2336:(i + 1) * 2336]
    if riff:
        head = b"RIFF" + struct.pack("<I", len(out) + 36) + b"CDXAfmt " + struct.pack("<I", 16) + bytes(16)
        head += b"data" + struct.pack("<I", len(out))
        out = head + out
    with open(dst, "wb") as f:
        f.write(out)
    return dst


def ffmpeg_frames(path, w, h):
    """The frames FFmpeg decodes from an STR (2352-byte sectors): [RGB h x w]."""
    r = subprocess.run([ffmpeg(), "-v", "error", "-nostdin", "-f", "psxstr", "-i", path, "-map", "0:v:0",
                        "-pix_fmt", "rgb24", "-f", "rawvideo", "-"], capture_output=True)
    if r.returncode:
        raise RuntimeError(r.stderr.decode(errors="replace"))
    n = len(r.stdout) // (w * h * 3)
    return list(np.frombuffer(r.stdout[:n * w * h * 3], np.uint8).reshape(n, h, w, 3))


def ffmpeg_audio(path, stereo):
    """The XA sound FFmpeg decodes from an STR (2352-byte sectors): int16 (n, 2) or (n,), and its rate."""
    r = subprocess.run([ffmpeg(), "-v", "error", "-nostdin", "-f", "psxstr", "-i", path, "-map", "0:a:0",
                        "-f", "s16le", "-"], capture_output=True)
    if r.returncode:
        raise RuntimeError(r.stderr.decode(errors="replace"))
    pcm = np.frombuffer(r.stdout, np.int16)
    return pcm.reshape(-1, 2) if stereo else pcm


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 99.0 if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)


def snr_db(ref, test):
    n = min(len(ref), len(test))
    ref, test = ref[:n].astype(np.float64), test[:n].astype(np.float64)
    noise = np.sum((ref - test) ** 2)
    return 99.0 if noise == 0 else 10 * np.log10(np.sum(ref ** 2) / noise)


def crc(b):
    return zlib.crc32(b) & 0xFFFFFFFF


# ---------------------------------------------------------------------------------------------------- the tester

class Run:
    """What vthost printed: decodes [(vsync, frame, size, crc)], shows [(vsync, frame, play_us)], keyons, problems
    (buffer / VRAM overflows, refused frames, hang, crash), done dict, the tester's lines, the sound (wav path)."""

    def __init__(self, text, code, wav, vsync_us):
        self.raw, self.code, self.wav, self.vsync_us = text, code, wav, vsync_us
        self.decodes, self.shows, self.keyons, self.problems, self.lines, self.badbs = [], [], [], [], [], []
        self.done = None
        for line in text.splitlines():
            f = line.split()
            if not f:
                continue
            try:
                if f[0] == "DECODE":
                    self.decodes.append((int(f[1]), int(f[2]), int(f[3]), int(f[4], 16)))
                elif f[0] == "SHOW":
                    self.shows.append((int(f[1]), int(f[2]), int(f[3])))
                elif f[0] == "KEYON":
                    self.keyons.append(int(f[1]))
                elif f[0] in ("OVERFLOW", "VRAM", "HANG", "CRASH"):
                    self.problems.append(line)
                elif f[0] == "BADBS":
                    self.badbs.append(line)
                elif f[0] == "DONE":
                    k = ("vsync", "shown", "dropped", "late", "underruns", "restarts", "stalls", "errors", "ended")
                    self.done = dict(zip(k, (int(x) for x in f[1:10])))
                elif f[0] == "VIDTEST":
                    self.lines.append(line)
            except (ValueError, IndexError):
                pass
        if code not in (0, None) and not self.problems:
            self.problems.append("exit code %s" % code)
        if self.done is None and not self.problems:
            self.problems.append("no DONE line")

    def show_wall(self, frame, nth=0):
        """When (s, from the start) a frame was shown (its nth showing), None if never."""
        times = [s[0] for s in self.shows if s[1] == frame]
        return times[nth] * self.vsync_us / 1e6 if len(times) > nth else None

    def sound(self):
        with wave.open(self.wav) as w:
            d = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, 2)
            return d, w.getframerate()


def run(movie, commands, vsyncs=60 * 30, until_end=True, pal=False, decode_vsyncs=1, read_kbps=500, timeout=120,
        name=None):
    """The movie tester on the PC: commands [(vsync, "text")], e.g. (0, "open movie.str speed 2 mode auto loop 0")."""
    exe = build_host.build()
    wav = os.path.join(TMP, (name or os.path.basename(movie)) + ".out.wav")
    cmd = [exe, "--dir", os.path.dirname(movie), "--vsyncs", str(vsyncs), "--decode-vsyncs", str(decode_vsyncs),
           "--read-kbps", str(read_kbps), "--audio-out", wav]
    if until_end:
        cmd.append("--until-end")
    if pal:
        cmd.append("--pal")
    for at, text in commands:
        cmd += ["--cmd", "%d:%s" % (at, text)]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return Run(r.stdout, r.returncode, wav, PAL_VSYNC_US if pal else NTSC_VSYNC_US)
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        res = Run(out, None, wav, PAL_VSYNC_US if pal else NTSC_VSYNC_US)
        res.problems.append("TIMEOUT after %d s" % timeout)
        return res


def beeps(sound, rate, threshold=6000):
    """Start times (s) of the beeps in a sound (channel 0)."""
    env = np.abs(sound[:, 0].astype(np.int32)) if sound.ndim == 2 else np.abs(sound.astype(np.int32))
    on = np.nonzero(env > threshold)[0]
    if not len(on):
        return []
    starts = [on[0]] + [b for a, b in zip(on, on[1:]) if b - a > rate * 0.3]
    return [s / rate for s in starts]


def av_offsets(run_result, fps):
    """For each second's beep heard (its nth), the time from it to the second's first frame shown: [(second, s)]
    (positive: the picture later than the sound)."""
    sound, rate = run_result.sound()
    out = []
    for k, t in enumerate(beeps(sound, rate)):
        frame = int(round(k * fps)) + 1
        shown = run_result.show_wall(frame)
        if shown is not None:
            out.append((k, shown - t))
    return out
