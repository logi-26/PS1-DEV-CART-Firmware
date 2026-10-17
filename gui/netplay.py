"""
PS1 DEV CART netplay lobby (Netplay page): players, chat, invites, and the start of the link games.

Self-hosted: the host's PS1 Dev Cart Manager runs LobbyServer (TCP, one JSON object per line), every Manager (the
host's too) connects with LobbyClient. When an invite is accepted, the server tells both players how to link their
consoles with "ps1transfer -netlink" (UDP, ps1transfer/usblink.c):
- a game with the host: the host waits on its UDP port (the lobby port), the guest connects to it (the address it
  used to join the lobby): direct, the lowest latency;
- a game between two guests (neither can be reached from the internet): both connect to a UDP relay of the host
  (ports lobby + 1 to lobby + RELAYS), which passes the packets on.
The host's router opens these ports itself when it can (UPnP, see Upnp: the lobby port in TCP and UDP, the relay
ports in UDP), otherwise the host forwards them; the guests need nothing. The relay also tells each guest the other
one's address as it sees it (NL_PEER packet): ps1transfer tries a direct link (UDP hole punching), the relay is used
until it works, and again if it stops. The host's internet address: from the router (UPnP), else from a STUN server.

Messages (client -> server): hello {name, proof, version}, status {state, game, hash}, chat {text, to}, invite {to},
answer {invite, accept}, ping {t}, done {}. Server -> client: challenge {nonce, version} (first), welcome {id, host},
players {list}, chat {from, name, text, to}, invite {invite, from, name, game, hash}, answer {invite, accept, reason},
start {role, port, peer, game, key, enc, n}, pong {t}, error {text}.

Security (version 2):
- the password isn't sent: the server sends a random challenge, the client answers HMAC-SHA256(password, challenge);
  wrong passwords: 5 from an address in a minute, then it is refused for a minute. At most 4 connections from an
  address, and a connection must say hello within 5 s (the 16 places can't be taken by connections doing nothing);
- each game gets a random 128-bit link key, given to its two players only (with a password set, encrypted with a key
  derived from the password and the player's challenge). ps1transfer -netkey tags every link packet with it
  (HMAC-SHA256, see usblink.c): other computers can't link in, send units or stop the game, the relay only passes
  packets with the game's tag. Chat, names and games are not encrypted.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import hashlib
import hmac
import ipaddress
import json
import os
import queue
import select
import socket
import struct
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ElementTree

DEFAULT_PORT = 7010
RELAYS = 4                  # Guest-guest games at once (UDP ports lobby + 1 ...)
MAX_PLAYERS = 16
MAX_TEXT = 300
VERSION = 2                 # 2: password challenge, link keys
QUIET_S = 12                # A player sending nothing (pings every 2 s) for this long has gone
RELAY_REBIND_S = 2          # A guest's relay address quiet this long can be replaced (its router changed the port)
GAME_MEMORY_S = 120         # A game started this long ago can still be resumed by its two players (reconnected)
NL_MAGIC = 0x4C4E5350       # ps1transfer -netlink packets (usblink.c): hello, peer address
NL_HELLO, NL_PEER = 1, 4
NL_VERSION_KEYED = 2        # Packets with the tag of the game's link key
NL_HEADER_SIZE = 36
LINK_KEY = 16               # Bytes of a game's link key (ps1transfer -netkey: 32 hex digits)
LINK_TAG = 8                # Bytes of the tag ending each link packet
HELLO_S = 5                 # A new connection must say hello within this time
MAX_PER_ADDRESS = 4         # Connections from one address at once (the host's own Manager: not counted)
FAILS_ALLOWED = 5           # Wrong passwords from an address in FAIL_WINDOW_S, then refused for FAIL_WINDOW_S
FAIL_WINDOW_S = 60


def link_tag(key, data):
    """The tag ending each link packet of a game: the first 8 bytes of HMAC-SHA256(key, packet) (usblink.c NlTag)."""
    return hmac.new(key, data, hashlib.sha256).digest()[:LINK_TAG]


def link_check(key, data):
    """True for a link packet with the game's tag."""
    return (len(data) >= NL_HEADER_SIZE + LINK_TAG and data[6] == NL_VERSION_KEYED and
            hmac.compare_digest(link_tag(key, data[:-LINK_TAG]), data[-LINK_TAG:]))


