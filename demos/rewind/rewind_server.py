#!/usr/bin/env python3
"""
PS1 REWIND server: the computer keeps the history of a PlayStation program that uses the rewind library (rewind.h),
so it can be rewound for minutes, not just the 10 seconds the PlayStation keeps in its RAM.

    python rewind_server.py [--port 7040] [--minutes 10]
    ps1transfer -r -e -f pegdrop.ps-exe -net 127.0.0.1:7040        (run.bat does both)

Each frame, the PlayStation sends what changed in its block since the frame before; the server rebuilds every frame
and keeps them. While rewinding, the PlayStation asks for the frames older than the oldest it has, and gets each as
what changed from the frame after it. When it goes on from a frame, the frames after it are forgotten.
Protocol: rewind.h.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""

import argparse
import asyncio
import socket
import struct
import time

VERSION = 1


def packet(kind, payload=b""):
    return kind + struct.pack("<H", len(payload)) + payload


def diff(a, b):
    """What changed between two blocks (as Diff in rewind.c): runs of skip (2), count (1), the bytes XOR."""
    out = bytearray()
    i, n = 0, len(a)
    while i < n:
        start = i
        while i < n and a[i] == b[i]:
            i += 1
        if i == n:
            break
        out += struct.pack("<H", i - start)
        count_at = len(out)
        out.append(0)
        while i < n and out[count_at] < 255:
            # (a run goes on over up to 3 equal bytes: cheaper than a new run)
            if all(j >= n or a[j] == b[j] for j in range(i, i + 4)):
                break
            out.append(a[i] ^ b[i])
            out[count_at] += 1
            i += 1
    return bytes(out)


def patch(base, d):
    """base XOR the runs; None if they don't fit."""
    out = bytearray(base)
    i = k = 0
    while k + 3 <= len(d):
        i += d[k] | d[k + 1] << 8
        count = d[k + 2]
        k += 3
        if i + count > len(out) or k + count > len(d):
            return None
        for _ in range(count):
            out[i] ^= d[k]
            i += 1
            k += 1
    return bytes(out) if k == len(d) else None


class Client:
    def __init__(self, reader, writer, log, max_frames):
        self.reader, self.writer, self.log = reader, writer, log
        self.max_frames = max_frames
        self.first = 0              # Frame of frames[0]
        self.frames = []            # Every frame's block, from first
        self.hello = False
        self.last_log = 0.0
        self.bytes_in = 0               # Received since the last log

    def send(self, data):
        self.writer.write(data)

    def newest(self):
        return self.first + len(self.frames) - 1

    async def run(self):
        while True:
            header = await self.reader.readexactly(3)
            kind, n = header[:1], struct.unpack("<H", header[1:])[0]
            data = await self.reader.readexactly(n) if n else b""
            self.bytes_in += 3 + n
            self.message(kind, data)

    def message(self, kind, data):
        if kind == b"H":
            self.send(packet(b"W", struct.pack("<BI", VERSION, self.first if self.frames else 0xFFFFFFFF)))
            if not self.hello:
                self.log("PlayStation connected (a block of %d bytes)" % struct.unpack_from("<H", data, 1)[0]
                         if len(data) >= 3 else "PlayStation connected")
            self.hello = True
        elif kind == b"K" and len(data) >= 4:
            f = struct.unpack_from("<I", data)[0]
            self.first, self.frames = f, [bytes(data[4:])]
        elif kind == b"D" and len(data) >= 4:
            f = struct.unpack_from("<I", data)[0]
            block = patch(self.frames[-1], data[4:]) if self.frames and f == self.newest() + 1 else None
            if block is None:
                self.send(packet(b"X"))         # (a frame missed: a keyframe wanted)
                return
            self.frames.append(block)
            if len(self.frames) > self.max_frames:
                drop = len(self.frames) - self.max_frames
                del self.frames[:drop]
                self.first += drop
        elif kind == b"T" and len(data) >= 8:
            f, total = struct.unpack_from("<II", data)
            if self.frames and self.first <= f <= self.newest():
                # (the PlayStation's block and the server's should be the same)
                ok = sum(self.frames[f - self.first]) & 0xFFFFFFFF == total
                self.log("Rewound to frame %d (%.1f s back): %s" % (f, (self.newest() - f) / 50,
                                                                    "same block" if ok else "DIFFERENT BLOCK"))
                del self.frames[f - self.first + 1:]
        elif kind == b"B" and len(data) >= 6:
            lo, n = struct.unpack_from("<IH", data)
            f = lo - 1
            sent = 0
            while sent < n and self.frames and self.first <= f < self.newest():
                i = f - self.first
                self.send(packet(b"O", struct.pack("<I", f) + diff(self.frames[i], self.frames[i + 1])))
                f -= 1
                sent += 1
            if sent < n:                        # (no more: the oldest it has)
                self.send(packet(b"N", struct.pack("<I", self.first if self.frames and self.first < lo else lo)))
        now = time.monotonic()
        if now - self.last_log > 10 and self.frames:
            if self.last_log:
                self.log("History: frames %d to %d (%.1f s), %.1f KB/s from the PlayStation" %
                         (self.first, self.newest(), len(self.frames) / 50,
                          self.bytes_in / 1024 / (now - self.last_log)))
            self.last_log, self.bytes_in = now, 0


async def main():
    ap = argparse.ArgumentParser(description="PS1 REWIND server")
    ap.add_argument("--port", type=int, default=7040)
    ap.add_argument("--minutes", type=float, default=10, help="history kept (default 10 minutes)")
    args = ap.parse_args()

    def log(text):
        print(time.strftime("%H:%M:%S ") + text, flush=True)

    async def handle(reader, writer):
        sock = writer.get_extra_info("socket")
        if sock is not None:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        log("Bridge connected")
        try:
            await Client(reader, writer, log, int(args.minutes * 60 * 50)).run()
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            pass
        writer.close()
        log("Bridge disconnected")

    server = await asyncio.start_server(handle, "127.0.0.1", args.port)
    log("PS1 REWIND server on 127.0.0.1:%d, waiting for the bridge (run.bat)" % args.port)
    await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
