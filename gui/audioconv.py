"""
PS1 DEV CART GUI: audio converters for the Sound Tools page (Convert tab): common audio files and MIDI made into the
PS1's sound formats.

- Audio -> VAG: a sample (SPU ADPCM, vag.py's format), optionally looping
- Samples -> VAB: a sound bank (VH + VB, or one VAB file): one tone per sample, grouped in programs (instruments), each
  with its root note, key range, volume, pan, envelope (ADSR), reverb and loop
- MIDI -> SEQ: a standard MIDI file (type 0 or 1, its tracks merged) as Sony's SEQ, played with a VAB (the MIDI program
  numbers are the VAB's programs), with loop markers
- Audio -> XA: XA ADPCM for streamed CD music (strmovie.py's encoder), up to the interleave's number of songs in one
  file (one per channel, as games do), 2336-byte Mode 2 sectors (mkpsxiso's "xa" files)

And back (PS1 -> common formats): VAG -> audio (its loop in the WAV's "smpl" chunk, or played again), VAB -> its samples
and a list of its programs and tones, SEQ -> MIDI (the loop as loopStart/loopEnd markers), XA -> audio (each channel).

The sounds are read by movieconv.load_sound: WAV files directly, the others (MP3, OGG, FLAC, M4A...) through FFmpeg,
which also writes them (save_audio).

    python audioconv.py vag in.mp3 out.vag [--rate 22050] [--loop [--loop-start S]]
    python audioconv.py seq in.mid out.seq [--loop-start TICK]
    python audioconv.py xa out.xa song1.mp3 [song2.ogg ...] [--rate 37800|18900] [--mono] [--speed 1|2]
    python audioconv.py vab out.vh a.wav [b.wav ...]       (one program per sample, root note C4)
    python audioconv.py vag2audio in.vag out.wav|.mp3 [--loops N]
    python audioconv.py vab2audio in.vh|in.vab out_folder [--ext .wav]
    python audioconv.py seq2mid in.seq out.mid [--loops N]
    python audioconv.py xa2audio in.xa out_base [--ext .wav] [--channel N]

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import argparse
import math
import os
import struct
import sys

import numpy as np

import movieconv
import strmovie
import vag
import vab

SPU_RAM = 0x80000
SPU_SAMPLES_START = 0x1010          # The sound tester's samples start there (the start of SPU RAM is reserved)
SPU_SAMPLES_MAX = SPU_RAM - SPU_SAMPLES_START
VAG_RATES = (44100, 32000, 22050, 16000, 11025, 8000)
EXACT_SECONDS = 3.0                 # Shorter sounds: vag.py's slower search (best filter/shift per block)
NORMALIZE_PEAK = 0.89               # -1 dBFS


class ConvertError(Exception):
    pass


# ---------------------------------------------------------------------------------------------------- sounds

def load(path, rate, stereo=False, ffmpeg="", start=0.0, duration=0.0, gain=1.0, normalize=False):
    """A sound at the rate, mono or stereo, trimmed (start, duration in seconds; 0: to the end): int16 numpy."""
    try:
        pcm = movieconv.load_sound(path, rate, stereo, ffmpeg, start, duration, 1.0)
    except (OSError, RuntimeError, ValueError) as e:
        raise ConvertError("%s: %s" % (os.path.basename(path), e))
    if not len(pcm):
        raise ConvertError("%s: no sound (empty, or trimmed to nothing)" % os.path.basename(path))
    x = pcm.astype(np.float32) * gain
    if normalize:
        peak = float(np.abs(x).max())
        if peak > 0:
            x *= NORMALIZE_PEAK * 32767 / peak
    return np.clip(x, -32768, 32767).astype(np.int16)


# ---------------------------------------------------------------------------------------------------- VAG

def _encode_fast(samples, loop_start=None, progress=None):
    """SPU ADPCM as vag.encode_adpcm (same layout and flags), faster for long sounds: each block's filter and shift
    from the original samples (numpy), then coded sample after sample from the decoded ones (as the SPU decodes)."""
    x = np.asarray(samples, np.int32)
    if loop_start is not None:
        pad = (-loop_start) % vag.BLOCK_SAMPLES
        x = np.r_[np.zeros(pad, np.int32), x]
        loop_block = (loop_start + pad) // vag.BLOCK_SAMPLES
    x = np.r_[x, np.zeros((-len(x)) % vag.BLOCK_SAMPLES, np.int32)]
    blocks = len(x) // vag.BLOCK_SAMPLES
    out = bytearray(16)
    if not blocks:
        out[1] = vag.FLAG_END
        return bytes(out)
    p1, p2 = np.r_[0, x[:-1]], np.r_[0, 0, x[:-2]]
    peaks = np.stack([np.abs(x - ((p1 * f0 + p2 * f1 + 32) >> 6)).reshape(-1, vag.BLOCK_SAMPLES).max(axis=1)
                      for f0, f1 in vag.FILTERS])
    filters = peaks.argmin(axis=0)
    peak = peaks[filters, np.arange(blocks)]
    shifts = np.full(blocks, 12)
    for sh in range(12, 0, -1):
        shifts = np.where((shifts == sh) & (peak > (7 << (12 - sh))), sh - 1, shifts)
    xs = x.tolist()
    s1 = s2 = 0
    for b in range(blocks):
        f, shift = int(filters[b]), int(shifts[b])
        f0, f1 = vag.FILTERS[f]
        nib = []
        base = b * vag.BLOCK_SAMPLES
        for j in range(vag.BLOCK_SAMPLES):
            pred = (s1 * f0 + s2 * f1 + 32) >> 6
            n = ((xs[base + j] - pred) * (1 << shift) + (1 << 11)) >> 12
            n = -8 if n < -8 else (7 if n > 7 else n)
            v = ((n << 12) >> shift) + pred
            v = -32768 if v < -32768 else (32767 if v > 32767 else v)
            nib.append(n & 15)
            s2, s1 = s1, v
        flags = 0
        if loop_start is not None and b == loop_block:
            flags |= vag.FLAG_LOOP_START
        if b == blocks - 1:
            flags |= vag.FLAG_END | (vag.FLAG_REPEAT if loop_start is not None else 0)
        out += bytes(((f << 4) | shift, flags)) + bytes(nib[i] | (nib[i + 1] << 4) for i in range(0, 28, 2))
        if progress and b % 2000 == 0:
            progress(b, blocks)
    return bytes(out)


def encode_adpcm(samples, rate, loop_start=None, progress=None):
    """SPU ADPCM of int16 samples (loop_start: sample where the loop starts, None: one-shot)."""
    if len(samples) <= EXACT_SECONDS * rate:
        return vag.encode_adpcm([int(v) for v in samples], loop_start)
    return _encode_fast(samples, loop_start, progress)


def to_vag(pcm, rate, loop=False, loop_start=0.0, name="", progress=None):
    """VAG file bytes of a mono sound."""
    start = int(loop_start * rate) if loop else None
    if start is not None and start >= len(pcm):
        start = 0
    return vag.vag_bytes(encode_adpcm(pcm, rate, start, progress), rate, name)


# ---------------------------------------------------------------------------------------------------- VAB

# Envelopes (SPU ADSR registers, see vab.py): name, adsr1, adsr2
ENVELOPES = [
    ("Held, short release", 0x80FF, 0x5FC4),
    ("Held, long release", 0x80FF, 0x5FED),
    ("Slow attack (pad)", 0x380F, 0x5FCE),
    ("Plucked (fades out)", 0x0090, 0xCA08),
    ("One-shot (drums, effects)", 0x80FF, 0x5FF4),
]


def tune(root, rate, cents=0):
    """(center, shift) of a tone: a tone plays its sample at 44100 Hz at its center note (the shift raising it by
    1/128 semitones), so a sample at another rate is played at its own pitch at its root note."""
    d = 12 * math.log2(rate / vab.SPU_RATE) + cents / 100.0
    k = math.floor(d)
    shift = int(round((d - k) * 128))
    if shift >= 128:
        shift, k = 0, k + 1
    return max(0, min(127, root - k)), shift


def build_vab(tones, progress=None):
    """A VAB bank (VH bytes, VB bytes) from tones: dicts with pcm (mono int16), rate, program, root, cents, min, max,
    vol, pan, adsr1, adsr2, reverb, loop, loop_start (seconds). Tones of the same program are its key splits (up to
    16), in their order."""
    if not tones:
        raise ConvertError("no samples")
    programs = {}
    for t in tones:
        programs.setdefault(int(t["program"]), []).append(t)
    for p, ts in programs.items():
        if not 0 <= p < vab.PROGRAMS:
            raise ConvertError("program %d: programs are 0-127" % p)
        if len(ts) > vab.TONES_PER_PROGRAM:
            raise ConvertError("program %d: %d samples, a program has 16 tones at most" % (p, len(ts)))
    if len(tones) > 254:
        raise ConvertError("%d samples: a VAB has 254 at most" % len(tones))
    ps = len(programs)
    vh = bytearray(vab.HEADER_SIZE + vab.PROGRAMS * vab.PROG_SIZE + ps * vab.TONES_PER_PROGRAM * vab.TONE_SIZE + 512)
    vags = []
    for i, t in enumerate(tones):
        if progress:
            progress(i, len(tones))
        start = int(t["loop_start"] * t["rate"]) if t.get("loop") else None
        if start is not None and start >= len(t["pcm"]):
            start = 0
        vags.append(encode_adpcm(t["pcm"], t["rate"], start))
    vb = b"".join(vags)
    struct.pack_into("<4sIII4H4BI", vh, 0, b"pBAV", 7, 0, len(vh) + len(vb), 0xEEEE, ps, len(tones), len(vags),
                     127, 64, 0, 0, 0xFFFFFFFF)
    for p in range(vab.PROGRAMS):
        struct.pack_into("<6BH", vh, vab.HEADER_SIZE + p * vab.PROG_SIZE, len(programs.get(p, ())), 127, 0, 0, 64,
                         0, 0)
    base = vab.HEADER_SIZE + vab.PROGRAMS * vab.PROG_SIZE
    index = {id(t): i for i, t in enumerate(tones)}
    for block, p in enumerate(sorted(programs)):            # (tone blocks in program order)
        for n, t in enumerate(programs[p]):
            center, shift = tune(int(t["root"]), t["rate"], t.get("cents", 0))
            struct.pack_into("<8B6B2xHHhh", vh, base + (block * vab.TONES_PER_PROGRAM + n) * vab.TONE_SIZE,
                             0, 4 if t.get("reverb") else 0, int(t["vol"]), int(t["pan"]), center, shift,
                             int(t["min"]), int(t["max"]), 0, 0, 0, 0, 2, 2, t["adsr1"], t["adsr2"], p,
                             index[id(t)] + 1)
    sizes = base + ps * vab.TONES_PER_PROGRAM * vab.TONE_SIZE
    for i, data in enumerate(vags):
        if len(data) // 8 > 0xFFFF:
            raise ConvertError("sample %d: %d KB, a VAB sample is 512 KB at most" % (i + 1, len(data) // 1024))
        struct.pack_into("<H", vh, sizes + (i + 1) * 2, len(data) // 8)
    return bytes(vh), vb


# ---------------------------------------------------------------------------------------------------- MIDI -> SEQ

def _varlen(v):
    if v < 0:
        raise ConvertError("negative delta time")
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append(0x80 | (v & 0x7F))
        v >>= 7
    return bytes(reversed(out))


def read_midi(path):
    """A standard MIDI file: (ticks per quarter note, events (tick, order, kind, data)): kind "ch" (status, data
    bytes), "tempo" (us per quarter note), "timesig" (numerator, denominator power of 2), "marker" (text)."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"MThd":
        raise ConvertError("not a MIDI file (no MThd)")
    hlen = struct.unpack(">I", d[4:8])[0]
    fmt, ntrks, division = struct.unpack(">HHH", d[8:14])
    if division & 0x8000:
        raise ConvertError("SMPTE time (not ticks per quarter note): not supported")
    if fmt > 1:
        raise ConvertError("MIDI type %d: only types 0 and 1 (tracks played together)" % fmt)
    events, pos, order = [], 8 + hlen, 0
    for _ in range(ntrks):
        while pos + 8 <= len(d) and d[pos:pos + 4] != b"MTrk":      # (unknown chunks skipped)
            pos += 8 + struct.unpack(">I", d[pos + 4:pos + 8])[0]
        if pos + 8 > len(d):
            break
        end = pos + 8 + struct.unpack(">I", d[pos + 4:pos + 8])[0]
        p, tick, status = pos + 8, 0, 0
        while p < min(end, len(d)):
            v = 0
            while True:
                b = d[p]
                p += 1
                v = (v << 7) | (b & 0x7F)
                if not b & 0x80:
                    break
            tick += v
            b = d[p]
            if b & 0x80:
                p += 1
                if b < 0xF0:
                    status = b
            else:
                b = status
            if b == 0xFF:
                typ = d[p]
                p += 1
                n = 0
                while True:
                    c = d[p]
                    p += 1
                    n = (n << 7) | (c & 0x7F)
                    if not c & 0x80:
                        break
                data = d[p:p + n]
                p += n
                if typ == 0x2F:
                    break
                if typ == 0x51 and n == 3:
                    events.append((tick, order, "tempo", (data[0] << 16) | (data[1] << 8) | data[2]))
                elif typ == 0x58 and n >= 2:
                    events.append((tick, order, "timesig", (data[0], data[1])))
                elif typ in (0x01, 0x06):
                    events.append((tick, order, "marker", data.decode("latin-1").strip().lower()))
            elif b in (0xF0, 0xF7):             # SysEx: not in a SEQ
                n = 0
                while True:
                    c = d[p]
                    p += 1
                    n = (n << 7) | (c & 0x7F)
                    if not c & 0x80:
                        break
                p += n
            elif b >= 0x80:
                size = 1 if b & 0xF0 in (0xC0, 0xD0) else 2
                events.append((tick, order, "ch", bytes((b,)) + d[p:p + size]))
                p += size
            else:
                raise ConvertError("bad MIDI data in track at byte %d" % p)
            order += 1
        pos = end
    events.sort(key=lambda e: (e[0], e[1]))
    return division, events