def password_proof(password, nonce):
    """Answer to the lobby's challenge: the password itself is never sent."""
    return hmac.new(password.encode("utf-8"), nonce, hashlib.sha256).hexdigest()


def session_secret(password, nonce):
    """Shared by a client and the server when the lobby has a password: encrypts the link keys sent to it."""
    return hmac.new(password.encode("utf-8"), nonce + b"session", hashlib.sha256).digest()


def seal_key(key, secret, n):
    """A game's link key encrypted for one player (n: game number, a new pad for each game), or opened again."""
    pad = hmac.new(secret, b"link key %d" % n, hashlib.sha256).digest()[:LINK_KEY]
    return bytes(a ^ b for a, b in zip(key, pad))


def file_hash(path):
    """SHA-1 of a game file (both players must have the same one), "" if it can't be read."""
    try:
        with open(path, "rb") as f:
            return hashlib.sha1(f.read()).hexdigest()[:16]
    except OSError:
        return ""


def game_name(path):
    return os.path.splitext(os.path.basename(path))[0] if path else ""


# ---------------------------------------------------------------- server

class _Player:
    def __init__(self, server, sock, addr, pid):
        self.server = server
        self.sock = sock
        self.addr = addr
        self.id = pid
        self.name = ""
        self.state = "idle"     # idle, invited, playing
        self.game = ""
        self.hash = ""
        self.ping = None
        self.partner = None
        self.local = addr[0] in ("127.0.0.1", "::1")    # The host's own Manager
        self.buf = b""
        self.last = time.time()     # Last message (pings every 2 s)
        self.sent = []          # Times of the last chat messages (rate limit)
        self.joined = time.time()
        self.nonce = os.urandom(16)     # Challenge for the password
        self.secret = None              # Encrypts its link keys (lobby with a password)
        self.lock = threading.Lock()

    def send(self, msg):
        try:
            with self.lock:
                self.sock.sendall((json.dumps(msg) + "\n").encode("utf-8"))
        except OSError:
            pass

    def info(self):
        return {"id": self.id, "name": self.name, "state": self.state, "game": self.game, "hash": self.hash,
                "ping": self.ping, "host": self.local, "partner": self.partner}


class _Relay:
    """UDP relay of a game between two guests: what one sends is sent to the other (addresses learned from their
    first packets)."""

    def __init__(self, port):
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", port))
        self.ends = []
        self.seen = {}          # Address -> time of its last packet
        self.ips = ()           # The two guests' addresses as seen by the lobby (their UDP packets come from there)
        self.key = None         # The game's link key: only packets with its tag are passed on
        self.busy = False
        self.players = ()

    def _known(self, addr):
        """True if addr is (or can become) one of the two ends: only the guests' addresses, while their game runs. A
        guest whose router gave it another port (after a dropout) replaces its old end once that one has gone quiet."""
        if addr in self.ends:
            return True
        if addr[0] not in self.ips:
            return False
        if len(self.ends) < 2:
            self.ends.append(addr)
            return True
        now = time.time()
        for i, end in enumerate(self.ends):
            if end[0] == addr[0] and now - self.seen.get(end, 0) > RELAY_REBIND_S:
                self.ends[i] = addr
                return True
        return False

    def handle(self):
        try:
            data, addr = self.sock.recvfrom(2048)
        except OSError:
            return
        if not self.busy or not self.key or not link_check(self.key, data) or not self._known(addr):
            return
        self.seen[addr] = time.time()
        # A hello once both guests are known: the other one's address, for a direct link (hole punching)
        if len(self.ends) == 2 and len(data) >= 5 and data[4] == NL_HELLO and \
                struct.unpack_from("<I", data)[0] == NL_MAGIC:
            other = self.ends[1] if addr == self.ends[0] else self.ends[0]
            peer = struct.pack("<IBBBBIIIHHIII", NL_MAGIC, NL_PEER, 0, NL_VERSION_KEYED, 0, 0, 0, 0, 0, 0, 0, 0, 0) + \
                socket.inet_aton(other[0]) + struct.pack("!H", other[1])
            peer += link_tag(self.key, peer)
            try:
                self.sock.sendto(peer, addr)
            except OSError:
                pass
        for end in self.ends:
            if end != addr:
                try:
                    self.sock.sendto(data, end)
                except OSError:
                    pass

    def reset(self):
        self.ends = []
        self.seen = {}
        self.ips = ()
        self.key = None
        self.busy = False
        self.players = ()


