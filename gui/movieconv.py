"""
PS1 DEV CART GUI: movie conversion for the Movie Tools page: a video file (FFmpeg) or pictures (TIM, PNG, JPG, BMP: a
slide show, a frame sequence) and a sound (from the video, a file, or none) made into an STR movie (strmovie.py).

FFmpeg is an outside program, run to read the videos and sounds (the settings' path, the PATH, or a copy the Movie
Tools page downloads: the "essentials" build of gyan.dev, GPL); WAV files, TIM and pictures don't need it.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""

import io
import os
import shutil
import subprocess
import sys
import threading
import urllib.request
import wave
import zipfile

import numpy as np

import strmovie

FFMPEG_URL = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip"
NO_WINDOW = 0x08000000 if sys.platform == "win32" else 0
VIDEO_TYPES = [("Videos", "*.mp4 *.mkv *.avi *.mov *.webm *.mpg *.mpeg *.wmv *.flv *.m4v *.gif *.str"),
               ("All files", "*.*")]
IMAGE_TYPES = [("Pictures", "*.tim *.png *.jpg *.jpeg *.bmp *.gif *.tga"), ("All files", "*.*")]
SOUND_TYPES = [("Sounds", "*.wav *.mp3 *.ogg *.flac *.aac *.m4a *.wma"), ("All files", "*.*")]

# Resolutions (width x height, multiples of 16) and what they are
RESOLUTIONS = [
    ("320x240", "320 x 240 (NTSC, the usual one)"), ("320x224", "320 x 224"), ("320x256", "320 x 256 (PAL)"),
    ("256x240", "256 x 240"), ("256x224", "256 x 224"), ("368x240", "368 x 240"), ("512x240", "512 x 240"),
    ("640x240", "640 x 240 (wide: drawn at half width by the tester)"), ("160x112", "160 x 112 (small, low rate)"),
]


def app_dir():
    base = os.environ.get("APPDATA") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "ps1devcart-gui")


def find_ffmpeg(setting=""):
    """FFmpeg's path: the setting, the PATH, or the copy downloaded here; "" if none."""
    if setting and os.path.isfile(setting):
        return setting
    found = shutil.which("ffmpeg")
    if found:
        return found
    own = os.path.join(app_dir(), "ffmpeg", "ffmpeg.exe" if sys.platform == "win32" else "ffmpeg")
    return own if os.path.isfile(own) else ""


def download_ffmpeg(progress=None):
    """Downloads FFmpeg (Windows: the essentials build of gyan.dev) next to the settings; its path."""
    dest = os.path.join(app_dir(), "ffmpeg")
    os.makedirs(dest, exist_ok=True)
    req = urllib.request.Request(FFMPEG_URL, headers={"User-Agent": "ps1devcart-gui"})
    with urllib.request.urlopen(req, timeout=60) as r:
        total = int(r.headers.get("Content-Length") or 0)
        data = bytearray()
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            data += chunk
            if progress:
                progress(len(data), total)
    with zipfile.ZipFile(io.BytesIO(bytes(data))) as z:
        for name in z.namelist():
            base = os.path.basename(name)
            if base in ("ffmpeg.exe", "ffprobe.exe") or name.endswith(("/LICENSE", "/README.txt")):
                with z.open(name) as src, open(os.path.join(dest, base or "LICENSE"), "wb") as out:
                    shutil.copyfileobj(src, out)
    return os.path.join(dest, "ffmpeg.exe")


def probe(ffmpeg, path):
    """(duration in seconds or 0, frame rate or 0, has sound) of a video, from FFmpeg's description."""
    r = subprocess.run([ffmpeg, "-hide_banner", "-i", path], capture_output=True, text=True, errors="replace",
                       creationflags=NO_WINDOW)
    dur, fps, sound = 0.0, 0.0, False
    for line in r.stderr.splitlines():
        line = line.strip()
        if line.startswith("Duration:"):
            t = line.split()[1].rstrip(",")
            try:
                h, m, s = t.split(":")
                dur = int(h) * 3600 + int(m) * 60 + float(s)
            except ValueError:
                pass
        if "Video:" in line and " fps" in line:
            try:
                fps = float(line.split(" fps")[0].split()[-1])
            except ValueError:
                pass
        if "Audio:" in line:
            sound = True
    return dur, fps, sound


def video_filter(w, h, fps, aspect, fit):
    """FFmpeg filter: the frames at fps, w x h, the picture shown at the display aspect (4:3 or 16:9 TV, the PS1's
    pixels aren't square: e.g. 256 x 240 is shown 4:3 too): letterboxed (fit), cropped (fill) or stretched."""
    dar = 16 / 9 if aspect == "16:9" else 4 / 3
    bw = int(round(h * dar / 2)) * 2       # The display box in square pixels
    chain = ["fps=%g" % fps, "scale=trunc(iw*sar/2)*2:ih", "setsar=1"]
    if fit == "fit":
        chain += ["scale=%d:%d:force_original_aspect_ratio=decrease" % (bw, h),
                  "pad=%d:%d:(ow-iw)/2:(oh-ih)/2" % (bw, h)]
    elif fit == "crop":
        chain += ["scale=%d:%d:force_original_aspect_ratio=increase" % (bw, h), "crop=%d:%d" % (bw, h)]
    chain += ["scale=%d:%d" % (w, h), "setsar=1"]
    return ",".join(chain)


