#!/usr/bin/env python3
"""
PicoStation Manager: a small app for the games of a PicoStation SD card (firmware RC5): their covers (.cov) and
cheats (.cht), shown and changed one game at a time, or for the whole card at once.

  - The card's games: every .cue, a folder of several .cue files being one game of several discs (its discs chosen
    in the panel). Each game's ID read from its disc image (picostation_ids.py), its title from the game database.
  - Covers: any image (PNG, JPG, BMP, GIF, WEBP) or the database's cover, converted to the PicoStation's .cov
    (picostation_covers.py: 256 colors, 128x128), shown before it is saved. Saved for each disc of the game, with
    the disc's ID after the image (the PS1 DEV CART's PicoStation screen finds its cheats by it), and beside the
    game's folder ("folder.cov", read by the cartridge at once). A cover can be saved as a PNG.
  - Cheats: the .cht of each disc (GameShark codes, "[name]" on when the game boots, "[*name]" off), edited, added
    from the PS1 DEV CART Manager's cheat database (gui/data/cheats.sqlite), checked against the PicoStation's
    limits (6 KB, 256 cheats) before they are saved.
  - Whole card: missing covers from the database, the game IDs written in the covers; what would change is listed
    first, nothing written before it is confirmed.

The game database: psio_assist.db of the PSIO Game Manager (github.com/logi-26/psio-game-manager, src/data: its 4
parts joined): game IDs, titles, 80x84 covers. Chosen in the app, or downloaded by it.

    python picostation_manager.py            (needs Pillow and ttkbootstrap: python -m pip install pillow ttkbootstrap)

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""

import io
import json
import os
import pathlib
import re
import sqlite3
import sys
import threading
import urllib.request
from tkinter import filedialog, messagebox

try:
    from PIL import Image, ImageTk
    import ttkbootstrap as tb
except ImportError:
    sys.exit("Pillow and ttkbootstrap are needed: python -m pip install pillow ttkbootstrap")

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import picostation_covers as covers     # noqa: E402
import picostation_ids as ids           # noqa: E402

APP_TITLE = "PicoStation Manager"
SETTINGS = os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~"), "PicoStationManager", "settings.json")
DB_PARTS = ["https://github.com/logi-26/psio-game-manager/raw/main/src/data/psio_assist_db_part_%d" % i
            for i in range(1, 5)]
CHEAT_DB = os.path.join(HERE, "..", "gui", "data", "cheats.sqlite")
CHT_SIZE = 3 * 2048         # What the PicoStation sends of a .cht
CHT_CHEATS = 256
CODE = re.compile(r"^([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{4})$")
PREVIEW = 256               # The cover shown 2x
SKIP_DIRS = ("System Volume Information",)


# ------------------------------------------------------------------------------------------------- settings, files

def load_settings():
    try:
        with open(SETTINGS) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_settings(s):
    try:
        os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
        with open(SETTINGS, "w") as f:
            json.dump(s, f, indent=1)
    except OSError:
        pass


def cov_image(data):
    """A .cov's 128x128 image (PIL), None if it isn't one."""
    if len(data) < ids.COVER_IMAGE:
        return None
    pal = []
    for i in range(256):
        c = int.from_bytes(data[i * 2:i * 2 + 2], "little")
        pal += [(c & 31) * 255 // 31, ((c >> 5) & 31) * 255 // 31, ((c >> 10) & 31) * 255 // 31]
    img = Image.frombytes("P", (128, 128), bytes(data[512:ids.COVER_IMAGE]))
    img.putpalette(pal)
    return img.convert("RGB")


def plain_id(serial):
    """An ID as the game database has it: "SLES_013.43" -> "SLES_01343"."""
    return (serial or "").upper().replace(".", "")


# ------------------------------------------------------------------------------------------------- .cht files

class Cheat:
    def __init__(self, name, codes, on=False):
        self.name, self.codes, self.on = name, codes, on     # codes: [(address, value)]

    def lines(self):
        return ["[%s%s]" % ("" if self.on else "*", self.name)] + ["%08X %04X" % c for c in self.codes]


def parse_cht(text):
    """The cheats of a .cht's text (lines it doesn't know left out)."""
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line[0] in "#;/\"":
            continue
        m = re.match(r"^\[(\*?)(.*)\]$", line)
        if m:
            out.append(Cheat(m.group(2).strip() or "Cheat", [], not m.group(1)))
            continue
        m = CODE.match(line)
        if m and out:
            out[-1].codes.append((int(m.group(1), 16), int(m.group(2), 16)))
    return [c for c in out if c.codes]


def cht_text(cheats, title, serial):
    lines = ["# %s (%s): PicoStation Manager" % (title, serial)]
    for c in cheats:
        lines += c.lines()
    return "\r\n".join(lines) + "\r\n"


def cht_limits(cheats, title, serial):
    """(size in bytes, a problem or "")."""
    size = len(cht_text(cheats, title, serial).encode("ascii", "replace"))
    if size > CHT_SIZE:
        return size, "too big for the PicoStation (%d bytes, at most %d)" % (size, CHT_SIZE)
    if len(cheats) > CHT_CHEATS:
        return size, "too many cheats (%d, at most %d)" % (len(cheats), CHT_CHEATS)
    return size, ""


# ------------------------------------------------------------------------------------------------- the card

class Disc:
    def __init__(self, cue):
        self.cue = cue
        self.base = os.path.splitext(cue)[0]
        self.cov = self.base + ".cov"
        self.cht = self.base + ".cht"
        self.serial = None
        self.error = ""


class Game:
    def __init__(self, sd, folder, cues):
        self.sd, self.folder = sd, folder
        self.discs = [Disc(os.path.join(folder, c)) for c in cues]
        in_folder = os.path.abspath(folder) != os.path.abspath(sd)
        self.name = os.path.basename(folder) if (in_folder and len(cues) > 1) else os.path.splitext(cues[0])[0]
        if in_folder and len(cues) == 1:
            self.name = os.path.basename(folder)
        # The cover beside the folder (read by the cartridge at once): a folder of one game, or a game's discs
        self.folder_cov = folder.rstrip("\\/") + ".cov" if in_folder else None
        self.title = ""
        self.cover = ""
        self.cheats = 0
        self.id_state = ""      # Its covers have the game's ID after the image: "Yes", "No", "" (no cover / no ID)

    @property
    def serial(self):
        return self.discs[0].serial


def scan(sd):
    """The card's games: a folder of several .cue files is one game (its discs), the others one game each."""
    games = []
    for folder, dirs, files in os.walk(sd):
        dirs[:] = sorted(d for d in dirs if not d.startswith(".") and d not in SKIP_DIRS)
        cues = sorted(f for f in files if f.lower().endswith(".cue"))
        if not cues:
            continue
        if os.path.abspath(folder) == os.path.abspath(sd):
            games += [Game(sd, folder, [c]) for c in cues]
        else:
            games.append(Game(sd, folder, cues))
    return sorted(games, key=lambda g: g.name.lower())


