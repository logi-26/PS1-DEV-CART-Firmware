"""
Square's AKAO music (gui/akao.py): sequences (versions 1.0 and 3.x) read and played as the driver plays them, sample
sets, FF7's INSTR.ALL / INSTR.DAT, LZS, and the SEQ + VAB made for the sound tester; the game discs of PS1_AKAO_CORPUS
(folders of disc images: every music found converted, its notes with a tone).

The files here are made by the tests: AKAO's layout as VGMTrans (zlib licensed) describes it.
"""

import glob
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "gui"))
import akao                                                 # noqa: E402
import audioconv                                            # noqa: E402
import vab as vabmod                                        # noqa: E402


def note(key, length_index=2):
    """A note byte: key (0-11) x 11 + one of the 11 lengths (2: 48 ticks, a beat)."""
    return key * 11 + length_index


TIE, REST = 0x84 + 2, 0x8F + 2          # (a beat each)


def seq_v1(tracks, ident=1):
    """A version 1.0 sequence: 14h-byte header (size from 10h), each track's offset from after itself."""
    n = len(tracks)
    head = 0x14 + 2 * n
    body, offsets = b"", []
    for t in tracks:
        offsets.append(head + len(body))
        body += bytes(t)
    table = b"".join(struct.pack("<H", o - (0x14 + 2 * i + 2)) for i, o in enumerate(offsets))
    total = head + len(body)
    return (b"AKAO" + struct.pack("<HHH", ident, total - 0x10, 0) + bytes(6) + struct.pack("<I", (1 << n) - 1) +
            table + body)


def seq_v3(tracks, ident=1, sample_set=64, instruments=b"", kit=b""):
    """A version 3 sequence: 40h-byte header, track offsets from themselves, then the instruments and drum kit."""
    n = len(tracks)
    head = 0x40 + 2 * n
    body, offsets = b"", []
    for t in tracks:
        offsets.append(head + len(body))
        body += bytes(t)
    table = b"".join(struct.pack("<H", o - (0x40 + 2 * i)) for i, o in enumerate(offsets))
    at = head + len(body)
    instr_at = at if instruments else 0
    kit_at = at + len(instruments) if kit else 0
    total = at + len(instruments) + len(kit)
    h = bytearray(0x40)
    h[0:4] = b"AKAO"
    struct.pack_into("<HHH", h, 4, ident, total, 0)
    struct.pack_into("<H", h, 0x14, sample_set)
    struct.pack_into("<I", h, 0x20, (1 << n) - 1)
    struct.pack_into("<II", h, 0x30, instr_at - 0x30 if instr_at else 0, kit_at - 0x34 if kit_at else 0)
    return bytes(h) + table + body + instruments + kit


def sample(blocks=4, loop_block=None):
    """SPU ADPCM: a zero block, then blocks, the last with the end flag."""
    d = bytearray(16 * (blocks + 1))
    for b in range(1, blocks + 1):
        d[16 * b] = 0x0C
        d[16 * b + 2:16 * b + 16] = bytes((b * 17 + k) & 0xFF for k in range(14))
    d[-15] = 1
    return bytes(d)


def sample_set(ident, first, count, spu, loop=True):
    """A version 3.1+ sample set: count articulations on one sample each."""
    samples, arts = b"", b""
    for i in range(count):
        s = sample(4)
        arts += struct.pack("<IIhHHH", len(samples), len(samples) + 32 if loop else len(samples), 0, 60, 0x80FF,
                            0x5FC0)
        samples += s
    h = bytearray(0x40)
    h[0:4] = b"AKAO"
    struct.pack_into("<H", h, 4, ident)
    struct.pack_into("<IIII", h, 0x10, spu, len(samples), first, count)
    return bytes(h) + arts + samples


