"""
Frame Inspector: the GPU commands of a captured frame. Packets made here from the GPU's command format (GP0: polygons
flat / gouraud / textured, triangles and quads; lines and polylines; rectangles of each size; fills; VRAM copies, loads,
stores; the drawing environment) decoded by gui/gpu.py: each field checked (kind, points with the drawing coordinates,
colours, texture page, CLUT, semi-transparency, the words each command takes, several commands in a packet); random
words (no exception). The ordering table walk of the capture (ps1transfer/gdbserver.c WalkList, compiled for the PC
on a RAM image): every packet in order, the draw area and offset, loops and bad links stopped; its output decoded.

    python -m unittest tests/tools/test_gpu.py -v

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import random
import struct
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cbuild                                       # noqa: E402
from toolhost import tmp_path, write                # noqa: E402

import gpu                                          # noqa: E402


def pack_xy(x, y):
    return (x & 0x7FF) | ((y & 0x7FF) << 16)


def col(c):
    return c[0] | (c[1] << 8) | (c[2] << 16)


def polygon(rng):
    """A random polygon command: (words, expected dict)."""
    gouraud, quad, tex, semi = (rng.random() < 0.5 for _ in range(4))
    cmd = 0x20 | (0x10 if gouraud else 0) | (0x08 if quad else 0) | (0x04 if tex else 0) | (0x02 if semi else 0)
    n = 4 if quad else 3
    pts = [(rng.randint(-1024, 1023), rng.randint(-1024, 1023)) for _ in range(n)]
    cols = [tuple(rng.randrange(256) for _ in range(3)) for _ in range(n if gouraud else 1)]
    clut, tpage = rng.randrange(0x10000), rng.randrange(0x10000)
    words = [(cmd << 24) | col(cols[0])]
    for v in range(n):
        if gouraud and v:
            words.append(col(cols[v]))
        words.append(pack_xy(*pts[v]))
        if tex:
            uv = rng.randrange(256) | (rng.randrange(256) << 8)
            words.append(uv | ((clut if v == 0 else tpage if v == 1 else 0) << 16))
    outline = [pts[0], pts[1], pts[3], pts[2]] if quad else pts
    return words, dict(kind="poly", points=outline, colors=cols, semi=semi, textured=tex,
                       clut=gpu.clut_text(clut) if tex else None, tpage=gpu.tpage_text(tpage) if tex else None)


def line(rng):
    gouraud, poly, semi = (rng.random() < 0.5 for _ in range(3))
    cmd = 0x40 | (0x10 if gouraud else 0) | (0x08 if poly else 0) | (0x02 if semi else 0)
    n = rng.randint(2, 7) if poly else 2
    pts = [(rng.randint(-1024, 1023), rng.randint(-1024, 1023)) for _ in range(n)]
    cols = [tuple(rng.randrange(256) for _ in range(3)) for _ in range(n if gouraud else 1)]
    words = [(cmd << 24) | col(cols[0])]
    for v in range(n):
        if gouraud and v:
            words.append(col(cols[v]))
        words.append(pack_xy(*pts[v]))
    if poly:
        words.append(0x55555555)
    return words, dict(kind="line", points=pts, colors=cols, semi=semi)


def rect(rng):
    size, tex, semi = rng.randrange(4), rng.random() < 0.5, rng.random() < 0.5
    cmd = 0x60 | (size << 3) | (0x04 if tex else 0) | (0x02 if semi else 0)
    x, y = rng.randint(-1024, 1023), rng.randint(-1024, 1023)
    c = tuple(rng.randrange(256) for _ in range(3))
    clut = rng.randrange(0x10000)
    words = [(cmd << 24) | col(c), pack_xy(x, y)]
    if tex:
        words.append(rng.randrange(0x10000) | (clut << 16))
    w, h = ((rng.randrange(1024), rng.randrange(512)) if size == 0 else gpu.RECT_SIZES[size])
    if size == 0:
        words.append(w | (h << 16))
    return words, dict(kind="rect", points=[(x, y), (x + w, y), (x + w, y + h), (x, y + h)], colors=[c], semi=semi,
                       textured=tex, clut=gpu.clut_text(clut) if tex else None)


def others(rng):
    k = rng.randrange(6)
    if k == 0:
        x, y, w, h = rng.randrange(1024), rng.randrange(512), rng.randrange(1024), rng.randrange(512)
        c = tuple(rng.randrange(256) for _ in range(3))
        return [0x02000000 | col(c), x | (y << 16), w | (h << 16)], dict(
            kind="fill", vram_rect=(x & 0x3F0, y, ((w & 0x3FF) + 15) & ~15, h))
    if k == 1:
        return [0x80000000] + [rng.randrange(1 << 32) for _ in range(3)], dict(kind="copy")
    if k == 2:
        w, h = rng.randint(1, 9), rng.randint(1, 9)
        return [0xA0000000, rng.randrange(1 << 32), w | (h << 16)] + [0] * ((w * h + 1) // 2), dict(kind="copy")
    if k == 3:
        return [0xC0000000, 0, 0x00010001], dict(kind="copy")
    if k == 4:
        cmd = rng.choice((0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6))
        return [(cmd << 24) | rng.randrange(1 << 24)], dict(kind="env")
    return [0x00000000], dict(kind="other")


class Decode(unittest.TestCase):

    def check(self, p, want):
        for k, v in want.items():
            if k in ("clut", "tpage"):
                if v:
                    self.assertIn(v, p.details)
            else:
                self.assertEqual(v, getattr(p, k), k)

    def test_each_command(self):
        rng = random.Random(1)
        for maker in (polygon, line, rect, others):
            for k in range(300):
                words, want = maker(rng)
                with self.subTest(maker=maker.__name__, case=k, words=["%08X" % w for w in words[:6]]):
                    prims = gpu.decode(0, 0x80100000, words)
                    self.assertEqual(1, len(prims), [p.name for p in prims])
                    self.assertEqual(words, prims[0].words, "the words it takes")
                    self.check(prims[0], want)

    def test_packets_of_several(self):
        rng = random.Random(2)
        for k in range(300):
            items = [rng.choice((polygon, line, rect, others))(rng) for _ in range(rng.randint(2, 6))]
            words = [w for ws, _ in items for w in ws]
            prims = gpu.decode(1, 0x80100000, words)
            with self.subTest(case=k):
                self.assertEqual([ws for ws, _ in items], [p.words for p in prims])
                for p, (_, want) in zip(prims, items):
                    self.check(p, want)

    def test_cut_and_random(self):
        """Cut packets (the capture prints 16 words of each) and random words: decoded without an exception."""
        rng = random.Random(3)
        for k in range(3000):
            if k % 2:
                words = polygon(rng)[0] + line(rng)[0]
                words = words[:rng.randrange(1, len(words) + 1)]
            else:
                words = [rng.randrange(1 << 32) for _ in range(rng.randint(1, 16))]
            prims = gpu.decode(0, 0, words)
            self.assertEqual(words, [w for p in prims for w in p.words][:len(words)])
            for p in prims:
                p.color_text()
                gpu.inside(p, rng.randint(-50, 50), rng.randint(-50, 50))

    def test_inside(self):
        p = gpu.decode(0, 0, [0x28FFFFFF, pack_xy(0, 0), pack_xy(100, 0), pack_xy(0, 50), pack_xy(100, 50)])[0]
        self.assertTrue(gpu.inside(p, 50, 25))
        self.assertFalse(gpu.inside(p, 150, 25))
        ln = gpu.decode(0, 0, [0x40FFFFFF, pack_xy(0, 0), pack_xy(100, 100)])[0]
        self.assertTrue(gpu.inside(ln, 51, 50))
        self.assertFalse(gpu.inside(ln, 80, 20))
        self.assertEqual((-1024, 1023), gpu.xy(pack_xy(-1024, 1023)))


WALKER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define	FRAME_NODES		200000
static uint8_t	host_ram[0x200000];

static bool	RamWord(uint32_t adrs, uint32_t *value)
{
	adrs &= 0x1FFFFC;
	memcpy(value, host_ram + adrs, 4);
	return (true);
}

@ENV@
@WALK@

int	main(int argc, char **argv)
{
	FILE		*f = fopen(argv[1], "rb");
	FrameEnv	env;
	bool		ok;

	fread(host_ram, 1, sizeof(host_ram), f);
	fclose(f);
	memset(&env, 0, sizeof(env));
	ok = WalkList(strtoul(argv[2], NULL, 16), 0, true, &env);
	printf("RESULT %d %d %d %d %d %d %d %d %d\n", ok, env.packets, env.known, (int)env.area[0], (int)env.area[1],
		   (int)env.area[2], (int)env.area[3], (int)env.offset[0], (int)env.offset[1]);
	return (0);
}
"""


