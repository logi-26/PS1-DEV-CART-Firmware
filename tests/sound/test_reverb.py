"""
Sound tester reverb: the registers of Chaos echo and Delay with a delay time and feedback, against libspu's
SpuSetReverbModeParam (its table, as psx-spx lists it, and its sums for SPU_REV_DELAYTIME and SPU_REV_FEEDBACK); the
voices sending to the reverb with "SEQ channels chosen".

    python -m unittest tests/sound/test_reverb.py

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import random
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_host                                       # noqa: E402

# psx-spx "SPU Reverb Examples": Chaos echo (libspu's Echo) and Delay, dAPF1 .. vRIN
ECHO_TABLE = [0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100, 0x0000, 0x0000, 0x1FFF, 0x0FFF, 0x1005,
              0x0005, 0x0000, 0x0000, 0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
              0x1004, 0x1002, 0x0004, 0x0002, 0x8000, 0x8000]
DELAY_TABLE = ECHO_TABLE[:7] + [0x0000] + ECHO_TABLE[8:]
NAMES = ("dAPF1 dAPF2 vIIR vCOMB1 vCOMB2 vCOMB3 vCOMB4 vWALL vAPF1 vAPF2 mLSAME mRSAME mLCOMB1 mRCOMB1 mLCOMB2 mRCOMB2 "
         "dLSAME dRSAME mLDIFF mRDIFF mLCOMB3 mRCOMB3 mLCOMB4 mRCOMB4 dLDIFF dRDIFF mLAPF1 mRAPF1 mLAPF2 mRAPF2 vLIN "
         "vRIN").split()
CHAOS_ECHO, DELAY = 8, 9


def libspu(table, delay, feedback):
    """SpuSetReverbModeParam with a mode and its delay time and feedback (16-bit registers, as libspu writes them)."""
    r = dict(zip(NAMES, table))
    d13, d12 = (delay << 13) // 127, (delay << 12) // 127
    r["mLSAME"] = d13 - r["dAPF1"]
    r["mRSAME"] = d12 - r["dAPF2"]
    r["mLCOMB1"] = d12 + r["mRCOMB1"]
    r["dLSAME"] = d12 + r["dRSAME"]
    r["mLAPF1"] = d12 + r["mLAPF2"]
    r["mRAPF1"] = d12 + r["mRAPF2"]
    r["vWALL"] = feedback * 0x8100 // 127
    return [r[n] & 0xFFFF for n in NAMES]


def host(*args):
    out = subprocess.run([build_host.build(), "--echo"] + [str(a) for a in args], capture_output=True, text=True,
                         timeout=60).stdout
    lines = dict(line.split(" ", 1) for line in out.splitlines() if " " in line)
    return [int(v, 16) for v in lines["REGS"].split()], int(lines["EON"], 16)


class ReverbTests(unittest.TestCase):
    def test_defaults_are_the_table(self):
        """No delay or feedback chosen: libspu's table (its defaults: delay 127, feedback 127 or 0)."""
        self.assertEqual(ECHO_TABLE, host(CHAOS_ECHO, -1, -1, 0xFFFF)[0])
        self.assertEqual(DELAY_TABLE, host(DELAY, -1, -1, 0xFFFF)[0])
        self.assertEqual(ECHO_TABLE, libspu(ECHO_TABLE, 127, 127))
        self.assertEqual(DELAY_TABLE, libspu(DELAY_TABLE, 127, 0))

    def test_delay_and_feedback(self):
        values = [0, 1, 2, 63, 64, 100, 126, 127] + random.Random(7).sample(range(128), 10)
        for delay in values:
            for feedback in values:
                for t, table in ((CHAOS_ECHO, ECHO_TABLE), (DELAY, DELAY_TABLE)):
                    with self.subTest(type=t, delay=delay, feedback=feedback):
                        got = host(t, delay, feedback, 0xFFFF)[0]
                        want = libspu(table, delay, feedback)
                        diff = [(n, hex(g), hex(w)) for n, g, w in zip(NAMES, got, want) if g != w]
                        self.assertEqual([], diff)

    def test_other_types_untouched(self):
        for t in range(1, 8):
            regs = host(t, 10, 10, 0xFFFF)[0]
            self.assertNotEqual(10 * 0x8100 // 127, regs[NAMES.index("vWALL")], "type %d" % t)

    def test_channels_chosen(self):
        for mask in (0xFFFF, 0, 0x0001, 0x8000, 0x1234, 0xFFFE):
            eon = host(CHAOS_ECHO, -1, -1, mask)[1]
            self.assertEqual(mask, eon & 0xFFFF, hex(mask))
            self.assertEqual(1, eon >> 16 & 1, "keyboard note, tone with reverb")
            self.assertEqual(0, eon >> 17 & 1, "keyboard note, tone without reverb")


if __name__ == "__main__":
    unittest.main()
