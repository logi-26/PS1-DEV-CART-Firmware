"""
PS1 DEV CART GUI: Sound page (VH/VB/VAB banks, SEQ music and VAG samples played by the sound tester).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import ttkbootstrap as tb

from pages.base import Page
from widgets import FileList

SOUND_TYPES = [("Sound files", "*.vh *.vb *.vab *.seq *.vag"), ("All files", "*.*")]


class SoundPage(Page):
    title = "Sound"

    def build(self):
        tb.Label(self, text="Sound tester", font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="Plays sound banks (VH + VB, or VAB), SEQ music with its bank, and VAG samples on the "
                            "PS1. The tester shows the controls on the screen.",
                 wraplength=780, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 8))
        self.files = FileList(self, self.app, "sound.files", "Sound files", filetypes=SOUND_TYPES, max_files=8,
                              height=10)
        self.files.pack(fill="both", expand=True, pady=(0, 8))
        tb.Button(self, text="Play on the PS1", bootstyle="primary", command=self.play).pack(anchor="w")

    def args(self):
        if not self.files.files():
            return None
        a = []
        for path in self.files.files():
            a += ["-snd", path]
        return a

    def play(self):
        if not self.files.files():
            return self.app.error("Add the sound files.")
        self.app.run(self.args(), "Sound tester")
