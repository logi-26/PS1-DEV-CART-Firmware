#!/usr/bin/env python3
"""
PS1 DEV CART Netplay test bot: a pretend player, to test Netplay without a second console.

It joins a Netplay lobby like a guest (from any network: another PC, a laptop on a phone's hotspot, an Android phone
with Termux on mobile data), accepts invites and plays the other console of the link tester: it speaks the link
of "ps1transfer -netlink" (UDP) and answers each packet of the tester at once, so the tester on your console shows
the real round trip and lost packets of the connection. Only Python 3, nothing to install.

    python netplay_bot.py HOST[:PORT] [--name TestBot] [--password X] [--delay MS] [--jitter MS] [--loss PERCENT]
    python netplay_bot.py --host [--port 7010] [--password X] [--hours 3]

--host: the bot hosts the lobby itself (netplay.py next to it) and plays as its host: you join it as a guest, e.g.
from a computer on a VPN (which can't be reached from the internet). The lobby's ports (TCP+UDP 7010, UDP 7011-7014)
must be reachable from the internet. --hours: the bot stops by itself after this time.

On your side (the host): Netplay page, Host; your game: ps1transfer/linktest.ps-exe, link "Net Yaroze game (tty,
-c link)"; invite the bot. --delay/--jitter/--loss make the bot's packets later or lost (worse than the real network).
--rings: pretend to be a psxcomb USB link program instead of a Net Yaroze tty one (for your own homebrew tests, no
answers then: the bot only counts what it receives).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import argparse
import hashlib
import hmac
import json
import os
import random
import select
import socket
import struct
import sys
import threading
import time

LOBBY_VERSION = 2       # 2: password challenge, link keys (netplay.py)
DEFAULT_PORT = 7010

# ps1transfer -netlink (usblink.c)
NL_MAGIC = 0x4C4E5350
NL_VERSION = 1
NL_VERSION_KEYED = 2                            # Packets ending with the tag of the game's link key
LINK_TAG = 8
NL_HELLO, NL_DATA, NL_BYE, NL_PEER = 1, 2, 3, 4
NL_HEADER = struct.Struct("<IBBBBIIIHHIII")     # magic, type, kind, version, flags, session, ack, first, count,
NL_MAX_PACKET = 1200                            # reserved, time, echo, echo_age
KIND_RINGS, KIND_TTY = 0, 1
RESEND_MS, KEEPALIVE_MS, QUIET_MS, TIMEOUT_MS = 10, 250, 3000, 30000
PUNCH_MS, PUNCH_EVERY, DIRECT_LOST = 6000, 50, 1500     # Direct link between two guests (hole punching)

# Link tester packets (ps1-tools/linktest/linktest.c): 0xA5, length, sequence, echo, pattern, checksum
SYNC = 0xA5


def now_ms():
    return time.monotonic() * 1000.0


def link_tag(key, data):
    """First 8 bytes of HMAC-SHA256(key, packet): ends each link packet of a game (netplay.py, usblink.c)."""
    return hmac.new(key, data, hashlib.sha256).digest()[:LINK_TAG]


def session_secret(password, nonce):
    return hmac.new(password.encode("utf-8"), nonce + b"session", hashlib.sha256).digest()


def open_key(start, secret):
    """The game's link key from a lobby start message (encrypted when the lobby has a password)."""
    key = bytes.fromhex(start["key"])
    if start.get("enc"):
        pad = hmac.new(secret, b"link key %d" % int(start["n"]), hashlib.sha256).digest()[:len(key)]
        key = bytes(a ^ b for a, b in zip(key, pad))
    return key


def tester_packet(seq, echo, size=8):
    p = bytearray(size)
    p[0], p[1] = SYNC, size
    struct.pack_into("<HH", p, 2, seq & 0xFFFF, echo & 0xFFFF)
    for i in range(6, size - 1):
        p[i] = (seq + i * 7) & 0xFF
    p[size - 1] = sum(p[:size - 1]) & 0xFF
    return bytes(p)


