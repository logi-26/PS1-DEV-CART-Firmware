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

Smoke: the computer simulates the fluid (stable fluids) on a grid of 160 x 112, 16 times as fine as the
PlayStation's own (40 x 28), 50 steps a second with the PlayStation's controls (the cursor, the wind), and sends it
as MDEC frames, the next one once the PlayStation has the last one.

Bounced light: the computer lights a room by radiosity (the light bouncing from wall to wall, 777 patches): the
form factors between all the patches when the box moves (with the box in the way or not), then the light, directly
and bounce after bounce, each time the light moves. It sends a colour a patch; the PlayStation draws them with its
Gouraud shading.

The rules of all four are the PlayStation's too (coproc.c, PS1 alone): the same scenes, so the two can be compared.
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
import threading
import time

import numpy as np

import mdec_encoder

# coproc.h
VERSION = 2
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


# Smoke (coproc.c has the same, on a grid of 40 x 28): "stable fluids" on a grid of 160 x 112 (cells of 2 pixels).
# Speeds in cells a step, smoke in colour units (0-320)
SM_W, SM_H, SM_CELL = 160, 112, 2
SM_ITER = 40                # Pressure iterations (Jacobi)
SM_REACH = 24               # The cursor's and the wind's reach (pixels)
SM_WIND_X, SM_WIND_V = 12, 24
SM_BG = np.array((8, 10, 20), np.float32)
SM_FRAME_BYTES = 3500       # A frame's MDEC data at most
SM_MAX_RATE = 30            # Frames a second at most
SM_IN_FLIGHT = 2            # Frames sent the PlayStation hasn't said it has
SM_PUSH_S = 0.2             # The cursor's push stops when no controls came for this long
PILLARS = ((120, 112, 16), (208, 62, 12), (208, 162, 12))
PALETTE = np.array(((255, 140, 40), (40, 200, 255), (255, 60, 160), (90, 255, 90), (255, 230, 60), (160, 120, 255)),
                   np.float32)


def shifted(a, dx, dy):
    """a seen from the neighbour at (dx, dy) (0 past the edges)."""
    out = np.zeros_like(a)
    h, w = a.shape[-2:]
    out[..., max(0, -dy):h - max(0, dy), max(0, -dx):w - max(0, dx)] = \
        a[..., max(0, dy):h - max(0, -dy), max(0, dx):w - max(0, -dx)]
    return out


