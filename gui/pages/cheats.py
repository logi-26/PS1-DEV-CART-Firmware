"""
PS1 DEV CART GUI: Cheats page. Finds a value in the RAM of the running program (a game booted from its disc too) and
keeps it where you want it, like a cheat cartridge.

Search: each search reads the whole RAM in one go (ps1transfer -memlive "dump", about 4 seconds while a game runs)
and keeps the addresses whose value matches: equal to a value, or changed, increased... since the search before
(the value not known: lives shown as hearts). Lose a life, search "Decreased", and a few rounds later the address is
found. The results are read live once there are few of them.

Cheats: an address, a type and a value. On: written again a few times a second (frozen, as a cheat cartridge's codes
are); "Write once" writes it once. A list per game, saved in the settings folder (cheats\\GAME.json). GameShark /
Action Replay codes in and out: 80XXXXXX YYYY (16-bit), 30XXXXXX 00YY (8-bit).

Everything goes through the live link (shared with the Memory page's watch list and the Remote Controller).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import json
import os
import re
import tkinter as tk
from tkinter import filedialog, simpledialog

import numpy as np
import ttkbootstrap as tb

import cheatdb
import codeengine
from codeengine import CHEAT_MAX, FEATURE_CHEATS, HOOK_PARAMS
from livelink import FRAME_CAPTURE, LIVE_LINK
from pages.base import Page
from settings import config_dir

RAM, RAM_SIZE = 0x80000000, 0x200000
KERNEL = 0x10000                # The BIOS kernel's area (skipped by default)
TYPES = {"u8": ("<u1", 1), "s8": ("<i1", 1), "u16": ("<u2", 2), "s16": ("<i2", 2), "u32": ("<u4", 4), "s32": ("<i4", 4)}
WITH_VALUE = ["Equal to", "Not equal to", "Greater than", "Less than", "Increased by", "Decreased by"]
COMPARE = WITH_VALUE + ["Changed", "Unchanged", "Increased", "Decreased", "Any (unknown value)"]
SHOWN = 500                     # Results listed at most
LIVE_RESULTS = 20               # Results read live when there are this many at most
LIVE_MS = 1000
FREEZE_MS = 100                 # The cheats that are on: written again (once the last writes are done)
KEEP_MS = 15000                 # The live link kept this long after the page last used it (the next search reuses it)
CODE = re.compile(r"^\s*([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{4})\s*$")
DISC_ID, DISC_ID_MAGIC = 0x8000DEE0, 0x43534944     # The booted disc's executable (firmware, see usb.h: DiscId)
FEATURE_DISC_ID = 0x10          # The booted disc's name kept (firmware 1.1)
CHEATS_FIRMWARE = (1, 1)        # The first firmware with the code engine, the disc's name and the cheat screen
ENGINE_TYPES = {0x80, 0x30, 0xD0, 0xD1, 0xD2, 0xD3, 0xE0, 0xE1, 0xE2, 0xE3, 0x50, 0xC0}
TESTS = {0: lambda a, b: a == b, 1: lambda a, b: a != b, 2: lambda a, b: a < b, 3: lambda a, b: a > b}


def version_text(v):
    return "%d.%d" % v if v else "?"


def rom_version(path):
    """The firmware version of a ROM file ("Firmware: x.y" in it), None if not found."""
    try:
        with open(path, "rb") as f:
            rom = f.read()
        i = rom.find(b"Firmware: ")
        return tuple(int(x) for x in rom[i + 10:i + 20].split(b"\0")[0].decode().split(".")) if i >= 0 else None
    except (OSError, ValueError):
        return None


def gs_lines(codes):
    """GameShark lines -> [(first word, value)], None if one isn't a code."""
    out = []
    for line in codes.split("\n"):
        m = CODE.match(line)
        if not m:
            return None
        out.append((int(m.group(1), 16), int(m.group(2), 16)))
    return out


def engine_ok(codes):
    """The code engine does every code of these lines."""
    lines = gs_lines(codes)
    return bool(lines) and all(c >> 24 in ENGINE_TYPES for c, _v in lines)


def gs_ops(codes):
    """GameShark lines (cheatdb) -> [("w", size, adrs, value) / ("if", size, test, adrs, value)], None if a line
    isn't one the page does (80, 30 writes; D0-D3, E0-E3 tests of the next line)."""
    ops = []
    for line in codes.split("\n"):
        m = CODE.match(line)
        if not m:
            return None
        code, v = int(m.group(1), 16), int(m.group(2), 16)
        kind, adrs = code >> 24, RAM | (code & 0x1FFFFF)
        if kind == 0x80:
            ops.append(("w", 2, adrs, v))
        elif kind == 0x30:
            ops.append(("w", 1, adrs, v & 0xFF))
        elif 0xD0 <= kind <= 0xD3:
            ops.append(("if", 2, kind & 3, adrs, v))
        elif 0xE0 <= kind <= 0xE3:
            ops.append(("if", 1, kind & 3, adrs, v & 0xFF))
        else:
            return None
    return ops


def value_text(value, typ):
    size = TYPES[typ][1]
    return "%d (0x%0*X)" % (value, size * 2, value & ((1 << (size * 8)) - 1))


def to_bytes(value, typ):
    size = TYPES[typ][1]
    return (value & ((1 << (size * 8)) - 1)).to_bytes(size, "little")


def codes_of(cheat):
    """The GameShark codes of a cheat: 30 (8-bit), 80 (16-bit), two 80 for 32 bits."""
    a, v, size = cheat["adrs"] & 0xFFFFFF, cheat["value"], TYPES[cheat["type"]][1]
    if size == 1:
        return ["30%06X 00%02X" % (a, v & 0xFF)]
    if size == 2:
        return ["80%06X %04X" % (a, v & 0xFFFF)]
    return ["80%06X %04X" % (a, v & 0xFFFF), "80%06X %04X" % (a + 2, (v >> 16) & 0xFFFF)]


