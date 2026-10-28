"""
PS1 DEV CART GUI: SPU RAM analysis for the SPU RAM Viewer page ("ps1transfer -spu": the 512KB of SPU RAM, then the
512 bytes of SPU registers 0x1F801C00-0x1F801DFF).

SPU RAM: 0x0000-0x0FFF the capture buffers (CD audio left/right, voices 1 and 3), then the samples (ADPCM: 16-byte
blocks of 28 samples, byte 1 = flags: 1 end, 2 repeat, 4 loop start, see vag.py), the reverb work area at the end
(from mBASE * 8, written by the SPU while the reverb is on). A sample runs from its first block to the block with the
end flag (silent all-zero blocks inside it included); all-zero blocks between samples are free space.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import struct
import subprocess
import sys
import tempfile
import wave

RAM_SIZE = 0x80000
REGS_SIZE = 512
DUMP_SIZE = RAM_SIZE + REGS_SIZE
CAPTURE_END = 0x1000
BLOCK = 16
VOICES = 24
FILTERS = ((0, 0), (60, 0), (115, -52), (98, -55), (122, -60))
FLAG_END, FLAG_REPEAT, FLAG_LOOP_START = 1, 2, 4
BASE_RATE = 44100           # Pitch 0x1000
NOISE_SIZE = 1024           # A "sample" shorter than this cut by non-ADPCM data: noise
FREE_RUN = 4096             # Zero blocks inside a sample: silence, up to this (then free memory)


class Voice:
    __slots__ = ("n", "vol_l", "vol_r", "pitch", "start", "adsr", "env", "repeat", "reverb", "ended")

    @property
    def rate(self):
        return self.pitch * BASE_RATE // 0x1000

    @property
    def active(self):
        return self.env > 0


class Region:
    """A part of SPU RAM: kind "capture", "sample", "free", "reverb" or "data" (not ADPCM)."""
    __slots__ = ("start", "end", "kind", "index", "voices", "loops", "loop_start", "warnings")

    def __init__(self, start, end, kind):
        self.start, self.end, self.kind = start, end, kind
        self.index = 0          # Sample number (1...)
        self.voices = []        # Voices starting (or looping) in it
        self.loops = False      # Its end block repeats (loop)
        self.loop_start = None  # Address of its loop start block
        self.warnings = []

    @property
    def size(self):
        return self.end - self.start

    def name(self):
        return {"capture": "Capture buffers (CD audio, voices 1 and 3)", "free": "Free",
                "reverb": "Reverb work area", "data": "Not ADPCM data",
                "sample": "Sample %d" % self.index}[self.kind]


REVERB_OFF = " (reverb off)"


class Spu:
    def __init__(self, dump):
        if len(dump) != DUMP_SIZE:
            raise ValueError("not an SPU dump (%d bytes, %d expected)" % (len(dump), DUMP_SIZE))
        self.ram = dump[:RAM_SIZE]
        self.regs = dump[RAM_SIZE:]
        r = self.regs
        self.voices = []
        eon, endx = struct.unpack_from("<I", r, 0x198)[0], struct.unpack_from("<I", r, 0x19C)[0]
        for n in range(VOICES):
            v = Voice()
            v.n = n
            v.vol_l, v.vol_r, v.pitch, start, adsr_lo, adsr_hi, v.env, repeat = struct.unpack_from("<hhHHHHhH", r, n * 16)
            v.start, v.repeat, v.adsr = start * 8, repeat * 8, adsr_lo | (adsr_hi << 16)
            v.reverb, v.ended = bool(eon >> n & 1), bool(endx >> n & 1)
            self.voices.append(v)
        self.mbase = struct.unpack_from("<H", r, 0x1A2)[0] * 8
        self.irq_adrs = struct.unpack_from("<H", r, 0x1A4)[0] * 8
        self.cnt = struct.unpack_from("<H", r, 0x1AA)[0]
        self.reverb_on = bool(self.cnt & 0x80)
        self.irq_on = bool(self.cnt & 0x40)
        self.regions = self._map()
        self._voices()

    # ------------------------------------------------------------ map

    def _block(self, adrs):
        """Kind of the 16-byte block: "zero", "adpcm" or "bad"."""
        b = self.ram[adrs:adrs + BLOCK]
        if not any(b):
            return "zero"
        return "adpcm" if (b[0] >> 4) <= 4 and (b[0] & 15) <= 12 and not (b[1] & ~7) else "bad"

    def _map(self):
        regions = [Region(0, CAPTURE_END, "capture")]
        # The reverb work area (mBASE: kept by the libraries even with the reverb off, e.g. 16 bytes at 0x7FFF0)
        limit = self.mbase if CAPTURE_END < self.mbase < RAM_SIZE else RAM_SIZE
        adrs, n = CAPTURE_END, 0
        while adrs < limit:
            kind = self._block(adrs)
            start = adrs
            if kind == "zero":
                while adrs < limit and self._block(adrs) == "zero":
                    adrs += BLOCK
                # A single zero block just before a sample: its first block (VAG files start with one)
                if adrs - start == BLOCK and adrs < limit and self._block(adrs) == "adpcm":
                    adrs = start
                    kind = "adpcm"
                else:
                    regions.append(Region(start, adrs, "free"))
                    continue
            if kind == "bad":
                while adrs < limit and self._block(adrs) == "bad":
                    adrs += BLOCK
                regions.append(Region(start, adrs, "data"))
                continue
            # A sample: up to its end flag (zero blocks inside: silence)
            region = Region(start, start, "sample")
            n += 1
            region.index = n
            ended = False
            zeros = None            # Start of the zero blocks running now
            while adrs < limit:
                k = self._block(adrs)
                if k == "bad":
                    region.warnings.append("Not ADPCM at 0x%05X before its end flag: overwritten?" % adrs)
                    break
                if k == "zero":
                    zeros = adrs if zeros is None else zeros
                    if adrs + BLOCK - zeros >= FREE_RUN:     # Free memory, not silence: its end is missing
                        adrs = zeros
                        region.warnings.append("No end flag before free memory at 0x%05X: cut, or cleared?" % zeros)
                        break
                else:
                    zeros = None
                flags = self.ram[adrs + 1]
                if flags & FLAG_LOOP_START and region.loop_start is None:
                    region.loop_start = adrs
                adrs += BLOCK
                if flags & FLAG_END:
                    region.loops = bool(flags & FLAG_REPEAT)
                    ended = True
                    break
            region.end = adrs
            if not ended and adrs >= limit:
                region.warnings.append("No end flag before %s" % (
                    "the end of SPU RAM" if limit == RAM_SIZE else
                    "the reverb work area: the reverb overwrites its end" if self.reverb_on else
                    "the reverb work area (reverb off now: overwritten once it is on)"))
            regions.append(region)
        if limit < RAM_SIZE:
            regions.append(Region(limit, RAM_SIZE, "reverb"))
        # Noise (e.g. left by another program: SPU RAM survives a reset) looks like ADPCM for a few blocks: a short
        # "sample" cut by non-ADPCM data is data too, joined with its neighbours
        cleaned = []
        for i, r in enumerate(regions):
            if r.kind == "sample" and r.size < NOISE_SIZE and any(w.startswith("Not ADPCM") for w in r.warnings):
                r.kind, r.warnings = "data", []
            if (r.kind == "sample" and r.size == BLOCK and 0 < i < len(regions) - 1 and
                    regions[i - 1].kind == "data" and regions[i + 1].kind in ("data", "sample") and
                    (regions[i + 1].kind == "data" or regions[i + 1].warnings)):
                r.kind, r.warnings = "data", []
            if cleaned and r.kind == "data" and cleaned[-1].kind == "data":
                cleaned[-1].end = r.end
                continue
            cleaned.append(r)
        n = 0
        for r in cleaned:
            if r.kind == "sample":
                n += 1
                r.index = n
        return cleaned

    def region_at(self, adrs):
        for r in self.regions:
            if r.start <= adrs < r.end:
                return r
        return None

    def _voices(self):
        for v in self.voices:
            r = self.region_at(v.start)
            if r is None:
                continue
            idle = not v.active and v.start == v.repeat
            if v.n not in r.voices:
                r.voices.append(v.n)
            if not v.active:
                continue
            what = "voice %d (playing)" % v.n
            if r.kind == "free":
                r.warnings.append("%s starts in free (silent) memory at 0x%05X: sample not loaded there?" %
                                  (what, v.start))
            elif r.kind in ("capture", "reverb", "data"):
                r.warnings.append("%s starts in the %s at 0x%05X" % (what, r.name().lower(), v.start))
            elif v.start != r.start and v.start != r.loop_start and not idle:
                r.warnings.append("%s starts 0x%X bytes into it (0x%05X): wrong address?" %
                                  (what, v.start - r.start, v.start))

    # ------------------------------------------------------------ totals

    def totals(self):
        free = [r.size for r in self.regions if r.kind == "free"]
        samples = [r for r in self.regions if r.kind == "sample"]
        return {"samples": len(samples), "used": sum(r.size for r in samples), "free": sum(free),
                "largest_free": max(free, default=0),
                "reverb": RAM_SIZE - self.mbase if self.reverb_on and self.mbase else 0,
                "warnings": sum(len(r.warnings) for r in self.regions)}

    def rate_of(self, region):
        """Sample rate of a sample: the pitch of a voice using it (the last one keyed on), else None."""
        for n in region.voices:
            v = self.voices[n]
            if v.pitch:
                return v.rate
        return None

    # ------------------------------------------------------------ decoding

    def decode(self, region, loops=0):
        """16-bit samples of a sample region, as the SPU plays it; loops: times its loop part is played again."""
        out = []
        s1 = s2 = 0
        ram = self.ram
        loop_index = None
        for adrs in range(region.start, region.end, BLOCK):
            if adrs == (region.loop_start if region.loop_start is not None else region.start):
                loop_index = (len(out), s1, s2)
            s1, s2 = decode_block(ram[adrs:adrs + BLOCK], s1, s2, out)
        if region.loops and loops and loop_index:
            part = out[loop_index[0]:]
            for _ in range(loops):
                out += part
        return out


def decode_block(block, s1, s2, out):
    """One ADPCM block decoded into out; returns the filter state (s1, s2)."""
    f = min(block[0] >> 4, 4)
    shift = block[0] & 15
    if shift > 12:
        shift = 9
    f0, f1 = FILTERS[f]
    for i in range(28):
        n = block[2 + i // 2] >> ((i & 1) * 4) & 15
        if n >= 8:
            n -= 16
        v = ((n << 12) >> shift) + ((s1 * f0 + s2 * f1 + 32) >> 6)
        v = -32768 if v < -32768 else (32767 if v > 32767 else v)
        out.append(v)
        s2, s1 = s1, v
    return s1, s2


def write_wav(path, samples, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<%dh" % len(samples), *samples))


class Player:
    """Plays a sound on the computer (Windows: winsound, otherwise aplay/paplay/afplay), one at a time."""

    def __init__(self):
        self.proc = None
        self.path = os.path.join(tempfile.gettempdir(), "ps1devcart_spu_play.wav")

    def play(self, samples, rate):
        self.stop()
        write_wav(self.path, samples, rate)
        if sys.platform == "win32":
            import winsound
            winsound.PlaySound(self.path, winsound.SND_FILENAME | winsound.SND_ASYNC)
            return True
        for tool in (["afplay"], ["paplay"], ["aplay", "-q"]):
            try:
                self.proc = subprocess.Popen(tool + [self.path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                return True
            except OSError:
                continue
        return False

    def stop(self):
        if sys.platform == "win32":
            import winsound
            winsound.PlaySound(None, 0)
        elif self.proc and self.proc.poll() is None:
            self.proc.terminate()
        self.proc = None
