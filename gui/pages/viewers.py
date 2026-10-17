"""
PS1 DEV CART GUI: Viewers page (TMD model viewer with textures, TIM and VRAM viewer with the palette editor).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import tkinter as tk

import ttkbootstrap as tb

from pages.base import Page
from widgets import FileField, FileList

TIM_TYPES = [("TIM images", "*.tim"), ("All files", "*.*")]


class ViewersPage(Page):
    title = "Viewers"

    def build(self):
        tb.Label(self, text="Viewers", font=("", 16, "bold")).pack(anchor="w", pady=(0, 8))
        cols = tb.Frame(self)
        cols.pack(fill="both", expand=True)
        cols.columnconfigure((0, 1), weight=1, uniform="v")
        cols.rowconfigure(0, weight=1)

        box = self.section("Model viewer", cols)
        box.grid(row=0, column=0, sticky="nsew", padx=(0, 6))
        tb.Label(box, text="A TMD model with its TIM textures (loaded at their VRAM position). On the PS1: rotate "
                           "and zoom with the pad.", wraplength=400, justify="left", bootstyle="secondary").pack(
            anchor="w", pady=(0, 6))
        self.tmd = FileField(box, self.app, "view.tmd", "Model (TMD)", width=30,
                             filetypes=[("TMD models", "*.tmd"), ("All files", "*.*")])
        self.tmd.pack(fill="x", pady=2)
        self.textures = FileList(box, self.app, "view.textures", "Textures", filetypes=TIM_TYPES, max_files=8)
        self.textures.pack(fill="both", expand=True, pady=(6, 6))
        tb.Button(box, text="Show the model", bootstyle="primary", command=self.model).pack(anchor="w")
        self.track(box, "model")

        box = self.section("TIM and VRAM viewer", cols)
        box.grid(row=0, column=1, sticky="nsew")
        tb.Label(box, text="VRAM map with overlaps and warnings, TIM view with its palettes, palette editor. On the "
                           "PS1: Select changes the view, Circle opens the palette editor.",
                 wraplength=400, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 6))
        self.tims = FileList(box, self.app, "view.tims", "TIM files", filetypes=TIM_TYPES, max_files=16)
        self.tims.pack(fill="both", expand=True, pady=(0, 6))
        self.server = tk.BooleanVar(value=self.app.settings.field("view.server", True))
        tb.Checkbutton(box, text="Palette editor save / reload (file server, keeps running)", variable=self.server,
                       bootstyle="round-toggle").pack(anchor="w", pady=(0, 6))
        tb.Button(box, text="Show the TIM files", bootstyle="primary", command=self.tim).pack(anchor="w")
        self.track(box, "tim")
        self.action = "model"

    def args(self):
        if self.action == "tim":
            if not self.tims.files():
                return None
            a = []
            for path in self.tims.files():
                a += ["-tim", path]
            if self.server.get():
                a.append("-c")
            return a
        if not self.tmd.get():
            return None
        a = ["-m", self.tmd.get()]
        for path in self.textures.files():
            a += ["-t", path]
        return a

    def model(self):
        self.action = "model"
        if not os.path.isfile(self.tmd.get()):
            return self.app.error("Choose the TMD model.")
        self.tmd.remember()
        self.app.run(self.args(), "Model viewer")

    def tim(self):
        self.action = "tim"
        if not self.tims.files():
            return self.app.error("Add the TIM files to show.")
        self.app.settings.set_field("view.server", self.server.get())
        server = self.server.get()
        self.app.run(self.args(), "TIM viewer", long_running=server, console=server,
                     cwd=os.path.dirname(self.tims.files()[0]))