class SequenceTests(unittest.TestCase):

    def play(self, data, n=0, version=None):
        s = akao.Sequence(data, version)
        return s, akao.Track(s, n, 48 * 1000)

    def test_versions(self):
        self.assertEqual(akao.V1_0, akao.Sequence(seq_v1([[0xA5, 4, note(0), 0xA0]])).version)
        self.assertEqual(akao.V3_2, akao.Sequence(seq_v3([[0xA5, 4, note(0), 0xA0]])).version)
        with self.assertRaises(akao.FormatError):
            akao.Sequence(b"AKAO" + bytes(60))                  # (size 0: a sample set)
        with self.assertRaises(akao.FormatError):
            akao.Sequence(seq_v1([[0x9A, 0xA0]]))               # (9Ah-9Fh: no command)

    def test_notes_ties_rests(self):
        # Octave 4: C, then a tie (one more beat), a rest, B with the shortest length (83h: a note, not a tie)
        s, t = self.play(seq_v1([[0xA5, 4, note(0), TIE, REST, 0x83, 0xA0]]))
        notes = [e for e in t.events if e[1] == "note"]
        self.assertEqual([(0, 48), (144, 59)], [(e[0], e[2][0]) for e in notes])
        self.assertEqual(48 + 46, notes[0][2][1])               # (version 1: a note ends 2 ticks before the next)
        self.assertEqual(144 + 4 - 2, notes[1][2][1])
        self.assertEqual(148, t.end)

    def test_version_3_note_lengths(self):
        # F0h-FDh: a key and its length in the next byte; the note lasts all of it
        s, t = self.play(seq_v3([[0xA5, 4, 0xF0 + 2, 30, 0xA6, note(0), 0xA0]]))
        notes = [e for e in t.events if e[1] == "note"]
        self.assertEqual([(0, 50, 30), (30, 60, 78)], [(e[0], e[2][0], e[2][1]) for e in notes])

    def test_octave_transpose_volume(self):
        s, t = self.play(seq_v1([[0xA5, 3, 0xC0, 2, 0xA3, 100, 0xA8, 90, 0xAA, 30, note(0), 0xA7, note(0), 0xA0]]))
        self.assertEqual([38, 26], [e[2][0] for e in t.events if e[1] == "note"])
        self.assertEqual([(0, "volume", 100), (0, "expression", 90), (0, "pan", 30)],
                         [tuple(e) for e in t.events if e[1] != "note"])

    def test_fades(self):
        s, t = self.play(seq_v1([[0xA8, 0, 0xA9, 12, 60, note(0), 0xA0]]))
        steps = [e for e in t.events if e[1] == "expression"]
        self.assertEqual([(0, 0), (3, 15), (6, 30), (9, 45), (12, 60)], [(e[0], e[2]) for e in steps])

    def test_counted_loop(self):
        # C8h loop start, C9h n: n times in all
        s, t = self.play(seq_v1([[0xA5, 4, 0xC8, note(0), 0xC9, 3, note(2), 0xA0]]))
        self.assertEqual([0, 48, 96, 144], [e[0] for e in t.events if e[1] == "note"])

    def test_endless_jump_repeats(self):
        # A note, then a jump (EEh, from after itself) back to the note: repeats from tick 0 every beat
        tr = [0xA5, 4, note(0), 0xEE] + list(struct.pack("<h", -4))
        s, t = self.play(seq_v1([tr]))
        self.assertEqual((0, 48, 0), t.repeat)
        self.assertIsNone(t.end)

    def test_loop_again_repeats(self):
        # An endless loop (CAh) with a counted one inside: the outer count isn't compared, so it repeats
        tr = [0xA5, 4, 0xC8, note(0), 0xC8, note(1), 0xC9, 2, 0xCA]
        s, t = self.play(seq_v1([tr]))
        self.assertEqual(144, t.repeat[1])

    def test_patterns_and_conditions(self):
        # 3.2: FEh 0Eh calls a pattern (offset from its own bytes), FEh 0Fh returns; FEh 07h jumps when the game's
        # condition (0) is its value
        tr = [0xA5, 4, 0xFE, 0x0E, 4, 0, note(0), 0xA0, note(5), 0xFE, 0x0F]
        s, t = self.play(seq_v3([tr]))
        self.assertEqual([53, 48], [e[2][0] for e in t.events if e[1] == "note"])
        tr = [0xA5, 4, 0xFE, 0x07, 0, 3, 0, note(1), note(2), 0xA0]
        s, t = self.play(seq_v3([tr]))
        self.assertEqual([50], [e[2][0] for e in t.events if e[1] == "note"])

    def test_tempo(self):
        # The driver's counter: 48 ticks a beat; tempo value t: t / 65536 * (33868800 / 8) / 44E8h ticks a second
        us = akao.tempo_us(akao.V3_2, 0x2000)
        self.assertAlmostEqual(48 * 1e6 / (0x2000 / 65536 * 4233600 / 0x44E8), us, delta=1)
        self.assertNotEqual(us, akao.tempo_us(akao.V1_0, 0x2000))


