"""
PS1 DEV CART GUI: VAB sound banks (VH header + VB samples) for the VAB editor of the Sound Tools page.

VH: header (32 bytes), 128 program attributes (16 bytes), 16 tone attributes (32 bytes) for each program with tones
(in program order), 256 VAG sizes (16-bit, in 8 bytes units, VAG 0 unused). The same layout as the sound tester's VAB
player (ps1-tools/soundtest/soundtest.c), which reads the attributes from the VH in RAM: the editor changes bytes at
the same offsets there.

ADSR (the SPU's two envelope registers): adsr1 = attack mode (bit 15: exponential), attack rate (bits 8-14), decay rate
(bits 4-7), sustain level (bits 0-3); adsr2 = sustain mode (bit 15: exponential), direction (bit 14: decrease),
sustain rate (bits 6-12), release mode (bit 5: exponential), release rate (bits 0-4).

    python vab.py out.vab       a small test bank (3 programs, 3 looping samples)

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import math
import struct
import sys

HEADER_SIZE = 32
PROG_SIZE = 16
TONE_SIZE = 32
PROGRAMS = 128
TONES_PER_PROGRAM = 16
SPU_RATE = 44100            # A tone plays its sample at this rate at its center note

# Fields: (name, offset, struct format)
HEADER_FIELDS = (("mvol", 24, "B"), ("pan", 25, "B"))
PROG_FIELDS = (("tones", 0, "B"), ("mvol", 1, "B"), ("prior", 2, "B"), ("mode", 3, "B"), ("mpan", 4, "B"))
TONE_FIELDS = (("prior", 0, "B"), ("mode", 1, "B"), ("vol", 2, "B"), ("pan", 3, "B"), ("center", 4, "B"),
               ("shift", 5, "B"), ("min", 6, "B"), ("max", 7, "B"), ("pbmin", 12, "B"), ("pbmax", 13, "B"),
               ("adsr1", 16, "<H"), ("adsr2", 18, "<H"), ("prog", 20, "<h"), ("vag", 22, "<h"))


class Vab:
    """A VH (with its VB if it was in the file). data: the VH bytes, edited in place; offsets for the console."""

    def __init__(self, vh, vb=b""):
        if vh[:4] != b"pBAV" or len(vh) < HEADER_SIZE:
            raise ValueError("not a VAB/VH file (no pBAV)")
        self.ps, self.ts, self.vs = struct.unpack_from("<3H", vh, 18)
        size = self.vh_size()
        if len(vh) < size:
            raise ValueError("VH too short: %d bytes, %d expected" % (len(vh), size))
        self.data = bytearray(vh[:size])
        self.vb = vb if vb else vh[size:]
        self.blocks = {}            # Program number -> its tone block
        block = 0
        for p in range(PROGRAMS):
            if self.prog(p)["tones"] and block < self.ps:
                self.blocks[p] = block
                block += 1

    @classmethod
    def load(cls, path, vb_path=None):
        with open(path, "rb") as f:
            vh = f.read()
        vb = b""
        if vb_path:
            with open(vb_path, "rb") as f:
                vb = f.read()
        return cls(vh, vb)

    def vh_size(self):
        return HEADER_SIZE + PROGRAMS * PROG_SIZE + self.ps * TONES_PER_PROGRAM * TONE_SIZE + 256 * 2

    # ------------------------------------------------------------ offsets

    @staticmethod
    def prog_offset(p):
        return HEADER_SIZE + p * PROG_SIZE

    def tone_offset(self, p, t):
        return HEADER_SIZE + PROGRAMS * PROG_SIZE + (self.blocks[p] * TONES_PER_PROGRAM + t) * TONE_SIZE

    # ------------------------------------------------------------ attributes

    def _get(self, base, fields):
        return {name: struct.unpack_from(fmt, self.data, base + off)[0] for name, off, fmt in fields}

    def _set(self, base, fields, name, value):
        off, fmt = next((o, f) for n, o, f in fields if n == name)
        struct.pack_into(fmt, self.data, base + off, value)

    def header(self):
        return self._get(0, HEADER_FIELDS)

    def set_header(self, name, value):
        self._set(0, HEADER_FIELDS, name, value)
        return 0, HEADER_SIZE

    def prog(self, p):
        return self._get(self.prog_offset(p), PROG_FIELDS)

    def set_prog(self, p, name, value):
        self._set(self.prog_offset(p), PROG_FIELDS, name, value)
        return self.prog_offset(p), PROG_SIZE

    def tone(self, p, t):
        return self._get(self.tone_offset(p, t), TONE_FIELDS)

    def set_tone(self, p, t, name, value):
        self._set(self.tone_offset(p, t), TONE_FIELDS, name, value)
        return self.tone_offset(p, t), TONE_SIZE

    def programs(self):
        """Program numbers with tones (the tester's program indexes are in this order)."""
        return sorted(self.blocks)

    def tones(self, p):
        return [self.tone(p, t) for t in range(min(self.prog(p)["tones"], TONES_PER_PROGRAM))]

    def vag_sizes(self):
        base = self.vh_size() - 512
        return [struct.unpack_from("<H", self.data, base + i * 2)[0] * 8 for i in range(256)]

    def vag_data(self, n):
        """ADPCM data of VAG n (1...) in the VB."""
        sizes = self.vag_sizes()
        start = sum(sizes[1:n])
        return self.vb[start:start + sizes[n]]

    def bytes(self, with_vb=True):
        return bytes(self.data) + (self.vb if with_vb else b"")


# ------------------------------------------------------------ ADSR

def adsr_fields(adsr1, adsr2):
    return {"attack_exp": adsr1 >> 15 & 1, "attack": adsr1 >> 8 & 127, "decay": adsr1 >> 4 & 15,
            "sustain_level": adsr1 & 15, "sustain_exp": adsr2 >> 15 & 1, "sustain_dec": adsr2 >> 14 & 1,
            "sustain": adsr2 >> 6 & 127, "release_exp": adsr2 >> 5 & 1, "release": adsr2 & 31}


def adsr_regs(f):
    adsr1 = f["attack_exp"] << 15 | f["attack"] << 8 | f["decay"] << 4 | f["sustain_level"]
    adsr2 = f["sustain_exp"] << 15 | f["sustain_dec"] << 14 | f["sustain"] << 6 | f["release_exp"] << 5 | f["release"]
    return adsr1, adsr2


def _phase(level, target, rising, exp, shift, step, max_ticks, points, t):
    """One envelope phase as the SPU runs it (a tick per sample at 44100 Hz), until target or max_ticks: (level, t)."""
    end = t + max_ticks
    base_cycles = 1 << max(0, shift - 11)
    base_step = step << max(0, 11 - shift)
    while t < end:
        if (rising and level >= target) or (not rising and level <= target):
            break
        cycles = base_cycles * (4 if exp and rising and level > 0x6000 else 1)
        st = base_step * level >> 15 if exp and not rising else base_step
        if st == 0 or t + cycles > end:     # (no progress any more, or the next step after the end: stays there)
            t = end
            break
        level = max(0, min(0x7FFF, level + st))
        t += cycles
        if not points or t - points[-1][0] >= 64:
            points.append((t, level))
    points.append((min(t, end), level))
    return level, min(t, end)


def envelope(adsr1, adsr2, hold=None, max_seconds=8):
    """Envelope points [(seconds, level 0-1)] with the key held then released, and the phase times in seconds
    (attack, decay, release; None: longer than max_seconds). hold: seconds held (default: until the sustain + 0.4 s)."""
    f = adsr_fields(adsr1, adsr2)
    cap = int(max_seconds * SPU_RATE)
    points = [(0, 0)]
    level, t = _phase(0, 0x7FFF, True, f["attack_exp"], f["attack"] >> 2, 7 - (f["attack"] & 3), cap, points, 0)
    attack = t / SPU_RATE if level >= 0x7FFF else None
    sustain_level = (f["sustain_level"] + 1) * 0x800
    t0 = t
    level, t = _phase(level, sustain_level, False, True, f["decay"], -8, cap, points, t)
    decay = (t - t0) / SPU_RATE if level <= sustain_level else None
    release_at = t + int(0.4 * SPU_RATE) if hold is None else int(hold * SPU_RATE)
    if release_at > t:
        sus_step = (-8 + (f["sustain"] & 3)) if f["sustain_dec"] else (7 - (f["sustain"] & 3))
        level, t = _phase(level, 0 if f["sustain_dec"] else 0x7FFF, not f["sustain_dec"], f["sustain_exp"],
                          f["sustain"] >> 2, sus_step, release_at - t, points, t)
        t = release_at
        points.append((t, level))
    t0 = t
    level, t = _phase(level, 0, False, f["release_exp"], f["release"], -8, cap, points, t)
    release = (t - t0) / SPU_RATE if level <= 0 else None
    return [(pt / SPU_RATE, lv / 0x7FFF) for pt, lv in points], (attack, decay, release)


# ------------------------------------------------------------ test bank

def _wave(kind, period, count, amp=0.55):
    out = []
    for i in range(count):
        x = 2 * math.pi * (i % period) / period
        if kind == "sine":
            v = math.sin(x)
        elif kind == "square":
            v = sum(math.sin(k * x) / k for k in (1, 3, 5, 7)) * 1.1
        else:
            v = sum(math.sin(k * x) / k for k in range(1, 9)) * 0.6
        out.append(int(v * amp * 32767 * min(1.0, i / 200)))
    return out


def build_test_vab():
    """A test bank: program 0 "lead" (sine below C4, square from C4), program 1 "pad" (saw, slow attack), program 2
    "pluck" (sine, fast decay). Samples at 44100 Hz, period 112 samples (G4: center note 67), looping on whole
    periods and blocks (no click)."""
    import vag
    period, intro, loop = 112, 4480, 896
    vags = [vag.encode_adpcm(_wave(k, period, intro + loop), loop_start=intro) for k in ("sine", "square", "saw")]
    tones = {0: [(1, 0, 59, 0x148B, 0x5FCB), (2, 60, 127, 0x148B, 0x5FCB)],     # Fast attack, decay to a sustain
             1: [(3, 0, 127, 0x380F, 0x5FCE)],      # Attack about 0.85 s, release about 0.75 s
             2: [(1, 0, 127, 0x0090, 0xCA08)]}      # Plucked: fast exponential decay to silence
    ps = len(tones)
    vh = bytearray(HEADER_SIZE + PROGRAMS * PROG_SIZE + ps * TONES_PER_PROGRAM * TONE_SIZE + 512)
    vb = b"".join(vags)
    struct.pack_into("<4sIII4H4BI", vh, 0, b"pBAV", 7, 0, len(vh) + len(vb), 0xEEEE, ps,
                     sum(len(t) for t in tones.values()), len(vags), 127, 64, 0, 0, 0xFFFFFFFF)
    for p in range(PROGRAMS):
        struct.pack_into("<6BH", vh, HEADER_SIZE + p * PROG_SIZE, len(tones.get(p, ())), 127, 0, 0, 64, 0, 0)
    base = HEADER_SIZE + PROGRAMS * PROG_SIZE
    for p, prog_tones in tones.items():
        for t, (v, lo, hi, a1, a2) in enumerate(prog_tones):
            struct.pack_into("<8B6B2xHHhh", vh, base + (p * TONES_PER_PROGRAM + t) * TONE_SIZE,
                             0, 0, 110, 64, 67, 0, lo, hi, 0, 0, 0, 0, 2, 2, a1, a2, p, v)
    sizes = base + ps * TONES_PER_PROGRAM * TONE_SIZE
    for i, data in enumerate(vags):
        struct.pack_into("<H", vh, sizes + (i + 1) * 2, len(data) // 8)
    return bytes(vh) + vb


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    data = build_test_vab()
    with open(sys.argv[1], "wb") as out:
        out.write(data)
    print("%s: %d bytes" % (sys.argv[1], len(data)))
