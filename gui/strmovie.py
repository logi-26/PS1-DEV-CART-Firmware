"""
PS1 DEV CART GUI: PlayStation movies (STR): MDEC video frames, XA ADPCM sound, the sectors of an STR file (Movie Tools).

- Video: each frame is a BS version 2 bitstream (what the MDEC decodes: the frames of STR movies). A frame (RGB, width
  and height multiples of 16) is cut into 16x16 macroblocks in column order (the MDEC outputs 16 pixel wide strips),
  each one into 6 blocks of 8x8 (Cr, Cb at half resolution, then the 4 Y blocks), through the DCT, the MDEC quantization
  table and the frame's quantization scale, then written as MPEG-1 run/level codes (table B.14; an escape code with a
  6-bit run and a 10-bit level for the others), "10" at the end of each block, in 16-bit little-endian words.
  The DCT, the quantization and the block order follow the MDEC image example of PSn00bSDK (encode_image.py, (C) 2022
  spicyjpeg, MPL licensed); the BS v2 writer is this project's (it was the video streamer's).
- Sound: XA ADPCM, 4 bits (37800 or 18900 Hz, mono or stereo): sound groups of 128 bytes (8 sound units of 28 samples,
  each with its filter and shift), 18 groups per sector. The CD-ROM controller plays it; the PS1 DEV CART VIDEO TESTER
  turns the units into SPU ADPCM blocks (the same filters and shifts).
- STR file: CD sectors of 2336 bytes (subheader + user data + EDC/ECC: what disc image tools such as mkpsxiso take for
  an "str" file), video sectors (Mode 2 Form 1: a 32-byte STR header + 2016 bytes of the frame) and XA audio sectors
  (Form 2) interleaved as the drive reads them: at 2x speed (150 sectors a second), 37800 Hz stereo sound is one
  sector in 8. Or 2048-byte sectors (video only, a plain data file). Read back: 2336, 2352 (raw) or 2048-byte sectors.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""

import math
import struct

import numpy as np

# ---------------------------------------------------------------------------------------------------- MDEC (BS v2)

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

# MPEG-1 dct_coeff_next codes (table B.14, without the sign bit). The encoder writes the short ones (up to 10 bits, as
# the video streamer did, tested on consoles) and the escape code for the others; the decoder (previews, other STR
# movies) knows them all.
_B14 = """
11 0 1|011 1 1|0100 0 2|0101 2 1|00101 0 3|00111 3 1|00110 4 1|000110 1 2|000111 5 1|000101 6 1|000100 7 1
0000110 0 4|0000100 2 2|0000111 8 1|0000101 9 1|00100110 0 5|00100001 0 6|00100101 1 3|00100100 3 2|00100111 10 1
00100011 11 1|00100010 12 1|00100000 13 1|0000001010 0 7|0000001100 1 4|0000001011 2 3|0000001111 4 2
0000001001 5 2|0000001110 14 1|0000001101 15 1|0000001000 16 1
000000011101 0 8|000000011000 0 9|000000010011 0 10|000000010000 0 11|000000011011 1 5|000000010100 2 4
000000011100 3 3|000000010010 4 3|000000011110 6 2|000000010101 7 2|000000010001 8 2|000000011111 17 1
000000011010 18 1|000000011001 19 1|000000010111 20 1|000000010110 21 1
0000000011010 0 12|0000000011001 0 13|0000000011000 0 14|0000000010111 0 15|0000000010110 1 6|0000000010101 1 7
0000000010100 2 5|0000000010011 3 4|0000000010010 5 3|0000000010001 9 2|0000000010000 10 2|0000000011111 22 1
0000000011110 23 1|0000000011101 24 1|0000000011100 25 1|0000000011011 26 1
00000000011111 0 16|00000000011110 0 17|00000000011101 0 18|00000000011100 0 19|00000000011011 0 20
00000000011010 0 21|00000000011001 0 22|00000000011000 0 23|00000000010111 0 24|00000000010110 0 25
00000000010101 0 26|00000000010100 0 27|00000000010011 0 28|00000000010010 0 29|00000000010001 0 30
00000000010000 0 31
000000000011000 0 32|000000000010111 0 33|000000000010110 0 34|000000000010101 0 35|000000000010100 0 36
000000000010011 0 37|000000000010010 0 38|000000000010001 0 39|000000000010000 0 40|000000000011111 1 8
000000000011110 1 9|000000000011101 1 10|000000000011100 1 11|000000000011011 1 12|000000000011010 1 13
000000000011001 1 14
0000000000010011 1 15|0000000000010010 1 16|0000000000010001 1 17|0000000000010000 1 18|0000000000010100 6 3
0000000000011010 11 2|0000000000011001 12 2|0000000000011000 13 2|0000000000010111 14 2|0000000000010110 15 2
0000000000010101 16 2|0000000000011111 27 1|0000000000011110 28 1|0000000000011101 29 1|0000000000011100 30 1
0000000000011011 31 1
"""
_DECODE = {}
for _e in _B14.replace("\n", "|").split("|"):
    if _e.strip():
        _c, _r, _l = _e.split()
        _DECODE[_c] = (int(_r), int(_l))
VLC = {rl: c for c, rl in _DECODE.items() if len(c) <= 10}
ESCAPE = "000001"
EOB = "10"
BS_VERSION = 2

# (run, level) -> code with its sign bit, for both signs
_CODES = {}
for (_run, _level), _code in VLC.items():
    _CODES[(_run, _level)] = _code + "0"
    _CODES[(_run, -_level)] = _code + "1"

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


def rgb_to_blocks(rgb):
    """RGB frame (H, W, 3 uint8) -> blocks (N, 8, 8) float32 in MDEC order (column-major macroblocks: Cr, Cb, Y1-4)."""
    f = rgb.astype(np.float32)
    r, g, b = f[..., 0], f[..., 1], f[..., 2]
    # ITU-R BT.601 full range (as Pillow's YCbCr, used by the PSn00bSDK example)
    y = 0.299 * r + 0.587 * g + 0.114 * b
    cb = 128 - 0.168736 * r - 0.331264 * g + 0.5 * b
    cr = 128 + 0.5 * r - 0.418688 * g - 0.081312 * b
    h, w = y.shape
    cb2 = cb.reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))
    cr2 = cr.reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))
    mbw, mbh = w // 16, h // 16
    ymb = y.reshape(mbh, 16, mbw, 16).transpose(2, 0, 1, 3).reshape(-1, 16, 16)
    cbmb = cb2.reshape(mbh, 8, mbw, 8).transpose(2, 0, 1, 3).reshape(-1, 8, 8)
    crmb = cr2.reshape(mbh, 8, mbw, 8).transpose(2, 0, 1, 3).reshape(-1, 8, 8)
    blocks = np.stack((crmb, cbmb, ymb[:, :8, :8], ymb[:, :8, 8:], ymb[:, 8:, :8], ymb[:, 8:, 8:]), axis=1)
    return blocks.reshape(-1, 8, 8) - 128.0


def blocks_to_rgb(blocks, w, h):
    """The inverse of rgb_to_blocks (blocks after the inverse DCT): RGB frame (h, w, 3 uint8)."""
    mbw, mbh = w // 16, h // 16
    b = (blocks + 128.0).reshape(mbw * mbh, 6, 8, 8)
    cr = b[:, 0].reshape(mbw, mbh, 8, 8).transpose(1, 2, 0, 3).reshape(h // 2, w // 2)
    cb = b[:, 1].reshape(mbw, mbh, 8, 8).transpose(1, 2, 0, 3).reshape(h // 2, w // 2)
    ymb = np.empty((mbw * mbh, 16, 16), np.float32)
    ymb[:, :8, :8], ymb[:, :8, 8:], ymb[:, 8:, :8], ymb[:, 8:, 8:] = b[:, 2], b[:, 3], b[:, 4], b[:, 5]
    y = ymb.reshape(mbw, mbh, 16, 16).transpose(1, 2, 0, 3).reshape(h, w)
    cb = cb.repeat(2, axis=0).repeat(2, axis=1) - 128.0
    cr = cr.repeat(2, axis=0).repeat(2, axis=1) - 128.0
    rgb = np.stack((y + 1.402 * cr, y - 0.344136 * cb - 0.714136 * cr, y + 1.772 * cb), axis=-1)
    return np.clip(np.rint(rgb), 0, 255).astype(np.uint8)


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


def dequantize(q, scale, w, h):
    """Quantized coefficients (zigzag order) -> RGB frame, as the MDEC decodes them (preview)."""
    c = q.astype(np.float32).copy()
    c[:, 1:] *= scale / 8.0
    natural = np.empty_like(c)
    natural[:, ZIGZAG] = c
    natural = natural.reshape(-1, 8, 8) * QUANT
    blocks = np.einsum("ji,njk,kl->nil", DCT, natural, DCT)
    return blocks_to_rgb(blocks, w, h)


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
    # The number of MDEC codes is made a multiple of 64 (whole 32-word chunks, as the MDEC is fed): zero coefficients
    # (escape code, level 0: nothing changes in the picture) are added where a block has only zeros after its DC
    total = 2 * nb + int(nz.sum())
    need = -total % 64
    room = np.where(nz.any(axis=1), nz.argmax(axis=1), 63)
    before = np.cumsum(room) - room
    pads = np.minimum(room, np.maximum(0, need - before))
    coded = nz | (np.arange(63) < pads[:, None])
    b, k = np.nonzero(coded)
    first = np.r_[True, b[1:] != b[:-1]] if len(b) else np.zeros(0, bool)
    prev = np.where(first, -1, np.r_[0, k[:-1]]) if len(b) else k
    acv = CODE_VAL[k - prev - 1, ac[b, k] + 512]
    acl = CODE_LEN[k - prev - 1, ac[b, k] + 512]
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
    left = val << (32 - ln)
    bits = ((left[:, None] >> (31 - _BIT)) & 1).astype(np.uint8)[_BIT[None, :] < ln[:, None]]
    bits = np.concatenate((bits, np.zeros(-len(bits) % 32, np.uint8)))
    data = np.packbits(bits).reshape(-1, 2)[:, ::-1].tobytes()
    header = struct.pack("<IHH", 0x38000000 | ((codes + 1) // 2), scale, BS_VERSION)
    return header + data, codes


def encode_frame(rgb, max_bytes=None, scale=1, max_scale=63):
    """BS v2 frame of an RGB frame: at the given scale, or (max_bytes) at the finest scale from it whose data fits.
    Returns (bytes, scale, quantized blocks). The DCT is done once; the sizes at the scales tried are estimated."""
    coeffs = dct_blocks(rgb)
    q_scale = max(1, min(63, scale))
    if max_bytes:
        lo, hi = q_scale, max_scale     # The finest scale that fits (estimates: binary search)
        if estimate_bytes(quantize_dct(coeffs, lo)) > max_bytes:
            while lo < hi:
                mid = (lo + hi) // 2
                if estimate_bytes(quantize_dct(coeffs, mid)) > max_bytes:
                    lo = mid + 1
                else:
                    hi = mid
            q_scale = lo
    while True:
        q = quantize_dct(coeffs, q_scale)
        data, _ = encode_quantized(q, q_scale)
        if not max_bytes or len(data) <= max_bytes or q_scale >= max_scale:
            return data, q_scale, q
        q_scale += 1


# BS v2 decoding (preview and checks of STR files): the code lengths of table B.14
_DECODE_LENS = sorted({len(c) for c in _DECODE})


def bs_header(bs):
    """(MDEC words, quantization scale, version) of a BS frame, or None if it isn't one."""
    if len(bs) < 8:
        return None
    words, magic, scale, version = struct.unpack_from("<HHHH", bs)
    if magic != 0x3800:
        return None
    return words, scale, version


