"""
PS1 DEV CART GUI: Run PSX and Run Yaroze pages (upload and execute a program; TTY mode, console).

- Run PSX: a PS-X EXE (PSn00bSDK, PsyQ, PsyQo... also a Net Yaroze game converted with yarexe, which includes the
  Net Yaroze library);
- Run Yaroze: a Net Yaroze game as the Net Yaroze tools make it, its auto script or ECOFF executable, run with the
  Net Yaroze library (libps.exe).
Run Linked (two cartridges or more connected): the program on the selected cartridge and another one, for link
games; with the USB Link tty, the relay is started after (as the VS Code extension does).
Each page keeps its own options (settings "run." and "runy.").

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import tkinter as tk

import ttkbootstrap as tb

from pages.base import Page
from pages.console import ConsolePage
from pages.linktest import RELAY_TASK
from programs import file_kind, program_name
from widgets import FileField, PS1_EXE_TYPES

NY_TYPES = [("Net Yaroze auto script or executable", "auto *.exe main"), ("All files", "*.*")]


class RunBase(Page):
    """A run page: its program section (program_section), the options, and ps1transfer's arguments."""
    key = "run"         # Settings prefix of the options
    heading = ""

    def build(self):
        tb.Label(self, text=self.heading, font=("", 16, "bold")).pack(anchor="w", pady=(0, 10))
        self.program_section()
        self.options_section()

    def program_section(self):
        raise NotImplementedError

    def run_buttons(self, box):
        """Run, and Run Linked (enabled with two cartridges or more)."""
        row = tb.Frame(box)
        row.pack(anchor="w", pady=(6, 0))
        tb.Button(row, text="Run", bootstyle="success", width=12, command=self.run).pack(side="left")
        self.linked_button = tb.Button(row, text="Run Linked", bootstyle="info", width=12, command=self.run_linked)
        self.linked_button.pack(side="left", padx=(8, 0))
        self.linked_hint = tb.Label(row, text="", bootstyle="secondary")
        self.linked_hint.pack(side="left", padx=(10, 0))
        self._update_linked()

    def _update_linked(self):
        serials = [d["serial"] for d in self.app.devices]
        if len(serials) >= 2:
            first = self.app.serial()
            second = next((s for s in serials if s != first), "")
            self.linked_button.configure(state="normal")
            self.linked_hint.configure(text="On %s and %s" % (first, second))
        else:
            self.linked_button.configure(state="disabled")
            self.linked_hint.configure(text="Run Linked: connect two cartridges")

    def on_show(self):
        self._update_linked()

    def on_devices_listed(self):
        self._update_linked()

    def on_device_changed(self):
        self._update_linked()

    def options_section(self):
        s = self.app.settings
        box = self.section("Options")
        box.pack(fill="x", pady=(0, 10))
        tb.Label(box, text="Where to send tty/printf:       ", foreground="yellow").grid(row=0, column=0, sticky="w")
        self.tty = tk.StringVar(value=s.field(self.key + ".tty", "usb"))
        # (in a row of their own: grid columns shared with the rows below spread them out)
        modes = tb.Frame(box)
        modes.grid(row=0, column=1, columnspan=4, sticky="w")
        for value, text in (("usb", "To this computer (USB)"), ("bios", "BIOS device"), ("sio", "Serial Port"),
                            ("link", "USB Link (2 dev carts)")):
            tb.Radiobutton(modes, text=text, value=value, variable=self.tty, command=self._update).pack(
                side="left", padx=(10, 0))
        tb.Label(box, text="Link Games: Serial Port with a link cable, USB Link without a cable "
                           "(Run Linked starts the relay)", bootstyle="secondary").grid(
            row=5, column=0, columnspan=5, sticky="w", pady=(6, 0))
        self.console = tk.BooleanVar(value=s.field(self.key + ".console", True))
        self.console_check = tb.Checkbutton(box, text="Open the console (printf, PCDRV file server)",
                                            variable=self.console, bootstyle="round-toggle")
        self.console_check.grid(row=1, column=0, columnspan=4, sticky="w", pady=(8, 0))
        self.reset = tk.BooleanVar(value=s.field(self.key + ".reset", True))
        tb.Checkbutton(box, text="Reset the PS1 first (needed if a program runs)", variable=self.reset,
                       bootstyle="round-toggle").grid(row=2, column=0, columnspan=4, sticky="w", pady=(4, 0))
        self.pcdrv = FileField(box, self.app, self.key + ".pcdrv", "PCDRV folder", mode="dir")
        self.pcdrv.grid(row=3, column=0, columnspan=4, sticky="ew", pady=(8, 0))
        tb.Label(box, text="Empty: the folder of the program", bootstyle="secondary").grid(
            row=4, column=0, columnspan=4, sticky="w")
        self.remote_pad = tk.BooleanVar(value=s.field(self.key + ".pad", True))
        tb.Checkbutton(box, text="Remote controller: port 1 or 2 can be driven from this computer (Remote Controller page)",
                       variable=self.remote_pad, bootstyle="round-toggle").grid(row=6, column=0, columnspan=5,
                                                                               sticky="w", pady=(8, 0))
        box.columnconfigure(3, weight=1)
        self._update()

    def _update(self):
        state = "normal" if self.tty.get() == "usb" else "disabled"     # No console without USB TTY
        self.console_check.configure(state=state)

    def options(self, linked=False):
        """linked: Run Linked, no console (the cartridges are each used once, then by the relay)."""
        a = ["-pad"] if self.remote_pad.get() else []
        tty = self.tty.get()
        if tty == "bios":
            a += ["-c", "off"]
        elif tty == "sio":
            a += ["-c", "sio"]
        elif tty == "link":     # Same tty driver on the USB link: run it on both consoles, then the relay (Link test)
            a += ["-c", "link"]
        elif self.console.get() and not linked:
            a.append("-c")
            if self.pcdrv.get():
                a += ["-p", self.pcdrv.get()]
        return a

    def _remember(self):
        s = self.app.settings
        for option, var in (("tty", self.tty), ("console", self.console), ("reset", self.reset),
                            ("pad", self.remote_pad)):
            s.set_field(self.key + "." + option, var.get())
        self.pcdrv.remember()

    def checked_args(self, linked=False):
        """The arguments, the files checked (an error shown, None if one is missing)."""
        raise NotImplementedError

    def program(self):
        """(path, name) of the program."""
        raise NotImplementedError

    def run(self):
        a = self.checked_args()
        if a:
            self._start(a, *self.program())

    def run_linked(self):
        """The program on the selected cartridge, then on another one; with the USB Link tty, then the relay
        between them (until it is stopped: Stop task, or the Link Connection Test page)."""
        a = self.checked_args(linked=True)
        if not a:
            return
        first = self.app.serial()
        second = next((d["serial"] for d in self.app.devices if d["serial"] != first), "")
        if not first or not second:
            self.app.error("Run Linked needs two cartridges: connect both to this computer, then Refresh.")
            return
        self._remember()
        path, name = self.program()
        cwd = self.pcdrv.get() or os.path.dirname(path) or None
        relay = self.tty.get() == "link"

        def on_second(code, out):
            if code == 0 and relay:
                self.app.run(["-relay", second], RELAY_TASK, device=first, long_running=True, stdin=True)

        def on_first(code, out):
            if code == 0:
                self.app.run(a, "Run %s on %s" % (name, second), device=second, cwd=cwd, on_done=on_second)

        self.app.run(a, "Run %s on %s" % (name, first), device=first, cwd=cwd, on_done=on_first)

    def _start(self, a, program, name=None):
        self._remember()
        console = "-c" in a and self.tty.get() == "usb"
        cwd = self.pcdrv.get() or os.path.dirname(program) or None
        self.app.run(a, "Run " + (name or os.path.basename(program)), long_running=console, console=console, cwd=cwd)
        if console:
            self.app.show_page(ConsolePage.title)