def midi_to_seq(path, loop="markers", loop_tick=0, drum_program=None):
    """SEQ bytes of a MIDI file and a summary dict. loop: "none", "markers" (a "loopStart"/"loopEnd" marker or
    CC 111 as in RPG Maker's music, else none), "whole" (the whole music) or "tick" (from loop_tick to the end).
    drum_program: the VAB program played on MIDI channel 10 (the GM drums), None: its own program changes."""
    ppqn, events = read_midi(path)
    tempo, sig = 500000, (4, 2)
    for t, _, kind, v in events:
        if t > 0:
            break
        if kind == "tempo":
            tempo = v
        elif kind == "timesig":
            sig = v
    end_tick = max([e[0] for e in events] or [0])
    start, stop = None, None
    if loop == "whole":
        start = 0
    elif loop == "tick":
        start = max(0, min(loop_tick, end_tick))
    elif loop == "markers":
        for t, _, kind, v in events:
            if kind == "marker" and v in ("loopstart", "loop start", "loop_start", "[") and start is None:
                start = t
            elif kind == "marker" and v in ("loopend", "loop end", "loop_end", "]"):
                stop = t
            elif kind == "ch" and v[0] & 0xF0 == 0xB0 and v[1] == 111 and start is None:
                start = t
    out = bytearray(b"pQES" + struct.pack(">IH", 1, ppqn) + struct.pack(">I", tempo)[1:] + bytes(sig))
    body = []           # (tick, order, bytes, is channel event)
    seen_tempo0 = False
    for t, o, kind, v in events:
        if kind == "tempo":
            if t == 0 and not seen_tempo0:
                seen_tempo0 = True          # (the first one is the header's tempo)
                continue
            body.append((t, o, b"\xFF\x51" + struct.pack(">I", v)[1:], False))
        elif kind == "ch":
            st, ch = v[0] & 0xF0, v[0] & 15
            if st == 0xB0 and v[1] in (6, 38, 98, 99, 100, 101, 111):  # (NRPN: SEQ loop markers; CC 111: a loop)
                continue
            if st == 0x80:                  # Note off as a note on at velocity 0 (running status, as Sony's SEQ)
                v = bytes((0x90 | ch, v[1], 0))
            if ch == 9 and drum_program is not None and st == 0xC0:
                v = bytes((0xC9, drum_program))
            body.append((t, o, bytes(v), True))
    if drum_program is not None:
        body.append((0, -1, bytes((0xC9, drum_program)), True))
    if start is not None:
        body.append((start, -0.5, b"\xB0\x63\x14", True))      # Loop start (NRPN 99 = 20), as Alundra's music
        body.append((start, -0.4, b"\xB0\x62\x7F", True))
        body.append(((stop if stop is not None and stop > start else end_tick), 1 << 40, b"\xB0\x63\x1E", True))
    body.sort(key=lambda e: (e[0], e[1]))
    last, status, notes, chans, poly, peak = 0, 0, 0, set(), {}, 0
    for t, _, data, is_ch in body:
        out += _varlen(t - last)
        last = t
        if is_ch and data[0] == status:
            out += data[1:]                 # Running status
        else:
            out += data
            status = data[0] if is_ch else 0    # (after a meta event: the next status written again)
        if is_ch and data[0] & 0xF0 == 0x90:
            key = (data[0] & 15, data[1])
            if data[2]:
                notes += 1
                chans.add(data[0] & 15)
                poly[key] = poly.get(key, 0) + 1
            elif poly.get(key):
                poly[key] -= 1
            peak = max(peak, sum(poly.values()))
    out += _varlen(0) + b"\xFF\x2F\x00"
    seconds = 0.0
    tempo_now, t_now = tempo, 0
    for t, _, kind, v in events:
        if kind == "tempo" and t > 0:
            seconds += (t - t_now) * tempo_now / ppqn / 1e6
            tempo_now, t_now = v, t
    seconds += (end_tick - t_now) * tempo_now / ppqn / 1e6
    info = dict(ppqn=ppqn, tempo=tempo, notes=notes, channels=sorted(chans), polyphony=peak, seconds=seconds,
                loop=start, loop_end=(stop if stop is not None and start is not None and stop > start else
                                      (end_tick if start is not None else None)),
                programs=sorted({(d[0] & 15, d[1]) for _, _, d, c in body if c and d[0] & 0xF0 == 0xC0}))
    return bytes(out), info


