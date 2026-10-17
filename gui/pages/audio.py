"""
PS1 DEV CART GUI: Audio streamer page (internet radio stations and audio files played by the PlayStation, see
../ps1-tools/audiostreamer).

The audio server (ps1-tools/audiostreamer/audio_server.py) runs in this program, on a free port of 127.0.0.1; ps1transfer
executes audiostreamer.ps-exe on the PlayStation and connects its USB link to the server (-net). Stop ends both.
The playlist and run code are the Video streamer page's. Needs miniaudio and NumPy (stations), PyAV (files and other
formats): pip install miniaudio numpy av, already in the GUI executable.

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

from pages.video import RATES, VideoPage
from widgets import MonoText, labeled

PROGRAM = "audiostreamer.ps-exe"
AUDIO_TYPES = [("Audio files", "*.mp3 *.flac *.wav *.ogg *.oga *.opus *.m4a *.aac *.wma *.aif *.aiff *.mka"),
               ("All files", "*.*")]

# Examples: UK stations of Global (http MP3 streams with song titles), as audio_server.STATIONS
EXAMPLES = [
    ["SMOOTH", "http://media-ice.musicradio.com/SmoothUKMP3", ""],
    ["CAPITAL LONDON", "http://media-ice.musicradio.com/CapitalMP3", ""],
    ["HEART", "http://media-ice.musicradio.com/HeartUKMP3", ""],
    ["CLASSIC FM", "http://media-ice.musicradio.com/ClassicFMMP3", ""],
    ["HEART 80S", "http://media-ice.musicradio.com/Heart80sMP3", ""],
    ["RADIO X CLASSIC ROCK", "http://media-ice.musicradio.com/RadioXClassicRockMP3", ""],
]


def _server_module():
    """audio_server.py: next to this program (executable), or in ps1-tools/audiostreamer of the repository."""
    folder = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "ps1-tools",
                                          "audiostreamer"))
    if folder not in sys.path:
        sys.path.insert(0, folder)
    import audio_server     # noqa: E402 (imported when used)
    return audio_server


class AudioPage(VideoPage):
    title = "Audio streamer"
    list_key = "audio.list"
    examples_list = EXAMPLES
    program_name = PROGRAM
    folder = "audiostreamer"

    def build(self):
        s = self.app.settings
        self.server = None
        self.log_queue = queue.Queue()

        tb.Label(self, text="PS1 DEV CART Audio Streamer", font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="Plays internet radio stations and audio files on the PlayStation, with its SPU, through "
                            "the cartridge. Add station links (Icecast/SHOUTcast MP3 streams show the song playing; "
                            "other formats play too) or audio files of any format FFmpeg reads (MP3, FLAC, WAV, OGG, "
                            "M4A, ...). At the end of a file the next one plays. The examples are UK radio stations.",
                 wraplength=780, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 8))

        box = self.section("Playlist")
        box.pack(fill="both", expand=True, pady=(0, 8))
        row = tb.Frame(box)
        row.pack(fill="both", expand=True)
        self.table = tb.Treeview(row, columns=("name", "source", "credit"), displaycolumns=("name", "source"),
                                 show="headings", height=7, selectmode="browse")
        for col, text, width in (("name", "Name (shown on the PS1)", 220), ("source", "Station link or file", 520)):
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
        _, self.rate = labeled(opts, "Sound Hz", tb.Combobox, width=9, values=RATES, state="readonly")
        self.rate.master.pack(side="left")
        self.rate.set(s.field("audio.rate", "22050"))
        self.rate.configure(width=7)
        tb.Label(opts, text="22050 Hz: 12.6 KB/s through the cartridge (32000 Hz: 18 KB/s, finer; 11025 Hz: 6 KB/s).",
                 bootstyle="secondary").pack(side="left", padx=(12, 0), anchor="s")

        bar = tb.Frame(self)
        bar.pack(fill="x", pady=(0, 6))
        tb.Button(bar, text="Start", bootstyle="success", width=12, command=self.start).pack(side="left")
        tb.Button(bar, text="Stop", bootstyle="danger-outline", width=12, command=self.app.stop_task).pack(
            side="left", padx=8)
        tb.Label(bar, text="On the PS1: Left/Right previous/next, Up/Down volume", bootstyle="secondary").pack(
            side="left", padx=8)
        self.speed = tb.Label(bar, text="", bootstyle="info")
        self.speed.pack(side="right")
        self.log = MonoText(self, height=8)
        self.log.pack(fill="both", expand=True)
        self.after(200, self._poll_log)

    def _name_for(self, source):
        try:
            return _server_module().file_name(source).upper()
        except ImportError:
            return os.path.splitext(os.path.basename(source.split("?")[0]))[0].upper()

    def add_file(self):
        for path in filedialog.askopenfilenames(filetypes=AUDIO_TYPES, parent=self):
            self.table.insert("", "end", values=(self._name_for(path), os.path.normpath(path), ""))
        self._save()

    def add_link(self):
        url = simpledialog.askstring("Add a link", "Station or audio link (http, https, or anything FFmpeg opens):",
                                     parent=self)
        if url and url.strip():
            self.table.insert("", "end", values=(self._name_for(url.strip()), url.strip(), ""))
            self._save()

    def args(self, port=7777):
        return super().args(port)

    def options(self):
        return types.SimpleNamespace(rate=int(self.rate.get() or 22050))

    def start(self):
        items = self.items()
        if not items:
            self.app.error("Add a station link or an audio file first.")
            return
        self.app.settings.set_field("audio.rate", self.rate.get())
        try:
            audio_server = _server_module()
        except ImportError as e:
            self.app.error("The audio server needs miniaudio and NumPy (and PyAV for files):\n\n"
                           "pip install miniaudio numpy av\n\n(%s)" % e)
            return
        self._stop_server()
        playlist = [(v[0], v[1]) for v in items]
        self.server = audio_server.Server(playlist, self.options(), 0, self.log_queue.put)
        self.log.append("\nAudio server on 127.0.0.1:%d, %d in the playlist\n" % (self.server.port, len(playlist)))
        self.app.run(self.args(self.server.port), "Audio streamer", long_running=True, stdin=True,
                     on_line=self._bridge_line, log_filter=lambda line: "B/s" not in line,
                     on_done=lambda code, out: self._stop_server())
