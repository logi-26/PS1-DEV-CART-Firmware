"""
PS1 DEV CART GUI: Hot reload page (files sent to the running program: TIM to VRAM, sounds to SPU RAM, data to RAM).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
from tkinter import filedialog

import ttkbootstrap as tb

from pages.base import Page
from widgets import labeled

MAX_FILES = 16
HOT_TYPES = [("Hot reload files", "*.tim *.vag *.vb *.bin"), ("All files", "*.*")]


def describe(path, target):
    """Where the file goes, for the table."""
    ext = os.path.splitext(path)[1].lower()
    if ext == ".tim":
        return "VRAM at %s" % target if target else "VRAM, at the TIM position"
    if not target:
        return "address needed (SPU RAM < 0x80000, or RAM 0x80...)"
    try:
        adrs = int(target, 0)
    except ValueError:
        return "bad address"
    return ("RAM at 0x%08X" if adrs >= 0x80000000 else "SPU RAM at 0x%X") % adrs


class HotReloadPage(Page):
    title = "Hot Reload"

    def build(self):
        tb.Label(self, text=self.title, font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="Send textures, sounds and data to the running program without restarting it. "
                            "With Watch, a file is sent again each time it is "
                            "saved, and the printf of the program is shown.",
                 wraplength=780, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 8))

        row = tb.Frame(self)
        row.pack(fill="both", expand=True)
        # Columns: file name, target, where it goes, full path (hidden, shown as a tooltip-like status below)
        self.table = tb.Treeview(row, columns=("file", "target", "where", "path"), displaycolumns=("file", "target",
                                 "where"), show="headings", height=10, selectmode="browse")
        for col, text, width in (("file", "File", 260), ("target", "@", 120), ("where", "Goes to", 320)):
            self.table.heading(col, text=text, anchor="w")
            self.table.column(col, width=width, anchor="w")
        self.table.pack(side="left", fill="both", expand=True)
        self.table.bind("<<TreeviewSelect>>", lambda e: self._selected())
        buttons = tb.Frame(row)
        buttons.pack(side="left", fill="y", padx=(6, 0))
        tb.Button(buttons, text="Add...", width=9, bootstyle="secondary-outline", command=self.add).pack(pady=1)
        tb.Button(buttons, text="Remove", width=9, bootstyle="secondary-outline", command=self.remove).pack(pady=1)

        self.path_label = tb.Label(self, text="", bootstyle="secondary")
        self.path_label.pack(anchor="w", pady=(4, 0))
        edit = tb.Frame(self)
        edit.pack(fill="x", pady=6)
        f, self.target = labeled(edit, "Target (@)", tb.Entry, width=16)
        f.pack(side="left")
        tb.Button(edit, text="Set for the selected file", bootstyle="secondary-outline",
                  command=self.set_target).pack(side="left", padx=6)
        tb.Label(edit, text="TIM: empty or x,y   Sound: SPU address (e.g. 0x1010)   Data: RAM address (0x80...)",
                 bootstyle="secondary").pack(side="left", padx=8)

        bar = tb.Frame(self)
        bar.pack(fill="x", pady=(4, 0))
        tb.Button(bar, text="Send now", bootstyle="primary", command=self.send).pack(side="left")
        tb.Button(bar, text="Watch", bootstyle="success", command=self.watch).pack(side="left", padx=6)
        tb.Button(bar, text="Stop", bootstyle="danger-outline", command=self.app.stop_task).pack(side="left")
        tb.Label(bar, text="The SPU address of a sound is where your program loaded it (the new data must not be "
                           "bigger).", bootstyle="secondary").pack(side="left", padx=12)

        for path, target in self.app.settings.field("hot.files", []):
            self._insert(path, target)
        self.action = "send"

    def _insert(self, path, target):
        self.table.insert("", "end", values=(os.path.basename(path), target, describe(path, target), path))

    def rows(self):
        """(full path, target) of each file."""
        return [(v[3], v[1]) for v in (self.table.item(i, "values") for i in self.table.get_children())]

    def _save(self):
        self.app.settings.set_field("hot.files", [list(r) for r in self.rows()])

    def add(self):
        paths = filedialog.askopenfilenames(filetypes=HOT_TYPES, parent=self)
        for path in paths:
            if len(self.rows()) >= MAX_FILES:
                self.app.error("At most %d files." % MAX_FILES)
                break
            self._insert(os.path.normpath(path), "")
        self._save()

    def remove(self):
        for i in self.table.selection():
            self.table.delete(i)
        self._save()

    def _selected(self):
        sel = self.table.selection()
        if sel:
            values = self.table.item(sel[0], "values")
            self.target.delete(0, "end")
            self.target.insert(0, values[1])
            self.path_label.configure(text=values[3])

    def set_target(self):
        sel = self.table.selection()
        if not sel:
            return self.app.error("Select a file in the table.")
        path = self.table.item(sel[0], "values")[3]
        target = self.target.get().strip()
        self.table.item(sel[0], values=(os.path.basename(path), target, describe(path, target), path))
        self._save()

    def args(self):
        rows = self.rows()
        if not rows:
            return None
        a = []
        for path, target in rows:
            a += ["-hot", path + ("@" + target if target else "")]
        if self.action == "watch":
            a.append("-watch")
        return a

    def _check(self):
        rows = self.rows()
        if not rows:
            self.app.error("Add the files to send.")
            return False
        for path, target in rows:
            if not os.path.isfile(path):
                self.app.error("File not found: %s" % path)
                return False
            if not path.lower().endswith(".tim") and not target:
                self.app.error("Give the address of %s (select it, type the address, Set)." % os.path.basename(path))
                return False
        return True

    def send(self):
        self.action = "send"
        if self._check():
            self.app.run(self.args(), "Hot reload")

    def watch(self):
        self.action = "watch"
        if self._check():
            self.app.run(self.args(), "Hot reload watch", long_running=True, console=True)
