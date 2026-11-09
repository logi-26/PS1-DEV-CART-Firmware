"""
Helpers of the sound tests: the console's player run on the PC (seqhost, build_host.py) and what it played compared
with the reference player (seqref.py) and the bank's attributes; SEQ files written for the tests; the test files
found on this computer (PS1_SOUND_CORPUS).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import glob
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "gui"))

import build_host                       # noqa: E402
import seqref                           # noqa: E402
import vab as vabmod                    # noqa: E402

NTSC_US, PAL_US = 16683, 20000
TMP = os.path.join(HERE, "build", "tmp")


# ---------------------------------------------------------------------------------------------------- running it

class HostResult:
    """What seqhost printed: tracks, scan (len_us, channels), notes [(wall_us, music_us, ch, note, vel, [voices])]
    with voices [(prog, center, shift, pitch, voll, volr)], restart / stop (wall us), state (after a seek), done,
    hang / crash lines, raw text."""

    def __init__(self, text, code):
        self.raw, self.code = text, code
        self.tracks, self.scan, self.notes, self.restart, self.stop = [], None, [], None, None
        self.state, self.done, self.hang, self.crash, self.error = None, None, None, None, None
        for line in text.splitlines():
            try:
                self._line(line)
            except (ValueError, IndexError):        # (a line cut by a timeout)
                pass

    def _line(self, line):
        if True:
            f = line.split()
            if not f:
                return
            if f[0] == "NOTE":
                self.notes.append((int(f[1]), int(f[2]), int(f[3]), int(f[4]), int(f[5]), []))
            elif f[0] == "VOICE" and self.notes:
                self.notes[-1][5].append(tuple(int(x) for x in f[3:9]))
            elif f[0] == "TRACK":
                self.tracks.append((int(f[2]), int(f[3]), int(f[4])))
            elif f[0] == "SCAN":
                self.scan = (int(f[1]), int(f[2]))
            elif f[0] == "RESTART":
                self.restart = int(f[1])
            elif f[0] == "STOP":
                self.stop = int(f[1])
            elif f[0] == "STATE":
                self.state = (int(f[1]), [tuple(int(x) for x in c.split(",")) for c in f[2:18]])
            elif f[0] == "DONE":
                self.done = tuple(int(x) for x in f[1:4])
            elif f[0] == "HANG":
                self.hang = line
            elif f[0] == "CRASH":
                self.crash = line
            elif f[0] == "ERROR":
                self.error = line

    def problem(self):
        """A hang, crash or error (text), or None."""
        if self.hang or self.crash or self.error:
            return self.hang or self.crash or self.error
        if self.code not in (0, None):
            return "exit code %s" % self.code
        if self.done is None:
            return "no DONE line (the program died?)"
        return None


def run(seq_path, vh=None, vb=None, timeout=60, **opts):
    """seqhost on a SEQ file: opts track, frame_us, play_ms, seek, repeat, tempo, transpose, mute, guard."""
    exe = build_host.build()
    cmd = [exe]
    if vh and vb:
        cmd += ["--vh", vh, "--vb", vb]
    for key, value in opts.items():
        if key == "guard":
            if value:
                cmd.append("--guard")
        elif value is not None:
            cmd += ["--" + key.replace("_", "-"), str(value)]
    cmd.append(seq_path)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return HostResult(r.stdout, r.returncode)
    except subprocess.TimeoutExpired as e:
        res = HostResult((e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or ""),
                         None)
        res.hang = res.hang or "TIMEOUT after %d s" % timeout
        return res


def run_bytes(data, name="test.seq", **kw):
    os.makedirs(TMP, exist_ok=True)
    fd, path = tempfile.mkstemp(suffix="_" + name, dir=TMP)
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    try:
        return run(path, **kw)
    finally:
        os.remove(path)


# ---------------------------------------------------------------------------------------------------- comparing

def compare_notes(ref, host, frame_us, horizon_us, tempo_pct=100, transpose=0, mute=0, tick_us=1500):
    """Differences between the reference notes and what the console player read: [text] (empty: the same). A note is
    read in the frame that reaches its time: from a frame before it to its time (plus rounding: a tick, tick_us)."""
    problems = []
    scale = 100.0 / tempo_pct
    want = [n for n in ref if n[0] * scale < horizon_us - 2 * frame_us]
    got = [n for n in host if n[0] < horizon_us - 2 * frame_us]
    if len(got) < len(want):
        problems.append("%d notes read, %d expected (first missing: %s)" % (
            len(got), len(want), _note_text(want[len(got)], scale)))
    for i, (w, g) in enumerate(zip(want, got)):
        t = w[0] * scale
        slack = 1500 + tick_us + t * 1e-5
        if (w[1], w[2], w[3]) != (g[2], g[3], g[4]) or not (t - frame_us - slack <= g[0] <= t + slack):
            problems.append("note %d: expected %s, read ch %d note %d vel %d at %.3f s" % (
                i, _note_text(w, scale), g[2], g[3], g[4], g[0] / 1e6))
            break
    return problems


def _note_text(n, scale=1.0):
    return "ch %d note %d vel %d at %.3f s" % (n[1], n[2], n[3], n[0] * scale / 1e6)


def expected_pitch(note, tone, transpose=0):
    n = max(0, min(127, note + transpose))
    cents = (n - tone["center"]) * 100 + tone["shift"] * 100 // 128
    return max(1, min(0x3FFF, round(4096 * 2 ** (cents / 1200.0))))


def expected_volumes(vel, ch, tone, prog, bank_mvol, master=100):
    vol = 0x3FFF * vel // 127
    for v in (ch["volume"], ch["expression"], tone["vol"], prog["mvol"], bank_mvol, master):
        vol = vol * v // 127
    pan = max(0, min(127, tone["pan"] + prog["mpan"] + ch["pan"] - 128))
    left = vol if pan <= 64 else vol * (127 - pan) // 63
    right = vol if pan >= 64 else vol * pan // 64
    return left, right


def compare_voices(ref, host, bank, transpose=0, mute=0, limit=3):
    """The voices keyed for each note against the bank: as many as its program's tones with the note in their range
    (and a sample), each at its tone's pitch and volume (channel state of the reference). [text]"""
    problems, header = [], bank.header()
    for w, g in zip(ref, host):
        ch, note, vel, st = w[1], w[2], w[3], w[4]
        prog_no = st["prog"]
        played = max(0, min(127, note + transpose))
        tones = []
        if prog_no in bank.blocks:
            tones = [t for t in bank.tones(prog_no) if t["min"] <= played <= t["max"] and 1 <= t["vag"] < 256]
        if mute & (1 << ch):
            tones = []
        if len(g[5]) != len(tones):
            problems.append("ch %d note %d at %.3f s (program %d): %d voices keyed, %d tones expected" % (
                ch, note, w[0] / 1e6, prog_no, len(g[5]), len(tones)))
        else:
            prog = bank.prog(prog_no) if tones else None
            for (vprog, center, shift, pitch, voll, volr), t in zip(g[5], tones):
                want_pitch = expected_pitch(note, t, transpose)
                want_l, want_r = expected_volumes(vel, st, t, prog, header["mvol"])
                if vprog != prog_no or abs(pitch - want_pitch) > want_pitch * 0.006 + 1:
                    problems.append("ch %d note %d at %.3f s: program %d pitch %04X, expected program %d pitch %04X" % (
                        ch, note, w[0] / 1e6, vprog, pitch, prog_no, want_pitch))
                elif abs(voll - want_l) > 2 or abs(volr - want_r) > 2:
                    problems.append("ch %d note %d at %.3f s: volume %d/%d, expected %d/%d" % (
                        ch, note, w[0] / 1e6, voll, volr, want_l, want_r))
        if len(problems) >= limit:
            break
    return problems


