"""
PS1 DEV CART GUI: Console page (printf of the running program, PCDRV file server and its activity).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import re
import tkinter as tk
from tkinter import filedialog

import ttkbootstrap as tb

from pages.base import Page
from widgets import FileField, MonoText

FIELDS = re.compile(r'(\w+)=("[^"]*"|\S+)')     # key=value pairs of the ps1transfer -pcdrvlog lines
MAX_FILES = 500                                 # Rows kept in the file list


def parse_pcdrv(line):
    """'pcdrv=open fd=2 name="a b.txt"' -> {"pcdrv": "open", "fd": "2", "name": "a b.txt"}"""
    return {k: v[1:-1] if v.startswith('"') else v for k, v in FIELDS.findall(line)}


class ConsolePage(Page):
    title = "Console"

    def build(self):
        tb.Label(self, text="Console", font=("", 16, "bold")).pack(anchor="w", pady=(0, 6))
        tb.Label(self, text="printf of the running program. \"Attach\" connects to a program already running "
                            "(started without the console), and also serves its PCDRV file requests.",
                 wraplength=700, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 8))
        self.pcdrv = FileField(self, self.app, "console.pcdrv", "PCDRV folder", mode="dir")
        self.pcdrv.pack(fill="x", pady=(0, 4))
        self.any_path = tk.BooleanVar(value=self.app.settings.field("console.pcdrvany", False))
        tb.Checkbutton(self, text="Let the PS1 open any file of this computer (not only the PCDRV folder)",
                       variable=self.any_path, bootstyle="round-toggle",
                       command=lambda: self.app.settings.set_field("console.pcdrvany", self.any_path.get())).pack(
            anchor="w", pady=(0, 6))

        bar = tb.Frame(self)
        bar.pack(fill="x", pady=(0, 6))
        tb.Button(bar, text="Attach", bootstyle="success", command=self.attach).pack(side="left")
        tb.Button(bar, text="Stop", bootstyle="danger-outline", command=self.app.stop_task).pack(side="left", padx=6)
        tb.Button(bar, text="Clear", bootstyle="secondary-outline", command=lambda: self.text.clear()).pack(side="left")
        tb.Button(bar, text="Save log...", bootstyle="secondary-outline", command=self.save).pack(side="left", padx=6)
        self.autoscroll = tk.BooleanVar(value=True)
        tb.Checkbutton(bar, text="Follow", variable=self.autoscroll, bootstyle="round-toggle").pack(side="right")

        panes = tb.Panedwindow(self, orient="vertical")
        panes.pack(fill="both", expand=True)
        self.text = MonoText(panes, height=18)
        panes.add(self.text, weight=3)
        panes.add(self._build_files(panes), weight=1)

    def _build_files(self, parent):
        box = tb.Frame(parent)
        head = tb.Frame(box)
        head.pack(fill="x", pady=(6, 2))
        tb.Label(head, text="Files", font=("", 11, "bold")).pack(side="left")
        self.files_status = tb.Label(head, text="", bootstyle="secondary")
        self.files_status.pack(side="left", padx=8)
        tb.Button(head, text="Clear", bootstyle="secondary-outline", command=self.files_clear).pack(side="right")

        columns = (("name", "File", 320), ("mode", "Mode", 80), ("state", "State", 220), ("read", "Read", 90),
                   ("written", "Written", 90))
        frame = tb.Frame(box)
        frame.pack(fill="both", expand=True)
        self.files = tb.Treeview(frame, columns=[c[0] for c in columns], show="headings", height=6)
        for key, text, width in columns:
            numeric = key in ("read", "written")
            self.files.heading(key, text=text, anchor="e" if numeric else "w")
            self.files.column(key, width=width, anchor="e" if numeric else "w", stretch=key == "name")
        self.files.tag_configure("error", foreground="#e74c3c")
        self.files.tag_configure("open", foreground="#3498db")
        scroll = tb.Scrollbar(frame, orient="vertical", command=self.files.yview)
        self.files.configure(yscrollcommand=scroll.set)
        self.files.pack(side="left", fill="both", expand=True)
        scroll.pack(side="left", fill="y")
        self.open_rows = {}     # fd -> row of the open file
        self.counts = {"open": 0, "error": 0, "read": 0, "written": 0}
        return box

    # ---------------------------------------------------------------- file server activity

    def files_started(self):
        """A task serving files starts: files left open by the previous one are closed."""
        for row in self.open_rows.values():
            if self.files.exists(row):
                self.files.set(row, "state", "closed (program ended)")
                self.files.item(row, tags=())
        self.open_rows = {}

    def files_clear(self):
        self.files.delete(*self.files.get_children())
        self.open_rows = {}
        self.counts = {"open": 0, "error": 0, "read": 0, "written": 0}
        self.files_status.configure(text="")

    def files_event(self, line):
        """A ps1transfer -pcdrvlog line (see ps1transfer/README.md)."""
        e = parse_pcdrv(line)
        kind = e.get("pcdrv")
        name = e.get("name", "")
        if kind == "init":
            self.files_status.configure(text="The program connected to the file server")
            return
        if kind in ("open", "create"):
            self.counts["open"] += 1
            row = self._add_row((name, e.get("mode", ""), "open" if kind == "open" else "created", 0, 0), "open")
            self.open_rows[e.get("fd")] = row
        elif kind == "close":
            row = self.open_rows.pop(e.get("fd"), None)
            values = (name, None, "closed", e.get("read", "0"), e.get("written", "0"))
            if row and self.files.exists(row):
                old = self.files.item(row, "values")
                self.files.item(row, values=(name, old[1], "closed") + values[3:], tags=())
            else:
                self._add_row(values[:1] + ("",) + values[2:], ())
            self.counts["read"] += int(e.get("read", 0))
            self.counts["written"] += int(e.get("written", 0))
        elif kind == "error":
            self.counts["error"] += 1
            self._add_row((name, e.get("mode", ""), "refused: " + e.get("reason", "?"), "", ""), "error")
        else:
            return
        c = self.counts
        self.files_status.configure(text="%d opened, %d refused, %s read, %s written" % (
            c["open"], c["error"], _size(c["read"]), _size(c["written"])))

    def _add_row(self, values, tags):
        rows = self.files.get_children()
        if len(rows) >= MAX_FILES:
            self.files.delete(rows[0])
        row = self.files.insert("", "end", values=values, tags=tags)
        self.files.see(row)
        return row

    # ---------------------------------------------------------------- console

    def append(self, text):
        self.text.append(text, autoscroll=self.autoscroll.get())

    def args(self):
        a = ["-c"]
        if self.pcdrv.get():
            a += ["-p", self.pcdrv.get()]
        return a

    def attach(self):
        self.pcdrv.remember()
        self.app.run(self.args(), "Console", long_running=True, console=True, cwd=self.pcdrv.get() or None)

    def save(self):
        path = filedialog.asksaveasfilename(defaultextension=".txt", filetypes=[("Text", "*.txt")], parent=self)
        if path:
            with open(path, "w", encoding="utf-8") as f:
                f.write(self.text.get_all())


def _size(n):
    return "%d bytes" % n if n < 10240 else "%.1f KB" % (n / 1024) if n < 10485760 else "%.1f MB" % (n / 1048576)
