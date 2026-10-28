"""
PS1 DEV CART GUI: VAB editor, a tab of the Sound Tools page: the bank the console's sound tester plays (the VH/VAB of
the sound files) tuned live, like Sony's SoundDelicatessen: bank, program and tone volume and pan, key ranges, center
note and fine tune, pitch bend range, the ADSR envelope (drawn). Each change is written in the VH in the console's RAM
("vabwrite" of ps1transfer -sndlive, see sndlive.c): the next notes use it, the notes sounding change at once. The
edited bank is saved as a VAB (VH + VB) or a VH.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import tkinter as tk
from tkinter import filedialog

import ttkbootstrap as tb

import vab
from widgets import ScrollFrame

SEND_MS = 40                # Changes sent together (sliders send many)
CHUNK = 4096                # Bytes per vabwrite when the whole VH is sent
KEY_W, ROW_H = 3, 13        # Key range strip: a pixel column of 4 per note, a row per tone
ENV_W, ENV_H = 250, 76
ENV_MAX = 4                 # Seconds of an envelope phase drawn at most
NOTE_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
TONE_COLORS = ("#1c7ed6", "#2f9e44", "#e8590c", "#ae3ec9", "#1098ad", "#f08c00", "#d6336c", "#5c940d")
VAB_TYPES = [("VAB bank (VH + VB)", "*.vab"), ("VH header only", "*.vh")]


def note_name(n):
    return "%s%d" % (NOTE_NAMES[n % 12], n // 12 - 1)


def pan_text(p):
    return "center" if p == 64 else ("L %d" % (64 - p) if p < 64 else "R %d" % (p - 64))


class VabEditor(tb.Frame):
    def __init__(self, parent, page):
        super().__init__(parent)
        self.page = page            # Sound Tools page: the live link, the note played
        self.app = page.app
        self.vab = None
        self.path = None            # File of the VH (.vab or .vh), its .vb
        self.vb_path = None
        self.prog = None            # Program number selected
        self.tone_n = None          # Tone selected (in the program)
        self.dirty = set()          # (offset, size) changed, not sent yet
        self.send_job = None
        self.edited = False         # Changed since loaded or saved
        self.on_console = False     # The console plays this bank (SNDVAB of the tester)
        self.updating = False       # Widgets set from the bank (not a change)
        self.vars = {}
        self._build()

    # ---------------------------------------------------------------- widgets

    def _build(self):
        scroll = ScrollFrame(self)          # (taller than the page at small window sizes)
        scroll.pack(fill="both", expand=True)
        top = tb.Frame(scroll.inner, padding=6)
        top.pack(fill="both", expand=True)
        bar = tb.Frame(top)
        bar.pack(fill="x", pady=(0, 6))
        self.title = tb.Label(bar, text="No VAB: add a bank (VH + VB, or VAB) to the sound files.",
                              font=("", 10, "bold"))
        self.title.pack(side="left")
        tb.Button(bar, text="Save VAB...", bootstyle="success-outline", command=self.save).pack(side="right")
        tb.Button(bar, text="Reload from the file", bootstyle="secondary-outline", command=self.reload).pack(
            side="right", padx=6)
        bar = tb.Frame(top)
        bar.pack(fill="x", pady=(0, 6))
        self.status = tb.Label(bar, text="", bootstyle="secondary")
        self.status.pack(side="left")
        # Listen: the program selected is the tester's, the note played as with the Sound Tools keyboard
        self.play_button = tb.Button(bar, text="Play (hold)", bootstyle="primary", width=11)
        self.play_button.pack(side="right")
        self.play_button.bind("<ButtonPress-1>", lambda e: self.page.press(self.note.get()))
        self.play_button.bind("<ButtonRelease-1>", lambda e: self.page.release())
        self.note_label = tb.Label(bar, text="C4", width=5)
        self.note_label.pack(side="right", padx=(0, 6))
        self.note = tk.IntVar(value=60)
        self.note_spin = tb.Spinbox(bar, from_=0, to=127, width=5, textvariable=self.note,
                                    command=self._note_changed)
        self.note_spin.pack(side="right", padx=4)
        tb.Label(bar, text="Note").pack(side="right")

        body = tb.Frame(top)
        body.pack(fill="both", expand=True)
        col1 = tb.Frame(body)
        col1.pack(side="left", fill="y", anchor="n")
        col2 = tb.Frame(body)
        col2.pack(side="left", fill="y", anchor="n", padx=8)
        col3 = tb.Frame(body)
        col3.pack(side="left", fill="y", anchor="n")

        # Bank and programs
        box = self.page.section("Bank", col1)
        box.pack(fill="x", pady=(0, 6))
        self._slider(box, "Volume", "bank.mvol", 0, 127, lambda v: self.set_header("mvol", v))
        box = self.page.section("Programs", col1)
        box.pack(fill="both", expand=True)
        self.prog_tree = self._tree(box, (("p", "Program", 64), ("tones", "Tones", 46), ("vol", "Vol", 36),
                                          ("pan", "Pan", 52)), 10, expand=True)     # (the column's height)
        self.prog_tree.bind("<<TreeviewSelect>>", lambda e: self.select_prog())
        self._slider(box, "Volume", "prog.mvol", 0, 127, lambda v: self.set_prog("mvol", v))
        self._slider(box, "Pan", "prog.mpan", 0, 127, lambda v: self.set_prog("mpan", v), pan_text)
        self._slider(box, "Priority", "prog.prior", 0, 15, lambda v: self.set_prog("prior", v))

        # Tones of the program: key ranges, list
        box = self.page.section("Tones and key ranges  (click a range to edit its tone)", col2)
        box.pack(fill="x")
        self.strip = tk.Canvas(box, width=128 * KEY_W + 1, height=ROW_H * 4 + 16, highlightthickness=0,
                               background="#1a1d20", cursor="hand2")
        self.strip.pack(anchor="w")
        self.strip.bind("<Button-1>", self.strip_click)
        self.tone_tree = self._tree(box, (("t", "Tone", 42), ("vag", "VAG", 36), ("keys", "Keys", 78),
                                          ("center", "Center", 50), ("fine", "Fine", 44), ("vol", "Vol", 34),
                                          ("pan", "Pan", 52)), 3)
        self.tone_tree.bind("<<TreeviewSelect>>", lambda e: self.select_tone())

        # Tone editor
        box = self.page.section("Tone", col3)
        box.pack(fill="x")
        self.tone_title = tb.Label(box, text="Select a tone", font=("", 10, "bold"))
        self.tone_title.pack(anchor="w", pady=(0, 4))
        self._slider(box, "Volume", "tone.vol", 0, 127, lambda v: self.set_tone("vol", v))
        self._slider(box, "Pan", "tone.pan", 0, 127, lambda v: self.set_tone("pan", v), pan_text)
        self._slider(box, "Center note", "tone.center", 0, 127, lambda v: self.set_tone("center", v),
                     lambda v: "%s (%d)" % (note_name(v), v))
        self._slider(box, "Fine tune", "tone.shift", 0, 127, lambda v: self.set_tone("shift", v),
                     lambda v: "+%d c" % (v * 100 // 128))
        self._slider(box, "Lowest key", "tone.min", 0, 127, lambda v: self.set_tone("min", v),
                     lambda v: "%s (%d)" % (note_name(v), v))
        self._slider(box, "Highest key", "tone.max", 0, 127, lambda v: self.set_tone("max", v),
                     lambda v: "%s (%d)" % (note_name(v), v))
        self._slider(box, "Bend range", "tone.pbmax", 0, 24, lambda v: self.set_tone("pbmax", v),
                     lambda v: "%d st" % v)
        self._slider(box, "VAG", "tone.vag", 1, 255, lambda v: self.set_tone("vag", v))

        box = self.page.section("Envelope (ADSR) of the tone", col2)
        box.pack(fill="x", pady=(6, 0))
        row = tb.Frame(box)
        row.pack(fill="x")
        self.env = tk.Canvas(row, width=ENV_W, height=ENV_H, highlightthickness=0, background="#1a1d20")
        self.env.pack(side="left")
        self.env_times = tb.Label(row, text="", bootstyle="secondary", font=("Consolas", 9), justify="left")
        self.env_times.pack(side="left", padx=10, anchor="n")
        self._slider(box, "Attack", "adsr.attack", 0, 127, lambda v: self.set_adsr(), self._rate_text, exp="attack_exp")
        self._slider(box, "Decay", "adsr.decay", 0, 15, lambda v: self.set_adsr(), self._rate_text)
        self._slider(box, "Sustain level", "adsr.sustain_level", 0, 15, lambda v: self.set_adsr(),
                     lambda v: "%d%%" % ((v + 1) * 100 // 16))
        self._slider(box, "Sustain", "adsr.sustain", 0, 127, lambda v: self.set_adsr(), self._rate_text,
                     exp="sustain_exp", direction=True)
        self._slider(box, "Release", "adsr.release", 0, 31, lambda v: self.set_adsr(), self._rate_text,
                     exp="release_exp")
        self.enable(False)

    @staticmethod
    def _rate_text(v):
        return "%d (fast)" % v if v == 0 else str(v)

    def _tree(self, parent, cols, height, expand=False):
        """A list with its scrollbar (expand: it takes the height left in its section)."""
        frame = tb.Frame(parent)
        frame.pack(fill="both" if expand else "x", expand=expand, pady=(4, 4))
        tree = tb.Treeview(frame, columns=[c[0] for c in cols], show="headings", height=height,
                           selectmode="browse")
        for key, text, width in cols:
            tree.heading(key, text=text, anchor="w")
            tree.column(key, width=width, anchor="w", stretch=False)
        scroll = tb.Scrollbar(frame, orient="vertical", command=tree.yview)
        tree.configure(yscrollcommand=scroll.set)
        tree.pack(side="left", fill="both", expand=True)
        scroll.pack(side="left", fill="y")
        return tree

    def _slider(self, parent, label, key, lo, hi, on_change, text=str, exp=None, direction=False):
        row = tb.Frame(parent)
        row.pack(fill="x")
        tb.Label(row, text=label, width=12).pack(side="left")
        var = tk.IntVar(value=lo)
        value = tb.Label(row, text=text(lo), width=8, anchor="w")

        def moved(_=None):
            v = int(float(var.get()))
            value.configure(text=text(v))
            if not self.updating:
                on_change(v)
        scale = tb.Scale(row, from_=lo, to=hi, variable=var, length=105, command=moved)
        scale.pack(side="left")
        value.pack(side="left", padx=(6, 0))
        self.vars[key] = (var, value, text, scale)
        if exp:
            ev = tk.BooleanVar(value=False)
            cb = tb.Checkbutton(row, text="Exp", variable=ev, bootstyle="round-toggle",
                                command=lambda: None if self.updating else self.set_adsr())
            cb.pack(side="left")
            self.vars["adsr." + exp] = (ev, None, None, cb)
        if direction:
            dv = tk.BooleanVar(value=True)
            cb = tb.Checkbutton(row, text="Down", variable=dv, bootstyle="round-toggle",
                                command=lambda: None if self.updating else self.set_adsr())
            cb.pack(side="left", padx=(6, 0))
            self.vars["adsr.sustain_dec"] = (dv, None, None, cb)

    def _show(self, key, v):
        var, label, text, _ = self.vars[key]
        var.set(v)
        if label is not None:
            label.configure(text=text(int(v)))

    def enable(self, on):
        for key, (_, _, _, widget) in self.vars.items():
            group = key.split(".")[0]
            ok = on and (group == "bank" or (group == "prog" and self.prog is not None) or
                         (group in ("tone", "adsr") and self.tone_n is not None))
            widget.configure(state="normal" if ok else "disabled")

    # ---------------------------------------------------------------- bank

    def bank_files(self):
        """The VH (.vab or .vh) of the sound files and its .vb (None for a .vab)."""
        files = self.page.files.files()
        vh = next((f for f in files if f.lower().endswith((".vab", ".vh"))), None)
        vb = next((f for f in files if f.lower().endswith(".vb")), None)
        return vh, (vb if vh and vh.lower().endswith(".vh") else None)

    def load(self, force=False):
        """The bank of the sound files (kept if already loaded, unless force)."""
        vh, vb = self.bank_files()
        if not vh:
            self.vab = None
            self.title.configure(text="No VAB: add a bank (VH + VB, or VAB) to the sound files.")
            self.fill()
            return
        if self.vab is not None and not force and os.path.normcase(vh) == os.path.normcase(self.path or ""):
            return
        try:
            self.vab = vab.Vab.load(vh, vb)
        except (OSError, ValueError) as e:
            self.vab = None
            self.title.configure(text="%s: %s" % (os.path.basename(vh), e))
            self.fill()
            return
        self.path, self.vb_path = vh, vb
        self.edited = False
        self.prog = self.tone_n = None
        self._title()
        self.fill()
        progs = self.vab.programs()
        if progs:
            self.prog_tree.selection_set(str(progs[0]))

    def reload(self):
        self.load(force=True)
        if self.on_console and self.vab:
            self.send_all()

    def _title(self):
        v = self.vab
        self.title.configure(text="%s%s: %d programs, %d tones, %d VAGs" % (
            os.path.basename(self.path), " (edited)" if self.edited else "", len(v.programs()), v.ts, v.vs))

    def console(self, adrs, size):
        """The tester's VH (SNDVAB): edits go live if it is this bank (the same size), all of it sent if it was
        edited before."""
        self.load()
        self.on_console = bool(adrs) and self.vab is not None and size == self.vab.vh_size()
        if self.on_console and self.edited:
            self.send_all()
        self._status()

    def stopped(self):
        self.on_console = False
        self._status()

    def _status(self, text=None):
        if text:
            self.status.configure(text=text)
        elif self.on_console:
            self.status.configure(text="Live: hold a note while moving a slider, the console plays the change at once "
                                       "(key ranges and VAGs: from the next note).", bootstyle="success")
        else:
            self.status.configure(text="Not on the console: the changes are kept here (Play on the PS1 sends them).",
                                  bootstyle="secondary")

    def save(self):
        if not self.vab:
            return self.app.error("No VAB to save: add a bank to the sound files.")
        stem = os.path.splitext(os.path.basename(self.path))[0]
        path = filedialog.asksaveasfilename(filetypes=VAB_TYPES, defaultextension=".vab", parent=self,
                                            initialfile=stem + "_edited.vab")
        if not path:
            return None
        with_vb = not path.lower().endswith(".vh")
        if with_vb and not self.vab.vb:
            return self.app.error("The VB isn't known (add the .vb to the sound files), or save a .vh.")
        with open(path, "wb") as f:
            f.write(self.vab.bytes(with_vb))
        self.edited = False
        self._title()
        self._status("Saved %s." % os.path.basename(path))
        return None

    # ---------------------------------------------------------------- lists

    def fill(self):
        self.prog_tree.delete(*self.prog_tree.get_children())
        if self.vab:
            self._show("bank.mvol", self.vab.header()["mvol"])
            for p in self.vab.programs():
                a = self.vab.prog(p)
                self.prog_tree.insert("", "end", iid=str(p), values=(p, a["tones"], a["mvol"], pan_text(a["mpan"])))
        self.fill_tones()
        self.enable(self.vab is not None)

    def fill_tones(self):
        self.tone_tree.delete(*self.tone_tree.get_children())
        if self.vab and self.prog is not None:
            for t, a in enumerate(self.vab.tones(self.prog)):
                self.tone_tree.insert("", "end", iid=str(t), values=(
                    t, a["vag"], "%s-%s" % (note_name(a["min"]), note_name(a["max"])), note_name(a["center"]),
                    "+%dc" % (a["shift"] * 100 // 128), a["vol"], pan_text(a["pan"])))
        self.draw_strip()

    def select_prog(self):
        sel = self.prog_tree.selection()
        if not sel or not self.vab:
            return
        self.prog = int(sel[0])
        self.updating = True
        try:
            a = self.vab.prog(self.prog)
            for k in ("mvol", "mpan", "prior"):
                self._show("prog." + k, a[k])
        finally:
            self.updating = False
        self.tone_n = None
        self.fill_tones()
        if self.vab.tones(self.prog):
            self.tone_tree.selection_set("0")
        # The tester plays this program (its index: programs with tones, in order)
        self.page.set_program(self.vab.programs().index(self.prog))
        self.enable(True)

    def select_tone(self):
        sel = self.tone_tree.selection()
        if not sel or self.prog is None:
            return
        self.tone_n = int(sel[0])
        a = self.vab.tone(self.prog, self.tone_n)
        self.updating = True
        try:
            for k in ("vol", "pan", "center", "shift", "min", "max", "pbmax", "vag"):
                self._show("tone." + k, a[k])
            for k, v in vab.adsr_fields(a["adsr1"], a["adsr2"]).items():
                self._show("adsr." + k, v)
        finally:
            self.updating = False
        self.tone_title.configure(text="Program %d, tone %d" % (self.prog, self.tone_n))
        # A note of the tone's range to listen to it
        if not a["min"] <= self.note.get() <= a["max"]:
            self.note.set(max(a["min"], min(a["max"], a["center"])))
            self._note_changed()
        self.enable(True)
        self.draw_strip()
        self.draw_env()

    def _note_changed(self):
        try:
            n = max(0, min(127, int(self.note.get())))
        except (tk.TclError, ValueError):
            return
        self.note_label.configure(text=note_name(n))
        self.draw_strip()

    # ---------------------------------------------------------------- drawings

    def draw_strip(self):
        c = self.strip
        c.delete("all")
        for n in range(128):                    # Keyboard: black keys darker, C marked
            x = n * KEY_W
            c.create_rectangle(x, 0, x + KEY_W, 10, outline="", fill="#202326" if n % 12 in (1, 3, 6, 8, 10)
                               else "#495057")
            if n % 12 == 0:
                c.create_line(x, 0, x, 10, fill="#adb5bd")
        if not self.vab or self.prog is None:
            return
        tones = self.vab.tones(self.prog)
        c.configure(height=max(4, len(tones)) * ROW_H + 16)
        for t, a in enumerate(tones):
            y = 14 + t * ROW_H
            color = TONE_COLORS[t % len(TONE_COLORS)]
            c.create_rectangle(a["min"] * KEY_W, y, (a["max"] + 1) * KEY_W, y + ROW_H - 3, fill=color,
                               outline="#ffd43b" if t == self.tone_n else "", width=2)
            cx = a["center"] * KEY_W + KEY_W // 2
            c.create_line(cx, y, cx, y + ROW_H - 3, fill="#ffffff")
        try:
            x = int(self.note.get()) * KEY_W + KEY_W // 2
            c.create_polygon(x - 4, 0, x + 4, 0, x, 8, fill="#ffd43b")
        except (tk.TclError, ValueError):
            pass

    def strip_click(self, event):
        if not self.vab or self.prog is None:
            return
        t = (event.y - 14) // ROW_H
        if 0 <= t < len(self.vab.tones(self.prog)):
            self.tone_tree.selection_set(str(t))
        note = event.x // KEY_W
        if 0 <= note <= 127 and t >= 0:
            self.note.set(note)
            self._note_changed()

    def draw_env(self):
        c = self.env
        c.delete("all")
        if self.tone_n is None:
            return
        a = self.vab.tone(self.prog, self.tone_n)
        points, (attack, decay, release) = vab.envelope(a["adsr1"], a["adsr2"], max_seconds=ENV_MAX)
        total = max(p[0] for p in points) or 1
        coords = []
        for t, level in points:
            coords += [t / total * (ENV_W - 4) + 2, ENV_H - 4 - level * (ENV_H - 8)]
        if len(coords) >= 4:
            c.create_line(*coords, fill="#51cf66", width=2)
        fmt = lambda s: "%.2f s" % s if s is not None else "> %d s" % ENV_MAX
        self.env_times.configure(text="Attack   %s\nDecay    %s\nRelease  %s\n(%.1f s shown, held then released)" % (
            fmt(attack), fmt(decay), fmt(release), total))

    # ---------------------------------------------------------------- changes

    def _changed(self, region):
        self.edited = True
        self._title()
        self.dirty.add(region)
        if self.send_job is None:
            self.send_job = self.after(SEND_MS, self.send_dirty)

    def set_header(self, name, v):
        if self.vab:
            self._changed(self.vab.set_header(name, v))

    def set_prog(self, name, v):
        if self.vab and self.prog is not None:
            self._changed(self.vab.set_prog(self.prog, name, v))
            a = self.vab.prog(self.prog)
            self.prog_tree.item(str(self.prog), values=(self.prog, a["tones"], a["mvol"], pan_text(a["mpan"])))

    def set_tone(self, name, v):
        if self.vab and self.tone_n is not None:
            a = self.vab.tone(self.prog, self.tone_n)
            if name == "min" and v > a["max"]:      # (a range stays a range)
                self._show("tone.max", v)
                self.vab.set_tone(self.prog, self.tone_n, "max", v)
            if name == "max" and v < a["min"]:
                self._show("tone.min", v)
                self.vab.set_tone(self.prog, self.tone_n, "min", v)
            if name == "vag":
                v = max(1, min(self.vab.vs, v))
            self._changed(self.vab.set_tone(self.prog, self.tone_n, name, v))
            a = self.vab.tone(self.prog, self.tone_n)
            self.tone_tree.item(str(self.tone_n), values=(
                self.tone_n, a["vag"], "%s-%s" % (note_name(a["min"]), note_name(a["max"])), note_name(a["center"]),
                "+%dc" % (a["shift"] * 100 // 128), a["vol"], pan_text(a["pan"])))
            self.draw_strip()

    def set_adsr(self):
        if not self.vab or self.tone_n is None:
            return
        f = {k: int(self.vars["adsr." + k][0].get()) for k in
             ("attack_exp", "attack", "decay", "sustain_level", "sustain_exp", "sustain_dec", "sustain",
              "release_exp", "release")}
        adsr1, adsr2 = vab.adsr_regs(f)
        self.vab.set_tone(self.prog, self.tone_n, "adsr1", adsr1)
        self._changed(self.vab.set_tone(self.prog, self.tone_n, "adsr2", adsr2))
        self.draw_env()

    def send_dirty(self):
        self.send_job = None
        if not self.on_console:
            self.dirty.clear()
            return
        for offset, size in sorted(self.dirty):
            self.page.send("vabwrite %d %s" % (offset, bytes(self.vab.data[offset:offset + size]).hex().upper()))
        self.dirty.clear()

    def send_all(self):
        data = self.vab.data
        for offset in range(0, len(data), CHUNK):
            self.page.send("vabwrite %d %s" % (offset, bytes(data[offset:offset + CHUNK]).hex().upper()))
