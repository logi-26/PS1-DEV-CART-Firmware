"""Pad play relay (games played in lockstep with only the buttons sent: Crash Team Racing), one per console. It joins
the lobby server's pad play room (host or joiner), and when the game starts: installs the driver (the game's package,
data/netplay/crash_team_racing.json, built by ps1-linkplay/padlink/build.py) in the console's game, sets the game up
at its main menu (ctr_package.py), then carries the button frames between the console's driver and the room.
The Net Play page runs it in a thread (Relay, run_relay: statuses through on_status, "start" / "quit" through
commands); for tests from the command line:

  python padrelay.py --lobby HOST:PORT --name NAME (--host MAX --settings JSON | --join ROOM_ID) --char N
                     (--bridge HOST:PORT | --emu DIR) [--delay 2] [--script P] [--control FILE] [--package FILE]

The console: --bridge: "ps1transfer -n CART -xserve PORT" (the kernel hook's exchange over USB); --emu: an emulator
running ps1-linkplay/padlink/padlink_emu.lua (the same exchange through files in DIR). --script P: this player's
buttons scripted (emulator tests: player P's pattern). --control FILE: lines appended: "start" (the host: Start now),
"quit". Prints one status line per event ("STATUS ...").

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import argparse
import json
import os
import queue
import socket
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ctr_package  # noqa: E402

GAME_TITLE = "Crash Team Racing"
VERSION = "VLNK 1"
MAGIC = 0x4B4E4C50
PC_SIZE, PS1_OFF, PS1_SIZE = 0xA0, 0xA0, 0x58
WIN = 16


def say(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


# ---- The console ------------------------------------------------------------------------------------------------
class EmuLink:
    """An emulator running padlink_emu.lua: requests appended to DIR/pl_cmd.txt, answers read from DIR/pl_ans.txt
    (served by the emulator at each vertical blank and each time the driver calls its service, as the kernel hook
    serves USB at each interrupt and in pl_service)."""

    def __init__(self, d):
        self.d = d
        for n in ("pl_cmd.txt", "pl_ans.txt"):
            open(os.path.join(d, n), "w").close()
        self.cmd = open(os.path.join(d, "pl_cmd.txt"), "a")
        self.ans = open(os.path.join(d, "pl_ans.txt"), "r")
        self.seq = 0
        self.partial = ""

    def _ask(self, line, timeout=30.0):
        self.seq += 1
        self.cmd.write("%d %s\n" % (self.seq, line))
        self.cmd.flush()
        end = time.time() + timeout
        while time.time() < end:
            data = self.ans.read()
            if data:
                lines = (self.partial + data).split("\n")
                self.partial = lines.pop()
                for ln in lines:
                    s, _, rest = ln.partition(" ")
                    if s == str(self.seq):
                        return bytes.fromhex(rest) if rest != "ok" else b""
            else:
                time.sleep(0.0005)
        raise IOError("the emulator didn't answer")

    def exchange(self, adrs, wdata, rsize):
        return self._ask("X %x %s %d" % (adrs, wdata.hex() or "-", rsize))


class BridgeLink:
    """A console through "ps1transfer -xserve": [b'X', adrs u32, wsize u16, rsize u16, wdata] -> [ok u8, rdata]."""

    def __init__(self, hostport):
        h, p = hostport.rsplit(":", 1)
        self.s = socket.create_connection((h, int(p)))
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def _recv(self, n):
        b = b""
        while len(b) < n:
            d = self.s.recv(n - len(b))
            if not d:
                raise IOError("ps1transfer closed the bridge")
            b += d
        return b

    def exchange(self, adrs, wdata, rsize):
        self.s.sendall(b"X" + struct.pack("<IHH", adrs, len(wdata), rsize) + wdata)
        if self._recv(1) != b"\x01":
            raise IOError("the exchange failed (USB)")
        return self._recv(rsize)


class Console:
    def __init__(self, link):
        self.link = link

    def read(self, adrs, n):
        out = b""
        while n:
            k = min(n, 1024)
            out += self.link.exchange(adrs, b"", k)
            adrs, n = adrs + k, n - k
        return out

    def write(self, adrs, data):
        for i in range(0, len(data), 1024):
            self.link.exchange(adrs + i, data[i:i + 1024], 0)

    def r32(self, a):
        return struct.unpack("<I", self.read(a, 4))[0]

    def w(self, a, size, v):
        self.write(a, v.to_bytes(size, "little"))


# ---- The lobby --------------------------------------------------------------------------------------------------
class Lobby:
    def __init__(self, hostport, name):
        h, p = hostport.rsplit(":", 1)
        self.s = socket.create_connection((h, int(p)))
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.s.setblocking(False)
        self.buf = b""
        self.send("L", "%s\t%s\t%s" % (VERSION, name, GAME_TITLE))

    def send(self, kind, data=b""):
        if isinstance(data, str):
            data = data.encode("utf-8")
        self.s.setblocking(True)
        self.s.sendall(bytes((ord(kind), len(data) & 0xFF, len(data) >> 8)) + data)
        self.s.setblocking(False)

    def frames(self):
        try:
            while True:
                d = self.s.recv(65536)
                if not d:
                    raise IOError("the lobby server closed the connection")
                self.buf += d
        except (BlockingIOError, InterruptedError):
            pass
        out = []
        while len(self.buf) >= 3:
            n = self.buf[1] | (self.buf[2] << 8)
            if len(self.buf) < 3 + n:
                break
            k, d = chr(self.buf[0]), self.buf[3:3 + n]
            self.buf = self.buf[3 + n:]
            if k == "Y":
                self.send("Y", d)
            else:
                out.append((k, d))
        return out


# ---- The relay --------------------------------------------------------------------------------------------------
class Relay:
    def __init__(self, a):
        self.a = a
        with open(a.package or os.path.join(HERE, "data", "netplay", "crash_team_racing.json")) as f:
            self.pkg = json.load(f)
        self.buf = self.pkg["buf"]
        self.lobby = Lobby(a.lobby, a.name)
        self.console = Console(EmuLink(a.emu) if a.emu else BridgeLink(a.bridge))
        self.members = []
        self.me = self.n = self.seed = None
        self.remote = {}            # (epoch, player 0-based, frame) -> buttons
        self.upto = {}              # (epoch, player) -> the last frame received with all before it
        self.gone = set()           # players (0-based) who left during the game
        self.hashes = {}            # (epoch, frame) -> {player: hash}
        self.sent_upto = {}         # epoch -> last own frame sent
        self.sent_hash = {}         # epoch -> last frame whose hash was sent
        self.desyncs = 0
        self.control_pos = 0
        self.commands = queue.Queue()   # (in the Manager: "start", "quit" put here; the command line: --control)
        self.on_status = None           # (in the Manager: called with each status line, else printed)

    def status(self, text):
        if self.on_status:
            self.on_status(text)
        else:
            print("STATUS " + text, flush=True)

    def control(self):
        out = []
        while not self.commands.empty():
            out.append(self.commands.get())
        if not self.a.control or not os.path.exists(self.a.control):
            return out
        with open(self.a.control) as f:
            f.seek(self.control_pos)
            data = f.read()
            self.control_pos = f.tell()
        return out + [ln.strip() for ln in data.split("\n") if ln.strip()]

    # -- before the game
    def room(self):
        a = self.a
        if a.host:
            self.lobby.send("D", "SCUS_944.26\t" + ctr_package.describe(json.loads(a.settings), a.host))
            self.lobby.send("M", "%d\t%d" % (a.host, a.char))
        else:
            self.lobby.send("K", "%s\t%d" % (a.join, a.char))
        while True:
            for k, d in self.lobby.frames():
                t = d.decode("utf-8", "replace")
                if k == "K":
                    self.members = [ln.split("\t") for ln in t.split("\n") if ln]
                    self.status("ROOM " + " | ".join("%s %s (%s)" % tuple(m) for m in self.members))
                    if a.host and len(self.members) == a.host:
                        self.start()
                elif k == "B":
                    me, n, seed, payload = t.split("\t", 3)
                    self.me, self.n, self.seed = int(me) - 1, int(n), int(seed)
                    self.game = json.loads(payload)
                    self.status("START player %d of %d, seed %d" % (self.me + 1, self.n, self.seed))
                    return
                elif k == "X":
                    raise RelayStop("CLOSED the host left: the game was called off")
                elif k == "E":
                    self.status("ERROR " + t)
                    if not a.host and not self.members:
                        raise RelayStop("ERROR " + t)
            for c in self.control():
                if c == "start" and a.host:
                    self.start()
                elif c == "quit":
                    self.lobby.send("Q")
                    raise RelayStop("")
            time.sleep(0.01)

    def start(self):
        chars = [int(m[2]) for m in self.members]
        settings = json.loads(self.a.settings)
        why = ctr_package.check_start(settings, chars)
        if why:
            self.status("ERROR can't start: " + why)
            return
        self.lobby.send("B", json.dumps({"settings": settings, "chars": chars}))

    # -- the console
    def install(self):
        c, p = self.console, self.pkg
        g = p["gap"]
        code = bytes.fromhex(g["hex"])
        # (the game's code at the patched places, the original or the driver's: waited for while the game boots)
        end, shown = time.time() + 120, False
        while True:
            bad = [(wd["adrs"], now) for wd in p["words"]
                   for now in [c.r32(wd["adrs"])] if now not in (wd["orig"], wd["new"])]
            if not bad:
                break
            if time.time() > end:
                raise RelayStop("ERROR not the expected game (%08X is %08X): boot Crash Team Racing (USA)" % bad[0])
            if not shown:
                self.status("WAITING for the game to boot")
                shown = True
            self.pump_lobby_idle()
            if "quit" in self.control():
                self.lobby.send("Q")
                raise RelayStop("")
            time.sleep(0.5)
        first = c.r32(g["adrs"])
        if first != int.from_bytes(bytes.fromhex(g["hex"])[:4], "little") and first != 0x27BDFF98:
            say("gap's first word %08X" % first)
        c.write(g["adrs"], code)
        area = bytearray(p["buf_size"])
        struct.pack_into("<IBBBBI", area, 0, MAGIC, self.n, self.me, self.a.delay, 0, self.seed)
        struct.pack_into("<4H", area, 0x10, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF)
        c.write(self.buf, bytes(area))
        for wd in p["words"]:
            c.w(wd["adrs"], 4, wd["new"])
        self.status("INSTALLED")

    def at_main_menu(self):
        g = self.console.r32(ctr_package.GGT_PTR)
        if not 0x80000000 <= g < 0x80200000:
            return None
        m1 = self.console.r32(g)
        demo = self.console.read(g + 0x1D32, 1)[0]
        if m1 & ctr_package.MAIN_MENU and not m1 & ctr_package.LOADING and not demo:
            return g
        return None

    def set_up(self):
        c = self.console
        writes = ctr_package.setup_writes(self.game["settings"], self.game["chars"])
        shown = False
        while True:
            g = self.at_main_menu()
            if g:
                break
            if not shown:
                self.status("WAITING for the console's main menu")
                shown = True
            self.pump_lobby_idle()
            if "quit" in self.control():
                self.lobby.send("Q")
                raise RelayStop("")
            time.sleep(0.2)
        flags = 1 | (2 if self.a.script else 0)
        c.w(self.buf + 7, 1, flags)                        # the driver on: fake multitap, races
        for adrs, size, v in writes:
            a = g + adrs[1] if isinstance(adrs, tuple) else adrs
            if size == "mode":
                clear, setb = v
                c.w(a, 4, (c.r32(a) & ~clear | setb) & 0xFFFFFFFF)
            else:
                c.w(a, size, v)
        self.status("SET UP " + ctr_package.describe(self.game["settings"], len(self.game["chars"])))

    def pump_lobby_idle(self):
        for k, d in self.lobby.frames():
            self.on_frame(k, d)

    def on_frame(self, k, d):
        if k == "F":
            for i in range(0, len(d) - 9, 10):
                typ, p, e, f, v = struct.unpack_from("<BBHHI", d, i)
                if typ == 0:
                    self.remote[(e, p, f)] = v & 0xFFFF
                else:
                    self.hashes.setdefault((e, f), {})[p] = v
                    self.check_hash(e, f)
        elif k == "X":
            p, _, name = d.decode("utf-8", "replace").partition("\t")
            if p.isdigit() and int(p) > 0:
                self.gone.add(int(p) - 1)
                self.status("LEFT player %s %s" % (p, name))

    def check_hash(self, e, f):
        h = self.hashes.get((e, f), {})
        if self.me in h and len(set(h.values())) > 1:
            self.desyncs += 1
            if self.desyncs <= 5:
                self.status("DESYNC race %d frame %d: %s" % (e, f, " ".join("%d=%08x" % kv for kv in sorted(h.items()))))

    def run(self):
        c = self.console
        pc = bytearray(PC_SIZE)
        flags = 1 | (2 if self.a.script else 0)
        last_report, frames0, t0 = time.time(), 0, time.time()
        pc_ready = False
        while True:
            for k, d in self.lobby.frames():
                self.on_frame(k, d)
            for cmd in self.control():
                if cmd == "quit":
                    c.w(self.buf + 7, 1, 0)         # (the driver off: the console plays on by itself)
                    self.lobby.send("Q")
                    raise RelayStop("")
            # one exchange: the computer's part written (from the last turn), the console's part (right after it) read
            ps1 = c.link.exchange(self.buf, bytes(pc), PS1_SIZE) if pc_ready else \
                c.link.exchange(self.buf + PS1_OFF, b"", PS1_SIZE)
            pc_ready = True
            e, fr, upto, state = struct.unpack_from("<HHHH", ps1, 0)
            out = struct.unpack_from("<16H", ps1, 8)
            hashes = struct.unpack_from("<8I", ps1, 0x28)
            if fr == 0xFFFF:
                fr = -1
            recs = b""
            if e:
                last = self.sent_upto.get(e, -1)
                for f in range(max(last + 1, upto - WIN + 1), upto + 1):
                    recs += struct.pack("<BBHHI", 0, self.me, e, f, out[f % WIN])
                self.sent_upto[e] = upto
                lh = self.sent_hash.get(e, -1)
                for f in range(max(lh + 1, fr - 7), fr):
                    if f >= 0:
                        recs += struct.pack("<BBHHI", 1, self.me, e, f, hashes[f % 8])
                        self.hashes.setdefault((e, f), {})[self.me] = hashes[f % 8]
                        self.check_hash(e, f)
                        self.sent_hash[e] = f
            if recs:
                self.lobby.send("F", recs)
            # write the computer's part: the others' windows for race e
            have = [0xFFFF] * 4
            btn = [[0] * WIN for _ in range(4)]
            if e:
                for p in range(self.n):
                    if p == self.me:
                        continue
                    if p in self.gone:
                        have[p] = (fr + WIN - 1) & 0xFFFF if fr >= 0 else WIN - 1
                        continue
                    # (the frames received in a row so far, kept per race and player: older ones get forgotten)
                    f = self.upto.get((e, p), -1)
                    while (e, p, f + 1) in self.remote and f + 1 <= fr + WIN - 1:
                        f += 1
                    self.upto[(e, p)] = f
                    # (frames older than the window are dropped)
                    for k in range(max(0, f - WIN + 1), f + 1):
                        btn[p][k % WIN] = self.remote[(e, p, k)]
                    if f >= 0:
                        have[p] = f
            test = 0
            if self.a.script:
                test = scripted(self.a.script, (fr + 1 + self.a.delay) if fr >= -1 else 0)
            struct.pack_into("<IBBBBIHH", pc, 0, MAGIC, self.n, self.me, self.a.delay, flags, self.seed, e, test)
            struct.pack_into("<4H", pc, 0x10, *have)
            for p in range(4):
                struct.pack_into("<16H", pc, 0x18 + 32 * p, *btn[p])
            # forget old frames
            if fr > 64 and fr % 64 == 0:
                for key in [k for k in self.remote if k[0] < e or (k[0] == e and k[2] < fr - 32)]:
                    del self.remote[key]
            now = time.time()
            if now - last_report >= 5:
                self.status("PLAYING race %d frame %d (%.1f frames/s), waits %d, desyncs %d" %
                            (e, fr, (fr - frames0) / (now - last_report) if fr >= frames0 else 0,
                             struct.unpack_from("<I", ps1, 0x48)[0], self.desyncs))
                last_report, frames0 = now, max(fr, 0)
            if state != 3 and not recs:
                time.sleep(0.002)


class RelayStop(Exception):
    """The relay ends: its last status ("" when asked to)."""


def run_relay(r):
    """The whole relay: the room, then the game. Returns its last status ("" stopped as asked)."""
    try:
        r.room()
        r.install()
        r.set_up()
        r.run()
        return ""
    except RelayStop as e:
        return str(e)
    except (IOError, OSError) as e:
        return "ERROR " + str(e)


def scripted(who, f):
    """The scripted players of the emulator tests (lockstep_n.lua's)."""
    B = {"LEFT": 7, "RIGHT": 5, "O": 13, "X": 14, "R1": 11}
    m = lambda *n: sum(1 << B[x] for x in n)
    if f < 90:
        return 0
    v = m("X")
    k = f // (35 + 10 * who)
    c = k % (3 + who)
    if c == 1:
        v |= m("LEFT")
    elif c == 3:
        v |= m("RIGHT")
    if f % (50 + 10 * who) == 20 * who:
        v |= m("O")
    if who == 1 and f % 150 >= 140:
        v |= m("R1")
    return v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lobby", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--host", type=int, default=0, help="open a room for this many players")
    ap.add_argument("--settings", default="{}")
    ap.add_argument("--join", default="")
    ap.add_argument("--char", type=int, default=0)
    ap.add_argument("--bridge", default="")
    ap.add_argument("--emu", default="")
    ap.add_argument("--delay", type=int, default=2)
    ap.add_argument("--script", type=int, default=0)
    ap.add_argument("--control", default="")
    ap.add_argument("--package", default="")
    a = ap.parse_args()
    last = run_relay(Relay(a))
    if last:
        print("STATUS " + last, flush=True)
    sys.exit(1 if last.startswith("ERROR") else 0)


if __name__ == "__main__":
    main()
