"""
PS1 DEV CART GUI: Net Play page - link-cable games played through the cartridges' USB (no link cable).

Layout (top to bottom):
  Your game: the cartridge chosen at the top of the window, the game in its console detected (its patch package,
      gui/data/netplay, written by ps1-linkplay/virtual/build_hw.py: any link-cable game can have one), and the
      automatic start (the package's "auto_steps": the Manager starts the game's link play on the console).
  Lobby: the online lobby's open games (ps1-linkplay/lobby/netplay_server.py: -vlinklobby; its address is
      LOBBY_SERVER), listed by the page itself (netplay.LobbyWatcher: no console needed) with the ping from this
      computer to each host; the player's own offer shows "(host)". Below the list: the chosen game's settings (the
      ones its host chose).
  Status: the link's state.
Host a game opens a window with the player's name, the game's own options (from its package: e.g. Red Alert's
country and color) and the host's game settings (map, credits...); Join opens one with the game's own options only.
Both then install the patch, run the relay (ps1transfer, the app's task), and start the console's link play when the
game has an automatic start: when hosting at once, when joining once paired with the host (its console already
waiting: this one answers, as on a cable).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import json
import os
import re
import socket
import tempfile
import threading
import time
import types

import ttkbootstrap as tb

import ctr_package
import netplay
import padrelay
from pages.base import Page

RELAY_TASK = "Net Play link"
# The lobby server (the players never type it). PS1DEVCART_LOBBY overrides it for tests (a local server).
# The lobby server on the NAS (ps1-linkplay/lobby/nas/netplay.sh; the router forwards TCP 7031 to it). For tests:
# PS1DEVCART_LOBBY=127.0.0.1:7031 (a local netplay_server.py), or 192.168.0.33:7031 (the NAS from the home network)
LOBBY_SERVER = os.environ.get("PS1DEVCART_LOBBY") or "play.logi26.co.uk:7031"
STATS = re.compile(r"VLINK (\S+) \d+ turns/s, (\d+) B in, \d+ B out, (\d+) frames/s\s+(\S+) \d+ turns/s, (\d+) B in, "
                   r"\d+ B out, (\d+) frames/s")
WRAP = 860
SETTINGS_COLS = 3                       # (the chosen game's settings: label/value pairs a row)


class NetPlayPage(Page):
    title = "Net Play"

    # ---------------------------------------------------------------- layout

    def build(self):
        self.patches = netplay.load_patches()
        self.game = None            # The detected game's patch package
        self.game_serial = ""       # (the cartridge it was detected on)
        self.disc = ""              # The detected game's disc (e.g. SLES_113.43: shown in the lobby with the offer)
        self.lobby_cmds = {"join": 0, "leave": 0, "auto": 0}
        self.auto_when = None       # The automatic start waiting for: "lobby" (to host), "paired" (joining), "now";
        self.pending_join = 0       # a game chosen in the list before the relay ran: joined once in the lobby
        self.my_id = 0              # This window's offer in the lobby (its relay's lobby id)
        self.hosting = False        # This window's console waits on its link page, the game offered ("VLINK OFFER")
        self.offer_name = ""        # (the name it is offered under: its row, with a lobby server that gives no ids)
        self.paired_with = ""       # The other player once paired
        self.switch_to = 0          # A game to join once this console left its link page (it was hosting)
        self.watcher = None         # The lobby's list (netplay.LobbyWatcher)
        self.games_shown = []
        self.settings_shown = None  # (the settings panel's content: rebuilt only when it changes)
        self.work_dir = os.path.join(tempfile.gettempdir(), "ps1devcart_netplay_%d" % os.getpid())
        self.tune_path = os.path.join(self.work_dir, "tune.txt")
        self.relay_log = None
        self.installing = False
        self.last_error = ""
        self.cancel = threading.Event()
        # Pad play (a package with "kind": "padplay", e.g. Crash Team Racing: 2-4 players, only the buttons sent; the
        # relay is padrelay.py in a thread, the console reached through "ps1transfer -xserve", the page's task)
        self.relay = None           # The pad play relay (padrelay.Relay) while it runs
        self.pad_room = []          # Its room: [player, name, character] per player
        self.pad_hosting = False    # This window opened the room (it can Start now)
        self.pad_started = False    # The game started (the room closed)

        head = tb.Frame(self)
        head.pack(fill="x")
        tb.Label(head, text="Net Play", font=("", 16, "bold")).pack(anchor="w")
        tb.Label(head, text="Play with other players over the internet: link-cable games (the PS1 DEV CART links the "
                            "two consoles, no cable needed) and two- to four-player games (each player on their own "
                            "console).", bootstyle="secondary", wraplength=WRAP, justify="left").pack(
            anchor="w", pady=(0, 10))
        body = tb.ScrolledFrame(self, autohide=True)
        body.pack(fill="both", expand=True)
        self._build_game(body)
        self._build_lobby(body)
        self._build_status(body)
        self._update_buttons()

    def _build_game(self, parent):
        box = self.game_box = self.section("Your game", parent)
        box.pack(fill="x")
        box.columnconfigure(3, weight=1)
        tb.Label(box, text="Cartridge").grid(row=0, column=0, sticky="w")
        self.cart_label = tb.Label(box, text="", font=("", 10, "bold"))
        self.cart_label.grid(row=0, column=1, sticky="w", padx=(8, 16))
        tb.Button(box, text="Detect the game", bootstyle="primary-outline", command=self.detect).grid(
            row=0, column=2, sticky="w")
        self.game_label = tb.Label(box, text="Boot the game from the cartridge's menu, then detect it.",
                                   bootstyle="secondary", wraplength=WRAP - 380, justify="left")
        self.game_label.grid(row=0, column=3, sticky="w", padx=(16, 0))
        names = ", ".join(p.get("title", "?") for p in self.patches)
        tb.Label(box, text="Supported: %s" % (names or "none (no patch package found)"), bootstyle="secondary",
                 wraplength=WRAP, justify="left").grid(row=1, column=0, columnspan=4, sticky="w", pady=(6, 0))
        # (the automatic start's switch and the game's hint: filled when a game is detected)
        self.auto_frame = tb.Frame(box)
        self.auto_frame.grid(row=2, column=0, columnspan=4, sticky="ew")

    def _build_lobby(self, parent):
        box = self.lobby_box = self.section("Lobby", parent)
        box.pack(fill="both", expand=True, pady=(10, 0))
        top = tb.Frame(box)
        top.pack(fill="x")
        tb.Label(top, text="Games", font=("", 13, "bold")).pack(side="left")
        self.lobby_state = tb.Label(top, text="", bootstyle="secondary")
        self.lobby_state.pack(side="right")
        bar = tb.Frame(box)
        bar.pack(fill="x", pady=(8, 6))
        self.host_button = tb.Button(bar, text="Host a game", bootstyle="success", width=14, command=self.host)
        self.host_button.pack(side="left")
        self.join_button = tb.Button(bar, text="Join", bootstyle="primary", width=10, command=self.join)
        self.join_button.pack(side="left", padx=(8, 0))
        self.leave_button = tb.Button(bar, text="Leave the game", bootstyle="secondary-outline", command=self.leave)
        self.leave_button.pack(side="left", padx=(8, 0))
        # (pad play: the host starts with the players who joined, without waiting for the room to fill)
        self.start_button = tb.Button(bar, text="Start now", bootstyle="warning", width=12, command=self.start_now)
        cols = tb.Frame(box)
        cols.pack(fill="both", expand=True)
        tb.Style().configure("NetPlay.Treeview", rowheight=28)
        self.games = tb.Treeview(cols, columns=("player", "game", "status", "ping"), show="headings", height=6,
                                 selectmode="browse", style="NetPlay.Treeview")
        self.games.heading("player", text="Host", anchor="w")
        self.games.heading("game", text="Game", anchor="w")
        self.games.heading("status", text="Status", anchor="w")
        self.games.heading("ping", text="Ping", anchor="e")
        self.games.column("player", width=180, anchor="w", stretch=False)
        self.games.column("game", width=400, anchor="w", stretch=True)
        self.games.column("status", width=110, anchor="w", stretch=False)
        self.games.column("ping", width=90, anchor="e", stretch=False)
        self.games.pack(side="left", fill="both", expand=True)
        bar_y = tb.Scrollbar(cols, orient="vertical", command=self.games.yview)
        bar_y.pack(side="left", fill="y")
        self.games.configure(yscrollcommand=bar_y.set)
        self.games.tag_configure("mine", foreground="#888888")
        self.games.tag_configure("playing", foreground="#888888")
        self.games.bind("<Double-1>", lambda e: self.join())
        self.games.bind("<<TreeviewSelect>>", lambda e: (self._update_buttons(), self._show_details()))
        tb.Label(box, text="Host a game to wait for a player, or choose a game and Join. The ping is the time a "
                           "message takes to the host and back.", bootstyle="secondary", wraplength=WRAP,
                 justify="left").pack(anchor="w", pady=(6, 0))
        # The chosen game's settings (the ones its host chose)
        self.details = tb.Labelframe(box, text="Game settings", padding=8)
        self.details.pack(fill="x", pady=(8, 0))
        self._show_details()

    def _build_status(self, parent):
        box = self.status_box = self.section("Status", parent)
        box.pack(fill="x", pady=(10, 0))
        row = tb.Frame(box)
        row.pack(fill="x")
        self.state = tb.Label(row, text="Not started.", font=("", 11, "bold"), wraplength=WRAP - 90,
                              justify="left")
        self.state.pack(side="left", anchor="w", fill="x", expand=True)
        tb.Button(row, text="Stop", bootstyle="danger-outline", width=8, command=self.stop).pack(side="right",
                                                                                             anchor="n")
        self.side_labels = [tb.Label(box, text="", wraplength=WRAP, justify="left", bootstyle="secondary")
                            for _ in range(2)]
        for label in self.side_labels:
            label.pack(anchor="w")
        self.stats = tb.Label(box, text="", bootstyle="info", wraplength=WRAP, justify="left")
        self.stats.pack(anchor="w", pady=(4, 0))
        self.status_note = tb.Label(box, text=self.LINK_NOTE, bootstyle="secondary", wraplength=WRAP, justify="left")
        self.status_note.pack(anchor="w", pady=(8, 0))

    LINK_NOTE = ("Leaving a link game: quit it on one console; the other returns to its menu after a few seconds "
                 "(\"connection lost\"). A console reset needs the game booted again.")
    PAD_NOTE = ("Leaving: Leave the game (or Stop): the others play on, your kart without a driver. Every console "
                "plays the same game in step, so the slowest connection sets the pace for all.")

    # ---------------------------------------------------------------- the cartridge

    def on_show(self):
        self._cartridge_changed()
        self._watch()

    def on_device_changed(self):
        self._cartridge_changed()

    def on_devices_listed(self):
        self._cartridge_changed()

    def _cartridge_changed(self):
        serial = self.app.serial()
        self.cart_label.configure(text=serial or "none connected", bootstyle="default" if serial else "danger")
        if self.game and serial != self.game_serial:   # (another cartridge: its game not known yet)
            self._set_game(None, "Not detected yet.", "secondary")

    # ---------------------------------------------------------------- the game and its options

    def _set_game(self, patch, text, style):
        self.game = patch
        self.game_serial = self.app.serial() if patch else ""
        self.game_label.configure(text=text, bootstyle=style)
        for child in self.auto_frame.winfo_children():
            child.destroy()
        self.status_note.configure(text=self.PAD_NOTE if patch and patch.get("kind") == "padplay" else
                                   self.LINK_NOTE)
        if patch:
            # (a game with an automatic start: always the Manager's, its link play locked on the console)
            if patch.get("kind") == "padplay":
                hint = ("Keep the game at its main menu: when the host starts, the Manager sets the game up on "
                        "every player's console (2-4 players, each with one controller).")
            elif patch.get("auto_steps"):
                hint = "Keep the game at its main menu: the Manager chooses %s for you%s." % (
                    patch.get("link_play", "the link play"),
                    ", and when you host, starts the game with your settings" if patch.get("host_steps") else "")
            else:
                hint = "On the console: %s." % patch.get("link_play", "choose the game's link play")
            tb.Label(self.auto_frame, text=hint, bootstyle="secondary", wraplength=WRAP, justify="left").pack(
                anchor="w", pady=(4, 0))
            if self.watcher and self.watcher.hello.split("\t")[-1] != patch.get("title"):
                self.watcher.stop()             # (the lobby of this game)
                self.watcher = None
        self._watch()
        self._update_buttons()

    def _auto_on(self):
        return bool(self.game and self.game.get("auto_steps"))

    def _hint(self):
        return self.game.get("link_play", "the game's link play") if self.game else "the game's link play"

    def _opt_key(self, kind, opt):
        """Where an option's choice is remembered: kind "option" (the player's own: country...), "host" (the host's
        game settings: map...)."""
        return "netplay.%s.%s.%s" % (kind, self.game.get("title", "?"), opt["label"])

    def _choice(self, kind, opt):
        """An option as last chosen (Host a game / Join windows), else the game's default."""
        saved = self.app.settings.field(self._opt_key(kind, opt), "")
        return saved if saved in opt["values"] else opt.get("default", opt["values"][0])

    def _settings_info(self):
        """The host's game settings as text for the lobby ("Label: value|..."), empty if the game has none."""
        opts = self.game.get("host_options", []) if self.game else []
        return "|".join("%s: %s" % (o["label"], self._choice("host", o)) for o in opts)

    def _option_values(self):
        """The values for the relay's steps ($0...): the player's options, then the host's settings; each its code
        in the package ("codes"), else its position in the list."""
        values = []
        for kind, key in (("option", "options"), ("host", "host_options")):
            for opt in (self.game.get(key, []) if self.game else []):
                value = self._choice(kind, opt)
                k = opt["values"].index(value) if value in opt["values"] else 0
                values.append(opt["codes"][k] if opt.get("codes") else k)
        return (values + [0] * 16)[:16]

    def _options_dialog(self, title, intro, sections, ok_text, on_ok, ask_name=False):
        """A window with the player's name (ask_name), then sections of the game's options (heading, kind, options),
        two a row; OK remembers them and calls on_ok()."""
        win = tb.Toplevel(title=title)
        win.transient(self.winfo_toplevel())
        win.resizable(False, False)
        body = tb.Frame(win, padding=16)
        body.pack(fill="both", expand=True)
        tb.Label(body, text=title, font=("", 14, "bold")).grid(row=0, column=0, columnspan=4, sticky="w")
        tb.Label(body, text=intro, bootstyle="secondary", wraplength=440, justify="left").grid(
            row=1, column=0, columnspan=4, sticky="w", pady=(2, 8))
        row = 2
        name = None
        if ask_name:
            tb.Label(body, text="Your name", width=14, anchor="w").grid(row=row, column=0, sticky="w", pady=3)
            name = tb.Entry(body, width=28)
            name.insert(0, self._player_name())
            name.grid(row=row, column=1, columnspan=3, sticky="w", pady=3)
            row += 1
        combos = []
        for heading, kind, opts in sections:
            if not opts:
                continue
            if heading:
                tb.Label(body, text=heading, font=("", 10, "bold")).grid(row=row, column=0, columnspan=4, sticky="w",
                                                                         pady=(12, 2))
                row += 1
            for k, opt in enumerate(opts):
                width = max(8, max(len(v) for v in opt["values"]) + 2)
                combo = tb.Combobox(body, state="readonly", width=width, values=opt["values"])
                combo.set(self._choice(kind, opt))
                tb.Label(body, text=opt["label"], width=14, anchor="w").grid(row=row + k // 2, column=(k % 2) * 2,
                                                                             sticky="w", pady=3)
                combo.grid(row=row + k // 2, column=(k % 2) * 2 + 1, sticky="w", padx=(0, 16), pady=3)
                combos.append((kind, opt, combo))
            row += (len(opts) + 1) // 2
        bar = tb.Frame(body)
        bar.grid(row=row, column=0, columnspan=4, sticky="e", pady=(16, 0))

        def defaults():
            for kind, opt, combo in combos:
                combo.set(opt.get("default", opt["values"][0]))

        def ok():
            if name is not None:
                text = name.get().strip()
                if "\t" in text or "|" in text:
                    self.app.error("Your name can't contain a tab or '|'.")
                    return
                # (the cartridge's name is the default: remembered only when another one is typed)
                self.app.settings.set_field("netplay.name", "" if text in ("", self.app.serial()) else text)
            for kind, opt, combo in combos:
                self.app.settings.set_field(self._opt_key(kind, opt), combo.get())
            win.destroy()
            self._show_details()
            on_ok()
        tb.Button(bar, text="Defaults", bootstyle="secondary-outline", command=defaults).pack(side="left")
        tb.Button(bar, text="Cancel", bootstyle="secondary", command=win.destroy).pack(side="left", padx=(8, 0))
        tb.Button(bar, text=ok_text, bootstyle="success", width=10, command=ok).pack(side="left", padx=(8, 0))
        win.bind("<Escape>", lambda e: win.destroy())
        win.bind("<Return>", lambda e: ok())
        win.update_idletasks()
        top = self.winfo_toplevel()
        win.geometry("+%d+%d" % (top.winfo_rootx() + (top.winfo_width() - win.winfo_width()) // 2,
                                 top.winfo_rooty() + (top.winfo_height() - win.winfo_height()) // 3))
        win.grab_set()
        (name or win).focus_set()

    def detect(self, then=None):
        self.after_detect = then
        serial = self.app.serial()
        if not serial:
            self.app.error("Connect a cartridge first (chosen at the top of the window).")
            return
        if self.installing or not self._free():
            return
        self.game_label.configure(text="Detecting...", bootstyle="default")
        threading.Thread(target=self._detect, args=(serial,), daemon=True).start()

    def _detect(self, serial):
        patch, text, style = None, "", "danger"
        try:
            ml = netplay.Memlive(self.app.exe(), serial)
            try:
                if not ml.running:
                    text, style = "At the cartridge menu: boot the game from it.", "warning"
                else:
                    disc = netplay.read_disc(ml)
                    patch = netplay.patch_for(disc, self.patches) if disc else None
                    if not disc:
                        text, style = "No game booted from its disc (boot it with the PS1 DEV CART menu).", "warning"
                    elif patch:
                        text, style = "%s (%s): supported." % (patch["title"], disc), "success"
                        self.disc = disc
                    else:
                        text = "%s: no Net Play patch for this game." % disc
            finally:
                ml.close()
        except IOError as e:
            text = str(e)
        self._ui(self._detected, patch, text, style)

    def _detected(self, patch, text, style):
        self._set_game(patch, text, style)
        then, self.after_detect = getattr(self, "after_detect", None), None
        if then and patch:
            then()

    # ---------------------------------------------------------------- the lobby's list

    def _watch(self):
        """The lobby's open games, listed by the page itself (the relay running or not)."""
        if self.watcher or not self.patches:
            return
        title = (self.game or self.patches[0]).get("title", "?")
        self.watcher = netplay.LobbyWatcher(LOBBY_SERVER, self._player_name() + " (list)", title,
                                            lambda games: self._ui(self._show_games, games),
                                            lambda text: self._ui(self._watch_state, text))
        self.watcher.start()
        self.lobby_state.configure(text="Connecting to the lobby...", bootstyle="secondary")

    def _watch_state(self, text):
        if text:
            self.lobby_state.configure(text=text, bootstyle="warning")
        else:
            self.lobby_state.configure(text="Connected to the lobby", bootstyle="success")

    @staticmethod
    def _game_text(g):
        """A listed game's title and disc, e.g. "Command & Conquer: Red Alert - Retaliation (SLES_113.43)"."""
        title = g.get("game") or "?"
        return "%s (%s)" % (title, g["disc"]) if g.get("disc") else title

    def _show_games(self, games):
        """The open games (dicts: id, name, ping ms or None, game, disc, info); this window's own offer marked
        "(host)"."""
        self.games_shown = games
        sel = self.games.selection()
        self.games.delete(*self.games.get_children())
        for g in games:
            mine = self._is_mine(g["id"], g["name"])
            ping = g.get("ping")
            playing = self._in_progress(g)
            self.games.insert("", "end", iid=str(g["id"]),
                              tags=("mine",) if mine else ("playing",) if playing else (), values=(
                g["name"] + ("  (you)" if mine else ""), self._game_text(g),
                ("In progress" if playing else "Waiting") + (" %s" % g["players"] if g.get("players") else ""),
                "(host)" if mine else ("%d ms" % ping if ping is not None else "...")))
        if sel and self.games.exists(sel[0]):
            self.games.selection_set(sel[0])
        self._show_details()
        if self.watcher and self.watcher.rtt is not None:
            self.lobby_state.configure(text="Connected to the lobby (ping %d ms)" % self.watcher.rtt,
                                       bootstyle="success")
        self._update_buttons()

    def _is_mine(self, gid, name):
        """This window's own offer: its relay's lobby id, or (a server that gives none) its name while hosting."""
        if self.relay and self.pad_hosting:        # (pad play: the room this window opened)
            return name == self._player_name()
        return (gid == self.my_id) if self.my_id else (self.hosting and name == self.offer_name)

    def _chosen(self):
        """The game chosen in the list (its dict), None if none."""
        sel = self.games.selection()
        if not sel:
            return None
        return next((g for g in self.games_shown if g["id"] == int(sel[0])), None)

    @staticmethod
    def _in_progress(g):
        return g.get("status") == "in progress"

    def _selected_game(self):
        """The game chosen in the list (its dict), None if none, this window's own or in progress (not joined)."""
        g = self._chosen()
        return None if not g or self._is_mine(g["id"], g["name"]) or self._in_progress(g) else g

    def _show_details(self):
        """The chosen game's settings (its host's choice: map, credits...), label/value pairs in columns."""
        g = self._chosen()
        if not g:
            content = ("Choose a game in the list to see its settings.",)
        elif g.get("players"):
            # A pad play room: its settings as one line, the players in it and the characters taken
            taken = ", ".join(ctr_package.CHARACTERS[c] for c in g.get("chars", []) if c < len(ctr_package.CHARACTERS))
            content = (("Game", (g.get("info") or "?").rsplit(", up to", 1)[0]), ("Players", g["players"]),
                       ("Characters taken", taken or "none"))
        else:
            if self._is_mine(g["id"], g["name"]) and self.game:
                info = self._settings_info()          # (this window's own offer: its settings as chosen now)
            else:
                info = g.get("info") or ""
            pairs = []
            for part in info.split("|"):
                label, _, value = part.partition(":")
                if label.strip():
                    pairs.append((label.strip(), value.strip()))
            content = tuple(pairs) or ("The host chooses the settings on the console.",)
        if content == self.settings_shown:
            return                                    # (unchanged: not rebuilt, no flicker)
        self.settings_shown = content
        for child in self.details.winfo_children():
            child.destroy()
        if isinstance(content[0], str):
            tb.Label(self.details, text=content[0], bootstyle="secondary").grid(row=0, column=0, sticky="w")
            return
        for k, (label, value) in enumerate(content):
            row, col = k // SETTINGS_COLS, (k % SETTINGS_COLS) * 2
            tb.Label(self.details, text=label, bootstyle="secondary", width=17, anchor="w").grid(
                row=row, column=col, sticky="w", pady=1)
            tb.Label(self.details, text=value, font=("", 10, "bold"), width=max(10, len(value)), anchor="w").grid(
                row=row, column=col + 1, sticky="w", padx=(0, 24), pady=1)

    def _update_buttons(self):
        if self._pad():
            in_room = bool(self.relay)
            self.join_button.configure(state="normal" if self._selected_game() and not in_room else "disabled")
            self.host_button.configure(state="disabled" if in_room else "normal")
            self.leave_button.configure(state="normal" if in_room else "disabled")
            if self.pad_hosting and in_room and not self.pad_started:
                self.start_button.pack(side="left", padx=(8, 0))
                self.start_button.configure(state="normal" if len(self.pad_room) >= 2 else "disabled")
            else:
                self.start_button.pack_forget()
            return
        self.start_button.pack_forget()
        self.join_button.configure(state="normal" if self._selected_game() and not self.installing else "disabled")
        self.host_button.configure(state="disabled" if self.hosting or self.paired_with or self.installing else
                                   "normal")
        self.leave_button.configure(state="normal" if self._lobby_running() and self.paired_with else "disabled")

    # ---------------------------------------------------------------- helpers

    def _player_name(self):
        """The name shown in the lobby: the one typed in Host a game's window, else the cartridge's."""
        return self.app.settings.field("netplay.name", "") or self.app.serial() or "player"

    def _pair(self):
        """The console used: the cartridge chosen at the top of the window."""
        a = self.app.serial()
        if not a:
            self.app.error("Connect a cartridge first (chosen at the top of the window).")
            return None
        return (a,)

    def args(self):
        return ["-vlinklobby", LOBBY_SERVER]

    def _online_args(self):
        return ["-vlinklobby", LOBBY_SERVER, "-vlinkname", self._player_name()]

    def _relay_running(self):
        task = self.app.runner.task
        return bool(task and task.name == RELAY_TASK) or self.installing

    def _lobby_running(self):
        task = self.app.runner.task
        return bool(task and task.name == RELAY_TASK)

    def _free(self):
        """The cartridges free for this page's own transfers (no task of the app holding one)."""
        if self.app.runner.busy:
            task = self.app.runner.task
            if task.name == RELAY_TASK:
                self.app.error("The link is running: Stop it first.")
                return False
            if not self.app.confirm("%s is running. Stop it?" % task.name):
                return False
            self.app.stop_task()
            time.sleep(0.5)
        return True

    def _ui(self, fn, *args):
        self.app.after(0, fn, *args)

    def _set(self, label, text, style="default"):
        """A label changed from a thread (in the GUI's thread)."""
        self.app.after(0, lambda: label.configure(text=text, bootstyle=style))

    # ---------------------------------------------------------------- start / stop

    def start(self, auto_when=None):
        pair = self._pair()
        if not pair or self.installing or not self._free():
            return
        self.auto_when = auto_when
        self.last_error = ""        # (the relay's reason if it stops with an error)
        for label in self.side_labels:
            label.configure(text="")
        self.installing = True
        self.cancel.clear()
        self.state.configure(text="Installing the link patch...", bootstyle="default")
        self.stats.configure(text="")
        self._update_buttons()
        threading.Thread(target=self._install, args=(pair,), daemon=True).start()

    def _install(self, pair):
        sides = [netplay.Installer(self.app.exe(), s) for s in pair]
        done = [None] * len(pair)   # The patch package of each console once ready
        failed = None
        try:
            while not self.cancel.is_set() and not all(done) and not failed:
                for i, side in enumerate(sides):
                    if done[i]:
                        continue
                    try:
                        state, text, disc = side.check(self.patches)
                    except IOError as e:
                        state, text, disc = "wait", str(e), None
                        side.close()
                    self._set(self.side_labels[i], "%s: %s" % (pair[i], text),
                              {"ok": "success", "no": "danger"}.get(state, "secondary"))
                    if state == "ok":
                        done[i] = netplay.patch_for(disc, self.patches)
                        if i == 0:
                            self._ui(self._game_known, done[0], disc)
                    elif state == "no":
                        failed = text
                time.sleep(0.5)
        finally:
            for side in sides:
                side.close()
        self._ui(self._installed, pair, done, failed)

    def _game_known(self, patch, disc):
        self.disc = disc
        """The game found while installing (Detect not pressed, or another game): its options shown."""
        if not self.game or self.game.get("title") != patch.get("title"):
            self._set_game(patch, "%s (%s): supported." % (patch["title"], disc), "success")

    def _installed(self, pair, done, failed):
        self.installing = False
        self._update_buttons()
        if failed or not all(done):
            self.pending_join = 0
            self.state.configure(text="Not started: %s" % failed if failed else "Stopped.",
                                 bootstyle="danger" if failed else "default")
            return
        args = self._online_args()
        words = netplay.sym_words(done[0])
        if words:
            args += ["-vlinksym", words]
        if done[0].get("auto_steps"):
            args += ["-vlinkauto", done[0]["auto_steps"]]
        if done[0].get("host_steps"):
            args += ["-vlinkhostauto", done[0]["host_steps"]]
        if netplay.keep_words(done[0]):          # (kept while the relay runs: e.g. the menus' patches, no intro)
            args += ["-vlinkkeep", netplay.keep_words(done[0])]
        if done[0].get("watch"):
            args += ["-vlinkwatch", done[0]["watch"]]
        else:
            self.auto_when = None
        # (the relay's commands file: join, leave, and the automatic start go through it)
        self.lobby_cmds = {"join": 0, "leave": 0, "auto": 0}
        self._write_cmds()
        args += ["-vlinktune", self.tune_path, "-vlinkgame", done[0]["title"]]
        if self.disc:
            args += ["-vlinkdisc", self.disc]
        info = self._settings_info()
        if info:
            args += ["-vlinkinfo", info]
        self.state.configure(text="Connecting to the lobby...", bootstyle="default")
        os.makedirs(self.work_dir, exist_ok=True)
        try:
            self.relay_log = open(os.path.join(self.work_dir, "relay.log"), "a", buffering=1)
            self.relay_log.write("\n--- %s %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), " ".join(args)))
        except OSError:
            self.relay_log = None
        self.app.run(args, RELAY_TASK, device=pair[0], long_running=True, stdin=True, cwd=self.work_dir,
                     on_line=lambda line: (self._log(line), self._ui(self._relay_line, pair, line)),
                     on_done=lambda code, out: self._ui(self._relay_done, code))
        self._update_buttons()
        if self.auto_when == "now":
            self.auto_when = None
            self._auto_start()

    def _relay_line(self, pair, line):
        if line.startswith("VLINK lobby id"):
            self.my_id = int(line.split()[-1]) if line.split()[-1].isdigit() else 0
            self._show_games(self.games_shown)
            return
        if line.startswith("VLINK in the lobby as"):
            self.offer_name = line[len("VLINK in the lobby as"):].strip()
        if line.startswith("VLINK OFFER"):
            self.hosting = True
            self._show_games(self.games_shown)
        elif line.startswith("VLINK WITHDRAW"):
            self.hosting = False
            self._show_games(self.games_shown)
            if self.switch_to and "(joined)" not in line:     # (it was hosting: now the game chosen joined)
                gid, self.switch_to = self.switch_to, 0
                self.lobby_cmds["join"] = gid
                self._write_cmds()
                self.auto_when = "paired"
                self.state.configure(text="Joining...", bootstyle="default")
                return
        elif line.startswith("VLINK paired with"):
            if self.hosting:                    # (this console waited: the host, its settings set once linked)
                self._host_start()
            self.hosting = False
            self.paired_with = line[len("VLINK paired with"):].strip()
            self._show_games(self.games_shown)
        elif line.startswith("VLINK left the game") or "the other player left" in line:
            self.paired_with = ""
            self._update_buttons()
        # A game chosen in the list before the relay ran: joined once in the lobby
        if self.pending_join and line.startswith("VLINK GAMES"):
            self.lobby_cmds["join"] = self.pending_join
            self.pending_join = 0
            self._write_cmds()
            self.state.configure(text="Joining...", bootstyle="default")
            return
        # The automatic start, when its moment comes (only if the game has one and it is switched on)
        if (self.auto_when == "lobby" and line.startswith("VLINK GAMES")) or \
                (self.auto_when == "paired" and line.startswith("VLINK paired with")) or \
                (self.auto_when == "linked" and line.startswith("VLINK carrying between") and
                 "(the other computer)" in line):
            self.auto_when = None
            if self._auto_on():
                self._auto_start()
                return
        if line.startswith("VLINK AUTO"):
            self._auto_line(line)
            return
        if line.startswith("VLINK HOSTAUTO"):
            text = line.split(":", 1)[-1].strip()
            if text.startswith("started"):
                self.state.configure(text="Linked: setting the game up with your settings...", bootstyle="success")
            elif text == "done":
                self.state.configure(text="The game is starting with your settings.", bootstyle="success")
            elif text.startswith("failed"):
                self.state.configure(text="Couldn't set the game up: choose the settings on the console and press X "
                                          "to start.", bootstyle="warning")
            return
        m = STATS.search(line)
        if m:
            linked = int(m.group(2)) or int(m.group(5))
            fps = max(int(m.group(3)), int(m.group(6))) if linked else 0
            self.stats.configure(text="%s and %s linked: %d game frames a second" % (m.group(1), m.group(4),
                                                                                     min(fps, 25))
                                 if linked else "")
        elif line.startswith("VLINK in the lobby as"):
            self.state.configure(text="In the lobby." + ("" if self._auto_on() else
                                                         " On your console: %s, to offer the game." % self._hint()),
                                 bootstyle="success")
        elif line.startswith("VLINK paired with"):
            self.state.configure(text="Playing with %s." % line[len("VLINK paired with"):].strip() +
                                 ("" if self._auto_on() else " On your console: %s (if not already there)." %
                                  self._hint()), bootstyle="success")
        elif line.startswith("VLINK joining game"):
            self.state.configure(text="Joining...", bootstyle="default")
        elif line.startswith("VLINK server:"):
            self.state.configure(text=line[len("VLINK server:"):].strip(), bootstyle="warning")
        elif line.startswith("VLINK left the game"):
            self.state.configure(text="Back in the lobby.", bootstyle="default")
        elif "the other console is busy" in line:
            self.state.configure(text="The other console is busy (its game stopped for a moment): waiting for it...",
                                 bootstyle="warning")
        elif "the other console is back" in line:
            self.state.configure(text="In a link game.", bootstyle="success")
        elif "the other player left (" in line:
            self.state.configure(text="The other player left: back in the lobby.", bootstyle="warning")
        elif line.startswith("VLINK carrying between") and " and other " not in line:
            self.state.configure(text="The other player is there." +
                                 ("" if self._auto_on() else " On both consoles: %s." % self._hint()),
                                 bootstyle="success")
        elif line.startswith("VLINK OFFER"):
            # (the lobby's entry carries the host's settings as just chosen)
            if self.game:
                self.lobby_cmds["details"] = "%s\t%s" % (self.disc or "", self._settings_info())
                self._write_cmds()
        elif line.startswith("VLINK WITHDRAW") and "(joined)" not in line:
            self.state.configure(text="Your console left the link page (Triangle, or its wait ran out): the game is "
                                      "no longer offered. Host a game to offer it again.", bootstyle="warning")
        elif "the other player left" in line:
            self.state.configure(text="The other player left: host or join a game again.",
                                 bootstyle="warning")
        elif line.startswith(("Can't connect", "Unknown host", "VLINK no answer", "VLINK can't use port",
                              "VLINK the other computer runs")):
            if line.startswith(("Can't connect", "Unknown host")):
                self.last_error = "can't reach the Net Play lobby (its server isn't answering). Try again later."
            else:
                self.last_error = line.replace("VLINK ", "")
            self.state.configure(text=self.last_error[:1].upper() + self.last_error[1:], bootstyle="danger")
        elif "symmetric exchange on" in line:
            self.state.configure(text="In a link game.", bootstyle="success")
        elif "symmetric exchange off" in line:
            self.state.configure(text="The link game ended.", bootstyle="default")

    # ---------------------------------------------------------------- automatic start

    def _auto_start(self):
        """The relay told to start the game's link play on the console (the game's options as chosen)."""
        self.lobby_cmds["auto"] = self.lobby_cmds.get("auto", 0) + 1
        self.lobby_cmds["values"] = self._option_values()
        self._write_cmds()
        chosen = ", ".join(self._choice("option", o) for o in (self.game.get("options", []) if self.game else []))
        self.state.configure(text="Starting the link play on your console%s..." % (" (%s)" % chosen if chosen else ""),
                             bootstyle="default")

    def _host_start(self):
        """The host's part (the game's "host_steps"): its game settings set on the console and the game started,
        once the other console has linked (the setup screen)."""
        if not self.game or not self.game.get("host_steps") or not self._auto_on():
            return
        self.lobby_cmds["hostauto"] = self.lobby_cmds.get("hostauto", 0) + 1
        self.lobby_cmds["host_values"] = self._option_values()
        self._write_cmds()

    def _auto_line(self, line):
        text = line.split(":", 1)[-1].strip()
        if text == "done":
            self.state.configure(text="Your console is on the link page, waiting for the other player.",
                                 bootstyle="success")
        elif text.startswith("failed"):
            self.state.configure(text="Couldn't start the link play on the console: is the game at its main menu? "
                                      "(Or on the console: %s.)" % self._hint(), bootstyle="warning")
        elif text.startswith("not available"):
            self.state.configure(text="No automatic start for this game: on the console: %s." % self._hint(),
                                 bootstyle="warning")

    # ---------------------------------------------------------------- lobby actions

    def host(self):
        """Host a game in the lobby: the game detected if not yet, its settings chosen (its window), then the lobby
        connected if needed and the console's link play started (the offer)."""
        if not self.game:
            self.detect(then=self.host)
            return
        if self._pad():
            self._options_dialog(
                "Host a game",
                "%s: your name in the lobby, your character, and the game. Up to the number of players chosen can "
                "join; Start now plays with those who joined." % self.game.get("title", "The game"),
                [(None, "option", self.game.get("options", [])),
                 ("Game settings", "host", self.game.get("host_options", []))],
                "Host", lambda: self._pad_go(None), ask_name=True)
            return
        self._options_dialog(
            "Host a game",
            "%s: your name in the lobby, your side, and the game's settings. Players see the settings in the lobby "
            "before they join; your console starts the game with them." % self.game.get("title", "The game"),
            [(None, "option", self.game.get("options", [])),
             ("Game settings", "host", self.game.get("host_options", []))],
            "Host", self._host_go, ask_name=True)

    def _host_go(self):
        if self._lobby_running():
            info = self._settings_info()
            self.lobby_cmds["details"] = "%s\t%s" % (self.disc or "", info)
            self._write_cmds()
            if self._auto_on():
                self._auto_start()
            else:
                self.state.configure(
                    text="On your console: %s, to offer the game." % self._hint(),
                    bootstyle="default")
            return
        self.start(auto_when="lobby")

    def join(self):
        """Join the game chosen in the list: the game detected if not yet, the player's own options chosen (its
        window: e.g. country and color), then joined."""
        chosen = self._selected_game()
        if not chosen:
            g = self._chosen()
            if not g:
                self.app.error("Choose a game in the list.")
            elif self._in_progress(g) and not self._is_mine(g["id"], g["name"]):
                self.app.error("%s's game is in progress: choose a game that is waiting for a player." % g["name"])
            return              # (this window's own game: not joined)
        if not self.game:
            self.detect(then=self.join)
            return
        name = chosen["name"]
        if self._pad():
            if chosen.get("game") != self.game.get("title") or not chosen.get("players"):
                self.app.error("%s hosts %s: your console runs %s." % (name, chosen.get("game"),
                                                                       self.game.get("title")))
                return
            if self.relay:
                self.app.error("You are in a game already: Leave the game first.")
                return
            # (the character: one nobody in the room has)
            free = [c for i, c in enumerate(ctr_package.CHARACTERS) if i not in chosen.get("chars", [])]
            opt = dict(self.game.get("options", [{}])[0], values=free, default=free[0])
            self._options_dialog("Join %s's game" % name, "%s: %s. Your character (those taken by the players "
                                 "already in it aren't offered)." % (self.game.get("title"), chosen.get("info", "")),
                                 [(None, "option", [opt])], "Join", lambda: self._pad_go(chosen))
            return
        if self.game and chosen.get("game") and chosen["game"] != self.game.get("title"):
            self.app.error("%s hosts %s: your console runs %s." % (name, chosen["game"], self.game.get("title")))
            return
        if self.paired_with:
            self.app.error("You are playing with %s: leave that game first (Leave the game), then join another one."
                           % self.paired_with)
            return
        if self.hosting and not self.app.confirm("You are hosting a game, so you can't join another one.\n\nStop "
                                                 "hosting and join %s's game?" % name):
            return
        opts = self.game.get("options", [])
        if opts:
            self._options_dialog("Join %s's game" % name, "%s: your side." % self.game.get("title", "The game"),
                                 [(None, "option", opts)], "Join", lambda: self._join_go(chosen))
        else:
            self._join_go(chosen)

    def _join_go(self, chosen):
        gid, name = chosen["id"], chosen["name"]
        if self.hosting:
            # The offer ends when the console leaves its link page (its RTS down: "VLINK WITHDRAW"); then this game
            # is joined (see _relay_line) and the console's link play started again once paired
            self.switch_to = gid
            self.state.configure(text="Press Triangle on your console to leave the link page: then %s's game is "
                                      "joined." % name, bootstyle="warning")
            return
        if not self._lobby_running():
            if self._relay_running():
                return
            # Not connected yet: the patch installed and the relay connected, then this game joined (and the
            # console's link play started once paired)
            self.pending_join = gid
            self.start(auto_when="paired")
            if not self.installing:
                self.pending_join = 0
            return
        self.lobby_cmds["join"] = gid
        self._write_cmds()
        self.auto_when = "paired"
        self.state.configure(text="Joining...", bootstyle="default")

    def leave(self):
        if self.relay:
            self.relay.commands.put("quit")
            self.state.configure(text="Leaving the game...", bootstyle="default")
            return
        if not self._lobby_running():
            return
        self.lobby_cmds["leave"] += 1
        self.lobby_cmds["join"] = 0
        self._write_cmds()

    def _log(self, line):
        """The relay's output kept in the working folder (with the time)."""
        if self.relay_log:
            try:
                self.relay_log.write("%s %s\n" % (time.strftime("%H:%M:%S"), line.rstrip()))
            except (OSError, ValueError):
                pass

    def _write_cmds(self):
        """The relay's commands (read once a second, acted on when changed): join, leave, auto, hostauto, details."""
        os.makedirs(self.work_dir, exist_ok=True)
        values = " ".join(str(v) for v in self.lobby_cmds.get("values", [0] * 16))
        host_values = " ".join(str(v) for v in self.lobby_cmds.get("host_values", [0] * 16))
        try:
            with open(self.tune_path, "w") as f:
                f.write("pcopy 1\ntrace 86400\njoin %d\nleave %d\nauto %d %s\nhostauto %d %s\n" % (
                    self.lobby_cmds["join"], self.lobby_cmds["leave"], self.lobby_cmds.get("auto", 0), values,
                    self.lobby_cmds.get("hostauto", 0), host_values))
                if self.lobby_cmds.get("details"):
                    f.write("details %s\n" % self.lobby_cmds["details"])
        except OSError as e:
            self.app.error("Can't write %s: %s" % (self.tune_path, e))

    def _relay_done(self, code):
        self.auto_when = None
        self.pending_join = self.switch_to = 0
        self.my_id = 0
        self.hosting = False
        self.paired_with = ""
        self._show_games(self.games_shown)
        if code == 2:
            text, style = "Stopped: a console was reset (boot the game again, then host or join).", "warning"
        elif code:
            # (the relay's own reason when it gave one, e.g. the lobby not answering; else the USB)
            text, style = "Stopped: %s" % (self.last_error or "USB error (see the log)."), "danger"
        else:
            text, style = "Stopped.", "default"
        self.state.configure(text=text, bootstyle=style)
        self.stats.configure(text="")
        self._update_buttons()

    def stop(self):
        if self.installing:
            self.cancel.set()
            return
        if self.relay:
            self.relay.commands.put("quit")       # (the relay ends, then the task: see _pad_done)
            return
        task = self.app.runner.task
        if task and task.name == RELAY_TASK:
            self.app.stop_task()
        else:
            self.state.configure(text="Not started.", bootstyle="default")

    # ---------------------------------------------------------------- pad play (2-4 players, the buttons only)

    def _pad(self):
        return bool(self.game and self.game.get("kind") == "padplay")

    def _pad_go(self, chosen):
        """Host (chosen None: a room for the host's settings) or join a pad play room: the console's bridge started
        (ps1transfer -xserve, the page's task), then the relay (a thread) once the bridge is ready."""
        pair = self._pair()
        if not pair or self.relay or not self._free():
            return
        char_opt = self.game.get("options", [{}])[0]
        chosen_char = self._choice("option", char_opt)
        char = ctr_package.CHARACTERS.index(chosen_char) if chosen_char in ctr_package.CHARACTERS else 0
        choices = {o["label"]: self._choice("host", o) for o in self.game.get("host_options", [])}
        settings, most = ctr_package.settings_from_choices(choices)
        with socket.socket() as s:                # (a free port on this computer for the bridge)
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        self.pad_args = types.SimpleNamespace(
            lobby=LOBBY_SERVER, name=self._player_name(), host=0 if chosen else most,
            settings=json.dumps(settings), join=str(chosen["id"]) if chosen else "", char=char,
            bridge="127.0.0.1:%d" % port, emu="", delay=2, script=0, control="",
            package=os.path.join(netplay.NETPLAY_DIR, "crash_team_racing.json"))
        self.pad_hosting = not chosen
        self.pad_started = False
        self.pad_room = []
        self.last_error = ""
        for label in self.side_labels:
            label.configure(text="")
        self.stats.configure(text="")
        self.state.configure(text="Connecting to the console...", bootstyle="default")
        os.makedirs(self.work_dir, exist_ok=True)
        try:
            self.relay_log = open(os.path.join(self.work_dir, "relay.log"), "a", buffering=1)
            self.relay_log.write("\n--- %s pad play %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"),
                                                            "host" if not chosen else "join %s" % chosen["id"]))
        except OSError:
            self.relay_log = None
        self.relay = True                         # (taken: the relay itself once the bridge is ready)
        emu = getattr(self, "pad_emu_dir", "") or os.environ.get("PS1DEVCART_PADPLAY_EMU", "")
        if emu:
            # Tests without a console: an emulator running ps1-linkplay/padlink/padlink_emu.lua instead of the
            # cartridge (no ps1transfer)
            self.pad_args.emu, self.pad_args.bridge = emu, ""
            threading.Thread(target=self._pad_relay, daemon=True).start()
            self._update_buttons()
            return
        self.app.run(["-xserve", str(port)], RELAY_TASK, device=pair[0], long_running=True, cwd=self.work_dir,
                     on_line=lambda line: (self._log(line), self._ui(self._pad_bridge_line, line)),
                     on_done=lambda code, out: self._ui(self._pad_bridge_done, code))
        self._update_buttons()

    def _pad_bridge_line(self, line):
        if line.startswith("XSERVE") and "ready" in line and self.relay is True:
            threading.Thread(target=self._pad_relay, daemon=True).start()
        elif line.startswith("XSERVE can't"):
            self.last_error = line[len("XSERVE "):]

    def _pad_relay(self):
        try:
            r = padrelay.Relay(self.pad_args)
        except (IOError, OSError) as e:
            msg = str(e)
            if "refused" in msg or "connect" in msg.lower():
                msg = "can't reach the Net Play lobby (its server isn't answering). Try again later."
            self._ui(self._pad_done, "ERROR " + msg)
            return
        r.on_status = lambda text: (self._log("STATUS " + text), self._ui(self._pad_status, text))
        self.relay = r
        self._ui(self._update_buttons)
        last = padrelay.run_relay(r)
        self._ui(self._pad_done, last)

    def _pad_status(self, text):
        kind, _, rest = text.partition(" ")
        names = ctr_package.CHARACTERS
        if kind == "ROOM" and isinstance(self.relay, padrelay.Relay):
            self.pad_room = list(self.relay.members)
            who = ", ".join("%s (%s)" % (m[1], names[int(m[2])] if m[2].isdigit() and int(m[2]) < len(names)
                                          else "?") for m in self.pad_room)
            most = self.pad_args.host
            if self.pad_hosting:
                more = ("Start now plays with the players here; the game starts by itself when %d are in." % most
                        if len(self.pad_room) >= 2 else "Waiting for players to join (up to %d)." % most)
                self.state.configure(text="Your game is in the lobby: %s. %s" % (who, more), bootstyle="success")
            else:
                self.state.configure(text="In %s's game: %s. Waiting for the host to start." % (
                    self.pad_room[0][1] if self.pad_room else "the host", who), bootstyle="success")
        elif kind == "START":
            self.pad_started = True
            self.state.configure(text="The game starts: you are %s. Installing the pad play driver on your "
                                      "console..." % rest.split(",")[0], bootstyle="default")
        elif kind == "WAITING":
            texts = {"for the game to boot": "Waiting for Crash Team Racing to boot on your console...",
                     "for the console's main menu": "Waiting for the game's main menu on your console (press a "
                                                    "button to leave the demo)."}
            self.state.configure(text=texts.get(rest, rest), bootstyle="warning")
        elif kind == "INSTALLED":
            self.state.configure(text="Driver installed.", bootstyle="default")
        elif kind == "SET":
            self.state.configure(text="Playing: %s." % rest.replace("UP ", "", 1).rsplit(", up to", 1)[0],
                                 bootstyle="success")
        elif kind == "PLAYING":
            m = re.search(r"race (\d+) frame (\d+) \(([\d.]+) frames/s\), waits (\d+), desyncs (\d+)", rest)
            if m:
                self.stats.configure(text="Race %s: %d game frames a second%s" % (
                    m.group(1), round(float(m.group(3))),
                    ", out of step %s times (see the log)" % m.group(5) if m.group(5) != "0" else ""))
        elif kind == "DESYNC":
            self.side_labels[1].configure(text="The consoles went out of step (%s): the log has the details." % rest,
                                          bootstyle="danger")
        elif kind == "LEFT":
            self.side_labels[0].configure(text="%s left the game: the others play on." % rest.split(" ", 2)[-1],
                                          bootstyle="warning")
        elif kind == "ERROR":
            self.last_error = rest
            self.state.configure(text=rest[:1].upper() + rest[1:], bootstyle="danger")
        self._update_buttons()

    def start_now(self):
        if isinstance(self.relay, padrelay.Relay):
            self.relay.commands.put("start")
            self.state.configure(text="Starting with the players here...", bootstyle="default")

    def _pad_done(self, last):
        """The relay ended (its last status, "" when stopped): the bridge stopped too."""
        self.relay = None
        self.pad_hosting = self.pad_started = False
        self.pad_room = []
        task = self.app.runner.task
        if task and task.name == RELAY_TASK:
            self.app.stop_task()
        if last.startswith("ERROR"):
            self.state.configure(text="Stopped: %s" % last[len("ERROR "):], bootstyle="danger")
        elif last.startswith("CLOSED"):
            self.state.configure(text="The host left: the game was called off.", bootstyle="warning")
        else:
            self.state.configure(text="Stopped.", bootstyle="default")
        self.stats.configure(text="")
        self._show_games(self.games_shown)
        self._update_buttons()

    def _pad_bridge_done(self, code):
        """The bridge (ps1transfer) ended: the relay told to stop if it still runs (the USB gone)."""
        if isinstance(self.relay, padrelay.Relay):
            self.relay.commands.put("quit")
        elif self.relay is True:                  # (it never got ready: the cartridge or the port)
            self.relay = None
            self.state.configure(text="Stopped: %s" % (self.last_error or "the cartridge didn't answer (USB)."),
                                 bootstyle="danger")
            self._update_buttons()
