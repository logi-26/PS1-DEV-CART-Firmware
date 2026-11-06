"""
PS1 DEV CART GUI: Sound page (VH/VB/VAB banks, SEQ music and VAG samples played by the sound tester).

"Play on the PS1" starts the console's sound tester with the files (ps1transfer -snd ... -sndlive, see
ps1transfer/sndlive.c). Its controls are then here too, linked live with the console: the program, note, volume and
SEQ chosen here are used at once by the console, and the ones changed with the controller show here. Notes are played
with the Play button or the keyboard (held: as Cross held on the controller), the voices sounding light up. So sounds
can be tested without a controller.
The VAB Editor tab tunes the bank live (pages/vabedit.py): each change is written in the bank the tester plays.
The Convert tab makes audio files and MIDI into VAG, VAB, SEQ and XA (pages/audioconvert.py, audioconv.py).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import time
import tkinter as tk

import ttkbootstrap as tb

from pages.base import Page
from pages.audioconvert import ConvertTab
from pages.vabedit import VabEditor
from widgets import FileList, ScrollFrame

SOUND_TYPES = [("Sound files", "*.vh *.vb *.vab *.seq *.sep *.vag"), ("All files", "*.*")]
NOTE_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
PROTOCOL = ("SNDLIVE", "SNDINFO", "SNDPROGS", "SNDSTATE", "SNDVAB", "SNDWROTE", "SNDVH", "SNDSPU", "SNDFX",
            "SNDCHAN")                                              # Answers of
                                                                    # ps1transfer -sndlive (not in the log)
VOICES = 24
QUIET = 0.6             # s: a control changed here is not set back by the console's state meanwhile
VOLUME_SEND_MS = 40     # Volume changes sent together (the slider sends many)
OCTAVES = 2             # Keyboard: 2 octaves (from the C of the note's octave) and the next C
WHITE_W, WHITE_H, BLACK_W, BLACK_H = 30, 104, 18, 64
WHITE_OF = (0, 2, 4, 5, 7, 9, 11)   # Semitones of the white keys in an octave
BLACK_OF = ((1, 0), (3, 1), (6, 3), (8, 4), (10, 5))   # Black key semitone, white key on its left
KEY_COLORS = {"white": "#f4f4f4", "black": "#202020", "note": "#4fc1ff", "held": "#2b9348", "outline": "#606060"}
LIGHT_ON, LIGHT_OFF = "#3fb950", "#2c3a2f"
SEQ_CHANNELS = 16
# SPU reverb types (soundtest.c reverb_types: libspu's modes), work area at the top of SPU RAM (bytes)
REVERBS = [("Off", 0), ("Room", 0x26C0), ("Studio small", 0x1F40), ("Studio medium", 0x4840),
           ("Studio large", 0x6FE0), ("Hall", 0xADE0), ("Half echo", 0x3C00), ("Space echo", 0xF6C0),
           ("Chaos echo", 0x18040), ("Delay", 0x18040)]
REVERB_VOICES = ["Notes of tones with reverb", "All notes", "No notes"]   # VAB tone mode 4: reverb (as libsnd)
FX_DEFAULTS = dict(reverb=0, depth=64, voices=0, transpose=0, tempo=100, mono=False)
SPU_RAM = 0x80000


def note_name(n):
    return "%s%d" % (NOTE_NAMES[n % 12], n // 12 - 1)


class SoundPage(Page):
    title = "Sound Tools"

    def build(self):
        self.live = False           # The console's tester answers (SNDLIVE ready)
        self.disconnected = False
        self.kind = 0               # 0: no sound data, 1: VAB, 2: VAG
        self.programs = []          # Program numbers (index order)
        self.state = {}             # Last SNDSTATE
        self.changed = {}           # Control -> time changed here (the console's value not taken meanwhile)
        self.held = None            # Note played from here (mouse held on a key or Play)
        self.volume_job = None
        self.updating = False       # Widgets set by the program (not a change)

        tb.Label(self, text="Sound tester", font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="Plays sound banks (VH + VB, or VAB), SEQ music with its bank, and VAG samples on the "
                            "PS1. Once it runs, the controls below and the console's controller act on the same sound "
                            "tester: each shows what the other one does, so sounds can be tested without a controller.",
                 wraplength=1000, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 8))

        top = self.section("Sound files")
        top.pack(fill="x", pady=(0, 8))
        self.files = FileList(top, self.app, "sound.files", "Sound files", filetypes=SOUND_TYPES, max_files=8,
                              height=3, compact=True)
        self.files.pack(fill="x")
        bar = tb.Frame(top)
        bar.pack(fill="x", pady=(6, 0))
        tb.Button(bar, text="Play on the PS1", bootstyle="success", command=self.play).pack(side="left")
        tb.Button(bar, text="Stop", bootstyle="danger-outline", width=8, command=self.app.stop_task).pack(
            side="left", padx=6)
        self.status = tb.Label(bar, text="Not running.", bootstyle="secondary")
        self.status.pack(side="left", padx=12)

        self.tabs = tb.Notebook(self)
        self.tabs.pack(fill="both", expand=True)
        scroll = ScrollFrame(self.tabs)     # (taller than the page: the SEQ channels and the effects)
        self.tabs.add(scroll, text="Tester")
        tester = tb.Frame(scroll.inner, padding=6)
        tester.pack(fill="both", expand=True)
        self.editor = VabEditor(self.tabs, self)
        self.tabs.add(self.editor, text="VAB Editor")
        self.convert = ConvertTab(self.tabs, self)
        self.tabs.add(self.convert, text="Convert")
        self.tabs.bind("<<NotebookTabChanged>>", lambda e: self.editor.load()
                       if self.tabs.select() == str(self.editor) else None)
        body = tb.Frame(tester)
        body.pack(fill="both", expand=True)
        left = tb.Frame(body)
        left.pack(side="left", fill="both", expand=True, padx=(0, 8))
        right = tb.Frame(body)
        right.pack(side="left", fill="y", anchor="n")
        self.controls = []          # Enabled while the tester is linked

        # SEQ music: start/stop, its 16 MIDI channels (each with its own program, set by the music), muted or solo
        box = self.section("SEQ music", left)
        box.pack(fill="x", pady=(0, 8))
        row = tb.Frame(box)
        row.pack(fill="x")
        self.seq_button = tb.Button(row, text="Start", bootstyle="success-outline", width=10, command=self.toggle_seq)
        self.seq_button.pack(side="left")
        self.controls.append(self.seq_button)
        self.seq_label = tb.Label(row, text="", bootstyle="secondary")
        self.seq_label.pack(side="left", padx=10)
        b = tb.Button(row, text="Unmute all", bootstyle="secondary-outline", command=self.unmute_all)
        b.pack(side="right")
        self.controls.append(b)
        # At the end of the music: played again, or once (as a game plays a jingle: its code decides; the loops
        # inside the music are played either way)
        self.repeat = tk.BooleanVar(value=self.app.settings.get("sound.repeat", True) is not False)
        tb.Checkbutton(box, text="Repeat at the end (off: a SEQ without loop markers plays once and stops)",
                       variable=self.repeat, command=self.repeat_changed).pack(anchor="w", pady=(4, 0))
        self.sep_row = tb.Frame(box)         # (a SEP: several SEQs in the file, one played)
        tb.Label(self.sep_row, text="SEP: play SEQ").pack(side="left")
        self.sep_choice = tb.Combobox(self.sep_row, state="readonly", width=12)
        self.sep_choice.pack(side="left", padx=6)
        self.sep_choice.bind("<<ComboboxSelected>>", lambda e: self.send("sep %d" % self.sep_choice.current()))
        tb.Label(self.sep_row, text="(Select on the controller: the next one)", bootstyle="secondary").pack(
            side="left")
        self.sep_count = 0
        self.info = tb.Label(box, text="", bootstyle="secondary")
        self.info.pack(anchor="w", pady=(6, 0))
        tb.Label(box, text="Channels: the music plays each channel with the program its events choose. M mutes a "
                           "channel, S plays only the solo channels.",
                 bootstyle="secondary", wraplength=720, justify="left").pack(anchor="w", pady=(4, 4))
        grid = tb.Frame(box)
        grid.pack(anchor="w")
        self.mute_vars, self.solo_vars, self.ch_cells = [], [], []
        for ch in range(SEQ_CHANNELS):
            cell = tb.Frame(grid, padding=(4, 2))
            cell.grid(row=ch // 8, column=ch % 8, padx=2, pady=2, sticky="w")
            light = tk.Canvas(cell, width=10, height=10, highlightthickness=0, background="#202020")
            light.grid(row=0, column=0, padx=(0, 4))
            name = tb.Label(cell, text="Ch %d" % (ch + 1), width=5)
            name.grid(row=0, column=1, columnspan=2, sticky="w")
            prog = tb.Label(cell, text="", bootstyle="secondary", font=("", 8))
            prog.grid(row=1, column=0, columnspan=3, sticky="w")
            mute, solo = tk.BooleanVar(), tk.BooleanVar()
            mb = tb.Checkbutton(cell, text="M", variable=mute, bootstyle="danger-outline-toolbutton", width=2,
                                command=self.mutes_changed)
            mb.grid(row=2, column=1, padx=(0, 2), pady=(2, 0))
            sb = tb.Checkbutton(cell, text="S", variable=solo, bootstyle="warning-outline-toolbutton", width=2,
                                command=self.mutes_changed)
            sb.grid(row=2, column=2, pady=(2, 0))
            self.mute_vars.append(mute)
            self.solo_vars.append(solo)
            self.ch_cells.append(dict(light=light, name=name, prog=prog, buttons=(mb, sb)))
        self.ch_used = 0            # SEQ channels with notes (SNDCHAN), the others greyed

        # Instrument and note: name, buttons (as the controller: Left/Right, Up/Down, L1/R1), keyboard
        box = self.section("Instrument and note  (click and hold a key, or Play, to play it: as Cross held on the "
                           "controller)", left)
        box.pack(fill="x", pady=(0, 8))
        row = tb.Frame(box)
        row.pack(fill="x", pady=(0, 6))
        tb.Label(row, text="Instrument", width=10).pack(side="left")
        b = tb.Button(row, text="◀", bootstyle="secondary-outline", width=3, command=lambda: self.step_program(-1))
        b.pack(side="left")
        self.controls.append(b)
        self.program_choice = tb.Combobox(row, state="readonly", width=24)
        self.program_choice.pack(side="left", padx=4)
        self.program_choice.bind("<<ComboboxSelected>>", lambda e: self.set_program(self.program_choice.current()))
        self.controls.append(self.program_choice)
        b = tb.Button(row, text="▶", bootstyle="secondary-outline", width=3, command=lambda: self.step_program(1))
        b.pack(side="left")
        self.controls.append(b)
        tb.Label(row, text="A program of the bank, played with the keys here (not the SEQ: it chooses its own).",
                 bootstyle="secondary").pack(side="left", padx=10)
        row = tb.Frame(box)
        row.pack(fill="x")
        self.note_label = tb.Label(row, text="C4", font=("", 20, "bold"), width=5)
        self.note_label.pack(side="left")
        self.note_number = tb.Label(row, text="(60)", bootstyle="secondary")
        self.note_number.pack(side="left", padx=(0, 16))
        for text, step, hint in (("Octave -", -12, "L1"), ("▼", -1, "Down"), ("▲", 1, "Up"),
                                 ("Octave +", 12, "R1")):
            b = tb.Button(row, text=text, bootstyle="secondary-outline", command=lambda s=step: self.step_note(s))
            b.pack(side="left", padx=2)
            self.controls.append(b)
        self.play_button = tb.Button(row, text="Play (hold)", bootstyle="primary", width=12)
        self.play_button.pack(side="left", padx=(16, 0))
        self.play_button.bind("<ButtonPress-1>", lambda e: self.press(self.note()))
        self.play_button.bind("<ButtonRelease-1>", lambda e: self.release())
        self.controls.append(self.play_button)
        self.keys = tk.Canvas(box, width=(OCTAVES * 7 + 1) * WHITE_W + 1, height=WHITE_H + 1, highlightthickness=0,
                              background="#303030", cursor="hand2")
        self.keys.pack(anchor="w", pady=(8, 0))
        self.keys.bind("<ButtonPress-1>", self.key_press)
        self.keys.bind("<B1-Motion>", self.key_drag)
        self.keys.bind("<ButtonRelease-1>", lambda e: self.release())

        # Effects (kept in the settings, sent to the tester each time it starts)
        fx = dict(FX_DEFAULTS, **(self.app.settings.get("sound.fx") or {}))
        box = self.section("Effects", left)
        box.pack(fill="x", pady=(0, 8))
        self.fx_controls = []
        grid = tb.Frame(box)
        grid.pack(fill="x")
        tb.Label(grid, text="Reverb", width=10).grid(row=0, column=0, sticky="w")
        self.reverb_choice = tb.Combobox(grid, state="readonly", width=16, values=[r[0] for r in REVERBS])
        self.reverb_choice.current(max(0, min(len(REVERBS) - 1, fx["reverb"])))
        self.reverb_choice.grid(row=0, column=1, sticky="w")
        self.reverb_choice.bind("<<ComboboxSelected>>", lambda e: self.fx_changed())
        tb.Label(grid, text="Depth").grid(row=0, column=2, sticky="w", padx=(16, 6))
        self.depth = tk.IntVar(value=fx["depth"])
        self.depth_scale = tb.Scale(grid, from_=0, to=127, variable=self.depth, length=160,
                                    command=lambda v: self.fx_changed(slider=True))
        self.depth_scale.grid(row=0, column=3, sticky="w")
        self.depth_label = tb.Label(grid, text=str(fx["depth"]), width=4, anchor="e")
        self.depth_label.grid(row=0, column=4, sticky="w")
        tb.Label(grid, text="On").grid(row=0, column=5, sticky="w", padx=(16, 6))
        self.rev_voices_choice = tb.Combobox(grid, state="readonly", width=24, values=REVERB_VOICES)
        self.rev_voices_choice.current(max(0, min(len(REVERB_VOICES) - 1, fx["voices"])))
        self.rev_voices_choice.grid(row=0, column=6, sticky="w")
        self.rev_voices_choice.bind("<<ComboboxSelected>>", lambda e: self.fx_changed())
        self.reverb_label = tb.Label(box, text="", bootstyle="secondary")
        self.reverb_label.pack(anchor="w", pady=(4, 6))

        grid = tb.Frame(box)
        grid.pack(fill="x")
        tb.Label(grid, text="Transpose", width=10).grid(row=0, column=0, sticky="w")
        self.transpose = tk.IntVar(value=fx["transpose"])
        self.transpose_spin = tb.Spinbox(grid, from_=-24, to=24, textvariable=self.transpose, width=5,
                                         command=self.fx_changed)
        self.transpose_spin.grid(row=0, column=1, sticky="w")
        self.transpose_spin.bind("<Return>", lambda e: self.fx_changed())
        self.transpose_spin.bind("<FocusOut>", lambda e: self.fx_changed())
        tb.Label(grid, text="semitones (SEQ)").grid(row=0, column=2, sticky="w", padx=(4, 0))
        tb.Label(grid, text="Tempo").grid(row=0, column=3, sticky="w", padx=(16, 6))
        self.tempo = tk.IntVar(value=fx["tempo"])
        self.tempo_spin = tb.Spinbox(grid, from_=25, to=400, increment=5, textvariable=self.tempo, width=5,
                                     command=self.fx_changed)
        self.tempo_spin.grid(row=0, column=4, sticky="w")
        self.tempo_spin.bind("<Return>", lambda e: self.fx_changed())
        self.tempo_spin.bind("<FocusOut>", lambda e: self.fx_changed())
        tb.Label(grid, text="% (SEQ)").grid(row=0, column=5, sticky="w", padx=(4, 0))
        self.mono = tk.BooleanVar(value=fx["mono"])
        self.mono_check = tb.Checkbutton(grid, text="Mono", variable=self.mono, bootstyle="round-toggle",
                                         command=self.fx_changed)
        self.mono_check.grid(row=0, column=6, sticky="w", padx=(16, 0))
        b = tb.Button(grid, text="Reset effects", bootstyle="secondary-outline", command=self.reset_fx)
        b.grid(row=0, column=7, sticky="w", padx=(16, 0))
        self.fx_controls = [self.reverb_choice, self.depth_scale, self.rev_voices_choice, self.transpose_spin,
                            self.tempo_spin, self.mono_check, b]
        self.fx_job = None
        self.fx_version = 0         # SNDFX: 0 an older tester (no effects)
        self.samples_end = 0
        self.show_reverb(None)

        # Volume, stop, voices
        box = self.section("Volume", right)
        box.pack(fill="x", pady=(0, 8))
        self.volume = tk.IntVar(value=100)
        row = tb.Frame(box)
        row.pack(fill="x")
        self.volume_scale = tb.Scale(row, from_=0, to=127, variable=self.volume, length=220,
                                     command=lambda v: self.volume_moved())
        self.volume_scale.pack(side="left")
        self.controls.append(self.volume_scale)
        self.volume_label = tb.Label(row, text="100", width=4, anchor="e")
        self.volume_label.pack(side="left", padx=(6, 0))
        tb.Label(box, text="Also for the notes sounding (Triangle/Square on the controller).",
                 bootstyle="secondary", wraplength=260).pack(anchor="w", pady=(4, 0))
        b = tb.Button(right, text="Stop all sounds", bootstyle="danger", command=self.stop_sounds)
        b.pack(fill="x", pady=(0, 8))
        self.controls.append(b)

        box = self.section("Voices", right)
        box.pack(fill="x")
        self.lights = tk.Canvas(box, width=12 * 20, height=2 * 20, highlightthickness=0, background="#202020")
        self.lights.pack(anchor="w")
        self.voices_label = tb.Label(box, text="0/%d sounding" % VOICES, bootstyle="secondary")
        self.voices_label.pack(anchor="w", pady=(4, 0))

        self.draw_keys()
        self.draw_lights(0)
        self.enable(False)

    # ---------------------------------------------------------------- console

    def args(self):
        files = self.files.files()
        if not files:
            return None
        a = []
        for path in files:
            a += ["-snd", path]
        return a + ["-sndlive"]

    def play(self):
        files = self.files.files()
        if not files:
            return self.app.error("Add the sound files.")
        self.live = False
        self.disconnected = False
        self.fx_version = 0
        for v in self.mute_vars + self.solo_vars:     # (new music: every channel heard)
            v.set(False)
        self.editor.load()
        self.status.configure(text="Starting the console's sound tester...")
        self.app.run(self.args(), "Sound tester", long_running=True, stdin=True, cwd=os.path.dirname(files[0]),
                     on_line=lambda line: self.after(0, self.line, line),
                     log_filter=lambda line: not line.startswith(PROTOCOL),
                     on_done=lambda code, out: self.after(0, self.stopped))

    def stopped(self):
        self.live = False
        self.held = None
        self.status.configure(text="Console disconnected." if self.disconnected else "Stopped.")
        self.disconnected = False
        self.enable(False)
        self.draw_lights(0)
        self.draw_keys()
        self.editor.stopped()
        self.show_reverb(None)
        self.show_channels(0, 0, [])
        self.show_sep(0, 0)

    def send(self, line):
        return self.live and self.app.runner.send(line)

    def line(self, line):
        fields = line.split()
        if line.startswith("SNDLIVE ready"):
            self.live = True
            self.enable(True)
            self.status.configure(text="Live: the controls here and the controller act on the console's tester.")
        elif line.startswith("SNDLIVE disconnected"):
            self.disconnected = True
        elif line.startswith("SNDINFO") and len(fields) >= 7:
            kind, n, ps, ts, vs, rate = (int(v) for v in fields[1:7])
            self.kind = kind
            self.enable(self.live)
            self.info.configure(text=("VAB bank: %d programs, %d tones, %d VAGs" % (ps, ts, vs)) if kind == 1 else
                                ("VAG sample: %d Hz (its original rate is note C4)" % rate) if kind == 2 else
                                "No sound data (VH + VB, VAB or VAG files)")
        elif line.startswith("SNDPROGS"):
            self.programs = [int(v) for v in fields[1:]]
            self.program_choice.configure(values=["Program %d" % p for p in self.programs] or ["(no program)"])
        elif line.startswith("SNDSTATE") and len(fields) >= 10:
            self.console_state(fields[1:])
        elif line.startswith("SNDVAB") and len(fields) >= 3:
            self.editor.console(int(fields[1], 16), int(fields[2]))
        elif line.startswith("SNDFX") and len(fields) >= 3:
            self.fx_version = int(fields[1])
            self.samples_end = int(fields[2], 16)
            self.enable(self.live)
            if self.fx_version:
                self.send_fx()          # (the effects chosen here, also for a tester just started)
                self.send("repeat %d" % self.repeat.get())
            else:
                self.reverb_label.configure(text="Effects: this soundtest.ps-exe is too old (rebuild it).")
        elif line.startswith("SNDCHAN") and len(fields) >= 4 + SEQ_CHANNELS:
            try:
                rev, used, active = int(fields[1]), int(fields[2], 16), int(fields[3], 16)
                progs = [int(v) for v in fields[4:4 + SEQ_CHANNELS]]
            except ValueError:
                return
            self.show_reverb(rev)
            self.show_channels(used, active, progs)
            if len(fields) >= 6 + SEQ_CHANNELS:
                self.show_sep(int(fields[4 + SEQ_CHANNELS]), int(fields[5 + SEQ_CHANNELS]))
        elif line.startswith("SNDERR"):
            self.status.configure(text=line[7:])

    def console_state(self, fields):
        """The console's state (changed with its controller, or applied from here): shown, except a control just
        changed here (the console may not have applied it yet)."""
        try:
            prog_index, program, note, volume, playing, seq, bar, loop = (int(v) for v in fields[:8])
            voices = int(fields[8], 16)
        except ValueError:
            return
        self.state = dict(prog_index=prog_index, note=note, volume=volume, seq=seq)
        now = time.time()
        quiet = lambda name: now - self.changed.get(name, 0) < QUIET
        self.updating = True
        try:
            if not quiet("note") and self.held is None:
                self.show_note(note)
            if not quiet("volume") and volume != self.volume.get():
                self.volume.set(volume)
                self.volume_label.configure(text=str(volume))
            if not quiet("program") and 0 <= prog_index < len(self.programs):
                self.program_choice.current(prog_index)
        finally:
            self.updating = False
        self.seq_button.configure(text="Stop" if seq == 2 else "Start",
                                  bootstyle="danger-outline" if seq == 2 else "success-outline")
        if self.live:
            self.seq_button.configure(state="normal" if seq else "disabled")
        self.seq_label.configure(text="No SEQ file" if not seq else
                                 ("Playing: bar %d, loop %d" % (bar, loop)) if seq == 2 else "Stopped")
        self.draw_lights(voices)
        self.draw_keys(playing=note if playing else None)

    def enable(self, on):
        for w in self.controls:
            try:
                w.configure(state=("readonly" if isinstance(w, tb.Combobox) else "normal") if on else "disabled")
            except tk.TclError:
                pass
        if on and self.kind != 1:
            self.program_choice.configure(state="disabled")
        fx_on = on and self.fx_version > 0
        for w in self.fx_controls:
            try:
                w.configure(state=("readonly" if isinstance(w, tb.Combobox) else "normal") if fx_on else "disabled")
            except tk.TclError:
                pass
        for ch, cell in enumerate(self.ch_cells):
            for b in cell["buttons"]:
                b.configure(state="normal" if fx_on and self.ch_used >> ch & 1 else "disabled")

    # ---------------------------------------------------------------- controls

    def note(self):
        return self.state.get("note", 60)

    def show_note(self, note):
        self.note_label.configure(text=note_name(note))
        self.note_number.configure(text="(%d)" % note)
        self.state["note"] = note
        self.draw_keys()

    def set_note(self, note):
        note = max(0, min(127, note))
        self.changed["note"] = time.time()
        self.show_note(note)
        self.send("note %d" % note)

    def step_note(self, step):
        self.set_note(self.note() + step)

    def press(self, note):
        """A note played from here, as long as the mouse button is held (Cross held on the controller)."""
        if not self.live:
            return
        note = max(0, min(127, note))
        self.held = note
        self.changed["note"] = time.time()
        self.show_note(note)
        self.send("play %d" % note)
        self.draw_keys(playing=note)

    def release(self):
        if self.held is None:
            return
        self.held = None
        self.changed["note"] = time.time()
        self.send("release")
        self.draw_keys()

    def set_program(self, index):
        if self.updating or not 0 <= index < len(self.programs):
            return
        self.changed["program"] = time.time()
        self.program_choice.current(index)
        self.send("program %d" % index)

    def step_program(self, step):
        if self.programs:
            current = self.program_choice.current()
            self.set_program(((current if current >= 0 else 0) + step) % len(self.programs))

    def volume_moved(self):
        if self.updating:
            return
        self.volume_label.configure(text=str(self.volume.get()))
        self.changed["volume"] = time.time()
        if self.volume_job is None:
            self.volume_job = self.after(VOLUME_SEND_MS, self.send_volume)

    def send_volume(self):
        self.volume_job = None
        self.changed["volume"] = time.time()
        self.send("volume %d" % self.volume.get())

    def toggle_seq(self):
        self.send("seq %d" % (0 if self.state.get("seq") == 2 else 1))

    # ---------------------------------------------------------------- effects and SEQ channels

    def fx_values(self):
        def number(var, default, low, high):
            try:
                return max(low, min(high, int(var.get())))
            except (tk.TclError, ValueError):
                return default
        return dict(reverb=max(0, self.reverb_choice.current()), depth=self.depth.get(),
                    voices=max(0, self.rev_voices_choice.current()), transpose=number(self.transpose, 0, -24, 24),
                    tempo=number(self.tempo, 100, 25, 400), mono=bool(self.mono.get()))

    def mute_mask(self):
        """Channels muted: the M ones, and while some are solo every channel that isn't."""
        muted = sum(1 << ch for ch, v in enumerate(self.mute_vars) if v.get())
        solo = sum(1 << ch for ch, v in enumerate(self.solo_vars) if v.get())
        return muted | ((~solo & 0xFFFF) if solo else 0)

    def fx_changed(self, slider=False):
        fx = self.fx_values()
        self.depth_label.configure(text=str(fx["depth"]))
        self.app.settings.set("sound.fx", fx)
        if slider:          # (the slider sends many changes: sent together)
            if self.fx_job is None:
                self.fx_job = self.after(VOLUME_SEND_MS, self.send_fx)
        else:
            self.send_fx()

    def repeat_changed(self):
        self.app.settings.set("sound.repeat", bool(self.repeat.get()))
        self.send("repeat %d" % self.repeat.get())

    def send_fx(self):
        self.fx_job = None
        if not self.fx_version:
            return
        fx = self.fx_values()
        self.send("fx %d %d %d %d %d %d %d" % (fx["reverb"], fx["depth"], fx["voices"], fx["transpose"], fx["tempo"],
                                               1 if fx["mono"] else 0, self.mute_mask()))

    def reset_fx(self):
        self.reverb_choice.current(FX_DEFAULTS["reverb"])
        self.depth.set(FX_DEFAULTS["depth"])
        self.rev_voices_choice.current(FX_DEFAULTS["voices"])
        self.transpose.set(FX_DEFAULTS["transpose"])
        self.tempo.set(FX_DEFAULTS["tempo"])
        self.mono.set(FX_DEFAULTS["mono"])
        self.fx_changed()

    def mutes_changed(self):
        self.send_fx()

    def unmute_all(self):
        for v in self.mute_vars + self.solo_vars:
            v.set(False)
        self.send_fx()

    def show_reverb(self, active):
        """The reverb the console uses (SNDCHAN): its work area, or why there's none."""
        chosen = max(0, self.reverb_choice.current())
        name, size = REVERBS[chosen]
        if active is None:
            text = "The tester applies the effects once it runs." if chosen else ""
        elif active < 0:
            text = ("Not enough SPU RAM for %s: it needs %d KB at the end, the samples use up to 0x%05X (a smaller "
                    "reverb fits up to %d KB)." % (name, size // 1024, self.samples_end,
                                                   max(0, SPU_RAM - self.samples_end) // 1024))
        elif active == 0:
            text = "Reverb off."
        else:
            base = (SPU_RAM - REVERBS[active][1]) & ~1023
            text = "%s: work area of %d KB at 0x%05X, the end of SPU RAM (%d KB left after the samples)." % (
                REVERBS[active][0], (SPU_RAM - base) // 1024, base, max(0, base - self.samples_end) // 1024)
        self.reverb_label.configure(text=text, bootstyle="warning" if active is not None and active < 0 else
                                    "secondary")

    def show_sep(self, index, count):
        """A SEP's SEQs (the row shown only for a SEP), the one played."""
        if count != self.sep_count:
            self.sep_count = count
            if count > 1:
                self.sep_choice.configure(values=["SEQ %d of %d" % (i + 1, count) for i in range(count)])
                self.sep_row.pack(anchor="w", pady=(6, 0), before=self.info)
            else:
                self.sep_row.pack_forget()
        if count > 1 and 0 <= index < count and self.sep_choice.current() != index:
            self.sep_choice.current(index)

    def show_channels(self, used, active, progs):
        changed = used != self.ch_used
        self.ch_used = used
        for ch, cell in enumerate(self.ch_cells):
            on = used >> ch & 1
            cell["light"].delete("all")
            cell["light"].create_rectangle(0, 0, 10, 10, outline="",
                                           fill=LIGHT_ON if active >> ch & 1 else LIGHT_OFF)
            cell["name"].configure(bootstyle="default" if on else "secondary")
            cell["prog"].configure(text=("Program %d" % progs[ch]) if on and ch < len(progs) else
                                   "(no notes)" if self.live and self.fx_version else "")
        if changed:
            self.enable(self.live)

    def stop_sounds(self):
        self.held = None
        self.send("stop")

    # ---------------------------------------------------------------- keyboard and voices

    def first_note(self):
        """The keyboard's first note: the C of the note's octave (the last octaves: shown from an earlier C)."""
        return max(0, min(self.note() // 12 * 12, 120 - (OCTAVES - 1) * 12))

    def key_rects(self):
        """(note, x0, y0, x1, y1, black) of the keys shown, black keys last (drawn over the white ones)."""
        first, keys = self.first_note(), []
        for i in range(OCTAVES * 7 + 1):
            note = first + (i // 7) * 12 + WHITE_OF[i % 7]
            if note <= 127:
                keys.append((note, i * WHITE_W, 0, (i + 1) * WHITE_W, WHITE_H, False))
        for octave in range(OCTAVES):
            for semitone, white in BLACK_OF:
                note = first + octave * 12 + semitone
                x = (octave * 7 + white + 1) * WHITE_W - BLACK_W // 2
                if note <= 127:
                    keys.append((note, x, 0, x + BLACK_W, BLACK_H, True))
        return keys

    def draw_keys(self, playing=None):
        if playing is None and self.held is not None:
            playing = self.held
        c = self.keys
        c.delete("all")
        note = self.note()
        for n, x0, y0, x1, y1, black in self.key_rects():
            fill = KEY_COLORS["held"] if n == playing else KEY_COLORS["note"] if n == note else \
                KEY_COLORS["black" if black else "white"]
            c.create_rectangle(x0, y0, x1, y1, fill=fill, outline=KEY_COLORS["outline"])
            if not black and n % 12 == 0:
                c.create_text((x0 + x1) // 2, y1 - 10, text=note_name(n), fill="#606060", font=("", 8))

    def key_at(self, x, y):
        for n, x0, y0, x1, y1, black in reversed(self.key_rects()):     # (black keys first: they are on top)
            if x0 <= x < x1 and y0 <= y < y1:
                return n
        return None

    def key_press(self, event):
        note = self.key_at(event.x, event.y)
        if note is not None:
            self.press(note)

    def key_drag(self, event):
        """Dragging over the keys plays each one (glissando)."""
        note = self.key_at(event.x, event.y)
        if self.held is not None and note is not None and note != self.held:
            self.press(note)

    def draw_lights(self, voices):
        c = self.lights
        c.delete("all")
        for v in range(VOICES):
            x, y = (v % 12) * 20 + 3, (v // 12) * 20 + 3
            c.create_rectangle(x, y, x + 14, y + 14, fill=LIGHT_ON if voices >> v & 1 else LIGHT_OFF, outline="")
        self.voices_label.configure(text="%d/%d sounding" % (bin(voices).count("1"), VOICES))
