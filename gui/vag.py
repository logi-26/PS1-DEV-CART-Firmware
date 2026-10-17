"""WAV -> VAG (PS1 SPU ADPCM) encoder, for the synth page (and usable on its own).

    python vag.py in.wav out.vag [--rate 22050] [--loop] [--loop-start SECONDS] [--name NAME]

SPU ADPCM: 16-byte blocks of 28 samples. Byte 0 = predictor filter (high nibble) and shift (low nibble), byte 1 =
flags (1: end, 2: repeat, 4: loop start), then 14 bytes of 4-bit residuals (low nibble first). Each block is encoded
with the filter/shift giving the smallest error, decoding as the SPU does so the error doesn't build up.
"""

import argparse
import os
import struct
import sys
import wave

BLOCK_SAMPLES = 28
FILTERS = ((0, 0), (60, 0), (115, -52), (98, -55), (122, -60))   # Predictor coefficients / 64
FLAG_END, FLAG_REPEAT, FLAG_LOOP_START = 1, 2, 4


def read_wav(path):
    """Returns (mono 16-bit samples, sample rate). 8, 16, 24 and 32-bit PCM; channels are mixed."""
    with wave.open(path, "rb") as w:
        channels, width, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
        raw = w.readframes(w.getnframes())
    count = len(raw) // width
    if width == 1:
        values = [(b - 128) << 8 for b in raw]
    elif width == 2:
        values = list(struct.unpack("<%dh" % count, raw[:count * 2]))
    elif width == 3:
        values = [int.from_bytes(raw[i:i + 3], "little", signed=True) >> 8 for i in range(0, count * 3, 3)]
    elif width == 4:
        values = [v >> 16 for v in struct.unpack("<%di" % count, raw[:count * 4])]
    else:
        raise ValueError("unsupported WAV sample size: %d bytes" % width)
    if channels > 1:
        values = [sum(values[i:i + channels]) // channels for i in range(0, len(values) - channels + 1, channels)]
    return values, rate


def resample(samples, rate, new_rate):
    """Linear interpolation (enough for samples played at other pitches anyway)."""
    if new_rate == rate or not samples:
        return samples
    step = rate / new_rate
    count = int(len(samples) / step)
    out = []
    last = len(samples) - 1
    for i in range(count):
        pos = i * step
        j = int(pos)
        frac = pos - j
        a = samples[j]
        b = samples[min(j + 1, last)]
        out.append(int(a + (b - a) * frac))
    return out


def _clamp16(v):
    return -32768 if v < -32768 else (32767 if v > 32767 else v)


def _encode_block(block, s1, s2, f, shift):
    """Encodes 28 samples with one filter/shift, decoding like the SPU. Returns (error, nibbles, s1, s2)."""
    f0, f1 = FILTERS[f]
    nibbles = []
    error = 0
    for x in block:
        predicted = (s1 * f0 + s2 * f1 + 32) >> 6
        residual = x - predicted
        # Nearest 4-bit value: decoded as (n << 12) >> shift
        n = (residual * (1 << shift) + (1 << 11)) >> 12
        n = -8 if n < -8 else (7 if n > 7 else n)
        decoded = _clamp16(((n << 12) >> shift) + predicted)
        error += (x - decoded) * (x - decoded)
        nibbles.append(n & 15)
        s2, s1 = s1, decoded
    return error, nibbles, s1, s2


def encode_adpcm(samples, loop_start=None):
    """ADPCM data for 16-bit samples. loop_start: sample where the loop starts (None: one-shot)."""
    if loop_start is not None:
        # The loop starts on a block boundary: silence added before so that the loop point is kept
        pad = (-loop_start) % BLOCK_SAMPLES
        samples = [0] * pad + list(samples)
        loop_block = (loop_start + pad) // BLOCK_SAMPLES
    samples = list(samples) + [0] * ((-len(samples)) % BLOCK_SAMPLES)
    blocks = len(samples) // BLOCK_SAMPLES
    out = bytearray(16)          # First block silent (usual for VAGs, the SPU reads it at key on)
    s1 = s2 = 0
    for b in range(blocks):
        block = samples[b * BLOCK_SAMPLES:(b + 1) * BLOCK_SAMPLES]
        best = None
        for f, (f0, f1) in enumerate(FILTERS):
            # Shift guessed from the open loop residual, then its neighbours tried with the real (closed loop) one
            p1, p2, peak = s1, s2, 0
            for x in block:
                r = x - ((p1 * f0 + p2 * f1 + 32) >> 6)
                peak = max(peak, abs(r))
                p2, p1 = p1, x
            shift = 12
            while shift > 0 and peak > (7 << (12 - shift)):
                shift -= 1
            for sh in (shift - 1, shift, shift + 1):
                if 0 <= sh <= 12:
                    result = _encode_block(block, s1, s2, f, sh)
                    if best is None or result[0] < best[0]:
                        best = result + (f, sh)
                if best and best[0] == 0:
                    break
        error, nibbles, s1, s2, f, sh = best
        flags = 0
        if loop_start is not None and b == loop_block:
            flags |= FLAG_LOOP_START
        if b == blocks - 1:
            flags |= FLAG_END | (FLAG_REPEAT if loop_start is not None else 0)
        data = bytes(nibbles[i] | (nibbles[i + 1] << 4) for i in range(0, BLOCK_SAMPLES, 2))
        out += bytes(((f << 4) | sh, flags)) + data
    if not blocks:
        out[1] = FLAG_END
    return bytes(out)


def vag_bytes(adpcm, rate, name=""):
    """VAG file: "VAGp", version, reserved, data size, sample rate (big endian), reserved, name (16), data."""
    header = b"VAGp" + struct.pack(">IIII", 0x20, 0, len(adpcm), rate) + bytes(12)
    header += name.encode("ascii", "replace")[:16].ljust(16, b"\0")
    return header + adpcm


def wav_to_vag(wav_path, vag_path=None, rate=None, loop=False, loop_start=0.0, name=None):
    """Encodes a WAV file; returns the VAG bytes (also written to vag_path if given)."""
    samples, wav_rate = read_wav(wav_path)
    rate = rate or wav_rate
    samples = resample(samples, wav_rate, rate)
    start = int(loop_start * rate) if loop else None
    if start is not None and start >= len(samples):
        start = 0
    if name is None:
        name = os.path.splitext(os.path.basename(wav_path))[0]
    data = vag_bytes(encode_adpcm(samples, start), rate, name)
    if vag_path:
        with open(vag_path, "wb") as f:
            f.write(data)
    return data


def main():
    parser = argparse.ArgumentParser(description="WAV to PS1 VAG (SPU ADPCM)")
    parser.add_argument("wav")
    parser.add_argument("vag")
    parser.add_argument("--rate", type=int, help="sample rate of the VAG (default: the WAV's)")
    parser.add_argument("--loop", action="store_true", help="loop the sample (to its end)")
    parser.add_argument("--loop-start", type=float, default=0.0, help="loop start in seconds (with --loop)")
    parser.add_argument("--name", help="name in the VAG header (default: the file name)")
    args = parser.parse_args()
    data = wav_to_vag(args.wav, args.vag, args.rate, args.loop, args.loop_start, args.name)
    print("%s: %d bytes of ADPCM" % (args.vag, len(data) - 48))


if __name__ == "__main__":
    sys.exit(main())
