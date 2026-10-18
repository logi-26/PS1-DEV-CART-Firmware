"""
PS1 DEV CART GUI: Run page (upload and execute a program: a PS-X EXE, or a Net Yaroze game as the Net Yaroze tools
make it, its auto script or ECOFF executable with the Net Yaroze library; TTY mode, console).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import tkinter as tk

import ttkbootstrap as tb

from pages.base import Page
from programs import file_kind
from widgets import FileField, PS1_EXE_TYPES

NY_TYPES = [("Net Yaroze auto script or executable", "auto *.exe main"), ("All files", "*.*")]


class RunPage(Page):
    title = "Run"

    def build(self):
        s = self.app.settings
        tb.Label(self, text="Run a program", font=("", 16, "bold")).pack(anchor="w", pady=(0, 10))

        box = self.section("PlayStation program (PS-X EXE)")
        box.pack(fill="x", pady=(0, 10))
        tb.Label(box, text="PSn00bSDK, PsyQ... also a Net Yaroze game converted with yarexe (it includes the Net "
                           "Yaroze library).", bootstyle="secondary").pack(anchor="w", pady=(0, 4))
        self.exe = FileField(box, self.app, "run.exe", "Executable", filetypes=PS1_EXE_TYPES)
        self.exe.pack(fill="x", pady=2)
        tb.Button(box, text="Run", bootstyle="success", width=12, command=self.run).pack(anchor="w", pady=(6, 0))

        box = self.section("Net Yaroze game (ECOFF)")
        box.pack(fill="x", pady=(0, 10))
        tb.Label(box, text="As the Net Yaroze tools make it: the auto script of siocons (its \"local load\" program "
                           "and \"local dload\" data files), or the executable alone.", bootstyle="secondary",
                 wraplength=900, justify="left").pack(anchor="w", pady=(0, 4))
        self.ny = FileField(box, self.app, "run.ny", "Auto script", filetypes=NY_TYPES)
        self.ny.pack(fill="x", pady=2)
        self.libps = FileField(box, self.app, "run.libps", "libps.exe", filetypes=PS1_EXE_TYPES)
        self.libps.pack(fill="x", pady=2)
        tb.Label(box, text="The Net Yaroze library, from your Net Yaroze boot disc (MON\\LIBPS.EXE). It is uploaded "
                           "first, as the boot disc does.", bootstyle="secondary").pack(anchor="w")
        tb.Button(box, text="Run the Net Yaroze game", bootstyle="success", command=self.run_ny).pack(
            anchor="w", pady=(6, 0))

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
        tb.Label(box, text="Empty: the folder of the program", bootstyle="secondary").grid(
            row=4, column=0, columnspan=4, sticky="w")
        box.columnconfigure(3, weight=1)
        self._update()

    def _update(self):
        state = "normal" if self.tty.get() == "usb" else "disabled"     # No console without USB TTY
        self.console_check.configure(state=state)

    def options(self):
        a = []
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

    def args(self):
        """ps1transfer arguments for the PS-X EXE section (None: no executable)."""
        exe = self.exe.get()
        if not exe:
            return None
        return (["-r"] if self.reset.get() else []) + ["-e", "-f", exe] + self.options()

    def ny_args(self):
        """ps1transfer arguments for the Net Yaroze section: an auto script (-auto) or an ECOFF executable (-e), with
        the library; a PS-X EXE (converted with yarexe) runs as one, without it. None: no file."""
        path = self.ny.get()
        if not path:
            return None
        kind = file_kind(path)
        a = ["-r"] if self.reset.get() else []
        if kind == "psx":
            return a + ["-e", "-f", path] + self.options()
        a += ["-auto", path] if kind == "auto" else ["-e", "-f", path]
        return a + ["-y", self.libps.get()] + self.options()

    def _start(self, a, program):
        s = self.app.settings
        for key, var in (("run.tty", self.tty), ("run.console", self.console), ("run.reset", self.reset)):
            s.set_field(key, var.get())
        self.pcdrv.remember()
        console = "-c" in a and self.tty.get() == "usb"
        cwd = self.pcdrv.get() or os.path.dirname(program) or None
        self.app.run(a, "Run " + os.path.basename(program), long_running=console, console=console, cwd=cwd)
        if console:
            self.app.show_page("Console")

    def run(self):
        a = self.args()
        if not a:
            self.app.error("Choose the executable to run.")
            return
        self.exe.remember()
        self._start(a, self.exe.get())

    def run_ny(self):
        a = self.ny_args()
        if not a:
            self.app.error("Choose the auto script or the executable of the Net Yaroze game.")
            return
        if "-y" in a and not os.path.isfile(self.libps.get()):
            self.app.error("Choose libps.exe, the Net Yaroze library (MON\\LIBPS.EXE on the Net Yaroze boot disc).")
            return
        self.ny.remember()
        if "-y" in a:
            self.libps.remember()
        self._start(a, self.ny.get())
