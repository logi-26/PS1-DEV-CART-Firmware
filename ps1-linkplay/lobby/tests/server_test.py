"""
Net Play lobby server on its own (no consoles): the lobby (offer, list, join, the relay frames forwarded, leave) and
the limits it has for the internet (the NAS): the player's address from the proxy's PROXY protocol line, connections
per address, bytes a second, idle connections, texts without control characters.

    python server_test.py          (starts its own servers on ports 7131-7134; prints OK or the failures)

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import socket
import subprocess
import sys
import time

SERVER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "netplay_server.py")
failures = []


def check(ok, text):
    print(("ok    " if ok else "FAIL  ") + text)
    if not ok:
        failures.append(text)


def start(port, *args):
    p = subprocess.Popen([sys.executable, "-u", SERVER, "--port", str(port)] + list(args), stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True)
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            return p
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("server didn't start")


class Half:
    """A client as the relay or the Manager's page: frames type, length (2 bytes), data."""

    def __init__(self, port, name="", game="Red Alert", proxy=None, hello=True):
        self.s = socket.create_connection(("127.0.0.1", port), 2)
        self.s.settimeout(2)
        self.buf = b""
        if proxy:
            self.s.sendall(("PROXY TCP4 %s 192.168.0.33 %d 7031\r\n" % proxy).encode())
        if hello:
            self.send("L", "VLNK 1\t%s\t%s" % (name, game))

    def send(self, kind, data=b""):
        if isinstance(data, str):
            data = data.encode()
        self.s.sendall(bytes((ord(kind), len(data) & 0xFF, len(data) >> 8)) + data)

    def frames(self, wait=0.5):
        """Frames received within wait seconds (the server's pings answered as a client does)."""
        out, end = [], time.time() + wait
        while time.time() < end:
            try:
                self.s.settimeout(max(0.01, end - time.time()))
                d = self.s.recv(65536)
                if not d:
                    out.append(("closed", b""))
                    break
                self.buf += d
            except socket.timeout:
                break
            except OSError:
                out.append(("closed", b""))
                break
            while len(self.buf) >= 3 and len(self.buf) >= 3 + (self.buf[1] | (self.buf[2] << 8)):
                n = self.buf[1] | (self.buf[2] << 8)
                kind, data, self.buf = chr(self.buf[0]), self.buf[3:3 + n], self.buf[3 + n:]
                if kind == "Y":
                    try:
                        self.send("Y", data)
                    except OSError:         # (closed meanwhile)
                        out.append(("closed", b""))
                out.append((kind, data))
        return out

    def last_list(self, wait=0.5):
        lists = [d.decode() for k, d in self.frames(wait) if k == "G"]
        return lists[-1] if lists else None

    def closed(self, wait=1.0):
        return any(k == "closed" for k, _ in self.frames(wait))


def lobby(port):
    a, b = Half(port, "BEN"), Half(port, "BILL")
    a.frames(0.3)
    b.frames(0.3)
    a.send("O")
    rows = (b.last_list() or "").splitlines()
    check(len(rows) == 1 and rows[0].split("\t")[1] == "BEN" and rows[0].endswith("waiting"), "an offer is listed")
    b.send("J", rows[0].split("\t")[0] if rows else "0")
    pa, pb = [d for k, d in a.frames() if k == "P"], [d for k, d in b.frames() if k == "P"]
    check(pa == [b"BILL"] and pb == [b"BEN"], "join pairs the two (each told the other's name)")
    a.send("R", b"\x01\x03" * 1000)
    b.send("S", bytes(16))
    ra, rb = [d for k, d in b.frames() if k == "R"], [d for k, d in a.frames() if k == "S"]
    check(ra == [b"\x01\x03" * 1000] and rb == [bytes(16)], "relay frames forwarded both ways")
    b.send("Q")
    check(any(k == "X" for k, _ in a.frames()), "leaving tells the other")
    evil = Half(port, "evil\nfake\t1\t2", "Red Alert\r\nx")
    evil.send("O")
    rows = (a.last_list() or "").splitlines()
    check(len(rows) == 1 and "\n" not in rows[0].split("\t")[1], "a name with control characters adds no fake row")
    for h in (a, b, evil):
        h.s.close()


def proxy(port):
    log = start_log[port]
    h = Half(port, "PLAYER", proxy=("86.1.2.3", 51000))
    h.frames(0.3)
    time.sleep(0.3)
    check(any("86.1.2.3" in l for l in log.lines()), "the player's address taken from the proxy line")
    bare = Half(port, "NOPROXY")            # (from the proxy's address, but no PROXY line)
    check(bare.closed(6.5), "from the proxy's address without a PROXY line: closed (5 s for the line)")
    hs = [Half(port, "P%d" % i, proxy=("86.9.9.9", 52000 + i)) for i in range(8)]
    for x in hs:
        x.frames(0.1)
    ninth = Half(port, "P9", proxy=("86.9.9.9", 52009))
    check(ninth.closed(), "a 9th connection from one address refused")
    other = Half(port, "OTHER", proxy=("86.7.7.7", 53000))
    check(not other.closed(0.5), "another address still accepted")
    for x in hs + [h, other]:
        x.s.close()


def rate(port):
    h = Half(port, "FLOOD")
    h.frames(0.2)
    try:
        for _ in range(100):
            h.send("R", bytes(60000))       # 6 MB at once (the limit: 64 KB a second, 256 KB at once here)
    except OSError:
        pass
    check(h.closed(2), "a flood of bytes disconnected")
    ok = Half(port, "NORMAL")
    for _ in range(10):
        ok.send("R", bytes(2000))           # (a link game's few KB a second)
        ok.frames(0.1)
    check(not ok.closed(0.3), "a normal rate kept")


def idle(port):
    h = Half(port, "QUIET")
    time.sleep(6.5)                         # (no frame sent, the pings not answered)
    check(h.closed(1.5), "an idle connection closed")
    talk = Half(port, "TALKS")
    for _ in range(9):
        talk.frames(1.0)                    # (answers the pings: every 3 s, under the 5 s)
    check(not talk.closed(0.2), "a client answering the pings kept")


class Log:
    def __init__(self, p):
        self.p, self.buf = p, []
        import threading
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.p.stdout:
            self.buf.append(line.rstrip())

    def lines(self):
        return list(self.buf)


start_log = {}
servers = []
try:
    for port, args, test in ((7131, [], lobby), (7132, ["--proxy-from", "127.0.0.1"], proxy),
                             (7133, ["--rate", "65536", "--burst", "262144"], rate), (7134, ["--idle", "5"], idle)):
        p = start(port, *args)
        servers.append(p)
        start_log[port] = Log(p)
        test(port)
finally:
    for p in servers:
        p.kill()
print("OK" if not failures else "%d FAILED" % len(failures))
sys.exit(1 if failures else 0)