def video_frames(ffmpeg, path, w, h, fps, aspect, fit, start=0.0, duration=0.0, stop=None):
    """The frames of a video (RGB numpy arrays h x w x 3), read from FFmpeg."""
    cmd = [ffmpeg, "-v", "error", "-nostdin"]
    if start:
        cmd += ["-ss", "%g" % start]
    cmd += ["-i", path]
    if duration:
        cmd += ["-t", "%g" % duration]
    cmd += ["-an", "-vf", video_filter(w, h, fps, aspect, fit), "-pix_fmt", "rgb24", "-f", "rawvideo", "-"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=NO_WINDOW)
    size = w * h * 3
    try:
        while not (stop and stop.is_set()):
            data = p.stdout.read(size)
            if len(data) < size:
                break
            yield np.frombuffer(data, np.uint8).reshape(h, w, 3)
    finally:
        p.kill()
        err = p.stderr.read().decode(errors="replace").strip()
        p.wait()
    if err and "Error" in err:
        raise RuntimeError(err.splitlines()[-1])


def load_picture(path):
    """A picture as a PIL RGB image (TIM: its first CLUT row)."""
    from PIL import Image
    if path.lower().endswith(".tim"):
        from pages.tim import Tim
        return Tim(path).image(0).convert("RGB")
    return Image.open(path).convert("RGB")