# ---------------------------------------------------------------------------------------------------- XA

def xa_channels(rate, stereo, speed):
    """Songs an XA file holds (one per channel: the drive reads one sector of each in turn)."""
    return strmovie.xa_interleave(rate, stereo, speed)


def write_xa(path, songs, rate, stereo, speed=2, file_no=1, progress=None):
    """XA file (2336-byte Mode 2 Form 2 sectors) of songs (int16 PCM each), interleaved: song n on channel n, the
    channels left silent (and the songs that end first). Returns (sectors, seconds)."""
    channels = xa_channels(rate, stereo, speed)
    if not songs or len(songs) > channels:
        raise ConvertError("%d songs: at %d Hz %s, %dx speed, an XA file holds %d (one per channel)"
                           % (len(songs), rate, "stereo" if stereo else "mono", speed, channels))
    coded = []
    for i, pcm in enumerate(songs):
        if progress:
            progress(i, len(songs) + 1)
        coded.append(strmovie.xa_encode(pcm, stereo))
    rows = max(len(c) for c in coded)
    coding = (1 if stereo else 0) | (4 if rate == 18900 else 0)
    silence = bytes(strmovie.XA_DATA + 20)
    sub_audio = strmovie.SUBMODE_AUDIO | strmovie.SUBMODE_FORM2 | strmovie.SUBMODE_RT
    with open(path, "wb") as f:
        subs, datas = [], []
        for row in range(rows):
            for ch in range(channels):
                c = coded[ch] if ch < len(coded) else []
                data = (c[row] + bytes(20)) if row < len(c) else silence
                sub = sub_audio | ((strmovie.SUBMODE_EOF | strmovie.SUBMODE_EOR) if row == rows - 1 else 0)
                subs.append((file_no, ch, sub, coding))
                datas.append(data)
            if len(datas) >= 512 or row == rows - 1:
                f.write(strmovie.mode2_sectors(subs, datas, [2] * len(datas)))
                subs, datas = [], []
    seconds = rows * strmovie.xa_samples_per_sector(stereo) / rate
    return rows * channels, seconds


