#!/usr/bin/env python3
"""
PS1 CO-PROCESSOR server: the computer doing the PlayStation's heavy work (coproc.ps-exe), as a co-processor.

    pip install -r requirements.txt
    python coproc_server.py [--port 7030]
    ps1transfer -r -e -f coproc.ps-exe -net 127.0.0.1:7030        (run.bat does both)

Swarm: the computer runs the simulation of the 1000 units (the path field around the walls to the target, each unit
pushed away from the ones near it) at 50 steps a second, and sends their positions, 3 bytes each; the next ones once the
PlayStation has the last ones (paced by the link, at most 12 a second). The PlayStation only draws them, moving them
smoothly between two updates.

Ray tracer: the computer ray traces the scene (spheres, reflections, shadows) seen from the PlayStation's camera,
160 x 112 while it moves, 320 x 224 with 4 rays a pixel when it stops, and sends it as an MDEC bitstream (the
PlayStation's own video format, mdec_encoder.py) that the PlayStation's MDEC decodes. One frame at a time: the next
request comes with the camera of that moment.

The rules of both are the PlayStation's too (coproc.c, PS1 alone): the same scenes, so the two can be compared.
Protocol: coproc.h.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""

import argparse
import asyncio
import heapq
import math
import socket
import struct
import time

import numpy as np

import mdec_encoder

# coproc.h
VERSION = 1
GRID_W, GRID_H, CELL = 80, 56, 4
CELLS = GRID_W * GRID_H
WALL_BYTES = CELLS // 8
SPEED, STEP_HZ = 48, 50
RAY_W, RAY_H = 160, 112

# Swarm rules (coproc.c has the same)
SEPARATION_R = 3.0          # Units closer than this push each other away...
SEPARATION_K = 40.0         # ...by this many pixels a second per pixel of push
MAX_SPEED = SPEED * 1.5
SWEEP = 48                  # Neighbours checked in x order (enough for the crowd at the target)
UNITS_MAX_RATE = 8          # Position updates a second at most (about 16 KB/s with the moves: the USB link carries
                            # about 24 at full frame rate)
KEYFRAME_S = 2.0            # Whole positions at least this often (else the moves since the last ones)


def packet(kind, payload=b""):
    return kind + struct.pack("<H", len(payload)) + payload


# 8 neighbours: (dx, dy, cost); a diagonal only when both of its sides are free
NEIGHBOURS = [(1, 0, 10), (-1, 0, 10), (0, 1, 10), (0, -1, 10), (1, 1, 14), (-1, 1, 14), (1, -1, 14), (-1, -1, 14)]
FAR = 1 << 30


class Swarm:
    def __init__(self):
        self.walls = np.zeros((GRID_H, GRID_W), bool)
        self.target = (GRID_W - 6, GRID_H // 2)
        self.pos = np.zeros((0, 2), np.float32)
        self.dirs = np.zeros((GRID_H, GRID_W, 2), np.float32)
        self.field_ms = 0.0

    def set_walls(self, data):
        bits = np.unpackbits(np.frombuffer(data[:WALL_BYTES], np.uint8), bitorder="little")
        self.walls = bits[:CELLS].reshape(GRID_H, GRID_W).astype(bool)

    def field(self):
        """Distances to the target (Dijkstra, 10 straight, 14 diagonal), then each cell's direction: towards its
        neighbour nearest to the target (straight, or diagonal * 0.7071), none at the target."""
        t0 = time.perf_counter()
        w = self.walls
        dist = np.full((GRID_H, GRID_W), FAR, np.int64)
        tx, ty = self.target
        heap = []
        if not w[ty, tx]:
            dist[ty, tx] = 0
            heap = [(0, tx, ty)]
        while heap:
            d, x, y = heapq.heappop(heap)
            if d > dist[y, x]:
                continue
            for dx, dy, c in NEIGHBOURS:
                nx, ny = x + dx, y + dy
                if not (0 <= nx < GRID_W and 0 <= ny < GRID_H) or w[ny, nx]:
                    continue
                if dx and dy and (w[y, nx] or w[ny, x]):
                    continue
                nd = d + c
                if nd < dist[ny, nx]:
                    dist[ny, nx] = nd
                    heapq.heappush(heap, (nd, nx, ny))
        dirs = np.zeros((GRID_H, GRID_W, 2), np.float32)
        best = dist.copy()
        pad = np.full((GRID_H + 2, GRID_W + 2), FAR, np.int64)
        pad[1:-1, 1:-1] = dist
        wpad = np.ones((GRID_H + 2, GRID_W + 2), bool)
        wpad[1:-1, 1:-1] = w
        for dx, dy, c in NEIGHBOURS:     # (same order as coproc.c: the first best one wins)
            nd = pad[1 + dy:1 + dy + GRID_H, 1 + dx:1 + dx + GRID_W]
            ok = nd < best
            if dx and dy:
                ok &= ~wpad[1:-1, 1 + dx:1 + dx + GRID_W] & ~wpad[1 + dy:1 + dy + GRID_H, 1:-1]
            k = 0.70710678 if dx and dy else 1.0
            best = np.where(ok, nd, best)
            dirs[ok] = (dx * k, dy * k)
        self.dirs = dirs
        self.field_ms = (time.perf_counter() - t0) * 1000

    def step(self, dt):
        p = self.pos
        if not len(p):
            return
        cx = np.clip((p[:, 0] / CELL).astype(np.int32), 0, GRID_W - 1)
        cy = np.clip((p[:, 1] / CELL).astype(np.int32), 0, GRID_H - 1)
        v = self.dirs[cy, cx] * SPEED
        # Pushed away from the units closer than SEPARATION_R: sorted by x, each pair of units up to SWEEP apart in
        # that order (the ones that can be that close, even in a crowd), both ways at once
        order = np.argsort(p[:, 0], kind="stable")
        q = p[order]
        push = np.zeros_like(q)
        for k in range(1, SWEEP + 1):
            d = q[:-k] - q[k:]
            d2 = (d * d).sum(axis=1)
            near = (d2 < SEPARATION_R * SEPARATION_R) & (d2 > 1e-6)
            if not near.any():
                if (q[k:, 0] - q[:-k, 0]).min() >= SEPARATION_R:
                    break
                continue
            # Push d * (R^2 - |d|^2) / (R (|d|^2 + 1)) (as coproc.c: one division, no square root)
            r2 = SEPARATION_R * SEPARATION_R
            f = (np.where(near, (r2 - d2) / (SEPARATION_R * (d2 + 1)), 0.0)[:, None] * d).astype(np.float32)
            push[:-k] += f
            push[k:] -= f
        v[order] += push * SEPARATION_K
        speed = np.sqrt((v * v).sum(axis=1))
        v *= np.minimum(1.0, MAX_SPEED / np.maximum(speed, 1e-6))[:, None]
        # Moved one axis after the other, not into a wall, not out of the field
        for axis in (0, 1):
            q = p.copy()
            q[:, axis] += v[:, axis] * dt
            q[:, 0] = np.clip(q[:, 0], 0, GRID_W * CELL - 1)
            q[:, 1] = np.clip(q[:, 1], 0, GRID_H * CELL - 1)
            free = ~self.walls[(q[:, 1] / CELL).astype(np.int32), (q[:, 0] / CELL).astype(np.int32)]
            p = np.where(free[:, None], q, p)
        self.pos = p.astype(np.float32)

    def pixels(self):
        return (np.clip(np.rint(self.pos[:, 0]), 0, 319).astype(np.int32),
                np.clip(np.rint(self.pos[:, 1]), 0, 223).astype(np.int32))

    @staticmethod
    def positions(x, y):
        out = np.empty((len(x), 3), np.uint8)
        out[:, 0] = x & 255
        out[:, 1] = x >> 8
        out[:, 2] = y
        return out.tobytes()


# Ray tracer scene (coproc.c has the same, in fixed point): a checkered floor, 4 spheres, a light, the sky
SPHERES = [  # centre, radius, colour, reflection
    ((0.0, 1.0, 0.0), 1.0, (230, 60, 60), 0.25),
    ((2.2, 0.7, 1.0), 0.7, (60, 200, 90), 0.1),
    ((-2.0, 0.9, 0.8), 0.9, (230, 230, 240), 0.8),
    ((0.8, 0.45, -1.8), 0.45, (230, 190, 60), 0.3),
]
FLOOR = ((200, 200, 210), (40, 50, 90), 0.15)
LIGHT = np.array((-0.4, 1.0, 0.5)) / np.linalg.norm((-0.4, 1.0, 0.5))
TARGET = np.array((0.0, 0.8, 0.0))
TAN_HALF_FOV = math.tan(math.radians(30))
SKY_LOW, SKY_HIGH = np.array((150, 180, 230.0)), np.array((40, 60, 140.0))


def camera(yaw, pitch, dist):
    """(origin, forward, right, up) of the orbit camera; angles as on the PlayStation (4096 = a turn)."""
    a, b = yaw * 2 * math.pi / 4096, pitch * 2 * math.pi / 4096
    origin = TARGET + dist * np.array((math.cos(b) * math.sin(a), math.sin(b), math.cos(b) * math.cos(a)))
    forward = (TARGET - origin) / np.linalg.norm(TARGET - origin)
    right = np.cross(forward, (0, 1, 0))
    right /= np.linalg.norm(right)
    return origin, forward, right, np.cross(right, forward)


def trace(o, d, depth):
    """Colours (N x 3) of the rays o + t d (N x 3)."""
    n = len(d)
    t = np.full(n, np.inf)
    hit = np.full(n, -1)                    # -1 sky, 0 floor, 1+ sphere
    with np.errstate(divide="ignore", invalid="ignore"):
        tf = np.where(d[:, 1] < -1e-6, -o[:, 1] / d[:, 1], np.inf)
    tf = np.where(tf > 1e-4, tf, np.inf)
    hit = np.where(tf < t, 0, hit)
    t = np.minimum(t, tf)
    for i, (c, r, _, _) in enumerate(SPHERES):
        oc = o - c
        b = (oc * d).sum(axis=1)
        disc = b * b - ((oc * oc).sum(axis=1) - r * r)
        sq = np.sqrt(np.maximum(disc, 0))
        ts = np.where(disc > 0, -b - sq, np.inf)
        ts = np.where(ts > 1e-4, ts, np.where((disc > 0) & (-b + sq > 1e-4), -b + sq, np.inf))
        closer = ts < t
        hit = np.where(closer, i + 1, hit)
        t = np.minimum(t, ts)
    up = np.clip(d[:, 1] * 1.5, 0, 1)[:, None]
    colour = SKY_LOW * (1 - up) + SKY_HIGH * up
    m = hit >= 0
    if not m.any():
        return colour
    p = o[m] + d[m] * t[m][:, None]
    h = hit[m]
    normal = np.zeros_like(p)
    base = np.zeros_like(p)
    refl = np.zeros(len(p))
    fl = h == 0
    normal[fl] = (0, 1, 0)
    checker = (np.floor(p[fl, 0]) + np.floor(p[fl, 2])).astype(np.int64) & 1
    base[fl] = np.where(checker[:, None] == 0, FLOOR[0], FLOOR[1])
    refl[fl] = FLOOR[2]
    for i, (c, r, col, rf) in enumerate(SPHERES):
        s = h == i + 1
        normal[s] = (p[s] - c) / r
        base[s] = col
        refl[s] = rf
    # Light: diffuse and highlight, unless something is between the point and the light
    diffuse = np.maximum(0, normal @ LIGHT)
    so = p + normal * 1e-3
    lit = diffuse > 0
    for c, r, _, _ in SPHERES:
        oc = so - c
        b = oc @ LIGHT
        disc = b * b - ((oc * oc).sum(axis=1) - r * r)
        lit &= ~((disc > 0) & (-b - np.sqrt(np.maximum(disc, 0)) > 0))
    diffuse = np.where(lit, diffuse, 0)
    rd = d[m] - 2 * (d[m] * normal).sum(axis=1)[:, None] * normal
    spec = np.where(lit, np.maximum(0, rd @ LIGHT) ** 32 * 0.6, 0)
    c = base * (0.15 + 0.85 * diffuse)[:, None] + spec[:, None] * 255
    if depth < 1:
        r = refl > 0
        if r.any():
            bounce = trace(so[r], rd[r], depth + 1)
            c[r] = c[r] * (1 - refl[r])[:, None] + bounce * refl[r][:, None]
    colour[m] = c
    return colour


def render(w, h, yaw, pitch, dist, samples=1):
    """The frame (h x w x 3, uint8) seen by the camera; samples: rays a pixel side (2: 4 rays a pixel)."""
    o, f, r, u = camera(yaw, pitch, dist)
    sw, sh = w * samples, h * samples
    px = (np.arange(sw) + 0.5) / sw * 2 - 1
    py = (np.arange(sh) + 0.5) / sh * 2 - 1
    aspect = 320 / 224
    d = (f[None, None, :] + px[None, :, None] * TAN_HALF_FOV * aspect * r[None, None, :] -
         py[:, None, None] * TAN_HALF_FOV * u[None, None, :]).reshape(-1, 3)
    d /= np.linalg.norm(d, axis=1)[:, None]
    img = trace(np.broadcast_to(o, d.shape).copy(), d, 0).reshape(sh, sw, 3)
    if samples > 1:
        img = img.reshape(h, samples, w, samples, 3).mean(axis=(1, 3))
    return np.clip(img, 0, 255).astype(np.uint8)


class Client:
    def __init__(self, reader, writer, log):
        self.reader, self.writer, self.log = reader, writer, log
        self.swarm = Swarm()
        self.swarm_on = False
        self.seq = 0
        self.acked = True
        self.last_sent = 0.0
        self.steps = 0
        self.step_us = 0
        self.ray_wanted = None          # Latest request not done yet
        self.ray_busy = False
        self.sent_xy = None             # Positions the PlayStation has (the moves are from them)
        self.keyframe_t = 0.0
        self.hello = False

    def send(self, data):
        self.writer.write(data)

    async def run(self):
        ticker = asyncio.ensure_future(self.tick())
        try:
            while True:
                header = await self.reader.readexactly(3)
                kind, n = header[:1], struct.unpack("<H", header[1:])[0]
                data = await self.reader.readexactly(n) if n else b""
                self.message(kind, data)
        finally:
            ticker.cancel()

    def message(self, kind, data):
        s = self.swarm
        if kind == b"H":                # (each second: the welcome back keeps the connection known)
            self.send(packet(b"W", bytes((VERSION,))))
            if not self.hello:
                self.log("PlayStation connected")
            self.hello = True
        elif kind == b"B" and len(data) >= 2 + WALL_BYTES + 2:
            s.target = (data[0], data[1])
            s.set_walls(data[2:2 + WALL_BYTES])
            count = struct.unpack_from("<H", data, 2 + WALL_BYTES)[0]
            xy = np.frombuffer(data[4 + WALL_BYTES:4 + WALL_BYTES + count * 4], "<i2").reshape(-1, 2)
            s.pos = (xy.astype(np.float32) / 16)
            s.field()
            self.swarm_on, self.acked, self.steps, self.sent_xy = True, True, 0, None
            self.log("Swarm on the computer: %d units (path field %.1f ms)" % (len(s.pos), s.field_ms))
        elif kind == b"G" and len(data) >= 2:
            s.target = (data[0], data[1])
            s.field()
        elif kind == b"L" and len(data) >= WALL_BYTES:
            s.set_walls(data)
            s.field()
        elif kind == b"K":
            self.acked = True
        elif kind == b"E":
            self.swarm_on = False
            self.log("Swarm back on the PlayStation")
        elif kind == b"R" and len(data) >= 9:
            self.ray_wanted = struct.unpack("<HBhhH", data[:9])
            if not self.ray_busy:
                asyncio.ensure_future(self.ray())

    async def tick(self):
        """Swarm steps at 50 a second (in real time), positions sent once the last ones arrived."""
        period = 1 / STEP_HZ
        next_t = time.monotonic()
        while True:
            await asyncio.sleep(max(0, next_t - time.monotonic()))
            next_t += period
            if not self.swarm_on:
                next_t = time.monotonic() + period
                continue
            t0 = time.perf_counter()
            self.swarm.step(period)
            self.step_us = int((time.perf_counter() - t0) * 1e6)
            self.steps += 1
            now = time.monotonic()
            if self.acked and now - self.last_sent >= 1 / UNITS_MAX_RATE:
                self.seq = (self.seq + 1) & 0xFFFF
                x, y = self.swarm.pixels()
                head = struct.pack("<HIHH", self.seq, self.step_us, self.steps, len(x))
                if self.sent_xy is not None and now - self.keyframe_t < KEYFRAME_S:
                    dx, dy = x - self.sent_xy[0], y - self.sent_xy[1]
                    if np.abs(dx).max(initial=0) <= 127 and np.abs(dy).max(initial=0) <= 127:
                        moves = np.column_stack((dx, dy)).astype(np.int8).tobytes()
                        self.send(packet(b"V", head + moves))
                        self.sent_xy = (x, y)
                        self.acked, self.last_sent, self.steps = False, now, 0
                        continue
                self.send(packet(b"U", head + self.swarm.positions(x, y)))
                self.sent_xy, self.keyframe_t = (x, y), now
                self.acked, self.last_sent, self.steps = False, now, 0

    async def ray(self):
        self.ray_busy = True
        loop = asyncio.get_running_loop()
        try:
            while self.ray_wanted:
                fid, quality, yaw, pitch, dist = self.ray_wanted
                self.ray_wanted = None
                w, h, samples, budget = (320, 224, 2, 24000) if quality else (RAY_W, RAY_H, 1, 6000)
                t0 = time.perf_counter()
                img = await loop.run_in_executor(None, render, w, h, yaw, pitch, dist / 256, samples)
                t1 = time.perf_counter()
                bs, _ = await loop.run_in_executor(None, mdec_encoder.encode_frame_fit, img, budget, 2)
                t2 = time.perf_counter()
                self.send(packet(b"F", struct.pack("<HHHHH", fid, w, h, int((t1 - t0) * 1000), int((t2 - t1) * 1000))
                                 + bs))
        finally:
            self.ray_busy = False


async def main():
    ap = argparse.ArgumentParser(description="PS1 CO-PROCESSOR server")
    ap.add_argument("--port", type=int, default=7030)
    args = ap.parse_args()

    def log(text):
        print(time.strftime("%H:%M:%S ") + text, flush=True)

    async def handle(reader, writer):
        sock = writer.get_extra_info("socket")
        if sock is not None:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        log("Bridge connected")
        try:
            await Client(reader, writer, log).run()
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            pass
        writer.close()
        log("Bridge disconnected")

    server = await asyncio.start_server(handle, "127.0.0.1", args.port)
    log("PS1 CO-PROCESSOR server on 127.0.0.1:%d, waiting for the bridge (run.bat or redux.bat)" % args.port)
    await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