class RunPsxPage(RunBase):
    title = "Run PSX"
    key = "run"
    heading = "Run a PlayStation program"

    def program_section(self):
        box = self.section("PlayStation program (PS-X EXE)")
        box.pack(fill="x", pady=(0, 10))
        tb.Label(box, text="PsyQ, PsyQo, PSn00bSDK (also a Net Yaroze game converted with Yarexe)", bootstyle="secondary").pack(anchor="w", pady=(0, 4))
        self.exe = FileField(box, self.app, "run.exe", "Executable", filetypes=PS1_EXE_TYPES)
        self.exe.pack(fill="x", pady=2)
        self.run_buttons(box)

    def args(self, linked=False):
        """ps1transfer arguments (None: no executable)."""
        exe = self.exe.get()
        if not exe:
            return None
        return (["-r"] if self.reset.get() else []) + ["-e", "-f", exe] + self.options(linked)

    def checked_args(self, linked=False):
        a = self.args(linked)
        if not a:
            self.app.error("Choose the executable to run.")
            return None
        self.exe.remember()
        return a

    def program(self):
        return self.exe.get(), os.path.basename(self.exe.get())


class RunYarozePage(RunBase):
    title = "Run Net Yaroze"
    key = "runy"
    heading = "Run a Net Yaroze game"

    def program_section(self):
        box = self.section("Net Yaroze game (ECOFF)")
        box.pack(fill="x", pady=(0, 10))
        tb.Label(box, text="Auto script of siocons or the executable alone (a game converted with Yarexe can also be provided)", bootstyle="secondary",
                 wraplength=900, justify="left").pack(anchor="w", pady=(0, 4))
        self.ny = FileField(box, self.app, "run.ny", "Auto script", filetypes=NY_TYPES)
        self.ny.pack(fill="x", pady=2)
        self.libps = FileField(box, self.app, "run.libps", "libps.exe", filetypes=PS1_EXE_TYPES)
        self.libps.pack(fill="x", pady=2)
        tb.Label(box, text="The Net Yaroze library (LIBPS.EXE) is uploaded into RAM.", bootstyle="secondary").pack(anchor="w")
        self.run_buttons(box)

    def args(self, linked=False):
        """ps1transfer arguments: an auto script (-auto) or an ECOFF executable (-e), with the library; a PS-X EXE
        (converted with yarexe) runs as one, without it. None: no file."""
        path = self.ny.get()
        if not path:
            return None
        kind = file_kind(path)
        a = ["-r"] if self.reset.get() else []
        if kind == "psx":
            return a + ["-e", "-f", path] + self.options(linked)
        a += ["-auto", path] if kind == "auto" else ["-e", "-f", path]
        return a + ["-y", self.libps.get()] + self.options(linked)

    def checked_args(self, linked=False):
        a = self.args(linked)
        if not a:
            self.app.error("Choose the auto script or the executable of the Net Yaroze game.")
            return None
        if "-y" in a and not os.path.isfile(self.libps.get()):
            self.app.error("Choose libps.exe, the Net Yaroze library (MON\\LIBPS.EXE on the Net Yaroze boot disc).")
            return None
        self.ny.remember()
        if "-y" in a:
            self.libps.remember()
        return a

    def program(self):
        return self.ny.get(), program_name(self.ny.get())   # (an auto script: its folder's name)