def decode_bs(bs, w, h):
    """BS v2 frame -> RGB frame (preview). ValueError if it can't be decoded (other versions, bad data)."""
    head = bs_header(bs)
    if not head:
        raise ValueError("not a BS frame")
    _, scale, version = head
    if version != 2:
        raise ValueError("BS version %d (only version 2 is decoded here)" % version)
    data = bs[8:]
    if len(data) & 1:
        data += b"\0"
    swapped = bytes(np.frombuffer(data, np.uint8).reshape(-1, 2)[:, ::-1].tobytes())
    bits = bin(int.from_bytes(b"\x01" + swapped, "big"))[3:]
    pw, ph = -(-w // 16) * 16, -(-h // 16) * 16     # (whole macroblocks: a 640x200 frame is coded 640x208)
    nblocks = (pw // 16) * (ph // 16) * 6
    q = np.zeros((nblocks, 64), np.int32)
    pos = 0
    n = len(bits)
    for blk in range(nblocks):
        if pos + 10 > n:
            raise ValueError("frame data ends at block %d of %d" % (blk, nblocks))
        dc = int(bits[pos:pos + 10], 2)
        q[blk, 0] = dc - 1024 if dc >= 512 else dc
        pos += 10
        k = 1
        while True:
            if bits.startswith(EOB, pos):
                pos += 2
                break
            if bits.startswith(ESCAPE, pos):
                run = int(bits[pos + 6:pos + 12], 2)
                lvl = int(bits[pos + 12:pos + 22], 2)
                lvl = lvl - 1024 if lvl >= 512 else lvl
                pos += 22
            else:
                for ln in _DECODE_LENS:
                    rl = _DECODE.get(bits[pos:pos + ln])
                    if rl:
                        run, lvl = rl
                        if bits[pos + ln] == "1":
                            lvl = -lvl
                        pos += ln + 1
                        break
                else:
                    raise ValueError("bad code at bit %d (block %d)" % (pos, blk))
            k += run
            if k > 63:
                raise ValueError("block %d has more than 64 coefficients" % blk)
            q[blk, k] = lvl
            k += 1
    return dequantize(q, scale, pw, ph)[:h, :w]


# ---------------------------------------------------------------------------------------------------- XA ADPCM

XA_RATES = (37800, 18900)
XA_K0 = (0, 60, 115, 98)
XA_K1 = (0, 0, -52, -55)
XA_GROUPS = 18                  # Sound groups per sector
XA_GROUP = 128
XA_DATA = 2304                  # 18 groups (2324 bytes of a Form 2 sector: then 20 zero bytes)


def xa_samples_per_sector(stereo):
    """Samples per channel in an XA sector (4 bits): 18 groups of 8 units of 28 samples, shared by the channels."""
    return XA_GROUPS * 8 * 28 // (2 if stereo else 1)


def xa_interleave(rate, stereo, speed):
    """One XA sector every N sectors (the drive reads 75 sectors a second at 1x): N."""
    per_second = rate / xa_samples_per_sector(stereo)
    return int(round(75 * speed / per_second))


def _xa_params(x):
    """Filter and shift of each 28-sample unit of a channel (int32, a multiple of 28 samples), all at once: the filter
    with the smallest prediction error on the original samples, the shift that keeps the error in 4 bits."""
    p1 = np.r_[0, x[:-1]]
    p2 = np.r_[0, 0, x[:-2]]
    peaks = np.stack([np.abs(x - ((p1 * XA_K0[f] + p2 * XA_K1[f] + 32) >> 6)).reshape(-1, 28).max(axis=1)
                      for f in range(4)])
    filters = peaks.argmin(axis=0)
    peak = peaks[filters, np.arange(peaks.shape[1])]
    shifts = np.full(peak.shape, 12)
    for sh in range(12, 0, -1):     # (the largest shift whose step keeps the peak in -8..7)
        shifts = np.where((shifts == sh) & ((peak >> (12 - sh)) > 7), sh - 1, shifts)
    return filters, shifts


def _xa_channel(x):
    """A channel (int32, a multiple of 28 samples) -> (parameter bytes, nibbles (units, 28)): coded sample after
    sample from the samples decoded (the prediction the decoder makes)."""
    filters, shifts = _xa_params(x)
    nunits = len(filters)
    nib = np.zeros((nunits, 28), np.uint8)
    xs = x.tolist()
    s1 = s2 = 0
    for u in range(nunits):
        f, shift = int(filters[u]), int(shifts[u])
        k0, k1 = XA_K0[f], XA_K1[f]
        step = 1 << (12 - shift)
        half = step >> 1
        row = nib[u]
        base = u * 28
        for j in range(28):
            pred = (s1 * k0 + s2 * k1 + 32) >> 6
            d = xs[base + j] - pred
            n = (d + half) // step if d >= 0 else -((-d + half) // step)
            n = -8 if n < -8 else (7 if n > 7 else n)
            v = ((n << 12) >> shift) + pred
            v = -32768 if v < -32768 else (32767 if v > 32767 else v)
            row[j] = n & 15
            s2, s1 = s1, v
    return (filters << 4 | shifts).astype(np.uint8), nib


def xa_encode(pcm, stereo):
    """PCM (int16, (n, 2) if stereo else (n,)) -> the user data of the XA sectors (2304 bytes each, zero samples after
    the end)."""
    pcm = np.asarray(pcm, np.int32)
    per = xa_samples_per_sector(stereo)
    n = pcm.shape[0]
    count = max(1, -(-n // per))
    total = count * per
    chans = [pcm[:, 0], pcm[:, 1]] if stereo else [pcm]
    coded = [_xa_channel(np.r_[c, np.zeros(total - n, np.int32)]) for c in chans]
    # Sound units in the groups: 8 per group, the channels alternating (stereo) or one after the other (mono)
    if stereo:
        params = np.stack((coded[0][0], coded[1][0]), axis=1).reshape(-1, 8)
        nibs = np.stack((coded[0][1], coded[1][1]), axis=1).reshape(-1, 8, 28)
    else:
        params = coded[0][0].reshape(-1, 8)
        nibs = coded[0][1].reshape(-1, 8, 28)
    groups = np.zeros((params.shape[0], XA_GROUP), np.uint8)
    groups[:, 0:4] = params[:, 0:4]
    groups[:, 4:12] = params
    groups[:, 12:16] = params[:, 4:8]
    # Data: 28 words, byte b of word j = units 2b (low nibble) and 2b+1 (high) at sample j
    data = nibs[:, 0::2, :] | (nibs[:, 1::2, :] << 4)          # (groups, 4, 28)
    groups[:, 16:] = data.transpose(0, 2, 1).reshape(-1, 112)
    raw = groups.reshape(count, XA_DATA).tobytes()
    return [raw[i * XA_DATA:(i + 1) * XA_DATA] for i in range(count)]


def xa_decode(sectors, stereo):
    """XA sector user data (4 bits) -> PCM int16 ((n, 2) if stereo else (n,))."""
    hist = [[0, 0], [0, 0]]
    out = [[], []]
    for data in sectors:
        for g in range(XA_GROUPS):
            base = g * XA_GROUP
            for u in range(8):
                c = (u & 1) if stereo else 0
                param = data[base + 4 + u]
                shift, f = param & 15, (param >> 4) & 3
                if shift > 12:
                    shift = 9
                k0, k1 = XA_K0[f], XA_K1[f]
                s1, s2 = hist[c]
                for j in range(28):
                    byte = data[base + 16 + j * 4 + (u >> 1)]
                    n = (byte >> 4) if (u & 1) else (byte & 15)
                    if n >= 8:
                        n -= 16
                    s = ((n << 12) >> shift) + ((s1 * k0 + s2 * k1 + 32) >> 6)
                    s = -32768 if s < -32768 else (32767 if s > 32767 else s)
                    out[c].append(s)
                    s2, s1 = s1, s
                hist[c] = [s1, s2]
    if stereo:
        n = min(len(out[0]), len(out[1]))
        return np.stack((np.array(out[0][:n], np.int16), np.array(out[1][:n], np.int16)), axis=1)
    return np.array(out[0], np.int16)


# ---------------------------------------------------------------------------------------------------- CD sectors

STR_MAGIC = b"\x60\x01\x01\x80"         # STR video sector header: 0x0160, 0x8001 (little-endian)
VIDEO_HEADER = 32
VIDEO_PAYLOAD = 2048 - VIDEO_HEADER     # Frame bytes in a video sector
SUBMODE_EOR, SUBMODE_VIDEO, SUBMODE_AUDIO, SUBMODE_DATA = 0x01, 0x02, 0x04, 0x08
SUBMODE_FORM2, SUBMODE_RT, SUBMODE_EOF = 0x20, 0x40, 0x80


def _edc_table():
    t = np.zeros(256, np.uint32)
    for i in range(256):
        e = i
        for _ in range(8):
            e = (e >> 1) ^ (0xD8018001 if e & 1 else 0)
        t[i] = e
    return t


_EDC = _edc_table()
_ECC_F = np.zeros(256, np.uint8)
_ECC_B = np.zeros(256, np.uint8)
for _i in range(256):
    _j = (_i << 1) ^ (0x11D if _i & 0x80 else 0)
    _ECC_F[_i] = _j
    _ECC_B[_i ^ _j] = _i


def edc(blocks):
    """CD EDC (CRC) of each row of a (N, size) uint8 array: (N,) uint32 (all the rows at once)."""
    crc = np.zeros(blocks.shape[0], np.uint32)
    for i in range(blocks.shape[1]):
        crc = _EDC[(crc ^ blocks[:, i]) & 0xFF] ^ (crc >> 8)
    return crc


def _ecc_block(src, major_count, minor_count, major_mult, minor_inc):
    """ECC P or Q parity of each row of src (header + subheader + data + EDC, and P for Q): (N, 2 * major_count)."""
    size = major_count * minor_count
    major = np.arange(major_count)
    index = (major >> 1) * major_mult + (major & 1)
    a = np.zeros((src.shape[0], major_count), np.uint8)
    b = np.zeros((src.shape[0], major_count), np.uint8)
    for _ in range(minor_count):
        t = src[:, index]
        index = index + minor_inc
        index = np.where(index >= size, index - size, index)
        a ^= t
        b ^= t
        a = _ECC_F[a]
    a = _ECC_B[_ECC_F[a] ^ b]
    return np.concatenate((a, a ^ b), axis=1)


def mode2_sectors(subheaders, datas, forms):
    """2336-byte sectors (subheader x2, user data, EDC, ECC for Form 1): subheaders (N, 4), datas: 2048 bytes (Form 1)
    or 2324 bytes (Form 2), forms: 1 or 2. Returns bytes (N * 2336)."""
    n = len(datas)
    out = np.zeros((n, 2336), np.uint8)
    sub = np.asarray(subheaders, np.uint8).reshape(n, 4)
    out[:, 0:4] = sub
    out[:, 4:8] = sub
    forms = np.asarray(forms)
    one = np.nonzero(forms == 1)[0]
    two = np.nonzero(forms == 2)[0]
    if len(one):
        d = np.frombuffer(b"".join(datas[i] for i in one), np.uint8).reshape(-1, 2048)
        out[one, 8:2056] = d
        e = edc(out[one, 0:2056])
        out[one, 2056:2060] = e.view(np.uint8).reshape(-1, 4)
        # ECC: over the 4-byte header (zero in Mode 2), the subheader, the data and the EDC
        src = np.zeros((len(one), 2064), np.uint8)
        src[:, 4:2064] = out[one, 0:2060]
        p = _ecc_block(src, 86, 24, 2, 86)
        src2 = np.concatenate((src, p), axis=1)     # (Q: over the same bytes and P)
        q = _ecc_block(src2, 52, 43, 86, 88)
        out[one, 2060:2232] = p
        out[one, 2232:2336] = q
    if len(two):
        d = np.frombuffer(b"".join(datas[i] for i in two), np.uint8).reshape(-1, 2324)
        out[two, 8:2332] = d
        e = edc(out[two, 0:2332])
        out[two, 2332:2336] = e.view(np.uint8).reshape(-1, 4)
    return out.tobytes()


def video_sector_header(index, count, frame, size, w, h, bs):
    """The 32 bytes before the frame data in each of its sectors (frame numbers start at 1)."""
    return struct.pack("<HHHHIIHH", 0x0160, 0x8001, index, count, frame, size, w, h) + bs[:8] + b"\0" * 4


# ---------------------------------------------------------------------------------------------------- STR writer

class StrWriter:
    """Writes an STR movie, frame after frame (the sound given first): the sectors in the order the drive reads them.
    speed: 1 or 2 (75 or 150 sectors a second), fps: frames a second, audio: None or (PCM, rate, stereo).
    fmt: 2336 (Mode 2 sectors, video and XA audio interleaved) or 2048 (video only: the user data)."""

    def __init__(self, path, w, h, fps, speed=2, audio=None, fmt=2336, file_no=1, channel=1):
        self.f = open(path, "wb")
        self.w, self.h, self.fps, self.speed, self.fmt = w, h, float(fps), speed, fmt
        self.file_no, self.channel = file_no, channel
        self.rate = 75 * speed
        self.audio = None
        self.interleave = 0
        if audio is not None and fmt == 2336:
            pcm, rate, stereo = audio
            self.audio = xa_encode(pcm, stereo)
            self.audio_rate, self.stereo = rate, stereo
            self.interleave = xa_interleave(rate, stereo, speed)
            self.coding = (1 if stereo else 0) | (4 if rate == 18900 else 0)
        self.audio_next = 0
        self.sector = 0             # Sectors written
        self.video_sectors = 0      # Video sectors so far (frames are given their share of them)
        self.frames = 0
        self.pending = []           # (subheader, data, form) to write
        self.stats = []             # Per frame: (scale, bytes, sectors)

    def video_rate(self):
        """Video sectors a second."""
        return self.rate * (1 - 1 / self.interleave) if self.interleave else self.rate

    def frame_budget(self, frame=None):
        """Sectors of the frame (its share of the video sectors: frames at a steady rate), and its byte budget."""
        f = self.frames if frame is None else frame
        vr = self.video_rate()
        n = int((f + 1) * vr / self.fps) - int(f * vr / self.fps)
        return max(1, n), max(1, n) * VIDEO_PAYLOAD

    def _audio_slot(self):
        return self.interleave and (self.sector % self.interleave == 0)

    def _put(self, sub, data, form):
        self.pending.append((sub, data, form))
        self.sector += 1
        if len(self.pending) >= 256:
            self.flush()

    def _put_audio(self):
        if self.audio_next < len(self.audio):
            data = self.audio[self.audio_next]
        else:
            data = bytes(XA_DATA)       # (silence once the sound has ended)
        self.audio_next += 1
        sub = (self.file_no, self.channel, SUBMODE_AUDIO | SUBMODE_FORM2 | SUBMODE_RT, self.coding)
        self._put(sub, data + bytes(20), 2)

    def add_frame(self, bs, sectors):
        """The BS frame in its sectors (sectors: its share, from frame_budget; the bytes must fit)."""
        self.frames += 1
        size = len(bs)
        payload = bs + bytes(sectors * VIDEO_PAYLOAD - size)
        for i in range(sectors):
            while self._audio_slot():
                self._put_audio()
            head = video_sector_header(i, sectors, self.frames, size, self.w, self.h, bs)
            data = head + payload[i * VIDEO_PAYLOAD:(i + 1) * VIDEO_PAYLOAD]
            sub = (self.file_no, self.channel, SUBMODE_DATA | SUBMODE_RT, 0)
            self._put(sub, data, 1)
            self.video_sectors += 1
        self.stats.append((bs_header(bs)[1], size, sectors))

    def flush(self):
        if not self.pending:
            return
        if self.fmt == 2048:
            self.f.write(b"".join(d for _, d, _ in self.pending))
        else:
            self.f.write(mode2_sectors([s for s, _, _ in self.pending], [d for _, d, _ in self.pending],
                                       [f for _, _, f in self.pending]))
        self.pending = []

    def close(self):
        """The sound left (its sectors), the last sector marked as the end of the file."""
        if self.audio is not None:
            while self.audio_next < len(self.audio):
                if self._audio_slot():
                    self._put_audio()
                else:   # (no more frames: the slots between the sound's are empty video sectors)
                    sub = (self.file_no, self.channel, SUBMODE_DATA | SUBMODE_RT, 0)
                    self._put(sub, bytes(2048), 1)
        if self.pending:
            sub, data, form = self.pending[-1]
            self.pending[-1] = ((sub[0], sub[1], sub[2] | SUBMODE_EOF | SUBMODE_EOR, sub[3]), data, form)
        self.flush()
        self.f.close()
        return self.sector


# ---------------------------------------------------------------------------------------------------- STR reader

class StrFile:
    """An STR movie read back: its frames (sectors, sizes, scales) and sound, for checks and previews."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        if self.data[0:4] == b"RIFF" and self.data[8:12] == b"CDXA":     # (a CD track copied by Windows: raw sectors)
            self.data = self.data[44:]
        self.path = path
        self.fmt, self.sub_off, self.data_off = self._detect()
        self.count = len(self.data) // self.fmt
        self.frames = []        # dict: number, w, h, sectors, first, last (sector indexes), size, scale, version, ok
        self.audio = []         # Sector indexes of the XA audio sectors
        self.coding = None
        self.problems = []
        self._scan()

    def _detect(self):
        d = self.data
        if len(d) >= 2352 and d[0:12] == b"\x00" + b"\xff" * 10 + b"\x00":
            return 2352, 16, 24
        if len(d) >= 2336 and len(d) % 2336 == 0 and d[0:4] == d[4:8] and (d[8:12] == STR_MAGIC or d[2] & 0x0E):
            return 2336, 0, 8
        if len(d) >= 2048 and len(d) % 2048 == 0 and d[0:4] == STR_MAGIC:
            return 2048, -1, 0
        if len(d) % 2336 == 0 and d[0:4] == d[4:8]:
            return 2336, 0, 8
        raise ValueError("not an STR movie (2336, 2352 or 2048-byte sectors with STR frames)")

    def sector_data(self, i):
        o = i * self.fmt + self.data_off
        return self.data[o:o + 2048]

    def _scan(self):
        cur = None
        for i in range(self.count):
            base = i * self.fmt
            sub = self.data[base + self.sub_off:base + self.sub_off + 4] if self.sub_off >= 0 else None
            if sub and (sub[2] & SUBMODE_AUDIO):
                self.audio.append(i)
                if self.coding is None:
                    self.coding = sub[3]
                continue
            d = self.sector_data(i)
            if d[0:4] != STR_MAGIC:
                continue
            index, count, number, size, w, h = struct.unpack_from("<HHIIHH", d, 4)
            if cur is None or cur["number"] != number:
                if cur:
                    self._close(cur)
                cur = {"number": number, "w": w, "h": h, "sectors": count, "first": i, "last": i, "size": size,
                       "got": set(), "parts": {}}
                self.frames.append(cur)
            cur["got"].add(index)
            cur["last"] = i
            cur["parts"][index] = i
        if cur:
            self._close(cur)

    def _close(self, fr):
        fr["ok"] = len(fr["got"]) == fr["sectors"]
        if not fr["ok"]:
            self.problems.append("frame %d: %d of its %d sectors" % (fr["number"], len(fr["got"]), fr["sectors"]))
        bs = self.frame_bs(fr)
        head = bs_header(bs) if bs else None
        fr["scale"], fr["version"] = (head[1], head[2]) if head else (0, 0)
        if not head:
            self.problems.append("frame %d: no BS header" % fr["number"])
        del fr["got"]

    def frame_bs(self, fr):
        """The BS data of a frame (its sectors' payloads, cut at its size)."""
        parts = [self.sector_data(fr["parts"][k])[VIDEO_HEADER:] for k in sorted(fr["parts"])]
        return b"".join(parts)[:fr["size"]]

    def decode_frame(self, n):
        fr = self.frames[n]
        return decode_bs(self.frame_bs(fr), fr["w"], fr["h"])

    def audio_info(self):
        """(rate, stereo) of the sound, or None."""
        if self.coding is None:
            return None
        return (18900 if self.coding & 4 else 37800), bool(self.coding & 1)

    def interleave(self):
        """Sectors between two audio sectors (0: none)."""
        if len(self.audio) < 2:
            return 0
        return self.audio[1] - self.audio[0]

    def speed(self):
        """1 or 2: the drive speed the sound's interleave was made for (2 if no sound)."""
        info = self.audio_info()
        n = self.interleave()
        if not info or not n:
            return 2
        per_second = info[0] / xa_samples_per_sector(info[1])
        return max(1, int(round(n * per_second / 75)))

    def decode_audio(self):
        info = self.audio_info()
        if not info:
            return None
        o = self.data_off
        sectors = [self.data[i * self.fmt + o:i * self.fmt + o + XA_DATA] for i in self.audio]
        return xa_decode(sectors, info[1])


if __name__ == "__main__":
    # Self test: a moving gradient and a tone through the writer, read back and decoded
    import os
    import tempfile
    import time

    h, w = 240, 320
    t0 = time.time()
    yy, xx = np.mgrid[0:h, 0:w]
    img = np.stack(((xx * 255 // w), (yy * 255 // h), ((xx + yy) % 64) * 4), axis=-1).astype(np.uint8)
    bs, scale, q = encode_frame(img, max_bytes=8 * VIDEO_PAYLOAD)
    back = decode_bs(bs, w, h)
    ref = dequantize(q, scale, w, h)
    print("frame: %d bytes, scale %d, decoded = encoder's reconstruction: %s, PSNR %.1f dB, %.2f s" % (
        len(bs), scale, np.array_equal(back, ref),
        10 * math.log10(255 ** 2 / max(1e-9, np.mean((back.astype(float) - img) ** 2))), time.time() - t0))
    t = np.arange(37800 * 2) / 37800.0
    tone = (np.stack((np.sin(2 * math.pi * 440 * t), np.sin(2 * math.pi * 660 * t)), axis=1) * 12000).astype(np.int16)
    t0 = time.time()
    xa = xa_encode(tone, True)
    pcm = xa_decode(xa, True)
    err = np.sqrt(np.mean((pcm[:len(tone)].astype(float) - tone) ** 2))
    print("XA: %d sectors, RMS error %.0f of 8485 (%.1f dB), %.2f s" % (len(xa), err, 20 * math.log10(8485 / err),
                                                                    time.time() - t0))
    path = os.path.join(tempfile.gettempdir(), "strmovie_test.str")
    wr = StrWriter(path, w, h, 15, speed=2, audio=(tone, 37800, True))
    for i in range(30):
        frame = np.roll(img, i * 8, axis=1)
        n, budget = wr.frame_budget()
        data, s, _ = encode_frame(frame, max_bytes=budget)
        wr.add_frame(data, n)
    total = wr.close()
    st = StrFile(path)
    print("STR: %d sectors written, read back: %d sectors of %d bytes, %d frames, %d audio sectors (1 in %d, %dx), "
          "problems: %s" % (total, st.count, st.fmt, len(st.frames), len(st.audio), st.interleave(), st.speed(),
                            st.problems or "none"))
    print("frame 10 decodes: %s" % (st.decode_frame(10).shape,))