class InstrumentTests(unittest.TestCase):

    def test_sample_set(self):
        s = akao.SampleSet(sample_set(64, 64, 3, 0x35100))
        self.assertEqual((64, 64, 3, 0x35100), (s.id, s.first, s.count, s.spu))
        self.assertEqual([64, 65, 66], sorted(s.arts))
        a = s.arts[65]
        self.assertEqual((80, 32, 60), (a.start, a.loop, a.root))

    def test_resident_sets(self):
        # The song's set (named by the sequence) over the sets kept loaded below it in SPU RAM; a set at an address
        # overlapping one of those isn't loaded with them
        base = ("", akao.SampleSet(sample_set(0, 0, 32, 0x3100)))
        mid = ("", akao.SampleSet(sample_set(5, 32, 32, 0x23100)))
        clash = ("", akao.SampleSet(sample_set(7, 32, 32, 0x3200)))
        song = ("", akao.SampleSet(sample_set(64, 64, 8, 0x35100)))
        other = ("", akao.SampleSet(sample_set(65, 64, 8, 0x35100)))
        seq = akao.Sequence(seq_v3([[0xA0]], sample_set=64))
        arts, names = akao.articulations_for(seq, [clash, base, mid, other, song])
        self.assertEqual("set 0, set 5, set 64", names)
        self.assertIs(arts[70].samples, song[1].data)
        self.assertIs(arts[40].samples, mid[1].data)

    def test_instr_all(self):
        # FF7: INSTR.ALL (SPU address, size, 8 bytes, samples), INSTR.DAT (64 bytes: addresses, ADSR bytes, pitches)
        samples = sample(4) + sample(2)
        all_data = struct.pack("<II8x", 0x1010, len(samples)) + samples
        dat = bytearray(128)
        struct.pack_into("<II8BI", dat, 0, 0x1010, 0x1030, 0x1C, 0x0F, 0x0F, 0x7F, 6, 1, 3, 3, 0x1000)
        struct.pack_into("<II8BI", dat, 64, 0x1010 + 80, 0x1010 + 80, 0x30, 0, 0x0F, 0, 0x1F, 5, 7, 7, 0x2000)
        arts = akao.instr_all(all_data, bytes(dat))
        self.assertEqual((0, 32, 72.0), (arts[0].start, arts[0].loop, arts[0].root))
        self.assertAlmostEqual(60.0, arts[1].root)                  # (twice the pitch: an octave lower root)
        self.assertEqual(akao.adsr_regs(0x1C, 0x0F, 0x0F, 0x7F, 6, 1, 3, 3), (arts[0].adsr1, arts[0].adsr2))

    def test_adsr(self):
        a1, a2 = akao.adsr_regs(0x30, 4, 9, 0x40, 0x1A, 5, 7, 7)
        self.assertEqual((0x8000 | 0x30 << 8 | 4 << 4 | 9, 0xC000 | 0x40 << 6 | 0x20 | 0x1A), (a1, a2))
        self.assertEqual(akao.adsr_regs(0x10, 4, 9, 0x40, 0x1A, 5, 7, 7), akao.with_adsr(a1, a2, {"ar": 0x10}))

    def test_own_instruments_and_kit(self):
        # FEh 14h n: the sequence's instrument n (regions: articulation, keys, ADSR bytes, volume); FEh 04h: its
        # drum kit (8 bytes a key)
        regions = bytes([64, 0, 59, 0x30, 0x40, 4, 0x1A, 0]) + bytes([65, 60, 127, 0x30, 0x40, 4, 0x1A, 64]) + bytes(8)
        table = struct.pack("<H", 0) + b"\xFF\xFF" * 15
        kit = bytearray(8 * 50)
        kit[8 * 48:8 * 49] = bytes([66, 60, 0x30, 0x40, 4, 0x1A, 0, 0x80 | 20])
        seq = akao.Sequence(seq_v3([[0xA0]], instruments=table + regions, kit=bytes(kit)))
        own, k = akao.instruments(seq)
        self.assertEqual([(64, 0, 59, 127), (65, 60, 127, 63)], [(r.art, r.lo, r.hi, r.vol) for r in own[0]])
        self.assertEqual((66, 60, 20, True), (k[48].art, k[48].play_key, k[48].pan, k[48].reverb))


class LzsTests(unittest.TestCase):

    def test_unlzs(self):
        # "ABCD" as they are (flags 1), then a copy of 6 from the ring's start (FEEh: the first byte written)
        body = bytes([0b00001111]) + b"ABCD" + bytes([0xEE, 0xF3])
        self.assertEqual(b"ABCDABCDAB", akao.unlzs(struct.pack("<I", len(body)) + body))
        self.assertIsNone(akao.unlzs(b"\xFF\xFF\x00\x00" + body))


