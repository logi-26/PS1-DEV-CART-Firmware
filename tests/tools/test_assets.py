"""
Asset Extractor and VRAM Viewer: TIM files (each depth, with CLUT rows) made here from the format, read by the VRAM
Viewer (pages/tim.py Tim) and the scanner (assetscan.tim_image) against a reference decoding; the real TIMs of
PS1_TIM_DIR (both readers agree); the scanner (assetscan.scan_bytes) on data with TIM, VAB, VH (its samples found in
SPU RAM), SEQ, SEP, VAG and TMD files put in real program bytes: each found at its offset with its bytes, none made up
in the programs alone; the textures a game draws (assetscan.textures_in_use) from GPU packets in RAM and its VRAM;
damaged files and random data (no exception but the ones the pages catch); games' music (SQ/HD and Crystal
Dynamics' SND, with the test files of tests/sound) found in other data and in a BIGFILE.DAT, saved as found and
converted for the tester.

    python -m unittest tests/tools/test_assets.py -v

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import glob
import io
import os
import random
import struct
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from toolhost import ROOT, tmp_path, write              # noqa: E402

import assetscan                                        # noqa: E402
import audioconv                                        # noqa: E402
from pages.tim import Tim                               # noqa: E402

VRAM_W, VRAM_H = 1024, 512


# ---------------------------------------------------------------------------------------------------- TIM

def ps_color(v):
    return ((v & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, ((v >> 10) & 31) * 255 // 31)


def make_tim(rng, mode, w_px, h, rows=1, x=None, y=None):
    """A TIM (bytes) of mode 0-3 (4, 8, 15, 24 bit) w_px x h pixels, its CLUT of rows rows; and its reference RGB
    (each CLUT row's: list of arrays)."""
    per = (4, 2, 1, None)[mode]
    if mode == 3:
        w_px -= w_px % 2
        wv = w_px * 3 // 2
    else:
        w_px -= w_px % per
        wv = w_px // per
    x = rng.randrange(0, VRAM_W - wv + 1) if x is None else x
    y = rng.randrange(0, VRAM_H - h + 1) if y is None else y
    out = struct.pack("<II", 0x10, mode | (8 if mode < 2 else 0))
    colors = None
    if mode < 2:
        n = 16 if mode == 0 else 256
        colors = [[rng.randrange(0x10000) for _ in range(n)] for _ in range(rows)]
        cx, cy = rng.randrange(0, VRAM_W - n + 1, 16), rng.randrange(0, VRAM_H - rows + 1)
        clut = b"".join(struct.pack("<%dH" % n, *r) for r in colors)
        out += struct.pack("<IHHHH", 12 + len(clut), cx, cy, n, rows) + clut
    if mode == 0:
        idx = np.array([[rng.randrange(16) for _ in range(w_px)] for _ in range(h)], np.uint8)
        raw = bytes((idx[:, 0::2] | (idx[:, 1::2] << 4)).astype(np.uint8).tobytes())
    elif mode == 1:
        idx = np.array([[rng.randrange(256) for _ in range(w_px)] for _ in range(h)], np.uint8)
        raw = idx.tobytes()
    elif mode == 2:
        px = np.array([[rng.randrange(0x10000) for _ in range(w_px)] for _ in range(h)], np.uint16)
        raw = px.astype("<u2").tobytes()
    else:
        rgb = np.array([[[rng.randrange(256) for _ in range(3)] for _ in range(w_px)] for _ in range(h)], np.uint8)
        raw = rgb.tobytes()
    out += struct.pack("<IHHHH", 12 + len(raw), x, y, wv, h) + raw
    refs = []
    if mode < 2:
        for r in colors:
            lut = np.array([ps_color(v) for v in r], np.uint8)
            refs.append(lut[idx])
    elif mode == 2:
        five = px.astype(np.int32)
        refs.append(np.stack([(five & 31), (five >> 5) & 31, (five >> 10) & 31], -1))   # 5-bit values
    else:
        refs.append(rgb)
    return out, refs


def same_image(test, img, ref, mode, what):
    a = np.asarray(img.convert("RGB"))
    if mode == 2:                       # (15-bit: 5 bits a component, scaled either way to 8 bits)
        test.assertTrue(np.array_equal(a >> 3, ref.astype(np.uint8)), what)
    else:
        test.assertTrue(np.array_equal(a, ref), what)


class Tims(unittest.TestCase):

    def test_each_depth(self):
        rng = random.Random(1)
        for mode in range(4):
            for k in range(12):
                w, h = rng.randint(1, 80) * 4, rng.randint(1, 60)
                rows = rng.randint(1, 4) if mode < 2 else 0
                data, refs = make_tim(rng, mode, max(w, 4), h, max(rows, 1))
                with self.subTest(mode=mode, case=k):
                    t = Tim(write(tmp_path("t.tim"), data))
                    self.assertEqual(refs[0].shape[1], t.width)
                    self.assertEqual(len(refs) if mode < 2 else 0, t.rows)
                    for r, ref in enumerate(refs):
                        same_image(self, t.image(r), ref, mode, "VRAM Viewer, row %d" % r)
                        same_image(self, assetscan.tim_image(data, r), ref, mode, "scanner, row %d" % r)
                    self.assertEqual(len(data), assetscan.tim_at(data, 0)[0], "the scanner's size")
                    self.assertEqual(bytes(data), bytes(t.bytes_with_colors()), "saved unchanged")

    def test_palette_edit_and_move(self):
        rng = random.Random(2)
        data, refs = make_tim(rng, 1, 64, 32, 2)
        p = write(tmp_path("e.tim"), data)
        t = Tim(p)
        t.colors[5] = 0x7FFF
        t.move("img", 640, 256)
        t.move("clut", 0, 480)
        out = tmp_path("e2.tim")
        t.save(out)
        t2 = Tim(out)
        self.assertEqual(0x7FFF, t2.colors[5])
        self.assertEqual((640, 256), t2.img[:2])
        self.assertEqual((0, 480), t2.clut[:2])
        self.assertEqual(t.colors[6:], t2.colors[6:])

    def test_real_tims(self):
        folder = os.environ.get("PS1_TIM_DIR")
        files = sorted(glob.glob(os.path.join(folder, "*.tim")))[:120] if folder else []
        if not files:
            self.skipTest("PS1_TIM_DIR not set (a folder of TIM files)")
        for f in files:
            with self.subTest(tim=os.path.basename(f)):
                with open(f, "rb") as fh:
                    data = fh.read()
                t = Tim(f)
                for r in range(max(1, t.rows)):
                    a = np.asarray(t.image(r))
                    b = np.asarray(assetscan.tim_image(data, r))
                    self.assertTrue(np.array_equal(a, b), "the two readers agree, row %d" % r)

    def test_damaged(self):
        rng = random.Random(3)
        bad = []
        for k in range(400):
            data, _ = make_tim(rng, rng.randrange(4), 32, 16, 2)
            d = bytearray(data)
            if k % 3 == 0:
                d = d[:rng.randrange(1, len(d))]
            for _ in range(rng.randint(1, 6)):
                if len(d) > 4:
                    d[rng.randrange(4, len(d))] = rng.randrange(256)
            p = write(tmp_path("d.tim"), d)
            try:
                t = Tim(p)
                t.image(0)
            except (ValueError, struct.error, OSError):      # (what the page catches)
                pass
            except Exception as e:      # noqa: BLE001
                bad.append("Tim: %s: %s" % (type(e).__name__, e))
            try:
                if assetscan.tim_at(bytes(d), 0):
                    assetscan.tim_image(bytes(d))
                    assetscan.tim_info(bytes(d))
            except Exception as e:      # noqa: BLE001
                bad.append("scanner: %s: %s" % (type(e).__name__, e))
        self.assertEqual([], sorted(set(bad))[:8])


# ---------------------------------------------------------------------------------------------------- scanner

def program_bytes():
    """Real program bytes (the repository's PS1 executables): code and data, as a game's RAM has."""
    out = b""
    for p in sorted(glob.glob(os.path.join(ROOT, "ps1-tools", "*", "*.ps-exe"))):
        with open(p, "rb") as f:
            out += f.read()[2048:]
    return out


def make_seq(rng, events=40):
    body = b"\x00\xC0\x01"
    for i in range(events):
        body += b"\x00" + bytes((0x90, 40 + i % 40, 100)) + bytes((rng.randrange(1, 100),)) + bytes((40 + i % 40, 0))
    body += b"\x00\xFF\x51\x07\xA1\x20\x00\xFF\x2F\x00"
    return b"pQES" + struct.pack(">IH", 1, 480) + struct.pack(">I", 500000)[1:] + b"\x04\x02" + body


def make_sep(rng, n=3):
    out = b"pQES" + b"\x00\x00"
    for i in range(n):
        seq = make_seq(rng, 10 + i)[15:]
        out += struct.pack(">HH", i, 480) + struct.pack(">I", 500000)[1:] + b"\x04\x02" + struct.pack(">I", len(seq)) + seq
    return out


def make_bank(rng, programs=3):
    tones = []
    for p in range(programs):
        pcm = (np.sin(np.arange(3000 + p * 500) * (0.05 + p * 0.01)) * 12000).astype(np.int16)
        tones.append(dict(program=p, pcm=pcm, rate=22050, root=60, cents=0, min=0, max=127, vol=100, pan=64,
                          adsr1=0x80FF, adsr2=0x5FC4, reverb=False, loop=False, loop_start=0.0))
    return audioconv.build_vab(tones)


def make_tmd(rng, nv=12, npr=6):
    """A TMD: one object, its primitives, vertices and normals after the object table."""
    prims = b"".join(struct.pack("<BBBB", 4, 3, 0, 0x20) + struct.pack("<HHHH", 0, 0, 1, 2) for _ in range(npr))
    verts = b"".join(struct.pack("<hhhh", rng.randint(-500, 500), rng.randint(-500, 500), rng.randint(-500, 500), 0)
                     for _ in range(nv))
    norms = b"".join(struct.pack("<hhhh", 0, 4096, 0, 0) for _ in range(nv))
    table = 28
    pt, vt = table, table + len(prims)
    nt = vt + len(verts)
    obj = struct.pack("<6Ii", vt, nv, nt, nv, pt, npr, 0)
    return struct.pack("<III", 0x41, 0, 1) + obj + prims + verts + norms


class Scanner(unittest.TestCase):

    def test_planted(self):
        rng = random.Random(5)
        noise = bytearray(program_bytes())
        self.assertGreater(len(noise), 100000)
        before = assetscan.scan_bytes(bytes(noise), "file.bin")
        self.assertLessEqual(len(before), 3, "made up in program bytes: %s" % [(a.kind, a.info) for a in before])
        noise_found = {(a.kind, a.offset) for a in before}
        vh, vb = make_bank(rng)
        files = [("TIM", make_tim(rng, m, 64, 32, 1)[0]) for m in range(4)]
        files += [("VAB", vh + vb), ("SEQ", make_seq(rng)), ("SEP", make_sep(rng)),
                  ("VAG", audioconv.to_vag((np.sin(np.arange(5000) * 0.1) * 9000).astype(np.int16), 22050,
                                           name="planted")),
                  ("TMD", make_tmd(rng))]
        data, where = bytearray(), []
        pos = 0
        for kind, f in files:
            gap = rng.randrange(1000, 20000) & ~3
            data += noise[pos:pos + gap]
            pos += gap
            where.append((kind, len(data), f))
            data += f
            data += bytes((-len(data)) % 4)
        data += noise[pos:pos + 5000]
        found = {(a.kind, a.offset): a for a in assetscan.scan_bytes(bytes(data), "file.bin")}
        for kind, off, f in where:
            with self.subTest(kind=kind):
                self.assertIn((kind, off), found, "found: %s" % sorted(found))
                a = found[(kind, off)]
                self.assertEqual(f if kind != "SEQ" else f, a.data[:len(f)] if kind in ("SEQ", "SEP") else a.data)
        extra = [k for k in found if k not in {(w[0], w[1]) for w in where}]
        self.assertLessEqual(len(extra), len(noise_found) + 1, "extra finds: %s" % extra)

    def test_vh_samples_in_spu(self):
        """A VH alone in RAM (its samples sent to the SPU): found again in the SPU RAM, the VAB put together."""
        rng = random.Random(6)
        vh, vb = make_bank(rng, 5)
        ram = bytearray(program_bytes()[:0x40000])
        ram[0x10000:0x10000 + len(vh)] = vh
        spu = bytearray(rng.randrange(256) for _ in range(512 * 1024))
        spu[0x1010:0x1010 + len(vb)] = vb
        for k in range(0x1010 + len(vb), len(spu), 16):         # (other "samples" after: no end flag in them)
            spu[k + 1] = 0
        found = [a for a in assetscan.scan_bytes(bytes(ram), "RAM", bytes(spu)) if a.offset == 0x10000]
        self.assertEqual(1, len(found))
        self.assertEqual("VAB", found[0].kind)
        self.assertEqual(vb, found[0].parts[".vb"])
        self.assertIn("0x01010", found[0].info)

    def test_random_data(self):
        rng = random.Random(7)
        for k in range(30):
            d = bytes(rng.randrange(256) for _ in range(rng.randint(10, 20000)))
            if k % 2:       # (headers in it: their checks on random data)
                d = bytearray(d)
                for magic in (b"\x10\0\0\0", b"pBAV", b"pQES", b"VAGp", b"\x41\0\0\0"):
                    o = rng.randrange(0, max(1, len(d) - 8)) & ~3
                    d[o:o + 4] = magic
                d = bytes(d)
            assetscan.scan_bytes(d, "x", d[:512 * 1024] if k % 3 == 0 else None)

    def test_random_data_game_music(self):
        """The games' music IDs in random data (their checks: no exception, nothing made up)."""
        rng = random.Random(11)
        for k in range(20):
            d = bytearray(rng.randrange(256) for _ in range(rng.randint(100, 30000)))
            for magic in (b"SSsq", b"SShd", b"DNSa", b"PMSa", b"QESa"):
                o = rng.randrange(12, max(13, len(d) - 8))
                d[o:o + 4] = magic
            found = assetscan.game_music(bytes(d), "x")
            self.assertEqual([], [a.kind for a in found if a.kind == "SND"])


# ---------------------------------------------------------------------------------------------------- games' music

class GameMusic(unittest.TestCase):
    """SQ/HD (sshd.py) and Crystal Dynamics' SND (aad.py) found by the scanner: the test files of their converters'
    tests (tests/sound) put in other data."""

    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, os.path.join(ROOT, "tests", "sound"))
        import test_aad
        import test_sshd
        cls.aad, cls.sshd = test_aad, test_sshd

    def save(self, asset):
        folder, out = tmp_path("assets_music"), {}
        for p in asset.save(folder):
            with open(p, "rb") as f:
                out[os.path.basename(p)[len(asset.name()) - len(asset.ext):]] = f.read()
        return out

    def test_sq(self):
        t = self.sshd
        sq = t.sq_file([((0x90, 30, 100), 240), ((0x80, 30, 0), 0), ((0x90, 70, 100), 240), ((0x80, 70, 0), 0)],
                       programs=(0,))
        junk = bytes(range(7, 251)) * 9                 # (a multiple of 4 bytes)
        d = junk + t.ScanTests.pad(sq) + t.ScanTests.pad(t.ScanTests.HD2) + t.ScanTests.BD2 + junk
        found = [a for a in assetscan.scan_bytes(d, "MUSIC.DAT") if a.kind == "SQ"]
        self.assertEqual(1, len(found))
        a = found[0]
        self.assertEqual((len(junk), sq), (a.offset, a.data))
        self.assertIn("its bank: 2 programs", a.info)
        files = self.save(a)
        self.assertEqual({".sq", ".seq", ".hd", ".bd", ".vh", ".vb"}, set(files))
        self.assertEqual((t.ScanTests.HD2, t.ScanTests.BD2), (files[".hd"], files[".bd"]))
        self.assertEqual(b"pQES", files[".seq"][:4])
        self.assertEqual(b"pBAV", files[".vh"][:4])

    def test_snd_stored_as_is(self):
        t = self.aad
        d = b"\x11" * 5001 + t.GEX + b"\x22" * 777 + t.SMP + b"\x33" * 300
        found = [a for a in assetscan.scan_bytes(d, "RAM") if a.kind == "SND"]
        self.assertEqual(1, len(found))
        a = found[0]
        self.assertEqual(5001, a.offset)
        self.assertEqual(t.GEX[:len(a.data)], a.data)  # (to its last end command)
        self.assertIn("2 programs, 1 music;", a.info)
        files = self.save(a)
        self.assertEqual({".snd", ".smp", ".vh", ".vb", "_1.seq"}, set(files))
        self.assertEqual(t.SMP, files[".smp"])
        self.assertEqual(b"pQES", files["_1.seq"][:4])

    def test_snd_in_archive(self):
        t = self.aad
        big = t.bigfile([b"TIM junk" * 50, t.GEX, t.SMP, t.AKUJI, t.SMP], packed=(1, 3))
        found = [a for a in assetscan.scan_bytes(big, "BIGFILE.DAT") if a.kind == "SND"]
        self.assertEqual(2, len(found))
        self.assertEqual([t.GEX, t.AKUJI], [a.data for a in found])     # (unpacked, as stored)
        self.assertIn("archive file 001", found[0].info)
        self.assertEqual(0x800 * 2, found[0].offset)    # (where the archive stores it: after the junk file)
        self.assertEqual(t.SMP, self.save(found[1])[".smp"])

    def test_sound_effects(self):
        snf, smf = self.aad.sfx_pair()
        found = [a for a in assetscan.scan_bytes(b"\x11" * 100 + snf + b"\x22" * 30 + smf, "x") if a.kind == "SFX"]
        self.assertEqual(1, len(found))
        self.assertEqual((100, snf), (found[0].offset, found[0].data))
        self.assertIn("3 effects, 2 samples", found[0].info)
        files = self.save(found[0])
        self.assertEqual({".snf", ".smf", ".vh", ".vb", ".seq"}, set(files))
        self.assertEqual(smf, files[".smf"])

    def test_no_samples(self):
        found = [a for a in assetscan.scan_bytes(b"\x11" * 64 + self.aad.GEX, "x") if a.kind == "SND"]
        self.assertIn("samples not found", found[0].info)
        self.assertEqual({".snd", "_1.seq"}, set(self.save(found[0])))


