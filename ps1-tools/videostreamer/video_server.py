"""
PS1 DEV CART video streamer server: streams videos to the PlayStation (videostreamer.ps-exe).

    pip install av numpy miniaudio
    python video_server.py [--port 7778] [--size 320x240] [--fps 15] [--kbps 90] [--rate 22050] [videos ...]
    ps1transfer -r -e -f videostreamer.ps-exe -net 127.0.0.1:7778

Each video (any file or http(s) stream FFmpeg can open, through PyAV) is decoded, its frames are letterboxed into
--size (320x240: full screen 1:1, 160x120: shown 2x, smaller and faster), picked at --fps, and encoded to MDEC BS v2 frames (mdec_encoder.py) within the --kbps budget (the quantization
scale goes up when a frame is too big); the sound is resampled to mono at --rate and encoded to SPU ADPCM (the net
radio's encoder). Everything is sent in time order, at most LEAD seconds ahead of real time. At the end of a video, the
next one starts. The PlayStation sends 'N' / 'P' (next / previous video, Left/Right on the controller).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""

import argparse
import math
import os
import socket
import struct
import sys
import threading
import time

import av
import numpy as np

import mdec_encoder

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "audiostreamer"))
from audio_server import AdpcmEncoder  # noqa: E402 (the audio streamer's SPU ADPCM encoder)

# Default videos: Sintel (trailer and film), Blender Foundation (https://durian.blender.org), Creative Commons
# Attribution 3.0. (source, name shown, credit shown)
VIDEOS = [
    ("https://download.blender.org/durian/trailer/Sintel_Trailer1.480p.DivX_Plus_HD.mkv",
     "SINTEL (TRAILER)", "(C) BLENDER FOUNDATION, CC BY 3.0"),
    ("https://download.blender.org/durian/movies/Sintel.2010.1080p.mkv",
     "SINTEL (FULL FILM, 15 MIN)", "(C) BLENDER FOUNDATION, CC BY 3.0"),
]

# videostreamer.h
VS_INFO, VS_FRAME, VS_AUDIO, VS_TEXT, VS_RESET, VS_ERROR = b"I", b"V", b"A", b"T", b"R", b"E"
VS_NEXT, VS_PREVIOUS = ord("N"), ord("P")
VS_MAX_TEXT = 120
VS_MAX_FRAME = 32768
BLOCK_SAMPLES = 28
AUDIO_PACKET = BLOCK_SAMPLES * 64       # Samples per audio packet (1 KB of ADPCM)
LEAD = 2.0                              # Seconds sent ahead of real time


def packet(kind, payload=b""):
    return kind + struct.pack("<H", len(payload)) + payload


def text(s):
    return s.upper().encode("ascii", "replace")[:VS_MAX_TEXT]


class Client:
    """One bridge connection: the player loop (video -> PS1) and a reader thread (commands from the PS1)."""

    def __init__(self, sock, videos, args, log=print):
        self.sock = sock
        self.log = log
        self.videos = videos
        self.args = args
        self.index = 0
        self.change = None
        self.alive = True
        self.sent_bytes = 0

    def send(self, data):
        self.sock.sendall(data)
        self.sent_bytes += len(data)

    def reader(self):
        try:
            while self.alive:
                data = self.sock.recv(16)
                if not data:
                    break
                for c in data:
                    if c in (VS_NEXT, VS_PREVIOUS):
                        step = 1 if c == VS_NEXT else -1
                        self.change = (self.index + step) % len(self.videos)
                        self.log("PS1: %s video" % ("next" if step > 0 else "previous"))
        except OSError:
            pass
        self.alive = False

    def play(self):
        source, name, credit = self.videos[self.index]
        args = self.args
        self.log("Video %d/%d: %s" % (self.index + 1, len(self.videos), source))
        try:
            container = av.open(source, timeout=10)
        except Exception as e:      # noqa: BLE001 (shown on the PS1)
            self.log("  Can't open it: %s" % e)
            self.send(packet(VS_ERROR, text("VIDEO NOT AVAILABLE")))
            self.wait_change(5)
            return
        vstream = container.streams.video[0]
        astream = container.streams.audio[0] if container.streams.audio else None
        vstream.thread_type = "AUTO"
        # Letterboxed into 160x120, even sizes
        sw, sh = vstream.codec_context.width, vstream.codec_context.height
        aspect = sw / sh if sh else 4 / 3
        src_fps = float(vstream.average_rate or vstream.guessed_rate or args.fps)
        # Frame rate: the source's divided by a whole number, at most --fps (every n-th frame: even steps, e.g.
        # 24 -> 12, 30 -> 15; 15 out of 24 would skip unevenly)
        fps = src_fps / math.ceil(src_fps / args.fps - 0.01)
        can_w, can_h = args.size
        # The picture fitted in the canvas (aspect kept); only the picture is sent (the PS1 centers it: the letterbox
        # bars of a wide film would cost the MDEC as much time as the picture)
        w, h = (can_w, int(can_w / aspect) & ~1) if aspect > can_w / can_h else (int(can_h * aspect) & ~1, can_h)
        enc_w, enc_h = (w + 15) & ~15, (h + 15) & ~15             # Encoded: whole macroblocks
        x0, y0 = 0, 0
        self.send(packet(VS_RESET) + packet(VS_INFO, struct.pack("<HHHHHHHIBBB", can_w, can_h, w, h, enc_w, enc_h,
                                                                   int(round(fps * 100)), args.rate, 1, self.index,
                                                                   len(self.videos)) + text(name)))
        self.send(packet(VS_TEXT, text(credit or "")))
        self.log("  %dx%d -> %dx%d (encoded %dx%d), %s fps, sound %s" % (sw, sh, w, h, enc_w, enc_h, round(fps, 2),
                 astream.codec_context.name if astream else "none"))

        canvas = np.zeros((enc_h, enc_w, 3), np.uint8)
        frame_budget = min(VS_MAX_FRAME - 4, int(args.kbps * 1024 / fps))
        resampler = av.AudioResampler(format="s16", layout="mono", rate=args.rate)
        adpcm = AdpcmEncoder()
        audio = np.zeros(0, np.int16)
        audio_sent = 0                      # Samples
        last_frame = -1
        scale = 8
        start = time.monotonic()
        stats_t, stats_frames, stats_bytes, stats_scale = start, 0, 0, 0

        def pace(media_time):
            ahead = media_time - (time.monotonic() - start)
            if ahead > LEAD:
                time.sleep(ahead - LEAD)

        def send_audio(until_samples, flush=False):
            nonlocal audio, audio_sent
            # Silence where there is no sound (no sound track, or before it starts)
            missing = until_samples - (audio_sent + len(audio))
            if missing > 0 and (astream is None or flush):
                audio = np.concatenate((audio, np.zeros(missing, np.int16)))
            while len(audio) >= AUDIO_PACKET:
                self.send(packet(VS_AUDIO, adpcm.encode(audio[:AUDIO_PACKET])))
                audio = audio[AUDIO_PACKET:]
                audio_sent += AUDIO_PACKET

        try:
            streams = [vstream] + ([astream] if astream else [])
            for pkt in container.demux(*streams):
                if not self.alive or self.change is not None:
                    break
                for f in pkt.decode():
                    if f.time is None:
                        continue
                    if isinstance(f, av.AudioFrame):
                        for r in resampler.resample(f):
                            pcm = r.to_ndarray().reshape(-1).astype(np.int16)
                            # Sound starting after 0: silence before it
                            gap = int(f.time * args.rate) - (audio_sent + len(audio))
                            if gap > args.rate // 10 and audio_sent == 0 and len(audio) == 0:
                                audio = np.zeros(gap, np.int16)
                            audio = np.concatenate((audio, pcm))
                        send_audio(0)
                        pace(audio_sent / args.rate)
                        continue
                    n = int(f.time * fps + 0.5)
                    if n <= last_frame:
                        continue            # Not needed at this frame rate
                    last_frame = n
                    canvas[y0:y0 + h, x0:x0 + w] = f.to_ndarray(format="rgb24", width=w, height=h)
                    bs, scale = mdec_encoder.encode_frame_fit(canvas, frame_budget, max(2, scale - 1))
                    pace(n / fps)
                    if astream is None:
                        send_audio(int((n + 1) / fps * args.rate), flush=True)
                    self.send(packet(VS_FRAME, struct.pack("<I", n) + bs))
                    stats_frames += 1
                    stats_bytes += len(bs)
                    stats_scale += scale
                    if time.monotonic() - stats_t >= 5:
                        dt = time.monotonic() - stats_t
                        self.log("  frame %d: %.1f fps, %.1f KB/frame (scale %.0f), %.1f KB/s sent" % (
                            n, stats_frames / dt, stats_bytes / stats_frames / 1024, stats_scale / stats_frames,
                            self.sent_bytes / dt / 1024))
                        stats_t, stats_frames, stats_bytes, stats_scale = time.monotonic(), 0, 0, 0
                        self.sent_bytes = 0
            else:
                self.log("  End of the video.")
                if self.change is None:
                    self.change = (self.index + 1) % len(self.videos)
        except OSError:
            self.alive = False
        finally:
            container.close()

    def wait_change(self, seconds):
        end = time.monotonic() + seconds
        while self.alive and self.change is None and time.monotonic() < end:
            time.sleep(0.1)

    def run(self):
        threading.Thread(target=self.reader, daemon=True).start()
        try:
            while self.alive:
                if self.change is not None:
                    self.index, self.change = self.change, None
                self.play()
        except OSError:
            pass
        self.alive = False
        self.log("Bridge disconnected.")


def video_name(source):
    """Name shown on the PS1 for a file or URL: its file name without the extension."""
    return os.path.splitext(os.path.basename(source.split("?")[0]))[0].replace("_", " ")


class Server:
    """The server on 127.0.0.1 (port 0: any free port, see .port), one bridge at a time, in a thread: used by the
    command line (main) and by the PS1 Dev Cart Manager (GUI). stop() ends it."""

    def __init__(self, videos, args, port=0, log=print):
        self.videos = videos
        self.args = args
        self.log = log
        self.client = None
        self.stopped = False
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if os.name != "nt":     # (on Windows it lets a second server use the same port: "already in use" is better)
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(1)
        self.sock.settimeout(0.5)
        self.port = self.sock.getsockname()[1]
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        while not self.stopped:
            try:
                sock, addr = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            sock.settimeout(None)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.log("Bridge connected from %s:%d" % addr)
            self.client = Client(sock, self.videos, self.args, self.log)
            self.client.run()
            sock.close()
            self.client = None

    def stop(self):
        self.stopped = True
        if self.client:
            self.client.alive = False
        try:
            self.sock.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser(description="PS1 DEV CART video streamer server (for videostreamer.ps-exe)")
    ap.add_argument("videos", nargs="*", help="video files or URLs (default: the Sintel trailer and film)")
    ap.add_argument("--port", type=int, default=7778)
    ap.add_argument("--size", default="320x240", help="picture size, multiple of 16 wide (default 320x240)")
    ap.add_argument("--fps", type=float, default=15, help="most frames per second sent (default 15)")
    ap.add_argument("--kbps", type=float, default=90, help="video budget in KB/s (default 90)")
    ap.add_argument("--rate", type=int, default=22050, help="sound sample rate (default 22050)")
    args = ap.parse_args()
    args.size = tuple(int(v) for v in args.size.lower().split("x"))
    videos = [(v, video_name(v), "") for v in args.videos] or VIDEOS
    server = Server(videos, args, args.port)
    print("Video server on 127.0.0.1:%d, %d video(s). Waiting for the bridge:" % (server.port, len(videos)))
    print("  ps1transfer -r -e -f videostreamer.ps-exe -net 127.0.0.1:%d" % server.port)
    try:
        while server.thread.is_alive():
            server.thread.join(0.5)
    except KeyboardInterrupt:
        server.stop()


if __name__ == "__main__":
    main()