class ConvertTests(unittest.TestCase):

    def convert(self, tracks, count=8):
        seq = akao.Sequence(seq_v3(tracks))
        arts, _ = akao.articulations_for(seq, [("", akao.SampleSet(sample_set(64, 64, count, 0x35100)))])
        data, vh, vb, info = akao.convert(seq, arts)
        bank = vabmod.Vab(vh, vb)
        events, ticks = audioconv._seq_events(audioconv.seq_tracks(data)[0][4])
        return data, bank, events, ticks, info

    def test_seq_and_vab(self):
        tr = [0xA1, 64, 0xA5, 4, note(0), note(4), note(7), 0xA1, 65, note(0), 0xA0]
        data, bank, events, ticks, info = self.convert([tr])
        self.assertEqual(4, info["notes"])
        self.assertEqual(2, info["programs"])
        self.assertEqual(192, ticks)
        ons = [d for _, k, d in events if k == "ev" and d[0] & 0xF0 == 0x90 and d[2]]
        self.assertEqual([48, 52, 55, 48], [d[1] for d in ons])
        self.assertEqual(sum(bank.vag_sizes()), len(bank.vb) if hasattr(bank, "vb") else sum(bank.vag_sizes()))
        tone = bank.tones(0)[0]
        self.assertEqual((60, 0, 0, 127), (tone["center"], tone["shift"], tone["min"], tone["max"]))

    def test_loop(self):
        # Two tracks repeating every beat and every two beats: one loop of two beats
        a = [0xA1, 64, 0xA5, 4, note(0), 0xFE, 0x06] + list(struct.pack("<h", -3))
        b = [0xA1, 64, 0xA5, 4, note(4), note(7), 0xFE, 0x06] + list(struct.pack("<h", -4))
        data, bank, events, ticks, info = self.convert([a, b])
        self.assertEqual((0, 96), (info["loop"], info["loop_end"]))

    def test_tracks_over_16(self):
        # 20 tracks with a held note each and 4 more after them: the notes share the 16 channels when free
        tracks = [[0xA1, 64, 0xA5, 4, 0xA3, 100, note(i % 12, 0), 0xA0] for i in range(16)]
        tracks += [[0xA1, 64, 0xA5, 4, REST, 0xA3, 50 + i, note(i, 2), 0xA0] for i in range(4)]
        data, bank, events, ticks, info = self.convert(tracks)
        ons = [d for _, k, d in events if k == "ev" and d[0] & 0xF0 == 0x90 and d[2]]
        self.assertEqual(20, len(ons))
        self.assertEqual(16, len({d[0] & 15 for d in ons}))
        self.assertTrue(any("sharing a channel" in s for s in info["skipped"]))

    def test_vag_loop_flags(self):
        art = akao.Articulation(sample(4), 0, 32, 60, 0, 0)
        v = akao._vag(art)
        self.assertEqual([0, 0, 4, 0, 3], [v[p + 1] & 7 for p in range(0, len(v), 16)])
        art = akao.Articulation(sample(4), 0, None, 60, 0, 0)
        self.assertEqual(1, akao._vag(art)[-15] & 7)