# ---------------------------------------------------------------------------------------------------- PS1 -> audio

AUDIO_OUT = (".wav", ".mp3", ".ogg", ".flac", ".m4a")


def save_audio(path, pcm, rate, ffmpeg="", root=None, loop=None):
    """A sound (int16, (n,) or (n, 2)) saved as WAV (with a "smpl" chunk: root note and loop, for samplers), or MP3,
    OGG, FLAC, M4A... through FFmpeg. loop: (start, end) samples."""
    pcm = np.asarray(pcm, np.int16)
    channels = 1 if pcm.ndim == 1 else pcm.shape[1]
    raw = pcm.astype("<i2").tobytes()
    if path.lower().endswith(".wav"):
        fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * channels * 2, channels * 2, 16)
        chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
        if root is not None or loop:
            period = int(round(1e9 / rate))
            loops = [loop] if loop else []
            smpl = struct.pack("<9I", 0, 0, period, int(root if root is not None else 60), 0, 0, 0, len(loops), 0)
            for i, (a, b) in enumerate(loops):
                smpl += struct.pack("<6I", i, 0, int(a), max(int(a), int(b) - 1), 0, 0)
            chunks += b"smpl" + struct.pack("<I", len(smpl)) + smpl
        chunks += b"data" + struct.pack("<I", len(raw)) + raw + (b"\0" if len(raw) & 1 else b"")
        with open(path, "wb") as f:
            f.write(b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks)
        return
    if not ffmpeg:
        raise ConvertError("%s files need FFmpeg (WAV files don't)" % os.path.splitext(path)[1].upper()[1:])
    cmd = [ffmpeg, "-v", "error", "-nostdin", "-y", "-f", "s16le", "-ar", str(rate), "-ac", str(channels), "-i", "-"]
    if path.lower().endswith(".mp3"):
        cmd += ["-b:a", "192k"]
    import subprocess
    r = subprocess.run(cmd + [path], input=raw, capture_output=True, creationflags=movieconv.NO_WINDOW)
    if r.returncode:
        raise ConvertError("FFmpeg: %s" % (r.stderr.decode(errors="replace").strip().splitlines() or ["failed"])[-1])


