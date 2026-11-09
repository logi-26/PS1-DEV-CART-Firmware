"""
PS1 DEV CART GUI: Music Tracker page. Songs made of riffs placed on tracks (tracker.py), played by the PS1's sound
tester (ps1transfer -snd song.vh -snd song.vb -snd song.seq -sndlive, see ps1transfer/sndlive.c) and saved as what
games play with libsnd: a SEQ and a VAB.

- Riffs: the song's own and a library of starter riffs (riffs.py: drums, bass, chords, arpeggios, melodies), picked
  on the left, listened to, put on the tracks.
- The arranger (tracker_arrange.py): tracks as lanes, riffs as blocks over time, moved and stretched with the mouse;
  the ruler plays from where it is clicked, and sets the loop.
- The riff editor: a piano roll (tracker_roll.py: notes put, moved and stretched with the mouse) or the note grid
  (notes typed on the computer keyboard as trackers, instrument, volume and effect columns in hexadecimal,
  selection, copy/paste, transpose).
- Instruments: built-in waveforms or samples (WAV, MP3... with FFmpeg), each a program of the song's VAB.
- Play: the song, or the riff edited (looped). While it plays, each change is sent to the console as a new SEQ
  (seqload), played from the same time; changed instruments with it (bankload). The notes put or typed are heard on
  the console (the sound tester started for them the first time).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import copy
import os
import tempfile
import time
import tkinter as tk
import tkinter.font as tkfont
from tkinter import filedialog, messagebox, simpledialog

import ttkbootstrap as tb

import audioconv
import movieconv
import riffs
import tracker
from pages.base import Page
from pages.sound import REVERBS
from pages.tracker_arrange import Arranger
from pages.tracker_roll import PianoRoll

PROTOCOL = ("SNDLIVE", "SNDINFO", "SNDPROGS", "SNDSTATE", "SNDVAB", "SNDWROTE", "SNDVH", "SNDSPU", "SNDFX",
            "SNDCHAN", "SNDPOS", "SNDSEQ", "SNDBANK")
SONG_TYPES = [("Tracker songs", "*" + tracker.EXTENSION), ("All files", "*.*")]
SAMPLE_TYPES = [("Sounds", "*.wav *.mp3 *.ogg *.flac *.m4a *.aif *.aiff"), ("All files", "*.*")]

# Piano keys of the computer keyboard (keysyms): semitones from the octave's C
LOWER_KEYS = ("z", "s", "x", "d", "c", "v", "g", "b", "h", "n", "j", "m", "comma", "l", "period", "semicolon",
              "slash")
UPPER_KEYS = ("q", "2", "w", "3", "e", "r", "5", "t", "6", "y", "7", "u", "i", "9", "o", "0", "p")
PIANO = {k: i for i, k in enumerate(LOWER_KEYS)}
PIANO.update({k: 12 + i for i, k in enumerate(UPPER_KEYS)})
NOTE_OFF_KEYS = ("1", "grave")

# Columns of a channel: cursor positions (field, digit), their text offset and width (characters)
COLS = [("note", 0, 0, 3), ("ins", 0, 4, 1), ("ins", 1, 5, 1), ("vol", 0, 7, 1), ("vol", 1, 8, 1),
        ("fx", 0, 10, 1), ("val", 0, 11, 1), ("val", 1, 12, 1)]
FIELD = {"note": 0, "ins": 1, "vol": 2, "fx": 3, "val": 4}
CH_CHARS = 14                   # Characters of a channel (13 and a gap)
ROWNUM_CHARS = 4
COLORS = dict(bg="#16191c", beat="#1f2327", bar="#272c32", cursor_row="#26334a", cursor="#4fc1ff",
              select="#35507a", play="#5c4316", text="#e6e6e6", empty="#4a4f55", ins="#7fd1ff", vol="#8fdf8f",
              fx="#f0b060", rownum="#7a8088", rownum_bar="#c8ccd0", header="#2a2f35", header_text="#d0d4d8",
              muted="#7a2a2a", sep="#30353b")
PREVIEW_MS = 350                # A note typed: heard this long
LIVE_UPDATE_MS = 350            # Changes sent to the console together
UNDO_MAX = 300
NOTE_LENGTHS = (("1/16 note", 0.25), ("1/8 note", 0.5), ("1/4 note (a beat)", 1), ("1/2 note", 2),
                ("Whole note (a bar)", 4))


def _clamp(v, lo, hi):
    return max(lo, min(hi, v))


class TrackerPage(Page):
    title = "Music Tracker"

    def build(self):
        self.song = tracker.new_song()
        self.path = None
        self.dirty = False
        self.cache = tracker.InstrumentCache()
        self.pattern = 0                # Riff edited
        self.cur = [0, 0, 0]            # Note grid cursor: row, channel, column (COLS)
        self.anchor = None              # Selection: the other corner (row, channel), None: no selection
        self.top, self.left = 0, 0      # First row and channel shown
        self.undo_stack, self.redo_stack = [], []
        self.clipboard = None
        self.muted = set()              # Channels muted in the note grid (riff played)
        self.muted_tracks = set()       # Tracks muted (song played)
        self.brush_id = None            # Riff picked to be put on the tracks: ("song", index) or ("lib", riff id)
        # Console
        self.running = False            # The tester task started from here
        self.live = False               # It answers (SNDLIVE ready)
        self.auto_started = False       # Started for the notes put (heard): once, not again if it failed
        self.bank_sent = None           # VH + VB the tester has
        self.seq_sent = None            # SEQ it plays
        self.play_kind = None           # What the SEQ on the console is: "song", ("riff", index), "listen"
        self.timing = None              # Its rows (tracker.compile_seq: (ms, 0, row))
        self.pending = None             # (start ms, repeat): played once the tester answers
        self.pending_seq = None         # A SEQ asked for while it starts (sent once it answers)
        self.restart = None             # Tester restarted (new instruments), then pending played
        self.seq_state = 0              # SNDSTATE: 0 no SEQ, 1 stopped, 2 playing
        self.pos = None                 # (ms, time received, paused) of the last SNDPOS while playing
        self.play_row = None            # Row playing (of the song, or of the riff played)
        self.live_job = self.preview_job = self.arrange_job = None
        self.seq_files = self.bank_files = 0
        self.bank_cache = None          # (instruments, (VH, VB))
        self.work = os.path.join(tempfile.gettempdir(), "ps1devcart_tracker")

        tb.Label(self, text="Music tracker", font=("", 16, "bold")).pack(anchor="w", pady=(0, 2))
        tb.Label(self, text="Make music from riffs: pick one on the left (or from the Library), click on a track to "
                            "put it there, and draw its notes in the piano roll below. It plays on the PS1 as you "
                            "make it, and saves as SEQ + VAB files that games play with libsnd (or as MIDI).",
                 bootstyle="secondary", wraplength=1100, justify="left").pack(anchor="w", pady=(0, 4))

        bar = tb.Frame(self)
        bar.pack(fill="x", pady=(0, 4))
        for text, cmd in (("New", self.new_song), ("Open...", self.open_song), ("Save", self.save_song),
                          ("Save as...", self.save_song_as), ("Import MIDI...", self.import_midi)):
            tb.Button(bar, text=text, bootstyle="secondary-outline", command=cmd).pack(side="left", padx=(0, 4))
        export = tb.Menubutton(bar, text="Export", bootstyle="secondary-outline")
        menu = tk.Menu(export, tearoff=0)
        menu.add_command(label="SEQ + VAB (VH + VB) for a game...", command=self.export_seq)
        menu.add_command(label="MIDI file...", command=self.export_midi)
        export["menu"] = menu
        export.pack(side="left", padx=(0, 16))
        tb.Button(bar, text="▶ Play song", bootstyle="success", command=lambda: self.play("song")).pack(
            side="left", padx=(0, 4))
        tb.Button(bar, text="▶ Play riff", bootstyle="success-outline", command=lambda: self.play("riff")).pack(
            side="left", padx=(0, 4))
        self.pause_button = tb.Button(bar, text="Pause", bootstyle="warning-outline", width=7,
                                      command=self.toggle_pause, state="disabled")
        self.pause_button.pack(side="left", padx=(0, 4))
        tb.Button(bar, text="■ Stop", bootstyle="danger-outline", command=self.stop_music).pack(side="left",
                                                                                               padx=(0, 4))
        tb.Button(bar, text="Disconnect", bootstyle="secondary-outline", command=self.disconnect).pack(
            side="left", padx=(0, 4))
        self.status = tb.Label(bar, text="", bootstyle="secondary")
        self.status.pack(side="left", padx=10)

        row = tb.Frame(self)
        row.pack(fill="x", pady=(0, 4))
        tb.Label(row, text="Title").pack(side="left")
        self.title_var = tk.StringVar()
        e = tb.Entry(row, textvariable=self.title_var, width=22)
        e.pack(side="left", padx=(4, 12))
        self.title_var.trace_add("write", lambda *a: self.song_setting("title", self.title_var.get()))
        tb.Label(row, text="BPM").pack(side="left")
        self.bpm_var = tk.StringVar()
        tb.Spinbox(row, from_=20, to=300, textvariable=self.bpm_var, width=5,
                   command=self.bpm_changed).pack(side="left", padx=(4, 12))
        self.bpm_var.trace_add("write", lambda *a: self.after_idle(self.bpm_changed))
        tb.Label(row, text="Rows per beat").pack(side="left")
        self.lpb_choice = tb.Combobox(row, state="readonly", width=4, values=[str(v) for v in tracker.LPB_CHOICES])
        self.lpb_choice.pack(side="left", padx=(4, 12))
        self.lpb_choice.bind("<<ComboboxSelected>>", lambda e: self.song_setting(
            "lpb", int(self.lpb_choice.get()), redraw=True))
        tb.Label(row, text="Reverb").pack(side="left")
        self.reverb_choice = tb.Combobox(row, state="readonly", width=13, values=[r[0] for r in REVERBS])
        self.reverb_choice.pack(side="left", padx=(4, 6))
        self.reverb_choice.bind("<<ComboboxSelected>>", lambda e: self.reverb_changed())
        tb.Label(row, text="Depth").pack(side="left")
        self.depth_var = tk.StringVar()
        tb.Spinbox(row, from_=0, to=127, increment=8, textvariable=self.depth_var, width=4,
                   command=self.reverb_changed).pack(side="left", padx=(4, 12))
        self.depth_var.trace_add("write", lambda *a: self.after_idle(self.reverb_changed))
        self.follow = tk.BooleanVar(value=True)
        tb.Checkbutton(row, text="Follow the music", variable=self.follow).pack(side="left")

        panes = tb.Panedwindow(self, orient="vertical")
        panes.pack(fill="both", expand=True)
        top = tb.Frame(panes)
        bottom = tb.Frame(panes)
        panes.add(top, weight=2)
        panes.add(bottom, weight=6)

        # Riffs (left) and the arranger
        browser = tb.Frame(top, width=230)
        browser.pack(side="left", fill="y", padx=(0, 6))
        browser.pack_propagate(False)
        tb.Label(browser, text="Riffs", font=("", 10, "bold")).pack(anchor="w")
        tree_box = tb.Frame(browser)
        tree_box.pack(fill="both", expand=True)
        self.riff_tree = tb.Treeview(tree_box, show="tree", selectmode="browse", height=6)
        sb = tb.Scrollbar(tree_box, orient="vertical", command=self.riff_tree.yview)
        self.riff_tree.configure(yscrollcommand=sb.set)
        self.riff_tree.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        self.riff_tree.bind("<<TreeviewSelect>>", lambda e: self.riff_picked())
        self.riff_tree.bind("<Double-Button-1>", lambda e: self.listen())
        self.riff_tree.insert("", "end", "song", text="This song's riffs", open=True)
        lib = self.riff_tree.insert("", "end", "lib", text="Library (starter riffs)", open=True)
        for cat, items in riffs.by_category():
            node = self.riff_tree.insert(lib, "end", "cat:" + cat, text=cat, open=False)
            for rid, name in items:
                self.riff_tree.insert(node, "end", "lib:" + rid, text=name)
        row = tb.Frame(browser)
        row.pack(fill="x", pady=(4, 0))
        tb.Button(row, text="▶ Listen", bootstyle="success-outline", command=self.listen).pack(side="left")
        tb.Button(row, text="New riff", bootstyle="secondary-outline", command=self.new_pattern).pack(
            side="left", padx=4)
        riff_menu = tb.Menubutton(row, text="More", bootstyle="secondary-outline")
        menu = tk.Menu(riff_menu, tearoff=0)
        menu.add_command(label="Rename the riff...", command=self.rename_pattern)
        menu.add_command(label="Copy the riff (a new one to change)", command=self.clone_pattern)
        menu.add_command(label="Delete the riff", command=self.delete_pattern)
        riff_menu["menu"] = menu
        riff_menu.pack(side="left")

        arr = tb.Frame(top)
        arr.pack(side="left", fill="both", expand=True)
        row = tb.Frame(arr)
        row.pack(fill="x", pady=(0, 2))
        tb.Label(row, text="Song", font=("", 10, "bold")).pack(side="left")
        tb.Label(row, text="click on a track: put the riff picked · drag a block: move it, its end: make it "
                           "longer · right click: its menu · click the ruler: play from there",
                 bootstyle="secondary").pack(side="left", padx=8)
        for text, cmd in (("−", lambda: self.arranger.zoom(-1)), ("+", lambda: self.arranger.zoom(1)),
                          ("Add track", self.add_track)):
            tb.Button(row, text=text, bootstyle="secondary-outline", command=cmd,
                      width=None if len(text) > 1 else 2).pack(side="right", padx=(4, 0))
        self.arranger = Arranger(arr, self)
        self.arranger.pack(fill="both", expand=True)

        # The riff editor (piano roll or note grid) and the instruments
        right = tb.Frame(bottom, width=330)
        right.pack(side="right", fill="y", padx=(8, 0))
        right.pack_propagate(False)
        left = tb.Frame(bottom)
        left.pack(side="left", fill="both", expand=True)
        row = tb.Frame(left)
        row.pack(fill="x", pady=(6, 3))
        self.riff_label = tb.Label(row, text="", font=("", 10, "bold"))
        self.riff_label.pack(side="left")
        tb.Label(row, text="Rows").pack(side="left", padx=(10, 2))
        self.rows_var = tk.StringVar()
        sp = tb.Spinbox(row, from_=1, to=tracker.MAX_ROWS, textvariable=self.rows_var, width=4,
                        command=self.rows_changed)
        sp.pack(side="left")
        sp.bind("<Return>", lambda e: self.rows_changed())
        sp.bind("<FocusOut>", lambda e: self.rows_changed())
        self.view = tk.StringVar(value="roll")
        tb.Radiobutton(row, text="Piano roll", variable=self.view, value="roll", bootstyle="toolbutton",
                       command=self.show_view).pack(side="left", padx=(12, 0))
        tb.Radiobutton(row, text="Note grid", variable=self.view, value="grid", bootstyle="toolbutton",
                       command=self.show_view).pack(side="left")
        self.roll_tools = tb.Frame(row)
        self.roll_tools.pack(side="left", padx=(12, 0))
        tb.Label(self.roll_tools, text="New notes").pack(side="left")
        self.length_choice = tb.Combobox(self.roll_tools, state="readonly", width=17,
                                         values=[n for n, _ in NOTE_LENGTHS])
        self.length_choice.current(2)
        self.length_choice.pack(side="left", padx=(4, 8))
        tb.Button(self.roll_tools, text="−", width=2, bootstyle="secondary-outline",
                  command=lambda: self.roll.zoom(-1)).pack(side="left")
        tb.Button(self.roll_tools, text="+", width=2, bootstyle="secondary-outline",
                  command=lambda: self.roll.zoom(1)).pack(side="left", padx=(4, 0))
        self.grid_tools = tb.Frame(row)
        tb.Label(self.grid_tools, text="Channels").pack(side="left")
        self.ch_var = tk.StringVar()
        tb.Spinbox(self.grid_tools, from_=1, to=tracker.MAX_CHANNELS, textvariable=self.ch_var, width=4,
                   command=self.channels_changed).pack(side="left", padx=(4, 12))
        tb.Label(self.grid_tools, text="Octave").pack(side="left")
        self.octave = tk.IntVar(value=4)
        tb.Spinbox(self.grid_tools, from_=0, to=8, textvariable=self.octave, width=3, state="readonly").pack(
            side="left", padx=(4, 12))
        tb.Label(self.grid_tools, text="Step").pack(side="left")
        self.step = tk.IntVar(value=1)
        tb.Spinbox(self.grid_tools, from_=0, to=16, textvariable=self.step, width=3, state="readonly").pack(
            side="left", padx=(4, 0))

        self.editor = tb.Frame(left)
        self.editor.pack(fill="both", expand=True)
        self.roll = PianoRoll(self.editor, self)
        self.roll_help = tb.Label(left, text="Click: put a note (with the instrument picked on the right; drag "
                                             "to make it longer) · drag a note: move it · drag its right end: "
                                             "its length · right click or Delete: remove it · arrows: move the "
                                             "note picked (Shift+Up/Down: an octave) · Ctrl+wheel: zoom · "
                                             "Space: play the riff / stop.",
                                  bootstyle="secondary", wraplength=900, justify="left")

        # Note grid editor: header (channels: click mutes, right click solo), rows, scrollbars
        self.font = tkfont.Font(family="Consolas", size=11)
        self.cw = self.font.measure("0")
        self.lh = self.font.metrics("linespace") + 3
        grid = self.grid_frame = tb.Frame(self.editor)
        self.header = tk.Canvas(grid, height=self.lh + 4, highlightthickness=0, background=COLORS["header"])
        self.header.grid(row=0, column=0, sticky="ew")
        self.canvas = tk.Canvas(grid, highlightthickness=1, highlightcolor=COLORS["cursor"],
                                highlightbackground=COLORS["sep"], background=COLORS["bg"], takefocus=1)
        self.canvas.grid(row=1, column=0, sticky="nsew")
        self.vbar = tb.Scrollbar(grid, orient="vertical", command=self.yview)
        self.vbar.grid(row=1, column=1, sticky="ns")
        self.hbar = tb.Scrollbar(grid, orient="horizontal", command=self.xview)
        self.hbar.grid(row=2, column=0, sticky="ew")
        grid.rowconfigure(1, weight=1)
        grid.columnconfigure(0, weight=1)
        self.canvas.bind("<Configure>", lambda e: self.draw())
        self.canvas.bind("<Key>", self.key)
        self.canvas.bind("<Tab>", lambda e: (self.move_channel(1), "break")[1])
        self.canvas.bind("<Shift-Tab>", lambda e: (self.move_channel(-1), "break")[1])
        self.canvas.bind("<ButtonPress-1>", self.click)
        self.canvas.bind("<B1-Motion>", self.drag)
        self.canvas.bind("<MouseWheel>", lambda e: self.scroll(-3 if e.delta > 0 else 3))
        self.canvas.bind("<FocusIn>", lambda e: self.draw())
        self.canvas.bind("<FocusOut>", lambda e: self.draw())
        self.header.bind("<ButtonPress-1>", lambda e: self.header_click(e, solo=False))
        self.header.bind("<ButtonPress-3>", lambda e: self.header_click(e, solo=True))
        self.grid_help = tb.Label(left, text="Notes: Z S X D C V... (an octave) and Q 2 W 3 E... (the next), 1: note "
                                             "off, Delete: clear. Instrument, volume, effect: hexadecimal. Effects: "
                                             "P pan, V volume, E expression, B bend, T tempo, G slide to the note "
                                             "(rows), A arpeggio (xy semitones). Space: play from the cursor / "
                                             "stop. Shift+arrows: select, Ctrl+C/X/V, Ctrl+Z/Y, Ctrl+Up/Down: "
                                             "transpose (Shift: an octave), Numpad +/-: octave. Header: click "
                                             "mutes a channel, right click plays it alone.",
                                  bootstyle="secondary", wraplength=900, justify="left")

        ins_box = tb.Labelframe(right, text="Instruments", padding=6)
        ins_box.pack(fill="both", expand=True)
        self.build_instrument_tab(ins_box)
        self.show_view()

        path = self.app.settings.get("tracker.song")
        if path and os.path.isfile(path):
            try:
                self.set_song(tracker.load(path), path)
            except (OSError, tracker.SongError):
                self.set_song(tracker.new_song(), None)
        else:
            self.set_song(tracker.new_song(), None)
        self.tick()

    def show_view(self):
        """The riff editor shown: the piano roll or the note grid."""
        roll = self.view.get() == "roll"
        for w in (self.roll, self.grid_frame, self.roll_help, self.grid_help, self.roll_tools, self.grid_tools):
            w.pack_forget()
        if roll:
            self.roll_tools.pack(side="left", padx=(12, 0))
            self.roll.pack(fill="both", expand=True)
            self.roll_help.pack(anchor="w", pady=(4, 0))
            self.after_idle(self.roll.show_notes)
        else:
            self.grid_tools.pack(side="left", padx=(12, 0))
            self.grid_frame.pack(fill="both", expand=True)
            self.grid_help.pack(anchor="w", pady=(4, 0))
            self.after_idle(self.draw)

    def focus_editor(self):
        (self.roll.grid_c if self.view.get() == "roll" else self.canvas).focus_set()

    # ---------------------------------------------------------------- side panels

    def build_instrument_tab(self, tab):
        self.ins_list = tk.Listbox(tab, height=7, activestyle="none", exportselection=False,
                                   background=COLORS["bg"], foreground=COLORS["text"], font=("Consolas", 10),
                                   selectbackground=COLORS["select"], highlightthickness=0)
        self.ins_list.pack(fill="x", pady=(0, 4))
        self.ins_list.bind("<<ListboxSelect>>", lambda e: self.instrument_selected())
        row = tb.Frame(tab)
        row.pack(fill="x", pady=(0, 8))
        tb.Button(row, text="Add sample...", bootstyle="secondary-outline", command=self.add_sample).pack(
            side="left")
        wave = tb.Menubutton(row, text="Add waveform", bootstyle="secondary-outline")
        menu = tk.Menu(wave, tearoff=0)
        for w in tracker.WAVEFORMS:
            menu.add_command(label=w.capitalize(), command=lambda w=w: self.add_wave(w))
        wave["menu"] = menu
        wave.pack(side="left", padx=4)
        tb.Button(row, text="Remove", bootstyle="danger-outline", command=self.remove_instrument).pack(side="left")

        # The instrument's settings: scrolled (the panel is as high as the riff editor)
        box = tb.Frame(tab)
        box.pack(fill="both", expand=True)
        view = tk.Canvas(box, highlightthickness=0, width=290, background=tb.Style().colors.bg)
        bar = tb.Scrollbar(box, orient="vertical", command=view.yview)
        view.configure(yscrollcommand=bar.set)
        bar.pack(side="right", fill="y")
        view.pack(side="left", fill="both", expand=True)
        inner = tb.Frame(view)
        view.create_window(0, 0, window=inner, anchor="nw")
        inner.bind("<Configure>", lambda e: view.configure(scrollregion=view.bbox("all")))

        def wheel(e):
            if inner.winfo_height() > view.winfo_height():
                view.yview_scroll(-1 if e.delta > 0 else 1, "units")
        view.bind_all("<MouseWheel>", lambda e: wheel(e) if str(e.widget).startswith(str(box)) else None, add="+")
        form = tb.Frame(inner)
        form.pack(fill="x")
        self.ins_vars = {}
        self.ins_widgets = {}

        def line(label, key, widget, **kw):
            r = len(self.ins_widgets)
            tb.Label(form, text=label).grid(row=r, column=0, sticky="w", pady=2)
            var = tk.BooleanVar() if widget is tb.Checkbutton else tk.StringVar()
            if widget is tb.Checkbutton:
                w = widget(form, variable=var, command=lambda: self.instrument_changed(key))
            else:
                w = widget(form, textvariable=var, **kw)
            w.grid(row=r, column=1, sticky="w", pady=2)
            self.ins_vars[key] = var
            self.ins_widgets[key] = w
            return w

        e = line("Name", "name", tb.Entry, width=22)
        e.bind("<KeyRelease>", lambda ev: self.instrument_changed("name"))
        w = line("Waveform", "wave", tb.Combobox, state="readonly", width=12, values=list(tracker.WAVEFORMS))
        w.bind("<<ComboboxSelected>>", lambda ev: self.instrument_changed("wave"))
        e = line("Sample", "path", tb.Entry, width=22)
        e.bind("<Double-Button-1>", lambda ev: self.browse_sample())
        e.bind("<FocusOut>", lambda ev: self.instrument_changed("path"))
        for label, key, lo, hi in (("Root note (MIDI)", "root", 0, 127), ("Fine tune (cents)", "cents", -100, 100),
                                   ("Volume", "vol", 0, 127), ("Pan (64 centre)", "pan", 0, 127),
                                   ("Bend range (semitones)", "bend", 1, 12)):
            line(label, key, tb.Spinbox, from_=lo, to=hi, width=6, command=lambda k=key: self.instrument_changed(k))
            self.ins_widgets[key].bind("<FocusOut>", lambda ev, k=key: self.instrument_changed(k))
            self.ins_widgets[key].bind("<Return>", lambda ev, k=key: self.instrument_changed(k))
        w = line("Sample rate (Hz)", "rate", tb.Combobox, width=8, values=[str(r) for r in audioconv.VAG_RATES])
        w.bind("<<ComboboxSelected>>", lambda ev: self.instrument_changed("rate"))
        w.bind("<FocusOut>", lambda ev: self.instrument_changed("rate"))
        w = line("Envelope", "envelope", tb.Combobox, state="readonly", width=24,
                 values=[e[0] for e in audioconv.ENVELOPES])
        w.bind("<<ComboboxSelected>>", lambda ev: self.instrument_changed("envelope"))
        line("Loop the sample", "loop", tb.Checkbutton)
        line("Loop start (s)", "loop_start", tb.Spinbox, from_=0, to=60, increment=0.01, width=6,
             command=lambda: self.instrument_changed("loop_start"))
        self.ins_widgets["loop_start"].bind("<FocusOut>", lambda ev: self.instrument_changed("loop_start"))
        line("Reverb", "reverb", tb.Checkbutton)
        line("Normalize", "normalize", tb.Checkbutton)
        tb.Label(inner, text="A waveform plays at the note's pitch. A sample plays at its own pitch at its root note "
                           "(the note it was recorded at). The instrument picked here is the one new notes play.",
                 bootstyle="secondary", wraplength=280, justify="left").pack(anchor="w", pady=(8, 0))
        self.ins_loading = False

    # ---------------------------------------------------------------- song

    def set_song(self, song, path):
        self.song = tracker.check(song)
        self.path = path
        self.dirty = False
        self.pattern = 0
        self.cur = [0, 0, 0]
        self.anchor = None
        self.top = self.left = 0
        self.undo_stack, self.redo_stack = [], []
        self.updating = True
        self.title_var.set(self.song["title"])
        self.bpm_var.set(str(self.song["bpm"]))
        self.lpb_choice.set(str(self.song["lpb"]))
        self.reverb_choice.current(self.song["reverb"])
        self.depth_var.set(str(self.song["depth"]))
        self.ch_var.set(str(self.song["channels"]))
        self.updating = False
        self.reverb_changed()
        self.muted_tracks = set()
        self.arranger.sel = None
        self.roll.sel = None
        self.brush_id = None
        self.refresh_lists()
        self.ins_list.selection_clear(0, "end")
        if self.song["instruments"]:
            self.ins_list.selection_set(0)
        self.instrument_selected()
        self.show_title()
        self.edit_pattern(self.song["clips"][0]["pattern"] if self.song["clips"] else 0)

    def show_title(self):
        name = os.path.basename(self.path) if self.path else "New song"
        self.status_text("%s%s" % (name, " (changed)" if self.dirty else ""))

    def status_text(self, text, error=False):
        self.status.configure(text=text, bootstyle="danger" if error else "secondary")

    def changed(self, redraw=True, lists=False):
        """The song changed: shown, sent to the console while it plays."""
        if not self.dirty:
            self.dirty = True
            self.show_title()
        if lists:
            self.refresh_lists()
        if redraw:
            self.draw()
            if self.view.get() == "roll":
                self.roll.draw()
        if not self.arrange_job:        # (the arranger shows the riffs' notes: drawn again, not at each change)
            self.arrange_job = self.after(120, self.arrange_redraw)
        if self.live:
            if self.live_job:
                self.after_cancel(self.live_job)
            self.live_job = self.after(LIVE_UPDATE_MS, self.live_update)

    def song_setting(self, key, value, redraw=False):
        if getattr(self, "updating", False) or self.song.get(key) == value:
            return
        self.song[key] = value
        self.changed(redraw)

    def reverb_changed(self):
        """The song's reverb (instruments with Reverb ticked): sent to the console with the mutes (fx)."""
        if getattr(self, "updating", False):
            return
        try:
            depth = max(0, min(127, int(self.depth_var.get())))
        except ValueError:
            return
        reverb = max(0, self.reverb_choice.current())
        if (reverb, depth) == (self.song["reverb"], self.song["depth"]):
            if getattr(self, "fx_sent", None) != (reverb, depth) and getattr(self, "live", False):
                self.send_mutes()
            return
        self.song["reverb"], self.song["depth"] = reverb, depth
        self.changed(redraw=False)
        if getattr(self, "live", False):
            self.send_mutes()

    def bpm_changed(self):
        try:
            bpm = int(self.bpm_var.get())
        except ValueError:
            return
        if 20 <= bpm <= 300:
            self.song_setting("bpm", bpm)

    def arrange_redraw(self):
        self.arrange_job = None
        self.arranger.draw()

    def channels_changed(self):
        try:
            n = _clamp(int(self.ch_var.get()), 1, tracker.MAX_CHANNELS)
        except ValueError:
            return
        self.song_setting("channels", n, redraw=True)
        self.cur[1] = min(self.cur[1], n - 1)
        self.draw()

    def set_channels(self, n):
        """More channels for the riffs (the piano roll's chords)."""
        self.song["channels"] = n
        self.updating = True
        self.ch_var.set(str(n))
        self.updating = False

    def confirm_discard(self):
        return not self.dirty or messagebox.askyesno(
            "Music tracker", "The song has changes that are not saved. Lose them?", parent=self)

    def new_song(self):
        if self.confirm_discard():
            self.set_song(tracker.new_song(), None)

    def open_song(self):
        if not self.confirm_discard():
            return
        path = filedialog.askopenfilename(title="Open a song", filetypes=SONG_TYPES, parent=self)
        if not path:
            return
        try:
            self.set_song(tracker.load(path), path)
            self.app.settings.set("tracker.song", path)
        except (OSError, tracker.SongError) as e:
            self.app.error(str(e))

    def import_midi(self):
        """A MIDI file as a new song (tracker.import_midi): its notes on the rows, an instrument per program."""
        if not self.confirm_discard():
            return
        path = filedialog.askopenfilename(title="Import a MIDI file", parent=self,
                                          filetypes=[("MIDI files", "*.mid *.midi"), ("All files", "*.*")])
        if not path:
            return
        try:
            song, info = tracker.import_midi(path)
        except (OSError, IndexError, tracker.SongError, audioconv.ConvertError) as e:
            return self.app.error("%s: %s" % (os.path.basename(path), e or "not a MIDI file"))
        self.set_song(song, None)
        self.dirty = True               # (not saved yet: asked before it is closed)
        self.show_title()
        text = ("Imported %s: %d notes on %d channels, %d riffs, %d rows per beat. The instruments are waveforms "
                "of their General MIDI families: replace them with samples." % (
                    os.path.basename(path), info["notes"], info["channels"], info["patterns"], info["lpb"]))
        if info["dropped"]:
            text += " %d notes left out (16 channels used)." % info["dropped"]
        self.status_text(text, bool(info["dropped"]))

    def save_song(self):
        if not self.path:
            return self.save_song_as()
        try:
            tracker.save(self.song, self.path)
        except OSError as e:
            return self.app.error("Couldn't save the song: %s" % e)
        self.dirty = False
        self.app.settings.set("tracker.song", self.path)
        self.show_title()

    def save_song_as(self):
        path = filedialog.asksaveasfilename(title="Save the song", filetypes=SONG_TYPES, parent=self,
                                            defaultextension=tracker.EXTENSION,
                                            initialfile=(self.song["title"] or "song") + tracker.EXTENSION)
        if path:
            self.path = path
            self.save_song()

    def export_seq(self):
        path = filedialog.asksaveasfilename(title="Export SEQ + VAB", parent=self, defaultextension=".seq",
                                            filetypes=[("SEQ music", "*.seq")],
                                            initialfile=(self.song["title"] or "song") + ".seq")
        if not path:
            return
        base = os.path.splitext(path)[0]
        try:
            seq, info = tracker.compile_song(self.song)
            vh, vb = self.bank()
            for ext, data in ((".seq", seq), (".vh", vh), (".vb", vb)):
                with open(base + ext, "wb") as f:
                    f.write(data)
        except (OSError, tracker.SongError, audioconv.ConvertError) as e:
            return self.app.error(str(e))
        self.status_text("Exported %s.seq/.vh/.vb: %d:%02d, %d notes, samples %d KB." % (
            os.path.basename(base), info["length_ms"] // 60000, info["length_ms"] // 1000 % 60, info["notes"],
            len(vb) // 1024))

    def export_midi(self):
        path = filedialog.asksaveasfilename(title="Export MIDI", parent=self, defaultextension=".mid",
                                            filetypes=[("MIDI files", "*.mid")],
                                            initialfile=(self.song["title"] or "song") + ".mid")
        if not path:
            return
        try:
            seq, _ = tracker.compile_song(self.song, midi=True)
            os.makedirs(self.work, exist_ok=True)
            tmp = os.path.join(self.work, "export.seq")
            with open(tmp, "wb") as f:
                f.write(seq)
            audioconv.seq_to_midi(tmp, path)
        except (OSError, tracker.SongError, audioconv.ConvertError) as e:
            return self.app.error(str(e))
        self.status_text("Exported %s (the instruments as General MIDI programs %s)." % (
            os.path.basename(path), ", ".join(str(i) for i in range(len(self.song["instruments"])))))

    def bank(self, song=None):
        """VH and VB of the instruments (made again only when they or their sample files change)."""
        song = song or self.song
        key = repr(song["instruments"]) + repr([os.path.getmtime(i["path"]) for i in song["instruments"]
                                                if i["source"] != "wave" and os.path.isfile(i["path"])])
        if self.bank_cache and self.bank_cache[0] == key:
            return self.bank_cache[1]
        vh_vb = tracker.build_bank(song, self.cache,
                                   movieconv.find_ffmpeg(self.app.settings.field("movie.ffmpeg", "")))
        self.bank_cache = (key, vh_vb)
        return vh_vb

    # ---------------------------------------------------------------- riffs and arrangement

    def refresh_lists(self):
        pats = self.song["patterns"]
        self.pattern = _clamp(self.pattern, 0, len(pats) - 1)
        tree = self.riff_tree
        lpb = self.song["lpb"]
        for i, p in enumerate(pats):        # (changed in place: the selection kept)
            uses = sum(1 for c in self.song["clips"] if c["pattern"] == i)
            bars = p["rows"] / (lpb * 4.0)
            text = "%s  (%s bar%s%s)" % (p["name"], ("%g" % bars), "" if bars == 1 else "s",
                                          "" if uses else ", not used")
            if tree.exists("pat:%d" % i):
                tree.item("pat:%d" % i, text=text)
            else:
                tree.insert("song", "end", "pat:%d" % i, text=text)
        for item in tree.get_children("song")[len(pats):]:
            tree.delete(item)
        self.select_tree("pat:%d" % self.pattern if self.brush_id in (None, ("song", self.pattern)) else
                         None)
        self.riff_label.configure(text="Riff: %s" % pats[self.pattern]["name"])
        self.updating = True
        self.rows_var.set(str(pats[self.pattern]["rows"]))
        self.updating = False
        isel = self.ins_list.curselection()
        self.ins_list.delete(0, "end")
        for i, ins in enumerate(self.song["instruments"]):
            self.ins_list.insert("end", "%02X %s" % (i, ins["name"]))
        if isel and isel[0] < len(self.song["instruments"]):
            self.ins_list.selection_set(isel[0])
        self.arranger.draw()

    def select_tree(self, item):
        if item and self.riff_tree.exists(item) and self.riff_tree.selection() != (item,):
            self.tree_item = item           # (its select event: nothing more to do)
            self.riff_tree.selection_set(item)
            self.riff_tree.see(item)

    def riff_picked(self):
        """A riff picked on the left: put next on the tracks (a song's riff also edited)."""
        sel = self.riff_tree.selection()
        if not sel or sel[0] == getattr(self, "tree_item", None):
            return
        item = self.tree_item = sel[0]
        if item.startswith("pat:"):
            self.brush_id = ("song", int(item[4:]))
            self.edit_pattern(int(item[4:]))
            self.status_text("Click on a track to put \"%s\" there." % self.song["patterns"][int(item[4:])]["name"])
        elif item.startswith("lib:"):
            self.brush_id = ("lib", item[4:])
            self.status_text("\"%s\": Listen to hear it, click on a track to put it in the song." %
                             riffs.RIFFS[item[4:]][1])
        else:
            self.brush_id = None

    def brush(self):
        """The riff to put on a track: its index in the song (a library riff added to the song the first time,
        its instruments too), None if none is picked."""
        if not self.brush_id:
            self.status_text("Pick a riff on the left first (yours, or one of the Library).", True)
            return None
        kind, value = self.brush_id
        if kind == "song":
            return value if value < len(self.song["patterns"]) else None
        for i, p in enumerate(self.song["patterns"]):          # (added before: the same riff again)
            if p.get("source") == value:
                return i
        self.push_undo(song=True)
        try:
            pat = riffs.build(value, self.song)
        except tracker.SongError as e:
            self.status_text(str(e), True)
            return None
        self.song["patterns"].append(pat)
        self.changed(redraw=False, lists=True)
        self.select_tree("lib:" + value)
        return len(self.song["patterns"]) - 1

    def listen(self):
        """The riff picked played alone (a library riff: with its instruments, the song not changed)."""
        sel = self.riff_tree.selection()
        if sel and sel[0].startswith("lib:"):
            temp = copy.deepcopy(self.song)
            try:
                temp["patterns"].append(riffs.build(sel[0][4:], temp))
            except tracker.SongError as e:
                return self.status_text(str(e), True)
            self.play("listen", 0, song=temp, index=len(temp["patterns"]) - 1)
        elif sel and sel[0].startswith("pat:"):
            self.play("riff", 0, index=int(sel[0][4:]))

    def edit_pattern(self, index):
        if 0 <= index < len(self.song["patterns"]):
            changed = index != self.pattern
            self.pattern = index
            self.anchor = None
            self.cur[0] = min(self.cur[0], self.song["patterns"][index]["rows"] - 1)
            if changed:
                self.roll.sel = None
            self.refresh_lists()
            self.draw()
            if changed or not self.roll.grid_c.find_all():
                self.roll.show_notes()
            else:
                self.roll.draw()

    def new_pattern(self):
        """A new empty riff of one bar, picked to be put on the tracks."""
        self.push_undo(song=True)
        n = len(self.song["patterns"]) + 1
        self.song["patterns"].append(tracker.new_pattern(self.song["lpb"] * 4, "Riff %d" % n))
        self.brush_id = ("song", n - 1)
        self.changed(lists=True)
        self.edit_pattern(n - 1)
        self.status_text("New riff: click on a track to put it there, and draw its notes below.")

    def rename_pattern(self):
        pat = self.pat()
        name = simpledialog.askstring("Riff", "Name of the riff:", initialvalue=pat["name"], parent=self)
        if name and name.strip():
            self.push_undo(song=True)
            pat["name"] = name.strip()[:32]
            pat.pop("source", None)
            self.changed(lists=True)

    def clone_pattern(self):
        self.push_undo(song=True)
        pat = tracker.clone_pattern(self.pat(), self.pat()["name"] + " copy")
        self.song["patterns"].append(pat)
        self.brush_id = ("song", len(self.song["patterns"]) - 1)
        self.changed(lists=True)
        self.edit_pattern(len(self.song["patterns"]) - 1)

    def delete_pattern(self):
        pats = self.song["patterns"]
        if len(pats) == 1:
            return self.app.error("A song has one riff at least.")
        used = sum(1 for c in self.song["clips"] if c["pattern"] == self.pattern)
        if not messagebox.askyesno("Music tracker", "Delete the riff \"%s\"%s?" % (
                self.pat()["name"], " (it is %d times in the song)" % used if used else ""), parent=self):
            return
        self.push_undo(song=True)
        gone = self.pattern
        del pats[gone]
        self.song["clips"] = [c for c in self.song["clips"] if c["pattern"] != gone]
        for c in self.song["clips"]:
            if c["pattern"] > gone:
                c["pattern"] -= 1
        self.brush_id = None
        self.arranger.sel = None
        self.pattern = min(gone, len(pats) - 1)
        self.changed(lists=True)
        self.edit_pattern(self.pattern)

    def rows_changed(self):
        if getattr(self, "updating", False):
            return
        try:
            rows = _clamp(int(self.rows_var.get()), 1, tracker.MAX_ROWS)
        except ValueError:
            return
        pat = self.song["patterns"][self.pattern]
        if rows == pat["rows"]:
            return
        self.push_undo()
        pat["rows"] = rows
        pat["cells"] = {k: v for k, v in pat["cells"].items() if k[0] < rows}
        self.cur[0] = min(self.cur[0], rows - 1)
        self.changed(lists=True)

    def add_track(self):
        tracks = self.song["tracks"]
        if len(tracks) >= tracker.MAX_TRACKS:
            return self.app.error("%d tracks at most." % tracker.MAX_TRACKS)
        self.push_undo(song=True)
        tracks.append(dict(name="Track %d" % (len(tracks) + 1)))
        self.changed(redraw=False)
        self.arranger.draw()

    def remove_track(self, t):
        tracks = self.song["tracks"]
        if len(tracks) <= 1:
            return
        used = sum(1 for c in self.song["clips"] if c["track"] == t)
        if used and not messagebox.askyesno("Music tracker", "Remove the track \"%s\" and its %d riff block%s?" % (
                tracks[t]["name"], used, "s" if used > 1 else ""), parent=self):
            return
        self.push_undo(song=True)
        del tracks[t]
        self.song["clips"] = [c for c in self.song["clips"] if c["track"] != t]
        for c in self.song["clips"]:
            if c["track"] > t:
                c["track"] -= 1
        self.muted_tracks = {m - (m > t) for m in self.muted_tracks if m != t}
        self.arranger.sel = None
        self.changed(redraw=False)
        self.arranger.draw()

    def toggle_track_mute(self, t):
        self.muted_tracks ^= {t}
        self.send_mutes()
        self.arranger.draw()

    def set_loop(self, row):
        self.push_undo(song=True)
        self.song["loop"] = row
        self.changed(redraw=False)
        self.arranger.draw()

    def note_rows(self):
        """Rows of a new note in the piano roll (its length chosen)."""
        beats = NOTE_LENGTHS[max(0, self.length_choice.current())][1]
        return max(1, int(round(beats * self.song["lpb"])))

    # ---------------------------------------------------------------- instruments

    def ins_index(self):
        sel = self.ins_list.curselection()
        return sel[0] if sel else None

    def instrument_selected(self):
        i = self.ins_index()
        self.ins_loading = True
        try:
            if i is None:
                for k, var in self.ins_vars.items():
                    var.set(False if isinstance(var, tk.BooleanVar) else "")
                return
            ins = self.song["instruments"][i]
            for k, var in self.ins_vars.items():
                if k == "envelope":
                    var.set(audioconv.ENVELOPES[_clamp(int(ins[k]), 0, len(audioconv.ENVELOPES) - 1)][0])
                elif isinstance(var, tk.BooleanVar):
                    var.set(bool(ins[k]))
                else:
                    var.set(str(ins[k]))
            wave = ins["source"] == "wave"
            for k in ("wave",):
                self.ins_widgets[k].configure(state="readonly" if wave else "disabled")
            for k in ("path", "root", "rate", "loop", "loop_start", "normalize"):
                self.ins_widgets[k].configure(state="disabled" if wave else "normal")
        finally:
            self.ins_loading = False

    def instrument_changed(self, key):
        i = self.ins_index()
        if i is None or self.ins_loading:
            return
        ins = self.song["instruments"][i]
        var = self.ins_vars[key]
        try:
            if key == "envelope":
                value = [e[0] for e in audioconv.ENVELOPES].index(var.get())
            elif key in ("loop", "reverb", "normalize"):
                value = bool(var.get())
            elif key == "loop_start":
                value = max(0.0, float(var.get()))
            elif key in ("root", "cents", "vol", "pan", "rate", "bend"):
                lo, hi = {"root": (0, 127), "cents": (-100, 100), "vol": (0, 127), "pan": (0, 127),
                          "rate": (4000, 44100), "bend": (1, 12)}[key]
                value = _clamp(int(float(var.get())), lo, hi)
            else:
                value = var.get()
        except ValueError:
            return
        if ins.get(key) == value:
            return
        self.push_undo(song=True)
        ins[key] = value
        self.changed(redraw=False, lists=key == "name")

    def add_wave(self, wave):
        if len(self.song["instruments"]) >= tracker.MAX_INSTRUMENTS:
            return
        self.push_undo(song=True)
        self.song["instruments"].append(tracker.new_instrument(wave.capitalize(), wave))
        self.changed(redraw=False, lists=True)
        self.select_instrument(len(self.song["instruments"]) - 1)

    def add_sample(self):
        paths = filedialog.askopenfilenames(title="Samples", filetypes=SAMPLE_TYPES, parent=self)
        if not paths:
            return
        self.push_undo(song=True)
        for path in paths:
            if len(self.song["instruments"]) >= tracker.MAX_INSTRUMENTS:
                break
            ins = tracker.new_instrument(os.path.splitext(os.path.basename(path))[0][:20])
            ins.update(source="file", path=path, envelope=4)
            self.song["instruments"].append(ins)
        self.changed(redraw=False, lists=True)
        self.select_instrument(len(self.song["instruments"]) - 1)

    def browse_sample(self):
        i = self.ins_index()
        if i is None or self.song["instruments"][i]["source"] != "file":
            return
        path = filedialog.askopenfilename(title="Sample", filetypes=SAMPLE_TYPES, parent=self)
        if path:
            self.ins_vars["path"].set(path)
            self.instrument_changed("path")

    def select_instrument(self, i):
        self.ins_list.selection_clear(0, "end")
        if 0 <= i < len(self.song["instruments"]):
            self.ins_list.selection_set(i)
            self.ins_list.see(i)
        self.instrument_selected()

    def remove_instrument(self):
        i = self.ins_index()
        if i is None:
            return
        uses = sum(1 for p in self.song["patterns"] for c in p["cells"].values() if c[1] == i)
        if not messagebox.askyesno("Music tracker", "Remove instrument %02X (%s)?%s The ones after it are numbered "
                                   "one less (the notes keep their sound)." % (
                                       i, self.song["instruments"][i]["name"],
                                       " Its %d notes are left without an instrument." % uses if uses else ""),
                                   parent=self):
            return
        self.push_undo(song=True)
        del self.song["instruments"][i]
        for p in self.song["patterns"]:
            for cell in p["cells"].values():
                if cell[1] is not None:
                    cell[1] = None if cell[1] == i else (cell[1] - 1 if cell[1] > i else cell[1])
        self.changed(lists=True)
        self.select_instrument(min(i, len(self.song["instruments"]) - 1))

    def current_instrument(self):
        i = self.ins_index()
        return 0 if i is None else i

    # ---------------------------------------------------------------- undo

    def push_undo(self, song=False):
        if song:
            entry = ("song", copy.deepcopy(self.song), self.pattern)
        else:
            entry = ("pattern", self.pattern, tracker.clone_pattern(self.song["patterns"][self.pattern]))
        self.undo_stack.append(entry)
        del self.undo_stack[:-UNDO_MAX]
        self.redo_stack.clear()

    def _swap(self, entry):
        if entry[0] == "song":
            current = ("song", copy.deepcopy(self.song), self.pattern)
            self.song = entry[1]
            self.pattern = _clamp(entry[2], 0, len(self.song["patterns"]) - 1)
            self.updating = True
            self.bpm_var.set(str(self.song["bpm"]))
            self.lpb_choice.set(str(self.song["lpb"]))
            self.ch_var.set(str(self.song["channels"]))
            self.title_var.set(self.song["title"])
            self.updating = False
            self.arranger.sel = None
            self.instrument_selected()
        else:
            index = entry[1]
            if index >= len(self.song["patterns"]):
                return None
            current = ("pattern", index, tracker.clone_pattern(self.song["patterns"][index]))
            self.song["patterns"][index] = entry[2]
            self.pattern = index
        return current

    def undo(self):
        if self.undo_stack:
            back = self._swap(self.undo_stack.pop())
            if back:
                self.redo_stack.append(back)
            self.changed(lists=True)

    def redo(self):
        if self.redo_stack:
            back = self._swap(self.redo_stack.pop())
            if back:
                self.undo_stack.append(back)
            self.changed(lists=True)

    # ---------------------------------------------------------------- pattern editor: drawing

    def pat(self):
        return self.song["patterns"][self.pattern]

    def visible(self):
        h, w = self.canvas.winfo_height(), self.canvas.winfo_width()
        rows = max(1, (h - 4) // self.lh)
        chans = max(1, (w - ROWNUM_CHARS * self.cw - 4) // (CH_CHARS * self.cw))
        return rows, chans

    def selection(self):
        if self.anchor is None:
            return None
        r0, c0 = self.anchor
        r1, c1 = self.cur[0], self.cur[1]
        return min(r0, r1), min(c0, c1), max(r0, r1), max(c0, c1)

    def keep_visible(self, row=None):
        rows, chans = self.visible()
        n = self.pat()["rows"]
        row = self.cur[0] if row is None else row
        if row < self.top + 2:
            self.top = row - 2
        elif row >= self.top + rows - 2:
            self.top = row - rows + 3
        self.top = _clamp(self.top, 0, max(0, n - rows))
        ch = self.cur[1]
        if ch < self.left:
            self.left = ch
        elif ch >= self.left + chans:
            self.left = ch - chans + 1
        self.left = _clamp(self.left, 0, max(0, self.song["channels"] - chans))

    def draw(self):
        c = self.canvas
        if not c.winfo_ismapped() and c.winfo_width() < 10:
            return
        c.delete("all")
        self.header.delete("all")
        pat, lpb = self.pat(), self.song["lpb"]
        rows, chans = self.visible()
        n, nch = pat["rows"], self.song["channels"]
        self.top = _clamp(self.top, 0, max(0, n - rows))
        self.left = _clamp(self.left, 0, max(0, nch - chans))
        cw, lh = self.cw, self.lh
        x0 = ROWNUM_CHARS * cw + 4
        width = x0 + min(chans, nch - self.left) * CH_CHARS * cw
        sel = self.selection()
        try:
            focus = self.focus_get() is c
        except (KeyError, tk.TclError):     # (a combobox's list open)
            focus = False
        play = self.play_pattern_row()
        font = self.font
        # Header
        for i in range(min(chans, nch - self.left)):
            ch = self.left + i
            x = x0 + i * CH_CHARS * cw
            if ch in self.muted:
                self.header.create_rectangle(x, 0, x + (CH_CHARS - 1) * cw, lh + 4, fill=COLORS["muted"],
                                             outline="")
            self.header.create_text(x + 2, 3, anchor="nw", font=font, fill=COLORS["header_text"],
                                    text="Ch %d%s" % (ch + 1, "  muted" if ch in self.muted else ""))
        self.header.create_text(4, 3, anchor="nw", font=font, fill=COLORS["rownum"], text="R%02d" % self.pattern)
        # Rows
        for i in range(min(rows, n - self.top)):
            r = self.top + i
            y = 2 + i * lh
            bg = None
            if r == play:
                bg = COLORS["play"]
            elif r == self.cur[0]:
                bg = COLORS["cursor_row"]
            elif r % (lpb * 4) == 0:
                bg = COLORS["bar"]
            elif r % lpb == 0:
                bg = COLORS["beat"]
            if bg:
                c.create_rectangle(0, y, width, y + lh, fill=bg, outline="")
            if sel and sel[0] <= r <= sel[2]:
                xs = x0 + (max(sel[1], self.left) - self.left) * CH_CHARS * cw
                xe = x0 + (min(sel[3], self.left + chans - 1) - self.left + 1) * CH_CHARS * cw - cw
                if xe > xs:
                    c.create_rectangle(xs, y, xe, y + lh, fill=COLORS["select"], outline="")
            c.create_text(4, y + 1, anchor="nw", font=font, text="%3d" % r,
                          fill=COLORS["rownum_bar"] if r % lpb == 0 else COLORS["rownum"])
            for j in range(min(chans, nch - self.left)):
                ch = self.left + j
                x = x0 + j * CH_CHARS * cw
                cell = pat["cells"].get((r, ch)) or [None] * 5
                note, ins, vol, fx, val = cell
                parts = ((0, tracker.note_text(note), COLORS["text"] if note is not None else COLORS["empty"]),
                         (4, ".." if ins is None else "%02X" % ins, COLORS["ins"] if ins is not None else
                          COLORS["empty"]),
                         (7, ".." if vol is None else "%02X" % vol, COLORS["vol"] if vol is not None else
                          COLORS["empty"]),
                         (10, "." if fx is None else fx, COLORS["fx"] if fx is not None else COLORS["empty"]),
                         (11, ".." if val is None else "%02X" % val, COLORS["fx"] if val is not None else
                          COLORS["empty"]))
                if ch in self.muted:
                    parts = tuple((o, t, COLORS["empty"]) for o, t, _ in parts)
                for off, text, color in parts:
                    c.create_text(x + off * cw, y + 1, anchor="nw", font=font, text=text, fill=color)
                if j:
                    c.create_line(x - cw // 2, y, x - cw // 2, y + lh, fill=COLORS["sep"])
            if r == self.cur[0] and self.left <= self.cur[1] < self.left + chans:
                _, _, off, w = COLS[self.cur[2]]
                x = x0 + (self.cur[1] - self.left) * CH_CHARS * cw + off * cw
                c.create_rectangle(x - 1, y, x + w * cw + 1, y + lh, outline=COLORS["cursor"] if focus else
                                   COLORS["rownum"], width=2)
        # Scrollbars
        self.vbar.set(self.top / max(1, n), min(1.0, (self.top + rows) / max(1, n)))
        self.hbar.set(self.left / max(1, nch), min(1.0, (self.left + chans) / max(1, nch)))

    def yview(self, *args):
        rows, _ = self.visible()
        n = self.pat()["rows"]
        if args[0] == "moveto":
            self.top = int(float(args[1]) * n)
        elif args[0] == "scroll":
            self.top += int(args[1]) * (rows if args[2] == "pages" else 1)
        self.draw()

    def xview(self, *args):
        _, chans = self.visible()
        n = self.song["channels"]
        if args[0] == "moveto":
            self.left = int(float(args[1]) * n)
        elif args[0] == "scroll":
            self.left += int(args[1]) * (chans if args[2] == "pages" else 1)
        self.draw()

    def scroll(self, rows):
        self.top += rows
        self.draw()

    # ---------------------------------------------------------------- pattern editor: mouse

    def at(self, event):
        """(row, channel, column) under the mouse."""
        x0 = ROWNUM_CHARS * self.cw + 4
        row = self.top + max(0, (event.y - 2) // self.lh)
        rel = event.x - x0
        ch = self.left + max(0, rel // (CH_CHARS * self.cw))
        char = (rel % (CH_CHARS * self.cw)) // self.cw if rel >= 0 else 0
        col = 0
        for i, (_, _, off, w) in enumerate(COLS):
            if char >= off:
                col = i
        return (_clamp(row, 0, self.pat()["rows"] - 1), _clamp(ch, 0, self.song["channels"] - 1), col)

    def click(self, event):
        self.canvas.focus_set()
        r, ch, col = self.at(event)
        if event.state & 0x1:
            if self.anchor is None:
                self.anchor = (self.cur[0], self.cur[1])
        else:
            self.anchor = None
        self.cur = [r, ch, col]
        self.draw()

    def drag(self, event):
        r, ch, col = self.at(event)
        if self.anchor is None and (r, ch) != (self.cur[0], self.cur[1]):
            self.anchor = (self.cur[0], self.cur[1])
        self.cur = [r, ch, col]
        self.keep_visible()
        self.draw()

    def header_click(self, event, solo):
        x0 = ROWNUM_CHARS * self.cw + 4
        if event.x < x0:
            return
        ch = self.left + (event.x - x0) // (CH_CHARS * self.cw)
        if ch >= self.song["channels"]:
            return
        if solo:
            others = set(range(tracker.MAX_CHANNELS)) - {ch}
            self.muted = set() if self.muted == others else others
        else:
            self.muted ^= {ch}
        self.send_mutes()
        self.draw()

    def send_mutes(self):
        """Channels muted: the tracks' (the song playing) or the note grid's (a riff)."""
        if self.play_kind == "song":
            try:
                bases = tracker.track_channels(self.song)[0]
            except tracker.SongError:
                return
            chans = {b + i for t, (b, w) in enumerate(bases) if t in self.muted_tracks for i in range(w)}
        else:
            chans = self.muted
        self.fx_sent = (self.song["reverb"], self.song["depth"])
        self.send("fx %d %d 0 0 100 0 %d" % (self.song["reverb"], self.song["depth"], sum(1 << c for c in chans)))

    # ---------------------------------------------------------------- pattern editor: keys

    def move(self, rows=0, cols=0, select=False):
        if select and self.anchor is None:
            self.anchor = (self.cur[0], self.cur[1])
        elif not select:
            self.anchor = None
        n = self.pat()["rows"]
        self.cur[0] = _clamp(self.cur[0] + rows, 0, n - 1)
        if cols:
            if select:      # (a selection is whole cells: by channels)
                self.cur[1] = _clamp(self.cur[1] + cols, 0, self.song["channels"] - 1)
            else:
                pos = self.cur[1] * len(COLS) + self.cur[2] + cols
                pos = _clamp(pos, 0, self.song["channels"] * len(COLS) - 1)
                self.cur[1], self.cur[2] = divmod(pos, len(COLS))
        self.keep_visible()
        self.draw()

    def move_channel(self, step):
        self.anchor = None
        self.cur[1] = (self.cur[1] + step) % self.song["channels"]
        self.cur[2] = 0
        self.keep_visible()
        self.draw()

    def advance(self):
        step = self.step.get()
        if step:
            self.cur[0] = (self.cur[0] + step) % self.pat()["rows"]
        self.keep_visible()

    def set_field(self, field, value, row=None, ch=None):
        row = self.cur[0] if row is None else row
        ch = self.cur[1] if ch is None else ch
        cells = self.pat()["cells"]
        cell = list(cells.get((row, ch)) or [None] * 5)
        cell[FIELD[field]] = value
        if all(v is None for v in cell):
            cells.pop((row, ch), None)
        else:
            cells[(row, ch)] = cell

    def key(self, event):
        ks, ctrl, shift = event.keysym, bool(event.state & 0x4), bool(event.state & 0x1)
        low = ks.lower() if len(ks) == 1 else ks
        if ctrl:
            if low == "z":
                self.undo()
            elif low == "y":
                self.redo()
            elif low == "c":
                self.copy()
            elif low == "x":
                self.copy()
                self.clear_block()
            elif low == "v":
                self.paste()
            elif low == "a":
                self.anchor = (0, 0)
                self.cur[0], self.cur[1] = self.pat()["rows"] - 1, self.song["channels"] - 1
                self.draw()
            elif ks in ("Up", "Down"):
                self.transpose((12 if shift else 1) * (1 if ks == "Up" else -1))
            elif low == "s":
                self.save_song()
            return "break"
        moves = {"Up": (-1, 0), "Down": (1, 0), "Left": (0, -1), "Right": (0, 1)}
        if ks in moves:
            self.move(*moves[ks], select=shift)
            return "break"
        if ks in ("Prior", "Next"):
            self.move(-16 if ks == "Prior" else 16, select=shift)
            return "break"
        if ks in ("Home", "End"):
            self.move(-100000 if ks == "Home" else 100000, select=shift)
            return "break"
        if ks in ("KP_Add", "KP_Subtract"):
            self.octave.set(_clamp(self.octave.get() + (1 if ks == "KP_Add" else -1), 0, 8))
            return "break"
        if ks == "space":
            self.space(row=self.cur[0])
            return "break"
        if ks == "Escape":
            self.anchor = None
            self.draw()
            return "break"
        if ks in ("Delete", "BackSpace"):
            if self.selection():
                self.clear_block()
            else:
                self.push_undo()
                field = COLS[self.cur[2]][0]
                if field == "note":
                    for f in ("note", "ins", "vol"):
                        self.set_field(f, None)
                elif field == "fx":
                    self.set_field("fx", None)
                    self.set_field("val", None)
                else:
                    self.set_field(field, None)
                self.advance()
                self.changed()
            return "break"
        field, digit, _, _ = COLS[self.cur[2]]
        if field == "note":
            if low in NOTE_OFF_KEYS:
                self.push_undo()
                self.set_field("note", tracker.NOTE_OFF)
                self.set_field("ins", None)
                self.advance()
                self.changed()
            elif low in PIANO:
                note = (self.octave.get() + 1) * 12 + PIANO[low]
                if note <= 127:
                    self.push_undo()
                    self.set_field("note", note)
                    self.set_field("ins", self.current_instrument())
                    self.preview(note)
                    self.advance()
                    self.changed()
            return "break"
        if field == "fx":
            if ks.upper() in tracker.EFFECTS:
                self.push_undo()
                self.set_field("fx", ks.upper())
                cell = self.pat()["cells"].get((self.cur[0], self.cur[1]))
                if cell[4] is None:
                    self.set_field("val", 0x40 if ks.upper() in "PB" else (0x7F if ks.upper() in "VE" else
                                                                           self.song["bpm"] & 0xFF))
                self.cur[2] += 1
                self.changed()
            return "break"
        if len(ks) == 1 and ks.lower() in "0123456789abcdef":
            d = int(ks, 16)
            self.push_undo()
            cell = self.pat()["cells"].get((self.cur[0], self.cur[1])) or [None] * 5
            old = cell[FIELD[field]] or 0
            value = (d << 4) | (old & 15) if digit == 0 else (old & 0xF0) | d
            if field in ("ins", "vol"):
                value = min(value, 127)
            self.set_field(field, value)
            if digit == 0:
                self.cur[2] += 1
            else:
                self.cur[2] -= 1
                self.advance()
            self.changed()
            return "break"
        return None

    # ---------------------------------------------------------------- blocks

    def block(self):
        sel = self.selection()
        if sel:
            return sel
        return self.cur[0], self.cur[1], self.cur[0], self.cur[1]

    def copy(self):
        r0, c0, r1, c1 = self.block()
        cells = self.pat()["cells"]
        self.clipboard = (r1 - r0 + 1, c1 - c0 + 1,
                          {(r - r0, c - c0): list(cells[(r, c)]) for (r, c) in cells
                           if r0 <= r <= r1 and c0 <= c <= c1})
        self.status_text("Copied %d rows x %d channels." % (r1 - r0 + 1, c1 - c0 + 1))

    def clear_block(self):
        r0, c0, r1, c1 = self.block()
        self.push_undo()
        cells = self.pat()["cells"]
        for key in [k for k in cells if r0 <= k[0] <= r1 and c0 <= k[1] <= c1]:
            del cells[key]
        self.changed()

    def paste(self):
        if not self.clipboard:
            return
        rows, chans, data = self.clipboard
        self.push_undo()
        cells, n, nch = self.pat()["cells"], self.pat()["rows"], self.song["channels"]
        r0, c0 = self.cur[0], self.cur[1]
        for dr in range(rows):
            for dc in range(chans):
                r, c = r0 + dr, c0 + dc
                if r < n and c < nch:
                    if (dr, dc) in data:
                        cells[(r, c)] = list(data[(dr, dc)])
                    else:
                        cells.pop((r, c), None)
        self.anchor = None
        self.changed()

    def transpose(self, semis):
        r0, c0, r1, c1 = self.block()
        self.push_undo()
        for (r, c), cell in self.pat()["cells"].items():
            if r0 <= r <= r1 and c0 <= c <= c1 and cell[0] is not None and cell[0] >= 0:
                cell[0] = _clamp(cell[0] + semis, 0, 127)
        self.changed()

    # ---------------------------------------------------------------- console

    def args(self):
        return None     # (started by Play: the files are made then)

    def send(self, line):
        return self.live and self.app.runner.send(line)

    def preview(self, note, ins=None):
        """The note put or typed heard on the console (the tester's program index is the instrument); the tester
        started for it the first time."""
        if not self.running and not self.auto_started:
            self.auto_started = True
            self.connect()
        if not self.live or self.restart:
            return
        self.send("program %d" % (self.current_instrument() if ins is None else ins))
        self.send("play %d" % note)
        if self.preview_job:
            self.after_cancel(self.preview_job)
        self.preview_job = self.after(PREVIEW_MS, self.preview_end)

    def preview_end(self):
        self.preview_job = None
        self.send("release")

    def write_work(self, name, data):
        os.makedirs(self.work, exist_ok=True)
        path = os.path.join(self.work, name)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def connect(self):
        """The sound tester started with the song's instruments (the notes put heard), nothing played."""
        try:
            vh, vb = self.bank()
            seq, _ = tracker.compile_riff(self.song, self.pattern)
        except (OSError, tracker.SongError, audioconv.ConvertError) as e:
            return self.status_text(str(e), True)
        self.start_tester(vh, vb, seq)

    def space(self, song=False, row=0):
        """Space: the music stopped if it plays, else the song (arranger) or the riff (from a row) played."""
        if self.seq_state == 2 and self.live and not (self.pos and self.pos[2]):
            self.stop_music()
        else:
            self.play("song" if song else "riff", 0 if song else row)

    def compile_kind(self, kind, song=None, index=None):
        song = song or self.song
        if kind == "song":
            return tracker.compile_song(song)
        return tracker.compile_riff(song, self.pattern if index is None else index)

    def play(self, kind="song", row=0, song=None, index=None):
        """The song or a riff (looped) played from a row ("listen": a library riff, in a copy of the song)."""
        song = song or self.song
        if kind == "riff" and index is None:
            index = self.pattern
        try:
            vh, vb = self.bank(song)
            seq, info = self.compile_kind(kind, song, index)
        except (OSError, tracker.SongError, audioconv.ConvertError) as e:
            return self.status_text(str(e), True)
        start = tracker.row_ms(info["rows"], 0, row) or 0
        self.play_kind = ("riff", index) if kind == "riff" else kind
        self.timing = info["rows"]
        repeat = 0 if kind == "song" and song["loop"] is None else 1
        if self.live:
            if self.bank_sent != (vh, vb):      # (new instruments: sent with the SEQ while the tester runs)
                self.send_bank(vh, vb, seq)
            elif seq != self.seq_sent:
                self.send_seq(seq)
            self.send_mutes()
            self.send("repeat %d" % repeat)
            self.send("seek %d" % start)
            self.send("pause 0")
            return
        self.pending = (start, repeat)
        if self.running and self.bank_sent == (vh, vb):     # (starting with these instruments: the SEQ sent after)
            self.pending_seq = seq
            return
        if self.running:            # New instruments: the tester started again with them
            self.restart = (vh, vb, seq)
            self.status_text("Sending the new instruments...")
            self.app.runner.stop()
            return
        self.start_tester(vh, vb, seq)

    def start_tester(self, vh, vb, seq):
        try:
            files = [self.write_work("song.vh", vh), self.write_work("song.vb", vb), self.write_work("song.seq", seq)]
        except OSError as e:
            return self.status_text("Can't write the files: %s" % e, True)
        self.bank_sent, self.seq_sent = (vh, vb), seq
        self.pending_seq = None
        self.running = True
        self.live = False
        self.status_text("Starting the console's sound tester...")
        args = []
        for f in files:
            args += ["-snd", f]
        self.app.run(args + ["-sndlive"], "Music tracker", long_running=True, stdin=True, cwd=self.work,
                     on_line=lambda line: self.after(0, self.line, line),
                     log_filter=lambda line: not line.startswith(PROTOCOL),
                     on_done=lambda code, out: self.after(0, self.stopped, code))
        if not self.app.runner.busy:        # (not started: another task kept)
            self.running = False

    def send_seq(self, seq):
        self.seq_files = (self.seq_files + 1) % 8      # (a file per change: ps1transfer reads it later)
        try:
            path = self.write_work("live%d.seq" % self.seq_files, seq)
        except OSError as e:
            return self.status_text("Can't write the SEQ: %s" % e, True)
        self.send("seqload %s" % path)
        self.seq_sent = seq

    def send_bank(self, vh, vb, seq):
        """New instruments (and the SEQ) while the tester runs: loaded by it, the music on from the same time."""
        self.bank_files = (self.bank_files + 1) % 4
        base = os.path.join(self.work, "bank%d" % self.bank_files)
        try:
            for ext, data in ((".vh", vh), (".vb", vb), (".seq", seq)):
                self.write_work(os.path.basename(base) + ext, data)
        except OSError as e:
            return self.status_text("Can't write the instruments: %s" % e, True)
        self.send("bankload %s" % base)
        self.bank_sent, self.seq_sent = (vh, vb), seq

    def live_update(self):
        """The song changed while the tester runs: its instruments and the music played sent again, played from
        the same time."""
        self.live_job = None
        if not self.live:
            return
        try:
            bank = self.bank()
        except (OSError, tracker.SongError, audioconv.ConvertError) as e:
            bank = self.bank_sent           # (the instruments as they were, the error shown)
            self.status_text(str(e), True)
        kind = self.play_kind
        seq = info = None
        if kind == "song" or (isinstance(kind, tuple) and kind[1] < len(self.song["patterns"])):
            try:
                seq, info = self.compile_kind("song" if kind == "song" else "riff", None,
                                              None if kind == "song" else kind[1])
            except tracker.SongError as e:
                self.status_text(str(e), True)
        if seq is None:                     # (nothing of the song playing: its instruments only)
            if bank != self.bank_sent and self.seq_sent:
                self.send_bank(bank[0], bank[1], self.seq_sent)
            return
        # The row playing kept (a tempo or length change moves the rows' times): its time in the new SEQ
        seek = None
        if self.pos and self.timing and self.seq_state == 2:
            ms, t, paused = self.pos
            now = ms if paused else ms + (time.time() - t) * 1000
            at = tracker.row_at(self.timing, now)
            old = tracker.row_ms(self.timing, *at) if at else None
            new = tracker.row_ms(info["rows"], *at) if at else None
            if old is not None and new is not None and abs(new - old) > 5:     # (the rows' times changed)
                seek = new + int(now - old)
        self.timing = info["rows"]
        if bank != self.bank_sent:
            self.send_bank(bank[0], bank[1], seq)
        elif seq != self.seq_sent:
            self.send_seq(seq)
        else:
            return
        if kind == "song":
            self.send_mutes()               # (the tracks' channels may have moved)
        if seek is not None:
            self.send("seek %d" % seek)

    def line(self, line):
        if not line.startswith("SND") and "SND" in line:     # (after the PS1's text, without its line end)
            line = line[line.index("SND"):]
        fields = line.split()
        if line.startswith("SNDLIVE ready"):
            self.live = True
            self.show_title()
            self.send_mutes()           # (and the reverb)
            if self.pending_seq is not None and self.pending_seq != self.seq_sent:
                self.send_seq(self.pending_seq)
            self.pending_seq = None
            if self.pending:
                start, repeat = self.pending
                self.pending = None
                self.send_mutes()
                self.send("repeat %d" % repeat)
                self.send("seek %d" % start)
        elif line.startswith("SNDSTATE") and len(fields) >= 7:
            try:
                self.seq_state = int(fields[6])
            except ValueError:
                return
            if self.seq_state != 2:
                self.pos = None
                if self.play_row is not None:
                    self.play_row = None
                    self.draw_play()
            self.pause_button.configure(state="normal" if self.seq_state == 2 else "disabled")
        elif line.startswith("SNDPOS") and len(fields) >= 4:
            try:
                ms, paused = int(fields[1]), int(fields[3])
            except ValueError:
                return
            self.pos = (ms, time.time(), paused) if self.seq_state == 2 else None
            self.pause_button.configure(text="Resume" if paused else "Pause")
        elif line.startswith("SNDBANK"):
            self.status_text("Instruments sent to the console.")
        elif line.startswith("SNDERR"):
            self.status_text(line[7:], True)
        elif line.startswith("SNDLIVE disconnected"):
            self.status_text("Console disconnected.", True)

    def stopped(self, code=0):
        if not self.live and not self.restart:
            self.status_text("The sound tester didn't start: see the log below (cartridge connected and chosen?)."
                             if code else "Disconnected.", bool(code))
        elif self.live:
            self.auto_started = False       # (the notes put start it again)
        elif not self.restart:
            self.status_text("Disconnected.")
        self.running = self.live = False
        self.seq_state = 0
        self.pos = self.play_row = None
        self.bank_sent = self.seq_sent = None
        self.pause_button.configure(state="disabled", text="Pause")
        self.draw_play()
        if self.restart:
            vh, vb, seq = self.restart
            self.restart = None
            self.after(300, lambda: self.start_tester(vh, vb, seq))

    def stop_music(self):
        self.send("seq 0")

    def toggle_pause(self):
        if self.pos:
            self.send("pause %d" % (0 if self.pos[2] else 1))

    def disconnect(self):
        if self.running:
            self.app.stop_task()

    def draw_grid(self):
        self.draw()

    def draw_play(self):
        """The row playing shown (arranger, piano roll, note grid)."""
        self.arranger.draw_play()
        if self.view.get() == "roll":
            self.roll.draw_play()
        else:
            self.draw()

    def song_play_row(self):
        """The song's row playing (the song played), None."""
        if self.play_kind == "song" and self.seq_state == 2 and self.play_row is not None:
            return self.play_row
        return None

    def play_pattern_row(self):
        """The edited riff's row playing: the riff played, or the song where a block of it plays (the first
        track with one), None."""
        if self.seq_state != 2 or self.play_row is None:
            return None
        if self.play_kind == ("riff", self.pattern):
            return self.play_row
        if self.play_kind == "song":
            ends = tracker.clip_ends(self.song)
            for c in sorted(self.song["clips"], key=lambda c: c["track"]):
                if c["pattern"] == self.pattern and c["start"] <= self.play_row < ends[id(c)]:
                    return (self.play_row - c["start"]) % self.pat()["rows"]
        return None

    def tick(self):
        """The row playing (the last SNDPOS and the time since), shown."""
        if self.pos and self.timing and self.seq_state == 2:
            ms, t, paused = self.pos
            now = ms if paused else ms + min(1000, (time.time() - t) * 1000)
            at = tracker.row_at(self.timing, now)
            row = at[1] if at else None
            if row != self.play_row:
                self.play_row = row
                if self.view.get() == "grid" and self.follow.get():
                    prow = self.play_pattern_row()
                    if prow is not None:
                        rows, _ = self.visible()
                        self.top = prow - rows // 2
                self.draw_play()
        self.after(30, self.tick)