class Smoke:
    def __init__(self):
        cy, cx = (np.mgrid[0:SM_H, 0:SM_W] * SM_CELL + SM_CELL / 2).astype(np.float32)
        self.solid = np.zeros((SM_H, SM_W), bool)
        for x, y, r in PILLARS:
            self.solid |= (cx - x) ** 2 + (cy - y) ** 2 < r * r
        fluid = (~self.solid).astype(np.float32)
        # The neighbours that are fluid (left, right, up, down), and how many
        self.nbr = [shifted(fluid, dx, dy) * fluid for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1))]
        n = sum(self.nbr)
        self.inv_n = np.where(n > 0, 1 / np.maximum(n, 1), 0).astype(np.float32)
        self.fluid = fluid
        self.cx, self.cy = cx, cy
        self.gx, self.gy = np.meshgrid(np.arange(SM_W, dtype=np.float32), np.arange(SM_H, dtype=np.float32))
        self.clear()

    def clear(self):
        self.f = np.zeros((5, SM_H, SM_W), np.float32)      # u, v (cells a step), smoke r, g, b
        self.p = np.zeros((SM_H, SM_W), np.float32)
        self.steps = 0

    def splat(self, px, py, fx, fy, rgb, dt=1):
        """Smoke of a colour (for dt steps) and a push (pixels a step * 16) around a place (pixels), less further away."""
        # (in the cells within reach)
        x0, x1 = max(0, (px - SM_REACH) // SM_CELL), min(SM_W, (px + SM_REACH) // SM_CELL + 1)
        y0, y1 = max(0, (py - SM_REACH) // SM_CELL), min(SM_H, (py + SM_REACH) // SM_CELL + 1)
        if x0 >= x1 or y0 >= y1:
            return
        win = np.s_[y0:y1, x0:x1]
        d2 = (self.cx[win] - px) ** 2 + (self.cy[win] - py) ** 2
        w = np.maximum(0, 1 - d2 / (SM_REACH * SM_REACH)) * self.fluid[win]
        self.f[0][win] += fx / (16 * SM_CELL) * w
        self.f[1][win] += fy / (16 * SM_CELL) * w
        if rgb is not None:
            self.f[2:, y0:y1, x0:x1] = np.minimum(self.f[2:, y0:y1, x0:x1] + rgb[:, None, None] * w * dt / 8, 320)

    def advect(self, dt):
        """Everything carried along the flow for dt steps: each cell takes what was where its flow comes from
        (bilinear)."""
        x = np.clip(self.gx - self.f[0] * dt, 0, SM_W - 1.001)
        y = np.clip(self.gy - self.f[1] * dt, 0, SM_H - 1.001)
        x0, y0 = x.astype(np.int32), y.astype(np.int32)
        fx, fy = (x - x0).ravel(), (y - y0).ravel()
        i = (y0 * SM_W + x0).ravel()
        f = self.f.reshape(5, -1)
        a, b = np.take(f, i, axis=1), np.take(f, i + 1, axis=1)
        c, d = np.take(f, i + SM_W, axis=1), np.take(f, i + SM_W + 1, axis=1)
        top = a + (b - a) * fx
        out = top + (c + (d - c) * fx - top) * fy
        self.f = out.reshape(5, SM_H, SM_W) * self.fluid

    def project(self):
        """The flow made incompressible: no flow through the pillars and the edges (as coproc.c)."""
        u, v = self.f[0], self.f[1]
        nl, nr, nu, nd = self.nbr
        div = 0.5 * (shifted(u, 1, 0) * nr - shifted(u, -1, 0) * nl + shifted(v, 0, 1) * nd - shifted(v, 0, -1) * nu)
        # (the pressure in a frame of zeros: the neighbours are slices of it)
        pp = np.zeros((SM_H + 2, SM_W + 2), np.float32)
        pp[1:-1, 1:-1] = self.p
        for _ in range(SM_ITER):
            pp[1:-1, 1:-1] = (pp[1:-1, :-2] * nl + pp[1:-1, 2:] * nr + pp[:-2, 1:-1] * nu + pp[2:, 1:-1] * nd -
                              div) * self.inv_n
        p = self.p = pp[1:-1, 1:-1].copy()
        # (a pillar's or an edge's side: the pressure of the cell itself)
        pl, pr = np.where(nl > 0, shifted(p, -1, 0), p), np.where(nr > 0, shifted(p, 1, 0), p)
        pu, pd = np.where(nu > 0, shifted(p, 0, -1), p), np.where(nd > 0, shifted(p, 0, 1), p)
        self.f[0] -= 0.5 * (pr - pl) * self.fluid
        self.f[1] -= 0.5 * (pd - pu) * self.fluid

    def step(self, cursor, dt=1):
        """A step dt steps long (1/50 s each: longer when the computer falls behind, as on the PlayStation).
        cursor: (x, y, push x, push y (pixels a step * 16), pushing, wind, colour)."""
        x, y, fx, fy, push, wind, colour = cursor
        if wind:
            self.splat(SM_WIND_X, 112, SM_WIND_V * dt, 0, PALETTE[(self.steps // 150) % 6], dt)
        if push:
            self.splat(x, y, fx * dt, fy * dt, PALETTE[colour % 6], dt)
        self.advect(dt)
        self.project()
        self.f[:2] *= (1 - 1 / 128) ** dt  # (the flow slows down, the smoke fades)
        self.f[2:] *= (1 - 1 / 128) ** dt
        self.steps += dt

    def frame(self):
        """The smoke as an RGB frame (SM_H x SM_W x 3)."""
        return np.clip(np.moveaxis(self.f[2:], 0, -1) + SM_BG, 0, 255).astype(np.uint8)


class SmokeRunner(threading.Thread):
    """The computer's smoke, in its own thread: 50 steps a second with the latest controls, and a frame each time
    the PlayStation has one of the last ones (SM_IN_FLIGHT, at most SM_MAX_RATE a second)."""

    def __init__(self, client):
        super().__init__(daemon=True)
        self.client, self.loop = client, asyncio.get_running_loop()
        self.smoke = Smoke()
        self.cursor = (160, 112, 0, 0, False, True, 0)
        self.cursor_t = 0.0
        self.clears = None
        self.in_flight = 0
        self.sent_t = 0.0
        self.seq = 0
        self.running = True

    def controls(self, data):
        x, y, dx, dy, n, flags, colour, clears = struct.unpack("<HHhhBBBB", data[:12])
        if self.clears is not None and clears != self.clears:
            self.smoke.clear()
        self.clears = clears
        # (the cursor's move over n frames: a push a step of half of it, as on the PlayStation)
        self.cursor = (x, y, dx / max(n, 1) / 2, dy / max(n, 1) / 2, bool(flags & 1), bool(flags & 2), colour)
        self.cursor_t = time.monotonic()

    def run(self):
        period = 1 / STEP_HZ
        last_t = time.monotonic()
        step_ms, steps = 0.0, 0
        while self.running:
            time.sleep(max(0.0, last_t + period - time.monotonic()))
            dt = max(1, min(3, int((time.monotonic() - last_t) / period + 0.01)))  # (steps due: more when behind)
            last_t += dt * period
            if time.monotonic() - last_t > 3 * period:
                last_t = time.monotonic()
            cursor = self.cursor
            if time.monotonic() - self.cursor_t > SM_PUSH_S:
                cursor = cursor[:4] + (False,) + cursor[5:]
            t0 = time.perf_counter()
            self.smoke.step(cursor, dt)
            step_ms = step_ms * 0.9 + (time.perf_counter() - t0) * 1000 * 0.1
            steps += dt
            now = time.monotonic()
            if self.in_flight < SM_IN_FLIGHT and now - self.sent_t >= 1 / SM_MAX_RATE:
                bs, _ = mdec_encoder.encode_frame_fit(self.smoke.frame(), SM_FRAME_BYTES, 2)
                self.seq = (self.seq + 1) & 0xFFFF
                data = packet(b"M", struct.pack("<HHHHH", self.seq, min(int(step_ms * 10), 65535), steps, SM_W, SM_H)
                              + bs)
                self.in_flight += 1
                self.sent_t, steps = now, 0
                self.loop.call_soon_threadsafe(self.client.send, data)
            elif self.in_flight and now - self.sent_t > 1.0:    # (a frame lost: the next one sent)
                self.in_flight = 0


# Lighting (coproc.h, coproc.c has the same): a room open at the front (x -480..480, y -480..480 downwards, z 0..960
# into the screen), red on the left, green on the right, a box on the floor that moves, a point light. Every surface in
# patches of 80 x 80 (777). The light is a lamp facing down. The bounced light by radiosity: each patch's light, from the light directly and from all
# the other patches (form factors, with the box in the way or not)
LT_PATCH = 80
LT_AREA = LT_PATCH * LT_PATCH
LT_BOUNCES = 24             # Iterations of the solution (each one: one more bounce)
LT_POWER = 1.2              # The light: its direct light on the floor 480 below it
LT_IN_FLIGHT = 2            # Colours on their way to the PlayStation at most
LT_MAX_E = 4.0              # Direct light at most (a patch right next to the light)
LT_BOX = (240, 320, 240)    # Box size (x, y, z); it stands on the floor
LIGHT_COLOURS = np.array(((1.0, 1.0, 1.0), (1.0, 0.78, 0.5), (0.6, 0.75, 1.0)), np.float32)
WHITE, RED, GREEN, BOX = (0.75, 0.75, 0.75), (0.75, 0.16, 0.12), (0.15, 0.6, 0.18), (0.8, 0.75, 0.6)


def lt_faces(bx, bz):
    """The faces: (origin, u, v (a patch's sides), patches along u, v, normal axis, normal sign, reflectance), as
    LtFaces in coproc.c."""
    bw, bh, bd = LT_BOX
    p, y0 = LT_PATCH, 480 - bh
    return [
        ((-480, 480, 0), (p, 0, 0), (0, 0, p), 12, 12, 1, -1, WHITE),       # Floor
        ((-480, -480, 0), (p, 0, 0), (0, 0, p), 12, 12, 1, 1, WHITE),       # Ceiling
        ((-480, -480, 960), (p, 0, 0), (0, p, 0), 12, 12, 2, -1, WHITE),    # Back
        ((-480, -480, 0), (0, 0, p), (0, p, 0), 12, 12, 0, 1, RED),         # Left
        ((480, -480, 0), (0, 0, p), (0, p, 0), 12, 12, 0, -1, GREEN),       # Right
        ((bx, y0, bz), (p, 0, 0), (0, 0, p), bw // p, bd // p, 1, -1, BOX),     # Box: top
        ((bx, y0, bz), (p, 0, 0), (0, p, 0), bw // p, bh // p, 2, -1, BOX),     # front
        ((bx, y0, bz + bd), (p, 0, 0), (0, p, 0), bw // p, bh // p, 2, 1, BOX),  # back
        ((bx, y0, bz), (0, 0, p), (0, p, 0), bd // p, bh // p, 0, -1, BOX),     # left
        ((bx + bw, y0, bz), (0, 0, p), (0, p, 0), bd // p, bh // p, 0, 1, BOX),  # right
    ]


def segment_blocked(p, q, lo, hi):
    """Segments p -> q (N x 3) going through the box lo..hi (slab test; touching doesn't count)."""
    d = q - p
    with np.errstate(divide="ignore", invalid="ignore"):
        ta, tb = (lo - p) / d, (hi - p) / d
    flat = d == 0
    inside = (p > lo) & (p < hi)
    t_in = np.where(flat, np.where(inside, -np.inf, np.inf), np.minimum(ta, tb))
    t_out = np.where(flat, np.where(inside, np.inf, -np.inf), np.maximum(ta, tb))
    t0 = np.maximum(t_in.max(axis=1), 0)
    t1 = np.minimum(t_out.min(axis=1), 1)
    return t0 < t1


class Lighting:
    def __init__(self):
        self.box = None
        self.form_ms = 0.0

    def build(self, bx, bz):
        """Patches of the room with the box at bx, bz, and the form factors between them."""
        t0 = time.perf_counter()
        c, n, rho = [], [], []
        for o, u, v, nu, nv, axis, sign, r in lt_faces(bx, bz):
            o, u, v = np.array(o, np.float32), np.array(u, np.float32), np.array(v, np.float32)
            normal = np.zeros(3, np.float32)
            normal[axis] = sign
            for j in range(nv):
                for i in range(nu):
                    c.append(o + (i + 0.5) * u + (j + 0.5) * v)
                    n.append(normal)
                    rho.append(r)
        self.c, self.n, self.rho = np.array(c), np.array(n), np.array(rho, np.float32)
        self.o = self.c + self.n * 4                # (just off the surface, for the box in the way)
        bw, bh, bd = LT_BOX
        self.lo = np.array((bx, 480 - bh, bz), np.float32)
        self.hi = self.lo + np.array(LT_BOX, np.float32)
        # Form factors: cos i cos j A / (pi r^2 + A) (a disc of the patch's area), 0 if the box is in the way
        d = self.c[None, :, :] - self.c[:, None, :]
        dni = (d * self.n[:, None, :]).sum(-1)
        dnj = -(d * self.n[None, :, :]).sum(-1)
        r2 = (d * d).sum(-1)
        facing = (dni > 0) & (dnj > 0)
        f = np.zeros_like(r2)
        f[facing] = dni[facing] * dnj[facing] / r2[facing] * LT_AREA / (np.pi * r2[facing] + LT_AREA)
        i, j = np.nonzero(np.triu(facing))
        blocked = segment_blocked(self.o[i], self.o[j], self.lo, self.hi)
        f[i[blocked], j[blocked]] = f[j[blocked], i[blocked]] = 0
        self.f = f
        self.box = (bx, bz)
        self.form_ms = (time.perf_counter() - t0) * 1000

    def solve(self, light, colour):
        """Each patch's colour (gamma 2: as on the PlayStation), N x 3 bytes: the direct light, then LT_BOUNCES
        iterations of B = rho (E + F B)."""
        L = np.array(light, np.float32)
        d = self.c - L
        dnj = -(d * self.n).sum(-1)
        r2 = (d * d).sum(-1)
        # (a lamp facing down: its light falls off as the cosine from straight down)
        e = np.where((dnj > 0) & (d[:, 1] > 0), LT_POWER * 480.0 * 480.0 * dnj * d[:, 1] / (r2 * r2), 0)
        e = np.minimum(e, LT_MAX_E) * ~segment_blocked(np.broadcast_to(L, self.o.shape), self.o, self.lo, self.hi)
        e = e[:, None] * LIGHT_COLOURS[colour % 3][None, :]
        b = self.rho * e
        for _ in range(LT_BOUNCES):
            b = self.rho * (e + self.f @ b)
        x = np.minimum(1023, (b * 1024).astype(np.int64))
        return np.sqrt(x * 65025 // 1023).astype(np.uint8)   # (as Isqrt on the PlayStation: rounded down)


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
        self.smoke = None               # SmokeRunner while the computer simulates the smoke
        self.lighting = Lighting()
        self.light_wanted = None        # Latest light and box not lit yet
        self.light_busy = False
        self.light_in_flight = 0        # Colours sent the PlayStation hasn't said it has (LT_IN_FLIGHT at most)
        self.light_sent_t = 0.0
        self.light_seq = 0
        self.light_last = None          # The light and box of the last colours sent

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
            self.smoke_end()

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
        elif kind == b"S" and len(data) >= 12:
            if not self.smoke:
                self.smoke = SmokeRunner(self)
                self.smoke.start()
                self.log("Smoke on the computer")
            self.smoke.controls(data)
        elif kind == b"N" and self.smoke:
            self.smoke.in_flight = max(0, self.smoke.in_flight - 1)
        elif kind == b"Q":
            self.smoke_end()
            self.log("Smoke back on the PlayStation")
        elif kind == b"O" and len(data) >= 11:
            state = struct.unpack("<5hB", data[:11])
            if state != self.light_last:
                self.light_wanted = state
            if not self.light_busy:
                asyncio.ensure_future(self.light())
        elif kind == b"Y":
            self.light_in_flight = max(0, self.light_in_flight - 1)
        elif kind == b"R" and len(data) >= 9:
            self.ray_wanted = struct.unpack("<HBhhH", data[:9])
            if not self.ray_busy:
                asyncio.ensure_future(self.ray())

    async def light(self):
        """The room's colours for the latest light and box (one at a time; at most LT_IN_FLIGHT on their way)."""
        self.light_busy = True
        loop = asyncio.get_running_loop()
        try:
            while self.light_wanted:
                while self.light_in_flight >= LT_IN_FLIGHT and time.monotonic() - self.light_sent_t < 1.0:
                    await asyncio.sleep(0.002)
                if self.light_in_flight >= LT_IN_FLIGHT:     # (colours lost: sent again)
                    self.light_in_flight = 0
                lx, ly, lz, bx, bz, colour = state = self.light_wanted
                self.light_wanted = None
                lighting = self.lighting
                t0 = time.perf_counter()
                form_ms = 0
                if lighting.box != (bx, bz):
                    if lighting.box is None:
                        self.log("Bounced light on the computer")
                    await loop.run_in_executor(None, lighting.build, bx, bz)
                    form_ms = int(lighting.form_ms)
                rgb = await loop.run_in_executor(None, lighting.solve, (lx, ly, lz), colour)
                tenths = int((time.perf_counter() - t0) * 10000)
                self.light_seq = (self.light_seq + 1) & 0xFFFF
                self.send(packet(b"C", struct.pack("<HHHH", self.light_seq, min(tenths, 65535), form_ms, len(rgb)) +
                                 rgb.tobytes()))
                self.light_last = state
                self.light_in_flight += 1
                self.light_sent_t = time.monotonic()
        finally:
            self.light_busy = False

    def smoke_end(self):
        if self.smoke:
            self.smoke.running = False
            self.smoke = None

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