class PitchTests(unittest.TestCase):
    """Vibrato and slides as the driver plays them (FF7's driver, SCUS_941.63: B4h at 80032614h, its update at
    8002ED74h, the slide at a note's start at 80031620h)."""

    TEMPO = 500000                                      # (us a beat: 48 ticks of 10416 us)

    def test_slide(self):
        # A4h 12, 12: an octave up over 12 ticks, a straight line in Hz (half way: 1.5 times the pitch, 702 cents)
        c = dict(akao.pitch_curve(akao.V1_0, 0, 48, dict(slide=(12, 12)), lambda t: 0, self.TEMPO))
        self.assertAlmostEqual(0.0, c[0])
        self.assertAlmostEqual(1200 * __import__("math").log2(1.5), c[6], places=3)
        self.assertAlmostEqual(1200.0, c[12])
        self.assertEqual(12, max(c))                    # (no more points once it stays there)

    def test_vibrato(self):
        # Delay 6 ticks, a step each driver update (rate 1), the 20-step sine (type 11), depth 127 at the 1/1 scale
        # (bit 7): up to depth/128 of the pitch up (doubled), half that down
        c = akao.pitch_curve(akao.V3_2, 0, 96, dict(vib=(6, 1, 11)), lambda t: 0x80 | 127, self.TEMPO)
        self.assertTrue(all(abs(x) < 1e-9 for t, x in c if t < 6))
        top = 1200 * __import__("math").log2(1 + 127 / 128.0)
        low = 1200 * __import__("math").log2(1 - 127 / 128.0 / 2)
        self.assertLessEqual(max(x for _, x in c), top + 1e-6)
        self.assertGreater(max(x for _, x in c), top * 0.9)
        self.assertGreaterEqual(min(x for _, x in c), low - 1e-6)
        # The 15/256 scale (bit 7 clear): about +-6 and -3 cents at depth 10 (FF7's usual)
        c = akao.pitch_curve(akao.V1_0, 0, 96, dict(vib=(0, 1, 11)), lambda t: 10, self.TEMPO)
        self.assertTrue(5 < max(x for _, x in c) < 8.5)
        # Depth 0: no curve
        self.assertEqual([], akao.pitch_curve(akao.V1_0, 0, 96, dict(vib=(0, 1, 11)), lambda t: 0, self.TEMPO))

    def test_waves(self):
        for version in (akao.V1_0, akao.V3_2):
            waves = akao.lfo_waves(version)
            self.assertEqual(16, len(waves))
            for values, loop in waves:
                self.assertTrue(0 < loop <= len(values))
                self.assertTrue(all(-32768 <= v <= 32767 for v in values))
        # Past the end: its repeating part again (type 6: four growing cycles of 39, then the last one)
        values, loop = akao.lfo_waves(akao.V1_0)[6]
        self.assertEqual(156, len(values))
        self.assertEqual(values[117 + 5], akao._wave_at(akao.V1_0, 6, 156 + 5))
        self.assertEqual(values[117 + 5], akao._wave_at(akao.V1_0, 6, 156 + 39 + 5))

    def test_converted_bends(self):
        # A track with vibrato and a slid note, another without: the bends on the first track's channel only, the
        # VAB's bend range wide enough for the slide (7 semitones)
        a = [0xA1, 64, 0xA5, 4, 0xB4, 0, 1, 11, 0xB5, 0x80 | 64, note(0, 0), 0xA4, 24, 7, note(0, 0), 0xA0]
        b = [0xA1, 64, 0xA5, 4, note(4, 0), note(4, 0), 0xA0]
        seq = akao.Sequence(seq_v3([a, b]))
        arts, _ = akao.articulations_for(seq, [("", akao.SampleSet(sample_set(64, 64, 4, 0x35100)))])
        data, vh, vb, info = akao.convert(seq, arts)
        self.assertEqual(2, info["bent"])
        self.assertGreaterEqual(info["bend_range"], 8)
        bank = vabmod.Vab(vh, vb)
        self.assertEqual(info["bend_range"], bank.tones(0)[0]["pbmax"])
        events, _ = audioconv._seq_events(audioconv.seq_tracks(data)[0][4])
        evs = [d for _, k, d in events if k == "ev"]
        a_chan = next(d[0] & 15 for d in evs if d[0] & 0xF0 == 0x90 and d[1] == 48)
        bends = [d for d in evs if d[0] & 0xF0 == 0xE0]
        self.assertTrue(bends)
        self.assertEqual({a_chan}, {d[0] & 15 for d in bends})


