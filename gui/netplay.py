"""
PS1 DEV CART GUI: Net Play - link-cable games played through the cartridges' USB instead of a link cable.

Each supported game has a patch package in data/netplay (written by ps1-linkplay/virtual/build_hw.py): the virtual link
driver (vlink.c) built for that game, put in dead code of the game's serial library, its rings in the kernel hook's
cheat table space. The driver is installed in the running game through "ps1transfer -memlive" (checks first: the
game's code, the driver's places, cheats off; never over another version of the driver, which the game may be waiting
in), then "ps1transfer -vlink" carries the link between the two cartridges (see ps1transfer/usblink.c).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import glob
import hashlib
import json
import os
import struct
import subprocess
import time

from devices import GUI_DIR, NO_WINDOW

NETPLAY_DIR = os.path.join(GUI_DIR, "data", "netplay")
DISC_ID = 0x8000DEE0                    # "DISC" + the booted executable's name, written by the cartridge firmware
CHEAT_TABLE = 0x8000D800                # (its magic "CHTS": cheats on, the space in use)
DRIVER_MAGIC = 0x4B4E4C56               # "VLNK", the driver's rings


def load_patches():
    """The patch packages: [{title, discs, ...vlink_hw.json fields}]."""
    out = []
    for path in sorted(glob.glob(os.path.join(NETPLAY_DIR, "*.json"))):
        try:
            with open(path) as f:
                out.append(json.load(f))
        except (OSError, ValueError):
            pass
    return out


def patch_for(disc, patches=None):
    """The patch package for a disc ID (e.g. "SLES_013.43"), or None."""
    for p in patches if patches is not None else load_patches():
        if disc in p.get("discs", []):
            return p
    return None


class Memlive:
    """One cartridge's "ps1transfer -memlive": read / write RAM while the game runs."""

    def __init__(self, exe, serial):
        self.serial = serial
        self.p = subprocess.Popen([exe, "-n", serial, "-memlive"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, bufsize=1, creationflags=NO_WINDOW)
        line = self._line()
        while not line.startswith("MEMLIVE ready"):
            if line.startswith("MEMLIVE disconnected"):
                self.close()
                raise IOError("%s isn't answering" % serial)
            line = self._line()
        self.running = line.split()[-1] == "1"      # (a program runs: not the cartridge menu)

    def _line(self):
        line = self.p.stdout.readline()
        if not line:
            raise IOError("%s: ps1transfer ended" % self.serial)
        return line.strip()

    def _cmd(self, cmd, answer):
        self.p.stdin.write(cmd + "\n")
        self.p.stdin.flush()
        while True:
            line = self._line()
            if line.startswith(answer):
                return line
            if line.startswith("MEMERR") or line.startswith("MEMLIVE disconnected"):
                raise IOError("%s: %s" % (self.serial, line))

    def read(self, adrs, n):
        out = bytearray()
        while n > 0:
            k = min(n, 4096)
            out += bytes.fromhex(self._cmd("read %08X %d" % (adrs, k), "MEMREAD").split()[2])
            adrs += k
            n -= k
        return bytes(out)

    def write(self, adrs, data):
        for i in range(0, len(data), 4096):
            self._cmd("write %08X %s" % (adrs + i, data[i:i + 4096].hex()), "MEMWROTE")

    def close(self):
        try:
            self.p.stdin.write("quit\n")
            self.p.stdin.flush()
            self.p.wait(3)
        except (OSError, ValueError, subprocess.TimeoutExpired):
            self.p.kill()


def read_disc(ml):
    """The disc the cartridge booted (its executable's name), or None."""
    d = ml.read(DISC_ID, 32)
    return d[4:].split(b"\0")[0].decode("ascii", "replace") if d[:4] == b"DISC" else None


def _jump(target):
    return struct.pack("<II", 0x08000000 | ((target >> 2) & 0x3FFFFFF), 0)


def _target(word8):
    w = struct.unpack("<I", word8[:4])[0]
    return 0x80000000 | ((w & 0x3FFFFFF) << 2) if (w >> 26 == 2 and word8[4:] == bytes(4)) else None


class Installer:
    """The driver of a patch package in one console's game. check() is called again and again until it says "ok"."""

    def __init__(self, exe, serial):
        self.exe = exe
        self.serial = serial
        self.ml = None
        self.seen = None        # When the game's code was first seen (its loading given a few seconds)

    def close(self):
        if self.ml:
            self.ml.close()
            self.ml = None

    def check(self, patches):
        """(state, text, disc): "ok" the driver is there (installed now or already), "wait" not yet (the game isn't
        there, loading...), "no" it can't be (unsupported game, cheats on, another version...)."""
        if self.ml is None:
            self.ml = Memlive(self.exe, self.serial)
        if not self.ml.running:
            self.close()                # (asked again next time: "running" comes only when connecting)
            self.seen = None
            return "wait", "at the cartridge menu: boot the game from it", None
        disc = read_disc(self.ml)
        if not disc:
            return "no", "no game booted from its disc (boot it with the PS1 DEV CART menu, firmware 1.1 or later)", None
        p = patch_for(disc, patches)
        if not p:
            return "no", "%s: not a supported game" % disc, disc
        entry, entry_hex = p["entry"]
        if self.ml.read(entry, 16).hex() != entry_hex:
            self.seen = None
            return "wait", "%s: the game isn't running yet" % disc, disc
        if self.seen is None:
            self.seen = time.time()
        if time.time() - self.seen < 3:
            return "wait", "%s: the game is loading" % disc, disc
        gaps = [(g["adrs"], bytes.fromhex(g["hex"]), g["size"], g["orig_sha1"]) for g in p["gaps"]]
        entries = [(e["patch"], e["orig"], e["target"]) for e in p["entries"].values()]
        words = [(w["adrs"], struct.pack("<I", w["orig"]), struct.pack("<I", w["new"]))
                 for w in p.get("install_words", [])]      # (e.g. the link page's longer wait)
        cur = [self.ml.read(a, 8) for a, _, _ in entries]
        ours = all(any(a <= (_target(c) or 0) < a + size for a, _, size, _ in gaps) for c in cur)
        if ours and all(c == _jump(t) for c, (_, _, t) in zip(cur, entries)) and \
                all(self.ml.read(a, len(code)) == code for a, code, _, _ in gaps) and \
                all(self.ml.read(a, 4) == new for a, _, new in words) and \
                struct.unpack("<I", self.ml.read(p["buf"], 4))[0] == DRIVER_MAGIC:
            apply_keep(self.ml, p)
            return "ok", "%s: ready (the link driver was there)" % disc, disc
        if ours:
            return "no", "%s: another version of the link driver is there: reset this console, boot the game " \
                         "again" % disc, disc
        if [1 for c, (_, orig, _) in zip(cur, entries) if c.hex() != orig[:16]]:
            return "no", "%s: the game's code isn't the expected one" % disc, disc
        if [1 for a, _, size, sha in gaps if hashlib.sha1(self.ml.read(a, size)).hexdigest() != sha] or \
                [1 for a, was, _ in words if self.ml.read(a, 4) != was]:
            return "no", "%s: the game's link code isn't the expected one" % disc, disc
        if struct.unpack("<I", self.ml.read(CHEAT_TABLE, 4))[0] == 0x53544843:
            return "no", "%s: cheats are on (they use the same memory): turn them off" % disc, disc
        size = 112 + p["rx_size"] + p["tx_size"]
        if any(self.ml.read(p["buf"], size)):
            return "no", "%s: the cheat table space isn't empty" % disc, disc
        b = bytearray(size)
        struct.pack_into("<I", b, 0, DRIVER_MAGIC)
        struct.pack_into("<I", b, 4 + p["rx_size"] + 36 + 24, 1)     # peer_seen
        self.ml.write(p["buf"], bytes(b))
        for a, code, _, _ in gaps:      # (dead code until the entry patches below)
            self.ml.write(a, code)
            if self.ml.read(a, len(code)) != code:
                return "no", "%s: the driver didn't read back correctly" % disc, disc
        for a, _, t in entries:
            self.ml.write(a, _jump(t))
        for a, _, new in words:
            self.ml.write(a, new)
        apply_keep(self.ml, p)
        return "ok", "%s: ready (the link driver installed)" % disc, disc


def apply_keep(ml, patch):
    """The package's "keep_steps" once (the relay keeps them while it runs, ps1transfer -vlinkkeep): its "w" words
    checked in order, its "s" words written while they hold (e.g. Red Alert's main menu idle timer, which leaves
    for the intro movie, made a nop when the menu's code is there unchanged)."""
    for step in filter(None, patch.get("keep_steps", "").split(",")):
        kind, words = step[0], [w.split("=") for w in step[1:].split("/")]
        if kind == "w":
            if any(ml.read(int(a, 16), 4) != struct.pack("<I", int(v, 16)) for a, v in words):
                return
        elif kind == "s":
            for a, v in words:
                ml.write(int(a, 16), struct.pack("<I", int(v, 16)))


def keep_words(patch):
    """The kept steps for ps1transfer -vlinkkeep ("" if the game has none)."""
    return patch.get("keep_steps", "")


def sym_words(patch):
    """The symmetric exchange's instructions for ps1transfer -vlinksym ("" if the game has none)."""
    return ",".join("%08X:%08X:%08X" % (w["adrs"], w["orig"], w["new"]) for w in patch.get("words", []))


class LobbyWatcher:
    """The open games of the Net Play lobby (ps1-linkplay/lobby/netplay_server.py), listed without a console: a
    connection of its own that says hello, answers the server's pings ('Y'), measures its own round trip ('T') and
    receives the lists ('G': id, host's name, host's round trip to the server). So the Net Play page shows the games
    and their ping before anything is started (Join then installs the patch, connects the relay and joins).
    Reconnects every few seconds. Called from its thread: on_games(list of dicts: id, name, ping (ms or None: from
    this computer to the host, the host's round trip to the server + this one's), game (title), disc, info (the
    host's game settings, text), status ("waiting": open to join, "in progress": playing, listed only)),
    on_state(text, or None when connected)."""

    VERSION = "VLNK 1"
    PING_EVERY = 2.0

    def __init__(self, server, name, title, on_games, on_state):
        host, _, port = server.rpartition(":")
        self.server = server
        self.addr = (host or "127.0.0.1", int(port) if port.isdigit() else 7031)
        self.hello = "%s\t%s\t%s" % (self.VERSION, name, title)
        self.title = title          # (a server before the game titles in its lists: only this game's offers listed)
        self.on_games, self.on_state = on_games, on_state
        self.stopping = False
        self.sock = None
        self.rtt = None             # This computer's round trip to the server, ms
        self.games = []             # The last list: dicts (id, name, rtt: the host's round trip or -1, game, ...)

    def start(self):
        import threading
        threading.Thread(target=self._run, daemon=True).start()

    def stop(self):
        self.stopping = True
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass

    def _send(self, kind, data=b""):
        if isinstance(data, str):
            data = data.encode("utf-8", "replace")
        self.sock.sendall(bytes((ord(kind), len(data) & 0xFF, len(data) >> 8)) + data)

    def _publish(self):
        out = []
        for g in self.games:
            g = dict(g)
            g["ping"] = g["rtt"] + self.rtt if (g["rtt"] >= 0 and self.rtt is not None) else None
            out.append(g)
        self.on_games(out)

    def _frame(self, kind, body):
        if kind == "G":
            games = []
            for line in filter(None, body.decode("utf-8", "replace").split("\n")):
                parts = line.split("\t") + [""] * 9
                if parts[0].isdigit():
                    # (the status: "waiting" or "in progress"; none from an older server, which lists open games only.
                    # Pad play rooms: "players" "2/4" and the characters taken "0,3")
                    games.append({"id": int(parts[0]), "name": parts[1] or "?",
                                  "rtt": int(parts[2]) if parts[2].lstrip("-").isdigit() else -1,
                                  "game": parts[3] or self.title, "disc": parts[4], "info": parts[5],
                                  "status": parts[6].strip() or "waiting", "players": parts[7].strip(),
                                  "chars": [int(c) for c in parts[8].split(",") if c.strip().isdigit()]})
            self.games = games
            self._publish()
        elif kind == "Y":                       # (the server's ping: sent back)
            self._send("Y", body)
        elif kind == "T":                       # (our own ping back: our round trip)
            try:
                rtt = int((time.monotonic() - float(body.decode())) * 1000)
            except ValueError:
                return
            changed = self.rtt is None or abs(rtt - self.rtt) >= 3
            self.rtt = max(0, rtt)
            if changed:
                self._publish()
        elif kind == "E":
            self.on_state("Lobby: %s" % body.decode("utf-8", "replace"))

    def _run(self):
        import select
        import socket
        while not self.stopping:
            try:
                self.sock = socket.create_connection(self.addr, timeout=5)
                self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                self._send("L", self.hello)
                self.on_state(None)
                buf, next_ping = b"", 0.0
                while not self.stopping:
                    now = time.monotonic()
                    if now >= next_ping:
                        self._send("T", "%.6f" % now)
                        next_ping = now + self.PING_EVERY
                    ready, _, _ = select.select([self.sock], [], [], 0.5)
                    if not ready:
                        continue
                    data = self.sock.recv(65536)
                    if not data:
                        raise OSError("the server closed the connection")
                    buf += data
                    while len(buf) >= 3:
                        n = buf[1] | (buf[2] << 8)
                        if len(buf) < 3 + n:
                            break
                        kind, body, buf = chr(buf[0]), buf[3:3 + n], buf[3 + n:]
                        self._frame(kind, body)
            except OSError as e:
                if not self.stopping:
                    self.games, self.rtt = [], None
                    self.on_games([])
                    self.on_state("Can't reach the lobby server %s (%s)" % (self.server, e))
            finally:
                try:
                    if self.sock:
                        self.sock.close()
                except OSError:
                    pass
            for _ in range(50):
                if self.stopping:
                    return
                time.sleep(0.1)