def fit_picture(img, w, h, aspect, fit):
    """A PIL picture made w x h as video_filter does (display aspect, letterbox / crop / stretch): numpy RGB."""
    from PIL import Image
    if fit != "stretch":
        dar = 16 / 9 if aspect == "16:9" else 4 / 3
        bw = int(round(h * dar))
        sx, sy = bw / img.width, h / img.height
        s = min(sx, sy) if fit == "fit" else max(sx, sy)
        nw, nh = max(1, int(round(img.width * s))), max(1, int(round(img.height * s)))
        scaled = img.resize((nw, nh), Image.LANCZOS)
        box = Image.new("RGB", (bw, h), (0, 0, 0))
        box.paste(scaled, ((bw - nw) // 2, (h - nh) // 2))
        img = box
    return np.asarray(img.resize((w, h), Image.LANCZOS), np.uint8)


def picture_frames(paths, w, h, fps, aspect, fit, seconds, fade, stop=None):
    """Frames of a slide show: each picture for "seconds", a cross-fade of "fade" seconds into the next one
    (seconds 0: one frame each, a frame sequence)."""
    pics = [fit_picture(load_picture(p), w, h, aspect, fit) for p in paths]
    hold = max(1, int(round(seconds * fps))) if seconds else 1
    fade_n = min(hold - 1, int(round(fade * fps))) if seconds and fade else 0
    for i, pic in enumerate(pics):
        nxt = pics[i + 1] if i + 1 < len(pics) else None
        for k in range(hold):
            if stop and stop.is_set():
                return
            if nxt is not None and fade_n and k >= hold - fade_n:
                a = (k - (hold - fade_n) + 1) / (fade_n + 1)
                yield (pic * (1 - a) + nxt * a).astype(np.uint8)
            else:
                yield pic


def load_sound(path, rate, stereo, ffmpeg="", start=0.0, duration=0.0, gain=1.0):
    """A sound (a WAV, else through FFmpeg) at the XA rate, mono or stereo: int16 numpy ((n, 2) or (n,))."""
    channels = 2 if stereo else 1
    if ffmpeg and not path.lower().endswith(".wav") or (ffmpeg and (start or duration)):
        cmd = [ffmpeg, "-v", "error", "-nostdin"]
        if start:
            cmd += ["-ss", "%g" % start]
        cmd += ["-i", path]
        if duration:
            cmd += ["-t", "%g" % duration]
        cmd += ["-vn", "-ac", str(channels), "-ar", str(rate), "-f", "s16le", "-"]
        r = subprocess.run(cmd, capture_output=True, creationflags=NO_WINDOW)
        if r.returncode:
            raise RuntimeError(r.stderr.decode(errors="replace").strip().splitlines()[-1:] or "FFmpeg failed")
        pcm = np.frombuffer(r.stdout, np.int16).astype(np.float32)
    else:
        if not path.lower().endswith(".wav"):
            raise RuntimeError("this sound needs FFmpeg (WAV files don't)")
        with wave.open(path, "rb") as wf:
            src_rate, n_ch, width = wf.getframerate(), wf.getnchannels(), wf.getsampwidth()
            raw = wf.readframes(wf.getnframes())
        if width == 2:
            data = np.frombuffer(raw, np.int16).astype(np.float32)
        elif width == 1:
            data = (np.frombuffer(raw, np.uint8).astype(np.float32) - 128) * 256
        elif width == 3:
            b = np.frombuffer(raw, np.uint8).reshape(-1, 3)
            data = ((b[:, 2].astype(np.int32) << 24 | b[:, 1].astype(np.int32) << 16 | b[:, 0].astype(np.int32) << 8)
                    >> 16).astype(np.float32)
        else:
            data = (np.frombuffer(raw, np.int32) >> 16).astype(np.float32)
        data = data.reshape(-1, n_ch)
        if start or duration:
            a = int(start * src_rate)
            data = data[a:a + int(duration * src_rate)] if duration else data[a:]
        data = data.mean(axis=1, keepdims=True) if channels == 1 or n_ch == 1 else data[:, :2]
        if channels == 2 and data.shape[1] == 1:
            data = np.repeat(data, 2, axis=1)
        if src_rate != rate and len(data):      # (linear interpolation: enough for a test)
            n = int(len(data) * rate / src_rate)
            x = np.arange(n) * (src_rate / rate)
            data = np.stack([np.interp(x, np.arange(len(data)), data[:, c]) for c in range(data.shape[1])], axis=1)
        pcm = data.reshape(-1)
    pcm = np.clip(pcm * gain, -32768, 32767).astype(np.int16)
    return pcm.reshape(-1, 2) if stereo else pcm


class Job:
    """A conversion in a thread: progress(done, total, text), done(error or None, result); stop() cancels it.
    opts: source ("video" / "pictures"), path or paths, w, h, fps, aspect, fit, speed, fmt, quality ("fit" / "fixed"),
    scale (fixed, or the finest of fit), sound ("video" / "file" / "none"), sound_path, rate, stereo, gain, start,
    duration, seconds, fade, out, ffmpeg."""

    def __init__(self, opts, progress, done):
        self.opts, self.progress, self.done = opts, progress, done
        self.stop_event = threading.Event()
        self.previews = []      # JPEG bytes of the frames as the MDEC decodes them
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def stop(self):
        self.stop_event.set()

    def _run(self):
        try:
            self.done(None, self._convert())
        except Exception as e:      # noqa: BLE001 (shown on the page)
            self.done(str(e), None)

    def _convert(self):
        from PIL import Image
        o = self.opts
        w, h, fps = o["w"], o["h"], o["fps"]
        ffmpeg = o.get("ffmpeg", "")
        sound = None
        if o["sound"] != "none" and o["fmt"] == 2336:
            src = o["path"] if o["sound"] == "video" else o["sound_path"]
            self.progress(0, 1, "Sound: %s" % os.path.basename(src))
            pcm = load_sound(src, o["rate"], o["stereo"], ffmpeg, o.get("start", 0) if o["sound"] == "video" else 0,
                             o.get("duration", 0) if o["sound"] == "video" else 0, o.get("gain", 1.0))
            sound = (pcm, o["rate"], o["stereo"])
        if o["source"] == "video":
            dur, _, _ = probe(ffmpeg, o["path"])
            length = o.get("duration") or max(0.0, dur - o.get("start", 0))
            total = int(length * fps) or 1
            frames = video_frames(ffmpeg, o["path"], w, h, fps, o["aspect"], o["fit"], o.get("start", 0),
                                  o.get("duration", 0), self.stop_event)
        else:
            n = len(o["paths"])
            total = n * max(1, int(round(o["seconds"] * fps))) if o["seconds"] else n
            frames = picture_frames(o["paths"], w, h, fps, o["aspect"], o["fit"], o["seconds"], o["fade"],
                                    self.stop_event)
        self.progress(0, total, "Encoding the sound (XA ADPCM)..." if sound else "Encoding...")
        writer = strmovie.StrWriter(o["out"], w, h, fps, o["speed"], sound, o["fmt"])
        over = 0
        for i, rgb in enumerate(frames):
            if self.stop_event.is_set():
                break
            n, budget = writer.frame_budget()
            if o["quality"] == "fixed":
                bs, scale, q = strmovie.encode_frame(rgb, None, o["scale"])
                if len(bs) > budget:        # (doesn't fit at that scale: the finest that does)
                    over += 1
                    bs, scale, q = strmovie.encode_frame(rgb, budget, o["scale"])
            else:
                bs, scale, q = strmovie.encode_frame(rgb, budget, o["scale"])
            writer.add_frame(bs, n)
            img = Image.fromarray(strmovie.dequantize(q, scale, w, h))
            buf = io.BytesIO()
            img.save(buf, "JPEG", quality=85)
            self.previews.append(buf.getvalue())
            if i % 2 == 0 or i + 1 == total:
                self.progress(i + 1, max(total, i + 1), "Frame %d / %d: %d bytes, quantization %d" % (
                    i + 1, max(total, i + 1), len(bs), scale))
        sectors = writer.close()
        if self.stop_event.is_set():
            raise RuntimeError("stopped")
        stats = writer.stats
        return {"sectors": sectors, "frames": len(stats), "stats": stats, "over": over, "interleave": writer.interleave,
                "seconds": sectors / (75.0 * o["speed"]), "bytes": os.path.getsize(o["out"])}
