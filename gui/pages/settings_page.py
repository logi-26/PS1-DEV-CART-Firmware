"""
PS1 DEV CART GUI: Settings page (ps1transfer path, theme).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os

import ttkbootstrap as tb

from devices import GUI_DIR, default_ps1transfer
from pages.base import Page
from settings import config_dir
from widgets import FileField


class SettingsPage(Page):
    title = "Settings"

    def build(self):
        tb.Label(self, text="Settings", font=("", 16, "bold")).pack(anchor="w", pady=(0, 8))

        box = self.section("ps1transfer")
        box.pack(fill="x", pady=(0, 8))
        self.exe = FileField(box, self.app, "settings.ps1transfer", "Program",
                             filetypes=[("Programs", "*.exe"), ("All files", "*.*")])
        self.exe.set(self.app.settings.get("ps1transfer") or "")
        self.exe.pack(fill="x", pady=2)
        tb.Label(box, text="Empty: %s" % default_ps1transfer(), bootstyle="secondary").pack(anchor="w")

        box = self.section("Appearance")
        box.pack(fill="x", pady=(0, 8))
        row = tb.Frame(box)
        row.pack(fill="x")
        tb.Label(row, text="Theme", width=14, anchor="w").pack(side="left")
        self.theme = tb.Combobox(row, values=self.app.style.theme_names(), state="readonly", width=30)
        self.theme.set(self.app.settings.get("theme"))
        self.theme.pack(side="left")
        self.theme.bind("<<ComboboxSelected>>", lambda e: self.app.set_theme(self.theme.get()))

        tb.Button(self, text="Save", bootstyle="primary", command=self.save).pack(anchor="w", pady=(4, 0))
        tb.Label(self, text="Settings file: %s" % self.app.settings.path, bootstyle="secondary").pack(
            anchor="w", pady=(16, 0))

        # Licenses of the libraries in the Manager executable (FFmpeg for the video/audio streamers, see the notice)
        box = self.section("About")
        box.pack(fill="x", pady=(16, 0))
        tb.Label(box, text="PS1 Dev Cart Manager %s. Its executable contains FFmpeg (video and audio decoding for "
                           "the streamer pages) and other open source libraries." % self.app.version,
                 wraplength=760, justify="left").pack(anchor="w")
        tb.Button(box, text="Third-party licences", bootstyle="secondary-outline",
                  command=self.licenses).pack(anchor="w", pady=(6, 0))

    def licenses(self):
        notice = os.path.join(GUI_DIR, "licenses", "FFMPEG-NOTICE.txt")
        if not os.path.isfile(notice):
            return self.app.error("The licence files are missing (licenses folder next to the Manager).")
        os.startfile(os.path.dirname(notice))

    def save(self):
        self.app.settings.set("ps1transfer", self.exe.get())
        self.exe.remember()
        self.app.settings.save()
        self.app.refresh_devices()