class CheatsPage(Page):
    title = "Cheats"
    needs_devcart = "all"

    def build(self):
        self.cur = None             # The RAM read by the last search, as the type searched (numpy)
        self.cand = None            # Indexes in it of the addresses still matching (numpy)
        self.search_type = None
        self.dumping = None         # "new" / "next" while the RAM is read
        self.want_link = None       # What to do once the live link is ready
        self.reads = {}             # Address -> size of the live reads asked, not answered yet
        self.writes = 0             # Writes asked, not answered yet
        self.live_values = {}       # Address -> value read live (results)
        self.cheats = []            # {"name", "adrs", "type", "value", "on"}
        self.known = []             # Known cheats of the game shown (database): {"id", "name", "codes", "ops", "on"}
        self.known_serial = None
        self.tests = {}             # Address -> size of the tests' reads (known cheats), not answered yet
        self.test_values = {}       # Address -> value read for the tests
        self.detecting = False
        self.engine = None          # The firmware's code engine: None not known yet (the link reads it), True, False
        self.timer = None
        self.ticks = 0              # (the results are read every LIVE_MS)
        self.unused = 0             # Ticks since the page last used the link
        self.dump_path = os.path.join(config_dir(), "cheats", "ram.bin")

        tb.Label(self, text=self.title, font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="Known cheats: the cheats of the game in the drive, switched on from here.  My cheats: your "
                            "own, found with the search on the left (search a value such as your lives, change it in "
                            "the game, search again until a few addresses are left: about 4 seconds a search).  On "
                            "the cartridge: cheats kept on the cartridge, chosen on the console without this computer.",
                 wraplength=930, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 6))

        panes = tb.Panedwindow(self, orient="horizontal")
        panes.pack(fill="both", expand=True)
        left = tb.Frame(panes, padding=(0, 0, 8, 0))
        right = tb.Frame(panes)
        panes.add(left, weight=1)
        panes.add(right, weight=1)
        self._build_search(left)
        self.tabs = tb.Notebook(right)
        self.tabs.pack(fill="both", expand=True)
        known_tab = tb.Frame(self.tabs, padding=8)
        mine_tab = tb.Frame(self.tabs, padding=8)
        cart_tab = tb.Frame(self.tabs, padding=8)
        self.tabs.add(known_tab, text="  Known cheats  ")
        self.tabs.add(mine_tab, text="  My cheats  ")
        self.tabs.add(cart_tab, text="  On the cartridge  ")
        self.cart_tab = cart_tab
        self._build_known(known_tab)
        self._build_cheats(mine_tab)
        self._build_cart(cart_tab)
        self.app.live_listen(self)
        self.load_games()

    # ---------------------------------------------------------------- layout

    def _build_search(self, box):
        s = self.app.settings
        sec = self.section("Search", box)
        sec.pack(fill="x")
        row = tb.Frame(sec)
        row.pack(fill="x")
        tb.Label(row, text="Type").pack(side="left")
        self.type = tb.Combobox(row, state="readonly", width=5, values=list(TYPES))
        self.type.set(s.field("cheats.type", "u8"))
        self.type.pack(side="left", padx=(4, 10))
        self.compare = tb.Combobox(row, state="readonly", width=19, values=COMPARE)
        self.compare.set(s.field("cheats.compare", "Equal to"))
        self.compare.pack(side="left")
        self.compare.bind("<<ComboboxSelected>>", lambda e: self.compare_changed())
        self.value = tb.Entry(row, width=10)
        self.value.pack(side="left", padx=(6, 0))
        self.value.bind("<Return>", lambda e: self.next_search() if self.cand is not None else self.new_search())
        row = tb.Frame(sec)
        row.pack(fill="x", pady=(6, 0))
        tb.Button(row, text="New search", bootstyle="primary", command=self.new_search).pack(side="left")
        self.next_button = tb.Button(row, text="Next search", bootstyle="success", command=self.next_search,
                                     state="disabled")
        self.next_button.pack(side="left", padx=6)
        self.skip_kernel = tk.BooleanVar(value=s.field("cheats.skip_kernel", True))
        tb.Checkbutton(row, text="Skip the BIOS area (first 64 KB)", variable=self.skip_kernel,
                       bootstyle="round-toggle").pack(side="left", padx=6)
        self.status = tb.Label(sec, text="No search yet.", bootstyle="secondary")
        self.status.pack(anchor="w", pady=(6, 0))

        frame = tb.Frame(box)
        frame.pack(fill="both", expand=True, pady=(6, 4))
        self.results = tb.Treeview(frame, columns=("adrs", "value", "prev"), show="headings", height=12)
        for key, text, width in (("adrs", "Address", 90), ("value", "Value", 150), ("prev", "Before", 150)):
            self.results.heading(key, text=text, anchor="w")
            self.results.column(key, width=width, anchor="w", stretch=key != "adrs")
        self.results.tag_configure("changed", foreground="#f0ad4e")
        scroll = tb.Scrollbar(frame, orient="vertical", command=self.results.yview)
        self.results.configure(yscrollcommand=scroll.set)
        self.results.pack(side="left", fill="both", expand=True)
        scroll.pack(side="left", fill="y")
        self.results.bind("<Double-1>", lambda e: self.add_results())
        row = tb.Frame(box)
        row.pack(fill="x")
        tb.Button(row, text="Add to the cheats ▶", bootstyle="primary-outline", command=self.add_results).pack(
            side="left")
        tb.Label(row, text="(or double-click)", bootstyle="secondary").pack(side="left", padx=6)
        self.compare_changed()

    def _build_known(self, box):
        row = tb.Frame(box)
        row.pack(fill="x")
        tb.Button(row, text="Detect the game", bootstyle="primary", command=self.detect).pack(side="left")
        tb.Label(row, text="or find one").pack(side="left", padx=(10, 4))
        self.find = tb.Combobox(row, width=30)
        self.find.pack(side="left", fill="x", expand=True)
        self.find.bind("<Return>", lambda e: self.find_games())
        self.find.bind("<<ComboboxSelected>>", lambda e: self.found_selected())
        self.found = []
        self.game_label = tb.Label(box, text="", font=("", 10, "bold"))
        self.game_label.pack(anchor="w", pady=(6, 0))
        self.known_status = tb.Label(box, text="", bootstyle="secondary", wraplength=440, justify="left")
        self.known_status.pack(anchor="w", pady=(0, 4))
        frame = tb.Frame(box)
        frame.pack(fill="both", expand=True)
        self.known_list = tb.Treeview(frame, columns=("on", "name", "codes"), show="headings", height=12)
        for key, text, width, anchor in (("on", "On", 40, "center"), ("name", "Cheat", 220, "w"),
                                         ("codes", "Codes", 150, "w")):
            self.known_list.heading(key, text=text, anchor=anchor)
            self.known_list.column(key, width=width, anchor=anchor, stretch=key == "name")
        self.known_list.tag_configure("off", foreground="#888888")
        scroll = tb.Scrollbar(frame, orient="vertical", command=self.known_list.yview)
        self.known_list.configure(yscrollcommand=scroll.set)
        self.known_list.pack(side="left", fill="both", expand=True)
        scroll.pack(side="left", fill="y")
        self.known_list.bind("<Double-1>", lambda e: self.toggle_known())
        row = tb.Frame(box)
        row.pack(fill="x", pady=(4, 0))
        tb.Button(row, text="On / off", bootstyle="success-outline", command=self.toggle_known).pack(side="left")
        tb.Button(row, text="Copy the codes", bootstyle="secondary-outline", command=self.copy_known).pack(
            side="left", padx=4)
        tb.Button(row, text="Import cheat files...", bootstyle="secondary-outline", command=self.import_files).pack(
            side="right")
        tb.Button(row, text="Put this game on the cartridge ▶", bootstyle="primary-outline",
                  command=self.cart_put_shown).pack(side="left", padx=4)
        tb.Label(box, text="1. Boot the game from its disc (hold Start + Select on the PS1 DEV CART menu).  2. Detect "
                           "the game: its cheats are listed (or find any game by its title).  3. Double-click a "
                           "cheat to switch it on: the console applies it at once.  The cheats come from a database "
                           "on this computer, built from your cheat files (Import cheat files...).",
                 wraplength=440, justify="left", bootstyle="secondary").pack(anchor="w", pady=(6, 0))
        self.show_counts()

    def _build_cart(self, box):
        tb.Label(box, text="The cheats kept on the cartridge itself, to use without this computer: on the console, "
                           "hold Select + Triangle on the menu, tick the cheats of the disc in the drive with X, "
                           "then Start boots the game with them.", wraplength=440, justify="left",
                 bootstyle="secondary").pack(anchor="w")
        row = tb.Frame(box)
        row.pack(fill="x", pady=(6, 0))
        tb.Button(row, text="Read the cartridge", bootstyle="primary", command=self.cart_read).pack(side="left")
        self.cart_read_status = tb.Label(row, text="Not read yet.", bootstyle="secondary")
        self.cart_read_status.pack(side="left", padx=8)
        self.cart_bar = tb.Progressbar(box, maximum=cheatdb.ROM_CHEATS_SIZE, bootstyle="info")
        self.cart_bar.pack(fill="x", pady=(6, 0))
        self.cart_space = tb.Label(box, text="", bootstyle="secondary")
        self.cart_space.pack(anchor="w")
        frame = tb.Frame(box)
        frame.pack(fill="both", expand=True, pady=(4, 0))
        columns = (("state", "", 110, "w"), ("title", "Game", 190, "w"), ("serial", "Serial", 90, "w"),
                   ("cheats", "Cheats", 50, "e"), ("size", "Size", 60, "e"))
        self.cart_list_view = tb.Treeview(frame, columns=[c[0] for c in columns], show="headings", height=10)
        for key, text, width, anchor in columns:
            self.cart_list_view.heading(key, text=text, anchor=anchor)
            self.cart_list_view.column(key, width=width, anchor=anchor, stretch=key == "title")
        self.cart_list_view.tag_configure("add", foreground="#5cb85c")
        self.cart_list_view.tag_configure("remove", foreground="#e74c3c")
        self.cart_list_view.tag_configure("full", foreground="#f0ad4e")
        scroll = tb.Scrollbar(frame, orient="vertical", command=self.cart_list_view.yview)
        self.cart_list_view.configure(yscrollcommand=scroll.set)
        self.cart_list_view.pack(side="left", fill="both", expand=True)
        scroll.pack(side="left", fill="y")
        row = tb.Frame(box)
        row.pack(fill="x", pady=(4, 0))
        tb.Label(row, text="Add a game").pack(side="left")
        self.cart_find = tb.Combobox(row, width=28)
        self.cart_find.pack(side="left", fill="x", expand=True, padx=4)
        self.cart_find.bind("<Return>", lambda e: self.cart_find_games())
        self.cart_find.bind("<<ComboboxSelected>>", lambda e: self.cart_found_selected())
        self.cart_found = []
        tb.Button(row, text="Remove", bootstyle="secondary-outline", command=self.cart_remove).pack(side="left")
        tb.Label(box, text="(type a title, Enter, then choose it)", bootstyle="secondary").pack(anchor="w")
        row = tb.Frame(box)
        row.pack(fill="x", pady=(6, 0))
        self.cart_write_button = tb.Button(row, text="Write to the cartridge", bootstyle="warning",
                                           command=self.cart_write)
        self.cart_write_button.pack(side="left")
        self.cart_note = tb.Label(row, text="", bootstyle="secondary", wraplength=300, justify="left")
        self.cart_note.pack(side="left", padx=8)
        self.cart_flashed = None        # The games on the cartridge (read), None: not read yet
        self.cart_refresh()

    def _build_cheats(self, box):
        row = tb.Frame(box)
        row.pack(fill="x")
        tb.Label(row, text="Game").pack(side="left")
        self.game = tb.Combobox(row, width=28)
        self.game.pack(side="left", padx=6, fill="x", expand=True)
        self.game.bind("<<ComboboxSelected>>", lambda e: self.load_game())
        self.game.bind("<Return>", lambda e: self.load_game())
        tb.Button(row, text="Load", bootstyle="secondary-outline", command=self.load_game).pack(side="left")
        self.game_status = tb.Label(box, text="", bootstyle="secondary")
        self.game_status.pack(anchor="w", pady=(2, 4))

        frame = tb.Frame(box)
        frame.pack(fill="both", expand=True)
        columns = (("on", "On", 40, "center"), ("name", "Cheat", 150, "w"), ("adrs", "Address", 90, "w"),
                   ("type", "Type", 50, "w"), ("value", "Value", 140, "w"))
        self.list = tb.Treeview(frame, columns=[c[0] for c in columns], show="headings", height=12)
        for key, text, width, anchor in columns:
            self.list.heading(key, text=text, anchor=anchor)
            self.list.column(key, width=width, anchor=anchor, stretch=key == "name")
        scroll = tb.Scrollbar(frame, orient="vertical", command=self.list.yview)
        self.list.configure(yscrollcommand=scroll.set)
        self.list.pack(side="left", fill="both", expand=True)
        scroll.pack(side="left", fill="y")
        self.list.bind("<Double-1>", self.cheat_double)

        row = tb.Frame(box)
        row.pack(fill="x", pady=(4, 0))
        tb.Button(row, text="On / off", bootstyle="success-outline", command=self.toggle).pack(side="left")
        tb.Button(row, text="Write once", bootstyle="warning-outline", command=self.write_once).pack(side="left", padx=4)
        tb.Button(row, text="Value...", bootstyle="secondary-outline", command=self.edit_value).pack(side="left")
        tb.Button(row, text="Name...", bootstyle="secondary-outline", command=self.rename).pack(side="left", padx=4)
        tb.Button(row, text="Remove", bootstyle="secondary-outline", command=self.remove).pack(side="left")
        row = tb.Frame(box)
        row.pack(fill="x", pady=(4, 0))
        tb.Button(row, text="Add codes...", bootstyle="secondary-outline", command=self.add_codes).pack(side="left")
        tb.Button(row, text="Copy as codes", bootstyle="secondary-outline", command=self.copy_codes).pack(side="left",
                                                                                                         padx=4)
        self.link_status = tb.Label(row, text="", bootstyle="secondary")
        self.link_status.pack(side="left", padx=8)
        tb.Label(box, text="On: applied by the console at every interrupt (the firmware's code engine: they stay on "
                           "until it's reset), or written 10 times a second by this page with an older firmware. "
                           "Double-click the On column to switch, the value to change it. Codes: GameShark / Action "
                           "Replay, 80XXXXXX YYYY (16-bit), 30XXXXXX 00YY (8-bit).", wraplength=440, justify="left",
                 bootstyle="secondary").pack(anchor="w", pady=(6, 0))

    # ---------------------------------------------------------------- live link

    def link(self, then):
        """then() once the live link is ready (started if needed)."""
        task = self.app.live_task()
        if task == FRAME_CAPTURE:
            return self.app.error("The frame capture runs (Frame Inspector): stop it first.")
        if task == LIVE_LINK and self.app.live_ready:
            then()
            return None
        self.want_link = then
        self.status_before = self.status.cget("text")
        self.status.configure(text="Connecting...")
        self.app.live_start()
        return None

    def using_live(self):
        on = any(c["on"] for c in self.cheats) or any(k["on"] and self.usable(k) for k in self.known)
        # (with the code engine, the console applies the cheats itself: the link only writes their table)
        cheats = on and not self.engine or self.engine and self.table() != self.app.engine_sent
        return bool(self.dumping or self.want_link or self.detecting or self.writes or cheats or self.live_results())

    def usable(self, k):
        """A known cheat can be applied: by the page (its codes are writes and tests), or by the code engine."""
        return k["ops"] is not None or (self.engine is not False and k["engine"])

    def table(self):
        """The code engine's table with the cheats that are on (codeengine.py: with the other pages' codes)."""
        lines = []
        for c in self.cheats:
            if c["on"]:
                lines += gs_lines("\n".join(codes_of(c)))
        for k in self.known:
            if k["on"] and self.usable(k):
                lines += gs_lines(k["codes"])
        codeengine.set_codes(self.app, "cheats", lines[:CHEAT_MAX])
        return codeengine.table(self.app)

    def live_stopped(self):
        if self.dumping or self.want_link:
            self.status.configure(text="The link stopped.")
        self.dumping = self.want_link = None
        self.reads, self.writes, self.tests, self.detecting = {}, 0, {}, False
        self.engine = None
        self.link_status.configure(text="")

    def live_line(self, line):
        fields = line.split()
        if line.startswith("MEMLIVE ready"):
            if self.want_link:
                then, self.want_link = self.want_link, None
                if self.status.cget("text") == "Connecting...":
                    self.status.configure(text=self.status_before)
                if fields[-1] == "0":
                    if self.game_label.cget("text") == "Detecting...":
                        self.game_label.configure(text="No game running on %s" % self.app.serial())
                        self.known_status.configure(text="Boot the game from its disc first: hold Start + Select on "
                                                         "the PS1 DEV CART menu, then Detect the game.")
                    else:
                        self.status.configure(text="No program runs (the PS1 DEV CART menu): start the game first.")
                    self.app.live_release()
                else:
                    then()
            self.engine, self.app.engine_sent = None, None
            self.reads[HOOK_PARAMS] = 4
            self.app.live_send("read %08X 4" % HOOK_PARAMS)
            self.tick()
        elif line.startswith("MEMLIVE disconnected"):
            self.status.configure(text="Console disconnected.")
        elif line.startswith("MEMDUMP") and self.dumping:
            self.dump_done()
        elif line.startswith("MEMERR dump") and self.dumping:
            self.dumping = None
            self.status.configure(text=line[7:])
        elif line.startswith("MEMREAD") and len(fields) == 3:
            adrs, data = int(fields[1], 16), bytes.fromhex(fields[2])
            if adrs == HOOK_PARAMS and adrs in self.reads:
                del self.reads[adrs]
                self.engine = bool(data[3] & FEATURE_CHEATS)
                self.fill_known()
                self.tick()
            elif adrs == DISC_ID and self.detecting:
                self.reads.pop(adrs, None)
                self.detected(data)
            elif adrs in self.tests and len(data) == self.tests[adrs]:
                del self.tests[adrs]
                self.test_values[adrs] = int.from_bytes(data, "little")
                if not self.tests:
                    self.known_write(self.test_values)
            elif adrs in self.reads:
                self.read_done(adrs, data)
        elif line.startswith("MEMWROTE"):
            self.writes = max(0, self.writes - 1)
        elif line.startswith("MEMERR") and (self.reads or self.writes or self.tests):
            self.reads, self.writes, self.tests = {}, 0, {}
            if self.detecting:
                self.detecting = False
                self.game_label.configure(text="The game couldn't be read: %s" % line[7:])

    def tick(self):
        """Every FREEZE_MS while the page uses the link: the cheats that are on written again, the results read."""
        if self.timer:
            self.after_cancel(self.timer)
            self.timer = None
        if self.app.live_task() != LIVE_LINK or not self.app.live_ready:
            return
        if self.engine and not self.writes:
            if self.table() != self.app.engine_sent:     # The console applies them: written when they change
                self.writes += 1
                codeengine.write(self.app)
        elif self.engine is False and not self.writes and not self.dumping:
            for c in self.cheats:
                if c["on"]:
                    self.write(c)
            self.known_tick()
        if self.live_results() and not self.reads and not self.dumping:
            self.ticks += 1
            if self.ticks * FREEZE_MS >= LIVE_MS:
                self.ticks = 0
                size = TYPES[self.search_type][1]
                for i in self.cand[:LIVE_RESULTS]:
                    adrs = RAM + int(i) * size
                    self.reads[adrs] = size
                    self.app.live_send("read %08X %d" % (adrs, size))
        on = sum(1 for c in self.cheats if c["on"]) + sum(1 for k in self.known if k["on"] and self.usable(k))
        self.link_status.configure(text="" if not on else "%d on, applied by the console (code engine)" % on
                                   if self.engine else "%d on, written 10 times a second by this computer (firmware "
                                   "%s: %s has the console apply them)" % (on, version_text(self.app.fw_version),
                                                                            version_text(CHEATS_FIRMWARE)))
        self.unused = 0 if self.using_live() else self.unused + 1
        if self.unused * FREEZE_MS < KEEP_MS:
            self.timer = self.after(FREEZE_MS, self.tick)
        else:
            self.unused = 0
            self.app.live_release()

    def write(self, cheat):
        self.writes += 1
        self.app.live_send("write %08X %s" % (cheat["adrs"], to_bytes(cheat["value"], cheat["type"]).hex().upper()))

    # ---------------------------------------------------------------- search

    def compare_changed(self):
        with_value = self.compare.get() in WITH_VALUE
        self.value.configure(state="normal" if with_value else "disabled")
        self.app.settings.set_field("cheats.compare", self.compare.get())

    def search_value(self):
        if self.compare.get() not in WITH_VALUE:
            return 0
        try:
            v = int(self.value.get().strip(), 0)
        except ValueError:
            self.app.error("Give the value to compare with (decimal, or hexadecimal 0x...).")
            return None
        size = TYPES[self.type.get()][1]
        if not -(1 << (size * 8 - 1)) <= v < (1 << (size * 8)):
            self.app.error("%d doesn't fit in %s." % (v, self.type.get()))
            return None
        return v

    def new_search(self):
        self.start("new")

    def next_search(self):
        if self.cand is None:
            return self.start("new")
        if self.type.get() != self.search_type:
            return self.app.error("The type changed: start a new search (the results are for %s)." % self.search_type)
        return self.start("next")

    def start(self, kind):
        if self.dumping:
            return
        if self.search_value() is None:
            return
        self.app.settings.set_field("cheats.type", self.type.get())
        self.app.settings.set_field("cheats.skip_kernel", self.skip_kernel.get())
        os.makedirs(os.path.dirname(self.dump_path), exist_ok=True)
        self.dumping = kind

        def ask():
            self.status.configure(text="Reading the RAM (about 4 seconds)...")
            self.app.live_send("dump %08X %d %s" % (RAM, RAM_SIZE, self.dump_path))
            self.tick()
        self.link(ask)

    def dump_done(self):
        kind, self.dumping = self.dumping, None
        try:
            with open(self.dump_path, "rb") as f:
                data = f.read()
        except OSError as e:
            return self.status.configure(text="Can't read the RAM file: %s" % e)
        typ = self.type.get()
        dtype, size = TYPES[typ]
        new = np.frombuffer(data[:len(data) // size * size], dtype=dtype).astype(np.int64)
        if kind == "new":
            self.search_type = typ
            first = KERNEL // size if self.skip_kernel.get() else 0
            idx, old = np.arange(first, len(new)), None
        else:
            idx, old = self.cand, self.cur
        how, v = self.compare.get(), self.search_value() or 0
        cur = new[idx]
        if old is None and how in ("Increased by", "Decreased by", "Changed", "Unchanged", "Increased", "Decreased"):
            how = "Any (unknown value)"         # (nothing to compare with yet)
        prev = old[idx] if old is not None else cur
        mask = {"Equal to": lambda: cur == (v & ((1 << size * 8) - 1) if typ[0] == "u" else v),
                "Not equal to": lambda: cur != v,
                "Greater than": lambda: cur > v,
                "Less than": lambda: cur < v,
                "Increased by": lambda: cur - prev == v,
                "Decreased by": lambda: prev - cur == v,
                "Changed": lambda: cur != prev,
                "Unchanged": lambda: cur == prev,
                "Increased": lambda: cur > prev,
                "Decreased": lambda: cur < prev}.get(how, lambda: np.ones(len(cur), bool))()
        self.prev_values = {int(i): int(p) for i, p in zip(idx[mask][:SHOWN], prev[mask][:SHOWN])}
        self.cand, self.cur = idx[mask], new
        self.live_values = {}
        self.next_button.configure(state="normal")
        self.status.configure(text="%s address%s left (%s, %s)." % (
            "{:,}".format(len(self.cand)), "" if len(self.cand) == 1 else "es", typ, how.lower()))
        self.fill_results()
        self.tick()
        return None

    def live_results(self):
        return self.cand is not None and 0 < len(self.cand) <= LIVE_RESULTS

    def fill_results(self):
        self.results.delete(*self.results.get_children())
        if self.cand is None:
            return
        size = TYPES[self.search_type][1]
        for i in self.cand[:SHOWN]:
            i = int(i)
            adrs = RAM + i * size
            v = self.live_values.get(adrs, int(self.cur[i]))
            self.results.insert("", "end", iid=str(adrs), values=(
                "%08X" % adrs, value_text(v, self.search_type), value_text(self.prev_values.get(i, v),
                                                                           self.search_type)))

    def read_done(self, adrs, data):
        size = self.reads.pop(adrs)
        if self.search_type is None or len(data) != size:
            return
        v = int.from_bytes(data, "little", signed=self.search_type[0] == "s")
        changed = self.live_values.get(adrs, v) != v
        self.live_values[adrs] = v
        if self.results.exists(str(adrs)):
            values = list(self.results.item(str(adrs), "values"))
            values[1] = value_text(v, self.search_type)
            self.results.item(str(adrs), values=values, tags=("changed",) if changed else ())

    # ---------------------------------------------------------------- cheats

    def add_results(self):
        if self.search_type is None:
            return
        for iid in self.results.selection() or self.results.get_children()[:1]:
            adrs = int(iid)
            size = TYPES[self.search_type][1]
            v = self.live_values.get(adrs, int(self.cur[(adrs - RAM) // size]))
            self.cheats.append({"name": "Cheat %d" % (len(self.cheats) + 1), "adrs": adrs, "type": self.search_type,
                                "value": v, "on": False})
        self.fill_cheats()
        self.save_game()

    def selected(self):
        return [self.cheats[int(i)] for i in self.list.selection()]

    def fill_cheats(self):
        sel = self.list.selection()
        self.list.delete(*self.list.get_children())
        for i, c in enumerate(self.cheats):
            self.list.insert("", "end", iid=str(i), values=("✓" if c["on"] else "", c["name"], "%08X" % c["adrs"],
                                                            c["type"], value_text(c["value"], c["type"])))
        for i in sel:
            if self.list.exists(i):
                self.list.selection_add(i)

    def cheat_double(self, event):
        row, col = self.list.identify_row(event.y), self.list.identify_column(event.x)
        if not row:
            return
        self.list.selection_set(row)
        if col == "#1":
            self.toggle()
        elif col == "#2":
            self.rename()
        else:
            self.edit_value()

    def toggle(self):
        cheats = self.selected()
        if not cheats:
            return None
        on = not cheats[0]["on"]
        for c in cheats:
            c["on"] = on
        self.fill_cheats()
        self.save_game()
        if on:
            return self.link(self.tick)
        self.tick()
        return None

    def write_once(self):
        cheats = self.selected()
        if cheats:
            self.link(lambda: [self.write(c) for c in cheats])

    def edit_value(self):
        cheats = self.selected()
        if not cheats:
            return
        c = cheats[0]
        text = simpledialog.askstring("Value", "Value of %s (%s, decimal or 0x...):" % (c["name"], c["type"]),
                                      initialvalue=str(c["value"]), parent=self)
        if text is None:
            return
        try:
            v = int(text.strip(), 0)
        except ValueError:
            return self.app.error("Not a number: %s" % text)
        for c in cheats:
            c["value"] = v
        self.fill_cheats()
        self.save_game()
        if any(c["on"] for c in cheats):
            self.tick()

    def rename(self):
        cheats = self.selected()
        if not cheats:
            return
        text = simpledialog.askstring("Name", "Name of the cheat:", initialvalue=cheats[0]["name"], parent=self)
        if text:
            cheats[0]["name"] = text.strip()
            self.fill_cheats()
            self.save_game()

    def remove(self):
        rows = {int(i) for i in self.list.selection()}
        self.cheats = [c for i, c in enumerate(self.cheats) if i not in rows]
        self.fill_cheats()
        self.save_game()

    def add_codes(self):
        """GameShark / Action Replay codes pasted: a line of text before codes names them."""
        text = simpledialog.askstring("Add codes", "GameShark / Action Replay codes, one a line (80XXXXXX YYYY, "
                                      "30XXXXXX 00YY); a line of text before them names them:", parent=self)
        if not text:
            return
        name, added, skipped = None, 0, []
        for line in re.split(r"[\r\n;]+", text):
            m = CODE.match(line)
            if not m:
                if line.strip():
                    name = line.strip()
                continue
            code, value = int(m.group(1), 16), int(m.group(2), 16)
            kind, adrs = code >> 24, RAM | (code & 0x1FFFFF)
            if kind == 0x80:
                typ = "u16"
            elif kind == 0x30:
                typ, value = "u8", value & 0xFF
            else:
                skipped.append(m.group(1))
                continue
            added += 1
            self.cheats.append({"name": name or "Code %d" % (len(self.cheats) + 1), "adrs": adrs, "type": typ,
                                "value": value, "on": True})
        self.fill_cheats()
        self.save_game()
        if skipped:
            self.app.error("%d code%s added. Not supported (only constant writes 80 and 30): %s" % (
                added, "" if added == 1 else "s", ", ".join(skipped)))
        if added:
            self.link(self.tick)

    def copy_codes(self):
        cheats = self.selected() or self.cheats
        lines = []
        for c in cheats:
            lines.append(c["name"])
            lines.extend(codes_of(c))
        self.clipboard_clear()
        self.clipboard_append("\n".join(lines))
        self.game_status.configure(text="%d cheat%s copied as codes." % (len(cheats), "" if len(cheats) == 1 else "s"))

    # ---------------------------------------------------------------- known cheats (database)

    def db(self):
        return cheatdb.connect()

    def show_counts(self):
        try:
            games, cheats, with_cheats = cheatdb.counts(self.db())
        except Exception as e:      # (a database that can't be opened)
            return self.known_status.configure(text="The cheat database can't be opened: %s" % e)
        if not cheats:
            return self.known_status.configure(text="No cheats yet: import your cheat files (Import cheat files...).")
        return self.known_status.configure(text="%s cheats for %s games in the database (%s titles)." % (
            "{:,}".format(cheats), "{:,}".format(with_cheats), "{:,}".format(games)))

    def on_show(self):
        game = getattr(self.app, "game", None)
        if game and game != self.known_serial:
            self.show_game(game)
        elif not self.known_serial and self.app.state.cget("text").strip() == "Running":
            self.detect()                           # (a game runs: its cheats at once)

    def detect(self):
        """The game booted from its disc: its executable, which the firmware keeps (DiscId)."""
        def ask():
            self.detecting = True
            self.reads[DISC_ID] = 32
            self.app.live_send("read %08X 32" % DISC_ID)
        self.game_label.configure(text="Detecting...")
        self.link(ask)

    def detected(self, data):
        self.detecting = False
        self.app.engine_sent = None                 # (the console may have been reset: its table written again)
        features = self.app.fw_features
        if self.engine is False or (features is not None and not features & FEATURE_DISC_ID):
            self.game_label.configure(text="The game can't be detected with this firmware")
            return self.known_status.configure(text="%s's firmware (%s) doesn't keep the disc's name: update it to "
                                                    "%s (Firmware page), or find the game by its title." % (
                self.app.serial(), version_text(self.app.fw_version), version_text(CHEATS_FIRMWARE)))
        if int.from_bytes(data[:4], "little") != DISC_ID_MAGIC:
            self.game_label.configure(text="No game booted from its disc")
            return self.known_status.configure(text="Boot the game with Start + Select on the PS1 DEV CART menu (a "
                                                    "firmware that keeps the disc's name), or find it by its title.")
        serial = data[4:].split(b"\0")[0].decode("latin-1")
        self.app.game = serial
        return self.show_game(serial)

    def find_games(self):
        text = self.find.get().strip()
        if not text:
            return
        self.found = cheatdb.search_games(self.db(), text)
        self.find.configure(values=["%s (%s) %s: %d cheat%s" % (t, r or "?", s, n, "" if n == 1 else "s")
                                    for s, t, r, n in self.found])
        if self.found:
            self.find.event_generate("<Down>")      # (the list opens)
        else:
            self.known_status.configure(text="No game found for %s." % text)

    def found_selected(self):
        i = self.find.current()
        if 0 <= i < len(self.found):
            self.show_game(self.found[i][0])

    def show_game(self, serial):
        db = self.db()
        title, region = cheatdb.game(db, serial)
        on = set(self.app.settings.field("cheats.known_on.%s" % serial, []))
        self.known_serial = serial
        self.known = []
        for cid, name, codes, by_title in cheatdb.cheats_of(db, serial):
            self.known.append({"id": cid, "name": name, "codes": codes, "ops": gs_ops(codes), "on": cid in on,
                               "engine": engine_ok(codes),
                               "by_title": by_title})
        self.game_label.configure(text="%s  %s%s" % (serial, title or "(not in the game lists)",
                                                     " (%s)" % region if region else ""))
        usable = sum(1 for k in self.known if self.usable(k))
        by_title = any(k["by_title"] for k in self.known)
        self.known_status.configure(text=("%d cheat%s for this game.%s" % (
            usable, "" if usable == 1 else "s", " ≈: from a cartridge's list, found by the title: it may be for "
            "another version of the game (language, region)." if by_title else "")) if usable
            else "No cheats for this game in the database.")
        self.fill_known()
        if any(k["on"] for k in self.known):
            self.link(self.tick)

    def fill_known(self):
        self.known_list.delete(*self.known_list.get_children())
        for i, k in enumerate(self.known):
            lines = k["codes"].split("\n")
            ok = self.usable(k)
            self.known_list.insert("", "end", iid=str(i), tags=() if ok else ("off",), values=(
                "✓" if k["on"] else "", (k["name"] if ok else k["name"] + " (codes not supported)") +
                (" ≈" if k["by_title"] else ""),
                lines[0] + (" +%d" % (len(lines) - 1) if len(lines) > 1 else "")))

    def toggle_known(self):
        rows = [self.known[int(i)] for i in self.known_list.selection() if self.usable(self.known[int(i)])]
        if not rows:
            return None
        on = not rows[0]["on"]
        for k in rows:
            k["on"] = on
        self.app.settings.set_field("cheats.known_on.%s" % self.known_serial, [k["id"] for k in self.known if k["on"]])
        sel = self.known_list.selection()
        self.fill_known()
        self.known_list.selection_set(sel)
        if on:
            return self.link(self.tick)
        self.tick()
        return None

    def copy_known(self):
        rows = [self.known[int(i)] for i in self.known_list.selection()] or self.known
        self.clipboard_clear()
        self.clipboard_append("\n".join("%s\n%s" % (k["name"], k["codes"]) for k in rows))
        self.known_status.configure(text="%d cheat%s copied as codes." % (len(rows), "" if len(rows) == 1 else "s"))

    def import_files(self):
        folder = filedialog.askdirectory(title="A folder of game lists (.txt) and cheat files (.cht)", parent=self)
        if not folder:
            return
        self.known_status.configure(text="Importing...")
        self.update_idletasks()
        games, cheats, skipped = cheatdb.import_folder(self.db(), folder, log=lambda text: None)
        self.known_status.configure(text="Imported: %d game titles, %d cheats%s." % (
            games, cheats, " (%d left out: codes that can't be used)" % skipped if skipped else ""))
        if self.known_serial:
            self.show_game(self.known_serial)

    def known_tick(self):
        """The known cheats that are on: their tests' values read first (live link), then the writes they allow."""
        on = [k for k in self.known if k["on"] and k["ops"]]       # (the page's own: writes and tests)
        if not on or self.tests:
            return
        need = {op[3]: op[1] for k in on for op in k["ops"] if op[0] == "if"}
        if need:
            self.tests, self.test_values = dict(need), {}
            for adrs, size in need.items():
                self.app.live_send("read %08X %d" % (adrs, size))
        else:
            self.known_write({})

    def known_write(self, values):
        for k in self.known:
            if not (k["on"] and k["ops"]):
                continue
            allowed = True
            for op in k["ops"]:
                if op[0] == "if":
                    v = values.get(op[3])
                    allowed = v is not None and TESTS[op[2]](v, op[4])
                    continue
                if allowed:
                    self.writes += 1
                    self.app.live_send("write %08X %s" % (op[2], op[3].to_bytes(op[1], "little").hex().upper()))
                allowed = True

    # ---------------------------------------------------------------- the cartridge's cheats (console cheat screen)

    def cart_key(self):
        return "cheats.cartridge.%s" % (self.app.serial() or "")

    def cart_games(self):
        """The games planned for the cartridge (this cartridge's: per device ID)."""
        return list(self.app.settings.field(self.cart_key(), []))

    def cart_set(self, games):
        self.app.settings.set_field(self.cart_key(), games)
        self.cart_refresh()

    def cart_refresh(self):
        """The list: what's on the cartridge (read) and what's planned, the space used."""
        try:
            db = self.db()
            planned = self.cart_games()
            blob, put, left = cheatdb.rom_cheats(db, planned)
        except Exception as e:                      # (no database yet)
            self.cart_space.configure(text="The cheat database can't be opened: %s" % e)
            return
        flashed = self.cart_flashed
        view = self.cart_list_view
        view.delete(*view.get_children())
        for serial in planned:
            title = cheatdb.game(db, serial)[0] or ""
            one = cheatdb.rom_cheats(db, [serial])[0]
            n = int.from_bytes(one[56:58], "little") if len(one) > 58 else 0
            if serial in left:
                state, tag = "Doesn't fit", "full"
            elif flashed is None:
                state, tag = "", ""
            elif serial in flashed:
                state, tag = "On the cartridge", ""
            else:
                state, tag = "To add", "add"
            view.insert("", "end", iid=serial, tags=(tag,) if tag else (), values=(
                state, title, serial, n, "%.1f KB" % ((len(one) - 12) / 1024)))
        for serial in flashed or []:
            if serial not in planned:
                view.insert("", "end", iid=serial, tags=("remove",), values=(
                    "To remove", cheatdb.game(db, serial)[0] or "", serial, "", ""))
        used = len(blob) if put else 0
        self.cart_bar.configure(value=used)
        self.cart_space.configure(text="%.1f KB of %d KB used, %.1f KB free (%d game%s)%s" % (
            used / 1024, cheatdb.ROM_CHEATS_SIZE // 1024, (cheatdb.ROM_CHEATS_SIZE - used) / 1024, len(put),
            "" if len(put) == 1 else "s", ": %d don't fit" % len(left) if left else ""))
        changed = flashed is None or put != flashed
        self.cart_write_button.configure(state="normal" if changed else "disabled")
        self.cart_note.configure(text="" if flashed is None else "Changes not written yet." if changed else
                                 "The cartridge has these cheats.")

    def cart_read(self):
        """The cheats area of the cartridge's flash (ps1transfer -d), at the menu or while a game runs (a task
        running is stopped first, after a question: the Manager's run)."""
        path = os.path.join(config_dir(), "cheats", "cartridge_read.bin")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.cart_read_status.configure(text="Reading...")
        self.app.run(["-d", "-f", path, "-a", "%08X" % (0x1F000000 + cheatdb.ROM_CHEATS), "-s",
                      str(cheatdb.ROM_CHEATS_SIZE)], "Read the cartridge's cheats",
                     on_done=lambda code, out: self.cart_read_done(code, path))
        return None

    def cart_read_done(self, code, path):
        try:
            with open(path, "rb") as f:
                games = cheatdb.read_rom_cheats(f.read()) if code == 0 else None
        except OSError:
            games = None
        if games is None:
            return self.cart_read_status.configure(text="Can't read the cartridge.")
        self.cart_flashed = [g[0] for g in games]
        if not self.app.settings.field(self.cart_key(), None):     # (nothing planned yet: what it has)
            self.app.settings.set_field(self.cart_key(), list(self.cart_flashed))
        old = self.app.fw_version is not None and self.app.fw_version < CHEATS_FIRMWARE
        self.cart_read_status.configure(text=(
            "%s has firmware %s: no cheat screen yet. Write to the cartridge updates it to %s with the cheats." % (
                self.app.serial(), version_text(self.app.fw_version), version_text(CHEATS_FIRMWARE)) if old else
            "%s has the cheats of %d game%s." % (self.app.serial(), len(games), "" if len(games) == 1 else "s")
            if games else "%s has no cheats yet." % self.app.serial()))
        if old:
            self.cart_flashed = []              # (whatever the old firmware has there isn't cheats)
        self.cart_refresh()
        return None

    def cart_add(self, serial):
        if not serial or ":" in serial:
            return self.app.error("This game has no serial number in the game lists: the console finds the cheats by "
                                  "the disc's serial number.")
        games = self.cart_games()
        if serial not in games:
            games.append(serial)
            self.cart_set(games)
        return None

    def cart_put_shown(self):
        """Known cheats: the game shown, put on the cartridge's list (its tab shown)."""
        if not self.known_serial:
            return self.app.error("Detect the game, or find one by its title, first.")
        self.cart_add(self.known_serial)
        self.tabs.select(self.cart_tab)
        return None

    def cart_find_games(self):
        text = self.cart_find.get().strip()
        if not text:
            return
        self.cart_found = [g for g in cheatdb.search_games(self.db(), text) if g[3] and ":" not in g[0]]
        self.cart_find.configure(values=["%s (%s) %s: %d cheat%s" % (t, r or "?", s, n, "" if n == 1 else "s")
                                         for s, t, r, n in self.cart_found])
        if self.cart_found:
            self.cart_find.event_generate("<Down>")
        else:
            self.cart_note.configure(text="No game with cheats found for %s." % text)

    def cart_found_selected(self):
        i = self.cart_find.current()
        if 0 <= i < len(self.cart_found):
            self.cart_add(self.cart_found[i][0])
            self.cart_find.set("")

    def cart_remove(self):
        """The games selected off the list (a game shown To remove: put back)."""
        games = self.cart_games()
        for serial in self.cart_list_view.selection():
            if serial in games:
                games.remove(serial)
            else:
                games.append(serial)
        self.cart_set(games)

    def cart_write(self):
        """The firmware ROM (the Firmware page's) with the planned cheats, flashed (read back and checked)."""
        from pages.firmware import default_rom, describe_rom
        blob, put, left = cheatdb.rom_cheats(self.db(), self.cart_games())
        fw = self.app.pages.get("Firmware")
        rom_path = (fw.rom.get() if fw else "") or default_rom()
        valid, text = describe_rom(rom_path) if rom_path else (False, "No firmware ROM (Firmware page).")
        if not valid:
            return self.app.error(text)
        new = rom_version(rom_path)
        if not new or new < CHEATS_FIRMWARE:
            return self.app.error("The firmware ROM %s (firmware %s, Firmware page) has no cheat screen: choose a %s "
                                  "ROM or later." % (os.path.basename(rom_path), version_text(new),
                                                     version_text(CHEATS_FIRMWARE)))
        cur = self.app.fw_version
        if cur is None:
            update = "\n\nThe cartridge's firmware is replaced by firmware %s (its version wasn't read: Refresh)." % (
                version_text(new))
        elif cur != new:
            update = ("\n\nThis also updates %s's firmware from %s to %s%s." % (
                self.app.serial(), version_text(cur), version_text(new), " (the cheat screen and the code engine come "
                "with it)" if cur < CHEATS_FIRMWARE else ""))
        else:
            update = "\n\nThe firmware stays %s." % version_text(cur)
        with open(rom_path, "rb") as f:
            rom = cheatdb.rom_with_cheats(f.read(), blob)
        out = os.path.join(config_dir(), "cheats", "cartridge.rom")
        with open(out, "wb") as f:
            f.write(rom)
        if not self.app.confirm("Write the cheats of %d game%s to the cartridge %s?\n\nIt's flashed with %s (%s) "
                                "and these cheats, then read back and checked. The console is reset.%s%s" % (
                                    len(put), "" if len(put) == 1 else "s", self.app.serial(),
                                    os.path.basename(rom_path), text, update,
                                    "\n\n%d game%s don't fit and are left out." % (len(left), "" if len(left) == 1
                                                                                     else "s") if left else "")):
            return None
        self.cart_note.configure(text="Flashing...")

        def done(code, output):
            if code == 0:
                self.cart_flashed = list(put)
                self.app.after(6000, self.app.refresh_status)   # (the new firmware's version)
                self.cart_read_status.configure(text="%s has the cheats of %d game%s." % (
                    self.app.serial(), len(put), "" if len(put) == 1 else "s"))
                self.cart_refresh()
            else:
                self.cart_note.configure(text="Flashing failed: %s" % ([l for l in output if l.strip()][-1:] or
                                                                       ["?"])[0])
        self.app.run(["-flash", out], "Flash", on_done=done)
        return None

    def on_device_changed(self):
        self.cart_flashed = None
        self.cart_read_status.configure(text="Not read yet.")
        self.cart_refresh()

    # ---------------------------------------------------------------- the lists, one per game

    def games_dir(self):
        return os.path.join(config_dir(), "cheats")

    def load_games(self):
        try:
            names = sorted(f[:-5] for f in os.listdir(self.games_dir()) if f.endswith(".json"))
        except OSError:
            names = []
        self.game.configure(values=names)
        last = self.app.settings.field("cheats.game", "")
        self.game.set(last or "My game")
        if last:
            self.load_game()

    def game_file(self):
        name = re.sub(r'[\\/:*?"<>|]', "_", self.game.get().strip()) or "My game"
        return os.path.join(self.games_dir(), name + ".json")

    def load_game(self):
        try:
            with open(self.game_file(), encoding="utf-8") as f:
                self.cheats = [dict(c, on=c.get("on", False)) for c in json.load(f)]
            self.game_status.configure(text="%d cheat%s." % (len(self.cheats), "" if len(self.cheats) == 1 else "s"))
        except FileNotFoundError:
            self.cheats = []
            self.game_status.configure(text="New list: the cheats are saved under this name.")
        except (OSError, ValueError) as e:
            self.cheats = []
            self.game_status.configure(text="Can't read the list: %s" % e)
        self.app.settings.set_field("cheats.game", self.game.get().strip())
        self.fill_cheats()
        if any(c["on"] for c in self.cheats) and self.app.live_task() == LIVE_LINK:
            self.tick()

    def save_game(self):
        os.makedirs(self.games_dir(), exist_ok=True)
        with open(self.game_file(), "w", encoding="utf-8") as f:
            json.dump(self.cheats, f, indent=1)
        self.app.settings.set_field("cheats.game", self.game.get().strip())
        self.load_games_list()

    def load_games_list(self):
        try:
            self.game.configure(values=sorted(f[:-5] for f in os.listdir(self.games_dir()) if f.endswith(".json")))
        except OSError:
            pass
