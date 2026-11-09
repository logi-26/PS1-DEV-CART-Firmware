"""
Synth: the console's synth (ps1-tools/synth/synth.c, up to its screen drawing) compiled for the PC with its mailbox,
SPU and staging area in host memory, driven as ps1transfer -synth drives it (commands, MIDI ring), against reference
models written here: the SPU RAM of the 16 slots' samples (first fit, no overlap, the free total, "not enough SPU RAM"),
the voices (note on / off, the same note again, the sustain pedal, all notes off, the 24 voices shared: the oldest
taken), the pitch (root note, fine tune, bend); the SysEx samples (gui/sysex.py: 7-bit packing, messages, checksum,
.syx files).

    python -m unittest tests/tools/test_synth.py -v

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import math
import os
import random
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cbuild                                       # noqa: E402
from toolhost import ROOT                           # noqa: E402

import sysex                                        # noqa: E402

START, END = 0x1010, 0x80000 - 0xADE0
SLOTS, VOICES = 16, 24

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
typedef struct {int unused;} DISPENV;
typedef struct {int unused;} DRAWENV;
#define	SPU_TRANSFER_BY_DMA	0
#define	SPU_TRANSFER_WAIT	1
static uint32_t	host_vsync;
static int	VSync(int m) {return (host_vsync);}
static void	SpuSetTransferMode(int m) {}
static void	SpuSetTransferStartAddr(uint32_t a) {}
static void	SpuWrite(const uint32_t *d, uint32_t n) {}
static void	SpuIsTransferCompleted(int m) {}
static volatile uint16_t	host_spu[0x200];
#include "synth.h"
static SynthMailbox			host_mb;
static uint8_t				host_staging[0x80000];
#undef	SYNTH_STAGING_ADRS
#define	SYNTH_STAGING_ADRS	((uintptr_t)host_staging)
@SYNTH@

int	main(void)
{
	char	line[256], cmd[32];
	int		a, b, c, d, v, i;

	DefaultChannels();
	host_mb.spu_total = host_mb.spu_free = SPU_SAMPLES_END - SPU_SAMPLES_START;
	ResetSlots();
	while (fgets(line, sizeof(line), stdin))
	{
		a = b = c = d = 0;
		if (sscanf(line, "%31s %i %i %i %i", cmd, &a, &b, &c, &d) < 1)
			continue;
		if (!strcmp(cmd, "load"))			// load SLOT SIZE RATE LOOP
		{
			memset(host_staging, 0, b + 64);
			if (d)
				host_staging[((b / 16) - 1) * 16 + 1] = 3;
			host_mb.cmd = SYNTH_CMD_LOAD, host_mb.slot = a, host_mb.size = b, host_mb.rate = c;
			snprintf(host_mb.name, SYNTH_NAME_LEN, "s%d", a);
			host_mb.cmd_seq++;
			ProcessCommand();
			printf("R %d\n", host_mb.result);
		}
		else if (!strcmp(cmd, "clear") || !strcmp(cmd, "set") || !strcmp(cmd, "reset"))
		{
			host_mb.cmd = !strcmp(cmd, "clear") ? SYNTH_CMD_CLEAR : (!strcmp(cmd, "set") ? SYNTH_CMD_SET : SYNTH_CMD_RESET);
			host_mb.slot = a, host_mb.param = b, host_mb.value = c;
			host_mb.cmd_seq++;
			ProcessCommand();
			printf("R %d\n", host_mb.result);
		}
		else if (!strcmp(cmd, "midi"))		// midi STATUS D1 D2
		{
			uint8_t	*m = host_mb.midi[host_mb.midi_head & (SYNTH_MIDI_RING - 1)];

			m[0] = a, m[1] = b, m[2] = c;
			host_mb.midi_head++;
			ProcessMidi();
		}
		else if (!strcmp(cmd, "tick"))		// released voices: their envelope ended
		{
			for (v = 0; v < VOICES; v++)
				host_spu[v * 8 + 6] = (voices[v].state == VOICE_RELEASED) ? 0 : 0x4000;
			host_vsync += 2;
			UpdateVoices();
		}
		else if (!strcmp(cmd, "dump"))
		{
			printf("FREE %u\n", host_mb.spu_free);
			for (i = 0; i < SYNTH_SLOTS; i++)
				printf("SLOT %d %u %u %u %d\n", i, host_mb.slots[i].spu_adrs, host_mb.slots[i].size, host_mb.slots[i].rate,
					   host_mb.slots[i].loop);
			for (v = 0; v < VOICES; v++)
				printf("VOICE %d %d %d %d %d %u %u %u %u\n", v, voices[v].state, voices[v].channel, voices[v].note,
					   voices[v].velocity, host_spu[v * 8 + 2], host_spu[v * 8 + 3], host_spu[v * 8], host_spu[v * 8 + 1]);
			printf("END\n");
		}
		fflush(stdout);
	}
	return (0);
}
"""


