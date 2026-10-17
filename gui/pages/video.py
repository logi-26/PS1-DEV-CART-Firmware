"""
PS1 DEV CART GUI: Video streamer page (videos from files or links played by the PlayStation, see ps1-tools/videostreamer).

The video server (ps1-tools/videostreamer/video_server.py) runs in this program, on a free port of 127.0.0.1; ps1transfer
executes videostreamer.ps-exe on the PlayStation and connects its USB link to the server (-net). Stop ends both.
Needs PyAV, NumPy and miniaudio (pip install av numpy miniaudio), already in the GUI executable.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import queue
import sys
import types
from tkinter import filedialog, simpledialog

import ttkbootstrap as tb

from pages.base import Page
from widgets import MonoText, labeled

PROGRAM = "videostreamer.ps-exe"
VIDEO_TYPES = [("Videos", "*.mp4 *.mkv *.avi *.mov *.webm *.m4v *.mpg *.mpeg *.wmv *.flv *.ts"), ("All files", "*.*")]

# Examples: the Sintel trailer and film, Blender Foundation (https://durian.blender.org), Creative Commons Attribution 3.0
EXAMPLES = [
    ["SINTEL (TRAILER)", "https://download.blender.org/durian/trailer/Sintel_Trailer1.480p.DivX_Plus_HD.mkv",
     "(C) BLENDER FOUNDATION, CC BY 3.0"],
    ["SINTEL (FULL FILM, 15 MIN)", "https://download.blender.org/durian/movies/Sintel.2010.1080p.mkv",
     "(C) BLENDER FOUNDATION, CC BY 3.0"],
]

SIZES = {"320x240 (full screen, 1:1)": (320, 240), "160x120 (shown 2x, smaller stream)": (160, 120)}
RATES = ("22050", "11025", "32000")


def _server_module():
    """video_server.py: next to this program (executable), or in ps1-tools/videostreamer of the repository."""
    here = os.path.dirname(os.path.abspath(__file__))
    tools = os.path.join(here, "..", "..", "ps1-tools")
    for folder in (os.path.join(tools, "videostreamer"), os.path.join(tools, "audiostreamer")):
        if folder not in sys.path:
            sys.path.insert(0, os.path.abspath(folder))
    import video_server     # noqa: E402 (imported when used: PyAV takes a moment to load)
    return video_server


class VideoPage(Page):
    title = "Video streamer"
    # (the Audio streamer page, pages/audio.py, uses the playlist and run code with its own values)
    list_key = "video.list"
    examples_list = EXAMPLES
    program_name = PROGRAM
    folder = "videostreamer"

    def build(self):
        s = self.app.settings
        self.server = None
        self.log_queue = queue.Queue()

        tb.Label(self, text="PS1 DEV CART Video Streamer", font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="Plays video files or links (any format FFmpeg reads: files, http(s) streams) on the "
                            "PlayStation, with its MDEC video decoder and SPU sound, through the cartridge. The "
                            "computer converts the video while it plays. The examples are open movies of the "
                            "Blender Foundation (CC BY 3.0).",
                 wraplength=780, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 8))

        box = self.section("Playlist")
        box.pack(fill="both", expand=True, pady=(0, 8))
        row = tb.Frame(box)
        row.pack(fill="both", expand=True)
        self.table = tb.Treeview(row, columns=("name", "source", "credit"), displaycolumns=("name", "source"),
                                 show="headings", height=6, selectmode="browse")
        for col, text, width in (("name", "Name (shown on the PS1)", 220), ("source", "File or link", 520)):
            self.table.heading(col, text=text, anchor="w")
            self.table.column(col, width=width, anchor="w", stretch=(col == "source"))
        self.table.pack(side="left", fill="both", expand=True)
        buttons = tb.Frame(row)
        buttons.pack(side="left", fill="y", padx=(6, 0))
        for text, command in (("Add file...", self.add_file), ("Add link...", self.add_link), ("Rename...", self.rename),
                              ("Remove", self.remove), ("Up", lambda: self.move(-1)), ("Down", lambda: self.move(1)),
                              ("Examples", self.examples)):
            tb.Button(buttons, text=text, width=11, bootstyle="secondary-outline", command=command).pack(pady=1)
        for item in s.field(self.list_key, None) or self.examples_list:
            self.table.insert("", "end", values=item)

        box = self.section("Options")
        box.pack(fill="x", pady=(0, 8))
        opts = tb.Frame(box)
        opts.pack(fill="x")
        _, self.size = labeled(opts, "Picture", tb.Combobox, width=10, values=list(SIZES), state="readonly")
        self.size.master.pack(side="left")
        self.size.set(s.field("video.size", list(SIZES)[0]))
        self.size.configure(width=32)
        _, self.fps = labeled(opts, "Frames/s (most)", tb.Spinbox, width=16, from_=5, to=30, increment=1)
        self.fps.master.pack(side="left", padx=(12, 0))
        self.fps.set(s.field("video.fps", "15"))
        self.fps.configure(width=5)
        _, self.kbps = labeled(opts, "Video KB/s", tb.Spinbox, width=10, from_=10, to=140, increment=10)
        self.kbps.master.pack(side="left", padx=(12, 0))
        self.kbps.set(s.field("video.kbps", "90"))
        self.kbps.configure(width=5)
        _, self.rate = labeled(opts, "Sound Hz", tb.Combobox, width=9, values=RATES, state="readonly")
        self.rate.master.pack(side="left", padx=(12, 0))
        self.rate.set(s.field("video.rate", "22050"))
        self.rate.configure(width=7)
        tb.Label(box, text="Frames per second: the video's own rate divided by a whole number, up to this (24 -> 12, "
                           "30 -> 15). Higher KB/s: finer picture (about 160 KB/s at most through the cartridge, "
                           "sound included).", wraplength=760, justify="left", bootstyle="secondary").pack(anchor="w",
                                                                                                    pady=(6, 0))

        bar = tb.Frame(self)
        bar.pack(fill="x", pady=(0, 6))
        tb.Button(bar, text="Start", bootstyle="success", width=12, command=self.start).pack(side="left")
        tb.Button(bar, text="Stop", bootstyle="danger-outline", width=12, command=self.app.stop_task).pack(
            side="left", padx=8)
        tb.Label(bar, text="On the PS1: Left/Right video, Up/Down volume, Select information",
                 bootstyle="secondary").pack(side="left", padx=8)
        self.speed = tb.Label(bar, text="", bootstyle="info")
        self.speed.pack(side="right")
        self.log = MonoText(self, height=8)
        self.log.pack(fill="both", expand=True)
        self.after(200, self._poll_log)

    # ---------------------------------------------------------------- playlist

    def items(self):
        return [list(self.table.item(i, "values")) for i in self.table.get_children()]

    def _save(self):
        self.app.settings.set_field(self.list_key, self.items())

    def _name_for(self, source):
        try:
            return _server_module().video_name(source).upper()
        except ImportError:
            return os.path.splitext(os.path.basename(source.split("?")[0]))[0].upper()

    def add_file(self):
        for path in filedialog.askopenfilenames(filetypes=VIDEO_TYPES, parent=self):
            self.table.insert("", "end", values=(self._name_for(path), os.path.normpath(path), ""))
        self._save()

    def add_link(self):
        url = simpledialog.askstring("Add a link", "Video link (http, https, or anything FFmpeg opens):", parent=self)
        if url and url.strip():
            self.table.insert("", "end", values=(self._name_for(url.strip()), url.strip(), ""))
            self._save()

    def rename(self):
        sel = self.table.selection()
        if not sel:
            return
        values = list(self.table.item(sel[0], "values"))
        name = simpledialog.askstring("Rename", "Name shown on the PlayStation:", initialvalue=values[0], parent=self)
        if name:
            values[0] = name.upper()
            self.table.item(sel[0], values=values)
            self._save()

    def remove(self):
        for i in self.table.selection():
            self.table.delete(i)
        self._save()

    def move(self, step):
        sel = self.table.selection()
        if sel:
            self.table.move(sel[0], "", max(0, self.table.index(sel[0]) + step))
            self._save()

    def examples(self):
        """Adds the examples back (those not in the list)."""
        sources = {v[1] for v in self.items()}
        for item in self.examples_list:
            if item[1] not in sources:
                self.table.insert("", "end", values=item)
        self._save()

    # ---------------------------------------------------------------- run

    def program(self):
        """The PlayStation program: next to ps1transfer, or in its folder of the repository."""
        here = os.path.dirname(os.path.abspath(__file__))
        for path in (os.path.join(os.path.dirname(self.app.exe()), self.program_name),
                     os.path.join(here, "..", "..", self.folder, self.program_name)):
            if os.path.exists(path):
                return os.path.abspath(path)
        return self.program_name

    def args(self, port=7778):
        return ["-r", "-e", "-f", self.program(), "-net", "127.0.0.1:%d" % port]

    def options(self):
        try:
            fps, kbps = float(self.fps.get()), float(self.kbps.get())
        except ValueError:
            fps, kbps = 15.0, 90.0
        return types.SimpleNamespace(size=SIZES.get(self.size.get(), (320, 240)), fps=max(1.0, fps),
                                     kbps=max(5.0, kbps), rate=int(self.rate.get() or 22050))

    def start(self):
        items = self.items()
        if not items:
            self.app.error("Add a video file or link first.")
            return
        s = self.app.settings
        for key, widget in (("video.size", self.size), ("video.fps", self.fps), ("video.kbps", self.kbps),
                            ("video.rate", self.rate)):
            s.set_field(key, widget.get())
        try:
            video_server = _server_module()
        except ImportError as e:
            self.app.error("The video server needs PyAV, NumPy and miniaudio:\n\npip install av numpy miniaudio\n\n(%s)"
                           % e)
            return
        self._stop_server()
        videos = [(v[1], v[0], v[2] if len(v) > 2 else "") for v in items]
        self.server = video_server.Server(videos, self.options(), 0, self.log_queue.put)
        self.log.append("\nVideo server on 127.0.0.1:%d, %d video(s)\n" % (self.server.port, len(videos)))
        # The bridge's throughput lines (one per second) go to the speed label instead of the log
        self.app.run(self.args(self.server.port), "Video streamer", long_running=True, stdin=True,
                     on_line=self._bridge_line, log_filter=lambda line: "B/s" not in line,
                     on_done=lambda code, out: self._stop_server())

    def _bridge_line(self, line):
        if "server ->" in line and line.endswith("B/s"):
            try:
                self.speed.configure(text="Link: %d KB/s" % (int(line.split()[-2]) // 1024))
            except ValueError:
                pass

    def _stop_server(self):
        self.speed.configure(text="")
        if self.server:
            self.server.stop()
            self.server = None

    def _poll_log(self):
        try:
            while True:
                self.log.append(self.log_queue.get_nowait() + "\n")
        except queue.Empty:
            pass
        self.after(200, self._poll_log)
