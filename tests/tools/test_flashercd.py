"""
The flasher CD made by the Manager (gui/flashercd.py): the licence sectors of the user's own disc image put in the
flasher image without licence (ps1-tools/ar_flash/ar_flash_nolicense.bin). A made-up disc stands for the user's (no
Sony data in the tests): its 16 first sectors must arrive as they are, everything else as in the image without
licence, from a .bin, a .cue or an .iso; anything else is refused.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import random
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from toolhost import ROOT, tmp_path                             # noqa: E402

import flashercd                                                # noqa: E402
import strmovie                                                 # noqa: E402

RAW = 2352
SYNC = b"\x00" + b"\xFF" * 10 + b"\x00"
BASE = os.path.join(ROOT, "ps1-tools", "ar_flash", "ar_flash_nolicense.bin")


def bcd(v):
    return ((v // 10) << 4) | (v % 10)


def raw_disc(sectors=40, seed=1):
    """A made-up PlayStation disc (Mode 2, 2352-byte sectors): random licence sectors with the licence text in sector
    4, sectors 12-15 empty Form 2 as on a disc. Returns (bytes, its 16 licence sectors' 2336 bytes)."""
    rng = random.Random(seed)
    datas, subs, forms = [], [], []
    for i in range(sectors):
        if 12 <= i < 16:
            datas.append(bytes(2324))
            subs.append((0, 0, 0x20, 0))
            forms.append(2)
            continue
        d = bytearray(rng.randrange(256) for _ in range(2048))
        if i == 4:
            d[32:32 + 64] = b"          Licensed  by          Sony Computer Entertainment Euro pe "[:64]
        datas.append(bytes(d))
        subs.append((0, 0, 8, 0))
        forms.append(1)
    body = strmovie.mode2_sectors(subs, datas, forms)
    out = bytearray()
    for i in range(sectors):
        m, s = divmod(i + 150, 75 * 60)
        s, f = divmod(s, 75)
        out += SYNC + bytes((bcd(m), bcd(s), bcd(f), 2)) + body[i * 2336:(i + 1) * 2336]
    return bytes(out), body[:16 * 2336]


@unittest.skipUnless(os.path.isfile(BASE), "no ar_flash_nolicense.bin (build ps1-tools/ar_flash)")
class FlasherCd(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        with open(BASE, "rb") as f:
            cls.base = f.read()
        cls.disc, cls.licence = raw_disc()

    def check(self, cue):
        with open(cue[:-4] + ".bin", "rb") as f:
            made = f.read()
        self.assertEqual(len(self.base), len(made))
        for i in range(16):
            self.assertEqual(self.base[i * RAW:i * RAW + 16], made[i * RAW:i * RAW + 16], "sector %d header" % i)
            self.assertEqual(self.licence[i * 2336:(i + 1) * 2336], made[i * RAW + 16:(i + 1) * RAW], "sector %d" % i)
        self.assertEqual(self.base[16 * RAW:], made[16 * RAW:])        # (the flasher and its padding as they are)
        with open(cue, encoding="ascii") as f:
            self.assertIn('FILE "ar_flash.bin" BINARY', f.read())

    def test_base_image(self):
        """The image without licence: its licence sectors empty, the flasher with the repository's ROM in it, 150
        empty sectors at the end (a PicoStation can't read an image's last sector)."""
        for i in range(16):
            self.assertFalse(any(self.base[i * RAW + 24:i * RAW + 24 + 2048]), "licence sector %d not empty" % i)
        with open(os.path.join(ROOT, "cartrom", "ps1devcart.rom"), "rb") as f:
            rom = f.read()
        data = b"".join(self.base[i * RAW + 24:i * RAW + 24 + 2048] for i in range(len(self.base) // RAW))
        self.assertIn(rom, data, "the flasher image doesn't hold cartrom/ps1devcart.rom: build ps1-tools/ar_flash")
        self.assertTrue(all(not any(self.base[i * RAW + 24:i * RAW + 24 + 2048])
                            for i in range(len(self.base) // RAW - 150, len(self.base) // RAW)))

    def test_from_bin(self):
        src = tmp_path("fcd_disc.bin")
        with open(src, "wb") as f:
            f.write(self.disc)
        self.check(flashercd.make(src, tmp_path("fcd_out_bin"), BASE))

    def test_from_cue(self):
        src = tmp_path("fcd_game.bin")
        with open(src, "wb") as f:
            f.write(self.disc)
        cue = tmp_path("fcd_game.cue")
        with open(cue, "w") as f:
            f.write('FILE "%s" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' % os.path.basename(src))
        self.check(flashercd.make(cue, tmp_path("fcd_out_cue"), BASE))

    def test_from_iso(self):
        """2048-byte sectors: the licence sectors made again (Form 1, then 12-15 empty Form 2) as the disc has them."""
        src = tmp_path("fcd_disc.iso")
        with open(src, "wb") as f:
            f.write(b"".join(self.disc[i * RAW + 24:i * RAW + 24 + 2048] for i in range(len(self.disc) // RAW)))
        self.check(flashercd.make(src, tmp_path("fcd_out_iso"), BASE))

    def test_refused(self):
        junk = tmp_path("fcd_junk.bin")
        with open(junk, "wb") as f:
            f.write(bytes(random.Random(2).randrange(256) for _ in range(RAW * 40)))
        with self.assertRaises(ValueError):
            flashercd.make(junk, tmp_path("fcd_out_junk"), BASE)
        disc = tmp_path("fcd_disc2.bin")
        with open(disc, "wb") as f:
            f.write(self.disc)
        with self.assertRaises(ValueError):                    # (a base that isn't the image without licence)
            flashercd.make(disc, tmp_path("fcd_out_base"), disc)


if __name__ == "__main__":
    unittest.main()
