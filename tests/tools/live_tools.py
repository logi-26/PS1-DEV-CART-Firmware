"""
Live test of the tools with a console (nothing written to the cartridge's flash or to a memory card): each tool run as
its Manager page runs it (ps1transfer), what the console gives back checked against what it must be. Every check runs
to its end (a failed one is noted, the next ones still run), then a summary.

    python tests/tools/live_tools.py [--cartridge BILL]

- Firmware: the cartridge's information
- Run PSX, Console & File Server: a program run with its printf and its files from this computer (the movie tester)
- Memory Peek/Poke, Cheats: variables read and written by symbol (peek / poke, the live link), RAM dumped and a value
  searched, the code engine keeping a value
- Screen Capture: a live screenshot
- Sound: SPU RAM Viewer (the sound tester's bank mapped, no false warning), Asset Extractor snapshot (the bank found)
- VRAM Viewer, Hot Reload: a TIM shown, another one sent into VRAM, read back
- Frame Inspector: a frame's GPU packets captured and decoded
- Profiler: a few seconds of measures
- Debug Server: GDB's protocol (stop reason, registers, memory, continue, pause)
- Model & Animation Viewer: a model shown, its view changed
- Synth: a sample loaded and played
- Remote Controller: buttons pressed from here, read in the program's pad buffer
- Memory Card Tools: the card in slot 1 listed (read only)

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import argparse
import json
import os
import queue
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from toolhost import PS1TRANSFER, ROOT, TMP, read, write            # noqa: E402

import assetscan                                                    # noqa: E402
import gpu                                                          # noqa: E402
import spu                                                          # noqa: E402

TOOLS = os.path.join(ROOT, "ps1transfer")
NM = os.path.join(ROOT, "buildtools", "psn00bsdk", "bin", "mipsel-none-elf-nm.exe")
results = []
scenario = [""]


def check(name, ok, detail=""):
    results.append((scenario[0], name, bool(ok), detail))
    print("%-5s %s: %s %s" % ("ok" if ok else "FAIL", scenario[0], name, str(detail)[:300]), flush=True)


def symbol(elf, name):
    for line in subprocess.run([NM, elf], capture_output=True, text=True).stdout.splitlines():
        f = line.split()
        if len(f) == 3 and f[2] == name:
            return int(f[0], 16) & 0xFFFFFFFF
    return None


class Tool:
    """ps1transfer running, its lines read as they come, commands sent on its input."""

    def __init__(self, cart, *args, cwd=None):
        self.p = subprocess.Popen([PS1TRANSFER, "-n", cart] + [str(a) for a in args], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=cwd, bufsize=0)
        self.lines, self.q = [], queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        buf = b""
        while True:
            b = self.p.stdout.read(1)
            if not b:
                break
            if b == b"\n":
                line = buf.replace(b"\0", b"").decode(errors="replace").strip()
                self.lines.append(line)
                self.q.put(line)
                buf = b""
            else:
                buf += b
        self.q.put(None)

    def send(self, line):
        try:
            self.p.stdin.write((line + "\n").encode())
            self.p.stdin.flush()
        except OSError:
            pass

    def wait(self, pattern, timeout=20):
        """The first line (from now) matching pattern (regex, searched in the line), or None."""
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = self.q.get(timeout=max(0.05, end - time.time()))
            except queue.Empty:
                break
            if line is None:
                return None
            if re.search(pattern, line):
                return line
        return None

    def stop(self, quit_line="quit"):
        self.send(quit_line)
        try:
            self.p.stdin.close()
        except OSError:
            pass
        try:
            self.p.wait(8)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait(5)

    def kill(self):
        self.p.kill()
        self.p.wait(5)


def run(cart, *args, timeout=60, cwd=None):
    try:
        r = subprocess.run([PS1TRANSFER, "-n", cart] + [str(a) for a in args], capture_output=True, text=True,
                           timeout=timeout, errors="replace", cwd=cwd)
        return r.returncode, (r.stdout + r.stderr).replace("\0", "")
    except subprocess.TimeoutExpired:
        return None, "timeout"


# ---------------------------------------------------------------------------------------------------- scenarios

def firmware(cart):
    code, out = run(cart, "-info")
    info = dict(l.split("=", 1) for l in out.splitlines() if "=" in l)
    check("information", code == 0 and info.get("device") == cart, {k: info.get(k) for k in ("device", "state", "firmware",
                                                                                             "monitor_version")})
    check("debug monitor", info.get("monitor") == "1", info.get("monitor_version"))


def run_and_memory(cart):
    """The movie tester run with its printf and file server; its variables read and written; the RAM searched; the
    code engine."""
    exe = os.path.join(TOOLS, "videotest.ps-exe")
    elf = os.path.join(TOOLS, "videotest.elf")
    folder = os.path.join(TMP, "live_files")
    os.makedirs(folder, exist_ok=True)
    sys.path.insert(0, os.path.join(ROOT, "tests", "movie"))
    import moviehost
    movie, _ = moviehost.make_movie(os.path.join(folder, "m.str"), w=160, h=112, seconds=2)
    t = Tool(cart, "-r", "-e", "-f", exe, "-c", "-p", folder, cwd=folder)
    ready = t.wait("VIDTEST ready", 30)
    check("Run PSX: the program's printf", ready, ready)
    t.send("open m.str speed 2 mode ram loop 1")
    opened = t.wait("VIDTEST open", 20)
    check("file server: the program read its file", opened and " 2336 " in opened, opened)
    t.kill()                                           # (the program goes on: the next tools attach to it)
    time.sleep(1)
    vol = symbol(elf, "volume")
    code, out = run(cart, "-peek", "@volume", "-s", "4", "-sym", elf)
    check("peek by symbol", code == 0 and re.search(r"%08X\s+64 00 00 00" % vol, out.upper()), out[-200:])
    run(cart, "-poke", "@volume", "37", "-s", "4", "-sym", elf)
    code, out = run(cart, "-peek", "0x%08X" % vol, "-s", "4")
    check("poke", re.search(r"25 00 00 00", out), out[-120:])
    # The live link (Memory page, Cheats page)
    m = Tool(cart, "-memlive")
    check("live link", m.wait("MEMLIVE ready 1", 20))
    m.send("read %08X 4" % vol)
    line = m.wait("MEMREAD", 10)
    check("live read", line and line.upper().endswith("25000000"), line)
    m.send("write %08X 64000000" % vol)
    check("live write", m.wait("MEMWROTE", 10))
    dump = os.path.join(TMP, "ram.bin")
    t0 = time.time()
    m.send("dump 80000000 %d %s" % (0x200000, dump))          # (the size in decimal, as the pages send it)
    line = m.wait("MEMDUMP", 30)
    ram = read(dump) if line and os.path.isfile(dump) else b""
    check("RAM dump (Cheats search)", len(ram) == 0x200000, "%.1f s" % (time.time() - t0))
    if ram:
        hits = [i for i in range(0, len(ram) - 3, 4) if ram[i:i + 4] == b"\x64\0\0\0"]
        check("search: the variable among the addresses with its value", (vol & 0x1FFFFF) in hits, "%d hits" % len(hits))
    # The code engine: volume kept at 55 by an 80 code, even when the program's value is written
    table = struct.pack("<II", 0x53544843, 1) + struct.pack("<IHH", 0x80000000 | (vol & 0x1FFFFF), 55, 0)
    m.send("write 8000D800 %s" % table.hex().upper())
    m.wait("MEMWROTE", 10)
    time.sleep(0.3)
    m.send("write %08X 64000000" % vol)
    m.wait("MEMWROTE", 10)
    time.sleep(0.3)
    m.send("read %08X 4" % vol)
    line = m.wait("MEMREAD", 10)
    check("code engine keeps the value", line and line.upper().endswith("37000000"), line)
    m.send("write 8000D800 0000000000000000")
    m.wait("MEMWROTE", 10)
    m.send("write %08X 64000000" % vol)
    m.wait("MEMWROTE", 10)
    time.sleep(0.3)
    m.send("read %08X 4" % vol)
    line = m.wait("MEMREAD", 10)
    check("code engine off", line and line.upper().endswith("64000000"), line)
    m.stop()
    # Screen Capture: the movie tester's screen, live
    png = os.path.join(TMP, "shot.png")
    code, out = run(cart, "-v", "0", "0", "640", "512", "-f", png)
    ok = code == 0 and os.path.isfile(png) and os.path.getsize(png) > 1000
    if ok:
        from PIL import Image
        im = Image.open(png)
        check("live screenshot", im.size[0] >= 320 and len(set(im.convert("L").getdata())) > 4, im.size)
    else:
        check("live screenshot", False, out[-200:])


def sound(cart):
    """The sound tester with a bank, a note held: the SPU RAM map (samples at 0x1010, the voice in a sample, no false
    warning) and a snapshot (the bank found)."""
    sys.path.insert(0, os.path.join(ROOT, "gui"))
    import audioconv
    import numpy as np
    tones = []
    for p in range(3):
        pcm = (np.sin(np.arange(20000) * (0.03 + p * 0.02)) * 12000).astype(np.int16)
        tones.append(dict(program=p, pcm=pcm, rate=22050, root=60, cents=0, min=0, max=127, vol=100, pan=64,
                          adsr1=0x80FF, adsr2=0x5FC4, reverb=False, loop=p == 1, loop_start=0.1))
    vh, vb = audioconv.build_vab(tones)
    vhp, vbp = write(os.path.join(TMP, "bank.vh"), vh), write(os.path.join(TMP, "bank.vb"), vb)
    t = Tool(cart, "-snd", vhp, "-snd", vbp, "-sndlive")
    check("sound tester", t.wait("SNDLIVE ready", 30))
    t.send("program 1")
    t.send("play 60")
    time.sleep(1.0)
    t.kill()                                            # (the note goes on: the program isn't told to stop)
    time.sleep(0.5)
    dump = os.path.join(TMP, "spu.bin")
    code, out = run(cart, "-spu", dump)
    if "No program runs" in out:        # (ps1transfer killed in the middle of a transfer: once more)
        time.sleep(2)
        code, out = run(cart, "-spu", dump)
    if code != 0 or not os.path.isfile(dump):
        check("SPU RAM read", False, out[-200:])
        return
    m = spu.Spu(read(dump))
    samples = [r for r in m.regions if r.kind == "sample"]
    check("SPU map: the bank's 3 samples", len(samples) >= 3 and samples[0].start in (0x1000, 0x1010),
          [(hex(r.start), r.size) for r in samples[:4]])
    playing = [v for v in m.voices if v.active]
    check("a voice playing a sample", playing and all(m.region_at(v.start).kind == "sample" for v in playing),
          [(v.n, hex(v.start)) for v in playing])
    # (SPU RAM keeps what earlier programs left above the bank: only the bank's samples and the voice's are checked)
    bank_end = 0x1010 + len(vb)
    warnings = [w for r in m.regions if r.start < bank_end or any(m.region_at(v.start) is r for v in playing)
                for w in r.warnings]
    check("no false warning on the bank", not warnings, warnings[:3])
    snap = os.path.join(TMP, "snapshot")
    code, out = run(cart, "-snapshot", snap, timeout=120)
    if code == 0:
        assets = assetscan.Snapshot(snap).scan()
        vabs = [a for a in assets if a.kind in ("VAB", "VH")]
        check("snapshot: the bank found", any(a.kind == "VAB" for a in vabs),
              [(a.kind, hex(a.offset), a.info) for a in vabs[:3]])
        check("snapshot: its samples in SPU RAM", any(a.kind == "Sample" for a in assets))
    else:
        check("snapshot", False, out[-200:])


def vram(cart):
    """A TIM shown by the TIM viewer, another one sent (Hot Reload), read back from VRAM."""
    sys.path.insert(0, os.path.join(ROOT, "tests", "tools"))
    import random
    import test_assets
    rng = random.Random(5)
    a, _ = test_assets.make_tim(rng, 2, 64, 64, x=640, y=0)
    b, _ = test_assets.make_tim(rng, 2, 32, 32, x=704, y=128)
    pa, pb = write(os.path.join(TMP, "a.tim"), a), write(os.path.join(TMP, "b.tim"), b)
    code, out = run(cart, "-r", "-tim", pa, timeout=60)
    time.sleep(2)
    code, out = run(cart, "-hot", pb)
    check("hot reload sent", code == 0, out[-200:])
    png = os.path.join(TMP, "vram.png")
    code, out = run(cart, "-v", "704", "128", "32", "32", "-f", png)
    if code == 0 and os.path.isfile(png):
        from PIL import Image
        got = Image.open(png).convert("RGB")
        px = struct.unpack("<%dH" % (32 * 32), b[20:20 + 32 * 32 * 2])
        bad = sum(1 for i, v in enumerate(px) if tuple(c >> 3 for c in got.getpixel((i % 32, i // 32))) !=
                  (v & 31, (v >> 5) & 31, (v >> 10) & 31))
        check("the TIM in VRAM", bad == 0, "%d pixels differ" % bad)
    else:
        check("VRAM read", False, out[-200:])


def frame(cart):
    # (the control tester: a program making file server requests while captured stalls the session, see the README)
    exe = os.path.join(TOOLS, "padtest.ps-exe")
    draw = symbol(os.path.join(TOOLS, "padtest.elf"), "DrawOTag")
    t = Tool(cart, "-frame", "-f", exe, "-progress")
    check("frame capture session", t.wait("FRMLIVE ready", 30))
    time.sleep(2)
    t.send(("capture %X" % draw if draw else "capture -") + "	" + os.path.join(TMP, "frame.bin"))
    end = t.wait("FRMEND|FRMERR", 30)
    pkts = [l for l in t.lines if l.startswith("FRMPKT")]
    prims = []
    for l in pkts:
        f = l.split()
        prims += gpu.decode(int(f[1]), int(f[2], 16), [int(v, 16) for v in f[3:]])
    kinds = {p.kind for p in prims}
    check("a frame captured", end and end.startswith("FRMEND") and pkts, end)
    check("its packets decoded", "rect" in kinds or "poly" in kinds, sorted(kinds))
    env = [l for l in t.lines if l.startswith("FRMENV")]
    check("draw area and offset", env and len(env[-1].split()) == 7, env[-1:] if env else None)
    t.stop()


def profiler(cart):
    # (the control tester: a program making file server requests while profiled stalls the profiler, see the README)
    exe = os.path.join(TOOLS, "padtest.ps-exe")
    t = Tool(cart, "-prof", "-json", "-e", "-f", exe, "-progress")
    lines = []
    end = time.time() + 12
    while time.time() < end and len(lines) < 3:
        line = t.wait("^PROF ", 6)
        if line:
            lines.append(line)
    t.kill()
    parsed = []
    for l in lines:
        try:
            parsed.append(json.loads(l[5:]))
        except ValueError:
            pass
    check("profiler measures", len(parsed) >= 2, lines[:1])
    if parsed:
        check("its fields", all(isinstance(p, dict) and p for p in parsed), sorted(parsed[-1])[:10])


def gdb(cart):
    exe = os.path.join(TOOLS, "padtest.ps-exe")
    t = Tool(cart, "-gdb", "3339", "-f", exe)
    t.wait("3339|GDB|gdb", 30)
    time.sleep(1)

    def packet(s, data):
        body = data.encode()
        s.sendall(b"$" + body + b"#%02x" % (sum(body) & 0xFF))
        out = b""
        end = time.time() + 15
        while time.time() < end:
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            out += chunk
            m = re.search(rb"\$([^#]*)#..", out)
            if m:
                s.sendall(b"+")
                return m.group(1).decode(errors="replace")
        return None
    try:
        s = socket.create_connection(("127.0.0.1", 3339), timeout=15)
        s.settimeout(15)
        stop = packet(s, "?")
        check("GDB: stop reason", stop and stop[:1] in "ST", stop)
        regs = packet(s, "g")
        check("registers", regs and len(regs) >= 38 * 8, len(regs or ""))
        mem = packet(s, "m80010000,10")
        check("memory read", mem and len(mem) == 32 and re.fullmatch("[0-9a-f]+", mem), mem)
        s.sendall(b"$c#63")
        time.sleep(1.5)
        s.sendall(b"\x03")
        out = b""
        end = time.time() + 10
        while time.time() < end and not re.search(rb"\$[ST][^#]*#", out):
            try:
                out += s.recv(4096)
            except socket.timeout:
                break
        check("continue, then pause", re.search(rb"\$[ST][^#]*#", out), out[-60:])
        s.sendall(b"+$D#44")
        time.sleep(0.5)
        s.close()
    except OSError as e:
        check("GDB connection", False, e)
    t.kill()


def model(cart):
    sys.path.insert(0, os.path.join(ROOT, "tests", "tools"))
    import random
    import test_assets
    tmd = write(os.path.join(TMP, "model.tmd"), test_assets.make_tmd(random.Random(1), nv=8, npr=8))
    t = Tool(cart, "-r", "-m", tmd, "-mdllive")
    check("model viewer", t.wait("MDLLIVE ready", 40))
    st = t.wait("MDLSTATE", 10)
    check("its state", st, st)
    t.send("rot 100 200 300")
    st = t.wait(r"MDLSTATE .* 100 200 300 ", 10)
    check("view changed from here", st, st)
    t.stop()


def synth(cart):
    sys.path.insert(0, os.path.join(ROOT, "gui"))
    import audioconv
    import numpy as np
    vag = write(os.path.join(TMP, "tone.vag"), audioconv.to_vag((np.sin(np.arange(22050) * 0.06) * 12000).astype(
        np.int16), 22050, name="tone"))
    t = Tool(cart, "-r", "-synth", "-progress")              # (as the Manager runs it: progress on lines of their own)
    check("synth", t.wait("^ready$", 40))
    t.send("load 1 %s" % vag)
    check("sample loaded", t.wait("^ok|^error", 20) == "ok")
    t.send("midi 90 3c 64")
    time.sleep(0.3)
    t.send("state")
    t.wait("^end", 10)
    slot = [l for l in t.lines if l.startswith("slot 1 ") or l.startswith("slot 1\t")]
    check("slot 1 playing", slot and "tone" in slot[-1], slot[-1:])
    t.send("midi 80 3c 00")
    t.stop()


def pad(cart):
    t = Tool(cart, "-r", "-padtest")
    time.sleep(4)
    t.kill()
    m = Tool(cart, "-memlive")
    m.wait("MEMLIVE ready", 20)
    m.send("padstate")
    st = m.wait("MEMPAD", 10)
    f = st.split() if st else []
    check("remote pad ready", len(f) >= 4 and f[1] == "1" and int(f[2], 16), st)
    if len(f) >= 4 and int(f[2], 16):
        buf = int(f[2], 16)
        for bits, name in ((0x0040, "Cross"), (0x1000 | 0x0008, "Up + R1"), (0, "none")):
            m.send("pad %X" % bits)
            time.sleep(0.4)
            m.send("read %08X 4" % buf)
            line = m.wait("MEMREAD", 10)
            data = bytes.fromhex(line.split()[-1]) if line else b""
            want = bytes((0, 0x41, ~bits >> 8 & 0xFF, ~bits & 0xFF))
            check("pad %s in the program's buffer" % name, data == want, (data.hex(), want.hex()))
        m.send("pad off")
    m.stop()


def card(cart):
    code, out = run(cart, "-mclist", "slot1", timeout=90)
    ok = code == 0 and re.search(r"save\(s\), \d+/15 blocks used", out)
    no_card = re.search(r"[Nn]o (memory )?card|not formatted|no answer", out)
    check("memory card listed (read only)", ok or no_card, out.strip().splitlines()[-1:] if out.strip() else out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cartridge", default="BILL")
    args = ap.parse_args()
    cart = args.cartridge
    os.makedirs(TMP, exist_ok=True)
    for name, fn in (("Firmware", firmware), ("Run, memory, cheats, screen", run_and_memory), ("Sound", sound),
                     ("VRAM, hot reload", vram), ("Frame Inspector", frame), ("Profiler", profiler),
                     ("Debug Server", gdb), ("Model viewer", model), ("Synth", synth), ("Remote Controller", pad),
                     ("Memory card", card)):
        scenario[0] = name
        try:
            fn(cart)
        except Exception:
            check("ran", False, traceback.format_exc())
        run(cart, "-r", timeout=30)
        time.sleep(2)
    bad = [r for r in results if not r[2]]
    print("\n%d checks, %d failed" % (len(results), len(bad)))
    for r in bad:
        print("  FAIL %s: %s %s" % (r[0], r[1], str(r[3])[:300]))


if __name__ == "__main__":
    main()