def decode_adpcm(adpcm, loops=0):
    """SPU ADPCM -> (int16 samples, loop (start, end) or None), decoded as the SPU does up to the block with the end
    flag. loops: the loop played that many more times (a looping sample's sustain heard longer)."""
    out, s1, s2, loop_start, looped = [], 0, 0, None, False
    for b in range(len(adpcm) // 16):
        blk = adpcm[b * 16:(b + 1) * 16]
        shift, f, flags = blk[0] & 15, (blk[0] >> 4) & 7, blk[1]
        if shift > 12:
            shift = 9
        f0, f1 = vag.FILTERS[f] if f < len(vag.FILTERS) else (0, 0)
        if flags & vag.FLAG_LOOP_START and loop_start is None:
            loop_start = len(out)
        for i in range(28):
            n = (blk[2 + (i >> 1)] >> ((i & 1) * 4)) & 15
            if n >= 8:
                n -= 16
            v = ((n << 12) >> shift) + ((s1 * f0 + s2 * f1 + 32) >> 6)
            v = -32768 if v < -32768 else (32767 if v > 32767 else v)
            out.append(v)
            s2, s1 = s1, v
        if flags & vag.FLAG_END:
            looped = bool(flags & vag.FLAG_REPEAT) and loop_start is not None
            break
    pcm = np.array(out, np.int16)
    if not looped:
        return pcm, None
    end = len(pcm)
    if loops > 0:
        pcm = np.concatenate([pcm] + [pcm[loop_start:end]] * loops)
    return pcm, (loop_start, end)


def read_vag(path):
    """A VAG file: (ADPCM data, sample rate, name)."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"VAGp":
        raise ConvertError("%s: not a VAG file (no VAGp)" % os.path.basename(path))
    size, rate = struct.unpack(">II", d[12:20])
    name = d[32:48].split(b"\0")[0].decode("ascii", "replace")
    data = d[48:48 + size] if size else d[48:]
    return data, rate or 22050, name


def vag_to_audio(src, out, ffmpeg="", loops=0):
    """VAG -> audio file; returns (seconds, rate, looping)."""
    data, rate, _ = read_vag(src)
    pcm, loop = decode_adpcm(data, loops)
    save_audio(out, pcm, rate, ffmpeg, loop=loop if out.lower().endswith(".wav") and not loops else None)
    return len(pcm) / rate, rate, loop is not None


def export_vab(vh_path, out_dir, vb_path=None, ext=".wav", ffmpeg="", progress=None):
    """The samples of a VAB (one file each) and a list of its programs and tones (bank.txt): each sample at the rate
    its first tone plays it at note C4 (root note 60 in the WAV), the other tones' root note at that rate listed.
    Returns (samples written, text file)."""
    if vb_path is None and not vh_path.lower().endswith(".vab"):
        vb_path = os.path.splitext(vh_path)[0] + ".vb"      # (a VH: its VB next to it)
        if not os.path.isfile(vb_path):
            raise ConvertError("%s: its VB file wasn't found (%s)" % (os.path.basename(vh_path),
                                                                     os.path.basename(vb_path)))
    v = vab.Vab.load(vh_path, vb_path)
    base = os.path.splitext(os.path.basename(vh_path))[0]
    os.makedirs(out_dir, exist_ok=True)
    first = {}                      # VAG -> (program, tone) using it first
    for p in v.programs():
        for t in v.tones(p):
            if 1 <= t["vag"] <= v.vs:
                first.setdefault(t["vag"], t)
    rates, lines, written = {}, [], 0
    sizes = v.vag_sizes()
    for n in range(1, v.vs + 1):
        if progress:
            progress(n - 1, v.vs)
        t = first.get(n)
        semis = (60 - t["center"] - t["shift"] / 128.0) if t else 0.0
        rate = int(round(vab.SPU_RATE * 2 ** (semis / 12)))
        if not 4000 <= rate <= 48000:
            rate = vab.SPU_RATE         # (unusual tuning: the sample at its center note)
        rates[n] = rate
        if not sizes[n]:
            continue
        pcm, loop = decode_adpcm(v.vag_data(n))
        name = "%s_%03d%s" % (base, n, ext)
        root = int(round(t["center"] + 12 * math.log2(rate / vab.SPU_RATE) + t["shift"] / 128.0)) if t else 60
        save_audio(os.path.join(out_dir, name), pcm, rate, ffmpeg, root=root, loop=loop)
        written += 1
    lines.append("%s: %d programs, %d tones, %d samples (%s_NNN%s)" % (os.path.basename(vh_path), v.ps, v.ts, v.vs,
                                                                     base, ext))
    lines.append("Each sample is at the rate its first tone plays it at C4 (root note 60). The other tones' root note"
                 " at the sample's rate is listed (as the Samples -> VAB converter takes it).")
    for p in v.programs():
        prog = v.prog(p)
        lines.append("")
        lines.append("Program %d: %d tones, volume %d, pan %d" % (p, prog["tones"], prog["mvol"], prog["mpan"]))
        for i, t in enumerate(v.tones(p)):
            n = t["vag"]
            rate = rates.get(n, vab.SPU_RATE)
            root = t["center"] + 12 * math.log2(rate / vab.SPU_RATE) + t["shift"] / 128.0
            lines.append("  tone %d: sample %03d (%d Hz), keys %d-%d, root note %.2f, volume %d, pan %d, reverb %s, "
                         "ADSR %04X %04X, pitch bend %d/%d" % (
                             i, n, rate, t["min"], t["max"], root, t["vol"], t["pan"],
                             "on" if t["mode"] & 4 else "off", t["adsr1"], t["adsr2"], t["pbmin"], t["pbmax"]))
    text = os.path.join(out_dir, base + "_bank.txt")
    with open(text, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return written, text


def seq_tracks(d):
    """The SEQs of a SEQ or SEP file (Sony's File Formats; the ID "SEQp" or "pQES": the same 32-bit ID either way
    round): [(resolution, tempo, numerator, denominator power, events bytes)]. A SEQ's version is 32-bit (1), or
    16-bit in some tools' files; a SEP (version 0) holds up to 16 SEQs: number, resolution, tempo, rhythm, size,
    data."""
    if len(d) < 16 or d[:4] not in (b"SEQp", b"pQES"):
        raise ConvertError("not a SEQ or SEP file (no SEQp / pQES)")
    be = lambda b: int.from_bytes(b, "big")
    if be(d[4:8]) == 1 or be(d[4:6]) == 1:
        h = 8 if be(d[4:8]) == 1 else 6
        return [(be(d[h:h + 2]), be(d[h + 2:h + 5]), d[h + 5], d[h + 6], d[h + 7:])]
    tracks, p = [], 6
    while p + 13 <= len(d) and len(tracks) < 16:
        size = be(d[p + 9:p + 13])
        tracks.append((be(d[p + 2:p + 4]), be(d[p + 4:p + 7]), d[p + 7], d[p + 8], d[p + 13:p + 13 + size]))
        p += 13 + size
    if not tracks:
        raise ConvertError("an empty SEP")
    return tracks


def _seq_events(d):
    """The events of a SEQ's data: [(tick, kind, data)]: kind "ev" (MIDI bytes), "start" (loop start), "count"
    (its count), "end" (loop end). Running status, also after a meta event (a meta event again)."""
    p, tick, status, out, n = 0, 0, 0, [], 0
    pending = False

    def varlen():
        nonlocal p
        v = 0
        while p < len(d):
            b = d[p]
            p += 1
            v = (v << 7) | (b & 0x7F)
            if not b & 0x80:
                break
        return v
    tick += varlen()
    while p + 1 < len(d) and n < 2000000:
        n += 1
        s = d[p]
        if s & 0x80:
            p += 1
        else:
            s = status
        if s == 0xFF:
            status = 0xFF
            typ = d[p]
            p += 1
            if typ == 0x2F:
                break
            if typ == 0x51:
                out.append((tick, "ev", b"\xFF\x51\x03" + d[p:p + 3]))
                p += 3
            else:
                p += varlen()
        else:
            if s < 0x80 or s >= 0xF0:
                raise ConvertError("bad SEQ data at byte %d" % p)
            status = s
            size = 1 if s & 0xF0 in (0xC0, 0xD0) else 2
            data = bytes((s,)) + d[p:p + size]
            p += size
            if s & 0xF0 == 0xB0 and data[1] == 99 and data[2] in (20, 30):
                out.append((tick, "start" if data[2] == 20 else "end", None))
                pending = data[2] == 20
            elif s & 0xF0 == 0xB0 and data[1] in (6, 98) and pending:
                out.append((tick, "count", data[2]))
                pending = False
            else:
                out.append((tick, "ev", data))
        tick += varlen()
    return out, tick


def seq_to_midi(src, out, loops=0):
    """SEQ (or each SEQ of a SEP: out_NN.mid) -> MIDI file (type 0), played as the console plays it: a loop with a
    count written that many more times, an endless one (count 127) marked loopStart / loopEnd (read back by
    midi_to_seq) and written `loops` more times, a loop end without a count read on. Returns an info dict
    (files: the MIDI files written)."""
    with open(src, "rb") as f:
        d = f.read()
    try:
        tracks = seq_tracks(d)
    except ConvertError as e:
        raise ConvertError("%s: %s" % (os.path.basename(src), e))
    files, total, endless = [], 0, None
    for n, (ppqn, tempo, num, den, data) in enumerate(tracks):
        events, end = _seq_events(data)
        # Played: the loops followed as the sound tester (and libsnd) follows them
        rendered, i, offset, start_i, count, extra, mark = [], 0, 0, None, -1, loops, None
        again = False           # Back at a loop start (a jump): its count is not read again
        while i < len(events):
            t, kind, v = events[i]
            if kind == "ev":
                rendered.append((t + offset, v))
            elif kind == "start":
                start_i, count, again = i, -1, False
            elif kind == "count" and not again:
                count = v
            elif kind == "end" and start_i is not None:
                length = t - events[start_i][0]
                if count == 127 and extra > 0:         # Endless: written again
                    extra -= 1
                    offset += length
                    i, again = start_i + 1, True
                    continue
                if count == 127:                        # Its last pass marked (a MIDI -> SEQ loops it)
                    mark = mark or (t + offset - length, t + offset)
                elif count > 0:                         # Counted: played that many more times
                    count -= 1
                    offset += length
                    i, again = start_i + 1, True
                    continue
            i += 1
            if len(rendered) > 2000000:
                raise ConvertError("the loops don't end (more than 2 million events)")
        if mark:
            rendered.append((mark[0], b"\xFF\x06\x09loopStart"))
            rendered.append((mark[1], b"\xFF\x06\x07loopEnd"))
            endless = endless or mark
        track = bytearray(b"\x00\xFF\x51\x03" + struct.pack(">I", tempo)[1:] + b"\x00\xFF\x58\x04" +
                          bytes((num or 4, den or 2, 24, 8)))
        last = 0
        for t, e in sorted(rendered, key=lambda x: x[0]):
            track += _varlen(t - last) + e
            last = t
        track += _varlen(max(0, end + offset - last)) + b"\xFF\x2F\x00"
        path = out if len(tracks) == 1 else "%s_%02d%s" % (os.path.splitext(out)[0], n, os.path.splitext(out)[1])
        with open(path, "wb") as f:
            f.write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, ppqn) + b"MTrk" + struct.pack(">I", len(track)) + track)
        files.append(path)
        total += len(rendered)
    return dict(ppqn=tracks[0][0], tempo=tracks[0][1], events=total, loop=endless[0] if endless else None,
                loop_end=endless[1] if endless else None, files=files, tracks=len(tracks))


def read_xa(path):
    """The XA audio of a file (2336-byte sectors, 2352-byte raw sectors, or a CD track copied by Windows: RIFF CDXA)
    by channel: {(file, channel): (coding, [2304-byte sound data of each sector])}."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] == b"RIFF" and d[8:12] == b"CDXA":
        d = d[44:]
    if len(d) >= 2352 and d[:12] == b"\x00" + b"\xff" * 10 + b"\x00":
        size, sub = 2352, 16
    elif len(d) >= 2336 and len(d) % 2336 == 0:
        size, sub = 2336, 0
    else:
        raise ConvertError("%s: not XA (2336 or 2352-byte sectors)" % os.path.basename(path))
    chans = {}
    for i in range(len(d) // size):
        s = d[i * size:(i + 1) * size]
        file_no, ch, submode, coding = s[sub:sub + 4]
        if submode & strmovie.SUBMODE_AUDIO and submode & strmovie.SUBMODE_FORM2:
            entry = chans.setdefault((file_no, ch & 31), [coding, []])
            entry[1].append(s[sub + 8:sub + 8 + strmovie.XA_DATA])
    if not chans:
        raise ConvertError("%s: no XA audio sectors" % os.path.basename(path))
    return {k: (v[0], v[1]) for k, v in sorted(chans.items())}


def xa_to_audio(src, out_base, ext=".wav", ffmpeg="", channel=None, progress=None):
    """XA -> one audio file per channel (out_base_chN.ext; channel: only that one), silent channels skipped. Returns
    [(channel, file, seconds)]."""
    chans = read_xa(src)
    todo = [(k, v) for k, v in chans.items() if channel is None or k[1] == channel]
    if not todo:
        raise ConvertError("no channel %d in %s (channels: %s)" % (channel, os.path.basename(src), ", ".join(
            str(k[1]) for k in chans)))
    written, multi_file = [], len({k[0] for k in chans}) > 1
    for i, ((file_no, ch), (coding, sectors)) in enumerate(todo):
        if progress:
            progress(i, len(todo))
        if coding & 0x30:
            raise ConvertError("channel %d: 8-bit XA (not supported)" % ch)
        stereo, rate = bool(coding & 1), 18900 if coding & 4 else 37800
        pcm = strmovie.xa_decode(sectors, stereo)
        loud = np.nonzero(np.abs(pcm.astype(np.int32)).reshape(len(pcm), -1).max(axis=1) >= 16)[0]
        if not len(loud):
            continue                        # (a silent channel)
        pcm = pcm[:loud[-1] + 1]            # (the silence after the song: the file's other songs are longer)
        name = "%s_ch%d%s%s" % (out_base, ch, ("_f%d" % file_no) if multi_file else "", ext)
        save_audio(name, pcm, rate, ffmpeg)
        written.append((ch, name, len(pcm) / rate))
    return written


# ---------------------------------------------------------------------------------------------------- command line

def main():
    ap = argparse.ArgumentParser(description="Audio and MIDI to PS1 formats (VAG, VAB, SEQ, XA)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("vag")
    a.add_argument("src")
    a.add_argument("out")
    a.add_argument("--rate", type=int, default=22050)
    a.add_argument("--loop", action="store_true")
    a.add_argument("--loop-start", type=float, default=0.0)
    a = sub.add_parser("seq")
    a.add_argument("src")
    a.add_argument("out")
    a.add_argument("--loop-start", type=int, help="loop from this tick (default: the file's loop markers)")
    a = sub.add_parser("xa")
    a.add_argument("out")
    a.add_argument("src", nargs="+")
    a.add_argument("--rate", type=int, default=37800, choices=strmovie.XA_RATES)
    a.add_argument("--mono", action="store_true")
    a.add_argument("--speed", type=int, default=2, choices=(1, 2))
    a = sub.add_parser("vab")
    a.add_argument("out", help="out.vh (and out.vb) or out.vab")
    a.add_argument("src", nargs="+")
    a.add_argument("--rate", type=int, default=22050)
    a = sub.add_parser("vag2audio")
    a.add_argument("src")
    a.add_argument("out")
    a.add_argument("--loops", type=int, default=0)
    a = sub.add_parser("vab2audio")
    a.add_argument("src")
    a.add_argument("out_dir")
    a.add_argument("--ext", default=".wav")
    a = sub.add_parser("seq2mid")
    a.add_argument("src")
    a.add_argument("out")
    a.add_argument("--loops", type=int, default=0)
    a = sub.add_parser("xa2audio")
    a.add_argument("src")
    a.add_argument("out_base")
    a.add_argument("--ext", default=".wav")
    a.add_argument("--channel", type=int)
    args = ap.parse_args()
    ff = movieconv.find_ffmpeg()
    if args.cmd == "vag2audio":
        print("%s: %.2f s at %d Hz, loop %s" % ((args.out,) + vag_to_audio(args.src, args.out, ff, args.loops)))
        return 0
    if args.cmd == "vab2audio":
        print("%d samples, %s" % export_vab(args.src, args.out_dir, None, args.ext, ff))
        return 0
    if args.cmd == "seq2mid":
        print("%s: %s" % (args.out, seq_to_midi(args.src, args.out, args.loops)))
        return 0
    if args.cmd == "xa2audio":
        for ch, name, sec in xa_to_audio(args.src, args.out_base, args.ext, ff, args.channel):
            print("channel %d: %s (%.1f s)" % (ch, name, sec))
        return 0
    if args.cmd == "vag":
        pcm = load(args.src, args.rate, False, ff)
        data = to_vag(pcm, args.rate, args.loop, args.loop_start, os.path.splitext(os.path.basename(args.src))[0])
        open(args.out, "wb").write(data)
        print("%s: %d bytes" % (args.out, len(data)))
    elif args.cmd == "seq":
        data, info = midi_to_seq(args.src, "tick" if args.loop_start is not None else "markers", args.loop_start or 0)
        open(args.out, "wb").write(data)
        print("%s: %d bytes, %s" % (args.out, len(data), info))
    elif args.cmd == "xa":
        songs = [load(p, args.rate, not args.mono, ff) for p in args.src]
        n, s = write_xa(args.out, songs, args.rate, not args.mono, args.speed)
        print("%s: %d sectors, %.1f s" % (args.out, n, s))
    else:
        adsr1, adsr2 = ENVELOPES[0][1:]
        tones = [dict(pcm=load(p, args.rate, False, ff), rate=args.rate, program=i, root=60, min=0, max=127, vol=100,
                      pan=64, adsr1=adsr1, adsr2=adsr2, reverb=False, loop=False, loop_start=0.0)
                 for i, p in enumerate(args.src)]
        vh, vb = build_vab(tones)
        if args.out.lower().endswith(".vab"):
            open(args.out, "wb").write(vh + vb)
        else:
            open(args.out, "wb").write(vh)
            open(os.path.splitext(args.out)[0] + ".vb", "wb").write(vb)
        print("%s: VH %d bytes, VB %d bytes" % (args.out, len(vh), len(vb)))


if __name__ == "__main__":
    sys.exit(main())