class LobbyServer:
    """The lobby, in threads of the host's Manager: port 0 = any free port (tests)."""

    def __init__(self, port=DEFAULT_PORT, password="", log=print):
        self.password = password
        self.log = log
        self.players = {}
        self.invites = {}       # id -> (from, to)
        self.games = {}         # frozenset of the two names -> time: games started (a reconnected player resumes one)
        self.failed = {}        # Address -> times of its wrong passwords
        self.game_no = 0        # Games started (link key encryption)
        self.next_id = 1
        self.lock = threading.RLock()
        self.stopped = False
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if os.name != "nt":
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", port))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.relays = []
        for i in range(RELAYS):
            try:
                self.relays.append(_Relay(self.port + 1 + i))
            except OSError:
                pass
        threading.Thread(target=self._serve, daemon=True).start()

    def stop(self):
        self.stopped = True
        for s in [self.sock] + [r.sock for r in self.relays] + [p.sock for p in list(self.players.values())]:
            try:
                s.close()
            except OSError:
                pass

    # Network loop: accept, read the players' lines, relay the guests' game packets
    def _serve(self):
        while not self.stopped:
            socks = [self.sock] + [p.sock for p in self.players.values()] + [r.sock for r in self.relays]
            try:
                ready, _, _ = select.select(socks, [], [], 0.5)
            except (OSError, ValueError):
                if self.stopped:
                    return
                self._drop_closed()
                continue
            now = time.time()
            for p in list(self.players.values()):     # (a connection lost without being closed, or no hello)
                if now - p.last > QUIET_S or (not p.name and now - p.joined > HELLO_S):
                    self._leave(p)
            for s in ready:
                if s is self.sock:
                    self._accept()
                    continue
                relay = next((r for r in self.relays if r.sock is s), None)
                if relay:
                    relay.handle()
                    continue
                player = next((p for p in self.players.values() if p.sock is s), None)
                if player:
                    self._read(player)

    def _drop_closed(self):
        for p in list(self.players.values()):
            if p.sock.fileno() < 0:
                self._leave(p)

    def _accept(self):
        try:
            sock, addr = self.sock.accept()
        except OSError:
            return
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with self.lock:
            same = sum(1 for q in self.players.values() if q.addr[0] == addr[0])
            full = len(self.players) >= MAX_PLAYERS
            if full or (same >= MAX_PER_ADDRESS and addr[0] not in ("127.0.0.1", "::1")):
                try:
                    sock.sendall(b'{"type": "error", "text": "The lobby is full."}\n' if full else
                                 b'{"type": "error", "text": "Too many connections from your address."}\n')
                except OSError:
                    pass
                sock.close()
                return
            p = _Player(self, sock, addr, self.next_id)
            self.next_id += 1
            self.players[p.id] = p
            p.send({"type": "challenge", "nonce": p.nonce.hex(), "version": VERSION})

    def _read(self, p):
        try:
            data = p.sock.recv(4096)
        except OSError:
            data = b""
        if not data:
            self._leave(p)
            return
        p.buf += data
        p.last = time.time()
        if len(p.buf) > 65536:
            self._leave(p)
            return
        while b"\n" in p.buf:
            line, p.buf = p.buf.split(b"\n", 1)
            try:
                msg = json.loads(line.decode("utf-8"))
            except ValueError:
                continue
            if isinstance(msg, dict):
                # (a message with unexpected values must not stop the lobby for everyone)
                try:
                    with self.lock:
                        self._handle(p, msg)
                except Exception as e:     # pylint: disable=broad-except
                    self.log("Lobby: message from %s ignored (%s: %s)" % (p.name or p.addr[0], type(e).__name__, e))

    def _leave(self, p):
        with self.lock:
            if p.id not in self.players:
                return
            del self.players[p.id]
            try:
                p.sock.close()
            except OSError:
                pass
            if p.name:
                self.log("%s left the lobby" % p.name)
                self._broadcast({"type": "chat", "from": 0, "name": "", "text": "%s left." % p.name})
            if p.partner and p.partner in self.players:
                self._end_game(self.players[p.partner])
            for iid, (a, b) in list(self.invites.items()):
                if p.id in (a, b):
                    del self.invites[iid]
            self._players()

    def _broadcast(self, msg):
        for q in self.players.values():
            if q.name:
                q.send(msg)

    def _players(self):
        self._broadcast({"type": "players", "list": [q.info() for q in self.players.values() if q.name]})

    def _end_game(self, p):
        p.state = "idle"
        p.partner = None
        for r in self.relays:
            if r.busy and p.id in r.players:
                r.reset()

    def _handle(self, p, msg):
        kind = msg.get("type")
        if not p.name:
            if kind != "hello":
                return
            name = str(msg.get("name", "")).strip()[:20]
            if msg.get("version") != VERSION:
                p.send({"type": "error", "text": "Different Netplay version: update the PS1 Dev Cart Manager."})
                return
            if self.password:
                now = time.time()
                fails = [t for t in self.failed.get(p.addr[0], []) if now - t < FAIL_WINDOW_S]
                self.failed[p.addr[0]] = fails
                if len(fails) >= FAILS_ALLOWED:
                    p.send({"type": "error", "text": "Too many wrong passwords: try again in a minute."})
                    self._leave(p)
                    return
                if not hmac.compare_digest(str(msg.get("proof", "")), password_proof(self.password, p.nonce)):
                    fails.append(now)
                    self.log("Wrong lobby password from %s" % p.addr[0])
                    p.send({"type": "error", "text": "Wrong lobby password."})
                    return
                p.secret = session_secret(self.password, p.nonce)
            same = next((q for q in self.players.values() if q is not p and q.name.lower() == name.lower()), None)
            if same and time.time() - same.last > 6:     # Reconnecting: the old connection has gone quiet
                self._leave(same)
                same = None
            if not name or same:
                p.send({"type": "error", "text": "Choose another name (empty or already used)."})
                return
            p.name = name
            p.send({"type": "welcome", "id": p.id, "host": p.local})
            self.log("%s joined the lobby" % name)
            self._broadcast({"type": "chat", "from": 0, "name": "", "text": "%s joined." % name})
            self._players()
            return
        if kind == "status":
            if p.state != "playing":
                p.game = str(msg.get("game", ""))[:40]
                p.hash = str(msg.get("hash", ""))[:40]
            partner = next((q for q in self.players.values() if msg.get("playing") and q.name == msg["playing"]),
                           None)
            # Back in the lobby during a game (reconnected): only a game started between these two players (another
            # player can't be shown as busy by naming it)
            started = self.games.get(frozenset((p.name, partner.name))) if partner else None
            if partner and p.state == "idle" and started and time.time() - started < GAME_MEMORY_S:
                p.state = "playing"
                p.partner = partner.id
                if partner.state == "idle":     # (set idle when this player left)
                    partner.state = "playing"
                    partner.partner = p.id
            self._players()
        elif kind == "ping":
            t = msg.get("t")
            p.send({"type": "pong", "t": t if isinstance(t, (int, float)) else None})
            last = msg.get("last")
            if isinstance(last, (int, float)) and not isinstance(last, bool) and 0 <= last < 100000:
                p.ping = round(last)
        elif kind == "chat":
            now = time.time()
            p.sent = [t for t in p.sent if now - t < 5] + [now]
            if len(p.sent) > 5:
                p.send({"type": "error", "text": "Slow down: 5 messages in 5 seconds at most."})
                return
            text = str(msg.get("text", "")).strip()[:MAX_TEXT]
            if not text:
                return
            out = {"type": "chat", "from": p.id, "name": p.name, "text": text}
            to = msg.get("to")
            if to:
                q = self.players.get(to) if isinstance(to, int) else None
                if q:
                    out["to"] = to
                    out["to_name"] = q.name
                    q.send(out)
                    p.send(out)
            else:
                self._broadcast(out)
        elif kind == "invite":
            q = self.players.get(msg.get("to")) if isinstance(msg.get("to"), int) else None
            if not q or q is p or not q.name:
                return
            if p.state != "idle" or q.state != "idle":
                p.send({"type": "error", "text": "%s is busy." % q.name if q.state != "idle" else
                        "Finish your game first."})
                return
            if not p.game:
                p.send({"type": "error", "text": "Choose your game first."})
                return
            iid = "%d-%d-%d" % (p.id, q.id, int(time.time() * 1000) % 100000)
            self.invites[iid] = (p.id, q.id)
            q.send({"type": "invite", "invite": iid, "from": p.id, "name": p.name, "game": p.game,
                    "hash": p.hash})
            p.send({"type": "chat", "from": 0, "name": "", "text": "Invite sent to %s." % q.name})
        elif kind == "answer":
            iid = msg.get("invite")
            if not isinstance(iid, str) or iid not in self.invites:
                return
            a, b = self.invites.pop(iid)
            inviter, guest = self.players.get(a), self.players.get(b)
            if not inviter or not guest or guest is not p:
                return
            if not msg.get("accept"):
                inviter.send({"type": "answer", "invite": iid, "accept": False,
                              "reason": str(msg.get("reason") or "%s declined." % guest.name)[:MAX_TEXT]})
                return
            if inviter.state != "idle" or guest.state != "idle":
                guest.send({"type": "error", "text": "%s is busy now." % inviter.name})
                return
            if inviter.hash and guest.hash and inviter.hash != guest.hash:
                for x in (inviter, guest):
                    x.send({"type": "error", "text": "Not the same game file (%s / %s): both players need the same "
                                                     "file." % (inviter.game, guest.game)})
                return
            self._start(inviter, guest)
        elif kind == "done":
            partner = self.players.get(p.partner) if p.partner else None
            if partner:
                self.games.pop(frozenset((p.name, partner.name)), None)
            self._end_game(p)
            if partner:
                self._end_game(partner)
                partner.send({"type": "chat", "from": 0, "name": "", "text": "%s stopped the game." % p.name})
            self._players()

    def _start(self, a, b):
        """Starts a game: direct with the host (it waits), through a relay between two guests. The game's link key
        goes to its two players only (and to the relay)."""
        if b.local and not a.local:
            a, b = b, a
        key = os.urandom(LINK_KEY)
        self.game_no += 1

        def key_for(x):
            if x.secret:
                return {"key": seal_key(key, x.secret, self.game_no).hex(), "enc": True, "n": self.game_no}
            return {"key": key.hex(), "enc": False, "n": self.game_no}

        if a.local:
            a.send(dict({"type": "start", "role": "host", "port": self.port, "peer": b.name, "game": a.game},
                        **key_for(a)))
            b.send(dict({"type": "start", "role": "guest", "port": self.port, "peer": a.name, "game": a.game},
                        **key_for(b)))
        else:
            relay = next((r for r in self.relays if not r.busy), None)
            if not relay:
                for x in (a, b):
                    x.send({"type": "error", "text": "The host can't relay more games between guests now."})
                return
            relay.reset()
            relay.busy = True
            relay.players = (a.id, b.id)
            relay.ips = (a.addr[0], b.addr[0])
            relay.key = key
            for x, y in ((a, b), (b, a)):
                x.send(dict({"type": "start", "role": "relay", "port": relay.port, "peer": y.name, "game": a.game},
                            **key_for(x)))
        for x, y in ((a, b), (b, a)):
            x.state = "playing"
            x.partner = y.id
        now = time.time()
        self.games = {k: t for k, t in self.games.items() if now - t < GAME_MEMORY_S}
        self.games[frozenset((a.name, b.name))] = now
        self.log("Game: %s and %s (%s)" % (a.name, b.name, a.game))
        self._broadcast({"type": "chat", "from": 0, "name": "", "text": "%s and %s are playing %s." %
                         (a.name, b.name, a.game)})
        self._players()