# ------------------------------------------------------------------------------------------------- the app

class App(tb.Window):
    def __init__(self):
        super().__init__(title=APP_TITLE, themename="bootstrap-dark", minsize=(1060, 680))
        self.settings = load_settings()
        self.games, self.shown = [], []
        self.game = None            # The game in the panel, its disc
        self.disc = None
        self.pending = None         # A cover chosen, not saved yet: (cov bytes, where it came from)
        self.cheats = []            # The disc's cheats as edited
        self.cheats_changed = False
        self.db = None              # The game database (sqlite3), the cheat database
        self.cheat_db = None
        self.template = covers.to_cov(covers.no_cover())   # (the placeholder's image: "No cover" in the list)
        self.photo = None
        self._build()
        self.open_db(self.settings.get("db") or self._find_db(), quiet=True)
        self._open_cheat_db()
        if self.settings.get("sd") and os.path.isdir(self.settings["sd"]):
            self.sd_var.set(self.settings["sd"])
            self.rescan()

    # --------------------------------------------------------------------------------------------- layout
    def _build(self):
        top = tb.Frame(self, padding=(12, 10, 12, 4))
        top.pack(fill="x")
        top.columnconfigure(1, weight=1)
        tb.Label(top, text="SD card", width=14).grid(row=0, column=0, sticky="w")
        self.sd_var = tb.StringVar()
        tb.Entry(top, textvariable=self.sd_var).grid(row=0, column=1, sticky="ew", padx=(0, 8))
        tb.Button(top, text="Browse...", bootstyle="secondary", command=self.choose_sd).grid(row=0, column=2)
        tb.Button(top, text="Rescan", bootstyle="primary", width=9, command=self.rescan).grid(row=0, column=3,
                                                                                             padx=(8, 0))
        tb.Label(top, text="Game database", width=14).grid(row=1, column=0, sticky="w", pady=(6, 0))
        self.db_label = tb.Label(top, text="", bootstyle="secondary")
        self.db_label.grid(row=1, column=1, sticky="w", pady=(6, 0))
        bar = tb.Frame(top)
        bar.grid(row=1, column=2, columnspan=2, sticky="e", pady=(6, 0))
        tb.Button(bar, text="Choose...", bootstyle="secondary", command=self.choose_db).pack(side="left")
        tb.Button(bar, text="Download", bootstyle="secondary-outline", command=self.download_db).pack(
            side="left", padx=(8, 0))

        body = tb.Panedwindow(self, orient="horizontal")
        body.pack(fill="both", expand=True, padx=12, pady=8)

        # The games
        left = tb.Frame(body)
        body.add(left, weight=3)
        row = tb.Frame(left)
        row.pack(fill="x", pady=(0, 6))
        tb.Label(row, text="Search").pack(side="left")
        self.filter_var = tb.StringVar()
        self.filter_var.trace_add("write", lambda *_: self.show_list())
        tb.Entry(row, textvariable=self.filter_var).pack(side="left", fill="x", expand=True, padx=8)
        self.count_label = tb.Label(row, text="", bootstyle="secondary")
        self.count_label.pack(side="left")
        cols = ("id", "region", "discs", "cover", "idcov", "cheats")
        self.tree = tb.Treeview(left, columns=cols, show="tree headings", selectmode="browse")
        self.tree.heading("#0", text="Game", anchor="w")
        self.tree.column("#0", width=300)
        for c, text, w in zip(cols, ("ID", "Region", "Discs", "Cover", "ID in cover", "Cheats"),
                              (100, 60, 50, 90, 80, 60)):
            self.tree.heading(c, text=text, anchor="w")
            self.tree.column(c, width=w, stretch=c == "cover")
        sb = tb.Scrollbar(left, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        self.tree.pack(fill="both", expand=True)
        self.tree.bind("<<TreeviewSelect>>", lambda e: self.select())

        # The game's panel
        right = tb.Frame(body, padding=(12, 0, 0, 0))
        body.add(right, weight=2)
        self.title_label = tb.Label(right, text="Choose a game", font=("", 13, "bold"), wraplength=440)
        self.title_label.pack(anchor="w")
        self.info_label = tb.Label(right, text="", bootstyle="secondary", wraplength=440, justify="left")
        self.info_label.pack(anchor="w", pady=(2, 6))
        row = tb.Frame(right)
        row.pack(fill="x")
        tb.Label(row, text="Disc").pack(side="left")
        self.disc_var = tb.StringVar()
        self.disc_box = tb.Combobox(row, textvariable=self.disc_var, state="readonly", width=40)
        self.disc_box.pack(side="left", padx=8)
        self.disc_box.bind("<<ComboboxSelected>>", lambda e: self.select_disc(self.disc_box.current()))

        cov = tb.Labelframe(right, text="Cover", padding=10)
        cov.pack(fill="x", pady=(8, 0))
        self.canvas = tb.Canvas(cov, width=PREVIEW, height=PREVIEW, highlightthickness=0, background="#000000")
        self.canvas.grid(row=0, column=0, rowspan=8, sticky="n")
        self.cover_note = tb.Label(cov, text="", bootstyle="secondary", wraplength=170, justify="left")
        self.cover_note.grid(row=0, column=1, sticky="nw", padx=(12, 0))
        self.cover_buttons = []
        for r, (text, style, cmd) in enumerate((("Image file...", "primary", self.cover_from_file),
                                                ("From database", "primary-outline", self.cover_from_db),
                                                ("Save cover", "success", self.save_cover),
                                                ("Cancel", "secondary", self.cancel_cover),
                                                ("Save as PNG...", "secondary-outline", self.export_png)), 1):
            b = tb.Button(cov, text=text, bootstyle=style, width=16, command=cmd)
            b.grid(row=r, column=1, sticky="w", padx=(12, 0), pady=(4, 0))
            self.cover_buttons.append(b)
        self.stretch_var = tb.BooleanVar(value=False)
        tb.Checkbutton(cov, text="Fill the square", variable=self.stretch_var, bootstyle="round-toggle",
                       command=self.restretch).grid(row=6, column=1, sticky="w", padx=(12, 0), pady=(8, 0))

        ch = tb.Labelframe(right, text="Cheats", padding=10)
        ch.pack(fill="both", expand=True, pady=(8, 0))
        self.cheat_tree = tb.Treeview(ch, columns=("on", "codes"), show="tree headings", height=7,
                                      selectmode="browse")
        self.cheat_tree.heading("#0", text="Cheat", anchor="w")
        self.cheat_tree.heading("on", text="At boot", anchor="w")
        self.cheat_tree.heading("codes", text="Codes", anchor="w")
        self.cheat_tree.column("#0", width=230)
        self.cheat_tree.column("on", width=60, stretch=False)
        self.cheat_tree.column("codes", width=50, stretch=False)
        self.cheat_tree.pack(fill="both", expand=True)
        self.cheat_tree.bind("<Double-1>", lambda e: self.edit_cheat())
        self.cheat_note = tb.Label(ch, text="", bootstyle="secondary")
        self.cheat_note.pack(anchor="w", pady=(4, 0))
        bar = tb.Frame(ch)
        bar.pack(fill="x", pady=(6, 0))
        self.cheat_buttons = []
        for text, style, cmd in (("Add", "primary", self.add_cheat), ("Edit", "secondary", self.edit_cheat),
                                 ("On/Off", "secondary", self.toggle_cheat), ("Delete", "danger-outline",
                                                                               self.delete_cheat),
                                 ("From cheat database", "primary-outline", self.import_cheats),
                                 ("Save", "success", self.save_cheats)):
            b = tb.Button(bar, text=text, bootstyle=style, command=cmd)
            b.pack(side="left", padx=(0, 6))
            self.cheat_buttons.append(b)

        bottom = tb.Frame(self, padding=(12, 0, 12, 10))
        bottom.pack(fill="x")
        tb.Label(bottom, text="Whole card:", bootstyle="secondary").pack(side="left")
        tb.Button(bottom, text="Missing covers from the database...", bootstyle="secondary-outline",
                  command=self.fill_covers).pack(side="left", padx=(8, 0))
        tb.Button(bottom, text="Add game IDs to existing covers...", bootstyle="secondary-outline",
                  command=self.write_ids).pack(side="left", padx=(8, 0))
        self.status = tb.Label(bottom, text="", bootstyle="secondary")
        self.status.pack(side="right")
        self.panel_state()

    def panel_state(self):
        game = self.disc is not None
        for b in self.cover_buttons:
            b.configure(state="normal" if game else "disabled")
        self.cover_buttons[1].configure(state="normal" if game and self.db else "disabled")
        self.cover_buttons[2].configure(state="normal" if self.pending else "disabled")
        self.cover_buttons[3].configure(state="normal" if self.pending else "disabled")
        for b in self.cheat_buttons:
            b.configure(state="normal" if game and self.disc.serial else "disabled")
        self.cheat_buttons[4].configure(state="normal" if game and self.disc.serial and self.cheat_db else "disabled")
        self.cheat_buttons[5].configure(state="normal" if game and self.cheats_changed else "disabled")

    # --------------------------------------------------------------------------------------------- databases
    def _find_db(self):
        for p in (os.path.join(HERE, "data", "psio_assist.db"), os.path.join(os.path.dirname(SETTINGS),
                                                                            "psio_assist.db")):
            if os.path.exists(p):
                return p
        return None

    def open_db(self, path, quiet=False):
        if not path or not os.path.exists(path):
            self.db = None
            self.db_label.configure(text="none: Choose your psio_assist.db, or Download it (about 150 MB)")
            return
        try:
            db = sqlite3.connect(pathlib.Path(os.path.abspath(path)).as_uri() + "?mode=ro", uri=True,
                                 check_same_thread=False)
            n = db.execute("select count(*) from games").fetchone()[0]
            c = db.execute("select count(*) from covers").fetchone()[0]
        except sqlite3.Error as e:
            if not quiet:
                messagebox.showerror(APP_TITLE, "Not a PSIO game database:\n%s\n\n%s" % (path, e))
            return
        self.db = db
        self.db_label.configure(text="%s (%d games, %d covers)" % (path, n, c))
        self.settings["db"] = path
        save_settings(self.settings)
        for g in self.games:
            g.title = self.db_title(g.serial) or g.title
        self.show_list()
        self.panel_state()

    def db_title(self, serial):
        if not self.db or not serial:
            return ""
        row = self.db.execute("select name from games where game_id = ?", (plain_id(serial),)).fetchone()
        return row[0] if row else ""

    def db_cover(self, serial):
        if not self.db or not serial:
            return None
        row = self.db.execute("select psio from covers where game_id = ?", (plain_id(serial),)).fetchone()
        return Image.open(io.BytesIO(row[0])).convert("RGB") if row else None

    def _open_cheat_db(self):
        try:
            sys.path.insert(0, os.path.join(HERE, "..", "gui"))
            import cheatdb
            import picostation_cheats
            if os.path.exists(CHEAT_DB):
                self.cheat_db = (cheatdb, picostation_cheats, cheatdb.connect(CHEAT_DB))
        except Exception:       # (the app works without it: no "From cheat database")
            self.cheat_db = None

    def choose_db(self):
        path = filedialog.askopenfilename(title="The PSIO game database", filetypes=[("psio_assist.db", "*.db"),
                                                                                     ("All files", "*.*")])
        if path:
            self.open_db(path)

    def download_db(self):
        target = os.path.join(os.path.dirname(SETTINGS), "psio_assist.db")
        if not messagebox.askyesno(APP_TITLE, "Download the PSIO Game Manager's database (4 parts, about 150 MB) "
                                              "from GitHub, to\n%s?" % target):
            return

        def work():
            try:
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target + ".part", "wb") as out:
                    for i, url in enumerate(DB_PARTS, 1):
                        req = urllib.request.Request(url, headers={"User-Agent": "picostation-manager"})
                        with urllib.request.urlopen(req, timeout=60) as r:
                            got = 0
                            while True:
                                block = r.read(1 << 16)
                                if not block:
                                    break
                                out.write(block)
                                got += len(block)
                                self.after(0, self.status.configure,
                                           {"text": "Downloading part %d of 4: %d MB" % (i, got >> 20)})
                with open(target + ".part", "rb") as f:
                    if f.read(16) != b"SQLite format 3\0":
                        raise ValueError("the downloaded parts aren't a database")
                os.replace(target + ".part", target)
                self.after(0, lambda: (self.status.configure(text="Database downloaded"), self.open_db(target)))
            except (OSError, ValueError) as e:
                msg = str(e)
                self.after(0, lambda: (self.status.configure(text=""),
                                       messagebox.showerror(APP_TITLE, "Download failed: %s" % msg)))
        threading.Thread(target=work, daemon=True).start()

    # --------------------------------------------------------------------------------------------- the card
    def choose_sd(self):
        path = filedialog.askdirectory(title="The PicoStation's SD card")
        if path:
            self.sd_var.set(os.path.normpath(path))
            self.rescan()

    def rescan(self):
        sd = self.sd_var.get().strip()
        if not os.path.isdir(sd):
            messagebox.showerror(APP_TITLE, "Not a folder: %s" % sd)
            return
        if not self.leave_cheats():
            return
        self.settings["sd"] = sd
        save_settings(self.settings)
        self.status.configure(text="Reading the card...")
        self.tree.delete(*self.tree.get_children())

        def work():
            games = scan(sd)
            for n, g in enumerate(games, 1):
                for d in g.discs:
                    try:
                        d.serial = ids.game_id(d.cue)
                    except (OSError, ValueError) as e:
                        d.error = str(e)
                self.after(0, self.status.configure, {"text": "Reading the card: %d of %d games" % (n, len(games))})
            self.after(0, self.scanned, games)
        threading.Thread(target=work, daemon=True).start()

    def scanned(self, games):
        for g in games:
            g.title = self.db_title(g.serial)
            self.refresh_game(g)
        self.games = games
        self.game = self.disc = None
        self.show_list()
        self.show_game()
        self.status.configure(text="%d games, %d discs" % (len(games), sum(len(g.discs) for g in games)))

    def refresh_game(self, g):
        """The list's cover and cheats columns of a game (its first disc's cover, all its discs' cheats)."""
        d = g.discs[0]
        data = open(d.cov, "rb").read() if os.path.exists(d.cov) else b""
        if not data:
            g.cover = "Missing"
        elif data[ids.COVER_IMAGE:ids.COVER_IMAGE + 4] == b"PSIX" or not any(data[:ids.COVER_IMAGE]):
            g.cover = "Blank"
        elif data[:ids.COVER_IMAGE] == self.template:
            g.cover = "No cover"
        else:
            g.cover = "Cover"
        states = [ids.write_cover(p, s, True) == "already done" for p, s in self.cover_targets(g)
                  if s and os.path.exists(p)]
        g.id_state = ("Yes" if all(states) else "No") if states else ""
        g.cheats = 0
        for d in g.discs:
            if os.path.exists(d.cht):
                try:
                    g.cheats += len(parse_cht(open(d.cht, errors="replace").read()))
                except OSError:
                    pass

    def show_list(self):
        if not hasattr(self, "tree"):
            return
        text = self.filter_var.get().lower().strip()
        sel = self.game
        self.tree.delete(*self.tree.get_children())
        self.shown = [g for g in self.games if not text or text in g.name.lower() or text in (g.title or "").lower()
                      or text in (g.serial or "").lower()]
        for i, g in enumerate(self.shown):
            self.tree.insert("", "end", iid=str(i), text=g.name, values=(
                g.serial or "?", covers.id_region(g.serial or "") or "", len(g.discs), g.cover, g.id_state,
                g.cheats or ""))
        self.count_label.configure(text="%d of %d" % (len(self.shown), len(self.games)))
        if sel in self.shown:
            self.tree.selection_set(str(self.shown.index(sel)))

    def update_row(self, g):
        self.refresh_game(g)
        if g in self.shown:
            iid = str(self.shown.index(g))
            self.tree.item(iid, values=(g.serial or "?", covers.id_region(g.serial or "") or "", len(g.discs),
                                        g.cover, g.id_state, g.cheats or ""))

    # --------------------------------------------------------------------------------------------- one game
    def select(self):
        sel = self.tree.selection()
        if not sel:
            return
        g = self.shown[int(sel[0])]
        if g is self.game:
            return
        if not self.leave_cheats():
            self.tree.selection_set(str(self.shown.index(self.game)) if self.game in self.shown else ())
            return
        self.game = g
        self.show_game()

    def show_game(self):
        g = self.game
        self.pending = None
        if not g:
            self.disc = None
            self.title_label.configure(text="Choose a game")
            self.info_label.configure(text="")
            self.disc_box.configure(values=[])
            self.disc_var.set("")
            self.draw_cover(None)
            self.load_cheats()
            return
        self.title_label.configure(text=g.title or g.name)
        self.disc_box.configure(values=["Disc %d: %s" % (i + 1, os.path.basename(d.cue))
                                        for i, d in enumerate(g.discs)])
        self.disc_box.configure(state="readonly" if len(g.discs) > 1 else "disabled")
        self.select_disc(0)

    def select_disc(self, i):
        if self.disc is not None and self.game and i >= 0 and self.disc is self.game.discs[i]:
            return
        if not self.leave_cheats():
            self.disc_box.current(self.game.discs.index(self.disc))
            return
        self.disc = self.game.discs[i]
        self.disc_box.current(i)
        self.pending = None
        d, g = self.disc, self.game
        info = ["ID: %s%s" % (d.serial or "not found", ("  (%s)" % d.error) if d.error else ""),
                "Region: %s" % (covers.id_region(d.serial or "") or "unknown"),
                "Folder: %s" % os.path.relpath(g.folder, g.sd)]
        if not g.title:
            info.append("Not in the game database" if self.db else "No game database")
        self.info_label.configure(text="\n".join(info))
        self.show_cover()
        self.load_cheats()

    # --------------------------------------------------------------------------------------------- covers
    def show_cover(self):
        d = self.disc
        data = open(d.cov, "rb").read() if d and os.path.exists(d.cov) else b""
        img = cov_image(data) if data else None
        if not data:
            note = "No cover file: choose an image, or the database's cover."
        elif data[ids.COVER_IMAGE:ids.COVER_IMAGE + 4] == b"PSIX":
            note = "A blank cover (made for the game's ID)."
        elif data[:ids.COVER_IMAGE] == self.template:
            note = "The \"NO COVER\" placeholder."
        else:
            note = os.path.basename(d.cov)
        self.cover_note.configure(text=note)
        self.draw_cover(img)
        self.panel_state()

    def draw_cover(self, img):
        self.canvas.delete("all")
        if img is None:
            self.photo = None
            self.canvas.create_text(PREVIEW // 2, PREVIEW // 2, text="No cover", fill="#888888")
            return
        self.photo = ImageTk.PhotoImage(img.resize((PREVIEW, PREVIEW), Image.NEAREST))
        self.canvas.create_image(0, 0, image=self.photo, anchor="nw")

    def set_pending(self, src, label):
        try:
            data = covers.to_cov(src, self.stretch_var.get())
        except (OSError, ValueError) as e:
            messagebox.showerror(APP_TITLE, "Not converted: %s" % e)
            return
        self.pending = (data, label, src)
        self.draw_cover(cov_image(data))
        self.cover_note.configure(text="%s\n\nNot saved yet: Save cover writes it for %s." % (
            label, "the game's %d discs" % len(self.game.discs) if len(self.game.discs) > 1 else "the game"))
        self.panel_state()

    def restretch(self):
        if self.pending:
            self.set_pending(self.pending[2], self.pending[1])

    def cover_from_file(self):
        path = filedialog.askopenfilename(title="Cover image", filetypes=[
            ("Images", " ".join("*" + e for e in covers.IMAGES)), ("All files", "*.*")])
        if not path:
            return
        try:
            img = Image.open(path)
            img.load()
        except OSError as e:
            messagebox.showerror(APP_TITLE, "Can't open %s: %s" % (path, e))
            return
        self.set_pending(img, os.path.basename(path))

    def cover_from_db(self):
        img = self.db_cover(self.disc.serial)
        if img is None:
            messagebox.showinfo(APP_TITLE, "The database has no cover for %s." % (self.disc.serial or "this game"))
            return
        self.set_pending(img, "The database's cover (%dx%d, scaled up)" % img.size)

    def cancel_cover(self):
        self.pending = None
        self.show_cover()

    def cover_targets(self, g):
        """Where a game's cover goes: (path, the ID written after it) for each disc, then beside its folder."""
        out = [(d.cov, d.serial) for d in g.discs]
        if g.folder_cov:
            out.append((g.folder_cov, g.discs[0].serial))
        return out

    def write_cover(self, g, data):
        for path, serial in self.cover_targets(g):
            with open(path, "wb") as f:
                f.write(data)
            if serial:
                ids.write_cover(path, serial, False)

    def save_cover(self):
        g = self.game
        if not self.pending or not g:
            return
        existing = [p for p, _s in self.cover_targets(g) if os.path.exists(p)]
        real = [p for p in existing if self.is_real_cover(p)]
        names = "\n".join(os.path.relpath(p, g.sd) for p, _s in self.cover_targets(g))
        if real and not messagebox.askyesno(APP_TITLE, "Replace the cover of %s?\n\n%s" % (g.title or g.name,
                                                                                          names)):
            return
        try:
            self.write_cover(g, self.pending[0])
        except OSError as e:
            messagebox.showerror(APP_TITLE, "Not saved: %s" % e)
            return
        self.pending = None
        self.status.configure(text="Cover saved: %s" % (g.title or g.name))
        self.update_row(g)
        self.show_cover()

    def is_real_cover(self, path):
        data = open(path, "rb").read()
        return len(data) >= ids.COVER_IMAGE and data[ids.COVER_IMAGE:ids.COVER_IMAGE + 4] != b"PSIX" and \
            data[:ids.COVER_IMAGE] != self.template and any(data[:ids.COVER_IMAGE])

    def export_png(self):
        data = self.pending[0] if self.pending else (open(self.disc.cov, "rb").read()
                                                     if os.path.exists(self.disc.cov) else b"")
        img = cov_image(data)
        if img is None:
            messagebox.showinfo(APP_TITLE, "No cover to save.")
            return
        path = filedialog.asksaveasfilename(title="Save the cover as", defaultextension=".png",
                                            initialfile=(self.game.title or self.game.name) + ".png",
                                            filetypes=[("PNG", "*.png")])
        if path:
            img.save(path)

    # --------------------------------------------------------------------------------------------- cheats
    def load_cheats(self):
        self.cheats, self.cheats_changed = [], False
        d = self.disc
        if d and os.path.exists(d.cht):
            try:
                self.cheats = parse_cht(open(d.cht, errors="replace").read())
            except OSError as e:
                messagebox.showerror(APP_TITLE, "Can't read %s: %s" % (d.cht, e))
        self.show_cheats()

    def show_cheats(self, select=None):
        self.cheat_tree.delete(*self.cheat_tree.get_children())
        for i, c in enumerate(self.cheats):
            self.cheat_tree.insert("", "end", iid=str(i), text=c.name, values=("On" if c.on else "", len(c.codes)))
        if select is not None and 0 <= select < len(self.cheats):
            self.cheat_tree.selection_set(str(select))
            self.cheat_tree.see(str(select))
        d = self.disc
        if not d:
            note = ""
        elif not d.serial:
            note = "The game's ID wasn't found: no cheats."
        else:
            size, problem = cht_limits(self.cheats, self.game.title or self.game.name, d.serial)
            note = "%d cheats, %d of %d bytes%s%s" % (len(self.cheats), size, CHT_SIZE,
                                                    ("  -  " + problem) if problem else "",
                                                    "  -  not saved" if self.cheats_changed else "")
        self.cheat_note.configure(text=note, bootstyle="danger" if "most" in note else
                                  ("warning" if self.cheats_changed else "secondary"))
        self.panel_state()

    def changed(self, select=None):
        self.cheats_changed = True
        self.show_cheats(select)

    def selected_cheat(self):
        sel = self.cheat_tree.selection()
        return int(sel[0]) if sel else None

    def cheat_dialog(self, cheat):
        """A cheat edited in a window: (name, codes, on), None if cancelled."""
        win = tb.Toplevel(self, title="Cheat")
        win.transient(self)
        win.grab_set()
        f = tb.Frame(win, padding=14)
        f.pack(fill="both", expand=True)
        tb.Label(f, text="Name").grid(row=0, column=0, sticky="w")
        name = tb.Entry(f, width=44)
        name.insert(0, cheat.name if cheat else "")
        name.grid(row=0, column=1, sticky="ew", pady=(0, 8))
        tb.Label(f, text="Codes").grid(row=1, column=0, sticky="nw")
        codes = tb.Text(f, width=30, height=10, font=("Consolas", 11))
        codes.insert("1.0", "\n".join("%08X %04X" % c for c in cheat.codes) if cheat else "")
        codes.grid(row=1, column=1, sticky="ew")
        tb.Label(f, text="GameShark codes, one a line: 8 hex digits, a space, 4 hex digits (80012345 0063)",
                 bootstyle="secondary", wraplength=360).grid(row=2, column=1, sticky="w", pady=(4, 0))
        on = tb.BooleanVar(value=cheat.on if cheat else False)
        tb.Checkbutton(f, text="On when the game boots", variable=on, bootstyle="round-toggle").grid(
            row=3, column=1, sticky="w", pady=(8, 0))
        result = []

        def ok():
            n = re.sub(r"[\[\]\r\n]", "", name.get()).strip()[:40]
            lines = [l.strip() for l in codes.get("1.0", "end").splitlines() if l.strip()]
            bad = [l for l in lines if not CODE.match(l)]
            if not n:
                messagebox.showerror(APP_TITLE, "The cheat needs a name.", parent=win)
                return
            if not lines or bad:
                messagebox.showerror(APP_TITLE, "Not a code: %s" % bad[0] if bad else "No codes.", parent=win)
                return
            result.append((n, [(int(CODE.match(l).group(1), 16), int(CODE.match(l).group(2), 16))
                               for l in lines], on.get()))
            win.destroy()
        bar = tb.Frame(f)
        bar.grid(row=4, column=1, sticky="e", pady=(12, 0))
        tb.Button(bar, text="Cancel", bootstyle="secondary", command=win.destroy).pack(side="left")
        tb.Button(bar, text="OK", bootstyle="success", width=8, command=ok).pack(side="left", padx=(8, 0))
        name.focus_set()
        self.wait_window(win)
        return result[0] if result else None

    def add_cheat(self):
        r = self.cheat_dialog(None)
        if r:
            self.cheats.append(Cheat(*r))
            self.changed(len(self.cheats) - 1)

    def edit_cheat(self):
        i = self.selected_cheat()
        if i is None or not self.disc or not self.disc.serial:
            return
        r = self.cheat_dialog(self.cheats[i])
        if r:
            self.cheats[i] = Cheat(*r)
            self.changed(i)

    def toggle_cheat(self):
        i = self.selected_cheat()
        if i is not None:
            self.cheats[i].on = not self.cheats[i].on
            self.changed(i)

    def delete_cheat(self):
        i = self.selected_cheat()
        if i is not None and messagebox.askyesno(APP_TITLE, "Delete \"%s\"?" % self.cheats[i].name):
            del self.cheats[i]
            self.changed(min(i, len(self.cheats) - 1))

    def import_cheats(self):
        cheatdb, pc, db = self.cheat_db
        serial = self.disc.serial
        made = pc.cht_text(db, serial, self.game.title or self.game.name)
        if not made:
            messagebox.showinfo(APP_TITLE, "The cheat database has no cheats for %s that the PicoStation and the "
                                           "PS1 DEV CART can use." % serial)
            return
        have = {c.name.lower() for c in self.cheats}
        new = [c for c in parse_cht(made[0]) if c.name.lower() not in have]
        if not new:
            messagebox.showinfo(APP_TITLE, "The game has all the database's cheats already.")
            return
        if not messagebox.askyesno(APP_TITLE, "Add %d cheats from the cheat database (off at boot)?\n\n%s" % (
                len(new), "\n".join(c.name for c in new[:25]) + ("\n..." if len(new) > 25 else ""))):
            return
        self.cheats += new
        self.changed()

    def save_cheats(self):
        d, g = self.disc, self.game
        title = g.title or g.name
        _size, problem = cht_limits(self.cheats, title, d.serial)
        if problem:
            messagebox.showerror(APP_TITLE, "Not saved: %s. Delete some cheats first." % problem)
            return False
        try:
            if self.cheats:
                with open(d.cht, "w", newline="", encoding="ascii", errors="replace") as f:
                    f.write(cht_text(self.cheats, title, d.serial))
            elif os.path.exists(d.cht):
                os.remove(d.cht)
        except OSError as e:
            messagebox.showerror(APP_TITLE, "Not saved: %s" % e)
            return False
        self.cheats_changed = False
        self.status.configure(text="Cheats saved: %s" % os.path.basename(d.cht))
        self.update_row(g)
        self.show_cheats(self.selected_cheat())
        return True

    def leave_cheats(self):
        """Cheats not saved: asked before another game or disc is shown. False: stay."""
        if not self.cheats_changed:
            return True
        r = messagebox.askyesnocancel(APP_TITLE, "Save the cheats of %s first?" % os.path.basename(self.disc.cht))
        if r is None:
            return False
        if r and not self.save_cheats():
            return False
        self.cheats_changed = False
        return True

    # --------------------------------------------------------------------------------------------- whole card
    def confirm_list(self, title, intro, lines, ok_text):
        """What would be done, listed: True if confirmed."""
        win = tb.Toplevel(self, title=title)
        win.transient(self)
        win.grab_set()
        win.geometry("760x480")
        f = tb.Frame(win, padding=14)
        f.pack(fill="both", expand=True)
        tb.Label(f, text=intro, wraplength=720, justify="left").pack(anchor="w")
        text = tb.ScrolledText(f, height=16, font=("Consolas", 10))
        text.pack(fill="both", expand=True, pady=8)
        text.insert("1.0", "\n".join(lines) or "(nothing to do)")
        text.configure(state="disabled")
        result = []
        bar = tb.Frame(f)
        bar.pack(fill="x")
        tb.Button(bar, text=ok_text, bootstyle="success", state="normal" if lines else "disabled",
                  command=lambda: (result.append(1), win.destroy())).pack(side="right")
        tb.Button(bar, text="Cancel", bootstyle="secondary", command=win.destroy).pack(side="right", padx=8)
        self.wait_window(win)
        return bool(result)

    def fill_covers(self):
        if not self.games:
            return
        if not self.db:
            messagebox.showinfo(APP_TITLE, "Choose or download the game database first.")
            return
        todo, lines, no = [], [], 0
        for g in self.games:
            if g.cover == "Cover":
                continue
            img = self.db_cover(g.serial)
            if img is None:
                no += 1
                continue
            todo.append((g, img))
            lines.append("%-50s %-12s %s -> the database's cover" % (g.name[:50], g.serial, g.cover.lower()))
        intro = ("%d games without a real cover (missing, blank or the NO COVER placeholder) have one in the "
                 "database; %d don't. Their cover files below are written (each disc's, and beside its folder). "
                 "Covers you chose are kept." % (len(todo), no))
        if not self.confirm_list("Missing covers", intro, lines, "Write %d covers" % len(todo)):
            return
        done = 0
        for g, img in todo:
            try:
                self.write_cover(g, covers.to_cov(img))
                done += 1
            except (OSError, ValueError) as e:
                messagebox.showerror(APP_TITLE, "%s: not written: %s" % (g.name, e))
                break
            self.update_row(g)
        self.status.configure(text="%d covers written" % done)
        if self.game:
            self.show_cover()

    def write_ids(self):
        """The game's ID added after the image of the covers already on the card (a library set up with images
        only, or covers made by another tool): the images aren't changed, a wrong ID is replaced."""
        if not self.games:
            return
        lines, todo, no_id = [], [], []
        for g in self.games:
            for path, serial in self.cover_targets(g):
                if not os.path.exists(path):
                    continue
                if not serial:
                    no_id.append(os.path.relpath(path, g.sd))
                    continue
                what = ids.write_cover(path, serial, True)
                if what != "already done" and not what.startswith("cover too small"):
                    todo.append((path, serial))
                    lines.append("%-60s %s" % (os.path.relpath(path, g.sd)[:60], serial))
        intro = ("The PS1 DEV CART finds a game's cheats by the game ID stored after its cover's image. %d cover "
                 "files below don't have their game's ID yet: it is added, the images aren't changed. Covers that "
                 "already have it are left alone.%s" % (len(todo), (" %d covers are skipped: their game's ID "
                 "couldn't be read from its disc image." % len(no_id)) if no_id else ""))
        if not self.confirm_list("Game IDs", intro, lines, "Write %d IDs" % len(todo)):
            return
        for path, serial in todo:
            try:
                ids.write_cover(path, serial, False)
            except OSError as e:
                messagebox.showerror(APP_TITLE, "%s: not written: %s" % (path, e))
                break
        for g in self.games:
            self.update_row(g)
        self.status.configure(text="Game IDs added to %d covers" % len(todo))


def main():
    App().mainloop()


if __name__ == "__main__":
    main()
