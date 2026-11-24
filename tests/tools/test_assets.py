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
import tempfile
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

    @staticmethod
    def bgm_file(rng, sub=False, footer=None, empty2=False, silent=()):
        """A Resident Evil 2 music file, as its loader reads it (Bio2/prog/SND.C, Snd_bgm_main / Snd_bgm_sub of
        github.com/MeganGrass/bio2-ds): MAINnn.BGM a SEQ, the VH, the VB, the words VB VH SEQ; SUB_nn.BGM a SEQ at 0,
        the VH, a second SEQ (empty2: no notes, as most of the game's), the VB, the words VB VH SEQ2 and one more.
        silent: programs at volume 0 (the game sets them for each room). (vh, vb, file)"""
        vh, vb = make_bank(rng, 3)
        vh = bytearray(vh)
        for prog in silent:
            vh[32 + prog * 16 + 1] = 0
        vh = bytes(vh)
        seq, seq2 = make_seq(rng), make_seq(rng, 20)
        if empty2:
            seq2 = seq2[:15] + bytes((0, 0xFF, 0x51, 0x07, 0xA1, 0x20, 0, 0xFF, 0x2F, 0))
        head = seq + bytes((-len(seq)) % 16)
        vh_off = len(head)
        head += vh
        seq2_off = len(head)
        if sub:
            head += seq2 + bytes((-len(seq2)) % 16)
        vb_off = len(head)
        words = footer or ((vb_off, vh_off, seq2_off, 0) if sub else (vb_off, vh_off, 0))
        return vh, vb, head + vb + struct.pack("<%dI" % len(words), *words)

    def test_re2_music_files(self):
        """Resident Evil 2's MAINnn.BGM and SUB_nn.BGM: the VH with its VB (from the file's last words), the SEQs."""
        for sub in (False, True):
            with self.subTest(sub=sub):
                vh, vb, data = self.bgm_file(random.Random(12), sub)
                found = assetscan.scan_bytes(data, "SUB_00.BGM" if sub else "MAIN00.BGM")
                vabs = [a for a in found if a.kind == "VAB"]
                self.assertEqual(1, len(vabs), [(a.kind, a.info) for a in found])
                self.assertEqual(vh, vabs[0].data)
                self.assertEqual(vb, vabs[0].parts[".vb"])
                self.assertIn("Resident Evil 2", vabs[0].info)
                seqs = [a for a in found if a.kind == "SEQ"]
                self.assertEqual(2 if sub else 1, len(seqs))
                self.assertEqual([True, True] if sub else [False],
                                 [("SEQ %d of 2" % (i + 1)) in a.info for i, a in enumerate(seqs)])
                self.assertIn("40 notes", seqs[0].info)

    def test_re2_layers_and_empty_seq(self):
        """A bank's programs at volume 0 (the game raises them for each room): at 127 in the VH saved, the others as
        they are; a SUB file's second SEQ with no notes: left out, the first one alone."""
        vh, vb, data = self.bgm_file(random.Random(15), sub=True, empty2=True, silent=(0, 2))
        found = assetscan.scan_bytes(data, "SUB_07.BGM")
        vab = next(a for a in found if a.kind == "VAB")
        self.assertEqual([127, vh[32 + 16 + 1], 127], [vab.data[32 + p * 16 + 1] for p in range(3)])
        self.assertEqual(vh[:32 + 1] + vh[32 + 2:32 + 16 * 2 + 1], vab.data[:32 + 1] + vab.data[32 + 2:32 + 16 * 2 + 1])
        self.assertEqual(vh[32 + 16 * 3:], vab.data[32 + 16 * 3:])
        self.assertIn("2 programs at volume 0", vab.info)
        seqs = [a for a in found if a.kind == "SEQ"]
        self.assertEqual([0], [a.offset for a in seqs])
        self.assertNotIn("of 2", seqs[0].info)
        vh2, _, data = self.bgm_file(random.Random(15))
        vab = next(a for a in assetscan.scan_bytes(data, "MAIN01.BGM") if a.kind == "VAB")
        self.assertEqual(vh2, vab.data)                         # (no program at 0: as it is)
        self.assertNotIn("volume 0", vab.info)

    def test_re2_music_files_damaged(self):
        """Footers that don't fit (offsets past the end, no VH or SEQ there, too short): the VH alone, no exception."""
        rng = random.Random(13)
        vh, vb, good = self.bgm_file(rng)
        n, (vb_off, vh_off, seq_off) = len(good), struct.unpack("<3I", good[-12:])
        self.assertEqual((vh_off, vb_off, [seq_off]), assetscan.bgm_banks(good))
        for footer in ((n + 16, vh_off, seq_off), (vb_off, vh_off + 4, seq_off), (vb_off, vh_off, seq_off + 4),
                       (vh_off - 16, vh_off, seq_off), (0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF)):
            with self.subTest(footer=footer):
                data = good[:-12] + struct.pack("<3I", *footer)
                found = assetscan.scan_bytes(data, "MAIN00.BGM")
                self.assertEqual([], [a for a in found if a.kind == "VAB" and "Resident" in a.info])
        for short in (b"", b"pBAV", good[-15:]):
            self.assertIsNone(assetscan.bgm_banks(short))
        self.assertIsNone(assetscan.bgm_banks(vh + vb))          # (an ordinary VAB: no footer)

    def test_re2_effect_banks_paired(self):
        """The disc scan: Resident Evil 2's NAME.EDH (a table, then the VH) and its NAME.VB beside it, a VAB."""
        from pages.gamedata import GameDataPage     # noqa: E402
        vh, vb = make_bank(random.Random(14), 2)
        edh = bytes(range(256)) * 3 + vh + struct.pack("<II", 768, 0)
        with tempfile.TemporaryDirectory() as folder:
            os.makedirs(os.path.join(folder, "COMMON", "SOUND"))
            for name, data in (("ARMS01.EDH", edh), ("ARMS01.VB", vb), ("ARMS02.EDH", edh), ("ARMS02.VB", vb[:-16])):
                with open(os.path.join(folder, "COMMON", "SOUND", name), "wb") as f:
                    f.write(data)
            found = []
            for name in ("ARMS01.EDH", "ARMS02.EDH"):
                found += [a for a in assetscan.scan_bytes(edh, name) if a.kind in ("VH", "VAB")]
            self.assertEqual(["VH", "VH"], [a.kind for a in found])
            GameDataPage.pair_banks({"found": found, "folder": folder})
        self.assertEqual("VAB", found[0].kind)
        self.assertEqual(vb, found[0].parts[".vb"])
        self.assertEqual("VH", found[1].kind)                   # (its VB a block short: not its samples)

    def test_disc_music(self):
        """Sound Tools' game music scan of a disc image (.bin): every SEQ with the bank it plays with: in its own file
        (Resident Evil 2's MAINnn.BGM, SUB_nn.BGM: two SEQs), NAME.VH + NAME.VB or NAME.VAB beside NAME.SEQ, none
        (a SEQ alone: listed without a bank); its length and how many of its notes have a tone."""
        sys.path.insert(0, os.path.join(ROOT, "tests", "sound"))
        from test_aad import iso, raw       # noqa: E402 (an ISO 9660 image made in the tests)
        rng = random.Random(31)
        vh1, vb1, main = self.bgm_file(rng)
        vh2, vb2, sub = self.bgm_file(rng, sub=True, silent=(0,))
        vh3, vb3 = make_bank(rng, 3)
        vh4, vb4 = make_bank(rng, 2)
        files = [("MAIN01.BGM", main), ("SUB_02.BGM", sub), ("SONG.SEQ", make_seq(rng)), ("SONG.VH", vh3),
                 ("SONG.VB", vb3), ("TUNE.SEQ", make_seq(rng, 12)), ("TUNE.VAB", vh4 + vb4),
                 ("ALONE.SEQ", make_seq(rng, 5)), ("MOVIE.STR", make_seq(rng))]
        path = tmp_path("music.bin")
        write(path, raw(iso(files)))
        found = assetscan.disc_music(path)
        self.assertEqual(["MAIN01", "SUB_02_1", "SUB_02_2", "SONG", "TUNE", "ALONE"], [m["name"] for m in found])
        by = {m["name"]: m for m in found}
        self.assertEqual((vh1, vb1), (by["MAIN01"]["vh"], by["MAIN01"]["vb"]))
        self.assertEqual(vb2, by["SUB_02_1"]["vb"])
        self.assertEqual(by["SUB_02_1"]["vh"], by["SUB_02_2"]["vh"])
        self.assertEqual(127, by["SUB_02_1"]["vh"][32 + 1])                # (the program at 0: raised)
        self.assertIn("raised to 127", by["SUB_02_1"]["note"])
        self.assertEqual("", by["MAIN01"]["note"])
        self.assertEqual((vh3, vb3, "SONG.VH + .VB"), (by["SONG"]["vh"], by["SONG"]["vb"], by["SONG"]["bank_file"]))
        self.assertEqual((vh4, vb4, "TUNE.VAB"), (by["TUNE"]["vh"], by["TUNE"]["vb"], by["TUNE"]["bank_file"]))
        self.assertIsNone(by["ALONE"]["vh"])
        seconds, notes = assetscan.seq_seconds(by["SONG"]["seq"])
        self.assertEqual(40, notes)
        self.assertGreater(seconds, 1)
        self.assertEqual(1.0, assetscan.seq_coverage(by["SONG"]["seq"], vh3))
        self.assertIsNone(assetscan.seq_coverage(by["SONG"]["seq"][:15] + b"\x00\xFF\x2F\x00", vh3))
        self.assertEqual([], assetscan.disc_music(write(tmp_path("none.bin"), raw(iso([("A.DAT", bytes(5000))])))))

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


