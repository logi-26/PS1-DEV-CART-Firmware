"""
The Crystal Dynamics music converter (gui/aad.py: Gex 3's and Akuji's SND bank + SMP samples into a VAB, the bank's
musics into SEQs) and the disc reading it shares with the SQ/HD scan (gui/discimage.py):
- bank: both header layouts (Gex 3: version 0x106, 32-bit header size; Akuji: older, 16-bit), programs, tones (key
  ranges, the tuning up and down, reverb, bend range, envelope), the waves as offsets (Gex 3) or SPU addresses (Akuji)
  in the SMP, programs counting more tones than the bank has (some of Gex 3's)
- sequences: running status, note offs as velocity 0, the tempo, the loop (DefineLabel / GotoLabel, LoopStart /
  LoopEnd), a bend written again after a note on of a bent channel, the game's commands listed as not converted, the
  result played by the console's player (seqhost)
- files: Gex's BIGFILE.DAT compression, an archive found in an ISO 9660 image of 2048 or 2352-byte sectors, the
  samples paired with their bank, images read in pieces (discimage)
- files cut short or of another kind: an error, no exception of another kind
- the game files of PS1_AAD_CORPUS (folders, separated by ";", of disc images or BIGFILE.DATs): each bank converted,
  each music played by the console's player

    python -m unittest tests/sound/test_aad.py -v

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

import aad                                                              # noqa: E402  (gui, added by soundhost)
import audioconv                                                        # noqa: E402
import discimage                                                        # noqa: E402


# ---------------------------------------------------------------------------------------------------- test files

def block(flags, fill=0x11):
    return bytes((0x0C, flags)) + bytes((fill,)) * 14


# SPU data: wave A at 0 (zero block, 2 blocks, end), wave B at 0x30 (2 blocks), wave C at 0x50 (1 block)
SPU = bytes(16) + block(4) + block(3) + block(0, 0x21) + block(1, 0x22) + block(1, 0x31)
SMP = b"PMSa" + struct.pack("<I", len(SPU)) + SPU       # (the size: of the data after it)


def varlen(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append(0x80 | (v & 0x7F))
        v >>= 7
    return bytes(reversed(out))


def seq(events, us=500000, ppqn=480, version=0x106):
    """A QESa sequence: events [(delta before it, bytes)]; ending with EndSequence."""
    body = b"".join(varlen(d) + bytes(e) for d, e in events) + b"\x00\xFF\x44\x00"
    return b"QESa" + struct.pack("<IHH", us, ppqn, version) + body


def cmd(n, *args):
    return bytes((0xFF, n, len(args))) + bytes(args)


def tone(prog, lo, hi, center, fine, wave, mode=0, bend=2, vol=100, pan=64, prio=0, adsr=(0x80FF, 0x5FC0)):
    return struct.pack("<BBBBBbBBBBHHh", prio, prog, vol, pan, center, fine, lo, hi, mode, bend, adsr[0], adsr[1], wave)


def snd(programs, tones, waves, seqs, labels, older=False, extra_tones=0):
    """A bank: programs [(number of tones, volume, pan)] (their tones one after the other), tones (bytes), waves,
    sequences (bytes), labels (offsets). older: Akuji's header. extra_tones: programs count that many more tones than
    the bank has (as some of Gex 3's)."""
    np, nt = len(programs), len(tones)
    table, first = b"", 0
    for i, (n, vol, pan) in enumerate(programs):
        n += extra_tones if i == np - 1 else 0
        table += struct.pack("<HHBBH", n, first, vol, pan, 0)
        first += n
    offsets, data = [], b""
    for s in seqs:
        offsets.append(len(data))
        data += s
    counts = struct.pack("<6H", nt, len(waves), len(seqs), len(labels), 3, 10000)
    if older:
        head = b"DNSa" + struct.pack("<H", 0x45) + b"\x03" + bytes((np,)) + counts
        head += bytes(0x48 - len(head))
    else:
        head = b"DNSa" + struct.pack("<IH", 0x80, 0x106) + b"\x03" + bytes((np,)) + counts
        head += bytes(0x80 - len(head))
    return (head + table + b"".join(tones) + struct.pack("<%dI" % len(waves), *waves) +
            struct.pack("<%dI" % len(offsets), *offsets) + struct.pack("<%dI" % len(labels), *labels) + data)


# A music: tempo, label 0, notes (running status, offs as velocity 0), a bend, back to label 0
MUSIC = seq([(0, cmd(5, 0x07, 0xA1, 0x20)), (0, b"\xC0\x00"), (0, b"\xB0\x07\x64"), (0, b"\xB0\x00\x01"),
             (0, cmd(49, 0)), (0, b"\x90\x3C\x64"), (240, b"\x3C\x00"), (0, b"\xE0\x00\x50"), (0, b"\x90\x48\x50"),
             (240, b"\x48\x00"), (0, cmd(51, 3, 7)), (0, cmd(50, 0))])
CONTROL = seq([(0, cmd(5, 0x07, 0xA1, 0x20)), (0, cmd(1, 0)), (0, cmd(2, 1)), (0, cmd(8))])
TONES = [tone(0, 0, 59, 60, 0, 0, mode=4), tone(0, 60, 127, 72, -10, 1, bend=7),
         tone(1, 0, 127, 60, 100, 2), tone(1, 0, 127, 48, 0, 1, pan=0)]
PROGRAMS = [(2, 110, 60), (2, 127, 64)]
WAVES = [0, 0x30, 0x50]


def music_label():
    """Label 0's offset in MUSIC's track: just after its DefineLabel."""
    track = MUSIC[12:]
    return track.index(cmd(49, 0)) + 3 + 1


GEX = snd(PROGRAMS, TONES, WAVES, [CONTROL, MUSIC], [music_label()])
AKUJI = snd(PROGRAMS, TONES, [w + 0x1010 for w in WAVES], [CONTROL, seq(
    [(d, e) for d, e in [(0, b"\x90\x3C\x64"), (480, b"\x3C\x00")]], version=0x45)], [0], older=True)


def lz_literal(data):
    """Gex's compression with every byte as it is (16 flags of 0, 16 bytes)."""
    return b"".join(b"\x00\x00" + data[i:i + 16] for i in range(0, len(data), 16))


def bigfile(files, packed=()):
    n = len(files)
    offset = (4 + n * 24 + 0x7FF) // 0x800 * 0x800
    table, body = struct.pack("<I", n), b""
    for i, f in enumerate(files):
        blob = lz_literal(f) if i in packed else f
        table += struct.pack("<6I", 0x1234 + i, len(f), len(blob) if i in packed else 0, offset + len(body), 0, 0)
        body += blob + bytes(-len(blob) % 0x800)
    return table + bytes(offset - len(table)) + body


def iso(files):
    """A small ISO 9660 image (2048-byte sectors): files [(name, bytes)] in the root directory."""
    sectors = {}
    lba = 20
    records = b""
    for name, data in [(".", None), ("..", None)] + list(files):
        if data is None:
            ident, ext, size, flags = (b"\x00" if name == "." else b"\x01"), 18, 2048, 2
        else:
            ident, ext, size, flags = name.encode() + b";1", lba, len(data), 0
            sectors[lba] = data
            lba += (len(data) + 2047) // 2048
        rec = struct.pack("<BBI4xI4x7xBBBH2xB", 0, 0, ext, size, flags, 0, 0, 1, len(ident)) + ident
        rec += bytes(len(rec) % 2)
        records += bytes((len(rec),)) + rec[1:]
    image = bytearray(lba * 2048)
    pvd = bytearray(2048)
    pvd[0:6] = b"\x01CD001"
    root = struct.pack("<BBI4xI4x7xBBBH2xB", 34, 0, 18, 2048, 2, 0, 0, 1, 1) + b"\x00"
    pvd[156:156 + 34] = root
    image[16 * 2048:17 * 2048] = pvd
    image[18 * 2048:18 * 2048 + len(records)] = records
    for at, data in sectors.items():
        image[at * 2048:at * 2048 + len(data)] = data
    return bytes(image)


def raw(image):
    """2048-byte sectors as 2352-byte ones (sync, header, data, error correction space)."""
    image += bytes(-len(image) % 2048)
    return b"".join(discimage.SECTOR_SYNC + bytes(12) + image[i:i + 2048] + bytes(280)
                    for i in range(0, len(image), 2048))


# ---------------------------------------------------------------------------------------------------- tests

class BankTests(unittest.TestCase):
    def check_bank(self, data):
        bank = aad.Snd(data)
        self.assertEqual(2, len(bank.programs))
        self.assertEqual(4, len(bank.tones))
        vh, vb, info = aad.to_vab(bank, SMP)
        self.assertEqual(SPU, vb)                               # (the samples as they are)
        v = vabmod.Vab(vh, vb)
        self.assertEqual([0, 1], v.programs())
        self.assertEqual([SPU[:0x30], SPU[0x30:0x50], SPU[0x50:0x60]], [v.vag_data(n) for n in (1, 2, 3)])
        low, high = v.tones(0)
        self.assertEqual((0, 59, 60, 0, 4, 1), (low["min"], low["max"], low["center"], low["shift"], low["mode"],
                                                low["vag"]))
        # -10 units: the library's pitch 4096 - (150732 * 10 * 100 >> 23) = 4079, -7.2 cents: the key above, 119/128
        self.assertEqual((60, 127, 73, 119, 0, 7, 2), (high["min"], high["max"], high["center"], high["shift"],
                                                       high["mode"], high["pbmax"], high["vag"]))
        self.assertEqual((0x80FF, 0x5FC0), (low["adsr1"], low["adsr2"]))
        a, b = v.tones(1)
        self.assertEqual((60, 95, 3), (a["center"], a["shift"], a["vag"]))     # (100 units: 4275, 74 cents, 95/128)
        self.assertEqual((48, 0, 0, 2), (b["center"], b["shift"], b["pan"], b["vag"]))
        self.assertEqual((110, 60), (v.prog(0)["mvol"], v.prog(0)["mpan"]))
        return bank

    def test_gex3(self):
        bank = self.check_bank(GEX)
        self.assertEqual(0, aad.wave_base(bank, aad.smp_data(SMP)))
        self.assertEqual(2, len(bank.sequences))

    def test_akuji(self):
        bank = self.check_bank(AKUJI)
        self.assertEqual(0x1010, aad.wave_base(bank, aad.smp_data(SMP)))      # (SPU RAM addresses)
        self.assertEqual(0x45, bank.sequences[1].version)

    def test_more_tones_counted(self):
        bank = aad.Snd(snd(PROGRAMS, TONES, WAVES, [MUSIC], [0], extra_tones=3))
        self.assertEqual(2, len(bank.program_tones(1)))
        self.assertEqual(4, aad.to_vab(bank, SMP)[2]["tones"])

    def test_wrong_samples(self):
        bank = aad.Snd(GEX)
        self.assertFalse(aad.smp_fits(bank, aad.smp_data(SMP)[16:]))        # (shifted: no wave on a sample start)
        self.assertFalse(aad.smp_fits(bank, aad.smp_data(SMP)[:0x50]))      # (too short: wave C at 0x50)
        with self.assertRaises(aad.FormatError):
            aad.to_vab(bank, SMP[:0x40])

    def test_bad_files(self):
        for bad in (b"", GEX[:0x1F], b"pBAV" + GEX[4:], GEX[:0x90], AKUJI[:0x60], GEX[:0x80] + bytes(400)):
            with self.assertRaises(aad.FormatError):
                aad.Snd(bad)
        bank = aad.Snd(GEX[:len(GEX) - 6])                    # (the music cut short: what was read is kept)
        self.assertFalse(bank.sequences[1].ended)


class SequenceTests(unittest.TestCase):
    def test_music(self):
        music = aad.Snd(GEX).sequences[1]
        self.assertEqual(2, music.notes)
        self.assertTrue(music.ended)
        self.assertEqual({0: 0}, music.label_ticks())
        data, info = aad.to_seq(music)
        self.assertEqual((0, 480, 2, [0]), (info["loop"], info["loop_end"], info["notes"], info["channels"]))
        self.assertEqual(["variables (1)"], info["skipped"])
        res, tempo, _, _, body = audioconv.seq_tracks(data)[0]
        self.assertEqual((480, 0x07A120), (res, tempo))
        events, ticks = audioconv._seq_events(body)
        self.assertEqual(480, ticks)
        evs = [(t, k, bytes(d) if d is not None and k == "ev" else d) for t, k, d in events]
        self.assertIn((0, "start", None), evs)
        self.assertIn((480, "end", None), evs)
        notes = [(t, d) for t, k, d in evs if k == "ev" and d[0] & 0xF0 in (0x90, 0xE0)]
        # The bend after the second note on too (the library: a note starts bent)
        self.assertEqual([(0, b"\x90\x3C\x64"), (240, b"\x90\x3C\x00"), (240, b"\xE0\x00\x50"),
                          (240, b"\x90\x48\x50"), (240, b"\xE0\x00\x50"), (480, b"\x90\x48\x00")], notes)
        self.assertNotIn(b"\xB0\x00\x01", [d for t, k, d in evs if k == "ev"])   # (bank select: left out)

    def test_counted_loop(self):
        s = aad.Sequence(seq([(0, cmd(46, 3)), (0, b"\x90\x3C\x64"), (120, b"\x3C\x00"), (0, cmd(47)),
                              (0, b"\x90\x40\x64"), (120, b"\x40\x00")]))
        data, info = aad.to_seq(s)
        events, ticks = audioconv._seq_events(audioconv.seq_tracks(data)[0][4])
        # (written out: 3 times the loop, then the rest)
        self.assertEqual([(0, 60), (120, 60), (240, 60), (360, 64)],
                         [(t, d[1]) for t, k, d in events if k == "ev" and d[0] == 0x90 and d[2]])
        self.assertEqual([], [k for t, k, d in events if k != "ev"])
        self.assertEqual(480, ticks)
        self.assertIsNone(info["loop"])                        # (counted: the music goes on)

    def test_tracks_looping_apart(self):
        """Soul Reaver: tracks looping over different lengths (and a counted loop inside an endless one, a jump
        forward over a part): the SEQ's loop where they all start again together."""
        a = [(0, cmd(46, 0)), (0, b"\x90\x3C\x64"), (240, b"\x3C\x00"), (0, cmd(47))]           # every 240
        b = [(0, cmd(46, 0)), (0, cmd(46, 2)), (0, b"\x91\x40\x64"), (180, b"\x40\x00"), (0, cmd(47)),
             (0, cmd(50, 0)), (0, b"\x91\x50\x64"), (0, cmd(49, 0)), (0, cmd(47))]             # every 360
        body_a = b"".join(varlen(d) + bytes(e) for d, e in a)
        body_b = b"".join(varlen(d) + bytes(e) for d, e in b)
        label = body_b.index(cmd(49, 0)) + 4                    # (after DefineLabel 0)
        tracks = [body_a + b"\x00\xFF\x44\x00", body_b + b"\x00\xFF\x44\x00"]
        offsets = [16 + 8, 16 + 8 + len(tracks[0])]
        s = aad.Sequence(b"QSMa" + struct.pack("<IHHHH", 500000, 480, 0x106, 2, 0) +
                         struct.pack("<2I", *offsets) + b"".join(tracks), [label])
        self.assertEqual(2, s.tracks)
        data, info = aad.to_seq(s)
        self.assertEqual((0, 720), (info["loop"], info["loop_end"]))   # (the least common multiple)
        events, ticks = audioconv._seq_events(audioconv.seq_tracks(data)[0][4])
        ons = [(t, d[0] & 15, d[1]) for t, k, d in events if k == "ev" and d[0] & 0xF0 == 0x90 and d[2]]
        self.assertEqual([(0, 0, 60), (240, 0, 60), (480, 0, 60)], [n for n in ons if n[1] == 0])
        self.assertEqual([(0, 1, 64), (180, 1, 64), (360, 1, 64), (540, 1, 64)], [n for n in ons if n[1] == 1])
        self.assertEqual(720, ticks)

    def test_transpose(self):
        s = aad.Sequence(seq([(0, cmd(0, 2)), (0, cmd(28, 0xF4)), (0, b"\x92\x48\x64"), (0, b"\x90\x48\x64"),
                              (120, cmd(28, 5)), (0, b"\x92\x48\x00"), (0, cmd(29)), (0, b"\x92\x40\x64"),
                              (120, b"\x92\x40\x00")]))
        data, info = aad.to_seq(s)
        events, _ = audioconv._seq_events(audioconv.seq_tracks(data)[0][4])
        notes = [(t, d[0] & 15, d[1], d[2]) for t, k, d in events if k == "ev" and d[0] & 0xF0 == 0x90]
        # Channel 2 down 12 (its note off as its note on, though the transpose changed), channel 0 not; then
        # channel 2's transpose ignored
        self.assertEqual([(0, 2, 60, 100), (0, 0, 72, 100), (120, 2, 60, 0), (120, 2, 64, 100), (240, 2, 64, 0)],
                         notes)
        self.assertEqual(1, info["transposed"])

    def test_played_by_the_console_player(self):
        data, _ = aad.to_seq(aad.Snd(GEX).sequences[1])
        h = run_bytes(data, play_ms=1500, repeat=0)
        self.assertIsNone(h.problem())
        # 0.5 s a beat: the loop's 2 notes every 0.5 s (endless)
        self.assertEqual([60, 72, 60, 72, 60, 72], [n[3] for n in h.notes[:6]])
        self.assertAlmostEqual(0.5, (h.notes[2][0] - h.notes[0][0]) / 1e6, delta=0.03)

    def test_control_sequence(self):
        self.assertEqual(0, aad.Snd(GEX).sequences[0].notes)   # (scan writes no SEQ for it)


class FileTests(unittest.TestCase):
    def test_lz(self):
        data = bytes(range(200)) * 3
        self.assertEqual(data, aad.unlz(lz_literal(data), len(data)))
        # A copy: "ab" then 6 bytes from 2 back
        self.assertEqual(b"abababab", aad.unlz(bytes((0b100, 0)) + b"ab" + bytes((0x05, 0x02)), 8))
        for bad in (b"\x00", bytes((1, 0, 0x05, 0x09))):
            with self.assertRaises(aad.FormatError):
                aad.unlz(bad, 8)

    def test_bigfile_and_pairing(self):
        other_smp = b"PMSa" + struct.pack("<I", 0x90) + bytes(0x88)         # (no waves of the bank)
        big = bigfile([b"TIM junk" * 50, GEX, other_smp, SMP, AKUJI, SMP], packed=(1, 4))
        for name, image in (("archive", big), ("2048", iso([("BIGFILE.DAT", big)])),
                            ("2352", raw(iso([("README.TXT", b"x" * 3000), ("BIGFILE.DAT", big)])))):
            with self.subTest(image=name):
                banks = aad.scan(image)
                self.assertEqual(2, len(banks))
                self.assertEqual(["001", "004"], [b["name"][-3:] for b in banks])
                self.assertEqual(["003", "005"], [b["smp_name"][-3:] for b in banks])
                self.assertEqual(aad.smp_data(SMP), banks[0]["smp"])

    def test_loose_files(self):
        image = iso([("MUSIC.SND", GEX), ("MUSIC.SMP", SMP)])
        banks = aad.scan(image)
        self.assertEqual(["/MUSIC.SND"], [b["name"] for b in banks])
        self.assertEqual("/MUSIC.SMP", banks[0]["smp_name"])

    def test_no_samples(self):
        banks = aad.scan(bigfile([GEX], packed=(0,)))
        self.assertIsNone(banks[0]["smp"])

    def test_image_reading(self):
        data = bytes(range(256)) * 40                           # 10 KB: 5 sectors
        for image in (data, raw(data)):
            img = discimage.Image(image)
            self.assertEqual(len(data) if image is data else 5 * 2048, len(img))
            self.assertEqual(data[2040:2060], img[2040:2060])   # (across a sector)
            self.assertEqual(data[:6000], img.read(0, 6000))
            self.assertEqual(data[5], img[5])
        img = discimage.Image(raw(b"\x00" * 3000 + b"FIND" + b"\x00" * 9000 + b"FIND"))
        old, discimage.FIND_CHUNK = discimage.FIND_CHUNK, 4096   # (matches across the pieces read)
        try:
            self.assertEqual([3000, 12004], list(img.find(b"FIND")))
        finally:
            discimage.FIND_CHUNK = old

    def test_bad_archives(self):
        big = bigfile([GEX, SMP], packed=(0,))
        for bad in (big[:0x7FF], big[:0x800], struct.pack("<I", 50000) + big[4:], big[:0x900]):
            self.assertEqual([], aad.scan(bad))


def sfx_pair(uid=7, effects=((300, 1, 0x30), (301, 2, 0x10), (302, 1, 0x30))):
    """A sound effect set (Soul Reaver): SNF and SMF; effects (effect ID, wave ID, wave size), a wave's data each
    time it is used (as in the files)."""
    snf = b"FNSa" + struct.pack("<4H", 0x100, uid, 0, len(effects)) + bytes(8) + b"".join(
        struct.pack("<H", e[0]) for e in effects)
    smf = b"FMSa" + struct.pack("<3H", 0x100, uid, len(effects)) + bytes(6)
    for sfx, wave, size in effects:
        data = (bytes(16) + block(1, 0x40 + wave))[:size] if size == 0x10 else bytes(16) + block(0, wave) + \
            block(1, wave)
        smf += struct.pack("<HH", sfx, wave) + tone(0, 60, 60, 60, 10, 0, mode=4) + struct.pack("<I", len(data))
        smf += data
    return snf, smf


class SfxTests(unittest.TestCase):
    """Sound effect sets (FNSa + FMSa: Soul Reaver)."""

    def test_set(self):
        snf, smf = sfx_pair()
        s = aad.SfxSet(snf, smf)
        self.assertEqual([300, 301, 302], s.ids)
        self.assertEqual([1, 2, 1], [e["wave"] for e in s.effects])
        self.assertEqual(len(smf), aad.SfxSet.smf_size(smf + b"junk"))
        vh, vb, info = aad.sfx_to_vab(s)
        self.assertEqual((3, 2, [300, 301, 302]), (info["effects"], info["samples"], info["ids"]))
        bank = vabmod.Vab(vh, vb)
        self.assertEqual([0, 1, 2], bank.programs())
        self.assertEqual([1, 2, 1], [bank.tones(p)[0]["vag"] for p in range(3)])     # (wave 1 once)
        t = bank.tones(0)[0]
        self.assertEqual((0, 127, 60, aad.tune(60, 10)[1], 4), (t["min"], t["max"], t["center"], t["shift"],
                                                                t["mode"]))
        self.assertEqual(len(vb), sum(bank.vag_sizes()))

    def test_demo_seq(self):
        data, seconds = aad.sfx_demo_seq(aad.SfxSet(*sfx_pair()))
        h = run_bytes(data, play_ms=2500, repeat=0)
        self.assertIsNone(h.problem())
        self.assertEqual([(0, 60), (0, 60), (0, 60)], [(n[2], n[3]) for n in h.notes])
        self.assertAlmostEqual(1.75, seconds)

    def test_not_a_pair(self):
        snf, _ = sfx_pair(uid=7)
        _, smf = sfx_pair(uid=8)
        for a, b in ((snf, smf), (snf, smf[:60]), (b"pBAV" + snf[4:], smf)):
            with self.assertRaises(aad.FormatError):
                aad.SfxSet(a, b)

    def test_scan(self):
        snf, smf = sfx_pair()
        other_snf, other_smf = sfx_pair(uid=9, effects=((5, 3, 0x30),))
        d = b"\x11" * 300 + snf + b"\x22" * 40 + other_snf + other_smf + b"\x33" * 9 + smf + b"\x44" * 100
        found = aad.scan_sfx(d)
        # (each SNF with the next SMF of its ID: the other set's SMF, between them, skipped)
        self.assertEqual([(300, 7), (300 + len(snf) + 40, 9)], sorted((f["at"], f["sfx"].uid) for f in found))
        self.assertEqual(smf, next(f["smf"] for f in found if f["sfx"].uid == 7))
        self.assertEqual([], aad.scan_sfx(b"\x11" * 300 + snf))                      # (no SMF)


FRAME_TOLERANCE_US = 20000      # A frame (16.7 ms NTSC) and a little: how far a note can move at the player's frames


class CorpusTests(unittest.TestCase):
    """Game files (PS1_AAD_CORPUS): disc images or BIGFILE.DATs; every bank converted, every music played."""

    def test_convert(self):
        files = [p for d in os.environ.get("PS1_AAD_CORPUS", "").split(";") if d
                 for p in sorted(glob.glob(os.path.join(d, "*")))
                 if p.lower().endswith((".bin", ".img", ".iso", ".dat"))]
        if not files:
            self.skipTest("no PS1_AAD_CORPUS")
        for path in files:
            banks = aad.scan(path)
            for b in banks:
                with self.subTest(file=os.path.basename(path), bank=b["name"]):
                    self.assertIsNotNone(b["smp"], "no samples found for the bank")
                    vh, vb, _ = aad.to_vab(b["snd"], b["smp"])
                    bank = vabmod.Vab(vh, vb)
                    self.assertEqual(len(vb), sum(bank.vag_sizes()))
                    for n, s in enumerate(b["snd"].sequences):
                        if not s.notes:
                            continue
                        data, info = aad.to_seq(s)
                        res, tempo, body = seqref.tracks(data)[0]
                        evs = seqref.events(body)
                        # (up to 2 s after the first note: some musics start with a long silence, Gex 3's 22 s)
                        ref, _ = seqref.play(res, tempo, evs, 600e6, loops=False)
                        horizon = ref[0][0] + 2e6
                        h = run_bytes(data, play_ms=int(horizon / 1e3), repeat=0)
                        self.assertIsNone(h.problem(), "sequence %d" % n)
                        # The notes before the cut, within a frame: the player starts its notes at frames (a note or
                        # a chord at most a frame from the cut can fall on either side of it)
                        cut = horizon - 0.1e6
                        played = len([g for g in h.notes if g[1] < cut])
                        lo = len([r for r in ref if r[0] < cut - FRAME_TOLERANCE_US])
                        hi = len([r for r in ref if r[0] < cut + FRAME_TOLERANCE_US])
                        self.assertTrue(lo <= played <= hi, "sequence %d: %d notes played, the reference %d to %d"
                                        % (n, played, lo, hi))


if __name__ == "__main__":
    unittest.main()
