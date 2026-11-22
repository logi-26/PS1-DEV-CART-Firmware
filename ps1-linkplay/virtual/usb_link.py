"""Red Alert Retaliation link play between two PS1 DEV CART consoles, without a link cable.

  python usb_link.py BEN BILL

1. Installs the virtual link driver (vlink_hw.json, built by build_hw.py from vlink.c) in each console's game, through
   "ps1transfer -memlive": its code in three gaps of the game's serial library that the patch makes dead, its rings in
   the kernel hook's cheat table space (after its magic word: the code engine stays off). Nothing of the game's own
   RAM is used. Only with the game running (not at the cartridge menu, whose code would be there too), its code the
   expected one (SLES_013.43; SLES_113.43 has the same code), and the cheat table space unused. Never over an earlier
   version of the driver (the game may be waiting inside it): that console needs a reset first.
2. Then runs "ps1transfer -n A -vlink B" (ps1transfer/ps1transfer.exe of this repository), which carries the rings
   between the two consoles: one USB transfer per console and turn. If a console is reset (its driver gone), back to 1.
Choose LINK GAME on both consoles once it says they are ready. Log: usb_link.log. Ctrl+C stops it.
"""
import argparse, hashlib, json, os, struct, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
MEMLIVE_EXE = r"C:\ps1shots\release\PS1-DEV-CART-1.4\ps1transfer.exe"
RELAY_EXE = os.path.join(REPO, "ps1transfer", "ps1transfer.exe")       # (with -vlink)
with open(os.path.join(HERE, "vlink_hw.json")) as _f:
    HW = json.load(_f)
BUF, RX_SIZE, TX_SIZE = HW["buf"], HW["rx_size"], HW["tx_size"]
SIZE = 112 + RX_SIZE + TX_SIZE         # magic, rx, the carrier's counters (36), the console's (40), tx, stats (vlink.c)
TUNE = os.path.join(HERE, "vlink_tune.txt")     # "drain_min N": read by ps1transfer -vlink, changed live
MAGIC = 0x4B4E4C56
GAPS = [(g["adrs"], bytes.fromhex(g["hex"]), g["size"], g["orig_sha1"]) for g in HW["gaps"]]
# Single instructions of the game (the in-game exchange made symmetric, see vlink.c vlink_free_write)
WORDS = [(w["adrs"], struct.pack("<I", w["orig"]), struct.pack("<I", w["new"])) for w in HW.get("words", [])]
# The same for the relay, which writes them only during a game ("sym 1" in vlink_tune.txt)
SYM_WORDS = ",".join("%08X:%08X:%08X" % (w["adrs"], w["orig"], w["new"]) for w in HW.get("words", []))
PATCHES = [(e["patch"], e["orig"], e["target"]) for e in
           (HW["entries"][k] for k in ("vlink_control", "vlink_read", "vlink_write"))]
CHEAT_TABLE = 0x8000D800                # (its magic "CHTS" there: cheats on, the space in use)
DISC_ID = 0x8000DEE0                    # "DISC" + the booted executable's name, written by the cartridge
ENTRY = (0x80154E4C, "1b80023c305542242080033cd0886324")       # (the exe is there)
OLD_CODE, OLD_END = 0x801F3000, 0x801F4868      # The first version: in the game's RAM (BILL's screen went blank)

logf = open(os.path.join(HERE, "usb_link.log"), "a", buffering=1)
T0 = time.time()


def say(text):
    line = "%s %8.3f  %s" % (time.strftime("%H:%M:%S"), time.time() - T0, text)
    logf.write(line + "\n")
    print(line, flush=True)


