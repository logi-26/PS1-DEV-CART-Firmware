"""
Disc images read by the game scans (gui/discimage.py): .bin (Mode 2 and Mode 1), .iso, .cue sheets (one file or a
file a track, an audio track after the data, MODE2/2336) and ECM files, each seen as its data track's 2048-byte
sectors.

The ECM files are made here as Neill Corlett's ECM writes them (records of a type and count; Mode 2 sectors' sync and
header as bytes as they are, then each sector from its subheader).
"""

import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "gui"))
import discimage                                            # noqa: E402

SYNC = discimage.SECTOR_SYNC
SECTORS = 40


def data(n=SECTORS):
    """The 2048-byte sectors' data: each sector's number and a pattern."""
    return b"".join(struct.pack("<I", i) + bytes((i * 31 + k * 7) & 0xFF for k in range(2044)) for i in range(n))


def msf(lba):
    lba += 150
    return bytes(int(str(v), 16) for v in (lba // 4500, lba // 75 % 60, lba % 75))      # (BCD)


def mode2(d, form2_every=0):
    """2352-byte Mode 2 sectors (Form 1; with form2_every, every Nth a Form 2 one: its 2324 bytes)."""
    out = bytearray()
    for i in range(len(d) // 2048):
        form2 = form2_every and i % form2_every == form2_every - 1
        sub = bytes([0, 0, 0x20 if form2 else 0x08, 0])
        body = d[i * 2048:(i + 1) * 2048]
        body = body + bytes(2324 - 2048) if form2 else body + bytes(280)
        out += SYNC + msf(i) + b"\x02" + sub + sub + body[:2324 if form2 else 2328] + bytes(4 if form2 else 0)
    return bytes(out)


def mode1(d):
    return b"".join(SYNC + msf(i) + b"\x01" + d[i * 2048:(i + 1) * 2048] + bytes(288) for i in range(len(d) // 2048))


def ecm_record(kind, num):
    n = num - 1
    out = bytearray([kind | (n & 0x1F) << 2 | (0x80 if n >> 5 else 0)])
    n >>= 5
    while n:
        out.append(n & 0x7F | (0x80 if n >> 7 else 0))
        n >>= 7
    return bytes(out)


def ecm(raw):
    """An ECM file of 2352-byte sectors: Mode 1 sectors as type 1; Mode 2: 16 bytes as they are, then the sector."""
    out = bytearray(b"ECM\x00")
    for i in range(0, len(raw), 2352):
        s = raw[i:i + 2352]
        if s[15] == 1:
            out += ecm_record(1, 1) + s[12:15] + s[16:16 + 2048]
        else:
            form2 = s[18] & 0x20
            out += ecm_record(0, 16) + s[:16]
            out += ecm_record(3 if form2 else 2, 1) + s[16:20] + s[24:24 + (2324 if form2 else 2048)]
    return bytes(out + b"\xFC\xFF\xFF\xFF\x3F" + bytes(4))      # (the end record, then an EDC: not checked here)


class Images(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.d = data()

    def tearDown(self):
        self.tmp.cleanup()

    def file(self, name, content):
        path = os.path.join(self.dir, name)
        with open(path, "wb" if isinstance(content, bytes) else "w") as f:
            f.write(content)
        return path

    def check(self, path, expect=None):
        expect = self.d if expect is None else expect
        with discimage.Image(path) as img:
            self.assertEqual(len(expect), len(img))
            self.assertEqual(expect, img.read(0, len(img)))
            self.assertEqual(expect[5000:9000], img[5000:9000])            # (parts of sectors)
            self.assertEqual([2048 * 7], list(img.find(struct.pack("<I", 7) + expect[2048 * 7 + 4:2048 * 7 + 12])))

    def test_bin_iso(self):
        self.check(self.file("game.bin", mode2(self.d)))
        self.check(self.file("mode1.bin", mode1(self.d)))
        self.check(self.file("game.iso", self.d))

    def test_cue_one_file(self):
        # Track 1 data, track 2 audio in the same .bin: only the data track's sectors
        audio = bytes(range(256)) * (2352 * 10 // 256)
        self.file("game.bin", mode2(self.d) + audio)
        self.check(self.file("game.cue", 'FILE "game.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
                                         '  TRACK 02 AUDIO\n    INDEX 00 00:00:%02d\n    INDEX 01 00:00:%02d\n' %
                             (SECTORS, SECTORS + 2)))

    def test_cue_file_a_track(self):
        self.file("Game (Track 1).bin", mode2(self.d))
        self.file("Game (Track 2).bin", bytes(2352 * 5))
        self.check(self.file("Game.cue", 'FILE "Game (Track 1).bin" BINARY\n  TRACK 01 MODE2/2352\n'
                                         '    INDEX 01 00:00:00\nFILE "Game (Track 2).bin" BINARY\n'
                                         '  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n'))

    def test_cue_other_sector_sizes(self):
        raw = mode2(self.d)
        self.file("m2336.bin", b"".join(raw[i + 16:i + 2352] for i in range(0, len(raw), 2352)))
        self.check(self.file("m2336.cue", "FILE m2336.bin BINARY\n TRACK 01 MODE2/2336\n  INDEX 01 00:00:00\n"))
        self.file("iso.bin", self.d)
        self.check(self.file("iso.cue", "FILE iso.bin BINARY\n TRACK 01 MODE1/2048\n  INDEX 01 00:00:00\n"))
        self.file("audio.bin", bytes(2352 * 4))
        with self.assertRaises(discimage.FormatError):
            discimage.Image(self.file("audio.cue", "FILE audio.bin BINARY\n TRACK 01 AUDIO\n  INDEX 01 00:00:00\n"))

    def test_ecm(self):
        raw = mode2(self.d, form2_every=5)                  # (Form 2 sectors among them, as XA sound)
        expect = b"".join(raw[i + 24:i + 24 + 2048] for i in range(0, len(raw), 2352))
        self.check(self.file("game.bin.ecm", ecm(raw)), expect)
        with discimage.Image(self.file("decoded.ecm", ecm(raw))) as img:
            self.assertEqual(len(raw), img.src.size)
            # The decoded sectors as they were (the error correction left out: zero; Form 1 sectors' is zero here)
            got = img._raw(0, len(raw))
            for i in range(0, len(raw), 2352):
                self.assertEqual(raw[i:i + 2348], got[i:i + 2348])
        self.check(self.file("mode1.ecm", ecm(mode1(self.d))))

    def test_cue_with_ecm(self):
        # The .cue names game.bin; only game.bin.ecm is there
        self.file("game.bin.ecm", ecm(mode2(self.d)))
        self.check(self.file("game.cue", 'FILE "game.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'))

    def test_long_ecm_records(self):
        # Records with counts over 32 (header bytes of 7 more bits each): 2048-byte sectors as one record
        raw = mode1(self.d)
        e = b"ECM\x00" + ecm_record(1, SECTORS) + b"".join(raw[i + 12:i + 15] + raw[i + 16:i + 16 + 2048]
                                                             for i in range(0, len(raw), 2352))
        e += ecm_record(0, 300) + bytes(300) + b"\xFC\xFF\xFF\xFF\x3F" + bytes(4)
        with discimage.Image(self.file("long.ecm", e)) as img:
            self.assertEqual(len(raw) + 300, img.src.size)
            self.assertEqual(self.d, img.read(0, len(self.d)))

    def test_bad_ecm(self):
        good = ecm(mode2(self.d))
        with self.assertRaises(discimage.FormatError):
            discimage.Image(self.file("cut.ecm", good[:len(good) // 2]))
        with self.assertRaises(discimage.FormatError):
            discimage.Image(self.file("noend.ecm", good[:-9]))


if __name__ == "__main__":
    unittest.main()