def harness():
    text = cbuild.source("ps1-tools/synth/synth.c")
    text = text[text.index("#define\tSCREEN_W"):text.index("/****************************************/\n// Screen")]
    for a, b in (("#define\tSPU_VOICE(_v_)\t((volatile uint16_t *)(0x1F801C00 + (_v_) * 16))",
                  "#define\tSPU_VOICE(_v_)\t(&host_spu[(_v_) * 8])"),
                 ("#define\tSPU_REG(_a_)\t(*((volatile uint16_t *)(_a_)))",
                  "#define\tSPU_REG(_a_)\t(host_spu[((_a_) - 0x1F801C00) / 2])"),
                 ("#define\tMB\t\t\t\t((SynthMailbox *)SYNTH_MAILBOX_ADRS)", "#define\tMB\t\t\t\t(&host_mb)")):
        if a not in text:
            raise RuntimeError("synth.c changed: %r (update the test)" % a)
        text = text.replace(a, b)
    return cbuild.build("synth_host", HARNESS.replace("@SYNTH@", text),
                        ["-I", os.path.join(ROOT, "ps1-tools", "synth")])


class Synth:
    """The host synth: lines in, its answers."""

    def __init__(self, exe):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def cmd(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        if line.split()[0] in ("load", "clear", "set", "reset"):
            return int(self.p.stdout.readline().split()[1])
        return None

    def dump(self):
        self.p.stdin.write("dump\n")
        self.p.stdin.flush()
        free, slots, voices = None, [], []
        while True:
            f = self.p.stdout.readline().split()
            if f[0] == "END":
                return free, slots, voices
            if f[0] == "FREE":
                free = int(f[1])
            elif f[0] == "SLOT":
                slots.append(tuple(int(x) for x in f[2:6]))
            elif f[0] == "VOICE":
                voices.append(tuple(int(x) for x in f[2:10]))

    def close(self):
        self.p.stdin.close()
        self.p.wait(10)


def ref_alloc(slots, size):
    """First fit after START, among the other slots' samples (address, size): the address or None."""
    adrs = START
    for a, s in sorted(x for x in slots if x[0]):
        if adrs + size <= a:
            break
        adrs = max(adrs, a + s)
    return adrs if adrs + size <= END else None


class Slots(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.exe = harness()

    def test_allocation(self):
        """1000 random loads and clears against the reference: same addresses, no overlap, the free total."""
        rng = random.Random(1)
        s = Synth(self.exe)
        ref = [(0, 0)] * SLOTS
        try:
            for k in range(1000):
                slot = rng.randrange(SLOTS)
                if rng.random() < 0.65:
                    size = rng.choice((16, 1000, 30000, 64 * 1024, 120000, 200000, 400000))
                    r = s.cmd("load %d %d 22050 %d" % (slot, size, rng.randrange(2)))
                    others = [x if i != slot else (0, 0) for i, x in enumerate(ref)]
                    a = ref_alloc(others, (size + 63) & ~63)
                    if a is None:
                        self.assertEqual(-2, r, "case %d: no room" % k)
                        ref[slot] = (0, 0)
                    else:
                        self.assertEqual(0, r)
                        ref[slot] = (a, (size + 63) & ~63)
                else:
                    self.assertEqual(0, s.cmd("clear %d" % slot))
                    ref[slot] = (0, 0)
                free, slots, _ = s.dump()
                self.assertEqual([x[:2] for x in ref], [x[:2] for x in slots], "case %d" % k)
                used = sorted(x for x in ref if x[0])
                for (a1, s1), (a2, s2) in zip(used, used[1:]):
                    self.assertLessEqual(a1 + s1, a2, "overlap")
                self.assertEqual(END - START - sum(x[1] for x in ref), free)
        finally:
            s.close()

    def test_commands(self):
        s = Synth(self.exe)
        try:
            self.assertEqual(-3, s.cmd("load 0 0 22050 0"), "no data")
            self.assertEqual(-3, s.cmd("load 0 %d 22050 0" % (0x80000 + 1)))
            self.assertEqual(-1, s.cmd("clear 16"))
            self.assertEqual(-3, s.cmd("set 0 9 1"))
            self.assertEqual(0, s.cmd("load 3 3200 0 1"))
            free, slots, _ = s.dump()
            self.assertEqual(44100, slots[3][2], "rate 0: 44100")
            self.assertEqual(1, slots[3][3], "loop flag found")
            self.assertEqual(0, s.cmd("reset"))
            free, slots, _ = s.dump()
            self.assertEqual(END - START, free)
            self.assertTrue(all(x[0] == 0 for x in slots))
        finally:
            s.close()


def ref_pitch(rate, note, root=60, tune=50, bend=0):
    cents = (note - root) * 100 + (tune - 50) + bend * 200 // 8192
    return max(1, min(0x3FFF, rate / 44100 * 4096 * 2 ** (cents / 1200)))


class Voices(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.exe = harness()

    def state(self, s):
        return {v: x for v, x in enumerate(s.dump()[2]) if x[0] != 0}

    def test_notes(self):
        s = Synth(self.exe)
        try:
            s.cmd("load 0 4096 22050 0")
            s.cmd("load 1 4096 44100 0")
            s.cmd("midi 0x90 60 100")
            s.cmd("midi 0x91 64 80")
            v = self.state(s)
            self.assertEqual({(1, 0, 60, 100), (1, 1, 64, 80)}, {x[:4] for x in v.values()})
            s.cmd("midi 0x90 60 90")                               # the same note again: the same voice
            self.assertEqual(2, len(self.state(s)))
            s.cmd("midi 0x90 60 0")                                # velocity 0: off
            released = [x for x in self.state(s).values() if x[1] == 0]
            self.assertEqual(2, released[0][0], "released")
            s.cmd("tick")
            self.assertEqual({(1, 1, 64, 80)}, {x[:4] for x in self.state(s).values()})
            s.cmd("midi 0x92 60 100")                              # an empty slot: nothing
            self.assertEqual(1, len(self.state(s)))
        finally:
            s.close()

    def test_sustain_and_all_off(self):
        s = Synth(self.exe)
        try:
            s.cmd("load 0 4096 22050 0")
            s.cmd("midi 0xB0 64 127")
            for n in (60, 62, 64):
                s.cmd("midi 0x90 %d 100" % n)
                s.cmd("midi 0x80 %d 0" % n)
            self.assertEqual({1}, {x[0] for x in self.state(s).values()}, "held by the pedal")
            s.cmd("midi 0xB0 64 0")
            self.assertEqual({2}, {x[0] for x in self.state(s).values()}, "released with the pedal")
            for n in (60, 62):
                s.cmd("midi 0x90 %d 100" % n)
            s.cmd("midi 0xB0 123 0")
            self.assertEqual({2}, {x[0] for x in self.state(s).values()}, "all notes off")
        finally:
            s.close()

    def test_voice_stealing(self):
        """30 notes held on 24 voices: the 24 most recent ones play (the oldest taken)."""
        s = Synth(self.exe)
        try:
            s.cmd("load 0 4096 22050 0")
            for n in range(30):
                s.cmd("midi 0x90 %d 100" % (30 + n))
            v = self.state(s)
            self.assertEqual(VOICES, len(v))
            self.assertEqual(set(range(36, 60)), {x[2] for x in v.values()})
        finally:
            s.close()

    def test_pitch(self):
        s = Synth(self.exe)
        try:
            s.cmd("load 0 4096 22050 0")
            cases = [(60, 60, 50, 0), (72, 60, 50, 0), (48, 60, 50, 0), (61, 60, 50, 0), (60, 67, 50, 0),
                     (60, 60, 100, 0), (60, 60, 0, 0), (60, 60, 50, 8191), (60, 60, 50, -8192), (100, 60, 50, 0),
                     (10, 60, 50, 0)]
            for note, root, tune, bend in cases:
                with self.subTest(note=note, root=root, tune=tune, bend=bend):
                    s.cmd("set 0 0 %d" % root)
                    s.cmd("set 0 8 %d" % tune)
                    b = bend + 8192
                    s.cmd("midi 0xE0 %d %d" % (b & 127, b >> 7))
                    s.cmd("midi 0x90 %d 100" % note)
                    v = [x for x in self.state(s).values() if x[2] == note][0]
                    want = ref_pitch(22050, note, root, tune, bend)
                    self.assertLess(abs(v[4] - want), want * 0.006 + 1, "pitch %d, expected %.0f" % (v[4], want))
                    s.cmd("midi 0x80 %d 0" % note)
                    s.cmd("tick")
        finally:
            s.close()


class SysEx(unittest.TestCase):

    def test_pack7(self):
        rng = random.Random(2)
        for n in (0, 1, 6, 7, 8, 13, 14, 15, 127, 128, 129, 1000):
            data = bytes(rng.randrange(256) for _ in range(n))
            p = sysex.pack7(data)
            self.assertTrue(all(b < 0x80 for b in p))
            self.assertEqual(data, sysex.unpack7(p))

    def test_messages_and_receiver(self):
        rng = random.Random(3)
        for size in (16, 112, 128, 129, 4096, 50000):
            with self.subTest(size=size):
                adpcm = bytes(rng.randrange(256) for _ in range(size))
                msgs = sysex.sample_messages(5, adpcm, 32000, "piano C4 long name x")
                for m in msgs:
                    self.assertEqual((0xF0, 0xF7), (m[0], m[-1]))
                    self.assertTrue(all(b < 0x80 for b in m[1:-1]), "7-bit inside a message")
                r = sysex.Receiver()
                results = [r.feed(m) for m in msgs]
                self.assertEqual([None] * (len(msgs) - 1), results[:-1])
                self.assertEqual(("load", 5, adpcm, 32000, "piano C4 long na"), results[-1])
                # A data byte changed, and a group's high-bits byte changed (128 moved from a byte to the next: the
                # sum doesn't see it, the CRC does)
                for pos in (6, 5):
                    bad = list(msgs)
                    bad[1] = bad[1][:pos] + bytes(((bad[1][pos] + 1) & 0x7F,)) + bad[1][pos + 1:]
                    if bad[1] == msgs[1]:
                        continue
                    r = sysex.Receiver()
                    self.assertEqual("error", [r.feed(m) for m in bad][-1][0], "byte %d changed" % pos)
                # A file of before (its END without the CRC): read, its sum checked
                old = list(msgs)
                old[-1] = old[-1][:7] + bytes((0xF7,))
                r = sysex.Receiver()
                self.assertEqual("load", [r.feed(m) for m in old][-1][0])
                r = sysex.Receiver()
                self.assertEqual("error", [r.feed(m) for m in msgs[:2] + msgs[3:]][-1][0] if len(msgs) > 3
                                 else "error", "a message lost")
        r = sysex.Receiver()
        self.assertEqual(("set", 3, 1, 100), r.feed(sysex.message(sysex.SET, bytes((3, 1, 100)))))
        self.assertEqual(("clear", 15), r.feed(sysex.message(sysex.CLEAR, bytes((15,)))))
        self.assertIsNone(r.feed(bytes((0xF0, 0x43, 0x10, 0xF7))), "another maker's SysEx")
        self.assertIsNone(r.feed(sysex.message(sysex.DATA, b"\x00" * 8)), "data without a start")

    def test_syx_file(self):
        import audioconv
        import numpy as np
        vag = audioconv.to_vag((np.sin(np.arange(3000) * 0.1) * 8000).astype(np.int16), 22050, name="tone")
        syx = sysex.vag_to_syx(vag, 7)
        msgs = sysex.split_syx(b"\x00junk" + syx + b"\xF0\x01")
        r = sysex.Receiver()
        out = [r.feed(m) for m in msgs][-1]
        self.assertEqual(("load", 7, vag[48:], 22050, "tone"), out)


if __name__ == "__main__":
    unittest.main()