class Walk(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        env = cbuild.block("ps1transfer/gdbserver.c", "typedef struct\n{\n\tint\t\t\tpackets;", "// The GPU linked list")
        walk = cbuild.function("ps1transfer/gdbserver.c", "WalkList")     # (GpuCommandWords: in the block above it)
        cls.exe = cbuild.build("walklist_host", WALKER.replace("@ENV@", env).replace("@WALK@", walk))

    def walk(self, ram, start):
        r = subprocess.run([self.exe, write(tmp_path("ram.bin"), bytes(ram)), "%X" % start], capture_output=True,
                           text=True, timeout=30)
        pkts, result = [], None
        for line in r.stdout.splitlines():
            f = line.split()
            if f[0] == "FRMPKT":
                pkts.append((int(f[2], 16), [int(v, 16) for v in f[3:]]))
            elif f[0] == "RESULT":
                result = [int(v) for v in f[1:]]
        return pkts, result

    def table(self, ram, nodes):
        """An ordering table: nodes [(address, words)] linked in order (the last one ends it)."""
        for i, (adrs, words) in enumerate(nodes):
            nxt = nodes[i + 1][0] if i + 1 < len(nodes) else 0xFFFFFF
            struct.pack_into("<I", ram, adrs, (len(words) << 24) | nxt)
            for k, w in enumerate(words):
                struct.pack_into("<I", ram, adrs + 4 + k * 4, w)

    def test_table(self):
        rng = random.Random(4)
        ram = bytearray(0x200000)
        nodes, adrs, want = [], 0x100000, []
        env = [0xE3000000 | 16 | (8 << 10), 0xE4000000 | 300 | (230 << 10), 0xE5000000 | (5 & 0x7FF) | ((-7 & 0x7FF) << 11)]
        nodes.append((0x0F0000, env))
        for k in range(400):
            # (random packets: their data words can start with E3-E5: only the commands count; a later env too)
            words = [] if k % 7 == 0 else [w for _ in range(rng.randint(1, 3))
                                           for w in rng.choice((polygon, line, rect, others))(rng)[0]
                                           if k % 50 or True]
            nodes.append((adrs, words))
            adrs = rng.choice((adrs + 4 + len(words) * 4 + rng.randrange(0, 64, 4), rng.randrange(0x10000, 0x1F0000, 4)))
            while any(a <= adrs < a + 4 + len(w) * 4 + 128 for a, w in nodes):
                adrs = rng.randrange(0x10000, 0x1F0000, 4)
        self.table(ram, nodes)
        pkts, res = self.walk(ram, 0x800F0000)
        self.assertEqual(1, res[0], "walked to its end")
        expect = [(0x80000000 | a, w[:16]) for a, w in nodes if w]
        self.assertEqual(expect, pkts)
        # The draw area and offset: the last E3 / E4 / E5 commands (gpu.decode's env commands of whole packets)
        area, offset = [16, 8, 300, 230], [5, -7]
        for a, w in nodes:
            for p in gpu.decode(0, a, w):
                c = p.words[0]
                if p.kind == "env" and c >> 24 == 0xE3:
                    area[0:2] = [c & 0x3FF, (c >> 10) & 0x3FF]
                elif p.kind == "env" and c >> 24 == 0xE4:
                    area[2:4] = [c & 0x3FF, (c >> 10) & 0x3FF]
                elif p.kind == "env" and c >> 24 == 0xE5:
                    offset = [gpu.s11(c), gpu.s11(c >> 11)]
        self.assertEqual([1, len(expect), 1] + area + offset, res)
        decoded = [p for a, w in pkts for p in gpu.decode(0, a, w)]
        self.assertGreaterEqual(len(decoded), len(expect))

    def test_loops_and_bad_links(self):
        ram = bytearray(0x200000)
        self.table(ram, [(0x1000, [0x28000000, 0, 0, 0, 0]), (0x2000, [0x01000000])])
        struct.pack_into("<I", ram, 0x2000, (1 << 24) | 0x1000)            # back to the first: a loop
        pkts, res = self.walk(ram, 0x80001000)
        self.assertEqual(0, res[0], "a loop: not followed forever")
        for bad in (0x1002, 0x200000, 0xFFFFFE):
            struct.pack_into("<I", ram, 0x2000, (1 << 24) | bad)
            pkts, res = self.walk(ram, 0x80001000)
            self.assertEqual(0, res[0], "bad link %X" % bad)
        for start in (0x80001002, 0x80200000):
            self.assertEqual(0, self.walk(ram, start)[1][0])
        struct.pack_into("<I", ram, 0x1FFFF8, (255 << 24) | 0xFFFFFF)       # 255 words at the RAM's end
        pkts, res = self.walk(ram, 0x801FFFF8)
        self.assertEqual(1, res[0])


if __name__ == "__main__":
    unittest.main()
