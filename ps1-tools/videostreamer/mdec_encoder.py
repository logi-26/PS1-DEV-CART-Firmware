"""
PS1 DEV CART video streamer: MDEC frame encoder (PlayStation .BS version 2 bitstreams, the frames of STR videos).

A frame (RGB, width and height multiples of 16) is cut into 16x16 macroblocks in column order (the MDEC outputs 16
pixel wide vertical strips), each one into 6 blocks of 8x8 (Cr, Cb at half resolution, then the 4 Y blocks). Each block
goes through the DCT, the MDEC quantization table and the frame's quantization scale (numpy, all the blocks at once),
then it is written as a BS v2 bitstream: the DC value as 10 bits, the AC coefficients as MPEG-1 run/level codes
(table B.14, "dct_coeff_next"; an escape code with a 6-bit run and a 10-bit level for the pairs not in the short
table), "10" at the end of each block. The bits fill 16-bit little-endian words from the most significant bit.
The PlayStation decodes it with PSn00bSDK's DecDCTvlc() (psxpress) and the MDEC.

The DCT, the quantization and the block order follow the MDEC image example of PSn00bSDK (encode_image.py, (C) 2022
spicyjpeg, MPL licensed); the BS v2 writer and the frame layout are this project's.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""

import math
import struct

import numpy as np

# Zigzag order: position in the 8x8 block of each coefficient in transmission order
ZIGZAG = np.array((
    0, 1, 5, 6, 14, 15, 27, 28,
    2, 4, 7, 13, 16, 26, 29, 42,
    3, 8, 12, 17, 25, 30, 41, 43,
    9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63), np.int32).argsort()

# MDEC default quantization table (MPEG-1's, with 2 for the DC)
QUANT = np.array((
    2, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83), np.float32).reshape(8, 8)

_S = [math.cos((i or 4) / 16 * math.pi) / 2 for i in range(8)]
DCT = np.array((
    _S[0], _S[0], _S[0], _S[0], _S[0], _S[0], _S[0], _S[0],
    _S[1], _S[3], _S[5], _S[7], -_S[7], -_S[5], -_S[3], -_S[1],
    _S[2], _S[6], -_S[6], -_S[2], -_S[2], -_S[6], _S[6], _S[2],
    _S[3], -_S[7], -_S[1], -_S[5], _S[5], _S[1], _S[7], -_S[3],
    _S[4], -_S[4], -_S[4], _S[4], _S[4], -_S[4], -_S[4], _S[4],
    _S[5], -_S[1], _S[7], _S[3], -_S[3], -_S[7], _S[1], -_S[5],
    _S[6], -_S[2], _S[2], -_S[6], -_S[6], _S[2], -_S[2], _S[6],
    _S[7], -_S[5], _S[3], -_S[1], _S[1], -_S[3], _S[5], -_S[7]), np.float32).reshape(8, 8)

# MPEG-1 dct_coeff_next codes (without the sign bit) for the short (run, level) pairs; the others use the escape
VLC = {
    (0, 1): "11", (1, 1): "011", (0, 2): "0100", (2, 1): "0101",
    (0, 3): "00101", (3, 1): "00111", (4, 1): "00110",
    (1, 2): "000110", (5, 1): "000111", (6, 1): "000101", (7, 1): "000100",
    (0, 4): "0000110", (2, 2): "0000100", (8, 1): "0000111", (9, 1): "0000101",
    (0, 5): "00100110", (0, 6): "00100001", (1, 3): "00100101", (3, 2): "00100100",
    (10, 1): "00100111", (11, 1): "00100011", (12, 1): "00100010", (13, 1): "00100000",
    (0, 7): "0000001010", (1, 4): "0000001100", (2, 3): "0000001011", (4, 2): "0000001111",
    (5, 2): "0000001001", (14, 1): "0000001110", (15, 1): "0000001101", (16, 1): "0000001000",
}
ESCAPE = "000001"
EOB = "10"


def _bits(value, n):
    return format(value & ((1 << n) - 1), "0%db" % n)


# (run, level) -> code with its sign bit, for both signs
_CODES = {}
for (_run, _level), _code in VLC.items():
    _CODES[(_run, _level)] = _code + "0"
    _CODES[(_run, -_level)] = _code + "1"


def rgb_to_blocks(rgb):
    """RGB frame (H, W, 3 uint8) -> blocks (N, 8, 8) float32 in MDEC order (column-major macroblocks: Cr, Cb, Y1-4),
    and the number of blocks per frame."""
    f = rgb.astype(np.float32)
    r, g, b = f[..., 0], f[..., 1], f[..., 2]
    # ITU-R BT.601 full range (as Pillow's YCbCr, used by the PSn00bSDK example)
    y = 0.299 * r + 0.587 * g + 0.114 * b
    cb = 128 - 0.168736 * r - 0.331264 * g + 0.5 * b
    cr = 128 + 0.5 * r - 0.418688 * g - 0.081312 * b
    h, w = y.shape
    # Chroma at half resolution (average of 2x2 pixels)
    cb2 = cb.reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))
    cr2 = cr.reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))
    mbw, mbh = w // 16, h // 16
    # (mbh, mbw, 16, 16) -> column-major macroblocks (x outer, y inner)
    ymb = y.reshape(mbh, 16, mbw, 16).transpose(2, 0, 1, 3).reshape(-1, 16, 16)
    cbmb = cb2.reshape(mbh, 8, mbw, 8).transpose(2, 0, 1, 3).reshape(-1, 8, 8)
    crmb = cr2.reshape(mbh, 8, mbw, 8).transpose(2, 0, 1, 3).reshape(-1, 8, 8)
    blocks = np.stack((crmb, cbmb, ymb[:, :8, :8], ymb[:, :8, 8:], ymb[:, 8:, :8], ymb[:, 8:, 8:]), axis=1)
    return blocks.reshape(-1, 8, 8) - 128.0


def quantize(blocks, scale):
    """DCT + quantization: (N, 64) int32 coefficients in zigzag order (DC, then the AC already scaled)."""
    coeffs = np.einsum("ij,njk,lk->nil", DCT, blocks, DCT) / QUANT
    coeffs = coeffs.reshape(-1, 64)[:, ZIGZAG]
    coeffs[:, 1:] *= 8.0 / scale
    q = np.rint(coeffs).astype(np.int32)
    np.clip(q, -512, 511, out=q)
    return q


# Code tables for numpy: [run 0-63, level + 512] -> code (with its sign bit) and its length; escape for the others
CODE_VAL = np.zeros((64, 1024), np.uint32)
CODE_LEN = np.full((64, 1024), 22, np.uint32)
for _run in range(64):
    _lv = np.arange(-512, 512)
    CODE_VAL[_run] = (1 << 16) | (_run << 10) | (_lv & 0x3FF)     # Escape: 000001 + run (6) + level (10)
for (_run, _level), _code in _CODES.items():
    CODE_VAL[_run, _level + 512] = int(_code, 2)
    CODE_LEN[_run, _level + 512] = len(_code)
_BIT = np.arange(32, dtype=np.uint32)


def dct_blocks(rgb):
    """DCT of the blocks divided by the quantization table, in zigzag order: (N, 64) float32, before the scale."""
    coeffs = np.einsum("ij,njk,lk->nil", DCT, rgb_to_blocks(rgb), DCT) / QUANT
    return coeffs.reshape(-1, 64)[:, ZIGZAG]


def quantize_dct(coeffs, scale):
    q = coeffs.copy()
    q[:, 1:] *= 8.0 / scale
    q = np.rint(q).astype(np.int32)
    np.clip(q, -512, 511, out=q)
    return q


def estimate_bytes(q):
    """Size of the BS frame of quantized blocks (without the padding codes), quickly."""
    ac = q[:, 1:]
    b, k = np.nonzero(ac)
    first = np.r_[True, b[1:] != b[:-1]]
    prev = np.where(first, -1, np.r_[0, k[:-1]])
    bits = 12 * q.shape[0] + int(CODE_LEN[k - prev - 1, ac[b, k] + 512].sum())
    return 8 + (bits + 31) // 32 * 4


def encode_quantized(q, scale):
    """BS v2 frame of quantized blocks (numpy). Returns (bytes, number of MDEC run-length codes)."""
    nb = q.shape[0]
    ac = q[:, 1:]
    nz = ac != 0
    # The number of MDEC codes is made a multiple of 64 (whole 32-word chunks, as the MDEC is fed): with an odd count a
    # half word started a block of its own, and with words after the codes in the last chunk the MDEC was still busy
    # after the frame. Zero coefficients are added where a block has only zeros (escape code, level 0: nothing changes
    # in the picture), at its first positions: they are simply coded as coefficients.
    total = 2 * nb + int(nz.sum())
    need = -total % 64
    room = np.where(nz.any(axis=1), nz.argmax(axis=1), 63)     # Zeros after the DC
    before = np.cumsum(room) - room
    pads = np.minimum(room, np.maximum(0, need - before))
    coded = nz | (np.arange(63) < pads[:, None])
    b, k = np.nonzero(coded)
    first = np.r_[True, b[1:] != b[:-1]] if len(b) else np.zeros(0, bool)
    prev = np.where(first, -1, np.r_[0, k[:-1]]) if len(b) else k
    acv = CODE_VAL[k - prev - 1, ac[b, k] + 512]
    acl = CODE_LEN[k - prev - 1, ac[b, k] + 512]
    # Stream order: DC, the coefficients, end of block, block after block
    per_block = np.bincount(b, minlength=nb) + 2
    start = np.cumsum(per_block) - per_block
    codes = int(per_block.sum())
    val = np.empty(codes, np.uint32)
    ln = np.empty(codes, np.uint32)
    val[start] = q[:, 0] & 0x3FF
    ln[start] = 10
    end = start + per_block - 1
    val[end] = 0b10
    ln[end] = 2
    rank = np.arange(len(b)) - (np.cumsum(per_block - 2) - (per_block - 2))[b]
    pos = start[b] + 1 + rank
    val[pos] = acv
    ln[pos] = acl
    # Bits, most significant first, then 16-bit words little-endian
    left = val << (32 - ln)
    bits = ((left[:, None] >> (31 - _BIT)) & 1).astype(np.uint8)[_BIT[None, :] < ln[:, None]]
    bits = np.concatenate((bits, np.zeros(-len(bits) % 32, np.uint8)))
    data = np.packbits(bits).reshape(-1, 2)[:, ::-1].tobytes()
    header = struct.pack("<IHH", 0x38000000 | ((codes + 1) // 2), scale, 2)
    return header + data, codes


def encode_frame(rgb, scale):
    """BS v2 frame for an RGB frame and a quantization scale (1-63). Returns (bytes, number of MDEC run-length codes)."""
    return encode_quantized(quantize_dct(dct_blocks(rgb), scale), scale)


def encode_frame_fit(rgb, max_bytes, scale=8, max_scale=63):
    """The frame at the finest scale (from "scale") whose BS data fits in max_bytes. Returns (bytes, scale).
    The DCT is done once; the sizes at the other scales are estimated from the code lengths."""
    coeffs = dct_blocks(rgb)
    q_scale = scale
    while q_scale < max_scale and estimate_bytes(quantize_dct(coeffs, q_scale)) > max_bytes:
        q_scale = min(max_scale, q_scale + max(1, q_scale // 4))
    while True:
        data, _ = encode_quantized(quantize_dct(coeffs, q_scale), q_scale)
        if len(data) <= max_bytes or q_scale >= max_scale:
            return data, q_scale
        q_scale += 1


def encode_frame_reference(rgb, scale):
    """The same BS v2 frame, coded block after block in Python (slow, kept to check encode_frame)."""
    q = quantize(rgb_to_blocks(rgb), scale)
    blocks = q.tolist()
    # The number of MDEC codes is made a multiple of 64 (whole 32-word chunks, as the MDEC is fed): with an odd count a
    # half word started a block of its own, and with words after the codes in the last chunk the MDEC was still busy
    # after the frame. Zero coefficients are added where a block has only zeros (escape code, level 0: nothing changes
    # in the picture), at its first positions.
    total = sum(2 + sum(1 for v in b[1:] if v) for b in blocks)
    need = -total % 64
    pads = [0] * len(blocks)
    for i, b in enumerate(blocks):
        if not need:
            break
        room = next((k for k in range(1, 64) if b[k]), 64) - 1     # Zeros after the DC
        pads[i] = min(room, need)
        need -= pads[i]
    out = []
    codes = 0
    for i, block in enumerate(blocks):
        out.append(_bits(block[0], 10))     # DC
        codes += 2                          # DC + end of block
        run = 0
        if pads[i]:                         # (the zeros at positions 1..pads: the next run is that much shorter)
            out.append((ESCAPE + _bits(0, 6) + _bits(0, 10)) * pads[i])
            codes += pads[i]
            run = -pads[i]
        for v in block[1:]:
            if v == 0:
                run += 1
                continue
            code = _CODES.get((run, v))
            out.append(code if code else ESCAPE + _bits(run, 6) + _bits(v, 10))
            codes += 1
            run = 0
        out.append(EOB)
    bits = "".join(out)
    bits += "0" * (-len(bits) % 32)             # Whole 32-bit words
    data = int(bits, 2).to_bytes(len(bits) // 8, "big") if bits else b""
    # 16-bit words, little-endian: swap the bytes of each pair
    data = bytes(b for i in range(0, len(data), 2) for b in (data[i + 1], data[i]))
    # MDEC run-length codes, in 32-bit words: exactly (DecDCTvlc decodes that many, a bigger count made it read past the
    # end of the frame and fail)
    words = (codes + 1) // 2
    header = struct.pack("<IHH", 0x38000000 | words, scale, 2)
    return header + data, codes


def decode_frame(bs, nblocks):
    """Reference decoder (tests): BS v2 frame of nblocks blocks -> list of (DC, [(run, level), ...]), scale."""
    header, scale, version = struct.unpack_from("<IHH", bs)
    assert version == 2 and (header >> 16) == 0x3800
    data = bs[8:]
    data = bytes(b for i in range(0, len(data), 2) for b in (data[i + 1], data[i]))
    bits = "".join(format(b, "08b") for b in data)
    decode = {code: rl for rl, code in VLC.items()}
    pos, blocks = 0, []
    while len(blocks) < nblocks:
        dc = int(bits[pos:pos + 10], 2)
        dc = dc - 1024 if dc >= 512 else dc
        pos += 10
        acs = []
        while True:
            if bits.startswith(EOB, pos):
                pos += 2
                break
            if bits.startswith(ESCAPE, pos):
                run = int(bits[pos + 6:pos + 12], 2)
                lvl = int(bits[pos + 12:pos + 22], 2)
                acs.append((run, lvl - 1024 if lvl >= 512 else lvl))
                pos += 22
                continue
            for n in range(2, 11):
                rl = decode.get(bits[pos:pos + n])
                if rl:
                    sign = bits[pos + n] == "1"
                    acs.append((rl[0], -rl[1] if sign else rl[1]))
                    pos += n + 1
                    break
            else:
                raise ValueError("bad code at bit %d" % pos)
        blocks.append((dc, acs))
    return blocks, scale


if __name__ == "__main__":
    # Self test: a gradient with some detail, encoded and decoded again
    h, w = 128, 160
    yy, xx = np.mgrid[0:h, 0:w]
    img = np.stack(((xx * 255 // w), (yy * 255 // h), ((xx + yy) % 64) * 4), axis=-1).astype(np.uint8)
    for scale in (4, 8, 16, 32):
        bs, codes = encode_frame(img, scale)
        q = quantize(rgb_to_blocks(img), scale)
        blocks, s = decode_frame(bs, q.shape[0])
        ok = len(blocks) >= q.shape[0]
        for i in range(q.shape[0]):
            dc, acs = blocks[i]
            row = [dc] + [0] * 63
            p = 1
            for run, lvl in acs:
                p += run
                row[p] = lvl
                p += 1
            if row != q[i].tolist():
                ok = False
                print("block %d differs" % i)
                break
        print("scale %2d: %5d bytes, %5d codes, decoded back: %s" % (scale, len(bs), codes, "OK" if ok else "FAILED"))