class LfoTests(unittest.TestCase):
    """Tremolo and pan LFO as the driver plays them (FF7's driver: B8h at 80032804h, BCh at 8003298Ch, their update at
    8002EE20h-8002EFACh), and overlay voices (F4h)."""

    TEMPO = 500000

    def test_tremolo(self):
        # Delay 6, a step each update, the 20-step sine; depth 64: the expression moved by half of itself, up and down
        c = akao.lfo_curve(akao.V3_2, "trem", 0, 96, (6, 1, 11), lambda t: 64, lambda t: 100, self.TEMPO)
        self.assertTrue(all(v == 100 for t, v in c if t < 6))
        values = [v for _, v in c]
        self.assertTrue(140 > max(values) >= 127 and 45 <= min(values) <= 55)       # (127 at most: clamped)
        # Depth 0 or no LFO change: no curve
        self.assertEqual([], akao.lfo_curve(akao.V3_2, "trem", 0, 96, (0, 1, 11), lambda t: 0, lambda t: 100,
                                            self.TEMPO))

    def test_pan_lfo(self):
        # No delay; depth 40: the pan moved by 20 either way
        c = akao.lfo_curve(akao.V1_0, "pan", 0, 96, (1, 11), lambda t: 40, lambda t: 64, self.TEMPO)
        values = [v for _, v in c]
        self.assertEqual((44, 84), (min(values), max(values)))

    def test_overlay_voice(self):
        # F4h 64, 65: articulation 64 with 65 over it; F6h 32: the balance (65 at 32, 64 at 127 - 32)
        tr = [0xF4, 64, 65, 0xF6, 32, 0xA5, 4, note(0), 0xA0]
        seq = akao.Sequence(seq_v1([tr]))
        arts = akao.SampleSet(sample_set(64, 64, 2, 0x1010)).arts
        data, vh, vb, info = akao.convert(seq, arts)
        tones = vabmod.Vab(vh, vb).tones(0)
        self.assertEqual([95, 32], [t["vol"] for t in tones])
        self.assertEqual(2, len(set(t["vag"] for t in tones)))

    def test_converted_tremolo(self):
        # A track with tremolo, another without: the expression changes on the first one's channel only
        a = [0xA1, 64, 0xA5, 4, 0xA8, 100, 0xB8, 0, 1, 11, 0xB9, 64, note(0, 0), 0xA0]
        b = [0xA1, 64, 0xA5, 4, note(4, 0), 0xA0]
        seq = akao.Sequence(seq_v3([a, b]))
        arts, _ = akao.articulations_for(seq, [("", akao.SampleSet(sample_set(64, 64, 4, 0x35100)))])
        data, vh, vb, info = akao.convert(seq, arts)
        self.assertEqual(1, info["tremolo"])
        events, _ = audioconv._seq_events(audioconv.seq_tracks(data)[0][4])
        evs = [d for _, k, d in events if k == "ev"]
        a_chan = next(d[0] & 15 for d in evs if d[0] & 0xF0 == 0x90 and d[1] == 48)
        expr = [d for d in evs if d[0] & 0xF0 == 0xB0 and d[1] == 11]
        self.assertGreater(len(expr), 5)
        self.assertEqual({a_chan}, {d[0] & 15 for d in expr if d[2] != 127})

    def test_fit_the_tester(self):
        # Too big for the tester: written again with coarser effect steps (here a tiny room forces it)
        a = [0xA1, 64, 0xA5, 4, 0xB4, 0, 1, 11, 0xB5, 0x80 | 64, note(0, 0), note(0, 0), 0xA0]
        seq = akao.Sequence(seq_v3([a]))
        arts, _ = akao.articulations_for(seq, [("", akao.SampleSet(sample_set(64, 64, 4, 0x35100)))])
        full = akao.convert(seq, arts)
        old = akao.TESTER_ROOM
        try:
            akao.TESTER_ROOM = sum(len(x) for x in full[:3]) - 1
            fitted = akao.convert(seq, arts)
        finally:
            akao.TESTER_ROOM = old
        self.assertIn("fitted", fitted[3])
        self.assertLess(len(fitted[0]), len(full[0]))
        self.assertEqual(2, akao.BEND_TICKS)                # (put back)


class CorpusTests(unittest.TestCase):
    """PS1_AKAO_CORPUS: folders (separated by ;) of disc images (.cue or .bin): every music converted."""

    def test_discs(self):
        discs = []
        for d in os.environ.get("PS1_AKAO_CORPUS", "").split(";"):
            if d:
                discs += sorted(glob.glob(os.path.join(d, "**", "*.cue"), recursive=True))
        if not discs:
            self.skipTest("no PS1_AKAO_CORPUS")
        for path in discs:
            with self.subTest(disc=os.path.basename(path)):
                musics = akao.scan(path)
                self.assertTrue(musics)
                notes = silent = 0
                for m in musics:
                    self.assertTrue(m["arts"], m["name"])
                    data, vh, vb, info = akao.convert(m["seq"], m["arts"])
                    bank = vabmod.Vab(vh, vb)
                    events, _ = audioconv._seq_events(audioconv.seq_tracks(data)[0][4])
                    prog = {}
                    for _, kind, d in events:
                        if kind != "ev":
                            continue
                        if d[0] & 0xF0 == 0xC0:
                            prog[d[0] & 15] = d[1]
                        elif d[0] & 0xF0 == 0x90 and d[2]:
                            notes += 1
                            p = prog.get(d[0] & 15, 0)
                            if p not in bank.blocks or not any(t["min"] <= d[1] <= t["max"] for t in bank.tones(p)):
                                silent += 1
                self.assertEqual(0, silent, "%d of %d notes with no tone" % (silent, notes))


if __name__ == "__main__":
    unittest.main()
