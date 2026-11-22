"""
PS1 DEV CART Net Play lobby server.

Each player's Manager runs its half of the link relay ("ps1transfer -vlinklobby HOST:PORT"), connected here. While a
player's console waits on the game's link page, its half offers the game; every other half of the same game is sent
the list of open offers; a half that joins one is paired with its host, and from then on the server forwards their
relay frames to each other (the link as with a cable, through this server: no router port needed by the players).
When one of the pair leaves, the other is told and both are back in the lobby.

Frames both ways: type (1 byte), length (2 bytes, little-endian), data.
  from a half: 'L' hello "VLNK 1<TAB>player name<TAB>game title"; 'D' the details for the list "disc<TAB>game
               settings as text" (a separate frame: a server before them refused a longer hello, and ignores it);
               'O' offer (its console waits); 'W' withdraw;
               'J' join "offer id"; 'Q' leave the pair; once paired: 'R' / 'S' (relay records / status) forwarded;
               'Y' the server's ping sent back; 'T' a ping of its own (sent back as it is)
  to a half:   'I' its id in the lobby (its own offer is listed with it); 'G' the lobby's games, of every game
               (a game is only joined by a half of the same game): "id<TAB>name<TAB>host's ping to the server in ms
               (-1 not known)<TAB>game title<TAB>disc<TAB>settings<TAB>status" per line, status "waiting" (open:
               joined with 'J') or "in progress" (its host paired: listed, not joined; the field last, so older
               Managers read the lines as before); 'P' paired "other player's name";
               'X' the other player left; 'E' error text; 'R' / 'S' from its pair; 'Y' ping (sent back); 'T' its
               own ping back
A Manager's Net Play page also connects on its own (no console: only listing the games, and its own ping), so the
ping shown for a game is the host's round trip to the server plus the page's: the path the link takes.

Pad play rooms (2-4 players, games played in lockstep with only the buttons sent, e.g. Crash Team Racing; the
frames above stay as they are for the link games):
  from a half: 'M' open a room "max players<TAB>host's character" (instead of 'O'; its 'D' details as for an offer);
               'K' join a room "room id<TAB>character" (the character must be free); 'B' start (the host only, from 2
               players; its Manager sends it by itself when the room is full) "setup": the host's Manager's game setup for the players
               in the room, given to all; 'F' pad frames / state hashes (binary, the relay's), forwarded to the room's
               other players; 'Q' leave the room
  to a half:   'K' the room "player<TAB>name<TAB>character" per line (player 1 the host, then in joining order), to
               its players whenever it changes; 'B' the start "me<TAB>players<TAB>seed<TAB>setup"; 'F' the others'
               frames; 'X' "player<TAB>name" left (a game started: its kart gets no buttons from then on; before:
               the room's new 'K'); 'E' error text
  The lists ('G') show rooms like offers, with two more fields last: "players/max" and the characters taken
  ("3,5"); status "waiting" (joinable while not full and not started) or "in progress".

  python netplay_server.py [--port 7031] [--proxy-from ADDRESS] [limits, see --help]

Open to the internet (the NAS, behind its nginx proxy: lobby/nas/netplay.sh), so each connection is limited: the
player's real address from the proxy's PROXY protocol line (trusted only from --proxy-from), connections per address
at once and per minute, bytes a second (a link game sends a few KB a second; a console catching up after a stall up to
60 KB at once), a client that stops reading dropped before its queue grows, no frame for --idle seconds (the server
pings every client every 3 seconds: they answer), and the texts shown to others (names, titles, settings) without
control characters (a line break in a name would add a fake game to the others' lists).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import argparse
import asyncio
import itertools
import re
import socket
import time

VERSION = "VLNK 1"
HELLO_TIMEOUT = 10
PING_EVERY = 3                          # (seconds: each client's round trip to the server, for the lists)
WRITE_BUFFER_MAX = 4 << 20              # A client's unsent bytes (it stopped reading): disconnected past this
CONTROL = re.compile(r"[\x00-\x1f\x7f]")


def clean(text, n):
    """A text shown to the other players: no control characters (tabs and line breaks separate the lists' fields
    and lines), at most n characters."""
    return CONTROL.sub(" ", text).strip()[:n]


def log(text):
    print("%s %s" % (time.strftime("%H:%M:%S"), text), flush=True)


class Client:
    ids = itertools.count(1)

    def __init__(self, reader, writer, addr=None):
        self.id = next(Client.ids)
        self.reader, self.writer = reader, writer
        self.name = self.game = None
        self.offering = False
        self.peer = None
        self.host = False               # (paired as the host: its game listed "in progress")
        self.rtt = -1                   # Round trip to the server, ms (-1 not known yet)
        self.disc = self.info = ""      # (its hello: the disc, the game settings it hosts with)
        self.addr = addr or writer.get_extra_info("peername")
        self.tokens = self.burst = 0.0  # (bytes a second it may send: a token bucket, filled by the lobby)
        self.tokens_time = time.monotonic()
        self.room = None                # (pad play: the Room it is in)
        self.char = ""                  # (pad play: its character)

    def send(self, kind, data=b""):
        if isinstance(data, str):
            data = data.encode("utf-8", "replace")
        data = data[:65535]
        try:
            if self.writer.transport.get_write_buffer_size() > WRITE_BUFFER_MAX:
                self.writer.transport.abort()       # (it stopped reading: its queue isn't left to grow)
                return
            self.writer.write(bytes((ord(kind), len(data) & 0xFF, len(data) >> 8)) + data)
        except (ConnectionError, RuntimeError, AttributeError):
            pass

    def label(self):
        return "%d %s (%s)" % (self.id, self.name or "?", self.addr[0] if self.addr else "?")


class Room:
    """A pad play room: its host (players[0]) and the players who joined, in order."""

    def __init__(self, host, max_players):
        self.host = host
        self.max = max(2, min(4, max_players))
        self.players = [host]
        self.started = False

    def open(self):
        return not self.started and len(self.players) < self.max

    def chars(self):
        return [c.char for c in self.players if c.char != ""]

    def send(self, kind, data, but=None):
        for c in self.players:
            if c is not but:
                c.send(kind, data)

    def send_members(self):
        self.send("K", "\n".join("%d\t%s\t%s" % (i + 1, c.name, c.char) for i, c in enumerate(self.players)))


class Lobby:
    def __init__(self, args):
        self.args = args
        self.clients = {}
        self.seeds = itertools.count(int(time.time()) & 0xFFFFFF)
        self.per_ip = {}                # Address -> connections open (before their hello too)
        self.recent = {}                # Address -> times of its connections in the last minute

    def admit(self, ip):
        """A new connection from ip allowed (not too many at once nor in the last minute)?"""
        now = time.monotonic()
        times = self.recent.setdefault(ip, [])
        while times and now - times[0] > 60:
            times.pop(0)
        if sum(self.per_ip.values()) >= self.args.max_clients or self.per_ip.get(ip, 0) >= self.args.per_ip or \
                len(times) >= self.args.connects_per_minute:
            return False
        times.append(now)
        self.per_ip[ip] = self.per_ip.get(ip, 0) + 1
        return True

    def release(self, ip):
        self.per_ip[ip] -= 1
        if not self.per_ip[ip]:
            del self.per_ip[ip]
        if not self.recent.get(ip):
            self.recent.pop(ip, None)

    def offers(self):
        return [c for c in self.clients.values() if c.offering and not c.peer]

    def playing(self):
        return [c for c in self.clients.values() if c.peer and c.host]

    def rooms(self):
        return [c.room for c in self.clients.values() if c.room and c.room.host is c]

    def send_list(self, game=None):
        """The games of every game, the open ones first, then those in progress, to every client not paired nor in
        a started room ("game": kept for the callers, unused)."""
        rows = [(c, "waiting", "", "") for c in self.offers()]
        rooms = self.rooms()
        rows += [(r.host, "waiting", "%d/%d" % (len(r.players), r.max), ",".join(r.chars()))
                 for r in rooms if r.open()]
        rows += [(c, "in progress", "", "") for c in self.playing()]
        rows += [(r.host, "in progress", "%d/%d" % (len(r.players), r.max), ",".join(r.chars()))
                 for r in rooms if not r.open()]
        text = "\n".join("%d\t%s\t%d\t%s\t%s\t%s\t%s%s" % (c.id, c.name, c.rtt, c.game, c.disc, c.info, status,
                                                          ("\t%s\t%s" % (n, chars)) if n else "")
                         for c, status, n, chars in rows)
        for c in self.clients.values():
            if not c.peer and not (c.room and c.room.started):
                c.send("G", text)

    def leave_room(self, c, why):
        r = c.room
        if not r:
            return
        c.room = None
        i = r.players.index(c)
        r.players.remove(c)
        log("left room %d: %s (%s)" % (r.host.id, c.label(), why))
        if r.started:
            r.send("X", "%d\t%s" % (i + 1, c.name))
            if r.host is c and r.players:
                r.host = r.players[0]       # (the room carries on under another host, for the list)
        elif r.host is c:                   # the host left before the start: the room closes
            for p in list(r.players):
                p.room = None
                p.send("X", "0\t%s" % c.name)
            r.players = []
        else:
            r.send_members()
        self.send_list()

    def start_room(self, r, setup):
        r.started = True
        seed = next(self.seeds)
        for i, c in enumerate(r.players):
            c.send("B", "%d\t%d\t%d\t%s" % (i + 1, len(r.players), seed, setup))
        log("room %d started: %d players, seed %d" % (r.host.id, len(r.players), seed))
        self.send_list()

    def unpair(self, c, why):
        p = c.peer
        if p:
            c.peer = p.peer = None
            c.host = p.host = False
            p.send("X", why)
            log("unpaired %s and %s (%s)" % (c.label(), p.label(), why))
            self.send_list(p.game)

    async def serve(self, reader, writer, addr):
        c = Client(reader, writer, addr)
        c.tokens = c.burst = float(self.args.burst)
        try:
            kind, data = await asyncio.wait_for(self.frame(c), HELLO_TIMEOUT)
            parts = data.decode("utf-8", "replace").split("\t")
            if kind != "L" or len(parts) < 3 or parts[0] != VERSION:
                c.send("E", "this server runs Net Play %s: update the Manager" % VERSION)
                return
            c.name, c.game = clean(parts[1], 32) or "player", clean(parts[2], 80)
            c.disc = clean(parts[3], 20) if len(parts) > 3 else ""
            c.info = clean(parts[4], 600) if len(parts) > 4 else ""
            self.clients[c.id] = c
            log("joined the lobby: %s, %s" % (c.label(), c.game))
            c.send("I", str(c.id))
            pinger = asyncio.ensure_future(self.ping(c))
            self.send_list(c.game)
            while True:
                kind, data = await asyncio.wait_for(self.frame(c), self.args.idle)
                if kind == "Y":                 # (our ping back: its round trip)
                    try:
                        rtt = max(0, int((time.monotonic() - float(data.decode())) * 1000))
                    except ValueError:
                        continue
                    changed = abs(rtt - c.rtt) >= 3 or c.rtt < 0
                    c.rtt = rtt
                    if changed and c.offering and not c.peer:
                        self.send_list(c.game)
                elif kind == "D":               # (the details shown in the list: disc, settings)
                    parts = data.decode("utf-8", "replace").split("\t") + ["", ""]
                    c.disc, c.info = clean(parts[0], 20), clean(parts[1], 600)
                    if c.offering:
                        self.send_list()
                elif kind == "T":               # (its own ping: back as it came)
                    c.send("T", data)
                elif kind in ("R", "S", "H"):
                    if c.peer:
                        c.peer.send(kind, data)
                elif kind == "O":
                    if not c.peer and not c.offering and not c.room:
                        c.offering = True
                        log("offers a game: %s" % c.label())
                        self.send_list(c.game)
                elif kind == "W":
                    if c.offering:
                        c.offering = False
                        self.send_list(c.game)
                elif kind == "J":
                    host = self.clients.get(int(data or b"0") if data.isdigit() else 0)
                    if host is None or host is c or host.peer or not host.offering or host.game != c.game or c.room:
                        c.send("E", "that game isn't open any more")
                        self.send_list(c.game)
                        continue
                    self.unpair(c, "joined another game")
                    host.offering = c.offering = False
                    host.peer, c.peer = c, host
                    host.host, c.host = True, False
                    host.send("P", c.name)
                    c.send("P", host.name)
                    log("paired %s (host) and %s" % (host.label(), c.label()))
                    self.send_list(c.game)
                elif kind == "F":               # (pad play frames: to the room's other players)
                    if c.room and c.room.started:
                        c.room.send("F", data, but=c)
                elif kind == "M":               # (pad play: open a room)
                    parts = data.decode("utf-8", "replace").split("\t") + [""]
                    if c.peer or c.room:
                        c.send("E", "already in a game")
                        continue
                    c.offering = False
                    c.char = clean(parts[1], 4)
                    c.room = Room(c, int(parts[0]) if parts[0].strip().isdigit() else 2)
                    log("opens a room for %d: %s" % (c.room.max, c.label()))
                    c.room.send_members()
                    self.send_list()
                elif kind == "K":               # (pad play: join a room "id<TAB>character")
                    parts = data.decode("utf-8", "replace").split("\t") + [""]
                    host = self.clients.get(int(parts[0]) if parts[0].strip().isdigit() else 0)
                    r = host.room if host else None
                    char = clean(parts[1], 4)
                    if not r or r.host is not host or not r.open() or host.game != c.game or c.room or c.peer:
                        c.send("E", "that game isn't open any more")
                        self.send_list()
                        continue
                    if char != "" and char in r.chars():
                        c.send("E", "that character is taken: choose another")
                        continue
                    c.offering = False
                    c.char = char
                    c.room = r
                    r.players.append(c)
                    log("joined room %d: %s" % (host.id, c.label()))
                    r.send_members()
                    self.send_list()
                elif kind == "B":               # (pad play: the host starts its room)
                    r = c.room
                    if not r or r.host is not c or r.started or len(r.players) < 2:
                        c.send("E", "the game can't start: it needs at least 2 players")
                        continue
                    self.start_room(r, clean(data.decode("utf-8", "replace"), 4000))
                elif kind == "Q":
                    self.leave_room(c, "left")
                    self.unpair(c, "the other player left the game")
                    self.send_list(c.game)
                await writer.drain()
        except (asyncio.IncompleteReadError, asyncio.TimeoutError, ConnectionError, ValueError) as e:
            if isinstance(e, ValueError) and str(e).startswith("limit"):
                log("%s disconnected: %s" % (c.label(), e))
        finally:
            if "pinger" in locals():
                pinger.cancel()
            if self.clients.pop(c.id, None):
                log("left the lobby: %s" % c.label())
                self.leave_room(c, "disconnected")
                self.unpair(c, "the other player left the game")
                if c.game:
                    self.send_list(c.game)
            writer.close()

    @staticmethod
    async def ping(c):
        """The client's round trip, measured every PING_EVERY seconds ('Y' with the time, sent back)."""
        try:
            while True:
                c.send("Y", "%.6f" % time.monotonic())
                await asyncio.sleep(PING_EVERY)
        except asyncio.CancelledError:
            pass

    async def frame(self, c):
        """A frame from c, its bytes counted against its rate (ValueError "limit..." past it)."""
        h = await c.reader.readexactly(3)
        n = h[1] | (h[2] << 8)
        data = await c.reader.readexactly(n) if n else b""
        now = time.monotonic()
        c.tokens = min(c.burst, c.tokens + (now - c.tokens_time) * self.args.rate)
        c.tokens_time = now
        c.tokens -= 3 + n
        if c.tokens < 0:
            raise ValueError("limit: more than %d bytes a second" % self.args.rate)
        return chr(h[0]), data


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=7031)
    ap.add_argument("--proxy-from", help="address of the proxy sending the PROXY protocol (the players' real addresses)")
    ap.add_argument("--max-clients", type=int, default=200, help="connections at once (default 200)")
    ap.add_argument("--per-ip", type=int, default=8, help="connections at once from one address (default 8: a "
                    "Manager's page and relay, a few computers behind one router)")
    ap.add_argument("--connects-per-minute", type=int, default=30, help="new connections a minute from one address")
    ap.add_argument("--rate", type=int, default=256 * 1024, help="bytes a second from one connection (default 256 KB)")
    ap.add_argument("--burst", type=int, default=2 << 20, help="bytes at once from one connection (default 2 MB)")
    ap.add_argument("--idle", type=int, default=90, help="seconds without a frame before a connection is closed")
    a = ap.parse_args()
    lobby = Lobby(a)

    async def handle(reader, writer):
        sock = writer.get_extra_info("socket")
        if sock is not None:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)     # (relay frames sent at once)
        peer = writer.get_extra_info("peername") or ("?", 0)
        addr = (peer[0], peer[1])
        if a.proxy_from and peer[0] == a.proxy_from:
            # Through the proxy: its PROXY protocol line first ("PROXY TCP4 <player> <proxy> <port> <port>"), the
            # player's real address (only trusted from that address: nobody else can claim one)
            try:
                line = await asyncio.wait_for(reader.readline(), 5)
                parts = line.decode("ascii").split()
                if len(parts) < 6 or parts[0] != "PROXY" or len(line) > 108:
                    raise ValueError
                addr = (parts[2], int(parts[4]))
            except (asyncio.TimeoutError, ValueError, UnicodeDecodeError, ConnectionError, OSError):
                writer.close()
                return
        if not lobby.admit(addr[0]):
            log("%s refused: too many connections" % addr[0])
            writer.close()
            return
        try:
            await lobby.serve(reader, writer, addr)
        finally:
            lobby.release(addr[0])

    server = await asyncio.start_server(handle, "0.0.0.0", a.port)
    log("Net Play lobby on port %d%s" % (a.port, (", players' addresses from the proxy at %s" % a.proxy_from)
                                          if a.proxy_from else ""))
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