# ---------------------------------------------------------------- client

class LobbyClient:
    """Connection to a lobby: messages received go to .events (a queue read by the page), pings every 2 s."""

    def __init__(self, host, port, name, password=""):
        self.host = host
        self.port = port
        self.events = queue.Queue()
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.alive = True
        self.ping = None
        self.lock = threading.Lock()
        self.buf = b""
        self.secret = None
        # The server's challenge first (the password itself is never sent)
        first = self._first_line()
        if first.get("type") == "error":
            self.events.put(first)
        elif first.get("type") != "challenge":
            first = {"type": "error", "text": "Different Netplay version: update the PS1 Dev Cart Manager."}
            self.events.put(first)
        self.sock.settimeout(None)
        if first.get("type") == "challenge":
            nonce = bytes.fromhex(str(first.get("nonce", "")))
            if password:
                self.secret = session_secret(password, nonce)
            self.send({"type": "hello", "name": name, "proof": password_proof(password, nonce), "version": VERSION})
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._pinger, daemon=True).start()

    def send(self, msg):
        try:
            with self.lock:
                self.sock.sendall((json.dumps(msg) + "\n").encode("utf-8"))
        except OSError:
            self.alive = False

    def _first_line(self):
        """The first message of the server (within the connection's 5 s timeout), {} if none."""
        try:
            while b"\n" not in self.buf:
                data = self.sock.recv(4096)
                if not data:
                    return {}
                self.buf += data
        except OSError:
            return {}
        line, self.buf = self.buf.split(b"\n", 1)
        try:
            msg = json.loads(line.decode("utf-8"))
        except ValueError:
            return {}
        return msg if isinstance(msg, dict) else {}

    def link_key(self, start):
        """The game's link key (32 hex digits for ps1transfer -netkey) from a start message, None if it has none."""
        try:
            key = bytes.fromhex(str(start.get("key", "")))
        except ValueError:
            return None
        if len(key) != LINK_KEY:
            return None
        if start.get("enc"):
            if not self.secret:
                return None
            key = seal_key(key, self.secret, int(start.get("n", 0)))
        return key.hex()

    def close(self):
        self.alive = False
        try:
            self.sock.close()
        except OSError:
            pass

    def _reader(self):
        buf = self.buf
        while self.alive:
            try:
                data = self.sock.recv(4096)
            except OSError:
                data = b""
            if not data:
                break
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                try:
                    msg = json.loads(line.decode("utf-8"))
                except ValueError:
                    continue
                if not isinstance(msg, dict):
                    continue
                if msg.get("type") == "pong" and isinstance(msg.get("t"), (int, float)):
                    self.ping = (time.time() - msg["t"]) * 1000
                    continue
                self.events.put(msg)
        self.alive = False
        self.events.put({"type": "closed"})

    def _pinger(self):
        while self.alive:
            self.send({"type": "ping", "t": time.time(), "last": self.ping})
            time.sleep(2)