# ---------------------------------------------------------------------------------------------------- textures

class Textures(unittest.TestCase):

    def test_drawn_textures(self):
        """GPU packets in RAM (an ordering table: a draw mode, then sprites; textured polygons) over a VRAM: the
        textures they draw cut from it, each with its CLUT."""
        rng = random.Random(8)
        vram = np.array([rng.randrange(0x10000) for _ in range(VRAM_W * VRAM_H)], np.uint16).reshape(VRAM_H, VRAM_W)
        ram = bytearray(program_bytes()[:0x100000].ljust(0x100000, b"\0"))
        nodes, adrs = [], 0x80000
        # 4-bit page 5,0 (x 320), CLUT at 0,480; 8-bit page 8,1 (x 512, y 256), CLUT at 256,500; a 16-bit page 12,0
        tp4, clut4 = 5 | (0 << 7), (0 >> 4) | (480 << 6)
        tp8, clut8 = 8 | (1 << 4) | (1 << 7), (256 >> 4) | (500 << 6)
        tp16 = 12 | (2 << 7)
        nodes.append([0xE1000000 | tp4])                                                   # draw mode
        nodes.append([0x64808080, 0x00100010, (clut4 << 16) | (8 << 8) | 16, 0x00100020])   # sprite 32x16 at u16 v8
        nodes.append([0x7C808080, 0x00200020, (clut4 << 16) | (8 << 8) | 48])              # 16x16 at u48 v8: touches
        nodes.append([0x24808080, 0x00000000, (clut8 << 16) | (0 << 8) | 0, 0x00000010,
                      (tp8 << 16) | (0 << 8) | 63, 0x00100000, (40 << 8) | 0])               # textured triangle
        nodes.append([0x2C808080, 0, (0 << 16) | 0, 0, (tp16 << 16) | 31, 0, (15 << 8), 0, (15 << 8) | 31])
        for i, words in enumerate(nodes):
            nxt = (adrs + 0x40) if i + 1 < len(nodes) else 0xFFFFFF
            struct.pack_into("<I", ram, adrs, (len(words) << 24) | nxt)
            for k, w in enumerate(words):
                struct.pack_into("<I", ram, adrs + 4 + k * 4, w)
            adrs += 0x40
        struct.pack_into("<I", ram, 0x7FFC0, (0 << 24) | 0x80000)                          # the table's start
        found = assetscan.textures_in_use(bytes(ram), vram.astype("<u2").tobytes())
        rects = {(a.info.split(" in VRAM")[0].split(", at ")[1], a.info.split("-bit")[0].split()[-1]): a for a in found}
        self.assertIn(("324,8", "4"), rects, [a.info for a in found])
        a4 = rects[("324,8", "4")]
        self.assertTrue(a4.info.startswith("48x16 4-bit"), a4.info)          # u 16..63 (two sprites merged), v 8..23
        self.assertIn("CLUT 0,480", a4.info)
        self.assertIn("drawn by 2 primitives", a4.info)
        # The TIM cut: its pixels = the VRAM's there, its CLUT = the VRAM's CLUT row
        t = Tim(write(tmp_path("tex.tim"), a4.data))
        self.assertEqual([int(v) for v in vram[480, 0:16]], t.colors)
        x, y, w, h = t.img
        self.assertEqual((324, 8, 12, 16), (x, y, w, h))
        raw = np.frombuffer(bytes(a4.data[t.img_offset:t.img_offset + w * h * 2]), "<u2").reshape(h, w)
        self.assertTrue(np.array_equal(vram[y:y + h, x:x + w], raw))
        self.assertIn(("512,256", "8"), rects, [a.info for a in found])
        self.assertTrue(rects[("512,256", "8")].info.startswith("64x41 8-bit"), rects[("512,256", "8")].info)
        self.assertIn(("768,0", "16"), rects)


if __name__ == "__main__":
    unittest.main()
