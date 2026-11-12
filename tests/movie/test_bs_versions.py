"""
BS frame versions read by the movie tools (gui/strmovie.py): v1, v2, v3 and IKI frames written here from the same
quantized blocks decode to the same picture; GT-ZIP (the DC values of IKI frames).

Version 3's DC codes, IKI's header and GT-ZIP are as psx-spx describes them ("CDROM File Video BS Compression").
"""

import os
import struct
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "gui"))
import strmovie                                             # noqa: E402

DC_Y = {v: k for k, v in strmovie._DC_Y.items()}
DC_C = {v: k for k, v in strmovie._DC_C.items()}


def picture(w, h):
    yy, xx = np.mgrid[0:h, 0:w]
    return np.stack((xx * 255 // w, yy * 255 // h, ((xx * 7 + yy * 3) % 64) * 4), axis=-1).astype(np.uint8)


def pack(bits):
    """A bit string as BS data: 16-bit little-endian words, the first bit in bit 15."""
    bits += "0" * (-len(bits) % 16)
    words = [int(bits[i:i + 16], 2) for i in range(0, len(bits), 16)]
    return struct.pack("<%dH" % len(words), *words)


def ac_bits(block):
    """A block's AC coefficients as run/level codes, then the end of block."""
    out, run = [], 0
    for lvl in block[1:]:
        lvl = int(lvl)
        if not lvl:
            run += 1
            continue
        code = strmovie.VLC.get((run, abs(lvl)))
        if code:
            out.append(code + ("1" if lvl < 0 else "0"))
        else:
            out.append(strmovie.ESCAPE + format(run, "06b") + format(lvl & 0x3FF, "010b"))
        run = 0
    return "".join(out) + strmovie.EOB


def dc_v3(diff, table):
    """A v3 DC difference (a multiple of 4)."""
    d = diff // 4
    size = abs(d).bit_length()
    code = table[size]
    if not size:
        return code
    return code + format(d if d > 0 else d + (1 << size) - 1, "0%db" % size)


def frame_v3(q, scale, wrap=True):
    """wrap: the shorter way round when a difference is over 508 (newer encoders: the DC wraps in 10 bits), else the
    difference as it is (older ones: up to 11 bits)."""
    last, bits = [0, 0, 0], []
    for blk, block in enumerate(q):
        kind = min(blk % 6, 2)
        diff = int(block[0]) - last[kind]
        if wrap and diff > 508:
            diff -= 1024
        elif wrap and diff < -508:
            diff += 1024
        last[kind] = int(block[0])
        bits.append(dc_v3(diff, DC_C if kind < 2 else DC_Y) + ac_bits(block))
    data = pack("".join(bits) + strmovie.BS_END)
    return struct.pack("<HHHH", 0, 0x3800, scale, 3) + data


def gtzip_literal(data):
    """GT-ZIP with no copies: a zero flag byte before each 8 bytes."""
    out = bytearray()
    for i in range(0, len(data), 8):
        out += b"\0" + data[i:i + 8]
    return bytes(out)


def frame_iki(q, scales, w, h):
    nb = q.shape[0]
    dcq = (np.asarray(scales, np.int32) << 10) | (q[:, 0] & 0x3FF)
    z = gtzip_literal(bytes((dcq >> 8).astype(np.uint8)) + bytes((dcq & 0xFF).astype(np.uint8)))
    z += bytes(len(z) & 1)
    data = pack("".join(ac_bits(b) for b in q))
    assert nb == (w + 15) // 16 * ((h + 15) // 16) * 6
    return struct.pack("<HHHHH", 0, 0x3800, w, h, len(z)) + z + data


class Versions(unittest.TestCase):

    W, H = 64, 48

    def setUp(self):
        _, self.scale, q = strmovie.encode_frame(picture(self.W, self.H), scale=4)
        q[:, 0] = q[:, 0] // 4 * 4                  # (v3's DC differences are in steps of 4)
        self.q = q
        self.ref = strmovie.dequantize(q, self.scale, self.W, self.H)

    def test_v2_and_v1(self):
        bs, _ = strmovie.encode_quantized(self.q, self.scale)
        self.assertTrue(np.array_equal(self.ref, strmovie.decode_bs(bs, self.W, self.H)))
        v1 = bs[:6] + struct.pack("<H", 1) + bs[8:]
        self.assertEqual((1,), strmovie.bs_header(v1)[2:])
        self.assertTrue(np.array_equal(self.ref, strmovie.decode_bs(v1, self.W, self.H)))

    def test_v3(self):
        bs = frame_v3(self.q, self.scale)
        self.assertEqual(3, strmovie.bs_header(bs)[2])
        self.assertTrue(np.array_equal(self.ref, strmovie.decode_bs(bs, self.W, self.H)))

    def test_v3_dc_wraps(self):
        # Y DCs from -512 to +508: a difference of -4 (a newer encoder's 10-bit wrap) or +1020 (an older one's)
        q = self.q.copy()
        q[:, 0] = 0
        q[2, 0], q[3, 0] = -512, 508
        ref = strmovie.dequantize(q, self.scale, self.W, self.H)
        for wrap in (True, False):
            with self.subTest(wrap=wrap):
                bs = frame_v3(q, self.scale, wrap)
                self.assertTrue(np.array_equal(ref, strmovie.decode_bs(bs, self.W, self.H)))

    def test_iki(self):
        # Each block its own scale; the frame's width and height in the header
        scales = (np.arange(self.q.shape[0]) % 5) + 2
        bs = frame_iki(self.q, scales, self.W, self.H)
        self.assertEqual("iki", strmovie.bs_header(bs)[2])
        ref = strmovie.dequantize(self.q, scales, self.W, self.H)
        self.assertTrue(np.array_equal(ref, strmovie.decode_bs(bs, self.W, self.H)))
        self.assertFalse(np.array_equal(self.ref, ref))

    def test_other_versions(self):
        bs, _ = strmovie.encode_quantized(self.q, self.scale)
        with self.assertRaises(ValueError):
            strmovie.decode_bs(bs[:6] + struct.pack("<H", 0) + bs[8:], self.W, self.H)


class GtZip(unittest.TestCase):

    def test_copies(self):
        # "a", "b", a copy of 5 bytes from 2 back (overlapping itself), "c"
        z = bytes([0b0100, ord("a"), ord("b"), 2, 1, ord("c")])
        self.assertEqual(b"abababac", strmovie.gtzip(z, 8))
        self.assertEqual(b"ababa", strmovie.gtzip(z, 5))           # (stops at the size asked)
        # A long distance: 300 bytes as they are, then a copy of 4 from 300 back (distance 12Bh: two bytes)
        lits = bytes((i * 7) & 0xFF for i in range(300))
        z = b"".join(b"\0" + lits[i:i + 8] for i in range(0, 296, 8)) + bytes([0b00010000]) + lits[296:300]
        z += bytes([1, 0x80 | (0x12B >> 8), 0x12B & 0xFF])
        self.assertEqual(lits + lits[:4], strmovie.gtzip(z, 304))

    def test_bad_data(self):
        with self.assertRaises(ValueError):
            strmovie.gtzip(b"\x01\x00\x05", 10)            # (a copy from before the start)
        with self.assertRaises(ValueError):
            strmovie.gtzip(b"\x00ab", 10)                  # (ends early)


if __name__ == "__main__":
    unittest.main()