# ---------------------------------------------------------------- the host's router and internet address

def _private(ip):
    try:
        a = ipaddress.ip_address(ip)
    except ValueError:
        return True
    return a.is_private or a in ipaddress.ip_network("100.64.0.0/10")     # (100.64/10: carrier-grade NAT)


def route_ip(target="8.8.8.8"):
    """This computer's address on the network leading to target (no packet sent)."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect((target, 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return ""


def stun_address(server=("stun.l.google.com", 19302), timeout=2.0):
    """This network's internet address as a STUN server sees it (RFC 5389 binding request), "" if no answer."""
    tid = os.urandom(12)
    req = struct.pack("!HHI", 1, 0, 0x2112A442) + tid
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(req, (socket.gethostbyname(server[0]), server[1]))
        data, _ = s.recvfrom(2048)
    except OSError:
        return ""
    finally:
        s.close()
    if len(data) < 20 or data[8:20] != tid:
        return ""
    pos = 20
    while pos + 4 <= len(data):
        kind, size = struct.unpack_from("!HH", data, pos)
        value = data[pos + 4:pos + 4 + size]
        if kind in (0x0020, 0x8020) and len(value) >= 8 and value[1] == 1:     # XOR-MAPPED-ADDRESS, IPv4
            return socket.inet_ntoa(struct.pack("!I", struct.unpack_from("!I", value, 4)[0] ^ 0x2112A442))
        if kind == 0x0001 and len(value) >= 8 and value[1] == 1:               # MAPPED-ADDRESS
            return socket.inet_ntoa(value[4:8])
        pos += 4 + size + (-size % 4)
    return ""