# ---------------------------------------------------------------------------------------------------- the page

class FakeSettings:
    def __init__(self):
        self.values = {}

    def field(self, key, default=""):
        return self.values.get(key, default)

    def set_field(self, key, value):
        self.values[key] = value


class FakeRunner:
    """The disc reader: each command sent answered as ps1transfer -disc answers it (disclive.c), a copy written from
    the test's files."""

    def __init__(self, page, disc):
        self.page, self.disc, self.sent = page, disc, []

    def send(self, line):
        self.sent.append(line)
        page = self.page
        if line.startswith("get "):
            _, path, out = line.split(" ", 2)
            data = self.disc[path]
            sectors = (len(data) + 2047) // 2048
            for done in (sectors // 2, sectors):
                page.disc_line("DISCPROG %d %d" % (done, sectors))
            with open(out, "wb") as f:
                f.write(data)
            page.disc_line("DISCSAVED %d 0 data %s" % (len(data), out))
        page.after(1, page.disc_line, "DISCDONE")


class FakeApp:
    def __init__(self):
        self.settings, self.errors = FakeSettings(), []

    def error(self, text):
        self.errors.append(text)


class ExtractorPage(unittest.TestCase):
    """The Asset Extractor page with a disc reader answering as the console's: files chosen on the disc copied and
    scanned, the assets of all of them in one list (by type, found), extracted with the progress bar."""

    @classmethod
    def setUpClass(cls):
        import tkinter
        import ttkbootstrap as tb
        try:
            cls.root = tb.Window(themename="bootstrap-dark")      # (as the GUI: ttkbootstrap's styles on its window)
        except tkinter.TclError as e:
            raise unittest.SkipTest("no display: %s" % e)
        cls.root.withdraw()

    @classmethod
    def tearDownClass(cls):
        cls.root.destroy()

    def make_disc(self):
        """COMMON\\SOUND of a Resident Evil 2 disc: two music files (MAIN, SUB with two SEQs and a program at 0), an
        effect bank (EDH + VB), a TIM elsewhere."""
        rng = random.Random(21)
        _, _, main = Scanner.bgm_file(rng)
        _, _, sub = Scanner.bgm_file(rng, sub=True, silent=(1,))
        vh, vb = make_bank(rng, 2)
        edh = bytes(range(256)) * 3 + vh + struct.pack("<II", 768, 0)
        tim = make_tim(rng, 1, 32, 16, 1)[0]
        return {"\\COMMON\\SOUND\\MAIN01.BGM": main, "\\COMMON\\SOUND\\SUB_02.BGM": sub,
                "\\COMMON\\SOUND\\ARMS01.EDH": edh, "\\COMMON\\SOUND\\ARMS01.VB": vb, "\\COMMON\\DATA\\PIC.TIM": tim}

    def pump(self, page, until, seconds=20):
        import time
        end = time.time() + seconds
        while not until() and time.time() < end:
            self.root.update()
            time.sleep(0.01)
        self.root.update()
        self.assertTrue(until(), "timed out")

    def test_extract_assets_button(self):
        """What a user does: the SOUND folder selected, Extract assets, a folder chosen; every music is there with its
        bank (SEQ + VH + VB in a folder for its file), the effect bank with its VB, nothing else copied there; one
        progress bar from 0 to 100, only forward; the page says what it wrote."""
        from unittest import mock
        from pages.gamedata import GameDataPage
        disc = self.make_disc()
        app = FakeApp()
        with tempfile.TemporaryDirectory() as tmp, mock.patch.dict(os.environ, {"PS1DEVCART_NO_OPEN": "1"}):
            with mock.patch.object(GameDataPage, "out_dir", lambda self, *parts: os.makedirs(
                    os.path.join(tmp, "gamedata", *parts), exist_ok=True) or os.path.join(tmp, "gamedata", *parts)):
                page = GameDataPage(self.root, app)
                app.runner = FakeRunner(page, disc)
                page.disc_live = True
                page.volume = "SLUS_00748"
                page.tree.insert("", "end", iid="\\COMMON", text="COMMON")
                for d in ("SOUND", "DATA"):
                    page.tree.insert("\\COMMON", "end", iid="\\COMMON\\" + d, text=d)
                for path, data in disc.items():
                    page.files[path] = (0, len(data), 0)
                    page.tree.insert(path.rsplit("\\", 1)[0], "end", iid=path, text=path.rsplit("\\", 1)[1])
                texts = []

                def walk(w):
                    for c in w.winfo_children():
                        try:
                            texts.append(c.cget("text"))
                        except Exception:       # noqa: BLE001 (widgets without text)
                            pass
                        walk(c)
                walk(page)
                self.assertIn("Extract assets...", texts)
                self.assertEqual([], [t for t in texts if t in ("Save selected...", "Save and scan for assets",
                                                                "Scan the whole disc")])

                page.extract_from_disc()                            # (nothing selected: said so)
                self.assertEqual(1, len(app.errors))
                self.assertIn("Select files or folders", app.errors[0])
                values = []
                page.progress.configure = (lambda orig: lambda **kw: (values.append(kw.get("value")), orig(**kw)))(
                    page.progress.configure)
                page.tree.selection_set("\\COMMON\\SOUND")
                out = os.path.join(tmp, "my assets")
                with mock.patch("pages.gamedata.filedialog.askdirectory", return_value=out):
                    page.extract_from_disc()
                self.pump(page, lambda: page.batch is None and not page.extracting)
                self.assertEqual(1, len(app.errors), app.errors)
                sound = os.path.join(out, "Music and sound banks")
                self.assertEqual(["Music and sound banks"], os.listdir(out))
                self.assertEqual(["ARMS01", "MAIN01", "SUB_02"], sorted(os.listdir(sound)))
                main = sorted(os.listdir(os.path.join(sound, "MAIN01")))
                self.assertEqual([".seq", ".vb", ".vh"], [os.path.splitext(p)[1] for p in main])
                sub = sorted(os.listdir(os.path.join(sound, "SUB_02")))
                self.assertEqual([".seq", ".seq", ".vb", ".vh"], [os.path.splitext(p)[1] for p in sub])
                self.assertEqual([".vb", ".vh"], sorted(os.path.splitext(p)[1] for p in os.listdir(
                    os.path.join(sound, "ARMS01"))))
                progress = [v for v in values if v is not None]
                self.assertEqual(0, progress[0])
                self.assertEqual(100, progress[-1])
                self.assertEqual(sorted(progress), progress)
                status = page.status.cget("text")
                self.assertTrue(status.startswith("Done: 6 assets extracted"), status)
                self.assertIn("music 3", status)
                self.assertIn(out, status)
                page.destroy()

    def test_scan_and_extract(self):
        from unittest import mock
        from pages.gamedata import GameDataPage, CATEGORIES
        disc = self.make_disc()
        app = FakeApp()
        with tempfile.TemporaryDirectory() as tmp:
            with mock.patch.object(GameDataPage, "out_dir", lambda self, *parts: os.makedirs(
                    os.path.join(tmp, "gamedata", *parts), exist_ok=True) or os.path.join(tmp, "gamedata", *parts)):
                page = GameDataPage(self.root, app)
                app.runner = FakeRunner(page, disc)
                page.disc_live = True
                page.volume = "SLUS_00748"
                for path, data in disc.items():
                    page.files[path] = (0, len(data), 0)
                    parent = ""
                    for part in path.strip("\\").split("\\")[:-1]:
                        iid = parent + "\\" + part
                        if not page.tree.exists(iid):
                            page.tree.insert(parent, "end", iid=iid, text=part)
                        parent = iid
                    page.tree.insert(parent, "end", iid=path, text=path.rsplit("\\", 1)[1])
                self.assertEqual([], [w for w in page.winfo_children() if "whole disc" in str(w)])

                # The SOUND folder chosen: its 4 files copied, each one scanned, the assets of all of them in the list
                page.tree.selection_set("\\COMMON\\SOUND")
                values = []
                page.progress.configure = (lambda orig: lambda **kw: (values.append(kw.get("value")), orig(**kw)))(
                    page.progress.configure)
                page.save_disc_files(True)
                self.pump(page, lambda: page.batch is None)
                self.assertEqual([], app.errors)
                self.assertEqual(4, sum(1 for s in app.runner.sent if s.startswith("get")))
                kinds = sorted(a.kind for a in page.assets)
                self.assertEqual(["SEQ", "SEQ", "SEQ", "VAB", "VAB", "VAB"], kinds)       # (the EDH: with its VB)
                progress = [v for v in values if v is not None]
                self.assertEqual(100, progress[-1])
                self.assertGreater(len(set(progress)), 4)
                self.assertEqual(sorted(progress[:-1]), progress[:-1])               # (only forward)
                self.assertIn("6 assets found", page.status.cget("text"))

                # Grouped by type, how many of each
                groups = {page.alist.item(c, "text"): len(page.alist.get_children(c)) for c in page.alist.get_children()}
                self.assertEqual({CATEGORIES[0][0] + "  (3)": 3, CATEGORIES[1][0] + "  (3)": 3}, groups)
                page.search.set("SUB_02")
                self.root.update()
                shown = [page.alist.item(i, "text") for c in page.alist.get_children() for i in page.alist.get_children(c)]
                self.assertEqual(3, len(shown))
                self.assertTrue(all(s.startswith("SUB_02") for s in shown), shown)
                page.search.set("")

                # A second scan adds to the list (a file already scanned: once)
                page.tree.selection_set(("\\COMMON\\DATA", "\\COMMON\\SOUND\\MAIN01.BGM"))
                page.save_disc_files(True)
                self.pump(page, lambda: page.batch is None)
                self.assertEqual(7, len(page.assets))
                self.assertIn("TIM", [a.kind for a in page.assets])

                # The music type selected: each SEQ extracted with the bank of its file, in one folder for the file
                def names(where, *kinds):
                    return sorted(p for x in page.assets if x.where == where and x.kind in kinds
                                  for p in [x.name()] + [os.path.splitext(x.name())[0] + e for e in x.parts])
                out = os.path.join(tmp, "out")
                page.alist.selection_set("cat0")
                self.root.update()
                self.assertEqual(3, len(page.chosen_assets()))
                with mock.patch("pages.gamedata.filedialog.askdirectory", return_value=out):
                    page.extract_assets(False)
                self.pump(page, lambda: not page.extracting)
                sound = os.path.join(out, "Music and sound banks")
                self.assertEqual(["MAIN01", "SUB_02"], sorted(os.listdir(sound)))       # (ARMS01: no music chosen)
                for f in ("MAIN01", "SUB_02"):
                    self.assertEqual(names(f + ".BGM", "SEQ", "VAB"), sorted(os.listdir(os.path.join(sound, f))))
                self.assertEqual(4, len(os.listdir(os.path.join(sound, "SUB_02"))))      # (2 SEQs, VH, VB)
                self.assertIn("5 assets extracted", page.status.cget("text"))

                # One SEQ selected alone: its bank with it
                one = next(i for i in page.alist.get_children("cat0") if page.alist.item(i, "text").startswith("MAIN01"))
                page.alist.selection_set(one)
                self.root.update()
                with mock.patch("pages.gamedata.filedialog.askdirectory", return_value=out + "1"):
                    page.extract_assets(False)
                self.pump(page, lambda: not page.extracting)
                self.assertEqual(names("MAIN01.BGM", "SEQ", "VAB"),
                                 sorted(os.listdir(os.path.join(out + "1", "Music and sound banks", "MAIN01"))))
                # The TIM: in Images
                tim = next(i for c in page.alist.get_children() for i in page.alist.get_children(c)
                           if page.assets[int(i[1:])].kind == "TIM")
                page.alist.selection_set(tim)
                self.root.update()
                with mock.patch("pages.gamedata.filedialog.askdirectory", return_value=out + "1"):
                    page.extract_assets(False)
                self.pump(page, lambda: not page.extracting)
                self.assertEqual(1, len(os.listdir(os.path.join(out + "1", "Images"))))

                # Everything, one folder for each file found in
                page.folders.set("A folder for each file found in")
                with mock.patch("pages.gamedata.filedialog.askdirectory", return_value=out + "2"):
                    page.extract_assets(True)
                self.pump(page, lambda: not page.extracting)
                self.assertEqual(["ARMS01", "MAIN01", "PIC", "SUB_02"], sorted(os.listdir(out + "2")))
                self.assertEqual(["SUB_02_seq_000000.seq", "SUB_02_seq_%06X.seq" % next(
                    a.offset for a in page.assets if a.where == "SUB_02.BGM" and a.kind == "SEQ" and a.offset),
                    "SUB_02_vab_%06X.vb" % next(a.offset for a in page.assets if a.where == "SUB_02.BGM" and a.kind == "VAB"),
                    "SUB_02_vab_%06X.vh" % next(a.offset for a in page.assets if a.where == "SUB_02.BGM" and a.kind == "VAB")],
                    sorted(os.listdir(os.path.join(out + "2", "SUB_02"))))
                page.destroy()


if __name__ == "__main__":
    unittest.main()
