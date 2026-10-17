"""
PS1 DEV CART GUI: Netplay page (link games between two consoles over the internet, see ../netplay.py and
ps1transfer -netlink).

One player hosts the lobby (this program runs it, the host forwards its ports), the others join it with the host's
address. The lobby shows the players and their games, with a chat (to everyone or one player) and invites. When an
invite is accepted, both consoles are reset, the game is started (Net Yaroze game with libps.exe and the USB link tty
driver "-c link", or a program using psxcomb's USB link) and linked with ps1transfer -netlink.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import atexit
import os
import queue
import socket
import sys
import threading
import time

import ttkbootstrap as tb
from ttkbootstrap.dialogs import Messagebox

from pages.base import Page
from widgets import FileField, MonoText, PS1_EXE_TYPES, labeled

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import netplay  # noqa: E402

LINKS = {"Net Yaroze game (tty, -c link)": "tty", "psxcomb USB link (homebrew)": "rings"}
EVERYONE = "Everyone"


def local_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "?"


class NetplayPage(Page):
    title = "Netplay"

    def build(self):
        s = self.app.settings
        self.server = None
        self.client = None
        self.my_id = None
        self.players = []
        self.playing = None         # Partner's name while a game runs
        self.log_queue = queue.Queue()
        self.router = None          # Upnp: ports opened on the host's router
        self.public = ""            # Host's internet address
        self.target = None          # (host, port, name, password) of the lobby, for reconnecting
        self.reconnect_at = None
        atexit.register(self._close_router)

        tb.Label(self, text="Netplay", font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="Link games with a console somewhere else, through the internet: Net Yaroze games using "
                            "the serial link (tty) and homebrew using psxcomb's USB link. One player hosts the lobby, "
                            "the others join it with the host's address.", wraplength=800, justify="left",
                 bootstyle="secondary").pack(anchor="w", pady=(0, 6))

        box = self.section("Lobby")
        box.pack(fill="x", pady=(0, 6))
        row = tb.Frame(box)
        row.pack(fill="x")
        _, self.name = labeled(row, "Your name", tb.Entry, width=10)
        self.name.master.pack(side="left")
        self.name.configure(width=14)
        self.name.insert(0, s.field("net.name", ""))
        self.mode = tb.StringVar(value=s.field("net.mode", "join"))
        tb.Radiobutton(row, text="Join", variable=self.mode, value="join", command=self._mode).pack(side="left",
                                                                                                   padx=(16, 0))
        tb.Radiobutton(row, text="Host", variable=self.mode, value="host", command=self._mode).pack(side="left",
                                                                                                   padx=(8, 0))
        _, self.address = labeled(row, "Address", tb.Entry, width=8)
        self.address.master.pack(side="left", padx=(16, 0))
        self.address.configure(width=22)
        self.address.insert(0, s.field("net.address", ""))
        _, self.port = labeled(row, "Port", tb.Entry, width=5)
        self.port.master.pack(side="left", padx=(12, 0))
        self.port.configure(width=6)
        self.port.insert(0, str(s.field("net.port", netplay.DEFAULT_PORT)))
        _, self.password = labeled(row, "Password", tb.Entry, width=9, show="*")
        self.password.master.pack(side="left", padx=(12, 0))
        self.password.configure(width=10)
        self.connect_button = tb.Button(row, text="Connect", bootstyle="success", width=11, command=self.connect)
        self.connect_button.pack(side="left", padx=(12, 0))
        row = tb.Frame(box)
        row.pack(fill="x", pady=(4, 0))
        self.status = tb.Label(row, text="", bootstyle="secondary", wraplength=820, justify="left")
        self.status.pack(side="left", fill="x", expand=True)
        self.copy_button = tb.Button(row, text="Copy address", bootstyle="secondary-outline", command=self.copy_address)

        box = self.section("Your game")
        box.pack(fill="x", pady=(0, 6))
        self.exe = FileField(box, self.app, "net.exe", "Game", filetypes=PS1_EXE_TYPES, on_change=self._game_changed)
        self.exe.pack(fill="x")
        row = tb.Frame(box)
        row.pack(fill="x", pady=(4, 0))
        self.libps = FileField(row, self.app, "net.libps", "libps.exe", filetypes=PS1_EXE_TYPES, width=30)
        self.libps.pack(side="left", fill="x", expand=True)
        _, self.link = labeled(row, "Link", tb.Combobox, width=5, values=list(LINKS), state="readonly")
        self.link.master.pack(side="left", padx=(12, 0))
        self.link.configure(width=26)
        self.link.set(s.field("net.link", list(LINKS)[0]))

        panes = tb.Panedwindow(self, orient="horizontal")
        panes.pack(fill="both", expand=True)
        left = tb.Frame(panes)
        right = tb.Frame(panes)
        panes.add(left, weight=2)
        panes.add(right, weight=3)
        self.table = tb.Treeview(left, columns=("name", "game", "state", "ping"), show="headings", height=6,
                                 selectmode="browse")
        for col, text, width in (("name", "Player", 110), ("game", "Game", 120), ("state", "Status", 110),
                                 ("ping", "Ping", 50)):
            self.table.heading(col, text=text, anchor="w")
            self.table.column(col, width=width, anchor="w", stretch=(col == "game"))
        self.table.pack(fill="both", expand=True)
        buttons = tb.Frame(left)
        buttons.pack(fill="x", pady=(4, 0))
        tb.Button(buttons, text="Invite to play", bootstyle="info", command=self.invite).pack(side="left")
        tb.Button(buttons, text="Message", bootstyle="secondary-outline", command=self.message_to).pack(side="left",
                                                                                                      padx=6)
        self.chat = MonoText(right, height=8)
        self.chat.pack(fill="both", expand=True, padx=(8, 0))
        row = tb.Frame(right)
        row.pack(fill="x", padx=(8, 0), pady=(4, 0))
        self.to = tb.Combobox(row, values=[EVERYONE], state="readonly", width=14)
        self.to.set(EVERYONE)
        self.to.pack(side="left")
        self.entry = tb.Entry(row)
        self.entry.pack(side="left", fill="x", expand=True, padx=6)
        self.entry.bind("<Return>", lambda e: self.send_chat())
        tb.Button(row, text="Send", bootstyle="secondary", command=self.send_chat).pack(side="left")

        bar = tb.Frame(self)
        bar.pack(fill="x", pady=(6, 0))
        self.game_status = tb.Label(bar, text="No game.", bootstyle="info")
        self.game_status.pack(side="left")
        tb.Button(bar, text="Stop the game", bootstyle="danger-outline", command=self.stop_game).pack(side="right")
        self._mode()
        self.after(100, self._poll)

    # ---------------------------------------------------------------- lobby connection

    def _mode(self):
        host = self.mode.get() == "host"
        self.address.configure(state="disabled" if host else "normal")

    def _say(self, text):
        self.chat.append(text + "\n")

    def connect(self):
        if self.client:
            self.disconnect()
            return
        s = self.app.settings
        name = self.name.get().strip()
        if not name:
            self.app.error("Choose your name.")
            return
        try:
            port = int(self.port.get())
        except ValueError:
            self.app.error("The port is a number (default %d)." % netplay.DEFAULT_PORT)
            return
        s.set_field("net.name", name)
        s.set_field("net.mode", self.mode.get())
        s.set_field("net.port", port)
        host = "127.0.0.1"
        if self.mode.get() == "host":
            if not self.password.get() and not self.app.confirm(
                    "Host without a password? Anyone who knows your address can then join the lobby and chat (the "
                    "games themselves stay protected by their link keys). A password of 10 characters or more is "
                    "best."):
                return
            try:
                self.server = netplay.LobbyServer(port, self.password.get(), log=self.log_queue.put)
            except OSError as e:
                self.app.error("Can't host on port %d: %s" % (port, e))
                return
            self.status.configure(text="Hosting on port %d. Opening the ports on your router (UPnP) and finding "
                                       "your internet address..." % port)
            threading.Thread(target=self._open_router, args=(port,), daemon=True).start()
        else:
            host = self.address.get().strip()
            if ":" in host:
                host, _, p = host.rpartition(":")
                port = int(p) if p.isdigit() else port
            if not host:
                self.app.error("Enter the host's address (internet address or name).")
                return
            s.set_field("net.address", self.address.get().strip())
        try:
            self.client = netplay.LobbyClient(host, port, name, self.password.get())
        except OSError as e:
            self.app.error("Can't connect to %s:%d: %s" % (host, port, e))
            self._stop_server()
            return
        self.target = (host, port, name, self.password.get())
        if self.mode.get() == "join":
            self.status.configure(text="Connected to %s:%d." % (host, port))
        self.connect_button.configure(text="Disconnect", bootstyle="danger-outline")
        self._game_changed()

    def _open_router(self, port):
        """(thread) UPnP port mappings and the internet address, shown by _poll."""
        up, address, notes = netplay.open_router(port, self.log_queue.put)
        self.log_queue.put(("router", up, address, notes, port))

    def _router_done(self, up, address, notes, port):
        if not self.server:     # (disconnected meanwhile)
            if up:
                up.remove_all()
            return
        self.router = up
        self.public = address
        self.status.configure(text="Hosting. Players join with %s (port %d). %s" % (
            address or "your internet address", port, notes))
        if address:
            self.copy_button.pack(side="left", padx=(8, 0))
        if up:
            self.after(30 * 60 * 1000, self._renew_router)

    def _renew_router(self):
        """The mappings are leased for 2 hours: renewed every 30 minutes while hosting."""
        up = self.router
        if not up or not self.server:
            return

        def renew():
            for port, proto in list(up.mapped):
                try:
                    up.add(port, proto)
                except OSError:
                    pass
            up.mapped = list(dict.fromkeys(up.mapped))
        threading.Thread(target=renew, daemon=True).start()
        self.after(30 * 60 * 1000, self._renew_router)

    def _close_router(self):
        if self.router:
            self.router.remove_all()
            self.router = None

    def copy_address(self):
        if self.public:
            self.clipboard_clear()
            self.clipboard_append("%s:%s" % (self.public, self.port.get()))
            self._say("* Address copied: %s:%s (send it to the other players)." % (self.public, self.port.get()))

    def disconnect(self):
        if self.playing:
            self.stop_game()
        self.target = None
        self.reconnect_at = None
        if self.client:
            self.client.close()
            self.client = None
        self._stop_server()
        self.copy_button.pack_forget()
        self.my_id = None
        self.players = []
        self._show_players()
        self.connect_button.configure(text="Connect", bootstyle="success")
        self.status.configure(text="Not connected.")
        self._say("* Disconnected.")

    def _stop_server(self):
        if self.server:
            self.server.stop()
            self.server = None
        if self.router:     # (at once: a thread could be cut off when the program closes)
            up, self.router = self.router, None
            up.remove_all()

    def _game_changed(self):
        if self.client:
            exe = self.exe.get()
            self.client.send({"type": "status", "game": netplay.game_name(exe),
                              "hash": netplay.file_hash(exe) if exe else "", "playing": self.playing or ""})

    # ---------------------------------------------------------------- lobby events

    def _poll(self):
        while not self.log_queue.empty():
            item = self.log_queue.get()
            if isinstance(item, tuple):
                self._router_done(*item[1:])
            else:
                self.app.log.append("Lobby: %s\n" % item)
        if not self.client and self.target and self.reconnect_at and time.time() >= self.reconnect_at:
            self._reconnect()
        client = self.client
        while client and not client.events.empty():     # (a "closed" event clears self.client)
            m = client.events.get()
            try:
                self._event(m)
            except (KeyError, TypeError, ValueError, AttributeError) as e:     # (polling must go on)
                self.app.log.append("Lobby: message ignored (%s: %s)\n" % (type(e).__name__, e))
            if self.client and self.client.ping is not None and not self.playing:
                self.game_status.configure(text="No game. Lobby ping %.0f ms." % self.client.ping)
        self.after(100, self._poll)

    def _event(self, m):
        kind = m.get("type")
        if kind == "welcome":
            self.my_id = m["id"]
            self._say("* Welcome to the lobby%s." % (" (you host it)" if m.get("host") else ""))
        elif kind == "players":
            self.players = m["list"]
            self._show_players()
        elif kind == "chat":
            if not m.get("from"):
                self._say("* " + m["text"])
            elif m.get("to"):
                me = m["from"] == self.my_id
                self._say("(%s) %s" % ("to " + m["to_name"] if me else m["name"] + " to you", m["text"]))
            else:
                self._say("%s: %s" % (m["name"], m["text"]))
        elif kind == "error":
            self._say("! " + m["text"])
        elif kind == "invite":
            self._invited(m)
        elif kind == "answer":
            self._say("* " + (m.get("reason") or "Invite declined."))
        elif kind == "start":
            self._start_game(m)
        elif kind == "closed":
            self.client = None
            if self.target:     # Lost (not Disconnect): tried again every 3 s, a game goes on meanwhile
                self._say("! Connection to the lobby lost: reconnecting..." +
                          (" (your game goes on)" if self.playing else ""))
                self.reconnect_at = time.time() + 3
            else:
                self._stop_server()
                self.connect_button.configure(text="Connect", bootstyle="success")
                self.status.configure(text="Not connected.")

    def _show_players(self):
        self.table.delete(*self.table.get_children())
        names = [EVERYONE]
        for p in self.players:
            state = {"idle": "In the lobby", "playing": "Playing"}.get(p["state"], p["state"])
            if p.get("host"):
                state += " (host)"
            me = p["id"] == self.my_id
            self.table.insert("", "end", iid=str(p["id"]), values=(p["name"] + (" (you)" if me else ""),
                              p["game"] or "-", state, "%d ms" % p["ping"] if p.get("ping") is not None else ""))
            if not me:
                names.append(p["name"])
        self.to.configure(values=names)
        if self.to.get() not in names:
            self.to.set(EVERYONE)

    def _selected(self):
        sel = self.table.selection()
        if not sel:
            self.app.error("Select a player in the list.")
            return None
        p = next((p for p in self.players if str(p["id"]) == sel[0]), None)
        if p and p["id"] == self.my_id:
            self.app.error("That's you.")
            return None
        return p

    def message_to(self):
        p = self._selected()
        if p:
            self.to.set(p["name"])
            self.entry.focus_set()

    def send_chat(self):
        text = self.entry.get().strip()
        if not text or not self.client:
            return
        msg = {"type": "chat", "text": text}
        if self.to.get() != EVERYONE:
            p = next((p for p in self.players if p["name"] == self.to.get()), None)
            if p:
                msg["to"] = p["id"]
        self.client.send(msg)
        self.entry.delete(0, "end")

    def _ready(self):
        """The game is chosen (and libps.exe for a Net Yaroze game)."""
        exe = self.exe.get()
        if not exe or not os.path.exists(exe):
            self.app.error("Choose your game first (the same file as the other player).")
            return False
        self.exe.remember()
        if self.libps.get():
            self.libps.remember()
        self.app.settings.set_field("net.link", self.link.get())
        return True

    def invite(self):
        if not self.client:
            self.app.error("Connect to a lobby first.")
            return
        p = self._selected()
        if p and self._ready():
            self._game_changed()
            self.client.send({"type": "invite", "to": p["id"]})

    def _invited(self, m):
        mine = netplay.file_hash(self.exe.get()) if self.exe.get() else ""
        if not mine:
            self.client.send({"type": "answer", "invite": m["invite"], "accept": False,
                              "reason": "%s hasn't chosen a game." % self.name.get()})
            self._say("* %s invited you to play %s: choose the game, then ask them to invite you again." %
                      (m["name"], m["game"]))
            return
        if m.get("hash") and m["hash"] != mine:
            self.client.send({"type": "answer", "invite": m["invite"], "accept": False,
                              "reason": "%s has another version of the game." % self.name.get()})
            self._say("! %s invited you to play %s, but your game file is different." % (m["name"], m["game"]))
            return
        answer = Messagebox.yesno("%s invites you to play %s.\n\nStart the game now? Your console will be reset." %
                                  (m["name"], m["game"]), "Netplay", parent=self)
        accept = answer in ("Yes", "yes", True)
        if self.client:
            self.client.send({"type": "answer", "invite": m["invite"], "accept": accept})

    # ---------------------------------------------------------------- game

    def _start_game(self, m):
        if not self._ready():
            self.client.send({"type": "done"})
            return
        if m["role"] == "host":
            target = ":%d" % m["port"]
        else:       # Guest: the host's address, relay: the host's relay port
            target = "%s:%d" % (self.client.host, m["port"])
        args = ["-r"]
        if self.libps.get() and LINKS.get(self.link.get()) == "tty":
            args += ["-y", self.libps.get()]
        args += ["-e", "-f", self.exe.get()]
        if LINKS.get(self.link.get()) == "tty":
            args += ["-c", "link"]
        args += ["-netlink", target]
        key = self.client.link_key(m)
        if not key:     # (the lobby gives each game a key: only its two players' packets are taken)
            self._say("! No link key for this game from the lobby: not started.")
            self.client.send({"type": "done"})
            return
        args += ["-netkey", key]
        self.playing = m["peer"]
        self.game_status.configure(text="Starting %s with %s..." % (m["game"], m["peer"]))
        self._say("* Starting %s with %s%s." % (m["game"], m["peer"],
                                                " through the host's relay" if m["role"] == "relay" else ""))
        self.app.run(args, "Netplay", long_running=True, stdin=True, on_line=self._line,
                     log_filter=lambda line: not line.startswith("netlink:"), on_done=self._game_done)

    def _line(self, line):
        if line.startswith("netlink:"):
            parts = line.split()
            try:
                rtt = float(parts[2])
                text = "Playing with %s: %.0f ms round trip (%.1f frames), %s units/s out, %s in." % (
                    self.playing, rtt, rtt * 60 / 1000.0, parts[7], parts[9])
            except (ValueError, IndexError):
                return
            self.after(0, lambda: self.game_status.configure(text=text))
        elif line.startswith("Linked with"):
            self.after(0, lambda: self.game_status.configure(text="Linked with %s." % self.playing))
        elif line.startswith("Waiting for the other computer") or line.startswith("Connecting to"):
            self.after(0, lambda: self.game_status.configure(text="Waiting for %s..." % self.playing))

    def _game_done(self, code, out):
        if self.playing:
            self._say("* Game with %s ended." % self.playing)
        self.playing = None
        self.game_status.configure(text="No game.")
        if self.client:
            self.client.send({"type": "done"})

    def stop_game(self):
        if self.playing:
            self.app.stop_task()

    def _reconnect(self):
        host, port, name, password = self.target
        try:
            self.client = netplay.LobbyClient(host, port, name, password)
        except OSError:
            self.reconnect_at = time.time() + 3
            return
        self.reconnect_at = None
        self._say("* Back in the lobby.")
        self._game_changed()
