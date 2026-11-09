"""
SPU RAM Viewer (gui/spu.py): SPU dumps made here (RAM + registers, as ps1transfer -spu gives them) with samples encoded
by the Sound Tools' encoder (looping and not), free memory, silence inside a sample, data that isn't ADPCM, a reverb
work area, and voices: the map (each region's kind, start, end, loop), the warnings (a voice starting in free memory,
inside a sample, in the reverb area; a sample without its end flag, running into the reverb area), the totals, the
samples decoded (= audioconv's decoder), and the Asset Extractor's samples (assetscan.scan_spu).

    python -m unittest tests/tools/test_spu.py -v

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import random
import struct
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolhost                         # noqa: E402,F401

import assetscan                        # noqa: E402
import audioconv                        # noqa: E402
import spu                              # noqa: E402

RAM = 0x80000


def adpcm(n_samples, freq=0.05, loop=False, seed=0):
    """ADPCM blocks of a tone (no VAG header, its leading zero block dropped: as a bank's samples are in SPU RAM)."""
    rng = np.random.default_rng(seed)
    pcm = (np.sin(np.arange(n_samples) * freq) * 12000 + rng.normal(0, 300, n_samples)).astype(np.int16)
    return audioconv.to_vag(pcm, 22050, loop=loop, loop_start=0.01 if loop else 0.0)[48:]     # (its zero block first)


class Dump:
    """An SPU dump being made: RAM, registers."""

    def __init__(self):
        self.ram = bytearray(RAM)
        self.regs = bytearray(512)

    def put(self, adrs, data):
        self.ram[adrs:adrs + len(data)] = data
        return adrs + len(data)

    def voice(self, n, start, pitch=0x1000, env=0x4000, repeat=None):
        struct.pack_into("<hhHHHHhH", self.regs, n * 16, 0x3FFF, 0x3FFF, pitch, start // 8, 0x80FF, 0x5FC4, env,
                         (repeat if repeat is not None else start) // 8)

    def reverb(self, mbase, on=True):
        struct.pack_into("<H", self.regs, 0x1A2, mbase // 8)
        struct.pack_into("<H", self.regs, 0x1AA, 0xC000 | (0x80 if on else 0))

    def bytes(self):
        return bytes(self.ram) + bytes(self.regs)


class Map(unittest.TestCase):

    def test_map_and_warnings(self):
        d = Dump()
        a = adpcm(20000, seed=1)
        b = adpcm(9000, loop=True, seed=2)
        c = adpcm(5000, seed=3)
        s1 = 0x1010
        e1 = d.put(s1, a)
        s2 = e1 + 0x800                                  # (free memory between)
        e2 = d.put(s2, b)
        s3 = e2
        silent = bytearray(c)
        silent[160:160 + 1024] = bytes(1024)             # 1 KB of silence inside it: still one sample
        e3 = d.put(s3, silent)
        junk = s3 + len(c) + 0x400
        d.put(junk, bytes([0xFF] * 0x800))               # not ADPCM
        s4 = junk + 0x1000
        cut = bytearray(adpcm(4000, seed=4))
        cut[-15] &= ~1                                    # its end flag gone, free memory after it
        e4 = d.put(s4, cut)
        mbase = 0x70000
        s5 = mbase - 0x400
        into = bytearray(adpcm(3000, seed=5))
        into[-15] &= ~1
        d.put(s5, into)                                  # runs into the reverb work area
        d.reverb(mbase)
        d.voice(0, s1)
        d.voice(1, s2, repeat=s2)
        d.voice(2, s1 + 0x100)                           # inside sample 1
        d.voice(3, e1 + 0x100)                           # free memory
        d.voice(4, mbase + 0x100)                        # reverb area
        d.voice(5, s3, env=0)                            # not playing: no warning
        d.voice(6, s2 + 16)                              # the block after its zero one: its sound's start
        m = spu.Spu(d.bytes())
        samples = [r for r in m.regions if r.kind == "sample"]
        starts = [r.start for r in samples]
        for s in (s1, s2, s3, s4):
            self.assertIn(s, starts, [(r.kind, hex(r.start), hex(r.end)) for r in m.regions])
        r1 = m.region_at(s1)
        self.assertEqual(e1, r1.end)
        self.assertFalse(r1.loops)
        self.assertEqual("free", m.region_at(e1 + 0x10).kind)
        r2 = m.region_at(s2)
        self.assertEqual((e2, True), (r2.end, r2.loops))
        self.assertEqual(e3, m.region_at(s3).end, "silence inside a sample: still that sample")
        self.assertEqual("data", m.region_at(junk + 0x10).kind)
        self.assertTrue(any("No end flag" in w for w in m.region_at(s4).warnings), m.region_at(s4).warnings)
        self.assertTrue(any("reverb work area" in w for w in m.region_at(s5).warnings), m.region_at(s5).warnings)
        self.assertEqual("reverb", m.region_at(mbase + 0x10).kind)
        self.assertTrue(any("voice 2" in w and "into it" in w for w in r1.warnings), r1.warnings)
        self.assertTrue(any("voice 3" in w and "free" in w for w in m.region_at(e1 + 0x100).warnings))
        self.assertTrue(any("voice 4" in w for w in m.region_at(mbase + 0x100).warnings))
        self.assertFalse(any("voice 5" in w or "voice 6" in w for r in m.regions for w in r.warnings),
                         [w for r in m.regions for w in r.warnings])
        self.assertEqual([0, 2], [v for v in r1.voices if v in (0, 2)])
        t = m.totals()
        self.assertEqual(RAM - mbase, t["reverb"])
        self.assertEqual(sum(r.size for r in m.regions if r.kind == "free"), t["free"])
        # Every byte in exactly one region, in order
        self.assertEqual(0, m.regions[0].start)
        for x, y in zip(m.regions, m.regions[1:]):
            self.assertEqual(x.end, y.start)
        self.assertEqual(RAM, m.regions[-1].end)

    def test_decode(self):
        """spu.py's decoder = audioconv's (two decoders of the same SPU ADPCM)."""
        d = Dump()
        a = adpcm(12000, seed=7)
        d.put(0x2000, a)
        m = spu.Spu(d.bytes())
        r = m.region_at(0x2000)
        mine = np.array(m.decode(r), np.int16)
        ref, _ = audioconv.decode_adpcm(a)
        self.assertTrue(np.array_equal(ref[:len(mine)], mine[:len(ref)]))

    def test_loop_decode(self):
        """A looping sample played again: as the SPU plays it (the loop part decoded again from its history)."""
        d = Dump()
        b = adpcm(9000, loop=True, seed=8)
        d.put(0x2000, b)
        m = spu.Spu(d.bytes())
        r = m.region_at(0x2000)
        once = m.decode(r)
        twice = np.array(m.decode(r, loops=1), np.int32)
        ref, loop = audioconv.decode_adpcm(b, loops=1)
        n = min(len(ref), len(twice))
        self.assertGreater(len(twice), len(once))
        err = np.abs(ref[:n].astype(np.int32) - twice[:n]).max()
        self.assertLess(err, 200, "the loop played again differs by %d" % err)

    def test_scan_spu(self):
        d = Dump()
        a, b = adpcm(8000, seed=9), adpcm(30, seed=10)       # (b: a few blocks, no voice: left out)
        e = d.put(0x1010, a)                                 # (its zero block after the capture buffers' end)
        d.put(e + 0x2000, b)
        d.voice(0, 0x1010, pitch=0x0800)
        assets = assetscan.scan_spu(d.bytes())
        self.assertEqual(1, len(assets), [a_.info for a_ in assets])
        self.assertIn("22050 Hz", assets[0].info)
        self.assertEqual(a, assets[0].data[48:])

    def test_random(self):
        rng = random.Random(1)
        for k in range(40):
            d = Dump()
            for _ in range(rng.randint(0, 30)):
                o = rng.randrange(0, RAM - 16) & ~15
                d.put(o, bytes(rng.randrange(256) for _ in range(rng.choice((16, 64, 2048)))))
            for n in range(24):
                d.voice(n, rng.randrange(RAM) & ~7, env=rng.randrange(2) * 100)
            if k % 2:
                d.reverb(rng.randrange(0x1000, RAM) & ~7, on=bool(k % 4))
            m = spu.Spu(d.bytes())
            m.totals()
            for r in m.regions:
                r.name()
                if r.kind == "sample":
                    m.decode(r, loops=1)
            assetscan.scan_spu(d.bytes())
        with self.assertRaises(ValueError):
            spu.Spu(bytes(100))


if __name__ == "__main__":
    unittest.main()