# ---------------------------------------------------------------------------------------------------- writing SEQs

def varlen(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.insert(0, (v & 0x7F) | 0x80)
        v >>= 7
    return bytes(out)


def seq_file(body, res=480, tempo=500000, ident=b"SEQp", version16=False, end=True):
    """A SEQ: body = the events' bytes (each with its delta time, see ev); FF 2F 00 added (end)."""
    head = ident + (struct.pack(">H", 1) if version16 else struct.pack(">I", 1))
    head += struct.pack(">H", res) + struct.pack(">I", tempo)[1:] + b"\x04\x02"
    return head + body + (b"\x00\xFF\x2F\x00" if end else b"")


def sep_file(seqs):
    """A SEP of [(res, tempo, body)]."""
    out = b"SEQp" + b"\x00\x00"
    for i, (res, tempo, body) in enumerate(seqs):
        data = body + b"\x00\xFF\x2F\x00"
        out += struct.pack(">HH", i, res) + struct.pack(">I", tempo)[1:] + b"\x04\x02" + struct.pack(">I", len(data))
        out += data
    return out


def ev(delta, *data):
    return varlen(delta) + bytes(data)


# ---------------------------------------------------------------------------------------------------- test files

def corpus_dirs():
    """Folders of SEQ / SEP / VAB test files: PS1_SOUND_CORPUS (separated by ;), else none."""
    return [d for d in os.environ.get("PS1_SOUND_CORPUS", "").split(";") if d and os.path.isdir(d)]


def _bank_for(seq_path):
    """(vh, vb) of a SEQ's bank: same name, else the folder's only bank (a .VAB split in two, in TMP)."""
    folder = os.path.dirname(seq_path)
    base = os.path.splitext(os.path.basename(seq_path))[0].lower()
    files = {os.path.basename(f).lower(): f for f in glob.glob(os.path.join(folder, "*"))}
    vhs = sorted(f for f in files if f.endswith(".vh") and f[:-3] + ".vb" in files)
    vabs = sorted(f for f in files if f.endswith(".vab"))
    for name in vhs:
        if name[:-3] == base:
            return files[name], files[name[:-3] + ".vb"]
    for name in vabs:
        if name[:-4] == base:
            return _split_vab(files[name])
    if vhs:
        return files[vhs[0]], files[vhs[0][:-3] + ".vb"]
    if vabs:
        return _split_vab(files[vabs[0]])
    return None, None


def _split_vab(path):
    with open(path, "rb") as f:
        d = f.read()
    v = vabmod.Vab(d)
    os.makedirs(TMP, exist_ok=True)
    base = os.path.join(TMP, os.path.basename(os.path.dirname(path)) + "_" + os.path.basename(path))
    with open(base + ".vh", "wb") as f:
        f.write(d[:v.vh_size()])
    with open(base + ".vb", "wb") as f:
        f.write(d[v.vh_size():])
    return base + ".vh", base + ".vb"


def corpus():
    """[(seq path, vh, vb)] of the test files."""
    out = []
    for d in corpus_dirs():
        for root, _, names in os.walk(d):
            for n in sorted(names):
                if n.lower().endswith((".seq", ".sep")):
                    path = os.path.join(root, n)
                    out.append((path,) + _bank_for(path))
    return out