class Link:
    """One link game: the other console's computer (or the lobby's relay) at addr, the bot answering its tester."""

    def __init__(self, addr, kind, delay, jitter, loss, log, listen_port=0, key=None):
        self.key = key              # The game's link key: packets tagged, the others ignored
        self.listen = bool(listen_port)    # Lobby host: waits for the guest on this UDP port
        self.peer = addr
        self.relay = addr           # The host, or the lobby's relay
        self.direct = None          # The other guest's address (NL_PEER from the relay)
        self.direct_ok = False
        self.punch_start = self.last_punch = self.last_direct = 0.0
        self.quiet = False
        self.kind = kind
        self.delay, self.jitter, self.loss = delay, jitter, loss
        self.log = log
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", listen_port))
        self.session = random.getrandbits(32)
        self.connected = False
        self.stopped = False
        self.store = {}             # Units sent, kept until acknowledged
        self.send_next = self.acked = self.recv_next = 0
        self.peer_time = 0
        self.peer_time_at = 0.0
        self.last_send = self.last_recv = 0.0
        self.rtt = self.rtt_min = None
        self.delayed = []           # (due, packet)
        # Tester
        self.rx = bytearray()
        self.tx_seq = 0
        self.peer_seq = 0
        self.expected = None
        self.received = self.lost = self.corrupted = 0
        self.units_in = self.units_out = 0
        self.rtts = []

    # --- packets
    def _raw(self, data):
        try:
            self.sock.sendto(data, self.peer)
        except OSError:
            pass

    def _send_packet(self, data):
        if self.loss and random.random() * 100 < self.loss:
            return
        if self.delay or self.jitter:
            due = now_ms() + self.delay + (random.random() * self.jitter if self.jitter else 0)
            if self.delayed and due < self.delayed[-1][0]:
                due = self.delayed[-1][0]       # (in order)
            self.delayed.append((due, data))
        else:
            self._raw(data)

    def send(self, kind=NL_DATA, raw=False):
        now = now_ms()
        units = []
        size = NL_HEADER.size
        if kind == NL_DATA:
            u = self.acked
            while u != self.send_next:
                data = self.store[u]
                if size + 2 + len(data) > NL_MAX_PACKET - LINK_TAG:
                    break
                units.append(struct.pack("<H", len(data)) + data)
                size += 2 + len(data)
                u = (u + 1) & 0xFFFFFFFF
        header = NL_HEADER.pack(NL_MAGIC, kind, self.kind, NL_VERSION_KEYED if self.key else NL_VERSION, 0,
                                self.session, self.recv_next, self.acked, len(units), 0, int(now) & 0xFFFFFFFF,
                                self.peer_time, int(now - self.peer_time_at) if self.peer_time_at else 0)
        self.last_send = now
        packet = header + b"".join(units)
        if self.key:
            packet += link_tag(self.key, packet)
        if raw:
            self._raw(packet)
        else:
            self._send_packet(packet)

    def unit(self, data):
        self.store[self.send_next] = data
        self.send_next = (self.send_next + 1) & 0xFFFFFFFF
        self.units_out += 1
        self.send()

    def receive(self, data, addr):
        if len(data) < NL_HEADER.size:
            return
        (magic, kind, pkind, version, _, session, ack, first, count, _, ptime, echo,
         echo_age) = NL_HEADER.unpack_from(data)
        if magic != NL_MAGIC:
            return
        if self.key:        # Only the packets of this game (the tag isn't part of the units after)
            if version != NL_VERSION_KEYED or len(data) < NL_HEADER.size + LINK_TAG or \
                    not hmac.compare_digest(link_tag(self.key, data[:-LINK_TAG]), data[-LINK_TAG:]):
                return
            data = data[:-LINK_TAG]
        elif version != NL_VERSION:
            return
        if kind == NL_PEER:
            if addr == self.relay and len(data) >= NL_HEADER.size + 6 and not self.direct:
                ip = socket.inet_ntoa(data[NL_HEADER.size:NL_HEADER.size + 4])
                self.direct = (ip, struct.unpack_from("!H", data, NL_HEADER.size + 4)[0])
                self.punch_start = now_ms()
                self.log("Trying a direct link with %s:%d" % self.direct)
            return
        if not self.connected:
            if kind not in (NL_HELLO, NL_DATA):
                return
            if pkind != self.kind:
                self.log("The other console runs another kind of link program (Net Yaroze tty / psxcomb): use %s." %
                         ("--rings" if pkind == KIND_RINGS else "no --rings"))
                self.stopped = True
                return
            self.peer = addr
            self.session = session
            self.connected = True
            self.log("Linked with %s:%d" % addr)
            self.send(NL_HELLO)
        elif addr not in (self.peer, self.relay, self.direct):
            return
        elif session != self.session:
            if kind != NL_HELLO or session > self.session:
                return
            self.session = session
            self.send(NL_HELLO)
        if kind == NL_BYE:
            self.log("The other computer stopped the link.")
            self.stopped = True
            return
        now = now_ms()
        if self.direct and addr == self.direct:
            self.last_direct = now
            if not self.direct_ok:
                self.direct_ok = True
                self.peer = self.direct
                self.log("Direct link with %s:%d (the host's relay isn't used any more)" % addr)
        if self.quiet:
            self.quiet = False
            self.log("The other computer is back.")
        self.last_recv = now
        self.peer_time = ptime
        self.peer_time_at = now
        if echo:
            rtt = now - echo - echo_age
            if 0 <= rtt < 10000:
                self.rtt = rtt if self.rtt is None else self.rtt * 0.8 + rtt * 0.2
                self.rtt_min = rtt if self.rtt_min is None else min(self.rtt_min, rtt)
                self.rtts.append(rtt)
        if 0 < ((ack - self.acked) & 0xFFFFFFFF) <= ((self.send_next - self.acked) & 0xFFFFFFFF):
            while self.acked != ack:
                self.store.pop(self.acked, None)
                self.acked = (self.acked + 1) & 0xFFFFFFFF
        pos = NL_HEADER.size
        got = False
        u = first
        for _ in range(count):
            if pos + 2 > len(data):
                break
            n = struct.unpack_from("<H", data, pos)[0]
            if pos + 2 + n > len(data):
                break
            if u == self.recv_next:
                self.recv_next = (self.recv_next + 1) & 0xFFFFFFFF
                self.units_in += 1
                got = True
                self.on_unit(data[pos + 2:pos + 2 + n])
            pos += 2 + n
            u = (u + 1) & 0xFFFFFFFF
        if got:
            self.send()

    # --- the other console's tester
    def on_unit(self, data):
        if self.kind != KIND_TTY:
            return
        self.rx += data
        while len(self.rx) >= 8:
            size = self.rx[1]
            if self.rx[0] != SYNC or size < 8:
                del self.rx[0]
                self.corrupted += 1
                continue
            if len(self.rx) < size:
                break
            if sum(self.rx[:size - 1]) & 0xFF != self.rx[size - 1]:
                del self.rx[0]
                self.corrupted += 1
                continue
            seq = struct.unpack_from("<H", self.rx, 2)[0]
            if self.expected is not None and seq != self.expected and ((seq - self.expected) & 0xFFFF) < 1000:
                self.lost += (seq - self.expected) & 0xFFFF
            self.expected = (seq + 1) & 0xFFFF
            self.peer_seq = seq
            self.received += 1
            del self.rx[:size]
            # Answer at once: its sequence comes back as our echo (the tester's round trip)
            self.unit(tester_packet(self.tx_seq, self.peer_seq))
            self.tx_seq = (self.tx_seq + 1) & 0xFFFF

    # --- loop
    def run(self, timeout_s=60):
        hello = 0.0
        start = now_ms()
        second = now_ms()
        while not self.stopped:
            if select.select([self.sock], [], [], 0.001)[0]:
                try:
                    data, addr = self.sock.recvfrom(2048)
                    self.receive(data, addr)
                except OSError:
                    pass        # (Windows: the port wasn't open yet on the other side)
            now = now_ms()
            while self.delayed and self.delayed[0][0] <= now:
                self._raw(self.delayed.pop(0)[1])
            if self.direct and not self.direct_ok and now - self.punch_start < PUNCH_MS and \
                    now - self.last_punch >= PUNCH_EVERY:
                saved, self.peer = self.peer, self.direct
                self.send(NL_HELLO, raw=True)
                self.peer = saved
                self.last_punch = now
            if self.direct_ok and now - self.last_direct > DIRECT_LOST:
                self.direct_ok = False
                self.peer = self.relay
                self.punch_start = now
                self.log("Direct link lost: through the host's relay again")
            if not self.connected:
                if not self.listen and now - hello > 250:
                    self.send(NL_HELLO)
                    hello = now
                if now - start > timeout_s * 1000:
                    self.log("No answer from the other console in %d s (port forwarded? game started?)." % timeout_s)
                    break
                continue
            if self.acked != self.send_next and now - self.last_send >= RESEND_MS + (self.rtt or 0):
                self.send()
            elif now - self.last_send >= KEEPALIVE_MS:
                self.send()
            if not self.quiet and now - self.last_recv > QUIET_MS:
                self.quiet = True
                self.log("Nothing from the other computer for %d s: waiting (up to %d s)..." %
                         (QUIET_MS // 1000, TIMEOUT_MS // 1000))
            if now - self.last_recv > TIMEOUT_MS:
                self.log("Nothing from the other computer for %d seconds: link lost." % (TIMEOUT_MS // 1000))
                break
            if now - second >= 1000:
                self.log("link: rtt %s ms (min %s)  tester packets %d received, %d lost  units %d out %d in" %
                         ("%.1f" % self.rtt if self.rtt is not None else "-",
                          "%.1f" % self.rtt_min if self.rtt_min is not None else "-",
                          self.received, self.lost, self.units_out, self.units_in))
                self.units_in = self.units_out = 0
                second = now
        if self.connected and not self.stopped:
            for _ in range(3):
                self.send(NL_BYE, raw=True)
        self.sock.close()

    def summary(self):
        if not self.rtts:
            return "no data received."
        r = sorted(self.rtts)
        return ("%d tester packets received, %d lost, %d corrupted; network round trip median %.0f ms, best %.0f, "
                "worst %.0f." % (self.received, self.lost, self.corrupted, r[len(r) // 2], r[0], r[-1]))


class Lobby:
    def __init__(self, host, port, name, password, log):
        self.host, self.port = host, port
        self.log = log
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.settimeout(None)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.lock = threading.Lock()
        self.alive = True
        self.buf = b""
        self.secret = None
        # The lobby's challenge first: answered with HMAC-SHA256(password, challenge), the password isn't sent
        self.sock.settimeout(10)
        first = next(self.messages(), {})
        self.sock.settimeout(None)
        if first.get("type") != "challenge":
            raise OSError(first.get("text") or "not a Netplay lobby of this version")
        nonce = bytes.fromhex(first["nonce"])
        if password:
            self.secret = session_secret(password, nonce)
        self.send({"type": "hello", "name": name, "version": LOBBY_VERSION,
                   "proof": hmac.new(password.encode("utf-8"), nonce, hashlib.sha256).hexdigest()})
        threading.Thread(target=self._pinger, daemon=True).start()

    def send(self, msg):
        try:
            with self.lock:
                self.sock.sendall((json.dumps(msg) + "\n").encode("utf-8"))
        except OSError:
            self.alive = False

    def _pinger(self):
        while self.alive:
            self.send({"type": "ping", "t": time.time()})
            time.sleep(2)

    def messages(self):
        while self.alive:
            try:
                data = self.sock.recv(4096)
            except OSError:
                data = b""
            if not data:
                self.alive = False
                return
            self.buf += data
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                try:
                    yield json.loads(line.decode("utf-8"))
                except ValueError:
                    pass


def main():
    ap = argparse.ArgumentParser(description="PS1 DEV CART Netplay test bot (pretend player for the link tester)")
    ap.add_argument("lobby", nargs="?", default="", help="host's address, HOST or HOST:PORT (default port %d)" %
                    DEFAULT_PORT)
    ap.add_argument("--host", action="store_true", help="host the lobby here and play as its host")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help="--host: lobby port")
    ap.add_argument("--hours", type=float, default=0, help="stop after this many hours")
    ap.add_argument("--name", default="TestBot")
    ap.add_argument("--password", default="")
    ap.add_argument("--delay", type=float, default=0, help="extra delay of the bot's packets (ms)")
    ap.add_argument("--jitter", type=float, default=0, help="extra random delay 0..JITTER ms")
    ap.add_argument("--loss", type=float, default=0, help="packets dropped (percent)")
    ap.add_argument("--rings", action="store_true", help="pretend to be a psxcomb USB link program")
    args = ap.parse_args()
    host, _, port = args.lobby.rpartition(":") if ":" in args.lobby else (args.lobby, "", "")
    port = int(port) if port else DEFAULT_PORT

    def log(text):
        print(time.strftime("%H:%M:%S ") + text, flush=True)

    if args.hours:
        def stop_later():
            time.sleep(args.hours * 3600)
            log("%g hours: stopping." % args.hours)
            os._exit(0)
        threading.Thread(target=stop_later, daemon=True).start()
    if args.host:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import netplay      # (lobby server, next to this file)
        port = args.port
        netplay.LobbyServer(port, args.password, log=lambda t: log("Lobby: " + t))
        host = "127.0.0.1"
        log("Hosting the lobby on port %d%s" % (port, " (password set)" if args.password else ""))
    elif not host:
        ap.error("the host's address is needed (or --host)")

    while True:
        try:
            lobby = Lobby(host, port, args.name, args.password, log)
        except OSError as e:
            log("Can't reach the lobby %s:%d (%s): trying again in 5 s." % (host, port, e))
            time.sleep(5)
            continue
        log("Connected to the lobby %s:%d" % (host, port))
        lobby.send({"type": "status", "game": "LINK TESTER (BOT)", "hash": ""})
        my_id = None
        for m in lobby.messages():
            kind = m.get("type")
            if kind == "welcome":
                my_id = m["id"]
                lobby.send({"type": "chat", "text": "Test bot here: choose the link tester (linktest.ps-exe, Net "
                                                    "Yaroze tty link) as your game and invite me."})
            elif kind == "error":
                log("Lobby: " + m.get("text", ""))
                if "password" in m.get("text", "") or "version" in m.get("text", ""):
                    return
                if "name" in m.get("text", ""):     # (its old connection may still be there after a dropout)
                    log("Trying again in 8 s (use --name to choose another name).")
                    time.sleep(8)
                    break
            elif kind == "chat" and m.get("from") and m.get("from") != my_id:
                log("%s%s: %s" % (m["name"], " (to me)" if m.get("to") else "", m["text"]))
                if m.get("to") == my_id:
                    lobby.send({"type": "chat", "to": m["from"], "text": "I'm a bot: invite me to test the link."})
            elif kind == "invite":
                log("Invite from %s (%s): accepted" % (m["name"], m["game"]))
                lobby.send({"type": "answer", "invite": m["invite"], "accept": True})
            elif kind == "start":
                addr = (socket.gethostbyname(host), m["port"])
                if m["role"] == "host":
                    log("Game with %s: waiting for it on UDP port %d" % (m["peer"], m["port"]))
                else:
                    log("Game with %s: linking to %s:%d%s" % (m["peer"], addr[0], addr[1],
                                                              " (host's relay)" if m["role"] == "relay" else ""))
                if args.delay or args.jitter or args.loss:
                    log("Bot's packets: +%g ms (+0-%g ms), %g%% lost" % (args.delay, args.jitter, args.loss))
                link = Link(None if m["role"] == "host" else addr, KIND_RINGS if args.rings else KIND_TTY,
                            args.delay, args.jitter, args.loss, log, m["port"] if m["role"] == "host" else 0,
                            open_key(m, lobby.secret))
                link.run()
                text = "Game with %s over: %s" % (m["peer"], link.summary())
                log(text)
                lobby.send({"type": "chat", "text": text})
                lobby.send({"type": "done"})
                lobby.send({"type": "status", "game": "LINK TESTER (BOT)", "hash": ""})
        log("Lobby connection lost: reconnecting in 5 s.")
        time.sleep(5)


if __name__ == "__main__":
    main()
