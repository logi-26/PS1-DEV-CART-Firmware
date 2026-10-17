"""
PS1 DEV CART GUI: Screen page (VRAM screenshots: live while a program runs, otherwise the VRAM saved at the last boot).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import shutil
import tkinter as tk
from tkinter import filedialog

import ttkbootstrap as tb

from pages.base import Page
from settings import config_dir
from widgets import ImageView

PRESETS = [
    ("Screen at 0,0 (320x240)", (0, 0, 320, 240)),
    ("Screen at 0,240 (320x240)", (0, 240, 320, 240)),
    ("Screen at 0,256 (320x240)", (0, 256, 320, 240)),
    ("Screen at 0,0 (640x480)", (0, 0, 640, 480)),
    ("Whole VRAM (1024x512)", (0, 0, 1024, 512)),
    ("Custom", None),
]


class ScreenPage(Page):
    title = "Screen"

    def build(self):
        s = self.app.settings
        tb.Label(self, text="Screen and VRAM", font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="Live while a program runs, otherwise the VRAM saved at the last boot.",
                 bootstyle="secondary").pack(anchor="w", pady=(0, 8))

        bar = tb.Frame(self)
        bar.pack(fill="x", pady=(0, 6))
        tb.Label(bar, text="Area").pack(side="left")
        self.preset = tb.Combobox(bar, values=[p[0] for p in PRESETS], state="readonly", width=28)
        self.preset.set(s.field("screen.preset", PRESETS[0][0]))
        self.preset.pack(side="left", padx=(6, 12))
        self.preset.bind("<<ComboboxSelected>>", lambda e: self._preset())
        self.xywh = []
        for label, value in zip("xywh", s.field("screen.xywh", [0, 0, 320, 240])):
            tb.Label(bar, text=label).pack(side="left")
            sb = tb.Spinbox(bar, from_=0, to=1024, width=5)
            sb.set(value)
            sb.pack(side="left", padx=(2, 8))
            self.xywh.append(sb)

        bar = tb.Frame(self)
        bar.pack(fill="x", pady=(0, 6))
        tb.Button(bar, text="Capture", bootstyle="primary", command=self.capture).pack(side="left")
        tb.Button(bar, text="Save as...", bootstyle="secondary-outline", command=self.save).pack(side="left", padx=6)
        self.auto = tk.BooleanVar(value=False)
        tb.Checkbutton(bar, text="Refresh every", variable=self.auto, bootstyle="round-toggle",
                       command=self._auto).pack(side="left", padx=(12, 4))
        self.interval = tb.Spinbox(bar, from_=1, to=60, width=4)
        self.interval.set(2)
        self.interval.pack(side="left")
        tb.Label(bar, text="s").pack(side="left", padx=(2, 0))
        self.info = tb.Label(bar, text="", bootstyle="secondary")
        self.info.pack(side="right")

        self.view = ImageView(self)
        self.view.pack(fill="both", expand=True)
        self.png = os.path.join(config_dir(), "screenshot.png")
        self._preset()

    def _preset(self):
        """A preset fills x, y, w, h (read-only), Custom lets them be edited."""
        values = dict(PRESETS).get(self.preset.get())
        for sb, v in zip(self.xywh, values or [None] * 4):
            sb.configure(state="normal")
            if v is not None:
                sb.set(v)
            sb.configure(state="normal" if values is None else "disabled")

    def args(self):
        return ["-v"] + [sb.get() for sb in self.xywh] + ["-f", self.png]

    def capture(self, quiet=False):
        if quiet and self.app.runner.busy:     # Automatic refresh: not while another task runs
            return
        os.makedirs(os.path.dirname(self.png), exist_ok=True)
        s = self.app.settings
        s.set_field("screen.preset", self.preset.get())
        s.set_field("screen.xywh", [int(sb.get()) for sb in self.xywh])
        self.app.run(self.args(), "Screenshot", quiet=quiet, on_done=self._done)

    def _done(self, code, output):
        from PIL import Image
        if code == 0 and os.path.isfile(self.png):
            with Image.open(self.png) as im:
                self.view.show(im.convert("RGB"))
            self.info.configure(text="%s x %s at %s,%s" % (self.xywh[2].get(), self.xywh[3].get(),
                                                          self.xywh[0].get(), self.xywh[1].get()))

    def _auto(self):
        if self.auto.get():
            if self.winfo_ismapped():
                self.capture(quiet=True)
            try:
                delay = max(1, int(self.interval.get())) * 1000
            except ValueError:
                delay = 2000
            self.after(delay, self._auto)

    def save(self):
        if not os.path.isfile(self.png):
            return self.app.error("Capture a screenshot first.")
        path = filedialog.asksaveasfilename(defaultextension=".png", filetypes=[("PNG", "*.png")], parent=self)
        if path:
            shutil.copyfile(self.png, path)
