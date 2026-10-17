"""
PS1 DEV CART GUI: Run page (upload and execute a program, TTY mode, console, Net Yaroze library).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import tkinter as tk

import ttkbootstrap as tb

from pages.base import Page
from widgets import FileField, PS1_EXE_TYPES


class RunPage(Page):
    title = "Run"

    def build(self):
        s = self.app.settings
        tb.Label(self, text="Run a program", font=("", 16, "bold")).pack(anchor="w", pady=(0, 10))

        box = self.section("Program")
        box.pack(fill="x", pady=(0, 10))
        self.exe = FileField(box, self.app, "run.exe", "Executable", filetypes=PS1_EXE_TYPES)
        self.exe.pack(fill="x", pady=2)
        row = tb.Frame(box)
        row.pack(fill="x", pady=2)
        self.use_libps = tk.BooleanVar(value=s.field("run.use_libps", False))
        tb.Checkbutton(row, text="Upload the Net Yaroze library first", variable=self.use_libps,
                       bootstyle="round-toggle", command=self._update).pack(side="left")
        self.libps = FileField(box, self.app, "run.libps", "libps.exe", filetypes=PS1_EXE_TYPES)
        self.libps.pack(fill="x", pady=2)

        box = self.section("Options")
        box.pack(fill="x", pady=(0, 10))
        tb.Label(box, text="printf / TTY").grid(row=0, column=0, sticky="w")
        self.tty = tk.StringVar(value=s.field("run.tty", "usb"))
        # (in a row of their own: grid columns shared with the rows below spread them out)
        modes = tb.Frame(box)
        modes.grid(row=0, column=1, columnspan=4, sticky="w")
        for value, text in (("usb", "To this computer (USB)"), ("bios", "BIOS device"), ("sio", "Serial Port"),
                            ("link", "USB Link")):
            tb.Radiobutton(modes, text=text, value=value, variable=self.tty, command=self._update).pack(
                side="left", padx=(10, 0))
        tb.Label(box, text="Link games (Net Yaroze tty:): Serial Port with a link cable, USB Link without a cable "
                           "(then the relay on the Link test page, or Netplay).", bootstyle="secondary").grid(
            row=5, column=0, columnspan=5, sticky="w", pady=(6, 0))
        self.console = tk.BooleanVar(value=s.field("run.console", True))
        self.console_check = tb.Checkbutton(box, text="Open the console (printf, PCDRV file server)",
                                            variable=self.console, bootstyle="round-toggle")
        self.console_check.grid(row=1, column=0, columnspan=4, sticky="w", pady=(8, 0))
        self.reset = tk.BooleanVar(value=s.field("run.reset", True))
        tb.Checkbutton(box, text="Reset the PS1 first (needed if a program runs)", variable=self.reset,
                       bootstyle="round-toggle").grid(row=2, column=0, columnspan=4, sticky="w", pady=(4, 0))
        self.pcdrv = FileField(box, self.app, "run.pcdrv", "PCDRV folder", mode="dir")
        self.pcdrv.grid(row=3, column=0, columnspan=4, sticky="ew", pady=(8, 0))
        tb.Label(box, text="Empty: the folder of the executable", bootstyle="secondary").grid(
            row=4, column=0, columnspan=4, sticky="w")
        box.columnconfigure(3, weight=1)

        buttons = tb.Frame(self)
        buttons.pack(fill="x", pady=(4, 0))
        tb.Button(buttons, text="Run", bootstyle="success", width=12, command=self.run).pack(side="left")
        tb.Button(buttons, text="Reset the PS1", bootstyle="secondary-outline",
                  command=self.app.reset_ps1).pack(side="left", padx=8)
        self._update()

    def _update(self):
        state = "normal" if self.tty.get() == "usb" else "disabled"     # No console without USB TTY
        self.console_check.configure(state=state)
        self.libps.combo.configure(state="normal" if self.use_libps.get() else "disabled")

    def args(self):
        exe = self.exe.get()
        if not exe:
            return None
        a = []
        if self.reset.get():
            a.append("-r")
        a += ["-e", "-f", exe]
        if self.use_libps.get() and self.libps.get():
            a += ["-y", self.libps.get()]
        tty = self.tty.get()
        if tty == "bios":
            a += ["-c", "off"]
        elif tty == "sio":
            a += ["-c", "sio"]
        elif tty == "link":     # Same tty driver on the USB link: run it on both consoles, then the relay (Link test)
            a += ["-c", "link"]
        elif self.console.get():
            a.append("-c")
            if self.pcdrv.get():
                a += ["-p", self.pcdrv.get()]
        return a

    def run(self):
        a = self.args()
        if not a:
            self.app.error("Choose the executable to run.")
            return
        for f in (self.exe, self.libps, self.pcdrv):
            f.remember()
        s = self.app.settings
        for key, var in (("run.use_libps", self.use_libps), ("run.tty", self.tty), ("run.console", self.console),
                         ("run.reset", self.reset)):
            s.set_field(key, var.get())
        console = "-c" in a and self.tty.get() == "usb"
        cwd = self.pcdrv.get() or os.path.dirname(self.exe.get()) or None
        self.app.run(a, "Run " + os.path.basename(self.exe.get()), long_running=console, console=console, cwd=cwd)
        if console:
            self.app.show_page("Console")
