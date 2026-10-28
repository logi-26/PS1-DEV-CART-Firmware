"""
PS1 DEV CART GUI: Movie Tools page: STR movies (MDEC video, XA sound) made and tested, like Sony's MovConv:
- Convert: a video file (FFmpeg) or pictures (TIM, PNG, JPG...: a slide show or a frame sequence), with its sound,
  the sound of a file or none, made into an STR at the resolution, frame rate, drive speed and quality chosen;
- Inspect & preview: any STR movie (2336, 2352 or 2048-byte sectors): its sectors, frames, sound, problems, the frames
  as the MDEC decodes them, played here with the sound;
- Test on the PS1: the PS1 DEV CART VIDEO TESTER plays it on the console (the file server gives it the movie: loaded
  in RAM, or read as it plays), controlled from here.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import io
import os
import queue
import sys
import tempfile
import threading
import tkinter as tk
import wave

import ttkbootstrap as tb

from pages.base import Page
from widgets import FileField, FileList, ImageView, ScrollFrame, labeled

TESTER_TASK = "Video tester"
STR_TYPES = [("STR movies", "*.str *.STR"), ("All files", "*.*")]
FMT_CHOICES = ["2336: video + XA sound (disc images: mkpsxiso \"str\")", "2048: video only (a plain data file)"]


class MoviePage(Page):
    title = "Movie Tools"

    def build(self):
        s = self.app.settings
        self.job = None
        self.result = None
        self.movie = None               # strmovie.StrFile of the Inspect tab
        self.previews = None            # JPEGs of the converted frames (else decoded from the movie)
        self.cache = {}
        self.playing_job = None
        self.tester_ready = False
        # Events of the threads (conversion, FFmpeg download, the tester's lines), handled here: Tk isn't called from
        # them
        self.events = queue.Queue()
        self.after(50, self._poll_events)

        tb.Label(self, text="Movie tools", font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="STR movies, as PlayStation games play them from the CD: MDEC video frames and XA ADPCM "
                            "sound. Convert videos or pictures, check and preview STR files, and play them on the PS1.",
                 wraplength=1000, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 6))
        self.tabs = tb.Notebook(self)
        self.tabs.pack(fill="both", expand=True)
        self._convert_tab(s)
        self._inspect_tab(s)
        self._test_tab(s)

    def _post(self, fn, *args):
        self.events.put((fn, args))

    def _poll_events(self):
        try:
            while True:
                fn, args = self.events.get_nowait()
                fn(*args)
        except queue.Empty:
            pass
        self.after(50, self._poll_events)

    # ---------------------------------------------------------------- Convert

    def _convert_tab(self, s):
        scroll = ScrollFrame(self.tabs)
        self.tabs.add(scroll, text="Convert")
        top = scroll.inner

        box = self.section("Source", top)
        box.pack(fill="x", pady=(4, 6), padx=4)
        self.source = tk.StringVar(value=s.field("movie.source", "video"))
        row = tb.Frame(box)
        row.pack(fill="x")
        for v, t in (("video", "A video file (FFmpeg)"), ("pictures", "Pictures: TIM, PNG, JPG, BMP")):
            tb.Radiobutton(row, text=t, value=v, variable=self.source, command=self._update).pack(side="left",
                                                                                                padx=(0, 16))
        self.video = FileField(box, self.app, "movie.video", "Video", filetypes=_types("VIDEO"),
                               on_change=lambda *a: self._probe())
        self.video.pack(fill="x", pady=(6, 2))
        self.video_info = tb.Label(box, text="", bootstyle="secondary")
        self.video_info.pack(anchor="w")
        self.pictures = FileList(box, self.app, "movie.pictures", "Pictures (in order)", filetypes=_types("IMAGE"),
                                 height=4, compact=True)
        self.pictures.pack(fill="x", pady=(6, 2))
        row = tb.Frame(box)
        row.pack(fill="x", pady=(2, 0))
        self.seconds = self._spin(row, "Seconds each", "movie.seconds", 3.0, 0, 60, 0.5,
                                  "0: one frame each (a sequence of frames)")
        self.fade = self._spin(row, "Fade", "movie.fade", 0.5, 0, 10, 0.25, "seconds of cross-fade into the next one")
        row = tb.Frame(box)
        row.pack(fill="x", pady=(6, 0))
        self.start = self._spin(row, "Start (s)", "movie.start", 0.0, 0, 36000, 1, "")
        self.duration = self._spin(row, "Length (s)", "movie.duration", 0.0, 0, 36000, 1, "0: up to the end")

        box = self.section("Picture", top)
        box.pack(fill="x", pady=(0, 6), padx=4)
        row = tb.Frame(box)
        row.pack(fill="x")
        f, self.resolution = labeled(row, "Resolution", tb.Combobox, width=12, field_width=44,
                                     values=[t for _, t in _resolutions()] + ["Custom (below)"], state="readonly")
        f.pack(side="left")
        self.resolution.set(s.field("movie.resolution", _resolutions()[0][1]))
        self.resolution.bind("<<ComboboxSelected>>", lambda e: self._update())
        self.custom_w = self._spin(row, "W", "movie.custom_w", 320, 16, 640, 16, "", width=5, label_width=3)
        self.custom_h = self._spin(row, "H", "movie.custom_h", 240, 16, 480, 16, "x16", width=5, label_width=3)
        row = tb.Frame(box)
        row.pack(fill="x", pady=(6, 0))
        f, self.aspect = labeled(row, "TV shape", tb.Combobox, width=12, field_width=24, state="readonly",
                                 values=["4:3", "16:9 (anamorphic)"])
        f.pack(side="left")
        self.aspect.set(s.field("movie.aspect", "4:3"))
        f, self.fit = labeled(row, "Fit", tb.Combobox, width=6, field_width=30, state="readonly",
                              values=["Letterbox (black bars)", "Crop (fill the screen)", "Stretch"])
        f.pack(side="left", padx=(16, 0))
        self.fit.set(s.field("movie.fit", "Letterbox (black bars)"))
        row = tb.Frame(box)
        row.pack(fill="x", pady=(6, 0))
        f, self.fps = labeled(row, "Frame rate", tb.Combobox, width=12, field_width=10,
                              values=["10", "12", "12.5", "15", "20", "24", "25", "30"])
        f.pack(side="left")
        self.fps.set(s.field("movie.fps", "15"))
        self.fps.bind("<<ComboboxSelected>>", lambda e: self._update())
        self.fps.bind("<KeyRelease>", lambda e: self._update())
        f, self.speed = labeled(row, "CD speed", tb.Combobox, width=10, field_width=18, state="readonly",
                                values=["2x (150 sectors/s)", "1x (75 sectors/s)"])
        f.pack(side="left", padx=(16, 0))
        self.speed.set(s.field("movie.speed", "2x (150 sectors/s)"))
        self.speed.bind("<<ComboboxSelected>>", lambda e: self._update())
        row = tb.Frame(box)
        row.pack(fill="x", pady=(6, 0))
        self.quality = tk.StringVar(value=s.field("movie.quality", "fit"))
        tb.Label(row, text="Quality", width=12, anchor="w").pack(side="left")
        tb.Radiobutton(row, text="The best that fits its sectors, quantization from", value="fit",
                       variable=self.quality).pack(side="left")
        self.scale = self._spin(row, "", "movie.scale", 1, 1, 63, 1, "(1: finest)", width=4, label_width=0)
        tb.Radiobutton(row, text="Fixed quantization (the scale above)", value="fixed",
                       variable=self.quality).pack(side="left", padx=(12, 0))

        box = self.section("Sound", top)
        box.pack(fill="x", pady=(0, 6), padx=4)
        self.sound = tk.StringVar(value=s.field("movie.sound", "video"))
        row = tb.Frame(box)
        row.pack(fill="x")
        for v, t in (("video", "The video's"), ("file", "A sound file (WAV, or others with FFmpeg)"), ("none", "None")):
            tb.Radiobutton(row, text=t, value=v, variable=self.sound, command=self._update).pack(side="left",
                                                                                               padx=(0, 16))
        self.sound_file = FileField(box, self.app, "movie.sound_file", "Sound file", filetypes=_types("SOUND"))
        self.sound_file.pack(fill="x", pady=(6, 2))
        row = tb.Frame(box)
        row.pack(fill="x", pady=(4, 0))
        f, self.rate = labeled(row, "Rate", tb.Combobox, width=12, field_width=10, state="readonly",
                               values=["37800", "18900"])
        f.pack(side="left")
        self.rate.set(s.field("movie.rate", "37800"))
        self.rate.bind("<<ComboboxSelected>>", lambda e: self._update())
        self.stereo = tk.BooleanVar(value=s.field("movie.stereo", True))
        tb.Checkbutton(row, text="Stereo", variable=self.stereo, bootstyle="round-toggle",
                       command=self._update).pack(side="left", padx=(16, 0))
        self.gain = self._spin(row, "Volume %", "movie.gain", 100, 0, 400, 10, "", width=5)

        box = self.section("Output", top)
        box.pack(fill="x", pady=(0, 6), padx=4)
        self.out = FileField(box, self.app, "movie.out", "STR file", mode="save", filetypes=STR_TYPES)
        self.out.pack(fill="x", pady=2)
        f, self.fmt = labeled(box, "Sectors", tb.Combobox, state="readonly", values=FMT_CHOICES, field_width=60)
        f.pack(anchor="w", pady=(4, 0))
        self.fmt.set(s.field("movie.fmt", FMT_CHOICES[0]))
        self.fmt.bind("<<ComboboxSelected>>", lambda e: self._update())
        self.budget = tb.Label(box, text="", bootstyle="info", wraplength=950, justify="left")
        self.budget.pack(anchor="w", pady=(6, 0))
        row = tb.Frame(box)
        row.pack(fill="x", pady=(6, 0))
        tb.Label(row, text="FFmpeg", width=12, anchor="w").pack(side="left")
        self.ffmpeg_label = tb.Label(row, text="", bootstyle="secondary")
        self.ffmpeg_label.pack(side="left")
        tb.Button(row, text="Browse...", bootstyle="secondary-outline", command=self._browse_ffmpeg).pack(
            side="right")
        self.get_ffmpeg = tb.Button(row, text="Get FFmpeg", bootstyle="info-outline", command=self._download_ffmpeg)
        self.get_ffmpeg.pack(side="right", padx=6)
        row = tb.Frame(box)
        row.pack(fill="x", pady=(10, 0))
        self.convert_button = tb.Button(row, text="Convert", bootstyle="success", width=12, command=self.convert)
        self.convert_button.pack(side="left")
        tb.Button(row, text="Stop", bootstyle="danger-outline", width=8, command=self.stop_convert).pack(
            side="left", padx=6)
        self.progress = tb.Progressbar(row, maximum=100, length=260)
        self.progress.pack(side="left", padx=(12, 8))
        self.status = tb.Label(row, text="", bootstyle="secondary")
        self.status.pack(side="left")
        for var in (self.source, self.sound, self.quality, self.stereo):
            var.trace_add("write", lambda *a: self._update())
        self._update()

    def _spin(self, parent, label, key, default, lo, hi, step, hint, width=7, label_width=None):
        frame = tb.Frame(parent)
        frame.pack(side="left", padx=(0, 16))
        if label:
            tb.Label(frame, text=label, width=label_width if label_width is not None else max(len(label) + 1, 6),
                     anchor="w").pack(side="left")
        var = tk.StringVar(value=str(self.app.settings.field(key, default)))
        spin = tb.Spinbox(frame, from_=lo, to=hi, increment=step, width=width, textvariable=var,
                          command=self._update)
        spin.pack(side="left")
        spin.bind("<KeyRelease>", lambda e: self._update())
        if hint:
            tb.Label(frame, text=hint, bootstyle="secondary").pack(side="left", padx=(4, 0))
        var.key = key
        return var

    @staticmethod
    def _num(var, default=0.0):
        try:
            return float(var.get())
        except (ValueError, tk.TclError):
            return default

    def _size(self):
        text = self.resolution.get()
        for key, label in _resolutions():
            if label == text:
                w, h = key.split("x")
                return int(w), int(h)
        w = int(self._num(self.custom_w, 320)) // 16 * 16
        h = int(self._num(self.custom_h, 240)) // 16 * 16
        return max(16, min(640, w)), max(16, min(480, h))

    def _speed(self):
        return 1 if self.speed.get().startswith("1") else 2

    def _fmt(self):
        return 2048 if self.fmt.get().startswith("2048") else 2336

    def _ffmpeg(self):
        import movieconv
        return movieconv.find_ffmpeg(self.app.settings.field("movie.ffmpeg", ""))

    def _update(self):
        """The fields that apply, and the sector budget of a frame."""
        import strmovie
        pictures = self.source.get() == "pictures"
        for w in (self.video,):
            _enable(w, not pictures)
        _enable(self.pictures, pictures)
        _enable(self.sound_file, self.sound.get() == "file")
        ff = self._ffmpeg()
        self.ffmpeg_label.configure(text=ff or "not found: videos and most sound files need it (WAV, TIM and "
                                               "pictures don't)")
        w, h = self._size()
        fps = self._num(self.fps, 15) or 15
        speed = self._speed()
        rate = 75 * speed
        sound = self.sound.get() != "none" and self._fmt() == 2336
        inter = strmovie.xa_interleave(int(self.rate.get()), self.stereo.get(), speed) if sound else 0
        video = rate * (1 - 1 / inter) if inter else rate
        per_frame = video / fps
        text = "%dx%d at %g fps, %dx: %.1f video sectors a frame (%.1f KB of picture data)" % (
            w, h, fps, speed, per_frame, per_frame * strmovie.VIDEO_PAYLOAD / 1024)
        if inter:
            text += ", sound %s Hz %s: 1 sector in %d" % (self.rate.get(), "stereo" if self.stereo.get() else "mono",
                                                          inter)
        text += ". %d KB/s" % (rate * (2336 if self._fmt() == 2336 else 2048) // 1024)
        if per_frame < 2:
            text += ". Very few sectors a frame: a low frame rate or 2x speed gives better pictures."
        if w > 320:
            text += " Wider than 320: the video tester shows it at half width."
        self.budget.configure(text=text)

    def _probe(self):
        import movieconv
        ff = self._ffmpeg()
        path = self.video.get()
        if not ff or not path or not os.path.isfile(path):
            self.video_info.configure(text="")
            return
        dur, fps, sound = movieconv.probe(ff, path)
        self.video_info.configure(text="%.1f s, %g fps, %s" % (dur, fps, "with sound" if sound else "no sound"))

    def _browse_ffmpeg(self):
        from tkinter import filedialog
        path = filedialog.askopenfilename(title="FFmpeg", filetypes=[("FFmpeg", "ffmpeg*"), ("All files", "*.*")])
        if path:
            self.app.settings.set_field("movie.ffmpeg", path)
            self._update()

    def _download_ffmpeg(self):
        import movieconv
        if not self.app.confirm("Download FFmpeg (about 100 MB, the essentials build of gyan.dev, GPL licence) into "
                                "%s?" % os.path.join(movieconv.app_dir(), "ffmpeg")):
            return
        self.get_ffmpeg.configure(state="disabled")

        def prog(done, total):
            self._post(lambda: self.status.configure(text="FFmpeg: %d / %d MB" % (done >> 20, total >> 20)))

        def work():
            try:
                path = movieconv.download_ffmpeg(prog)
                self._post(lambda: (self.status.configure(text="FFmpeg: %s" % path), self._update(), self._probe()))
            except Exception as e:      # noqa: BLE001
                err = str(e)
                self._post(lambda: self.status.configure(text="FFmpeg download failed: %s" % err))
            self._post(lambda: self.get_ffmpeg.configure(state="normal"))
        threading.Thread(target=work, daemon=True).start()

    def _save_settings(self):
        s = self.app.settings
        for key, var in (("source", self.source), ("sound", self.sound), ("quality", self.quality),
                         ("stereo", self.stereo)):
            s.set_field("movie." + key, var.get())
        for key, combo in (("resolution", self.resolution), ("aspect", self.aspect), ("fit", self.fit),
                           ("fps", self.fps), ("speed", self.speed), ("rate", self.rate), ("fmt", self.fmt)):
            s.set_field("movie." + key, combo.get())
        for var in (self.seconds, self.fade, self.start, self.duration, self.custom_w, self.custom_h, self.scale,
                    self.gain):
            s.set_field(var.key, var.get())
        for f in (self.video, self.sound_file, self.out):
            f.remember()

    def convert(self):
        import movieconv
        if self.job:
            return
        pictures = self.source.get() == "pictures"
        out = self.out.get()
        if not out:
            return self.app.error("Choose the STR file to write (Output).")
        if not out.lower().endswith(".str"):
            out += ".str"
            self.out.set(out)
        ff = self._ffmpeg()
        if pictures and not self.pictures.files():
            return self.app.error("Add the pictures of the movie.")
        if not pictures and not (self.video.get() and os.path.isfile(self.video.get())):
            return self.app.error("Choose the video file.")
        if not pictures and not ff:
            return self.app.error("Videos are read with FFmpeg: Get FFmpeg, or Browse to choose it.")
        sound = self.sound.get()
        if pictures and sound == "video":
            sound = "none"
        if sound == "file" and not os.path.isfile(self.sound_file.get()):
            return self.app.error("Choose the sound file.")
        w, h = self._size()
        fps = self._num(self.fps, 15)
        if not 1 <= fps <= 60:
            return self.app.error("The frame rate: 1 to 60.")
        self._save_settings()
        fit = {"L": "fit", "C": "crop"}.get(self.fit.get()[:1], "stretch")
        opts = {"source": self.source.get(), "path": self.video.get(), "paths": self.pictures.files(), "w": w, "h": h,
                "fps": fps, "aspect": "16:9" if self.aspect.get().startswith("16") else "4:3", "fit": fit,
                "speed": self._speed(), "fmt": self._fmt(), "quality": self.quality.get(),
                "scale": int(self._num(self.scale, 1)), "sound": sound, "sound_path": self.sound_file.get(),
                "rate": int(self.rate.get()), "stereo": self.stereo.get(), "gain": self._num(self.gain, 100) / 100,
                "start": self._num(self.start), "duration": self._num(self.duration),
                "seconds": self._num(self.seconds), "fade": self._num(self.fade), "out": out, "ffmpeg": ff}
        self.convert_button.configure(state="disabled")
        self.progress.configure(value=0)
        self.status.configure(text="Starting...")
        self.job = movieconv.Job(opts, lambda d, t, text: self._post(self._job_progress, d, t, text),
                                 lambda err, res: self._post(self._job_done, err, res))

    def stop_convert(self):
        if self.job:
            self.job.stop()

    def _job_progress(self, done, total, text):
        self.progress.configure(value=100.0 * done / max(total, 1))
        self.status.configure(text=text)

    def _job_done(self, err, res):
        job, self.job = self.job, None
        self.convert_button.configure(state="normal")
        if err:
            self.status.configure(text="Not converted: %s" % err)
            return
        self.progress.configure(value=100)
        scales = [st[0] for st in res["stats"]]
        sizes = [st[1] for st in res["stats"]]
        text = "%s: %d frames, %.1f s, %d sectors (%.1f MB), quantization %d-%d (average %.1f), %d bytes a frame" % (
            os.path.basename(self.out.get()), res["frames"], res["seconds"], res["sectors"], res["bytes"] / 1048576,
            min(scales), max(scales), sum(scales) / len(scales), sum(sizes) // len(sizes))
        if res["over"]:
            text += "; %d frames didn't fit at the fixed scale (coarser)" % res["over"]
        self.status.configure(text=text)
        self.app.log.append("Movie: %s\n" % text)
        self.inspect_file.set(self.out.get())
        self.test_file.set(self.out.get())
        self.open_movie(previews=job.previews)
        self.tabs.select(1)

    # ---------------------------------------------------------------- Inspect & preview

    def _inspect_tab(self, s):
        tab = tb.Frame(self.tabs, padding=6)
        self.tabs.add(tab, text="Inspect & preview")
        row = tb.Frame(tab)
        row.pack(fill="x")
        self.inspect_file = FileField(row, self.app, "movie.inspect", "STR movie", filetypes=STR_TYPES)
        self.inspect_file.pack(side="left", fill="x", expand=True)
        tb.Button(row, text="Open", bootstyle="primary", width=8, command=self.open_movie).pack(side="left", padx=6)
        self.info = tb.Label(tab, text="", wraplength=1000, justify="left")
        self.info.pack(anchor="w", pady=(6, 4))
        body = tb.Frame(tab)
        body.pack(fill="both", expand=True)
        left = tb.Frame(body)
        left.pack(side="left", fill="y")
        cols = (("f", "Frame", 54), ("first", "Sector", 58), ("n", "Sectors", 58), ("bytes", "Bytes", 64),
                ("q", "Quant.", 52), ("v", "BS", 34), ("size", "Size", 70))
        self.tree = tb.Treeview(left, columns=[c[0] for c in cols], show="headings", height=14, selectmode="browse")
        for key, text, width in cols:
            self.tree.heading(key, text=text, anchor="w")
            self.tree.column(key, width=width, anchor="w", stretch=False)
        sb = tb.Scrollbar(left, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.pack(side="left", fill="y")
        sb.pack(side="left", fill="y")
        self.tree.bind("<<TreeviewSelect>>", lambda e: self._select_frame())
        right = tb.Frame(body)
        right.pack(side="left", fill="both", expand=True, padx=(10, 0))
        self.view = ImageView(right, height=300)
        self.view.pack(fill="both", expand=True)
        self.frame_scale = tb.Scale(right, from_=0, to=0, command=lambda v: self._show_frame(int(float(v))))
        self.frame_scale.pack(fill="x", pady=(6, 0))
        row = tb.Frame(right)
        row.pack(fill="x", pady=(6, 0))
        self.play_button = tb.Button(row, text="Play here (with sound)", bootstyle="success", command=self.play_pc)
        self.play_button.pack(side="left")
        tb.Button(row, text="Stop", bootstyle="danger-outline", width=8, command=self.stop_pc).pack(side="left",
                                                                                                    padx=6)
        tb.Button(row, text="Save frame as PNG...", bootstyle="secondary-outline",
                  command=self.save_frame).pack(side="left", padx=(12, 0))
        tb.Button(row, text="Save sound as WAV...", bootstyle="secondary-outline",
                  command=self.save_wav).pack(side="left", padx=6)
        self.frame_label = tb.Label(row, text="", bootstyle="secondary")
        self.frame_label.pack(side="left", padx=12)
        self.current = 0

    def open_movie(self, previews=None):
        import strmovie
        path = self.inspect_file.get()
        if not path or not os.path.isfile(path):
            return self.app.error("Choose an STR movie.")
        try:
            self.movie = strmovie.StrFile(path)
        except (OSError, ValueError) as e:
            return self.app.error("%s: %s" % (os.path.basename(path), e))
        self.inspect_file.remember()
        self.previews = previews if previews and len(previews) == len(self.movie.frames) else None
        self.cache = {}
        m = self.movie
        speed = m.speed()
        secs = m.count / (75.0 * speed)
        info = m.audio_info()
        fps = len(m.frames) / secs if secs else 0
        sizes = sorted({(f["w"], f["h"]) for f in m.frames})
        text = "%s: %d sectors of %d bytes, %d frames %s, about %.1f fps, %.1f s at %dx, BS version %s" % (
            os.path.basename(path), m.count, m.fmt, len(m.frames),
            ", ".join("%dx%d" % s for s in sizes) or "-", fps, secs, speed,
            "/".join(str(v) for v in sorted({f["version"] for f in m.frames})) or "-")
        text += (". Sound: XA %d Hz %s, %d sectors, 1 in %d" % (info[0], "stereo" if info[1] else "mono",
                                                                 len(m.audio), m.interleave()) if info else
                 ". No sound")
        if m.problems:
            text += ". Problems: %s%s" % ("; ".join(m.problems[:3]), "..." if len(m.problems) > 3 else "")
        self.info.configure(text=text)
        self.tree.delete(*self.tree.get_children())
        for i, f in enumerate(m.frames):
            self.tree.insert("", "end", iid=str(i), values=(f["number"], f["first"], f["sectors"], f["size"],
                                                            f["scale"], f["version"], "%dx%d" % (f["w"], f["h"])))
        self.frame_scale.configure(to=max(0, len(m.frames) - 1))
        self.fps_preview = fps or 15
        self.test_file.set(path)
        if m.frames:
            self._show_frame(0)

    def _select_frame(self):
        sel = self.tree.selection()
        if sel:
            self._show_frame(int(sel[0]))

    def _frame_image(self, n):
        from PIL import Image
        if self.previews:
            return Image.open(io.BytesIO(self.previews[n]))
        if n not in self.cache:
            try:
                self.cache[n] = Image.fromarray(self.movie.decode_frame(n))
            except ValueError as e:
                self.frame_label.configure(text="Frame %d: %s" % (n + 1, e))
                return None
        return self.cache[n]

    def _show_frame(self, n):
        if not self.movie or not self.movie.frames:
            return
        n = max(0, min(n, len(self.movie.frames) - 1))
        self.current = n
        img = self._frame_image(n)
        if img is not None:
            f = self.movie.frames[n]
            # Shown at the TV's shape (4:3): the PS1's pixels aren't square
            self.view.show(img.resize((int(img.height * 4 / 3), img.height)) if f["w"] <= 640 else img)
            self.frame_label.configure(text="Frame %d / %d: %d bytes, quantization %d" % (
                n + 1, len(self.movie.frames), f["size"], f["scale"]))

    def play_pc(self):
        """The frames at the movie's rate, with the sound (Windows: winsound)."""
        if not self.movie or not self.movie.frames:
            return
        self.stop_pc()
        self._play_pos = 0
        self._play_t0 = None
        wav = self._write_wav(os.path.join(tempfile.gettempdir(), "ps1devcart_preview.wav"))
        if wav and sys.platform == "win32":
            import winsound
            winsound.PlaySound(wav, winsound.SND_FILENAME | winsound.SND_ASYNC)
        import time
        self._play_t0 = time.time()
        self._tick()

    def _tick(self):
        import time
        if self._play_t0 is None:
            return
        n = int((time.time() - self._play_t0) * self.fps_preview)
        if n >= len(self.movie.frames):
            self._play_t0 = None
            return
        self._show_frame(n)
        self.frame_scale.set(n)
        self.playing_job = self.after(max(5, int(1000 / self.fps_preview / 2)), self._tick)

    def stop_pc(self):
        self._play_t0 = None
        if self.playing_job:
            self.after_cancel(self.playing_job)
            self.playing_job = None
        if sys.platform == "win32":
            import winsound
            winsound.PlaySound(None, 0)

    def _write_wav(self, path):
        pcm = self.movie.decode_audio() if self.movie else None
        info = self.movie.audio_info() if self.movie else None
        if pcm is None or not len(pcm):
            return None
        with wave.open(path, "wb") as wf:
            wf.setnchannels(2 if info[1] else 1)
            wf.setsampwidth(2)
            wf.setframerate(info[0])
            wf.writeframes(pcm.tobytes())
        return path

    def save_frame(self):
        from tkinter import filedialog
        img = self._frame_image(self.current) if self.movie else None
        if img is None:
            return
        path = filedialog.asksaveasfilename(defaultextension=".png", filetypes=[("PNG", "*.png")],
                                            initialfile="frame%04d.png" % (self.current + 1))
        if path:
            img.save(path)

    def save_wav(self):
        from tkinter import filedialog
        if not self.movie or not self.movie.audio_info():
            return self.app.error("This movie has no sound.")
        path = filedialog.asksaveasfilename(defaultextension=".wav", filetypes=[("WAV", "*.wav")])
        if path:
            self._write_wav(path)

    # ---------------------------------------------------------------- Test on the PS1

    def _test_tab(self, s):
        tab = tb.Frame(self.tabs, padding=6)
        self.tabs.add(tab, text="Test on the PS1")
        tb.Label(tab, text="The PS1 DEV CART VIDEO TESTER plays the movie as a game plays it from the CD (the "
                           "sectors at the drive's pace, the sound with the pictures), its file from this computer "
                           "(the file server). RAM: the movie loaded first (up to 1 MB: about 3 s at 2x, 6 s at 1x), "
                           "then played exactly; Stream: read while it plays (about 400 KB/s, the console busy "
                           "meanwhile: 1x movies at their pace, 2x ones slower: stalls). Auto: RAM if it fits.",
                 wraplength=1000, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 8))
        self.test_file = FileField(tab, self.app, "movie.test", "STR movie", filetypes=STR_TYPES)
        self.test_file.pack(fill="x", pady=2)
        row = tb.Frame(tab)
        row.pack(fill="x", pady=(6, 0))
        f, self.test_mode = labeled(row, "Read", tb.Combobox, width=14, field_width=12, state="readonly",
                                    values=["Auto", "RAM", "Stream"])
        f.pack(side="left")
        self.test_mode.set(s.field("movie.test_mode", "Auto"))
        f, self.test_speed = labeled(row, "CD speed", tb.Combobox, width=9, field_width=16, state="readonly",
                                     values=["From the movie", "1x", "2x"])
        f.pack(side="left", padx=(16, 0))
        self.test_speed.set(s.field("movie.test_speed", "From the movie"))
        self.test_loop = tk.BooleanVar(value=s.field("movie.test_loop", True))
        tb.Checkbutton(row, text="Loop", variable=self.test_loop, bootstyle="round-toggle",
                       command=lambda: self.send("loop %d" % self.test_loop.get())).pack(side="left", padx=(16, 0))
        row = tb.Frame(tab)
        row.pack(fill="x", pady=(6, 0))
        f, self.test_depth = labeled(row, "Colours", tb.Combobox, width=14, field_width=30, state="readonly",
                                     values=["15 bits (with the information)", "24 bits (as games: no drawing)"])
        f.pack(side="left")
        self.test_depth.set(s.field("movie.test_depth", "15 bits (with the information)"))
        self.test_depth.bind("<<ComboboxSelected>>", lambda e: self.send("depth %s" % self.test_depth.get()[:2]))
        f, self.test_scale = labeled(row, "Picture", tb.Combobox, width=8, field_width=22, state="readonly",
                                     values=["1x", "Fit the screen", "Full screen"])
        f.pack(side="left", padx=(16, 0))
        self.test_scale.set(s.field("movie.test_scale", "Fit the screen"))
        self.test_scale.bind("<<ComboboxSelected>>", lambda e: self.send("scale %s" % self._scale_word()))
        self.test_info = tk.BooleanVar(value=s.field("movie.test_info", True))
        tb.Checkbutton(row, text="Information on the screen", variable=self.test_info, bootstyle="round-toggle",
                       command=lambda: self.send("info %d" % self.test_info.get())).pack(side="left", padx=(16, 0))
        row = tb.Frame(tab)
        row.pack(fill="x", pady=(12, 0))
        tb.Button(row, text="Play on the PS1", bootstyle="success", command=self.test_play).pack(side="left")
        for text, cmd in (("Pause / Play", "toggle"), ("Restart", "restart"), ("Next frame", "step")):
            tb.Button(row, text=text, bootstyle="secondary-outline",
                      command=lambda c=cmd: self.send(c)).pack(side="left", padx=(6, 0))
        tb.Button(row, text="Stop", bootstyle="danger-outline", width=8, command=self.test_stop).pack(side="left",
                                                                                                      padx=(6, 0))
        row = tb.Frame(tab)
        row.pack(fill="x", pady=(10, 0))
        tb.Label(row, text="Volume", width=8, anchor="w").pack(side="left")
        self.test_volume = tk.IntVar(value=s.field("movie.test_volume", 100))
        tb.Scale(row, from_=0, to=100, variable=self.test_volume, length=200,
                 command=lambda v: self._volume_changed()).pack(side="left")
        self.test_status = tb.Label(tab, text="Not running.", font=("Consolas", 10), justify="left")
        self.test_status.pack(anchor="w", pady=(12, 0))
        self._volume_job = None

    def _scale_word(self):
        return {"1": "1x", "F": "fit" if self.test_scale.get().startswith("Fit") else "full"}[self.test_scale.get()[0]]

    def _volume_changed(self):
        if self._volume_job:
            self.after_cancel(self._volume_job)
        self._volume_job = self.after(150, lambda: self.send("volume %d" % self.test_volume.get()))

    def _tester_running(self):
        task = self.app.runner.task
        return task is not None and task.name == TESTER_TASK

    def send(self, line):
        if self._tester_running():
            self.app.runner.send(line)

    def test_play(self):
        import strmovie
        path = self.test_file.get()
        if not path or not os.path.isfile(path):
            return self.app.error("Choose the STR movie to play.")
        tester = os.path.join(os.path.dirname(self.app.exe()), "videotest.ps-exe")
        if not os.path.isfile(tester):
            return self.app.error("The video tester (videotest.ps-exe) isn't next to ps1transfer.")
        self.test_file.remember()
        s = self.app.settings
        for key, w in (("test_mode", self.test_mode), ("test_speed", self.test_speed), ("test_depth", self.test_depth),
                       ("test_scale", self.test_scale)):
            s.set_field("movie." + key, w.get())
        for key, v in (("test_loop", self.test_loop), ("test_info", self.test_info),
                       ("test_volume", self.test_volume)):
            s.set_field("movie." + key, v.get())
        speed = self.test_speed.get()
        if speed.startswith("From"):
            try:
                speed = str(strmovie.StrFile(path).speed())
            except (OSError, ValueError):
                speed = "2"
        speed = speed[0]
        name = os.path.basename(path)
        self._open_line = 'open "%s" speed %s mode %s loop %d' % (name, speed, self.test_mode.get().lower(),
                                                                  self.test_loop.get())
        self.tester_ready = False
        self.test_status.configure(text="Starting the video tester...")
        folder = os.path.dirname(os.path.abspath(path))
        self.app.run(["-r", "-e", "-f", tester, "-c", "-p", folder], TESTER_TASK, long_running=True, stdin=True,
                     cwd=folder, on_line=lambda line: self._post(self._tester_line, line),
                     log_filter=lambda line: not line.startswith(("VIDTEST pos", "VIDTEST info", "VIDTEST sound")),
                     on_done=lambda code, out: self._post(self._tester_done))

    def test_stop(self):
        if self._tester_running():
            self.app.stop_task()

    def _tester_line(self, line):
        line = line.strip()
        if not line.startswith("VIDTEST"):
            return
        parts = line.split()
        what = parts[1] if len(parts) > 1 else ""
        if what == "ready" and not self.tester_ready:
            self.tester_ready = True
            for cmd in ("depth %s" % self.test_depth.get()[:2], "scale %s" % self._scale_word(),
                        "info %d" % self.test_info.get(), "volume %d" % self.test_volume.get(), self._open_line):
                self.app.runner.send(cmd)
        elif what == "open":
            self._open_text = "Playing %s: %s sectors of %s bytes, %s mode" % (parts[2], parts[3], parts[4],
                                                                              parts[5].upper())
            self.test_status.configure(text=self._open_text)
        elif what == "info" and len(parts) >= 6:
            self._info_text = "%sx%s, sound %s" % (parts[2], parts[3], ("%s Hz %s" % (
                parts[4], "stereo" if parts[5] == "1" else "mono")) if parts[4] != "0" else "none")
        elif what == "sound" and len(parts) >= 5:
            self._sound_text = "   sound underruns %s" % parts[2] if parts[2] != "0" else ""
        elif what == "pos" and len(parts) >= 11:
            f, total, dec, size, q, stalls, late, dropped, buf = parts[2:11]
            self.test_status.configure(text="%s\n%s\nFrame %s / %s   decode %d ms   %s bytes   quantization %s\n"
                                            "Late %s   dropped %s   stalls %s   %s %s%%%s" % (
                                                getattr(self, "_open_text", ""), getattr(self, "_info_text", ""), f,
                                                total, int(dec) // 1000, size, q, late, dropped, stalls,
                                                "RAM" if "RAM" in getattr(self, "_open_text", "") else "buffer", buf,
                                                getattr(self, "_sound_text", "")))
        elif what == "end":
            pass
        elif what == "error":
            self.test_status.configure(text="Video tester: %s" % " ".join(parts[2:]))

    def _tester_done(self):
        self.tester_ready = False
        self.test_status.configure(text="Not running (the movie may go on from RAM on the console).")


def _resolutions():
    import movieconv
    return movieconv.RESOLUTIONS


def _types(kind):
    import movieconv
    return getattr(movieconv, kind + "_TYPES")


def _enable(widget, on):
    """Enables or disables a compound widget (its children)."""
    state = "normal" if on else "disabled"
    stack = [widget]
    while stack:
        w = stack.pop()
        stack.extend(w.winfo_children())
        try:
            if isinstance(w, (tb.Combobox,)):
                w.configure(state="readonly" if on else "disabled")
            elif "state" in w.keys():
                w.configure(state=state)
        except tk.TclError:
            pass
