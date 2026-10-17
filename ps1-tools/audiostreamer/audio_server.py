"""
PS1 DEV CART audio streamer server: streams internet radio stations and audio files to the PlayStation
(audiostreamer.ps-exe).

    pip install miniaudio numpy av
    python audio_server.py [--port 7777] [--rate 22050] [stations (links) or audio files ...]
    ps1transfer -r -e -f audiostreamer.ps-exe -net 127.0.0.1:7777

Links of Icecast/SHOUTcast stations (http MP3 streams) are played with miniaudio, which gives the song titles of the
stream (ICY metadata); local files of any format FFmpeg reads (MP3, FLAC, WAV, OGG, M4A/AAC, ...), and links in other
formats, with PyAV (the file's title tags are shown). The sound is resampled to mono at --rate, encoded to SPU ADPCM
(the PlayStation's sound format, 16 bytes per 28 samples: 12.6 KB/s at 22050 Hz) and sent to the bridge
("ps1transfer -net") in the packets of audiostreamer.h, at most LEAD seconds ahead of real time (a change is heard
quickly). At the end of a file the next one starts; a station that stops is opened again. The PlayStation sends 'N' /
'P' (next / previous, Left/Right on the controller). The PS1 Dev Cart Manager's Audio streamer page uses Server.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""

import argparse
import os
import socket
import struct
import threading
import time

import miniaudio
import numpy as np

# UK stations of Global, with http MP3 streams (name shown, link). Bauer's stations (e.g. Absolute 80s, Rock FM) only
# have AAC streams that need a session key of their player.
STATIONS = [
    ("SMOOTH", "http://media-ice.musicradio.com/SmoothUKMP3"),
    ("CAPITAL LONDON", "http://media-ice.musicradio.com/CapitalMP3"),
    ("HEART", "http://media-ice.musicradio.com/HeartUKMP3"),
    ("CLASSIC FM", "http://media-ice.musicradio.com/ClassicFMMP3"),
    ("HEART 80S", "http://media-ice.musicradio.com/Heart80sMP3"),
    ("RADIO X CLASSIC ROCK", "http://media-ice.musicradio.com/RadioXClassicRockMP3"),
]

# audiostreamer.h
AS_INFO, AS_TITLE, AS_AUDIO, AS_RESET, AS_ERROR = b"I", b"T", b"A", b"R", b"E"
AS_NEXT, AS_PREVIOUS = ord("N"), ord("P")
AS_MAX_TEXT = 120
BLOCK_SAMPLES = 28
BLOCKS_PER_PACKET = 64          # 1 KB of audio per packet
LEAD = 2.5                      # Seconds sent ahead of real time
FILTERS = ((0, 0), (60, 0), (115, -52), (98, -55), (122, -60))   # SPU ADPCM predictors (/ 64)
GAIN = 0.9                      # A little headroom for the 4-bit encoding


class AdpcmEncoder:
    """SPU ADPCM, block after block (the decoder history is kept between calls). The filter and shift of a block are
    chosen on the input (numpy, all the blocks at once), then the block is encoded decoding like the SPU, so the error
    doesn't build up. Fast enough for live audio (pure Python with all the filter/shift tries was not)."""

    def __init__(self):
        self.s1 = self.s2 = 0          # Decoded history (what the SPU has)
        self.x1 = self.x2 = 0          # Input history (for the filter choice)

    def encode(self, samples):
        """samples: int16 numpy array, length multiple of 28. Returns the ADPCM bytes (flags 0)."""
        x = samples.astype(np.int32)
        blocks = x.reshape(-1, BLOCK_SAMPLES)
        prev1 = np.concatenate(([self.x1], x[:-1])).reshape(-1, BLOCK_SAMPLES)
        prev2 = np.concatenate(([self.x2, self.x1], x[:-2])).reshape(-1, BLOCK_SAMPLES)
        peaks = np.stack([np.abs(blocks - ((prev1 * f0 + prev2 * f1 + 32) >> 6)).max(axis=1) for f0, f1 in FILTERS])
        best_filter = peaks.argmin(axis=0)
        best_peak = peaks.min(axis=0)
        self.x1, self.x2 = int(x[-1]), int(x[-2])
        out = bytearray()
        s1, s2 = self.s1, self.s2
        for block, f, peak in zip(blocks.tolist(), best_filter.tolist(), best_peak.tolist()):
            shift = 12
            while shift > 0 and peak > (7 << (12 - shift)):
                shift -= 1
            f0, f1 = FILTERS[f]
            nibbles = []
            for v in block:
                predicted = (s1 * f0 + s2 * f1 + 32) >> 6
                n = ((v - predicted) * (1 << shift) + 2048) >> 12
                n = -8 if n < -8 else (7 if n > 7 else n)
                decoded = ((n << 12) >> shift) + predicted
                decoded = -32768 if decoded < -32768 else (32767 if decoded > 32767 else decoded)
                s2, s1 = s1, decoded
                nibbles.append(n & 15)
            out += bytes(((f << 4) | shift, 0)) + bytes(nibbles[i] | (nibbles[i + 1] << 4) for i in range(0, 28, 2))
        self.s1, self.s2 = s1, s2
        return bytes(out)


