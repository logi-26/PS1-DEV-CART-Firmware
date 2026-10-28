"""
PS1 DEV CART GUI: SPU RAM Viewer page, the sound counterpart of the VRAM Viewer: the 512KB of SPU RAM of the running
program read by the cartridge ("ps1transfer -spu", debug monitor version 10), mapped (samples, free space, reverb
work area, capture buffers), each sample with the voices using it, decoded and played on the computer, saved as WAV
or VAG. Warnings point at the usual causes of a wrong or silent sound: a voice starting in free memory or inside a
sample, a sample overwritten or running into the reverb work area. See spu.py.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import tkinter as tk
from tkinter import filedialog

import ttkbootstrap as tb

import spu
import vag
from pages.base import Page
from settings import config_dir

COLS, ROWS = 64, 32                 # Map cells: 256 bytes each
CELL = spu.RAM_SIZE // (COLS * ROWS)
CW, CH = 9, 8
WAVE_H = 110
DUMP_TYPES = [("SPU dump (ps1transfer -spu)", "*.bin"), ("All files", "*.*")]
RATES = ("Voice pitch", "11025", "22050", "32000", "44100")
DEFAULT_RATE = 22050
COLORS = {"capture": "#495057", "free": "#25292d", "reverb": "#6f42c1", "data": "#7a3b3b",
          "sample": ("#2f9e44", "#5c940d"), "idle": ("#1c5d8c", "#2a6f97"), "warning": "#e8590c",
          "selected": "#ffd43b"}
PRIORITY = {"free": 0, "capture": 1, "reverb": 2, "data": 3, "sample": 4}


class SpuPage(Page):
    title = "SPU RAM Viewer"

    def build(self):
        self.spu = None
        self.selected = None
        self.samples = []           # Decoded samples of the selected region
        self.player = spu.Player()
        self.dump = os.path.join(config_dir(), "spu_ram.bin")

        tb.Label(self, text=self.title, font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="The sound RAM of the running program (512KB): which samples sit where, the voices playing "
                            "them, free space, the reverb work area. Select a sample to see and play it on this "
                            "computer. Warnings point at wrong or silent sounds. SPU RAM keeps what earlier programs "
                            "left (it survives a reset): samples no voice uses may be leftovers.",
                 wraplength=930, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 8))
        bar = tb.Frame(self)
        bar.pack(fill="x", pady=(0, 6))
        tb.Button(bar, text="Read SPU RAM", bootstyle="primary", command=self.read).pack(side="left")
        tb.Button(bar, text="Open dump...", bootstyle="secondary-outline", command=self.open_dump).pack(
            side="left", padx=(6, 0))
        tb.Button(bar, text="Save dump...", bootstyle="secondary-outline", command=self.save_dump).pack(
            side="left", padx=6)
        self.summary = tb.Label(bar, text="Nothing read yet: start a program, then Read SPU RAM.",
                                bootstyle="secondary")
        self.summary.pack(side="left", padx=8)
        self.info = tb.Label(self, text="", bootstyle="secondary")
        self.info.pack(anchor="w", pady=(0, 6))

        panes = tb.Panedwindow(self, orient="horizontal")
        panes.pack(fill="both", expand=True)
        left = tb.Frame(panes)
        panes.add(left, weight=0)
        right = tb.Panedwindow(panes, orient="vertical")
        panes.add(right, weight=1)

        # Map, legend, the address under the mouse
        self.map = tk.Canvas(left, width=COLS * CW, height=ROWS * CH, highlightthickness=0, background="#1a1d20",
                             cursor="hand2")
        self.map.pack(anchor="w")
        self.map.bind("<Button-1>", self.map_click)
        self.map.bind("<Motion>", self.map_motion)
        legend = tb.Frame(left)
        legend.pack(anchor="w", pady=(4, 0))
        for name, color in (("Sample (a voice uses it)", COLORS["sample"][0]), ("Sample (no voice)", COLORS["idle"][0]),
                            ("Warning", COLORS["warning"]), ("Free", COLORS["free"]), ("Not ADPCM", COLORS["data"]),
                            ("Reverb", COLORS["reverb"]), ("Capture", COLORS["capture"])):
            tk.Canvas(legend, width=10, height=10, highlightthickness=0, background=color).pack(side="left")
            tb.Label(legend, text=name, bootstyle="secondary").pack(side="left", padx=(3, 9))
        self.hover = tb.Label(left, text="0x00000-0x7FFFF: 256 bytes per cell, 8KB per row", bootstyle="secondary",
                              font=("Consolas", 9))
        self.hover.pack(anchor="w", pady=(2, 8))

        # Selected sample: waveform, play, save
        self.sel_label = tb.Label(left, text="Select a sample (map or list).", font=("", 10, "bold"),
                                  wraplength=COLS * CW, justify="left")
        self.sel_label.pack(anchor="w")
        self.wave = tk.Canvas(left, width=COLS * CW, height=WAVE_H, highlightthickness=0, background="#1a1d20")
        self.wave.pack(anchor="w", pady=(4, 4))
        ctl = tb.Frame(left)
        ctl.pack(anchor="w")
        self.play_button = tb.Button(ctl, text="Play", bootstyle="success", width=8, command=self.play,
                                     state="disabled")
        self.play_button.pack(side="left")
        tb.Button(ctl, text="Stop", bootstyle="secondary-outline", width=6, command=self.player.stop).pack(
            side="left", padx=4)
        self.loop = tk.BooleanVar(value=True)
        tb.Checkbutton(ctl, text="Through its loop", variable=self.loop, bootstyle="round-toggle").pack(
            side="left", padx=(8, 8))
        tb.Label(ctl, text="Rate").pack(side="left")
        self.rate = tb.Combobox(ctl, values=RATES, width=11)
        self.rate.set(self.app.settings.field("spu.rate", RATES[0]))
        self.rate.pack(side="left", padx=(4, 8))
        self.save_wav_button = tb.Button(ctl, text="Save WAV...", bootstyle="secondary-outline",
                                         command=self.save_wav, state="disabled")
        self.save_wav_button.pack(side="left")
        self.save_vag_button = tb.Button(ctl, text="Save VAG...", bootstyle="secondary-outline",
                                         command=self.save_vag, state="disabled")
        self.save_vag_button.pack(side="left", padx=4)

        # Regions and voices
        top = tb.Frame(right)
        right.add(top, weight=3)
        cols = (("adrs", "Address", 62, "w"), ("size", "Size", 58, "e"), ("what", "What", 120, "w"),
                ("voices", "Voices", 60, "w"), ("loop", "Loop", 38, "w"), ("notes", "Notes", 320, "w"))
        self.tree = self._tree(top, cols)
        self.tree.tag_configure("warning", foreground="#fd7e14")
        self.tree.tag_configure("dim", foreground="#868e96")
        self.tree.bind("<<TreeviewSelect>>", self.tree_select)
        bottom = tb.Frame(right)
        right.add(bottom, weight=2)
        tb.Label(bottom, text="Voices", font=("", 10, "bold")).pack(anchor="w", pady=(6, 2))
        cols = (("n", "#", 30, "e"), ("state", "State", 70, "w"), ("start", "Start", 65, "w"),
                ("repeat", "Loop at", 65, "w"), ("rate", "Rate", 60, "e"), ("vol", "Volume L/R", 85, "w"),
                ("in", "In", 160, "w"))
        self.vtree = self._tree(bottom, cols)
        self.vtree.tag_configure("playing", foreground="#51cf66")
        self.vtree.tag_configure("dim", foreground="#868e96")
        self.vtree.bind("<<TreeviewSelect>>", self.voice_select)

        if os.path.isfile(self.dump):
            self.load(self.dump, quiet=True)
        else:
            self.draw_map()
            self.draw_wave()

    def _tree(self, parent, cols):
        frame = tb.Frame(parent)
        frame.pack(fill="both", expand=True)
        tree = tb.Treeview(frame, columns=[c[0] for c in cols], show="headings", selectmode="browse")
        for key, text, width, anchor in cols:
            tree.heading(key, text=text, anchor=anchor)
            tree.column(key, width=width, anchor=anchor, stretch=key in ("notes", "in", "what"))
        scroll = tb.Scrollbar(frame, orient="vertical", command=tree.yview)
        tree.configure(yscrollcommand=scroll.set)
        tree.pack(side="left", fill="both", expand=True)
        scroll.pack(side="left", fill="y")
        return tree

    # ---------------------------------------------------------------- reading

    def read(self):
        os.makedirs(os.path.dirname(self.dump), exist_ok=True)
        part = self.dump + ".new"
        if os.path.exists(part):
            os.remove(part)
        self.summary.configure(text="Reading the SPU RAM (about 3 seconds)...")

        def done(code, output):
            if code == 0 and os.path.isfile(part):
                os.replace(part, self.dump)
                self.load(self.dump)
            else:
                why = [line for line in output if "too old" in line or "No program" in line]
                self.summary.configure(text=why[-1] if why else "The SPU RAM couldn't be read (see the console).")
        self.app.run(["-spu", part], "SPU RAM read", on_done=done)

    def open_dump(self):
        path = filedialog.askopenfilename(filetypes=DUMP_TYPES, parent=self)
        if path:
            self.load(path)

    def save_dump(self):
        if not self.spu:
            return self.app.error("Read the SPU RAM first.")
        path = filedialog.asksaveasfilename(filetypes=DUMP_TYPES, defaultextension=".bin", parent=self)
        if path:
            with open(path, "wb") as f:
                f.write(self.spu.ram + self.spu.regs)
        return None

    def load(self, path, quiet=False):
        try:
            with open(path, "rb") as f:
                self.spu = spu.Spu(f.read())
        except (OSError, ValueError) as e:
            if not quiet:
                self.app.error("Can't read %s: %s" % (path, e))
            return
        self.selected = None
        self.fill()
        t = self.spu.totals()
        played = sum(1 for r in self.spu.regions if r.kind == "sample" and r.voices)
        self.summary.configure(text="%d samples (%d used by a voice), %d KB of samples, %d KB free (largest %d KB)%s" % (
            t["samples"], played, t["used"] // 1024, t["free"] // 1024, t["largest_free"] // 1024,
            ", %d warning%s" % (t["warnings"], "s" if t["warnings"] > 1 else "") if t["warnings"] else ""),
            bootstyle="warning" if t["warnings"] else "secondary")
        s = self.spu
        playing = sum(1 for v in s.voices if v.active)
        self.info.configure(text="SPU %s   reverb %s (work area from 0x%05X, %d KB)   IRQ %s at 0x%05X   %d voice%s "
                                 "playing   (%s)" % (
            "on" if s.cnt & 0x8000 else "OFF", "on" if s.reverb_on else "off", s.mbase,
            (spu.RAM_SIZE - s.mbase) // 1024 if s.mbase else 0, "on" if s.irq_on else "off", s.irq_adrs,
            playing, "" if playing == 1 else "s", os.path.basename(path)))

    # ---------------------------------------------------------------- lists

    def fill(self):
        self.tree.delete(*self.tree.get_children())
        for i, r in enumerate(self.spu.regions):
            loop = ("yes" if r.loops else "no") if r.kind == "sample" else ""
            notes = "; ".join(r.warnings)
            if not notes and r.kind == "sample" and r.voices and r.start == spu.CAPTURE_END and r.size == spu.BLOCK:
                notes = "Silent block (idle voices point here)"
            tag = "warning" if r.warnings else ("dim" if r.kind == "sample" and not r.voices else "")
            name = r.name() + (spu.REVERB_OFF if r.kind == "reverb" and not self.spu.reverb_on else "")
            self.tree.insert("", "end", iid=str(i), tags=(tag,), values=(
                "0x%05X" % r.start, self._size(r.size), name, self._voices(r.voices), loop, notes))
        self.vtree.delete(*self.vtree.get_children())
        for v in self.spu.voices:
            r = self.spu.region_at(v.start)
            state = "playing" if v.active else ("ended" if v.ended else "off")
            self.vtree.insert("", "end", iid="v%d" % v.n, tags=("playing" if v.active else "dim",), values=(
                v.n, state, "0x%05X" % v.start, "0x%05X" % v.repeat, v.rate, "%d / %d" % (v.vol_l, v.vol_r),
                r.name() if r else ""))
        self.draw_map()
        self.select(None)

    @staticmethod
    def _size(n):
        return "%d B" % n if n < 1024 else "%.1f KB" % (n / 1024)

    @staticmethod
    def _voices(voices):
        if len(voices) > 4:
            return "%d voices" % len(voices)
        return ", ".join(str(n) for n in voices)

    def tree_select(self, event=None):
        sel = self.tree.selection()
        if sel and self.spu:
            r = self.spu.regions[int(sel[0])]
            if r is not self.selected:
                self.select(r, from_tree=True)

    def voice_select(self, event=None):
        sel = self.vtree.selection()
        if sel and self.spu:
            r = self.spu.region_at(self.spu.voices[int(sel[0][1:])].start)
            if r:
                self.select(r)

    def select(self, region, from_tree=False):
        self.selected = region
        if region and not from_tree:
            iid = str(self.spu.regions.index(region))
            self.tree.selection_set(iid)
            self.tree.see(iid)
        sample = region is not None and region.kind == "sample"
        self.samples = self.spu.decode(region) if sample else []
        for b in (self.play_button, self.save_wav_button, self.save_vag_button):
            b.configure(state="normal" if sample else "disabled")
        if region is None:
            self.sel_label.configure(text="Select a sample (map or list).")
        else:
            rate = self.spu.rate_of(region) if sample else None
            text = "%s: 0x%05X-0x%05X, %s" % (region.name(), region.start, region.end - 1, self._size(region.size))
            if sample:
                text += ", %d samples%s%s" % (len(self.samples),
                                              ", %.2f s at %d Hz (voice pitch)" % (len(self.samples) / rate, rate)
                                              if rate else "", ", loops from 0x%05X" % region.loop_start
                                              if region.loops and region.loop_start is not None else
                                              (", loops" if region.loops else ""))
            if region.voices:
                text += "  - voices %s" % ", ".join(str(n) for n in region.voices)
            self.sel_label.configure(text=text)
        self.draw_map()
        self.draw_wave()

    # ---------------------------------------------------------------- map

    def _cell_color(self, r):
        if r is self.selected:
            return COLORS["selected"]
        if r.warnings:
            return COLORS["warning"]
        if r.kind == "sample":
            return COLORS["sample" if r.voices else "idle"][r.index & 1]
        return COLORS[r.kind]

    def draw_map(self):
        c = self.map
        c.delete("all")
        cells = [None] * (COLS * ROWS)
        if self.spu:
            for r in self.spu.regions:
                for i in range(r.start // CELL, (r.end - 1) // CELL + 1):
                    cur = cells[i]
                    if (cur is None or r is self.selected or
                            (cur is not self.selected and
                             (bool(r.warnings), PRIORITY[r.kind]) > (bool(cur.warnings), PRIORITY[cur.kind]))):
                        cells[i] = r
        for i, r in enumerate(cells):
            x, y = (i % COLS) * CW, (i // COLS) * CH
            c.create_rectangle(x, y, x + CW - 1, y + CH - 1, outline="",
                               fill=self._cell_color(r) if r else COLORS["free"])

    def _map_adrs(self, event):
        col, row = event.x // CW, event.y // CH
        if 0 <= col < COLS and 0 <= row < ROWS:
            return (row * COLS + col) * CELL
        return None

    def map_motion(self, event):
        adrs = self._map_adrs(event)
        if adrs is None or not self.spu:
            return
        r = self.spu.region_at(adrs)
        self.hover.configure(text="0x%05X: %s%s" % (adrs, r.name() if r else "",
                                                   " (voices %s)" % self._voices(r.voices) if r and r.voices else ""))

    def map_click(self, event):
        adrs = self._map_adrs(event)
        if adrs is not None and self.spu:
            # The sample covering most of the cell (small ones share a cell)
            cell = [r for r in self.spu.regions if r.start < adrs + CELL and r.end > adrs]
            samples = [r for r in cell if r.kind == "sample"]
            best = max(samples or cell, key=lambda r: min(r.end, adrs + CELL) - max(r.start, adrs), default=None)
            if best:
                self.select(best)

    # ---------------------------------------------------------------- waveform, playing

    def draw_wave(self):
        c = self.wave
        c.delete("all")
        w = COLS * CW
        mid = WAVE_H // 2
        c.create_line(0, mid, w, mid, fill="#343a40")
        s = self.samples
        if not s:
            c.create_text(w // 2, mid, text="No sample selected", fill="#6c757d")
            return
        per = max(1, len(s) / w)
        for x in range(min(w, len(s))):
            part = s[int(x * per):int((x + 1) * per) or 1]
            lo, hi = min(part), max(part)
            c.create_line(x, mid - hi * (mid - 2) // 32768, x, mid - lo * (mid - 2) // 32768 + 1, fill="#51cf66")
        r = self.selected
        if r.loops and r.loop_start is not None:
            x = (r.loop_start - r.start) // spu.BLOCK * 28 / per
            c.create_line(x, 0, x, WAVE_H, fill="#ffd43b", dash=(3, 3))
            c.create_text(x + 3, 3, text="loop", fill="#ffd43b", anchor="nw", font=("", 8))

    def play_rate(self):
        choice = self.rate.get()
        self.app.settings.set_field("spu.rate", choice)
        if choice == RATES[0]:
            return self.spu.rate_of(self.selected) or DEFAULT_RATE
        try:
            return max(1000, min(96000, int(choice)))
        except ValueError:
            return DEFAULT_RATE

    def _render(self):
        r = self.selected
        if self.loop.get() and r.loops:
            # The loop played again until about 2 seconds (at most 20 times): a click at the loop point is heard
            rate = self.play_rate()
            loop_len = len(self.samples) - ((r.loop_start - r.start) // spu.BLOCK * 28 if r.loop_start else 0)
            times = min(20, max(1, (2 * rate - len(self.samples)) // max(1, loop_len)))
            return self.spu.decode(r, loops=times)
        return self.samples

    def play(self):
        if not self.samples:
            return
        if not self.player.play(self._render(), self.play_rate()):
            self.app.error("No sound player found (aplay, paplay or afplay).")

    def save_wav(self):
        if not self.samples:
            return
        path = filedialog.asksaveasfilename(filetypes=[("WAV", "*.wav")], defaultextension=".wav", parent=self,
                                            initialfile="sample_%05X.wav" % self.selected.start)
        if path:
            spu.write_wav(path, self.samples, self.play_rate())

    def save_vag(self):
        if not self.samples:
            return
        r = self.selected
        path = filedialog.asksaveasfilename(filetypes=[("VAG", "*.vag")], defaultextension=".vag", parent=self,
                                            initialfile="sample_%05X.vag" % r.start)
        if path:
            with open(path, "wb") as f:
                f.write(vag.vag_bytes(self.spu.ram[r.start:r.end], self.play_rate(), "SPU_%05X" % r.start))
