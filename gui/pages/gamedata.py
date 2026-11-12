"""
PS1 DEV CART GUI: Asset Extractor page. What a disc or a running game holds, without copying the whole disc:

- Disc: the disc reader runs on the console (ps1transfer -disc, see ps1transfer/disclive.c): the disc's files are
  listed (its directories only are read), and only the files chosen are copied. XA audio and STR movies keep their
  whole sectors (2336 bytes, what the Convert tab and the movie tester read); CD audio tracks are saved as WAV files
  (played by the drive and recorded through the SPU's capture buffers as it plays: the track's own length).
  A file can be scanned for the standard formats inside it (games often keep them in archive files).
- Snapshot: the game booted from its disc (ps1transfer -bootcd), then the RAM, VRAM and SPU RAM of what it runs
  (ps1transfer -snapshot), in about 15 seconds. The textures it draws are found from its GPU packets in RAM (each
  textured polygon or sprite: its texture page, CLUT and UV area) and cut from VRAM with their own colors, outlined on
  the VRAM; the RAM and SPU RAM are scanned too: sound banks (put together with their samples from SPU RAM), music,
  samples, TIMs and models as the game keeps them. A snapshot of the disc reader (no game) is pointed out.
- Assets found: the files the scanner found (assetscan.py), saved or opened in the other pages.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import queue
import threading
import time
import tkinter as tk
from tkinter import filedialog

import ttkbootstrap as tb

import assetscan
from pages.base import Page
from settings import config_dir
from widgets import ImageView

PROTOCOL = ("DISCLIVE", "DISCID", "DISCVOL", "DISCTRACK", "DISCTOC", "DISCFILE", "DISCDIR", "DISCLS", "DISCPROG",
            "DISCSAVED", "DISCERR", "DISCWARN", "DISCSTOPPED", "DISCDONE", "SNAPSHOT")
XA_FORM2, XA_INTERLEAVED, XA_CDDA = 0x1000, 0x2000, 0x4000
EXT_TYPES = {".STR": "Movie (STR)", ".XA": "XA audio", ".TIM": "Image (TIM)", ".VAB": "Sound bank (VAB)",
             ".VH": "Sound bank header (VH)", ".VB": "Sound bank samples (VB)", ".SEQ": "Music (SEQ)",
             ".SEP": "Music (SEP)", ".VAG": "Sample (VAG)", ".TMD": "Model (TMD)", ".TOD": "Animation (TOD)",
             ".EXE": "Program", ".CNF": "Boot settings", ".DA": "CD audio (the tracks)", ".MDEC": "Movie",
             ".BS": "Image (MDEC)", ".PAK": "Archive", ".BIN": "Data", ".DAT": "Data"}
VRAM_SCALE = 0.75
FILTERS = {"All": None, "Textures and images": ("Texture", "TIM"), "Sound banks and music": ("VAB", "VH", "SEQ", "SEP", "SQ", "SND", "SFX", "AKAO"),
           "Samples": ("VAG", "Sample"), "Models": ("TMD",)}
DRIVE_STATES = {2: "licensed disc", 5: "unlicensed disc or audio CD"}


def file_type(path, attr):
    ext = os.path.splitext(path)[1].upper()
    if attr & XA_CDDA:
        return "CD audio"
    if ext in EXT_TYPES:
        return EXT_TYPES[ext]
    if attr & (XA_FORM2 | XA_INTERLEAVED):
        return "XA sectors (audio or movie)"
    if len(ext) == 3 and ext[1:].isdigit():      # SLES_123.45
        return "Program (the game)"
    return "Data"


def size_text(n):
    return "%d B" % n if n < 1024 else "%.1f KB" % (n / 1024) if n < 1024 * 1024 else "%.1f MB" % (n / 1048576)


class GameDataPage(Page):
    title = "Asset Extractor"
    needs_devcart = "all"

    def build(self):
        self.events = queue.Queue()
        self.disc_live = False
        self.busy_cmd = None        # Command sent to the disc reader, its answer awaited
        self.cmd_started, self.cmd_name = 0.0, ""
        self.volume = "disc"
        self.disc_scan = None       # Scan the whole disc: {"total", "saved", "scanned", "found", "folder"}
        self.pending = []           # (command line, function called with its DISCSAVED fields)
        self.on_saved = None
        self.files = {}             # Disc path -> (lba, size, attr)
        self.tracks = []            # (track, lba, sectors)
        self.snapshot = None
        self.vram_photo = None
        self.textures = []          # The snapshot's textures in use (assets "Texture"), the one chosen
        self.tex_sel = None
        self.snap_text = ""
        self.assets = []
        self.shown = []             # The assets in the list (the filter)
        self.asset_base = "snapshot"

        tb.Label(self, text=self.title, font=("", 16, "bold")).pack(anchor="w")
        tb.Label(self, text="What a disc or a running game holds, without dumping the whole disc: copy only the files "
                 "you choose from the disc in the console's drive, or take a snapshot of what the running game has "
                 "loaded (RAM, VRAM, SPU RAM) and find its textures, sound banks, music and models.",
                 wraplength=900, justify="left").pack(anchor="w", pady=(2, 8))
        self.tabs = tb.Notebook(self)
        self.tabs.pack(fill="both", expand=True)
        for title, build in (("Disc", self.build_disc), ("Snapshot", self.build_snapshot),
                             ("Assets found", self.build_assets)):
            frame = tb.Frame(self.tabs, padding=8)
            self.tabs.add(frame, text=title)
            build(frame)
        bottom = tb.Frame(self)
        bottom.pack(fill="x", pady=(6, 0))
        self.progress = tb.Progressbar(bottom, length=220, maximum=100, bootstyle="info-striped")
        self.progress.pack(side="left")
        self.status = tb.Label(bottom, text="", bootstyle="secondary", wraplength=900, justify="left")
        self.status.pack(side="left", padx=10, fill="x", expand=True)
        self.after(50, self.poll_events)

    def post(self, fn, *args, **kwargs):
        """fn(*args, **kwargs) run on the main thread (from a worker thread)."""
        self.events.put((fn, args, kwargs))

    def poll_events(self):
        try:
            while True:
                fn, args, kwargs = self.events.get_nowait()
                fn(*args, **kwargs)
        except queue.Empty:
            pass
        self.after(50, self.poll_events)

    def out_dir(self, *parts):
        d = os.path.join(config_dir(), "gamedata", *parts)
        os.makedirs(d, exist_ok=True)
        return d

    # ------------------------------------------------------------------------------------------------ Disc

    def build_disc(self, f):
        row = tb.Frame(f)
        row.pack(fill="x")
        self.disc_start = tb.Button(row, text="Read the disc", bootstyle="success", command=self.start_disc)
        self.disc_start.pack(side="left")
        tb.Button(row, text="Read again", bootstyle="secondary-outline", command=self.reload_disc).pack(side="left",
                                                                                                         padx=6)
        tb.Button(row, text="Stop the motor", bootstyle="secondary-outline",
                  command=lambda: self.disc_command("stop")).pack(side="left")
        tb.Label(row, text="Speed").pack(side="left", padx=(16, 4))
        self.speed = tb.Combobox(row, values=("2x", "1x"), width=4, state="readonly")
        self.speed.set("2x")
        self.speed.pack(side="left")
        self.speed.bind("<<ComboboxSelected>>", lambda e: self.disc_command("speed %s" % self.speed.get()[0]))
        self.disc_info = tb.Label(f, text="Put a disc in the console's drive, then Read the disc: the disc reader "
                                  "runs on the console (the program running now is stopped).", bootstyle="secondary",
                                  wraplength=900, justify="left")
        self.disc_info.pack(anchor="w", pady=6)

        box = tb.Frame(f)
        box.pack(fill="both", expand=True)
        self.tree = tb.Treeview(box, columns=("size", "type", "lba"), selectmode="extended")
        for col, text, w in (("#0", "File", 360), ("size", "Size", 90), ("type", "Type", 220), ("lba", "Sector", 80)):
            self.tree.heading(col, text=text, anchor="w")
            self.tree.column(col, width=w, stretch=(col == "#0"))
        sb = tb.Scrollbar(box, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        sb.pack(side="left", fill="y")
        self.tree.bind("<Double-1>", lambda e: self.save_disc_files(scan=False))

        row = tb.Frame(f)
        row.pack(fill="x", pady=(6, 0))
        tb.Button(row, text="Save selected...", bootstyle="primary", command=lambda: self.save_disc_files(False)).pack(
            side="left")
        tb.Button(row, text="Save and scan for assets", bootstyle="info-outline",
                  command=lambda: self.save_disc_files(True)).pack(side="left", padx=6)
        tb.Button(row, text="Scan the whole disc", bootstyle="info", command=self.scan_disc).pack(side="left",
                                                                                                    padx=(0, 6))
        self.stop_button = tb.Button(row, text="Stop", bootstyle="danger-outline", command=self.stop_saving,
                                     state="disabled")
        self.stop_button.pack(side="left")
        self.xa_wav = tk.BooleanVar(value=True)
        tb.Checkbutton(row, text="XA audio: also as WAV", variable=self.xa_wav).pack(side="left", padx=10)
        self.track_secs = tk.StringVar(value="")
        tb.Label(row, text="CD audio: first").pack(side="left", padx=(10, 4))
        tb.Entry(row, textvariable=self.track_secs, width=5).pack(side="left")
        tb.Label(row, text="seconds (empty: the whole track; recorded as it plays: as long as the track)").pack(
            side="left", padx=4)

    def args(self):
        return ["-disc"]

    def start_disc(self):
        if self.disc_live:
            return self.reload_disc()
        self.pending, self.busy_cmd = [], None
        self.tree.delete(*self.tree.get_children())
        self.files, self.tracks = {}, []
        self.disc_info.configure(text="Starting the disc reader on the console...")
        self.app.run(["-disc"], "Disc reader", long_running=True, stdin=True,
                     on_line=lambda line: self.after(0, self.disc_line, line),
                     log_filter=lambda line: not line.startswith(PROTOCOL),
                     on_done=lambda code, out: self.after(0, self.disc_stopped, out))

    def reload_disc(self):
        if not self.disc_live:
            return self.start_disc()
        self.tree.delete(*self.tree.get_children())
        self.files, self.tracks = {}, []
        for c in ("init", "toc", "ls"):
            self.disc_command(c)
        return None

    def disc_stopped(self, output=()):
        if not self.disc_live:      # (it stopped before it was ready: why)
            why = [l for l in output if l.startswith("DISCERR") or "error" in l.lower() or "not" in l.lower()]
            self.disc_info.configure(text="The disc reader stopped: %s" % (why[-1].replace("DISCERR ", "") if why else
                                                                            "see the console page."))
        self.disc_live = False
        self.busy_cmd = None
        self.pending = []
        self.disc_start.configure(text="Read the disc")
        self.stop_button.configure(state="disabled")

    def disc_command(self, line, on_saved=None):
        if not self.disc_live:
            return False
        self.pending.append((line, on_saved))
        self.next_command()
        return True

    def stop_saving(self):
        """The files waiting dropped, the one being copied (or the track recorded) stopped: nothing written for it."""
        self.pending = [(line, cb) for line, cb in self.pending if not line.startswith(("get", "track"))]
        if self.busy_cmd and (self.busy_cmd.startswith("get") or self.busy_cmd == "track"):
            self.app.runner.send("stop")
            self.status.configure(text="Stopping...")
        self.stop_button.configure(state="disabled")

    def next_command(self):
        if self.busy_cmd or not self.pending:
            return
        line, self.on_saved = self.pending.pop(0)
        self.busy_cmd = line.split()[0]
        self.cmd_started = time.time()
        self.stop_button.configure(state="normal" if self.busy_cmd.startswith(("get", "track")) else "disabled")
        f = line.split(None, 2)
        self.cmd_name = os.path.basename(f[1]) if self.busy_cmd.startswith("get") and len(f) > 1 else             ("track %s" % f[1] if self.busy_cmd == "track" and len(f) > 1 else "")
        self.app.runner.send(line)

    def disc_line(self, line):
        f = line.split()
        if not f:
            return
        kind = f[0]
        if kind == "DISCLIVE" and f[1:2] == ["ready"]:
            self.disc_live = True
            self.disc_start.configure(text="Read the disc again")
            for c in ("init", "toc", "ls"):
                self.disc_command(c)
        elif kind == "DISCID" and len(f) >= 3:
            irq, idb = int(f[1]), bytes.fromhex(f[2])
            region = idb[4:8].decode("ascii", "replace").strip("\0 ")
            self.disc_text = "Disc: %s%s" % (DRIVE_STATES.get(irq, "?"), (" (%s)" % region) if region else "")
            self.disc_info.configure(text=self.disc_text)
        elif kind == "DISCVOL":
            self.volume = "".join(c for c in " ".join(f[1:]) if c.isalnum() or c in "_-") or "disc"
            self.disc_text += ", volume %s" % " ".join(f[1:])
            self.disc_info.configure(text=self.disc_text)
        elif kind == "DISCTRACK" and len(f) >= 4:
            self.tracks.append((int(f[1]), int(f[2]), int(f[3])))
        elif kind == "DISCTOC":
            audio = [t for t in self.tracks if t[0] > 1]
            if audio:
                node = self.tree.insert("", "end", iid="#tracks", text="CD audio tracks", open=True)
                for t, lba, n in audio:
                    self.tree.insert(node, "end", iid="#track%d" % t, text="Track %02d" % t,
                                     values=("%d:%02d" % (n // 75 // 60, n // 75 % 60), "CD audio (WAV)", lba))
            self.disc_text += ", %d track%s" % (len(self.tracks), "s" if len(self.tracks) > 1 else "")
            self.disc_info.configure(text=self.disc_text)
        elif kind == "DISCDIR":
            path = " ".join(f[1:])
            parent = path.rsplit("\\", 1)[0]
            self.tree.insert(parent if parent and self.tree.exists(parent) else "", "end", iid=path,
                             text=path.rsplit("\\", 1)[-1], values=("", "Folder", ""))
        elif kind == "DISCFILE" and len(f) >= 5:
            lba, size, attr, path = int(f[1]), int(f[2]), int(f[3], 16), " ".join(f[4:])
            self.files[path] = (lba, size, attr)
            parent = path.rsplit("\\", 1)[0]
            self.tree.insert(parent if parent and self.tree.exists(parent) else "", "end", iid=path,
                             text=path.rsplit("\\", 1)[-1], values=(size_text(size), file_type(path, attr), lba))
        elif kind == "DISCLS":
            self.status.configure(text="%d files on the disc. Choose files, then Save selected." % len(self.files))
        elif kind == "DISCPROG" and len(f) >= 3:
            done, total = int(f[1]), int(f[2])
            self.progress.configure(value=100 * done / max(1, total))
            secs = max(0.1, time.time() - self.cmd_started)
            if self.busy_cmd == "track":
                self.status.configure(text="Recording %s: %d:%02d of %d:%02d (recorded as it plays)" % (
                    self.cmd_name, done // 4500, done // 75 % 60, total // 4500, total // 75 % 60))
            else:
                left = len(self.pending)
                self.status.configure(text="Copying %s: %d of %d sectors, %.0f KB/s%s" % (
                    self.cmd_name, done, total, done * 2048 / 1024 / secs,
                    (", %d more file%s after it" % (left, "" if left == 1 else "s")) if left else ""))
        elif kind == "DISCSAVED" and len(f) >= 5:
            self.progress.configure(value=100)
            if self.on_saved:
                self.on_saved(line)
        elif kind == "DISCWARN":
            self.status.configure(text="Disc reader: " + " ".join(f[1:]))
        elif kind == "DISCERR":
            self.status.configure(text="Disc reader: " + " ".join(f[1:]))
            if self.busy_cmd == "init":
                self.disc_info.configure(text=" ".join(f[1:]).capitalize() + ". Put a disc in, then Read the disc again.")
        elif kind == "DISCSTOPPED":
            self.status.configure(text="Stopped: %s not saved." % (self.cmd_name or "the file"))
            self.progress.configure(value=0)
        elif kind == "DISCDONE":
            self.stop_button.configure(state="disabled")
            self.busy_cmd = None
            self.on_saved = None
            self.next_command()
            if self.disc_scan is not None:
                self.disc_scan_progress()

    def selected_files(self):
        """The disc files selected (a folder: its files), and the CD audio tracks."""
        out, tracks = [], []

        def add(iid):
            if iid.startswith("#track"):
                tracks.append(int(iid[6:]))
            elif iid == "#tracks":
                for c in self.tree.get_children(iid):
                    add(c)
            elif iid in self.files:
                out.append(iid)
            else:
                for c in self.tree.get_children(iid):
                    add(c)
        for iid in self.tree.selection():
            add(iid)
        return list(dict.fromkeys(out)), list(dict.fromkeys(tracks))

    def save_disc_files(self, scan):
        if not self.disc_live:
            return self.app.error("Read the disc first.")
        paths, tracks = self.selected_files()
        if not paths and not tracks:
            return self.app.error("Select files on the disc.")
        if scan:
            folder = self.out_dir("disc")
        else:
            folder = filedialog.askdirectory(title="Save the files in", parent=self,
                                             initialdir=self.app.settings.field("gamedata.folder", ""))
            if not folder:
                return None
            self.app.settings.set_field("gamedata.folder", folder)
        total = sum((self.files[p][1] for p in paths), 0)
        self.status.configure(text="Copying %d file%s (%s) from the disc..." % (
            len(paths) + len(tracks), "s" if len(paths) + len(tracks) > 1 else "", size_text(total)))
        self.saved_now = []
        for p in paths:
            out = os.path.join(folder, *p.strip("\\").split("\\"))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            self.disc_command("get %s %s" % (p, out), lambda f, scan=scan: self.file_saved(f, scan))
        secs = self.track_secs.get().strip()
        for t in tracks:
            out = os.path.join(folder, "track%02d.wav" % t)
            self.disc_command("track %d %s %s" % (t, secs if secs.isdigit() else "0", out),
                              lambda f: self.file_saved(f, False))
        return None

    def file_saved(self, line, scan):
        # DISCSAVED BYTES RETRIES MODE OUT
        f = line.split(None, 4)
        size, retries, mode, out = int(f[1]), int(f[2]), f[3], f[4]
        self.saved_now.append(out)
        self.status.configure(text="Saved %s (%s%s)" % (out, size_text(size), (", %d sectors read again" % retries)
                                                        if retries else ""))
        if mode == "xa" and out.upper().endswith(".XA") and self.xa_wav.get():
            threading.Thread(target=self.xa_to_wav, args=(out,), daemon=True).start()
        if scan and mode == "data":
            threading.Thread(target=self.scan_file, args=(out, scan == "disc"), daemon=True).start()

    def xa_to_wav(self, path):
        import audioconv
        try:
            written = audioconv.xa_to_audio(path, os.path.splitext(path)[0], ".wav")
            self.post(self.status.configure, text="XA audio converted: %s" % ", ".join(
                os.path.basename(p) for p in (written or [])))
        except Exception as e:      # (an XA file without audio sectors: a movie's)
            self.post(self.status.configure, text="%s: no XA audio to convert (%s)" % (os.path.basename(path), e))

    def scan_file(self, path, whole_disc=False):
        try:
            with open(path, "rb") as f:
                data = f.read()
            found = assetscan.scan_bytes(data, os.path.basename(path))
        except (OSError, ValueError, MemoryError):
            found = []
        if whole_disc:
            self.post(self.disc_file_scanned, found)
            return
        self.post(self.show_assets, found, os.path.splitext(os.path.basename(path))[0],
                  "%s: %d asset%s found" % (os.path.basename(path), len(found), "" if len(found) == 1 else "s"))

    # Scan the whole disc: every data file copied (into the settings folder) and scanned, the results added up

    def scan_disc(self):
        if not self.disc_live or not self.files:
            return self.app.error("Read the disc first.")
        skip = (XA_FORM2 | XA_INTERLEAVED | XA_CDDA)
        paths = [p for p, (lba, size, attr) in sorted(self.files.items(), key=lambda kv: kv[1][0])
                 if not attr & skip and os.path.splitext(p)[1].upper() not in (".STR", ".XA", ".DA") and size]
        if not paths:
            return self.app.error("No data files on this disc (only audio and movies).")
        folder = self.out_dir("disc", self.volume)
        total = sum(self.files[p][1] for p in paths)
        self.disc_scan = {"total": len(paths), "saved": 0, "scanned": 0, "found": [], "folder": folder}
        self.saved_now = []
        self.show_assets([], self.volume, "Scanning the disc: 0 of %d files" % len(paths), switch=False)
        self.status.configure(text="Scanning the whole disc: %d files (%s; XA, STR and CD audio left out)..." % (
            len(paths), size_text(total)))
        for p in paths:
            out = os.path.join(folder, *p.strip("\\").split("\\"))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            self.disc_command("get %s %s" % (p, out), lambda f: self.disc_file_copied(f))
        return None

    def disc_file_copied(self, line):
        if self.disc_scan is None:
            return self.file_saved(line, False)
        self.disc_scan["saved"] += 1
        self.file_saved(line, "disc")
        return None

    def disc_file_scanned(self, found):
        d = self.disc_scan
        if d is None:
            return
        d["scanned"] += 1
        d["found"] += found
        self.disc_scan_progress()

    def disc_scan_progress(self, stopped=False):
        d = self.disc_scan
        if d is None:
            return
        copying = self.busy_cmd == "get" or any(l.startswith("get") for l, _ in self.pending)
        finished = not copying and d["scanned"] >= d["saved"]
        text = "Disc scan: %d of %d files, %d asset%s found" % (d["scanned"], d["total"], len(d["found"]),
                                                                "" if len(d["found"]) == 1 else "s")
        if finished:
            text = ("Disc scan %s: %d of %d files scanned, %d asset%s found (the files copied to %s)" % (
                "stopped" if d["scanned"] < d["total"] else "done", d["scanned"], d["total"], len(d["found"]),
                "" if len(d["found"]) == 1 else "s", d["folder"]))
        if finished:
            self.pair_banks(d)
        self.show_assets(d["found"], self.volume, text, switch=finished and bool(d["found"]))
        if finished:
            self.disc_scan = None

    @staticmethod
    def pair_banks(d):
        """A VH found in a file NAME.VH with a NAME.VB beside it on the disc (the samples of a bank are often a file of
        their own): a whole VAB (VH + VB), if the VB's size is the VH's samples."""
        for a in d["found"]:
            if a.kind != "VH" or not a.where.upper().endswith(".VH"):
                continue
            for root, _, files in os.walk(d["folder"]):
                vb = next((f for f in files if f.upper() == a.where.upper()[:-3] + ".VB"), None)
                if not vb:
                    continue
                with open(os.path.join(root, vb), "rb") as f:
                    data = f.read()
                v = assetscan.vh_at(a.data, 0)
                if v and sum(assetscan.vag_sizes(a.data, v[0], v[3])) == len(data):
                    a.kind, a.parts = "VAB", {".vb": data}
                    a.info = a.info.replace("samples not found (VH only)", "with its samples (%s)" % vb)
                break

    # ------------------------------------------------------------------------------------------------ Snapshot

    def build_snapshot(self, f):
        row = tb.Frame(f)
        row.pack(fill="x")
        tb.Button(row, text="1. Boot the game from its disc", bootstyle="info", command=self.boot_game).pack(side="left")
        tb.Button(row, text="2. Take a snapshot", bootstyle="success", command=self.take_snapshot).pack(side="left",
                                                                                                         padx=6)
        tb.Button(row, text="Open a snapshot...", bootstyle="secondary-outline", command=self.open_snapshot).pack(
            side="left", padx=(12, 0))
        tb.Button(row, text="Open its folder", bootstyle="secondary-outline", command=self.open_snapshot_folder).pack(
            side="left", padx=6)
        tb.Button(row, text="SPU RAM Viewer", bootstyle="secondary-outline", command=self.open_spu).pack(side="left")
        self.snap_info = tb.Label(f, text="Put the game in the console's drive and boot it (button 1, or from the PS1 "
                                  "DEV CART menu), play to the part you want (a race, a level), then take a snapshot "
                                  "(button 2): what the game has loaded is read in about 15 seconds. The textures it "
                                  "draws are shown on its VRAM below, with their own colors; its sound bank, music and "
                                  "samples are in the Assets found tab.", bootstyle="secondary", wraplength=1000,
                                  justify="left")
        self.snap_info.pack(anchor="w", pady=6)

        body = tb.Frame(f)
        body.pack(fill="both", expand=True)
        w, h = int(assetscan.VRAM_W * VRAM_SCALE), int(assetscan.VRAM_H * VRAM_SCALE)
        self.vram_canvas = tk.Canvas(body, width=w, height=h, background="#000000", highlightthickness=0,
                                     cursor="hand2")
        self.vram_canvas.pack(side="left", anchor="n")
        self.vram_canvas.bind("<Button-1>", self.vram_click)
        self.vram_canvas.bind("<Motion>", self.vram_motion)

        side = tb.Frame(body, padding=(10, 0, 0, 0))
        side.pack(side="left", fill="both", expand=True)
        tb.Label(side, text="The VRAM: the yellow outlines are the textures the game draws (from its drawing commands "
                 "in RAM). Click one.", wraplength=270, justify="left").pack(anchor="w")
        self.tex_label = tb.Label(side, text="", wraplength=270, justify="left")
        self.tex_label.pack(anchor="w", pady=(6, 0))
        self.pos_label = tb.Label(side, text="", bootstyle="secondary")
        self.pos_label.pack(anchor="w")
        self.texture = ImageView(side, width=270, height=220)
        self.texture.pack(fill="both", expand=True, pady=6)
        row = tb.Frame(side)
        row.pack(fill="x")
        tb.Button(row, text="Save PNG...", bootstyle="primary-outline", command=lambda: self.save_texture("png")).pack(
            side="left")
        tb.Button(row, text="Save TIM...", bootstyle="primary-outline", command=lambda: self.save_texture("tim")).pack(
            side="left", padx=6)
        tb.Button(row, text="All textures...", bootstyle="primary", command=self.save_all_textures).pack(side="left")
        tb.Button(side, text="Save the whole VRAM as PNG...", bootstyle="secondary-outline",
                  command=self.save_vram).pack(anchor="w", pady=(6, 0))

    def boot_game(self):
        self.snap_info.configure(text="Booting the disc in the drive (the console resets, the game starts on the TV). "
                                 "Play to the part you want, then Take a snapshot.")
        self.app.run(["-bootcd"], "Boot the disc")

    def take_snapshot(self):
        stamp = time.strftime("%Y%m%d-%H%M%S")
        folder = self.out_dir("snapshots", stamp)
        self.snap_info.configure(text="Reading the RAM, VRAM and SPU RAM (about 15 seconds)...")
        self.progress.configure(value=0)

        def done(code, output):
            skipped = [l[len("SNAPSHOT skipped "):] for l in output if l.startswith("SNAPSHOT skipped")]
            if code != 0 or not os.path.isfile(os.path.join(folder, "ram.bin")):
                why = [l for l in output if "No program" in l or "error" in l.lower()]
                self.after(0, self.snap_info.configure, {"text": (why[-1] if why else "The snapshot couldn't be taken "
                                                                  "(see the console).") + " Boot the game first."})
                return
            self.after(0, self.load_snapshot, folder, skipped)
        self.app.run(["-snapshot", folder], "Snapshot", on_done=done)

    def open_snapshot(self):
        folder = filedialog.askdirectory(title="Snapshot folder (ram.bin, vram.bin, spu.bin)", parent=self,
                                         initialdir=self.out_dir("snapshots"))
        if folder:
            self.load_snapshot(folder, [])

    def open_snapshot_folder(self):
        if self.snapshot:
            os.startfile(self.snapshot.folder)

    def open_spu(self):
        if not self.snapshot or not self.snapshot.spu:
            return self.app.error("Take a snapshot first (with the SPU RAM).")
        page = self.app.pages.get("SPU RAM Viewer")
        page.load(os.path.join(self.snapshot.folder, "spu.bin"))
        self.app.show_page("SPU RAM Viewer")
        return None

    def load_snapshot(self, folder, skipped):
        try:
            snap = assetscan.Snapshot(folder)
        except (OSError, ValueError) as e:
            return self.app.error(str(e))
        if snap.game and os.path.basename(folder)[:1].isdigit():      # (renamed after the game it holds)
            new = os.path.join(os.path.dirname(folder), snap.game.replace(".", "") + "_" + os.path.basename(folder))
            try:
                os.rename(folder, new)
                snap.folder = folder = new
            except OSError:
                pass
        self.snapshot = snap
        self.textures, self.tex_sel = [], None
        if snap.program == "game":
            text = "Snapshot of %s." % snap.game
        elif snap.program == "disc reader":
            text = ("This is a snapshot of the disc reader (the Disc tab's program), not of a game: what it shows is "
                    "only what is left in memory from before. Boot the game (button 1), play to the part you want, "
                    "then take the snapshot again.")
        else:
            text = ("No game booted from its disc was running: this is a snapshot of the program running. For a "
                    "game, boot it with button 1 (or from the PS1 DEV CART menu) first.")
        if skipped:
            text += "  Not read: " + "; ".join(skipped)
        self.snap_text = text
        self.snap_info.configure(text=text + "  Scanning...", bootstyle="secondary" if snap.program == "game" else
                                 "warning")
        self.draw_vram()
        self.texture.show(None)
        self.tex_label.configure(text="")
        threading.Thread(target=self.scan_snapshot, args=(snap,), daemon=True).start()
        return None

    def scan_snapshot(self, snap):
        found = snap.scan(progress=lambda n, t: self.post(self.progress.configure, value=100 * n / max(1, t)))
        base = snap.game.replace(".", "") if snap.game else "snapshot"
        self.post(self.snapshot_scanned, snap, found, base)

    def snapshot_scanned(self, snap, found, base):
        if snap is not self.snapshot:
            return
        self.textures = [a for a in found if a.kind == "Texture"]
        self.snap_info.configure(text="%s  %d textures drawn by the game found (outlined below), %d other assets "
                                 "(the Assets found tab)." % (self.snap_text, len(self.textures),
                                                              len(found) - len(self.textures)))
        self.draw_vram()
        self.show_assets(found, base, "Snapshot: %d asset%s found" % (len(found), "" if len(found) == 1 else "s"),
                         switch=False)

    # VRAM view: the textures in use outlined, one chosen

    def draw_vram(self):
        from PIL import Image, ImageTk
        c = self.vram_canvas
        c.delete("all")
        if not self.snapshot or not self.snapshot.vram:
            return
        rgb = assetscan.vram_rgb(assetscan.vram_words(self.snapshot.vram))
        img = Image.fromarray(rgb, "RGB").resize((int(assetscan.VRAM_W * VRAM_SCALE),
                                                  int(assetscan.VRAM_H * VRAM_SCALE)), Image.BILINEAR)
        self.vram_photo = ImageTk.PhotoImage(img)
        c.create_image(0, 0, image=self.vram_photo, anchor="nw")
        s = VRAM_SCALE
        for t in self.textures:
            x, y, w, h = self.tex_rect(t)
            c.create_rectangle(x * s, y * s, (x + w) * s, (y + h) * s, outline="#ffd43b", width=1)
        if self.tex_sel is not None:
            x, y, w, h = self.tex_rect(self.textures[self.tex_sel])
            c.create_rectangle(x * s - 1, y * s - 1, (x + w) * s + 1, (y + h) * s + 1, outline="#ff6b6b", width=3)

    @staticmethod
    def tex_rect(asset):
        """A texture asset's VRAM rectangle (VRAM units), from its TIM's image block."""
        _, mode, img, clut = assetscan.tim_at(asset.data, 0)
        return img[1], img[2], img[3], img[4]

    def vram_xy(self, event):
        return (max(0, min(1023, int(event.x / VRAM_SCALE))), max(0, min(511, int(event.y / VRAM_SCALE))))

    def texture_at(self, x, y):
        """The smallest texture outline around x, y (its index), None."""
        best = None
        for i, t in enumerate(self.textures):
            tx, ty, tw, th = self.tex_rect(t)
            if tx <= x < tx + tw and ty <= y < ty + th and (best is None or tw * th < best[1]):
                best = (i, tw * th)
        return best[0] if best else None

    def vram_motion(self, event):
        x, y = self.vram_xy(event)
        i = self.texture_at(x, y)
        self.pos_label.configure(text="VRAM %d,%d (texture page %d)%s" % (
            x, y, x // 64 + (y // 256) * 16, ("  - texture: " + self.textures[i].info.split(",")[0]) if i is not None
            else ""))

    def vram_click(self, event):
        i = self.texture_at(*self.vram_xy(event))
        if i is None:
            return
        self.tex_sel = i
        t = self.textures[i]
        self.tex_label.configure(text=t.info)
        self.texture.show(assetscan.tim_image(t.data))
        self.draw_vram()

    def save_texture(self, kind):
        if self.tex_sel is None:
            return self.app.error("Click a texture (a yellow outline) on the VRAM first.")
        t = self.textures[self.tex_sel]
        path = filedialog.asksaveasfilename(defaultextension="." + kind, parent=self,
                                            filetypes=[("PNG image" if kind == "png" else "TIM image", "*." + kind)],
                                            initialdir=self.app.settings.field("gamedata.assets", ""),
                                            initialfile=os.path.splitext(t.name(self.asset_base))[0] + "." + kind)
        if not path:
            return None
        with open(path, "wb") as f:
            f.write(t.parts[".png"] if kind == "png" else t.data)
        self.status.configure(text="Saved %s" % path)
        return None

    def save_all_textures(self):
        if not self.textures:
            return self.app.error("No textures: boot the game, then take a snapshot.")
        folder = filedialog.askdirectory(title="Save the textures (TIM and PNG) in", parent=self,
                                         initialdir=self.app.settings.field("gamedata.assets", ""))
        if not folder:
            return None
        self.app.settings.set_field("gamedata.assets", folder)
        for t in self.textures:
            t.save(folder, self.asset_base)
        self.status.configure(text="%d textures (TIM and PNG) written to %s" % (len(self.textures), folder))
        return None

    def save_vram(self):
        from PIL import Image
        if not self.snapshot or not self.snapshot.vram:
            return self.app.error("Take a snapshot first.")
        path = filedialog.asksaveasfilename(defaultextension=".png", parent=self, filetypes=[("PNG image", "*.png")],
                                            initialdir=self.snapshot.folder, initialfile="vram.png")
        if path:
            Image.fromarray(assetscan.vram_rgb(assetscan.vram_words(self.snapshot.vram)), "RGB").save(path)
            self.status.configure(text="Saved %s" % path)
        return None

    # ------------------------------------------------------------------------------------------------ Assets

    def build_assets(self, f):
        self.assets_info = tb.Label(f, text="Nothing scanned yet: take a snapshot, or save a disc file with Save and "
                                    "scan for assets.", bootstyle="secondary", wraplength=900, justify="left")
        self.assets_info.pack(anchor="w")
        row = tb.Frame(f)
        row.pack(fill="x", pady=(6, 0))
        tb.Label(row, text="Show").pack(side="left")
        self.filter = tb.Combobox(row, values=list(FILTERS), state="readonly", width=24)
        self.filter.set("All")
        self.filter.pack(side="left", padx=6)
        self.filter.bind("<<ComboboxSelected>>", lambda e: self.fill_assets())
        body = tb.Frame(f)
        body.pack(fill="both", expand=True, pady=6)
        self.alist = tb.Treeview(body, columns=("kind", "where", "offset", "size", "info"), show="headings",
                                 selectmode="extended")
        for col, text, w in (("kind", "Type", 70), ("where", "Found in", 110), ("offset", "Offset", 80),
                             ("size", "Size", 80), ("info", "Details", 420)):
            self.alist.heading(col, text=text, anchor="w")
            self.alist.column(col, width=w, stretch=(col == "info"))
        sb = tb.Scrollbar(body, orient="vertical", command=self.alist.yview)
        self.alist.configure(yscrollcommand=sb.set)
        self.alist.pack(side="left", fill="both", expand=True)
        sb.pack(side="left", fill="y")
        self.alist.bind("<<TreeviewSelect>>", lambda e: self.preview_asset())
        self.preview = ImageView(body, width=280)
        self.preview.pack(side="left", fill="both", padx=(8, 0))

        row = tb.Frame(f)
        row.pack(fill="x")
        tb.Button(row, text="Save selected...", bootstyle="primary", command=lambda: self.save_assets(False)).pack(
            side="left")
        tb.Button(row, text="Save all...", bootstyle="primary-outline", command=lambda: self.save_assets(True)).pack(
            side="left", padx=6)
        tb.Button(row, text="Open in the tools", bootstyle="info-outline", command=self.open_assets).pack(side="left")
        tb.Label(row, text="Textures and TIMs: VRAM Viewer; VAB, SEQ, VAG: Sound Tools; TMD: Model Viewer",
                 bootstyle="secondary").pack(side="left", padx=10)

    def fill_assets(self):
        kinds = FILTERS[self.filter.get()]
        self.shown = [a for a in self.assets if not kinds or a.kind in kinds]
        self.alist.delete(*self.alist.get_children())
        for i, a in enumerate(self.shown):
            self.alist.insert("", "end", iid=str(i), values=(a.kind, a.where, "0x%06X" % a.offset, size_text(a.size),
                                                             a.info))

    def show_assets(self, found, base, text, switch=True):
        self.assets, self.asset_base = found, base
        self.fill_assets()
        counts = {}
        for a in found:
            counts[a.kind] = counts.get(a.kind, 0) + 1
        detail = ", ".join("%d %s" % (n, k) for k, n in counts.items())
        self.assets_info.configure(text=text + ((": " + detail) if detail else "") +
                                   ". Textures: the ones the game draws, cut from VRAM. The others: the standard "
                                   "formats as the game keeps them in memory (compressed data can't be found).")
        self.status.configure(text=text)
        self.progress.configure(value=100)
        if found and switch:
            self.tabs.select(2)

    def chosen_assets(self):
        return [self.shown[int(i)] for i in self.alist.selection()]

    def preview_asset(self):
        sel = self.chosen_assets()
        if sel and sel[0].kind in ("TIM", "Texture"):
            try:
                self.preview.show(assetscan.tim_image(sel[0].data))
            except Exception:       # (a TIM the scanner kept but PIL can't draw)
                self.preview.show(None)
        else:
            self.preview.show(None)

    def save_assets(self, everything):
        items = self.shown if everything else self.chosen_assets()
        if not items:
            return self.app.error("Nothing to save: select assets in the list.")
        folder = filedialog.askdirectory(title="Save the assets in", parent=self,
                                         initialdir=self.app.settings.field("gamedata.assets", ""))
        if not folder:
            return None
        self.app.settings.set_field("gamedata.assets", folder)
        n = 0
        for a in items:
            n += len(a.save(folder, self.asset_base))
        self.status.configure(text="%d file%s written to %s" % (n, "" if n == 1 else "s", folder))
        return None

    @staticmethod
    def music_groups(saved):
        """The tester's files among an SQ's or SND's saved files: [[.vh, .vb, its (first) .seq]] for each asset."""
        groups, bases = [], []
        for p in saved:
            if p.lower().endswith((".sq", ".snd", ".snf")):
                bases.append(os.path.splitext(p)[0])
        for base in bases:
            seqs = sorted((p for p in saved if p.startswith(base) and p.lower().endswith(".seq")), key=len)
            banks = [base + e for e in (".vh", ".vb") if base + e in saved]
            if seqs:
                groups.append(banks + seqs[:1])
        return groups

    def open_assets(self):
        items = self.chosen_assets()
        if not items:
            return self.app.error("Select assets in the list.")
        folder = self.out_dir("assets", self.asset_base)
        paths = {a.kind: [] for a in items}
        for a in items:
            paths[a.kind] += a.save(folder, self.asset_base)
        tims = [p for k in ("Texture", "TIM") for p in paths.get(k, []) if p.endswith(".tim")]
        if tims:
            page = self.app.pages["VRAM Viewer"]
            page.files.set_files(tims[:32])
            if hasattr(page, "load_files"):
                page.load_files()
            self.app.show_page("VRAM Viewer")
            return None
        if paths.get("TMD"):
            page = self.app.pages["Model & Animation Viewer"]
            page.tmd.set(paths["TMD"][0])
            self.app.show_page("Model & Animation Viewer")
            return None
        sound = [p for k in ("VAB", "VH", "SEQ", "SEP", "VAG", "Sample") for p in paths.get(k, [])]
        for k in ("SQ", "SND", "SFX", "AKAO"):      # (games' music: their files converted for the tester, a bank's 1st music)
            for group in self.music_groups(paths.get(k, [])):
                sound += group
        if sound:
            page = self.app.pages["Sound Tools"]
            page.files.set_files(sound[:8])
            self.app.show_page("Sound Tools")
            return None
        return self.app.error("No tool opens these assets.")