class Memlive:
    """One cartridge's "ps1transfer -memlive": read / write RAM while the game runs."""

    def __init__(self, name):
        self.name = name
        self.p = subprocess.Popen([MEMLIVE_EXE, "-n", name, "-memlive"], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        line = self._line()
        while not line.startswith("MEMLIVE ready"):
            if line.startswith("MEMLIVE disconnected"):
                raise IOError("%s: cartridge not answering" % name)
            line = self._line()
        self.running = line.split()[-1] == "1"

    def _line(self):
        line = self.p.stdout.readline()
        if not line:
            raise IOError("%s: ps1transfer ended" % self.name)
        return line.strip()

    def _cmd(self, cmd, answer):
        self.p.stdin.write(cmd + "\n")
        self.p.stdin.flush()
        while True:
            line = self._line()
            if line.startswith(answer):
                return line
            if line.startswith("MEMERR") or line.startswith("MEMLIVE disconnected"):
                raise IOError("%s: %s (%s)" % (self.name, line, cmd[:40]))

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
        except (OSError, subprocess.TimeoutExpired):
            self.p.kill()


def jump(target):
    return struct.pack("<II", 0x08000000 | ((target >> 2) & 0x3FFFFFF), 0)


def target(word8):
    w = struct.unpack("<I", word8[:4])[0]
    return 0x80000000 | ((w & 0x3FFFFFF) << 2) if (w >> 26 == 2 and word8[4:] == bytes(4)) else None


def in_ours(word8):
    """A patch word jumping to a driver of ours (any version): into the library's gaps or the first version's RAM."""
    t = target(word8)
    return t is not None and (OLD_CODE <= t < OLD_END or any(a <= t < a + size for a, _, size, _ in GAPS))


class Side:
    def __init__(self, name):
        self.name = name
        self.ml = None
        self.seen = None
        self.last = None
        self.menu_since = 0

    def status(self, text):
        if text != self.last:
            say("%s: %s" % (self.name, text))
            self.last = text

    def connect(self):
        if self.ml is None:
            self.ml = Memlive(self.name)

    def close(self):
        if self.ml:
            self.ml.close()
            self.ml = None

    def try_install(self):
        """'ok' when the driver is ready in this console, else what it waits for."""
        self.connect()
        if not self.ml.running:
            # (booting the game from the menu doesn't end the connection, and "running" comes only when connecting)
            self.seen = None
            if time.time() - self.menu_since > 20:     # (rarely: not while the menu boots the disc)
                self.menu_since = time.time()
                self.close()
                self.connect()
                if self.ml.running:
                    return "a program is running"
            return "at the cartridge menu"
        if self.ml.read(ENTRY[0], 16).hex() != ENTRY[1]:
            self.seen = None
            return "game not there"
        if self.seen is None:
            self.seen = time.time()
        if time.time() - self.seen < 3:
            return "game loading"
        disc = self.ml.read(DISC_ID, 32)
        if disc[:4] == b"DISC":
            self.status("disc %s" % disc[4:].split(b"\0")[0].decode("ascii", "replace"))
        cur = [self.ml.read(a, 8) for a, _, _ in PATCHES]
        mine = all(c == jump(t) for c, (_, _, t) in zip(cur, PATCHES)) and \
            all(self.ml.read(a, len(code)) == code for a, code, _, _ in GAPS) and \
            all(self.ml.read(a, 4) == new for a, _, new in WORDS)
        if mine and struct.unpack("<I", self.ml.read(BUF, 4))[0] == MAGIC:
            return "ok"                 # (this version already there: ps1transfer -vlink carries on from its state)
        ours = all(in_ours(c) for c in cur)
        first = ours and any(OLD_CODE <= target(c) < OLD_END for c in cur)
        if ours:
            # An earlier version: never replaced in a running game (it may be waiting inside it: its rings don't
            # change then, so "idle" can't be told; replacing it under a waiting game hung BILL)
            return "an earlier version of the driver is installed: reset this console (then boot the game) to install"
        else:
            if [1 for c, (_, orig, _) in zip(cur, PATCHES) if c.hex() != orig[:16]]:
                return "game code differs from SLES_013.43: not patched"
            if [1 for a, _, size, sha in GAPS if hashlib.sha1(self.ml.read(a, size)).hexdigest() != sha]:
                return "the library's code differs from SLES_013.43: not patched"
            if [1 for a, was, _ in WORDS if self.ml.read(a, 4) != was]:
                return "the game's exchange differs from SLES_013.43: not patched"
        if struct.unpack("<I", self.ml.read(CHEAT_TABLE, 4))[0] == 0x53544843:
            return "cheats are on (the cheat table space is in use): turn them off"
        if not ours and any(self.ml.read(BUF, SIZE)):
            return "the cheat table space isn't empty: not patched"
        b = bytearray(SIZE)
        struct.pack_into("<I", b, 0, MAGIC)
        struct.pack_into("<I", b, 4 + RX_SIZE + 36 + 24, 1)    # peer_seen
        self.ml.write(BUF, bytes(b))
        for a, code, _, _ in GAPS:      # (dead code until the patches below, or the earlier driver's: idle)
            self.ml.write(a, code)
            if self.ml.read(a, len(code)) != code:
                return "driver didn't read back correctly"
        for a, _, t in PATCHES:
            self.ml.write(a, jump(t))
        for a, _, new in WORDS:         # (the exchange: only used in a link game, not at the menus)
            self.ml.write(a, new)
        if first:
            self.ml.write(OLD_CODE, bytes(OLD_END - OLD_CODE))     # (the game's RAM back as it was: zeros)
            say("%s: the first version's RAM (%08X-%08X) zeroed again" % (self.name, OLD_CODE, OLD_END))
        say("%s: driver installed (%s bytes in the serial library's gaps, rings at %08X)" %
            (self.name, "+".join(str(len(c)) for _, c, _, _ in GAPS), BUF))
        return "ok"


def install_both(sides, stop):
    ready = {s.name: False for s in sides}
    while not stop.is_set() and not all(ready.values()):
        for s in sides:
            if ready[s.name]:
                continue
            try:
                r = s.try_install()
            except IOError as e:
                s.status("%s (reconnecting)" % e)
                s.close()
                time.sleep(1)
                continue
            if r == "ok":
                ready[s.name] = True
                s.status("ready")
            else:
                s.status(r)
        time.sleep(0.5)
    for s in sides:
        s.close()
    return all(ready.values())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("carts", nargs=2, help="the two cartridges' names, e.g. BEN BILL")
    ap.add_argument("--sym", action="store_true",
                    help="the game's exchange made symmetric (vlink.c vlink_free_write): breaks the setup screen's "
                         "last step on consoles (2026-11-13), only for tests")
    a = ap.parse_args()
    if not a.sym:
        WORDS.clear()
    if not os.path.exists(RELAY_EXE):
        sys.exit("%s missing (buildtools/build_ps1transfer.sh)" % RELAY_EXE)
    if os.path.exists(TUNE):            # (a "stop" line left from the last run would stop the relay at once)
        with open(TUNE) as f:
            keep = [l for l in f if not l.startswith("stop")]
        with open(TUNE, "w") as f:
            f.writelines(keep)
    say("start: %s and %s" % tuple(a.carts))
    sides = [Side(n) for n in a.carts]
    stop = threading.Event()
    try:
        while not stop.is_set():
            if not install_both(sides, stop):
                break
            say("both ready: choose LINK GAME on both consoles (ps1transfer -vlink carries the link)")
            p = subprocess.Popen([RELAY_EXE, "-n", a.carts[0], "-vlink", a.carts[1]], stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, bufsize=1,
                                 env=dict(os.environ, VLINK_TUNE=TUNE, VLINK_SYM_WORDS=SYM_WORDS))
            n = 0
            for line in p.stdout:
                line = line.rstrip()
                if not line or line.startswith("PS1 DEV CART Transfer"):
                    continue
                if line.startswith("VLINK") and " B in" in line:
                    n += 1
                    logf.write("%s %8.3f  %s\n" % (time.strftime("%H:%M:%S"), time.time() - T0, line))
                    if n % 5 == 1:          # (every 5 s on the screen, every second in the log)
                        print(line, flush=True)
                elif line.startswith("VTIME"):  # (timing: the log only)
                    logf.write("%s %8.3f  %s\n" % (time.strftime("%H:%M:%S"), time.time() - T0, line))
                else:
                    say(line)
            code = p.wait()
            # Never again by itself: retrying kept reaching a crashed (or booting) console every few seconds
            if code == 2:
                say("a console's driver went (reset?): stopped; start again once both games are at the menu")
            else:
                say("ps1transfer -vlink ended (%d)%s" % (code, "" if code == 0 else ": stopped"))
            break
    except KeyboardInterrupt:
        pass
    stop.set()
    say("stopped")


if __name__ == "__main__":
    main()
