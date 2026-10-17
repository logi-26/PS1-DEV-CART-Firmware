"""
PS1 DEV CART GUI: Memory page (peek, poke, upload, download; addresses in hexadecimal or @symbol[+offset]).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import tkinter as tk

import ttkbootstrap as tb

from pages.base import Page
from widgets import FileField, MonoText, browse_space, labeled

SYMBOL_TYPES = [("Symbols (map, nm output, ELF)", "*.map *.sym *.elf"), ("All files", "*.*")]


class MemoryPage(Page):
    title = "Memory"

    def build(self):
        s = self.app.settings
        tb.Label(self, text="Memory", font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="Works while a program runs. Addresses: hexadecimal (0x80100000) or a "
                            "symbol (@player, @table+0x10).", bootstyle="secondary").pack(anchor="w", pady=(0, 8))
        self.sym = FileField(self, self.app, "mem.sym", "Symbol file", filetypes=SYMBOL_TYPES)
        self.sym.pack(fill="x", pady=(0, 2))
        tb.Label(self, text="Empty: the .map/.sym/.elf next to the last executable run", bootstyle="secondary").pack(
            anchor="w", pady=(0, 8))

        top = tb.Frame(self)
        top.pack(fill="x")
        top.columnconfigure((0, 1), weight=1, uniform="m")

        # Every field of the four panels starts at the same place (labels as wide as the file fields' labels) and ends
        # at the same place (a space as wide as the Browse buttons after the fields without one); the buttons are at
        # the bottom of their panel (the panels of a row have the same height)
        rows = []

        def row(box, label, widget_class, **kwargs):
            f, widget = labeled(box, label, widget_class, field_width=10, **kwargs)
            f.pack(fill="x", pady=2)
            rows.append(f)
            return widget

        box = self.section("Peek", top)
        box.grid(row=0, column=0, sticky="nsew", padx=(0, 6), pady=(0, 6))
        tb.Button(box, text="Peek", bootstyle="primary", command=self.peek).pack(side="bottom", anchor="w",
                                                                                  pady=(6, 0))
        self.peek_adrs = row(box, "Address", tb.Entry)
        self.peek_adrs.insert(0, s.field("mem.peek_adrs", "0x80010000"))
        self.peek_size = row(box, "Size", tb.Spinbox, from_=1, to=4096)
        self.peek_size.set(s.field("mem.peek_size", "64"))
        self.track(box, "peek")

        box = self.section("Poke", top)
        box.grid(row=0, column=1, sticky="nsew", pady=(0, 6))
        tb.Button(box, text="Poke", bootstyle="warning", command=self.poke).pack(side="bottom", anchor="w",
                                                                                  pady=(6, 0))
        self.poke_adrs = row(box, "Address", tb.Entry)
        self.poke_adrs.insert(0, s.field("mem.poke_adrs", ""))
        self.poke_value = row(box, "Value", tb.Entry)
        self.poke_size = row(box, "Size", tb.Combobox, values=("1", "2", "4"), state="readonly")
        self.poke_size.set(s.field("mem.poke_size", "4"))
        self.track(box, "poke")

        box = self.section("Upload a file to memory", top)
        box.grid(row=1, column=0, sticky="nsew", padx=(0, 6), pady=(0, 6))
        tb.Button(box, text="Upload", bootstyle="primary", command=self.upload).pack(side="bottom", anchor="w",
                                                                                      pady=(6, 0))
        self.up_file = FileField(box, self.app, "mem.up_file", "File", width=10)
        self.up_file.pack(fill="x", pady=2)
        self.up_adrs = row(box, "Address", tb.Entry)
        self.up_adrs.insert(0, s.field("mem.up_adrs", "0x80100000"))
        self.track(box, "upload")

        box = self.section("Download memory to a file", top)
        box.grid(row=1, column=1, sticky="nsew", pady=(0, 6))
        tb.Button(box, text="Download", bootstyle="primary", command=self.download).pack(side="bottom", anchor="w",
                                                                                          pady=(6, 0))
        self.down_file = FileField(box, self.app, "mem.down_file", "File", mode="save", width=10)
        self.down_file.pack(fill="x", pady=2)
        self.down_adrs = row(box, "Address", tb.Entry)
        self.down_adrs.insert(0, s.field("mem.down_adrs", "0x80010000"))
        self.down_size = row(box, "Size", tb.Entry)
        self.down_size.insert(0, s.field("mem.down_size", "2048"))
        self.track(box, "download")
        for f in rows:
            browse_space(f, self.up_file)

        tb.Label(self, text="Result").pack(anchor="w")
        self.result = MonoText(self, height=10)
        self.result.pack(fill="both", expand=True)
        self.action = "peek"

    def _sym(self):
        return ["-sym", self.sym.get()] if self.sym.get() else []

    def args(self):
        if self.action == "peek":
            return ["-peek", self.peek_adrs.get().strip(), "-s", self.peek_size.get()] + self._sym()
        if self.action == "poke":
            return ["-poke", self.poke_adrs.get().strip(), self.poke_value.get().strip() or "0",
                    "-s", self.poke_size.get()] + self._sym()
        if self.action == "upload":
            return ["-u", "-f", self.up_file.get(), "-a", self.up_adrs.get().strip()] + self._sym()
        if self.action == "download":
            return ["-d", "-f", self.down_file.get(), "-a", self.down_adrs.get().strip(),
                    "-s", self.down_size.get().strip()] + self._sym()
        return None

    def _save_fields(self):
        s = self.app.settings
        self.sym.remember()
        for key, w in (("mem.peek_adrs", self.peek_adrs), ("mem.peek_size", self.peek_size),
                       ("mem.poke_adrs", self.poke_adrs), ("mem.poke_size", self.poke_size),
                       ("mem.up_adrs", self.up_adrs), ("mem.down_adrs", self.down_adrs),
                       ("mem.down_size", self.down_size)):
            s.set_field(key, w.get().strip())

    def _run(self, action, name):
        self.action = action
        self._save_fields()
        self.result.clear()
        self.app.run(self.args(), name, on_line=lambda line: self.result.append(line + "\n"))

    def peek(self):
        if not self.peek_adrs.get().strip():
            return self.app.error("Give the address to read.")
        self._run("peek", "Peek")

    def poke(self):
        if not self.poke_adrs.get().strip() or not self.poke_value.get().strip():
            return self.app.error("Give the address and the value to write.")
        self._run("poke", "Poke")

    def upload(self):
        if not os.path.isfile(self.up_file.get()) or not self.up_adrs.get().strip():
            return self.app.error("Choose the file to upload and the address.")
        self.up_file.remember()
        self._run("upload", "Upload")

    def download(self):
        if not self.down_file.get() or not self.down_adrs.get().strip() or not self.down_size.get().strip():
            return self.app.error("Choose the file to write, the address and the size.")
        self.down_file.remember()
        self._run("download", "Download")
