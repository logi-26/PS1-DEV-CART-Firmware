"""
PS1 DEV CART GUI: Debug page (crash report, GDB server for GDB / VS Code).

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
from pages.memory import SYMBOL_TYPES
from settings import config_dir
from widgets import FileField, ImageView, MonoText, PS1_EXE_TYPES, labeled


class DebugPage(Page):
    title = "Debug Server"

    def build(self):
        s = self.app.settings
        tb.Label(self, text=self.title, font=("", 16, "bold")).pack(anchor="w", pady=(0, 8))

        box = self.section("Crash report")
        box.pack(fill="x", pady=(0, 8))
        tb.Label(box, text="When the PS1 DEV CART crash screen is shown: cause, registers and call stack with function names.",
                 bootstyle="secondary").pack(anchor="w")
        self.sym = FileField(box, self.app, "debug.sym", "Symbol file", filetypes=SYMBOL_TYPES)
        self.sym.pack(fill="x", pady=4)
        tb.Button(box, text="Get the crash report", bootstyle="primary", command=self.crash).pack(anchor="w")
        self.track(box, "crash")

        box = self.section("GDB server (GDB, VS Code)")
        box.pack(fill="x", pady=(0, 8))
        tb.Label(box, text="Loads the program stopped at its entry point, then GDB connects to localhost:port. "
                           "Stop it here or from the debugger (detach: the program runs on; kill: the PS1 is reset).",
                 bootstyle="secondary", wraplength=760, justify="left").pack(anchor="w")
        self.exe = FileField(box, self.app, "debug.exe", "Executable", filetypes=PS1_EXE_TYPES)
        self.exe.pack(fill="x", pady=4)
        row = tb.Frame(box)
        row.pack(fill="x", pady=2)
        f, self.port = labeled(row, "Port", tb.Spinbox, from_=1024, to=65535, field_width=8)   # (label as the file fields)
        self.port.set(s.field("debug.port", "3333"))
        f.pack(side="left")
        tb.Label(row, text="printf / TTY").pack(side="left", padx=(20, 6))
        self.tty = tk.StringVar(value=s.field("debug.tty", "usb"))
        for value, text in (("usb", "USB"), ("bios", "BIOS"), ("sio", "Serial port")):
            tb.Radiobutton(row, text=text, value=value, variable=self.tty).pack(side="left", padx=4)
        row = tb.Frame(box)
        row.pack(fill="x", pady=(6, 0))
        tb.Button(row, text="Start the GDB server", bootstyle="success", command=self.gdb).pack(side="left")
        tb.Button(row, text="Stop", bootstyle="danger-outline", command=self.app.stop_task).pack(side="left", padx=6)
        tb.Label(row, text="GDB:").pack(side="left", padx=(16, 4))
        self.connect = tb.Entry(row, width=34)
        self.connect.pack(side="left")
        tb.Button(row, text="Copy", bootstyle="secondary-outline", command=self._copy).pack(side="left", padx=4)
        self.track(box, "gdb")
        self.port.bind("<KeyRelease>", lambda e: self._connect_text(), add="+")
        self.port.configure(command=self._connect_text)
        self._connect_text()

        tb.Label(self, text="Result").pack(anchor="w")
        row = tb.Frame(self)
        row.pack(fill="both", expand=True)
        # Last frame the crashed program displayed (kept in VRAM: the crash screen is drawn in the other buffer)
        self.frame_box = tb.Frame(row)
        tb.Label(self.frame_box, text="Last frame before the crash").pack(anchor="w")
        tb.Label(self.frame_box, text="(double buffered programs)", bootstyle="secondary").pack(anchor="w")
        self.frame_view = ImageView(self.frame_box, width=320, height=240)
        self.frame_view.pack(fill="both", expand=True, pady=4)
        tb.Button(self.frame_box, text="Save as...", bootstyle="secondary-outline",
                  command=self._save_frame).pack(anchor="w")
        self.result = MonoText(row, height=14)
        self.result.pack(side="left", fill="both", expand=True)
        self.frame_png = os.path.join(config_dir(), "crash_frame.png")
        self.action = "crash"

    def _connect_text(self):
        self.connect.delete(0, "end")
        self.connect.insert(0, "target remote localhost:%s" % self.port.get())

    def _copy(self):
        self.clipboard_clear()
        self.clipboard_append(self.connect.get())

    def args(self):
        if self.action == "gdb":
            a = ["-gdb", self.port.get(), "-f", self.exe.get()]
            if self.tty.get() == "bios":
                a += ["-c", "off"]
            elif self.tty.get() == "sio":
                a += ["-c", "sio"]
            return a
        a = ["-crash"]
        if self.sym.get():
            a.append(self.sym.get())
        return a + ["-f", self.frame_png]

    def crash(self):
        self.action = "crash"
        self.sym.remember()
        self.result.clear()
        self.frame_box.pack_forget()
        os.makedirs(os.path.dirname(self.frame_png), exist_ok=True)
        if os.path.isfile(self.frame_png):
            os.remove(self.frame_png)
        self.app.run(self.args(), "Crash report", on_line=lambda line: self.result.append(line + "\n"),
                     on_done=self._crash_done)

    def _crash_done(self, code, output):
        from PIL import Image
        if code == 0 and os.path.isfile(self.frame_png):
            with Image.open(self.frame_png) as im:
                self.frame_view.show(im.convert("RGB"))
            self.frame_box.pack(side="right", fill="y", padx=(8, 0))

    def _save_frame(self):
        path = filedialog.asksaveasfilename(defaultextension=".png", filetypes=[("PNG", "*.png")],
                                            initialfile="crash_frame.png", parent=self)
        if path:
            shutil.copyfile(self.frame_png, path)

    def gdb(self):
        self.action = "gdb"
        if not os.path.isfile(self.exe.get()):
            return self.app.error("Choose the executable to debug.")
        self.exe.remember()
        self.app.settings.set_field("debug.port", self.port.get())
        self.app.settings.set_field("debug.tty", self.tty.get())
        self.result.clear()
        self.app.run(self.args(), "GDB server", long_running=True, cwd=os.path.dirname(self.exe.get()),
                     on_line=lambda line: self.result.append(line + "\n"))
