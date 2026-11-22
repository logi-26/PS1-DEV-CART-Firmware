"""Pad play rooms in netplay_server.py, without consoles: a server on a spare port and four scripted clients.
  python room_test.py
Checks: a room opened for 4, joins with free characters (a taken one refused), the list's players / characters,
"Start now" refused alone and accepted with 3 players (each told its player number, the same seed and setup),
frames forwarded to the others only, a player leaving mid-game told to the rest, a Red Alert offer still listed."""
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = 7099


class Cli:
    def __init__(self, name, game="Crash Team Racing"):
        self.s = socket.create_connection(("127.0.0.1", PORT))
        self.s.settimeout(0.05)
        self.buf = b""
        self.got = []
        self.name = name
        self.send("L", "VLNK 1\t%s\t%s" % (name, game))

    def send(self, kind, data=b""):
        if isinstance(data, str):
            data = data.encode()
        self.s.sendall(bytes((ord(kind), len(data) & 0xFF, len(data) >> 8)) + data)

    def pump(self):
        try:
            while True:
                d = self.s.recv(65536)
                if not d:
                    break
                self.buf += d
        except socket.timeout:
            pass
        while len(self.buf) >= 3:
            n = self.buf[1] | (self.buf[2] << 8)
            if len(self.buf) < 3 + n:
                break
            k, d = chr(self.buf[0]), self.buf[3:3 + n]
            self.buf = self.buf[3 + n:]
            if k == "Y":
                self.send("Y", d)
            else:
                self.got.append((k, d))

    def last(self, kind):
        self.pump()
        for k, d in reversed(self.got):
            if k == kind:
                return d.decode("utf-8", "replace") if kind != "F" else d
        return None

    def all(self, kind):
        self.pump()
        return [d for k, d in self.got if k == kind]

    def id(self):
        return int(self.last("I"))


def settle(clients, t=0.3):
    end = time.time() + t
    while time.time() < end:
        for c in clients:
            c.pump()
        time.sleep(0.02)


def check(what, ok):
    print("%s  %s" % ("ok  " if ok else "FAIL", what))
    if not ok:
        check.failed = True


check.failed = False


def main():
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, "..", "netplay_server.py"), "--port", str(PORT)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(1.0)
    try:
        host, j1, j2, j3 = Cli("BEN"), Cli("BILL"), Cli("TED"), Cli("ZED")
        ra = Cli("RA", "Command & Conquer: Red Alert - Retaliation")
        cl = [host, j1, j2, j3, ra]
        settle(cl)
        ra.send("O")
        host.send("D", "SCUS_944.26\tBattle, Skull Rock, 10 points")
        host.send("M", "4\t3")
        settle(cl)
        rid = host.id()
        check("host told the room", host.last("K") == "1\tBEN\t3")
        lst = j1.last("G")
        row = [r for r in lst.split("\n") if r.startswith("%d\t" % rid)]
        check("room listed waiting 1/4 with character 3", bool(row) and row[0].split("\t")[6:] == ["waiting", "1/4", "3"])
        check("Red Alert offer still listed (7 fields)", any(r.split("\t")[1] == "RA" and len(r.split("\t")) == 7
                                                            for r in lst.split("\n")))
        host.send("B", "setup")
        settle(cl)
        check("start alone refused", "at least 2" in (host.last("E") or ""))
        j1.send("K", "%d\t3" % rid)
        settle(cl)
        check("taken character refused", "taken" in (j1.last("E") or ""))
        j1.send("K", "%d\t5" % rid)
        j2.send("K", "%d\t0" % rid)
        settle(cl)
        check("room members", host.last("K") == "1\tBEN\t3\n2\tBILL\t5\n3\tTED\t0")
        row = [r for r in j3.last("G").split("\n") if r.startswith("%d\t" % rid)]
        check("list 3/4, characters 3,5,0", bool(row) and row[0].split("\t")[7:] == ["3/4", "3,5,0"])
        host.send("B", "{mode='battle', players=3}")
        settle(cl)
        b = [c.last("B") for c in (host, j1, j2)]
        check("each told its player number", [x.split("\t")[0] for x in b if x] == ["1", "2", "3"])
        check("same players, seed, setup", len({tuple(x.split("\t")[1:]) for x in b if x}) == 1 and
              b[0].split("\t")[1] == "3" and b[0].endswith("{mode='battle', players=3}"))
        row = [r for r in j3.last("G").split("\n") if r.startswith("%d\t" % rid)]
        check("started room listed in progress", bool(row) and row[0].split("\t")[6] == "in progress")
        j3.send("K", "%d\t7" % rid)
        settle(cl)
        check("joining a started room refused", "isn't open" in (j3.last("E") or ""))
        j1.send("F", b"\x01\x00\x02\x05\x00\x40\x00")
        settle(cl)
        check("frame to the others", host.last("F") == b"\x01\x00\x02\x05\x00\x40\x00" and
              j2.last("F") == b"\x01\x00\x02\x05\x00\x40\x00" and j1.last("F") is None)
        check("no frames to outsiders", j3.last("F") is None and ra.last("F") is None)
        j2.send("Q")
        settle(cl)
        check("leaving player told to the rest", host.last("X") == "3\tTED" and j1.last("X") == "3\tTED")
        j1.s.close()
        settle([host, j3, ra], 0.5)
        check("disconnected player told", host.last("X") == "2\tBILL")
    finally:
        srv.terminate()
        out = srv.communicate()[0]
    print("--- server log\n" + out)
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    main()
