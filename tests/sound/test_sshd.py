"""
The SQ / HD / BD converter (gui/sshd.py: Sony's later music format made into a SEQ and a VAB):
- SQ -> SEQ: the notes at their ticks, the 1-byte pitch bend, the tempo in beats per minute, the header's programs,
  the loop, the result played by the console's player (seqhost)
- HD + BD -> VAB: key splits, layers, a drum kit, the tuning (fine tune up and down), the envelope, the reverb flag,
  the samples cut out of the BD, empty tones
- files cut short or of another kind: an error, no exception of another kind
- running status and note offs as note ons of velocity 0 (Alundra 2's files)
- scan: musics found in a larger file with the bank before or after them, the BD after padding or after the music, a
  disc image's 2352-byte sectors, copies listed once, a music alone left out when a copy has its bank
- the game files of PS1_SSHD_CORPUS (folders, separated by ";", of name.sq + name.hd + name.bd or name.a.hd +
  name.a.bd): each converted, the notes of the music with a tone of the bank (a few have none in the games' own
  files: Ape Escape's drum channels play percussion keys their bank has no sample for, 180 of 3726 notes at most)

    python -m unittest tests/sound/test_sshd.py -v

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import glob
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import seqref                                                           # noqa: E402
from soundhost import run_bytes, vabmod                                 # noqa: E402

import audioconv                                                        # noqa: E402  (gui, added by soundhost)
import sshd                                                             # noqa: E402


# ---------------------------------------------------------------------------------------------------- test files

def sq_file(events, tempo=120, ppqn=480, programs=(5,), end=True):
    """An SQ: events [(bytes, delta after it)], channel n on programs[n]."""
    d = bytearray(struct.pack("<HHH6x4s", 100, ppqn, tempo, b"SSsq"))
    for c in range(16):
        prog = programs[c] if c < len(programs) else 0xFF
        d += bytes((0, c, prog, 90 + c, 64 - c)) + bytes(7) + bytes((0x10, 0, 0x64, 0))
    for data, delta in events:
        d += bytes(data) + sshd._write_varlen(delta)
    return bytes(d + (b"\xFF\x2F\x00" if end else b""))


def block(flags, fill=0x11):
    return bytes((0x0C, flags)) + bytes((fill,)) * 14


def tone(lo, hi, root, fine, adpcm, flags=0, vol=100, pan=64, bend=2, adsr=0x5FC080FF):
    return struct.pack("<BBBbHIBBBBBB", lo, hi, root, fine, adpcm, adsr, 0, vol, pan, bend, 0x7F, flags)


def hd_file(programs, bd_size):
    """An HD: programs {number: (type byte, [tone bytes], first note, last note)}."""
    count = max(programs) + 1
    table, data = [0xFFFF] * count, b""
    for n in sorted(programs):
        kind, tones, first, last = programs[n]
        table[n] = 2 + count * 2 + len(data)
        data += bytes((kind, 110, 60, 0, 12, 0x7F, first, last)) + b"".join(tones)
    block0 = struct.pack("<H%dH" % count, count - 1, *table) + data
    velocity = bytes(2) + bytes(range(128))
    offsets = [0x80, 0x80 + len(block0)] + [0xFFFFFFFF] * 26
    size = 0x80 + len(block0) + len(velocity)
    return struct.pack("<III4s28I", size, bd_size, 0, b"SShd", *offsets) + block0 + velocity


# Samples: A at 0 (3 blocks, looping), B at 0x30 (2 blocks), C at 0x50 (1 block); 0x10 bytes never used after them
BD = (block(4, 0x11) + block(0, 0x12) + block(3, 0x13) + block(0, 0x21) + block(1, 0x22) + block(1, 0x31) +
      bytes(16))
HD = hd_file({
    0: (0x01, [tone(0, 59, 60, 0, 0, flags=0x80), tone(60, 127, 72, -3, 0x30 // 8, bend=7)], 0, 0),
    3: (0x81, [tone(0, 127, 60, 5, 0, pan=0), tone(0, 127, 60, -5, 0, pan=127, flags=0x10)], 0, 0),
    5: (0xFF, [tone(0, 0, 48, 0, 0x50 // 8), tone(0, 0, 0, 0, 0xFFFF), tone(0, 0, 50, -8, 0x30 // 8)], 36, 38),
    6: (0x00, [tone(0, 127, 60, 0, 0xFFFF)], 0, 0),
}, len(BD))


class SqTests(unittest.TestCase):
    def test_events(self):
        sq = sq_file([((0x90, 60, 100), 240), ((0xE0, 96), 0), ((0x80, 60, 64), 240), ((0xFF, 0x51, 90, 0), 0),
                      ((0x90, 62, 80), 0x1234), ((0x80, 62, 0), 5)], tempo=120)
        data, info = sshd.to_seq(sq)
        self.assertEqual(2, info["notes"])
        res, tempo, _, _, body = audioconv.seq_tracks(data)[0]
        self.assertEqual((480, 500000), (res, tempo))
        events, ticks = audioconv._seq_events(body)
        self.assertEqual(240 + 240 + 0x1234 + 5, ticks)
        ev = [(t, d) for t, k, d in events if k == "ev"]
        # The header's settings first, then the music
        self.assertEqual([(0, b"\xC0\x05"), (0, b"\xB0\x07\x5A"), (0, b"\xB0\x0A\x40")], ev[:3])
        self.assertEqual([(0, b"\x90\x3C\x64"), (240, b"\xE0\x00\x60"), (240, b"\x80\x3C\x40"),
                          (480, b"\xFF\x51\x03" + (666667).to_bytes(3, "big")), (480, b"\x90\x3E\x50"),
                          (480 + 0x1234, b"\x80\x3E\x00")], ev[3:])
        self.assertAlmostEqual(480 / 480 * 0.5 + (0x1234 + 5) / 480 * 60 / 90, info["seconds"], places=6)

    def test_loop(self):
        sq = sq_file([((0x90, 60, 100), 100), ((0xB0, 99, 20), 0), ((0xB0, 6, 127), 0), ((0x80, 60, 0), 0),
                      ((0x91, 64, 100), 380), ((0x81, 64, 0), 0), ((0xB0, 99, 30), 0)], programs=(5, 0))
        data, info = sshd.to_seq(sq)
        self.assertEqual((100, 480), (info["loop"], info["loop_end"]))
        events, _ = audioconv._seq_events(audioconv.seq_tracks(data)[0][4])
        self.assertEqual([(100, "start"), (100, "count"), (480, "end")], [(t, k) for t, k, d in events if k != "ev"])

    def test_played_by_the_console_player(self):
        sq = sq_file([((0x90, 60, 100), 240), ((0x80, 60, 0), 0), ((0x91, 64, 90), 240), ((0x81, 64, 0), 480)],
                     programs=(5, 0))
        data, _ = sshd.to_seq(sq)
        h = run_bytes(data, play_ms=3000, repeat=0)
        self.assertIsNone(h.problem())
        self.assertEqual([(0, 60, 100), (1, 64, 90)], [(n[2], n[3], n[4]) for n in h.notes])
        res, tempo, body = seqref.tracks(data)[0]
        self.assertAlmostEqual(seqref.length_us(res, tempo, seqref.events(body)) / 1e6, 1.0, places=3)

    def test_running_status(self):
        # As Alundra 2: the status left out when it is the last one's, also after a tempo event
        sq = sq_file([((0xB0, 7, 100), 10), ((10, 64), 0), ((0x90, 60, 100), 240), ((60, 0), 0),
                      ((0xFF, 0x51, 100, 0), 0), ((62, 90), 120), ((62, 0), 0), ((0xE0, 70), 5), ((80,), 0)])
        q = sshd.Ssq(sq)
        self.assertEqual([b"\xB0\x07\x64", b"\xB0\x0A\x40", b"\x90\x3C\x64", b"\x90\x3C\x00", 100,
                          b"\x90\x3E\x5A", b"\x90\x3E\x00", b"\xE0\x46", b"\xE0\x50"],
                         [bytes(v) if k == "ev" else v for _, k, v in q.events])
        self.assertEqual(len(sq), q.size)
        data, info = sshd.to_seq(q)
        self.assertEqual(2, info["notes"])
        h = run_bytes(data, play_ms=2000, repeat=0)
        self.assertIsNone(h.problem())
        self.assertEqual([60, 62], [n[3] for n in h.notes])

    def test_bad_files(self):
        good = sq_file([((0x90, 60, 100), 240), ((0x80, 60, 0), 0)])
        for bad in (b"", good[:0x100], good[:12] + b"SEQp" + good[16:], good[:0x112],
                    good[:0x110] + b"\x40\x00\x00", good[:0x110] + b"\xFF\x10\x00\x00",
                    good[:0x110] + b"\xF0\x00\x00", sq_file([], tempo=0)):
            with self.assertRaises(sshd.FormatError):
                sshd.Ssq(bad)
        # No end event: what was read is kept
        sq = sshd.Ssq(sq_file([((0x90, 60, 100), 240)], end=False))
        self.assertFalse(sq.ended)
        self.assertEqual(240, sq.ticks)


class HdTests(unittest.TestCase):
    def setUp(self):
        self.vh, self.vb, self.info = sshd.to_vab(HD, BD)
        self.bank = vabmod.Vab(self.vh, self.vb)

    def test_programs(self):
        self.assertEqual([0, 3, 5], self.info["programs"])      # (6: only an empty tone)
        self.assertEqual([0, 3, 5], self.bank.programs())
        self.assertEqual(6, self.info["tones"])
        self.assertEqual([2, 2, 2], [self.bank.prog(p)["tones"] for p in (0, 3, 5)])
        self.assertEqual((110, 60), (self.bank.prog(0)["mvol"], self.bank.prog(0)["mpan"]))

    def test_samples(self):
        self.assertEqual(3, self.info["samples"])
        self.assertEqual(BD[:0x60], self.vb)                    # (the unused end of the BD left out)
        self.assertEqual([BD[:0x30], BD[0x30:0x50], BD[0x50:0x60]], [self.bank.vag_data(n) for n in (1, 2, 3)])
        self.assertEqual([1, 2], [t["vag"] for t in self.bank.tones(0)])
        self.assertEqual([3, 2], [t["vag"] for t in self.bank.tones(5)])

    def test_tones(self):
        low, high = self.bank.tones(0)
        self.assertEqual((0, 59, 60, 0), (low["min"], low["max"], low["center"], low["shift"]))
        # 3/16 semitone down: the key above, 13/16 up
        self.assertEqual((60, 127, 73, 104), (high["min"], high["max"], high["center"], high["shift"]))
        self.assertEqual((4, 0), (low["mode"], high["mode"]))   # (reverb)
        self.assertEqual((2, 7), (low["pbmax"], high["pbmin"]))
        self.assertEqual((0x80FF, 0x5FC0), (low["adsr1"], low["adsr2"]))
        a, b = self.bank.tones(3)                               # Layers: both over all keys
        self.assertEqual([(0, 127, 60, 40, 0), (0, 127, 61, 88, 127)],
                         [(t["min"], t["max"], t["center"], t["shift"], t["pan"]) for t in (a, b)])
        self.assertEqual((2, 12), (a["pbmax"], b["pbmax"]))     # (b: the program's bend range)

    def test_drum_kit(self):
        kick, snare = self.bank.tones(5)
        self.assertEqual((36, 36, 48, 0), (kick["min"], kick["max"], kick["center"], kick["shift"]))
        self.assertEqual((38, 38, 51, 64), (snare["min"], snare["max"], snare["center"], snare["shift"]))

    def test_large_kit(self):
        hd = hd_file({0: (0xFF, [tone(0, 0, 60, 0, 0)] * 20, 40, 59)}, len(BD))
        vh, vb, info = sshd.to_vab(hd, BD)
        bank = vabmod.Vab(vh, vb)
        self.assertEqual(list(range(40, 56)), [t["min"] for t in bank.tones(0)])
        self.assertIn("56, 57, 58, 59", info["skipped"][0])

    def test_large_program_one_key_a_tone(self):
        # As Arc the Lad III: a layered program of one key a tone (some keys twice), more than 16: the first 16 kept
        keys = [50, 50, 51, 52, 54, 54, 55, 61, 62, 63, 64, 65, 66, 67, 70, 71, 72, 73, 74]
        hd = hd_file({0: (0x80 | (len(keys) - 1), [tone(k, k, 60, 0, 0) for k in keys], 0, 0)}, len(BD))
        vh, vb, info = sshd.to_vab(hd, BD)
        bank = vabmod.Vab(vh, vb)
        self.assertEqual(keys[:16], [t["min"] for t in bank.tones(0)])
        self.assertIn("a tone for each key", info["skipped"][0])
        self.assertIn("72, 73, 74", info["skipped"][0])
        # (tones over key ranges, more than 16: not something to cut, refused)
        wide = hd_file({0: (0x80 | 16, [tone(0, 127, 60, 0, 0)] * 17, 0, 0)}, len(BD))
        with self.assertRaises(sshd.FormatError):
            sshd.to_vab(wide, BD)

    def test_bad_files(self):
        for bad in (b"", HD[:0x60], HD[:12] + b"pBAV" + HD[16:], HD[:0x90], HD[:0xA0]):
            with self.assertRaises(sshd.FormatError):
                sshd.to_vab(bad, BD)
        with self.assertRaises(sshd.FormatError):
            sshd.to_vab(HD, BD[:0x40])                          # (a sample past the BD's end)
        effects = struct.pack("<III4s28I", 0x80, 0, 0, b"SShd", *([0xFFFFFFFF] * 28))
        with self.assertRaises(sshd.FormatError):
            sshd.to_vab(effects, BD)


class ScanTests(unittest.TestCase):
    """Musics found inside a game's files."""

    SQ = sq_file([((0x90, 30, 100), 240), ((0x80, 30, 0), 0), ((0x90, 70, 100), 240), ((0x80, 70, 0), 0)],
                 programs=(0,))
    KIT = sq_file([((0x90, 36, 100), 240), ((0x80, 36, 0), 0), ((0x90, 38, 100), 240), ((0x80, 38, 0), 0)],
                  programs=(5,))
    BD2 = BD[:0x60]                     # (a BD ends with its last sample)
    HD2 = hd_file({0: (0x01, [tone(0, 59, 60, 0, 0), tone(60, 127, 72, 0, 0x30 // 8)], 0, 0),
                   5: (0xFF, [tone(0, 0, 48, 0, 0x50 // 8), tone(0, 0, 0, 0, 0xFFFF), tone(0, 0, 50, 0, 0x30 // 8)],
                       36, 38)}, len(BD2))
    OTHER = hd_file({0: (0x00, [tone(0, 127, 60, 0, 0)], 0, 0)}, len(BD2))     # (a tone for every note too)
    JUNK = bytes(range(7, 251)) * 9     # (a multiple of 4 bytes: files start at multiples of 4, as in the games)

    @staticmethod
    def pad(b, n=4):
        return b + bytes(-len(b) % n)

    def check(self, found, sq, hd, coverage=1.0):
        self.assertEqual(sq, found["sq_bytes"])
        self.assertEqual(hd, found["hd_bytes"])
        self.assertEqual(self.BD2, found["bd_bytes"])
        self.assertAlmostEqual(coverage, found["coverage"])

    def test_bank_after(self):
        d = self.JUNK + self.pad(self.SQ) + self.pad(self.HD2) + self.BD2 + self.JUNK
        found = sshd.scan(d)
        self.assertEqual(1, len(found))
        self.assertEqual(len(self.JUNK), found[0]["at"])
        self.check(found[0], self.SQ, self.HD2)

    def test_bank_before_and_padding(self):
        # The BD 0x80 bytes after its HD, the music after it; then a file with another bank and no music
        d = (self.JUNK + self.HD2 + bytes(0x80) + self.BD2 + self.pad(self.KIT) + self.JUNK +
             self.pad(self.OTHER) + self.BD2 + self.JUNK)
        found = sshd.scan(self.pad(d))
        self.assertEqual(1, len(found))
        self.check(found[0], self.KIT, self.HD2)

    def test_bd_after_the_music(self):
        # As Ape Escape: HD, an effects HD (no programs), SQ, BD
        effects = struct.pack("<III4s28I", 0x80, 0x40, 0, b"SShd", *([0xFFFFFFFF] * 28))
        d = self.JUNK + self.pad(self.HD2) + effects + self.pad(self.SQ) + self.BD2 + bytes(0x40) + self.JUNK
        found = sshd.scan(d)
        self.assertEqual(1, len(found))
        self.check(found[0], self.SQ, self.HD2)

    def test_bd_before_the_bank(self):
        # As Arc the Lad III: BD, SQ, HD (each file at a multiple of 2048 bytes in its archive)
        d = self.JUNK + self.pad(self.BD2, 2048) + self.pad(self.SQ, 2048) + self.pad(self.HD2, 2048) + self.JUNK
        found = sshd.scan(d)
        self.assertEqual(1, len(found))
        self.check(found[0], self.SQ, self.HD2)

    def test_bd_nearest_side(self):
        # A BD on each side of the HD (the same samples' layout, other contents): the one nearer the HD
        other = b"".join(block(f, 0x40 + i) for i, f in enumerate((4, 0, 3, 0, 1, 1)))
        far, near = self.JUNK * 3, bytes(0x40)
        for nearer, before, after in (("after", other + far, near + self.BD2), ("before", self.BD2 + near, far + other)):
            with self.subTest(nearer=nearer):
                d = self.JUNK + before + self.pad(self.SQ) + self.pad(self.HD2) + after + self.JUNK
                found = sshd.scan(d)
                self.assertEqual(1, len(found))
                self.assertEqual(self.BD2, found[0]["bd_bytes"])

    def test_packed_files(self):
        # HD BD SQ, HD BD SQ: the second music has a bank on each side with a tone for all its notes; the first
        # one's only good bank is before it, so the second's is too
        one = self.pad(self.HD2) + self.BD2 + self.pad(self.KIT)
        two = self.pad(self.OTHER) + self.BD2 + self.pad(self.SQ)
        d = self.JUNK + one + two + self.pad(self.OTHER[:-1] + b"\x01") + self.BD2 + self.JUNK
        found = sshd.scan(d)
        self.assertEqual(2, len(found))
        self.check(found[0], self.KIT, self.HD2)
        self.check(found[1], self.SQ, self.OTHER)

    def test_copies(self):
        file = self.pad(self.SQ) + self.pad(self.HD2) + self.BD2
        alone = self.pad(self.SQ)                               # (the music with no bank next to it)
        d = self.JUNK + file + self.JUNK * 3 + file + self.JUNK * 3 + alone + self.JUNK
        found = sshd.scan(d)
        self.assertEqual(1, len(found))
        self.check(found[0], self.SQ, self.HD2)

    def test_no_bank(self):
        found = sshd.scan(self.JUNK + self.SQ + self.JUNK)
        self.assertEqual(1, len(found))
        self.assertIsNone(found[0]["hd"])
        self.assertEqual([], sshd.scan(self.JUNK + self.HD2 + self.BD2 + b"SSsq" + self.JUNK))

    def test_disc_image(self):
        data = self.pad(self.JUNK + self.pad(self.SQ) + self.pad(self.HD2) + self.BD2, 2048 * 20)
        image = b"".join(b"\x00" + b"\xFF" * 10 + b"\x00" + bytes(12) + data[i:i + 2048] + bytes(280)
                         for i in range(0, len(data), 2048))
        self.assertEqual(data, sshd.disc_data(image))
        self.assertEqual(data, sshd.disc_data(data))
        found = sshd.scan(sshd.disc_data(image))
        self.assertEqual(1, len(found))
        self.check(found[0], self.SQ, self.HD2)


def mrg(files, sectors=False):
    """An MRG archive (The Legend of Dragoon): "MRG", 0x1A, the number of files, each one's offset and size (in
    bytes, or 2048-byte sectors: files sector-aligned), the files."""
    n = len(files)
    head = 8 + n * 8
    if sectors:
        at, table, body = (head + 2047) // 2048, b"", b""
        for f in files:
            table += struct.pack("<II", at + len(body) // 2048, len(f))
            body += f + bytes(-len(f) % 2048)
        return b"MRG\x1a" + struct.pack("<I", n) + table + bytes(at * 2048 - head) + body
    table, body = b"", b""
    for f in files:
        table += struct.pack("<II", head + len(body), len(f))
        body += f + bytes(-len(f) % 4)
    return b"MRG\x1a" + struct.pack("<I", n) + table + body


class DragoonTests(unittest.TestCase):
    """The Legend of Dragoon: each music in a small MRG with its bank, in a big archive (offsets in sectors)."""

    T = ScanTests

    def check(self, found, hd):
        self.assertEqual((self.T.SQ, hd, self.T.BD2), (found["sq_bytes"], found["hd_bytes"], found["bd_bytes"]))

    def test_bank_in_its_group(self):
        t = self.T
        # The group before ends with a bank that has the music's tones too: the music's own bank is taken
        before = mrg([b"\x00\x00", t.pad(t.KIT), t.pad(t.OTHER), t.BD2])
        own = mrg([b"\x00\x00", t.pad(t.SQ), t.pad(t.HD2), t.BD2])
        found = sshd.scan(mrg([before, own], sectors=True))
        music = [f for f in found if f["sq_bytes"] == t.SQ]
        self.assertEqual(1, len(music))
        self.check(music[0], t.HD2)

    def test_split_samples(self):
        t = self.T
        # The bank's BD: the group's last file, then the archive's next files (each after the other)
        hd = t.HD2[:4] + struct.pack("<I", len(t.BD2)) + t.HD2[8:]
        group = mrg([b"\x00\x00", t.pad(t.SQ), t.pad(hd), t.BD2[:0x30]])
        archive = mrg([group, t.BD2[0x30:0x50], b"", t.BD2[0x50:]], sectors=True)
        found = sshd.scan(archive)
        self.assertEqual(1, len(found))
        self.assertEqual(t.BD2, found[0]["bd_bytes"])
        self.assertEqual(hd, found[0]["hd_bytes"])
        # Not enough samples after it: not taken
        self.assertIsNone(sshd.scan(mrg([group, t.BD2[0x30:0x40]], sectors=True))[0]["hd"])

    def test_group_without_bank(self):
        t = self.T
        # A music played with a bank loaded before: the bank of the archive with all its notes
        low = hd_file({0: (0x00, [tone(0, 59, 60, 0, 0)], 0, 0)}, len(t.BD2))     # (note 30 only: half)
        alone = mrg([b"\x00\x00", t.pad(t.SQ)])
        partial = mrg([b"\x00\x00", t.pad(t.KIT), t.pad(low), t.BD2])
        full = mrg([b"\x00\x00", t.pad(t.KIT), t.pad(t.HD2), t.BD2])
        found = sshd.scan(mrg([alone, partial, full], sectors=True))
        music = [f for f in found if f["sq_bytes"] == t.SQ]
        self.check(music[0], t.HD2)

    def test_copy_packed_with_a_bank(self):
        t = self.T
        # Dragoon's entries 696-701: a music alone, a battle set after it, and a copy of the music packed with a
        # bank elsewhere. The copy's bank lacks some of its notes: the set's bank, the copy left out
        low = hd_file({0: (0x00, [tone(0, 59, 60, 0, 0)], 0, 0)}, len(t.BD2))
        alone = mrg([b"\x00\x00", t.pad(t.SQ)])
        battle = mrg([b"\x00\x00", t.pad(t.KIT), t.pad(t.HD2), t.BD2])
        copy = mrg([b"\x00\x00", t.pad(t.SQ), t.pad(low), t.BD2])
        music = [f for f in sshd.scan(mrg([alone, battle, copy], sectors=True)) if f["sq_bytes"] == t.SQ]
        self.assertEqual(1, len(music))
        self.check(music[0], t.HD2)
        # The copy's bank has all its notes: the copy, with its own bank
        copy = mrg([b"\x00\x00", t.pad(t.SQ), t.pad(t.OTHER), t.BD2])
        music = [f for f in sshd.scan(mrg([alone, battle, copy], sectors=True)) if f["sq_bytes"] == t.SQ]
        self.assertEqual(1, len(music))
        self.check(music[0], t.OTHER)


class CorpusTests(unittest.TestCase):
    """Game files (PS1_SSHD_CORPUS): each music and its bank converted; the notes have a tone (but a few)."""

    def test_convert(self):
        files = [p for d in os.environ.get("PS1_SSHD_CORPUS", "").split(";") if d
                 for p in sorted(glob.glob(os.path.join(d, "*.sq")))]
        if not files:
            self.skipTest("no PS1_SSHD_CORPUS")
        for path in files:
            base = path[:-3]
            hd = next((base + e for e in (".hd", ".a.hd") if os.path.exists(base + e)), None)
            with self.subTest(file=os.path.basename(path)):
                with open(path, "rb") as f:
                    sq = sshd.Ssq(f.read())
                self.assertTrue(sq.ended, "no end event")
                data, info = sshd.to_seq(sq)
                events, ticks = audioconv._seq_events(audioconv.seq_tracks(data)[0][4])
                self.assertEqual(sq.ticks, ticks)
                if not hd:
                    continue
                with open(hd, "rb") as f, open(hd[:-2] + "bd", "rb") as g:
                    vh, vb, _ = sshd.to_vab(f.read(), g.read())
                bank = vabmod.Vab(vh, vb)
                self.assertEqual(len(vb), sum(bank.vag_sizes()))
                prog, silent, notes = {}, [], 0
                for _, kind, d in events:
                    if kind != "ev":
                        continue
                    if d[0] & 0xF0 == 0xC0:
                        prog[d[0] & 15] = d[1]
                    elif d[0] & 0xF0 == 0x90 and d[2]:
                        p = prog.get(d[0] & 15, 0)
                        notes += 1
                        if p not in bank.blocks or not any(t["min"] <= d[1] <= t["max"] for t in bank.tones(p)):
                            silent.append((p, d[1]))
                self.assertLessEqual(len(silent), notes * 0.06, "notes (program, key) with no tone: %s" % sorted(
                    set(silent)))


if __name__ == "__main__":
    unittest.main()