class Upnp:
    """The router's port mappings (UPnP Internet Gateway Device: WANIPConnection / WANPPPConnection service)."""

    SEARCH = ("urn:schemas-upnp-org:device:InternetGatewayDevice:1",
              "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
              "urn:schemas-upnp-org:service:WANIPConnection:1")

    def __init__(self, control, service, local_ip):
        self.control = control
        self.service = service
        self.local_ip = local_ip
        self.mapped = []

    @classmethod
    def discover(cls, timeout=2.0, tries=3):
        """The router, or None (no UPnP, or switched off). Its answers sometimes don't come: tried again."""
        for _ in range(tries):
            up = cls._discover(timeout)
            if up:
                return up
        return None

    @classmethod
    def _discover(cls, timeout):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(timeout)
        locations = []
        try:
            for st in cls.SEARCH:
                msg = ("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
                       "MX: 2\r\nST: %s\r\n\r\n" % st)
                s.sendto(msg.encode("ascii"), ("239.255.255.250", 1900))
            end = time.time() + timeout
            while time.time() < end:
                try:
                    data, _ = s.recvfrom(4096)
                except socket.timeout:
                    break
                for line in data.decode("latin-1").split("\r\n"):
                    if line.lower().startswith("location:"):
                        loc = line.split(":", 1)[1].strip()
                        if loc not in locations:
                            locations.append(loc)
        except OSError:
            return None
        finally:
            s.close()
        for loc in locations:
            try:
                xml = urllib.request.urlopen(loc, timeout=3).read()
                root = ElementTree.fromstring(xml)
            except Exception:       # noqa: BLE001 (another device answering)
                continue
            ns = root.tag[:root.tag.index("}") + 1] if root.tag.startswith("{") else ""
            base = root.findtext(ns + "URLBase") or loc
            for svc in root.iter(ns + "service"):
                stype = svc.findtext(ns + "serviceType") or ""
                if "WANIPConnection" in stype or "WANPPPConnection" in stype:
                    control = urllib.parse.urljoin(base, svc.findtext(ns + "controlURL") or "")
                    router = urllib.parse.urlparse(loc).hostname
                    up = cls(control, stype, route_ip(router))
                    try:
                        up.external_ip()
                        return up
                    except Exception:       # noqa: BLE001
                        continue
        return None

    def _soap(self, action, args=()):
        body = "".join("<%s>%s</%s>" % (k, v, k) for k, v in args)
        envelope = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
                    's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
                    '<u:%s xmlns:u="%s">%s</u:%s></s:Body></s:Envelope>' % (action, self.service, body, action))
        req = urllib.request.Request(self.control, envelope.encode("utf-8"), {
            "Content-Type": 'text/xml; charset="utf-8"', "SOAPAction": '"%s#%s"' % (self.service, action)})
        try:
            xml = urllib.request.urlopen(req, timeout=5).read()
        except urllib.error.HTTPError as e:
            text = e.read().decode("utf-8", "replace")
            code = text.split("<errorCode>")[1].split("<")[0] if "<errorCode>" in text else str(e.code)
            raise OSError("UPnP %s: error %s" % (action, code))
        out = {}
        for el in ElementTree.fromstring(xml).iter():
            out[el.tag.split("}")[-1]] = el.text
        return out

    def external_ip(self):
        return self._soap("GetExternalIPAddress").get("NewExternalIPAddress") or ""

    def add(self, port, proto, desc="PS1 DEV CART Netplay"):
        args = [("NewRemoteHost", ""), ("NewExternalPort", port), ("NewProtocol", proto),
                ("NewInternalPort", port), ("NewInternalClient", self.local_ip), ("NewEnabled", 1),
                ("NewPortMappingDescription", desc)]
        try:
            try:
                self._soap("AddPortMapping", args + [("NewLeaseDuration", 7200)])
            except OSError as e:
                if "718" not in str(e):     # ConflictInMappingEntry: replaced if it's one of ours (e.g. left over)
                    raise
                old = self._soap("GetSpecificPortMappingEntry", [("NewRemoteHost", ""), ("NewExternalPort", port),
                                                                 ("NewProtocol", proto)])
                if old.get("NewPortMappingDescription") != desc:
                    raise OSError("port %d/%s is already used on the router by %s (%s)" % (
                        port, proto, old.get("NewInternalClient"), old.get("NewPortMappingDescription")))
                self._soap("DeletePortMapping", [("NewRemoteHost", ""), ("NewExternalPort", port),
                                                 ("NewProtocol", proto)])
                self._soap("AddPortMapping", args + [("NewLeaseDuration", 7200)])
        except OSError as e:
            if "725" not in str(e):     # OnlyPermanentLeasesSupported
                raise
            self._soap("AddPortMapping", args + [("NewLeaseDuration", 0)])
        self.mapped.append((port, proto))

    def remove_all(self):
        for port, proto in self.mapped:
            try:
                self._soap("DeletePortMapping", [("NewRemoteHost", ""), ("NewExternalPort", port),
                                                 ("NewProtocol", proto)])
            except OSError:
                pass
        self.mapped = []


