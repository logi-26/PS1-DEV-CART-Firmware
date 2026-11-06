"""
PS1 DEV CART GUI: the Convert tab of the Sound Tools page: common audio files and MIDI made into the PS1's formats
(audioconv.py), then played by the sound tester with one click.

- Audio -> VAG: a sample, its rate, trimmed, volume or normalized, looping
- Samples -> VAB: a sound bank from several samples, each a tone of a program (instrument) with its root note, key
  range, fine tune, volume, pan, envelope, reverb and loop (the VAB Editor tab tunes it further on the console)
- MIDI -> SEQ: a MIDI file as SEQ music for a VAB (its program numbers), with its loop and the GM drums' program
- Audio -> XA: streamed CD music, several songs in one file (one per channel)
And back (each tab's "From the PS1" part): VAG -> audio, VAB -> its samples and a list of its programs, SEQ -> MIDI,
XA (or an STR movie's sound) -> audio, one file per channel. The results open with the computer's own programs.

The sounds other than WAV are read with FFmpeg (the one of the Movie Tools page: the same setting).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import queue
import threading
import tkinter as tk
from tkinter import filedialog

import ttkbootstrap as tb

import audioconv
import movieconv
import strmovie
from widgets import FileField, FileList, ScrollFrame

AUDIO_TYPES = [("Sounds", "*.wav *.mp3 *.ogg *.flac *.aac *.m4a *.wma *.opus *.aif *.aiff"), ("All files", "*.*")]
MIDI_TYPES = [("MIDI files", "*.mid *.midi *.smf"), ("All files", "*.*")]
AUDIO_SAVE_TYPES = [("WAV", "*.wav"), ("MP3", "*.mp3"), ("OGG Vorbis", "*.ogg"), ("FLAC", "*.flac"),
                    ("All files", "*.*")]
SAMPLE_FORMATS = (".wav", ".flac", ".ogg", ".mp3")
NOTE_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
LOOP_MODES = [("markers", "The file's loop markers (loopStart/loopEnd, CC 111)"), ("none", "No loop"),
              ("whole", "Loop the whole music"), ("tick", "Loop from a tick to the end")]
SAMPLE_DEFAULTS = dict(program=0, root=60, cents=0, min=0, max=127, vol=100, pan=64, envelope=0, reverb=True,
                       loop=False, loop_start=0.0, rate=22050, normalize=True)
COLUMNS = (("file", "Sample", 180), ("program", "Program", 60), ("root", "Root", 50), ("keys", "Keys", 90),
           ("vol", "Vol", 40), ("pan", "Pan", 40), ("envelope", "Envelope", 150), ("loop", "Loop", 60),
           ("rate", "Rate", 60))


def note_name(n):
    return "%s%d" % (NOTE_NAMES[n % 12], n // 12 - 1)


def number(var, default, low=None, high=None, kind=int):
    try:
        v = kind(var.get())
    except (tk.TclError, ValueError):
        return default
    if low is not None:
        v = max(low, v)
    if high is not None:
        v = min(high, v)
    return v


class ConvertTab(tb.Frame):
    def __init__(self, master, page):
        super().__init__(master, padding=6)
        self.page = page
        self.app = page.app
        self.busy = False
        self.events = queue.Queue()     # (the threads' results, taken here: Tk is used from its own thread only)
        self.after(50, self.poll_events)

        row = tb.Frame(self)
        row.pack(fill="x", pady=(0, 6))
        tb.Label(row, text="FFmpeg:").pack(side="left")
        self.ffmpeg_label = tb.Label(row, text="", bootstyle="secondary")
        self.ffmpeg_label.pack(side="left", padx=6)
        self.get_ffmpeg = tb.Button(row, text="Get FFmpeg", bootstyle="info-outline", command=self.download_ffmpeg)
        self.get_ffmpeg.pack(side="right")
        tb.Button(row, text="Browse...", bootstyle="secondary-outline", command=self.browse_ffmpeg).pack(
            side="right", padx=6)
        self.show_ffmpeg()

        self.tabs = tb.Notebook(self)
        self.tabs.pack(fill="both", expand=True)
        # Each converter: to the PS1's format, then back (scrolled: taller than the page)
        for title, build, back in (("VAG ↔ Audio", self.build_vag, self.build_vag_back),
                                   ("VAB ↔ Samples", self.build_vab, self.build_vab_back),
                                   ("SEQ ↔ MIDI", self.build_seq, self.build_seq_back),
                                   ("XA ↔ Audio", self.build_xa, self.build_xa_back)):
            scroll = ScrollFrame(self.tabs)
            self.tabs.add(scroll, text=title)
            inner = tb.Frame(scroll.inner, padding=8)
            inner.pack(fill="both", expand=True)
            box = tb.Labelframe(inner, text="To the PS1", padding=8)
            box.pack(fill="x")
            build(box)
            box = tb.Labelframe(inner, text="From the PS1", padding=8)
            box.pack(fill="x", pady=(10, 0))
            back(box)

        bottom = tb.Frame(self)
        bottom.pack(fill="x", pady=(6, 0))
        self.progress = tb.Progressbar(bottom, length=220, maximum=100, bootstyle="info-striped")
        self.progress.pack(side="left")
        self.status = tb.Label(bottom, text="", bootstyle="secondary", wraplength=900, justify="left")
        self.status.pack(side="left", padx=10, fill="x", expand=True)

    def post(self, fn, *args):
        self.events.put((fn, args))

    def poll_events(self):
        try:
            while True:
                fn, args = self.events.get_nowait()
                fn(*args)
        except queue.Empty:
            pass
        self.after(50, self.poll_events)

    # ---------------------------------------------------------------- FFmpeg

    def ffmpeg(self):
        return movieconv.find_ffmpeg(self.app.settings.field("movie.ffmpeg", ""))

    def show_ffmpeg(self):
        ff = self.ffmpeg()
        self.ffmpeg_label.configure(text=ff or "not found: WAV files convert without it, MP3, OGG, FLAC, M4A... "
                                               "need it (Get FFmpeg downloads it)")

    def browse_ffmpeg(self):
        path = filedialog.askopenfilename(title="FFmpeg", filetypes=[("FFmpeg", "ffmpeg*"), ("All files", "*.*")],
                                          parent=self)
        if path:
            self.app.settings.set_field("movie.ffmpeg", path)
            self.show_ffmpeg()

    def download_ffmpeg(self):
        from tkinter import messagebox
        if not messagebox.askyesno("Get FFmpeg", "Download FFmpeg (about 90 MB, the essentials build of gyan.dev) "
                                   "to %s?" % os.path.join(movieconv.app_dir(), "ffmpeg"), parent=self):
            return
        self.get_ffmpeg.configure(state="disabled")
        self.set_status("Downloading FFmpeg...")

        def work():
            try:
                movieconv.download_ffmpeg(lambda done, total: self.post(self.set_progress, done, total))
                self.post(self.set_status, "FFmpeg downloaded.")
            except Exception as e:      # (network, zip)
                self.post(self.set_status, "FFmpeg download failed: %s" % e, True)
            self.post(lambda: (self.get_ffmpeg.configure(state="normal"), self.show_ffmpeg(),
                               self.set_progress(0, 1)))
        threading.Thread(target=work, daemon=True).start()

    # ---------------------------------------------------------------- jobs

    def set_status(self, text, error=False):
        self.status.configure(text=text, bootstyle="danger" if error else "secondary")

    def set_progress(self, done, total):
        self.progress.configure(value=100.0 * done / total if total else 0)

    def run(self, what, work, done):
        """work(progress) in a thread, then done(result) here; errors shown."""
        if self.busy:
            return self.set_status("A conversion is running.", True)
        self.busy = True
        self.set_status(what + "...")
        self.set_progress(0, 1)

        def thread():
            try:
                result = work(lambda d, t: self.post(self.set_progress, d, t))
                self.post(finish, None, result)
            except audioconv.ConvertError as e:
                self.post(finish, str(e), None)
            except Exception as e:      # (a file that can't be written, a bad file)
                self.post(finish, "%s: %s" % (type(e).__name__, e), None)

        def finish(error, result):
            self.busy = False
            self.set_progress(0 if error else 1, 1)
            if error:
                self.set_status(error, True)
            else:
                done(result)
        threading.Thread(target=thread, daemon=True).start()

    def play(self, paths):
        """The files given to the Tester tab, which starts them on the console."""
        self.page.files.set_files(paths)
        self.page.tabs.select(0)
        self.page.play()

    def open_path(self, path):
        """A result opened with the computer's own program (a folder: in the file manager)."""
        if not path or not os.path.exists(path):
            return self.set_status("Not found: %s" % path, True)
        try:
            if hasattr(os, "startfile"):
                os.startfile(path)
            else:
                import subprocess
                subprocess.Popen(["xdg-open", path])
        except OSError as e:
            self.set_status("Can't open %s: %s" % (path, e), True)

    def open_button(self, row, text, path_fn):
        b = tb.Button(row, text=text, bootstyle="info-outline", state="disabled",
                      command=lambda: self.open_path(path_fn()))
        b.pack(side="left", padx=6)
        return b

    @staticmethod
    def audio_out(field, src, ext=".wav"):
        """An audio output path (the source's name if empty, .wav if no audio extension)."""
        out = field.get()
        if not out and src:
            out = os.path.splitext(src)[0] + ext
        if not out.lower().endswith(audioconv.AUDIO_OUT):
            out += ext
        field.set(out)
        return out

    @staticmethod
    def out_path(field, src, ext):
        """The output field's path, else the source's with the extension."""
        out = field.get()
        if not out and src:
            out = os.path.splitext(src)[0] + ext
            field.set(out)
        return out

    # ---------------------------------------------------------------- Audio -> VAG

    def build_vag(self, tab):
        tb.Label(tab, text="A sound as a VAG sample (SPU ADPCM, about 3.5 times smaller than 16-bit PCM): played by "
                           "the tester, used by programs, or added to a VAB.", bootstyle="secondary",
                 wraplength=900, justify="left").pack(anchor="w", pady=(0, 6))
        self.vag_src = FileField(tab, self.app, "sound.conv.vag.src", "Sound", filetypes=AUDIO_TYPES,
                                 on_change=lambda: self.vag_out.set(""))
        self.vag_src.pack(fill="x", pady=2)
        self.vag_out = FileField(tab, self.app, "sound.conv.vag.out", "VAG file", mode="save",
                                 filetypes=[("VAG", "*.vag")])
        self.vag_out.pack(fill="x", pady=2)
        grid = tb.Frame(tab)
        grid.pack(fill="x", pady=6)
        self.vag_rate = tk.StringVar(value=str(self.app.settings.get("sound.conv.vag.rate", 22050)))
        self.vag_vol = tk.IntVar(value=100)
        self.vag_norm = tk.BooleanVar(value=True)
        self.vag_start = tk.StringVar(value="0")
        self.vag_len = tk.StringVar(value="0")
        self.vag_loop = tk.BooleanVar(value=False)
        self.vag_loop_start = tk.StringVar(value="0")
        tb.Label(grid, text="Sample rate", width=14).grid(row=0, column=0, sticky="w")
        tb.Combobox(grid, textvariable=self.vag_rate, values=[str(r) for r in audioconv.VAG_RATES], width=8).grid(
            row=0, column=1, sticky="w")
        tb.Label(grid, text="Hz (lower: smaller, duller)").grid(row=0, column=2, sticky="w", padx=4)
        tb.Checkbutton(grid, text="Normalize", variable=self.vag_norm, bootstyle="round-toggle").grid(
            row=0, column=3, sticky="w", padx=(20, 0))
        tb.Label(grid, text="Volume %").grid(row=0, column=4, sticky="w", padx=(20, 4))
        tb.Spinbox(grid, from_=1, to=400, textvariable=self.vag_vol, width=5).grid(row=0, column=5, sticky="w")
        tb.Label(grid, text="Start (s)", width=14).grid(row=1, column=0, sticky="w", pady=(6, 0))
        tb.Entry(grid, textvariable=self.vag_start, width=8).grid(row=1, column=1, sticky="w", pady=(6, 0))
        tb.Label(grid, text="Length (s, 0: to the end)").grid(row=1, column=2, columnspan=2, sticky="w", padx=4,
                                                              pady=(6, 0))
        tb.Entry(grid, textvariable=self.vag_len, width=8).grid(row=1, column=4, sticky="w", padx=(20, 0),
                                                                pady=(6, 0))
        tb.Checkbutton(grid, text="Loop", variable=self.vag_loop, bootstyle="round-toggle").grid(
            row=2, column=0, sticky="w", pady=(6, 0))
        tb.Label(grid, text="from (s)").grid(row=2, column=2, sticky="w", padx=4, pady=(6, 0))
        tb.Entry(grid, textvariable=self.vag_loop_start, width=8).grid(row=2, column=1, sticky="w", pady=(6, 0))
        row = tb.Frame(tab)
        row.pack(fill="x", pady=(6, 0))
        tb.Button(row, text="Convert", bootstyle="success", command=self.convert_vag).pack(side="left")
        self.vag_play = tb.Button(row, text="Play in the tester", bootstyle="info-outline", state="disabled",
                                  command=lambda: self.play([self.vag_out.get()]))
        self.vag_play.pack(side="left", padx=6)

    def convert_vag(self):
        src = self.vag_src.get()
        if not src:
            return self.set_status("Choose the sound.", True)
        out = self.out_path(self.vag_out, src, ".vag")
        rate = number(self.vag_rate, 22050, 4000, 48000)
        self.app.settings.set("sound.conv.vag.rate", rate)
        start, length = number(self.vag_start, 0.0, 0, kind=float), number(self.vag_len, 0.0, 0, kind=float)
        loop, loop_start = self.vag_loop.get(), number(self.vag_loop_start, 0.0, 0, kind=float)
        gain, norm, ff = number(self.vag_vol, 100, 1, 400) / 100.0, self.vag_norm.get(), self.ffmpeg()
        name = os.path.splitext(os.path.basename(src))[0]
        self.vag_src.remember()
        self.vag_out.remember()

        def work(progress):
            pcm = audioconv.load(src, rate, False, ff, start, length, gain, norm)
            data = audioconv.to_vag(pcm, rate, loop, loop_start, name, progress)
            with open(out, "wb") as f:
                f.write(data)
            return len(pcm), len(data)

        def done(result):
            samples, size = result
            self.vag_play.configure(state="normal")
            self.set_status("%s: %.2f s at %d Hz, %d KB%s." % (
                os.path.basename(out), samples / rate, rate, size // 1024,
                ", looping from %.2f s" % loop_start if loop else ""))
        self.run("Converting to VAG", work, done)

    def build_vag_back(self, tab):
        tb.Label(tab, text="A VAG sample decoded as an audio file (WAV: its loop kept in the file for samplers; MP3, "
                           "OGG, FLAC with FFmpeg).", bootstyle="secondary", wraplength=900,
                 justify="left").pack(anchor="w", pady=(0, 6))
        self.unvag_src = FileField(tab, self.app, "sound.conv.unvag.src", "VAG file",
                                   filetypes=[("VAG", "*.vag"), ("All files", "*.*")],
                                   on_change=lambda: self.unvag_out.set(""))
        self.unvag_src.pack(fill="x", pady=2)
        self.unvag_out = FileField(tab, self.app, "sound.conv.unvag.out", "Audio file", mode="save",
                                   filetypes=AUDIO_SAVE_TYPES)
        self.unvag_out.pack(fill="x", pady=2)
        row = tb.Frame(tab)
        row.pack(fill="x", pady=(6, 0))
        self.unvag_loops = tk.StringVar(value="0")
        tb.Label(row, text="Loop played again", width=14).pack(side="left")
        tb.Spinbox(row, from_=0, to=99, textvariable=self.unvag_loops, width=4).pack(side="left")
        tb.Label(row, text="times (a looping sample: its sustain heard longer)", bootstyle="secondary").pack(
            side="left", padx=6)
        row = tb.Frame(tab)
        row.pack(fill="x", pady=(6, 0))
        tb.Button(row, text="Convert", bootstyle="success", command=self.convert_unvag).pack(side="left")
        self.unvag_open = self.open_button(row, "Open", self.unvag_out.get)

    def convert_unvag(self):
        src = self.unvag_src.get()
        if not src:
            return self.set_status("Choose the VAG file.", True)
        out, ff, loops = self.audio_out(self.unvag_out, src), self.ffmpeg(), number(self.unvag_loops, 0, 0, 99)
        self.unvag_src.remember()
        self.unvag_out.remember()

        def done(result):
            seconds, rate, looping = result
            self.unvag_open.configure(state="normal")
            self.set_status("%s: %.2f s at %d Hz%s." % (os.path.basename(out), seconds, rate,
                                                        ", looping" if looping else ""))
        self.run("Decoding the VAG", lambda progress: audioconv.vag_to_audio(src, out, ff, loops), done)

    # ---------------------------------------------------------------- Samples -> VAB

    def build_vab(self, tab):
        tb.Label(tab, text="Samples made into a VAB bank: each sample is a tone of a program (an instrument, its "
                           "number as in MIDI program changes). Samples of the same program are its key splits "
                           "(e.g. a drum kit: one note each). The VAB Editor tab tunes the bank further.",
                 bootstyle="secondary", wraplength=900, justify="left").pack(anchor="w", pady=(0, 6))
        row = tb.Frame(tab)
        row.pack(fill="both", expand=True)
        self.vab_tree = tb.Treeview(row, columns=[c[0] for c in COLUMNS], show="headings", height=7,
                                    selectmode="browse")
        for key, title, width in COLUMNS:
            self.vab_tree.heading(key, text=title)
            self.vab_tree.column(key, width=width, anchor="w" if key in ("file", "envelope") else "center")
        self.vab_tree.pack(side="left", fill="both", expand=True)
        self.vab_tree.bind("<<TreeviewSelect>>", lambda e: self.vab_select())
        buttons = tb.Frame(row)
        buttons.pack(side="left", fill="y", padx=(6, 0))
        for text, command in (("Add...", self.vab_add), ("Remove", self.vab_remove), ("Up", lambda: self.vab_move(-1)),
                              ("Down", lambda: self.vab_move(1)), ("Clear", self.vab_clear)):
            tb.Button(buttons, text=text, width=8, bootstyle="secondary-outline", command=command).pack(pady=1)

        # The selected sample's settings
        edit = tb.Labelframe(tab, text="Selected sample", padding=8)
        edit.pack(fill="x", pady=6)
        self.vab_vars = {k: (tk.BooleanVar() if isinstance(v, bool) else tk.StringVar())
                         for k, v in SAMPLE_DEFAULTS.items()}
        self.vab_widgets = []

        def field(r, c, label, key, widget, **kw):
            tb.Label(edit, text=label).grid(row=r, column=c, sticky="w", padx=(0 if c == 0 else 14, 4), pady=2)
            if widget is tb.Checkbutton:
                w = tb.Checkbutton(edit, variable=self.vab_vars[key], bootstyle="round-toggle",
                                   command=self.vab_changed, **kw)
            else:
                w = widget(edit, textvariable=self.vab_vars[key], **kw)
                w.bind("<FocusOut>", lambda e: self.vab_changed())
                w.bind("<Return>", lambda e: self.vab_changed())
                if widget in (tb.Spinbox, tb.Combobox):
                    w.configure(command=self.vab_changed) if widget is tb.Spinbox else \
                        w.bind("<<ComboboxSelected>>", lambda e: self.vab_changed())
            w.grid(row=r, column=c + 1, sticky="w", pady=2)
            self.vab_widgets.append(w)
            return w
        field(0, 0, "Program", "program", tb.Spinbox, from_=0, to=127, width=5)
        field(0, 2, "Root note", "root", tb.Spinbox, from_=0, to=127, width=5)
        self.root_name = tb.Label(edit, text="", bootstyle="secondary", width=5)
        self.root_name.grid(row=0, column=4, sticky="w")
        field(0, 5, "Fine tune (cents)", "cents", tb.Spinbox, from_=-100, to=100, width=5)
        field(0, 7, "Rate (Hz)", "rate", tb.Combobox, values=[str(r) for r in audioconv.VAG_RATES], width=7)
        field(1, 0, "Lowest key", "min", tb.Spinbox, from_=0, to=127, width=5)
        field(1, 2, "Highest key", "max", tb.Spinbox, from_=0, to=127, width=5)
        field(1, 5, "Volume", "vol", tb.Spinbox, from_=0, to=127, width=5)
        field(1, 7, "Pan (64: center)", "pan", tb.Spinbox, from_=0, to=127, width=5)
        tb.Label(edit, text="Envelope").grid(row=2, column=0, sticky="w", pady=2)
        self.env_choice = tb.Combobox(edit, state="readonly", width=26, values=[e[0] for e in audioconv.ENVELOPES])
        self.env_choice.grid(row=2, column=1, columnspan=3, sticky="w", pady=2)
        self.env_choice.bind("<<ComboboxSelected>>", lambda e: self.vab_changed())
        self.vab_widgets.append(self.env_choice)
        field(2, 5, "Reverb", "reverb", tb.Checkbutton)
        field(2, 7, "Normalize", "normalize", tb.Checkbutton)
        field(3, 0, "Loop", "loop", tb.Checkbutton)
        field(3, 2, "Loop from (s)", "loop_start", tb.Entry, width=7)
        b = tb.Button(edit, text="Same settings for all", bootstyle="secondary-outline", command=self.vab_copy_all)
        b.grid(row=3, column=5, columnspan=3, sticky="w", padx=(14, 0))
        self.vab_widgets.append(b)

        self.vab_out = FileField(tab, self.app, "sound.conv.vab.out", "Bank (VH/VAB)", mode="save",
                                 filetypes=[("VH + VB files", "*.vh"), ("One VAB file", "*.vab")])
        self.vab_out.pack(fill="x", pady=2)
        row = tb.Frame(tab)
        row.pack(fill="x", pady=(6, 0))
        tb.Button(row, text="Convert", bootstyle="success", command=self.convert_vab).pack(side="left")
        self.vab_play = tb.Button(row, text="Play in the tester", bootstyle="info-outline", state="disabled",
                                  command=self.play_vab)
        self.vab_play.pack(side="left", padx=6)
        tb.Label(row, text=".vh: the VH and VB files (the VB next to it); .vab: one file", bootstyle="secondary").pack(
            side="left", padx=10)

        self.samples = [dict(SAMPLE_DEFAULTS, **s) for s in self.app.settings.get("sound.conv.vab.samples") or []
                        if s.get("path")]
        self.vab_fill()

    def vab_save(self):
        self.app.settings.set("sound.conv.vab.samples", self.samples)

    def vab_row(self, s):
        return (os.path.basename(s["path"]), s["program"], note_name(int(s["root"])),
                "%s to %s" % (note_name(int(s["min"])), note_name(int(s["max"]))) if s["min"] != s["max"] else
                note_name(int(s["min"])), s["vol"], s["pan"], audioconv.ENVELOPES[s["envelope"]][0],
                ("from %gs" % s["loop_start"]) if s["loop"] else "no", s["rate"])

    def vab_fill(self, select=None):
        self.vab_tree.delete(*self.vab_tree.get_children())
        for i, s in enumerate(self.samples):
            self.vab_tree.insert("", "end", iid=str(i), values=self.vab_row(s))
        if select is not None and 0 <= select < len(self.samples):
            self.vab_tree.selection_set(str(select))
        self.vab_select()

    def vab_index(self):
        sel = self.vab_tree.selection()
        return int(sel[0]) if sel else None

    def vab_select(self):
        i = self.vab_index()
        state = "normal" if i is not None else "disabled"
        for w in self.vab_widgets:
            w.configure(state=("readonly" if w is self.env_choice else state) if i is not None else "disabled")
        if i is None:
            self.root_name.configure(text="")
            return
        s = self.samples[i]
        for k, var in self.vab_vars.items():
            var.set(s[k])
        self.env_choice.current(s["envelope"])
        self.root_name.configure(text=note_name(int(s["root"])))

    def vab_changed(self):
        i = self.vab_index()
        if i is None:
            return
        s, v = self.samples[i], self.vab_vars
        s.update(program=number(v["program"], s["program"], 0, 127), root=number(v["root"], s["root"], 0, 127),
                 cents=number(v["cents"], s["cents"], -100, 100), min=number(v["min"], s["min"], 0, 127),
                 max=number(v["max"], s["max"], 0, 127), vol=number(v["vol"], s["vol"], 0, 127),
                 pan=number(v["pan"], s["pan"], 0, 127), rate=number(v["rate"], s["rate"], 4000, 48000),
                 loop_start=number(v["loop_start"], s["loop_start"], 0, kind=float),
                 envelope=max(0, self.env_choice.current()), reverb=bool(v["reverb"].get()),
                 loop=bool(v["loop"].get()), normalize=bool(v["normalize"].get()))
        if s["min"] > s["max"]:
            s["min"], s["max"] = s["max"], s["min"]
        self.vab_tree.item(str(i), values=self.vab_row(s))
        self.root_name.configure(text=note_name(s["root"]))
        self.vab_save()

    def vab_add(self):
        folder = self.app.settings.field("sound.conv.vab.folder", "") or None
        paths = filedialog.askopenfilenames(initialdir=folder, filetypes=AUDIO_TYPES, parent=self)
        if not paths:
            return
        self.app.settings.set_field("sound.conv.vab.folder", os.path.dirname(paths[0]))
        # Each new sample its own program (after the last one used), playing on every key
        nxt = max([s["program"] for s in self.samples] + [-1]) + 1
        for path in paths:
            self.samples.append(dict(SAMPLE_DEFAULTS, path=os.path.normpath(path), program=min(127, nxt)))
            nxt += 1
        self.vab_save()
        self.vab_fill(len(self.samples) - 1)

    def vab_remove(self):
        i = self.vab_index()
        if i is not None:
            del self.samples[i]
            self.vab_save()
            self.vab_fill(min(i, len(self.samples) - 1))

    def vab_move(self, step):
        i = self.vab_index()
        if i is not None and 0 <= i + step < len(self.samples):
            self.samples[i], self.samples[i + step] = self.samples[i + step], self.samples[i]
            self.vab_save()
            self.vab_fill(i + step)

    def vab_clear(self):
        self.samples = []
        self.vab_save()
        self.vab_fill()

    def vab_copy_all(self):
        """The selected sample's envelope, volume, pan, reverb, rate and normalize for every sample."""
        i = self.vab_index()
        if i is None:
            return
        src = self.samples[i]
        for s in self.samples:
            for k in ("envelope", "vol", "pan", "reverb", "rate", "normalize"):
                s[k] = src[k]
        self.vab_save()
        self.vab_fill(i)

    def vab_paths(self):
        out = self.vab_out.get()
        if out.lower().endswith(".vab"):
            return [out]
        return [out, os.path.splitext(out)[0] + ".vb"]

    def convert_vab(self):
        if not self.samples:
            return self.set_status("Add the samples.", True)
        out = self.out_path(self.vab_out, self.samples[0]["path"], ".vh")
        if not out.lower().endswith((".vh", ".vab")):
            out += ".vh"
            self.vab_out.set(out)
        self.vab_out.remember()
        samples, ff = [dict(s) for s in self.samples], self.ffmpeg()
        paths = self.vab_paths()

        def work(progress):
            tones = []
            for n, s in enumerate(samples):
                progress(n, len(samples) * 2)
                a1, a2 = audioconv.ENVELOPES[s["envelope"]][1:]
                tones.append(dict(s, pcm=audioconv.load(s["path"], s["rate"], False, ff, normalize=s["normalize"]),
                                  adsr1=a1, adsr2=a2))
            vh, vb = audioconv.build_vab(tones, lambda d, t: progress(len(samples) + d, len(samples) * 2))
            if len(paths) == 1:
                with open(paths[0], "wb") as f:
                    f.write(vh + vb)
            else:
                with open(paths[0], "wb") as f:
                    f.write(vh)
                with open(paths[1], "wb") as f:
                    f.write(vb)
            return len(vh), len(vb), len({s["program"] for s in samples})

        def done(result):
            vh, vb, programs = result
            self.vab_play.configure(state="normal")
            room = audioconv.SPU_SAMPLES_MAX
            self.set_status("%s: %d programs, %d samples, VB %d KB of the %d KB of SPU RAM for samples%s." % (
                os.path.basename(paths[0]), programs, len(samples), vb // 1024, room // 1024,
                " - TOO BIG: the end won't fit (lower the rates)" if vb > room else
                (" (%d KB left: reverb fits up to that)" % ((room - vb) // 1024))))
            if not self.seq_vh.get():
                self.seq_vh.set(paths[0])
        self.run("Building the VAB", work, done)

    def play_vab(self):
        self.play(self.vab_paths())

    def build_vab_back(self, tab):
        tb.Label(tab, text="The samples of a VAB bank as audio files (one per sample, in a folder) and a list of its "
                           "programs and tones: key ranges, root notes, volume, pan, envelopes. Each sample's WAV "
                           "has its root note and loop (for samplers).", bootstyle="secondary", wraplength=900,
                 justify="left").pack(anchor="w", pady=(0, 6))
        self.unvab_src = FileField(tab, self.app, "sound.conv.unvab.src", "Bank (VH/VAB)",
                                   filetypes=[("VH or VAB", "*.vh *.vab"), ("All files", "*.*")],
                                   on_change=lambda: self.unvab_out.set(""))
        self.unvab_src.pack(fill="x", pady=2)
        self.unvab_out = FileField(tab, self.app, "sound.conv.unvab.out", "Folder", mode="dir")
        self.unvab_out.pack(fill="x", pady=2)
        row = tb.Frame(tab)
        row.pack(fill="x", pady=(6, 0))
        tb.Label(row, text="Format", width=14).pack(side="left")
        self.unvab_fmt = tb.Combobox(row, state="readonly", width=6, values=SAMPLE_FORMATS)
        self.unvab_fmt.current(0)
        self.unvab_fmt.pack(side="left")
        tb.Label(row, text="(the root note and loop are kept in WAV files only)", bootstyle="secondary").pack(
            side="left", padx=6)
        row = tb.Frame(tab)
        row.pack(fill="x", pady=(6, 0))
        tb.Button(row, text="Convert", bootstyle="success", command=self.convert_unvab).pack(side="left")
        self.unvab_open = self.open_button(row, "Open the folder", self.unvab_out.get)
        self.unvab_list = self.open_button(row, "Open the list", lambda: getattr(self, "unvab_text", ""))

    def convert_unvab(self):
        src = self.unvab_src.get()
        if not src:
            return self.set_status("Choose the bank (VH with its VB, or VAB).", True)
        out = self.unvab_out.get()
        if not out:
            out = os.path.splitext(src)[0] + "_samples"
            self.unvab_out.set(out)
        ext, ff = SAMPLE_FORMATS[max(0, self.unvab_fmt.current())], self.ffmpeg()
        self.unvab_src.remember()
        self.unvab_out.remember()

        def done(result):
            written, text = result
            self.unvab_text = text
            self.unvab_open.configure(state="normal")
            self.unvab_list.configure(state="normal")
            self.set_status("%d samples written in %s, the bank's list in %s." % (written, out,
                                                                                 os.path.basename(text)))
        self.run("Exporting the samples", lambda progress: audioconv.export_vab(src, out, None, ext, ff, progress),
                 done)

    # ---------------------------------------------------------------- MIDI -> SEQ

    def build_seq(self, tab):
        tb.Label(tab, text="A MIDI file (type 0 or 1: its tracks are merged) as SEQ music. A SEQ plays with a VAB "
                           "bank: the MIDI program changes choose the bank's programs (General MIDI numbers only "
                           "match a bank made for them). The console has 24 voices.",
                 bootstyle="secondary", wraplength=900, justify="left").pack(anchor="w", pady=(0, 6))
        self.seq_src = FileField(tab, self.app, "sound.conv.seq.src", "MIDI file", filetypes=MIDI_TYPES,
                                 on_change=lambda: self.seq_out.set(""))
        self.seq_src.pack(fill="x", pady=2)
        self.seq_out = FileField(tab, self.app, "sound.conv.seq.out", "SEQ file", mode="save",
                                 filetypes=[("SEQ", "*.seq")])
        self.seq_out.pack(fill="x", pady=2)
        grid = tb.Frame(tab)
        grid.pack(fill="x", pady=6)
        tb.Label(grid, text="Loop", width=14).grid(row=0, column=0, sticky="w")
        self.loop_choice = tb.Combobox(grid, state="readonly", width=44, values=[m[1] for m in LOOP_MODES])
        self.loop_choice.current(0)
        self.loop_choice.grid(row=0, column=1, sticky="w")
        tb.Label(grid, text="tick").grid(row=0, column=2, sticky="w", padx=(10, 4))
        self.loop_tick = tk.StringVar(value="0")
        tb.Entry(grid, textvariable=self.loop_tick, width=8).grid(row=0, column=3, sticky="w")
        self.drums = tk.BooleanVar(value=False)
        self.drum_prog = tk.StringVar(value="0")
        tb.Checkbutton(grid, text="MIDI channel 10 (GM drums) plays program", variable=self.drums,
                       bootstyle="round-toggle").grid(row=1, column=0, columnspan=2, sticky="w", pady=(6, 0))
        tb.Spinbox(grid, from_=0, to=127, textvariable=self.drum_prog, width=5).grid(row=1, column=2, columnspan=2,
                                                                                     sticky="w", pady=(6, 0))
        self.seq_vh = FileField(tab, self.app, "sound.conv.seq.vh", "Bank to play", filetypes=[
            ("VH or VAB", "*.vh *.vab"), ("All files", "*.*")])
        self.seq_vh.pack(fill="x", pady=2)
        row = tb.Frame(tab)
        row.pack(fill="x", pady=(6, 0))
        tb.Button(row, text="Convert", bootstyle="success", command=self.convert_seq).pack(side="left")
        self.seq_play = tb.Button(row, text="Play in the tester with the bank", bootstyle="info-outline",
                                  state="disabled", command=self.play_seq)
        self.seq_play.pack(side="left", padx=6)
        self.seq_info = tb.Label(tab, text="", bootstyle="secondary", wraplength=900, justify="left")
        self.seq_info.pack(anchor="w", pady=(8, 0))

    def convert_seq(self):
        src = self.seq_src.get()
        if not src:
            return self.set_status("Choose the MIDI file.", True)
        out = self.out_path(self.seq_out, src, ".seq")
        mode = LOOP_MODES[max(0, self.loop_choice.current())][0]
        tick = number(self.loop_tick, 0, 0)
        drums = number(self.drum_prog, 0, 0, 127) if self.drums.get() else None
        self.seq_src.remember()
        self.seq_out.remember()

        def work(progress):
            data, info = audioconv.midi_to_seq(src, mode, tick, drums)
            with open(out, "wb") as f:
                f.write(data)
            return len(data), info

        def done(result):
            size, info = result
            self.seq_play.configure(state="normal")
            minutes, seconds = divmod(int(round(info["seconds"])), 60)
            loop = ("loop from tick %d to %d" % (info["loop"], info["loop_end"])) if info["loop"] is not None else \
                "no loop"
            self.set_status("%s: %d bytes, %d:%02d, %d notes, %s." % (os.path.basename(out), size, minutes, seconds,
                                                                     info["notes"], loop))
            progs = {}
            for ch, p in info["programs"]:
                progs.setdefault(ch, []).append(p)
            lines = ["Channels: " + ", ".join(
                "%d (program %s)" % (ch + 1, "/".join(str(p) for p in progs.get(ch, [0]))) for ch in info["channels"])]
            lines.append("Most notes at once: %d%s" % (info["polyphony"], " - more than the 24 voices: some notes "
                                                       "take the oldest voices" if info["polyphony"] > 23 else ""))
            self.seq_info.configure(text="\n".join(lines))
        self.run("Converting the MIDI file", work, done)

    def play_seq(self):
        vh = self.seq_vh.get()
        if not vh:
            return self.set_status("Choose the bank (VH or VAB) the music plays with.", True)
        files = [vh] if vh.lower().endswith(".vab") else [vh, os.path.splitext(vh)[0] + ".vb"]
        missing = [f for f in files if not os.path.isfile(f)]
        if missing:
            return self.set_status("Not found: %s" % ", ".join(missing), True)
        self.seq_vh.remember()
        self.play(files + [self.seq_out.get()])

    def build_seq_back(self, tab):
        tb.Label(tab, text="SEQ music as a MIDI file (type 0; a SEP: one per SEQ), played as the console plays it: "
                           "a loop with a count written out, an endless loop as loopStart/loopEnd markers (read back "
                           "by MIDI -> SEQ), or written again after itself for players that don't loop. Its programs are the bank's: General MIDI instruments won't "
                           "match them.", bootstyle="secondary", wraplength=900,
                 justify="left").pack(anchor="w", pady=(0, 6))
        self.unseq_src = FileField(tab, self.app, "sound.conv.unseq.src", "SEQ / SEP file",
                                   filetypes=[("SEQ or SEP", "*.seq *.sep"), ("All files", "*.*")],
                                   on_change=lambda: self.unseq_out.set(""))
        self.unseq_src.pack(fill="x", pady=2)
        self.unseq_out = FileField(tab, self.app, "sound.conv.unseq.out", "MIDI file", mode="save",
                                   filetypes=[("MIDI", "*.mid")])
        self.unseq_out.pack(fill="x", pady=2)
        row = tb.Frame(tab)
        row.pack(fill="x", pady=(6, 0))
        self.unseq_loops = tk.StringVar(value="0")
        tb.Label(row, text="Loop written again", width=14).pack(side="left")
        tb.Spinbox(row, from_=0, to=20, textvariable=self.unseq_loops, width=4).pack(side="left")
        tb.Label(row, text="times", bootstyle="secondary").pack(side="left", padx=6)
        row = tb.Frame(tab)
        row.pack(fill="x", pady=(6, 0))
        tb.Button(row, text="Convert", bootstyle="success", command=self.convert_unseq).pack(side="left")
        self.unseq_open = self.open_button(row, "Open", self.unseq_out.get)

    def convert_unseq(self):
        src = self.unseq_src.get()
        if not src:
            return self.set_status("Choose the SEQ file.", True)
        out = self.out_path(self.unseq_out, src, ".mid")
        loops = number(self.unseq_loops, 0, 0, 20)
        self.unseq_src.remember()
        self.unseq_out.remember()

        def done(info):
            self.unseq_open.configure(state="normal")
            names = ", ".join(os.path.basename(f) for f in info["files"])
            self.set_status("%s: %d events, %d ticks per quarter note%s%s." % (
                names, info["events"], info["ppqn"],
                (", endless loop from tick %d%s" % (info["loop"], " (written %d more times)" % loops if loops else
                                                    "")) if info["loop"] is not None else ", no endless loop",
                " (a SEP: one MIDI file per SEQ)" if info["tracks"] > 1 else ""))
        self.run("Converting the SEQ", lambda progress: audioconv.seq_to_midi(src, out, loops), done)

    # ---------------------------------------------------------------- Audio -> XA

    def build_xa(self, tab):
        tb.Label(tab, text="Sounds as XA ADPCM: CD music streamed while a game runs (the CD drive decodes it). An XA "
                           "file holds several songs, one per channel, read in turn; a game plays one channel. Put "
                           "the file on the disc image (mkpsxiso: a file of type \"xa\"). The tester can't play it: "
                           "XA is read from the CD.", bootstyle="secondary", wraplength=900,
                 justify="left").pack(anchor="w", pady=(0, 6))
        self.xa_songs = FileList(tab, self.app, "sound.conv.xa.songs", "Songs (song 1 on channel 0, ...)",
                                 filetypes=AUDIO_TYPES, height=4, compact=True)
        self.xa_songs.pack(fill="x")
        grid = tb.Frame(tab)
        grid.pack(fill="x", pady=6)
        self.xa_rate = tk.StringVar(value="37800")
        self.xa_stereo = tk.BooleanVar(value=True)
        self.xa_speed = tk.StringVar(value="2")
        self.xa_file_no = tk.StringVar(value="1")
        self.xa_norm = tk.BooleanVar(value=False)
        self.xa_vol = tk.IntVar(value=100)
        tb.Label(grid, text="Sample rate", width=14).grid(row=0, column=0, sticky="w")
        c = tb.Combobox(grid, state="readonly", textvariable=self.xa_rate, width=7,
                        values=[str(r) for r in strmovie.XA_RATES])
        c.grid(row=0, column=1, sticky="w")
        c.bind("<<ComboboxSelected>>", lambda e: self.show_xa())
        tb.Checkbutton(grid, text="Stereo", variable=self.xa_stereo, bootstyle="round-toggle",
                       command=self.show_xa).grid(row=0, column=2, sticky="w", padx=(16, 0))
        tb.Label(grid, text="CD speed").grid(row=0, column=3, sticky="w", padx=(16, 4))
        c = tb.Combobox(grid, state="readonly", textvariable=self.xa_speed, width=3, values=["1", "2"])
        c.grid(row=0, column=4, sticky="w")
        c.bind("<<ComboboxSelected>>", lambda e: self.show_xa())
        tb.Label(grid, text="x").grid(row=0, column=5, sticky="w")
        tb.Label(grid, text="File number").grid(row=0, column=6, sticky="w", padx=(16, 4))
        tb.Spinbox(grid, from_=0, to=255, textvariable=self.xa_file_no, width=4).grid(row=0, column=7, sticky="w")
        tb.Checkbutton(grid, text="Normalize", variable=self.xa_norm, bootstyle="round-toggle").grid(
            row=1, column=0, sticky="w", pady=(6, 0))
        tb.Label(grid, text="Volume %").grid(row=1, column=1, sticky="e", padx=(0, 4), pady=(6, 0))
        tb.Spinbox(grid, from_=1, to=400, textvariable=self.xa_vol, width=5).grid(row=1, column=2, sticky="w",
                                                                                  padx=(16, 0), pady=(6, 0))
        self.xa_channels_label = tb.Label(tab, text="", bootstyle="secondary")
        self.xa_channels_label.pack(anchor="w")
        self.xa_out = FileField(tab, self.app, "sound.conv.xa.out", "XA file", mode="save",
                                filetypes=[("XA", "*.xa")])
        self.xa_out.pack(fill="x", pady=2)
        row = tb.Frame(tab)
        row.pack(fill="x", pady=(6, 0))
        tb.Button(row, text="Convert", bootstyle="success", command=self.convert_xa).pack(side="left")
        self.show_xa()

    def xa_settings(self):
        return number(self.xa_rate, 37800), bool(self.xa_stereo.get()), number(self.xa_speed, 2, 1, 2)

    def show_xa(self):
        rate, stereo, speed = self.xa_settings()
        n = audioconv.xa_channels(rate, stereo, speed)
        self.xa_channels_label.configure(text="At %d Hz %s and %dx: one sector of each of %d channels in turn, so "
                                              "up to %d songs (the unused channels are silent)." % (
                                                  rate, "stereo" if stereo else "mono", speed, n, n))

    def convert_xa(self):
        songs = self.xa_songs.files()
        if not songs:
            return self.set_status("Add the songs.", True)
        rate, stereo, speed = self.xa_settings()
        out = self.out_path(self.xa_out, songs[0], ".xa")
        self.xa_out.remember()
        file_no, ff = number(self.xa_file_no, 1, 0, 255), self.ffmpeg()
        gain, norm = number(self.xa_vol, 100, 1, 400) / 100.0, self.xa_norm.get()

        def work(progress):
            pcm = []
            for n, path in enumerate(songs):
                progress(n, len(songs) * 2)
                pcm.append(audioconv.load(path, rate, stereo, ff, gain=gain, normalize=norm))
            lengths = [len(p) / rate for p in pcm]
            sectors, seconds = audioconv.write_xa(out, pcm, rate, stereo, speed, file_no,
                                                  lambda d, t: progress(len(songs) + d, len(songs) * 2))
            return sectors, seconds, lengths

        def done(result):
            sectors, seconds, lengths = result
            self.set_status("%s: %d sectors (%.1f MB on the disc), %d:%02d. %s" % (
                os.path.basename(out), sectors, sectors * 2336 / 1048576, int(seconds) // 60, int(seconds) % 60,
                "  ".join("Channel %d: %s (%d:%02d)" % (i, os.path.basename(p), int(l) // 60, int(l) % 60)
                          for i, (p, l) in enumerate(zip(songs, lengths)))))
        self.run("Converting to XA", work, done)

    def build_xa_back(self, tab):
        tb.Label(tab, text="XA audio decoded: each channel (song) as an audio file, NAME_chN (the silent channels "
                           "left out). Also the sound of an STR movie. Files with 2336 or 2352-byte sectors.",
                 bootstyle="secondary", wraplength=900, justify="left").pack(anchor="w", pady=(0, 6))
        self.unxa_src = FileField(tab, self.app, "sound.conv.unxa.src", "XA file",
                                  filetypes=[("XA or STR", "*.xa *.str *.xai"), ("All files", "*.*")],
                                  on_change=lambda: self.unxa_out.set(""))
        self.unxa_src.pack(fill="x", pady=2)
        self.unxa_out = FileField(tab, self.app, "sound.conv.unxa.out", "Audio files", mode="save",
                                  filetypes=AUDIO_SAVE_TYPES)
        self.unxa_out.pack(fill="x", pady=2)
        row = tb.Frame(tab)
        row.pack(fill="x", pady=(6, 0))
        tb.Label(row, text="Channel", width=14).pack(side="left")
        self.unxa_channel = tb.Combobox(row, state="readonly", width=6, values=["All"] + [str(c) for c in range(32)])
        self.unxa_channel.current(0)
        self.unxa_channel.pack(side="left")
        row = tb.Frame(tab)
        row.pack(fill="x", pady=(6, 0))
        tb.Button(row, text="Convert", bootstyle="success", command=self.convert_unxa).pack(side="left")
        self.unxa_open = self.open_button(row, "Open the folder", lambda: os.path.dirname(self.unxa_out.get()))
        self.unxa_files = tb.Label(tab, text="", bootstyle="secondary", wraplength=900, justify="left")
        self.unxa_files.pack(anchor="w", pady=(6, 0))

    def convert_unxa(self):
        src = self.unxa_src.get()
        if not src:
            return self.set_status("Choose the XA file.", True)
        out = self.audio_out(self.unxa_out, src)
        base, ext = os.path.splitext(out)
        channel = None if self.unxa_channel.current() <= 0 else self.unxa_channel.current() - 1
        ff = self.ffmpeg()
        self.unxa_src.remember()
        self.unxa_out.remember()

        def done(written):
            self.unxa_open.configure(state="normal")
            if not written:
                return self.set_status("No sound in %s (every channel silent)." % os.path.basename(src), True)
            self.set_status("%d file%s written." % (len(written), "s" if len(written) > 1 else ""))
            self.unxa_files.configure(text="\n".join("Channel %d: %s (%d:%02d)" % (
                ch, os.path.basename(name), int(sec) // 60, int(sec) % 60) for ch, name, sec in written))
        self.run("Decoding the XA", lambda progress: audioconv.xa_to_audio(src, base, ext, ff, channel, progress),
                 done)