def packet(kind, payload=b""):
    return kind + struct.pack("<H", len(payload)) + payload


def text(s):
    # The PlayStation font has no lower case, accents are dropped
    return s.upper().encode("ascii", "replace")[:AS_MAX_TEXT]


def is_link(source):
    return "://" in source


def file_name(source):
    """Name of a file or link: its file name without the extension."""
    return os.path.splitext(os.path.basename(source.split("?")[0]))[0].replace("_", " ")


class Client:
    """One bridge connection: the player loop (playlist -> PS1) and a reader thread (commands from the PS1)."""

    def __init__(self, sock, items, args, log=print):
        self.sock = sock
        self.items = items          # [(name, source), ...]
        self.args = args
        self.log = log
        self.index = 0
        self.change = None          # Other item asked by the PS1, or next one at the end of a file
        self.alive = True
        self.lock = threading.Lock()

    def send(self, data):
        with self.lock:
            self.sock.sendall(data)

    def reader(self):
        try:
            while self.alive:
                data = self.sock.recv(16)
                if not data:
                    break
                for c in data:
                    if c in (AS_NEXT, AS_PREVIOUS):
                        step = 1 if c == AS_NEXT else -1
                        self.change = (self.index + step) % len(self.items)
                        self.log("PS1: %s" % ("next" if step > 0 else "previous"))
        except OSError:
            pass
        self.alive = False

    def title(self, title):
        self.log("  Now playing: %s" % title)
        try:
            self.send(packet(AS_TITLE, text(title)))
        except OSError:
            self.alive = False

    def start_item(self, name):
        self.send(packet(AS_RESET) + packet(AS_INFO, struct.pack("<IBBB", self.args.rate, 1, self.index,
                                                                 len(self.items)) + text(name)))

    def stream(self, chunks):
        """Sends the sound (int16 mono arrays at args.rate) as ADPCM packets, paced, until the end, a change or a stop.
        Returns True at the end of the source."""
        encoder = AdpcmEncoder()
        pending = np.zeros(0, dtype=np.int16)
        size = BLOCK_SAMPLES * BLOCKS_PER_PACKET
        start = time.monotonic()
        sent = 0
        for pcm in chunks:
            if not self.alive or self.change is not None:
                return False
            pcm = np.clip(pcm.astype(np.float32) * GAIN, -32768, 32767).astype(np.int16)
            pending = np.concatenate((pending, pcm))
            while len(pending) >= size:
                # At most LEAD seconds ahead of real time (a station's first burst, or a file, is spread out)
                ahead = sent / self.args.rate - (time.monotonic() - start)
                if ahead > LEAD:
                    time.sleep(ahead - LEAD)
                if not self.alive or self.change is not None:
                    return False
                self.send(packet(AS_AUDIO, encoder.encode(pending[:size])))
                pending = pending[size:]
                sent += size
        if len(pending):        # The end, padded to whole blocks
            pending = np.concatenate((pending, np.zeros(-len(pending) % BLOCK_SAMPLES, dtype=np.int16)))
            self.send(packet(AS_AUDIO, encoder.encode(pending)))
            sent += len(pending)
        # Until the PS1 has played it (the next item's reset would drop what it has buffered; it starts playing
        # once about a second is buffered)
        end = start + sent / self.args.rate + 1.5
        while self.alive and self.change is None and time.monotonic() < end:
            time.sleep(0.1)
        return True

    def play_station(self, name, source):
        """An Icecast/SHOUTcast MP3 station with miniaudio (song titles). False if miniaudio can't play it."""
        last_title = []

        def on_title(_client, title):
            if last_title != [title]:     # (some stations repeat it every few seconds)
                last_title[:] = [title]
                self.title(title)

        try:
            client = miniaudio.IceCastClient(source, update_stream_title=on_title)
        except Exception as e:      # noqa: BLE001 (another format: PyAV)
            self.log("  (miniaudio: %s)" % e)
            return False
        if client.audio_format == miniaudio.FileFormat.UNKNOWN:
            client.close()
            return False
        self.start_item(name or client.station_name or file_name(source))
        try:
            frames = miniaudio.stream_any(client, source_format=client.audio_format, nchannels=1,
                                          sample_rate=self.args.rate, frames_to_read=BLOCK_SAMPLES * BLOCKS_PER_PACKET)
            self.stream(np.frombuffer(f, dtype=np.int16) for f in frames)
        finally:
            client.close()
        return True

    def play_pyav(self, name, source):
        """A file or link of any format FFmpeg reads. True at its end."""
        import av   # (only needed here)
        container = av.open(source, timeout=10)
        try:
            astream = container.streams.audio[0]
            tags = {k.lower(): v for k, v in container.metadata.items()}
            tags.update({k.lower(): v for k, v in astream.metadata.items()})
            self.start_item(name or tags.get("title") or file_name(source))
            title = " - ".join(t for t in (tags.get("artist"), tags.get("title")) if t)
            if title:
                self.title(title)
            resampler = av.AudioResampler(format="s16", layout="mono", rate=self.args.rate)

            def chunks():
                for frame in container.decode(astream):
                    for r in resampler.resample(frame):
                        yield r.to_ndarray().reshape(-1)
            return self.stream(chunks())
        finally:
            container.close()

    def play(self):
        name, source = self.items[self.index]
        self.log("%d/%d: %s" % (self.index + 1, len(self.items), source))
        try:
            if is_link(source) and self.play_station(name, source):
                if self.alive and self.change is None:
                    self.log("  The station stopped: opening it again")
                    time.sleep(2)
                return
            ended = self.play_pyav(name, source)
            if ended and self.change is None:
                self.log("  End of the file.")
                if not is_link(source):
                    self.change = (self.index + 1) % len(self.items)
        except OSError:
            self.alive = False
        except Exception as e:      # noqa: BLE001 (shown on the PS1)
            self.log("  Can't play it: %s" % e)
            try:
                self.start_item(name or file_name(source))
                self.send(packet(AS_ERROR, text("CAN'T PLAY IT, TRY ANOTHER ONE")))
            except OSError:
                self.alive = False
            self.wait_change(10)

    def wait_change(self, seconds):
        end = time.monotonic() + seconds
        while self.alive and self.change is None and time.monotonic() < end:
            time.sleep(0.1)

    def run(self):
        threading.Thread(target=self.reader, daemon=True).start()
        while self.alive:
            if self.change is not None:
                self.index, self.change = self.change, None
            self.play()
        self.log("Bridge disconnected.")


class Server:
    """The server on 127.0.0.1 (port 0: any free port, see .port), one bridge at a time, in a thread: used by the
    command line (main) and by the PS1 Dev Cart Manager (GUI). stop() ends it."""

    def __init__(self, items, args, port=0, log=print):
        self.items = items
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
            self.client = Client(sock, self.items, self.args, self.log)
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
    ap = argparse.ArgumentParser(description="PS1 DEV CART audio streamer server (for audiostreamer.ps-exe)")
    ap.add_argument("sources", nargs="*", help="station links or audio files (default: UK stations)")
    ap.add_argument("--port", type=int, default=7777)
    ap.add_argument("--rate", type=int, default=22050, help="sample rate sent to the PS1 (default 22050)")
    args = ap.parse_args()
    items = [("", s) for s in args.sources] or STATIONS
    server = Server(items, args, args.port)
    print("Audio server on 127.0.0.1:%d, %d in the playlist. Waiting for the bridge:" % (server.port, len(items)))
    print("  ps1transfer -r -e -f audiostreamer.ps-exe -net 127.0.0.1:%d" % server.port)
    try:
        while server.thread.is_alive():
            server.thread.join(0.5)
    except KeyboardInterrupt:
        server.stop()


if __name__ == "__main__":
    main()