def open_router(port, log):
    """The lobby's ports opened on the router (UPnP), and the internet address: (upnp or None, address, message)."""
    up = Upnp.discover()
    address, notes = "", []
    if up:
        try:
            for p, proto in [(port, "TCP"), (port, "UDP")] + [(port + 1 + i, "UDP") for i in range(RELAYS)]:
                up.add(p, proto)
            address = up.external_ip()
            notes.append("Ports opened by your router (UPnP).")
        except OSError as e:
            log("UPnP: %s" % e)
            up.remove_all()
            up = None
    if not up:
        notes.append("Your router didn't open the ports (UPnP off or not supported): forward them on your router "
                     "to this computer (%s): TCP and UDP %d, UDP %d-%d." % (route_ip(), port, port + 1,
                                                                              port + RELAYS))
    stun = stun_address()
    if not address or _private(address):
        if address and stun and stun != address:
            notes.append("Your router's address %s isn't on the internet: your provider shares addresses "
                         "(carrier-grade NAT), players can't reach you. Let another player host." % address)
        address = stun or address
    elif stun and stun != address:
        notes.append("Two routers in a row (address %s seen from the internet, %s from your router): the first one "
                     "needs the ports forwarded too." % (stun, address))
        address = stun
    return up, address, " ".join(notes)


def close_router(port):
    """The lobby's port mappings removed from the router (e.g. by a script stopping a hosted lobby)."""
    up = Upnp.discover()
    if up:
        up.mapped = [(port, "TCP"), (port, "UDP")] + [(port + 1 + i, "UDP") for i in range(RELAYS)]
        up.remove_all()
    return up is not None
