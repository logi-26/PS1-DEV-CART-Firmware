"""
PS1 DEV CART GUI: Memory page (the variables of the running program watched live, peek, poke, upload, download;
addresses in hexadecimal or @symbol[+offset]).

The symbol file (ELF, Net Yaroze executable, .map, nm output: symbols.py) lists the program's variables: search them,
add them to the watch list. "Start watching" attaches to the running program (ps1transfer -memlive, see
ps1transfer/memlive.c, which also shows its printf and serves its files, as the console): the values are read a few
times a second and shown as the type chosen (8/16/32-bit, signed or not, float, bytes); a double click on a value
writes a new one.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import struct
import time
import tkinter as tk
from tkinter import simpledialog

import ttkbootstrap as tb

import symbols
from livelink import FRAME_CAPTURE, LIVE_LINK
from pages.base import Page
from widgets import FileField, MonoText, browse_space, labeled

SYMBOL_TYPES = [("Symbols (ELF, Net Yaroze executable, map, nm output)", "*.elf *.map *.sym *.exe main"),
                ("All files", "*.*")]
# Watch types: struct format, size; "bytes": up to 16 bytes in hexadecimal
TYPES = {"u8": ("<B", 1), "s8": ("<b", 1), "u16": ("<H", 2), "s16": ("<h", 2), "u32": ("<I", 4), "s32": ("<i", 4),
         "float": ("<f", 4), "bytes": (None, 16)}
RATES = {"1 / s": 1000, "2 / s": 500, "5 / s": 200, "10 / s": 100}
MAX_LIST = 2000             # Symbols shown (the search narrows them)


def default_type(size):
    """A variable's type from its size (unknown: a 32-bit integer, the most common)."""
    return {0: "s32", 1: "u8", 2: "s16", 4: "s32"}.get(size, "bytes")


class MemoryPage(Page):
    title = "Memory Peek/Poke"

    def build(self):
        s = self.app.settings
        self.syms = []              # symbols.Symbol of the symbol file
        self.by_name = {}
        self.shown_syms = []        # The ones in the list (search)
        self.watch = []             # {"name", "adrs", "type", "size", "value", "row", "changed"}
        self.live = False           # Watching (the live link answers, the values are read)
        self.want_watch = False     # Watching asked, the live link starting
        self.pending = 0            # Reads sent, not answered yet (the next refresh waits for them)
        self.poll_job = None
        self.peek_live = None       # A Peek made through the watch link: (address, size)

        tb.Label(self, text=self.title, font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="The variables of the running program: choose its symbol file, add variables to the watch "
                            "list and start watching (their values live; double-click a value to change it). Or peek "
                            "and poke any address: hexadecimal (0x80100000) or a symbol (@player, @table+0x10).",
                 wraplength=930, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 6))
        row = tb.Frame(self)
        row.pack(fill="x")
        self.sym = FileField(row, self.app, "mem.sym", "Symbol file", filetypes=SYMBOL_TYPES,
                             on_change=lambda: self.after_idle(self.load_symbols))
        self.sym.pack(side="left", fill="x", expand=True)
        self.sym_status = tb.Label(self, text="", bootstyle="secondary")
        self.sym_status.pack(anchor="w", pady=(2, 6))

        tabs = tb.Notebook(self)
        tabs.pack(fill="both", expand=True)
        watch_tab = tb.Frame(tabs, padding=8)
        tools_tab = tb.Frame(tabs, padding=8)
        tabs.add(watch_tab, text="  Watch variables  ")
        tabs.add(tools_tab, text="  Peek / Poke / Files  ")
        self._build_watch(watch_tab)
        self._build_tools(tools_tab, s)
        self.watch_load()
        self.after_idle(self.load_symbols)
        self.app.live_listen(self)

    # ---------------------------------------------------------------- layout

    def _build_watch(self, tab):
        panes = tb.Panedwindow(tab, orient="horizontal")
        panes.pack(fill="both", expand=True)

        # The symbols: search, list, add
        left = tb.Frame(panes, padding=(0, 0, 8, 0))
        panes.add(left, weight=2)
        row = tb.Frame(left)
        row.pack(fill="x")
        tb.Label(row, text="Search").pack(side="left")
        self.search = tk.StringVar()
        self.search.trace_add("write", lambda *_: self.fill_symbols())
        tb.Entry(row, textvariable=self.search).pack(side="left", fill="x", expand=True, padx=6)
        self.functions = tk.BooleanVar(value=False)
        tb.Checkbutton(row, text="Functions too", variable=self.functions, bootstyle="round-toggle",
                       command=self.fill_symbols).pack(side="left")
        frame = tb.Frame(left)
        frame.pack(fill="both", expand=True, pady=(4, 4))
        self.sym_list = tb.Treeview(frame, columns=("name", "adrs", "size"), show="headings", height=10)
        for key, text, width, anchor in (("name", "Variable", 140, "w"), ("adrs", "Address", 80, "w"),
                                         ("size", "Size", 55, "e")):
            self.sym_list.heading(key, text=text, anchor=anchor)
            self.sym_list.column(key, width=width, anchor=anchor, stretch=key == "name")
        scroll = tb.Scrollbar(frame, orient="vertical", command=self.sym_list.yview)
        self.sym_list.configure(yscrollcommand=scroll.set)
        self.sym_list.pack(side="left", fill="both", expand=True)
        scroll.pack(side="left", fill="y")
        self.sym_list.bind("<Double-1>", lambda e: self.add_selected())
        self.sym_list.bind("<<TreeviewSelect>>", lambda e: self.symbol_selected())
        row = tb.Frame(left)
        row.pack(fill="x")
        tb.Button(row, text="Add to the watch list ▶", bootstyle="primary-outline",
                  command=self.add_selected).pack(side="left")
        tb.Label(row, text="(or double-click)", bootstyle="secondary").pack(side="left", padx=6)

        # The watch list: values live, type, edit
        right = tb.Frame(panes)
        panes.add(right, weight=3)
        bar = tb.Frame(right)
        bar.pack(fill="x")
        self.watch_button = tb.Button(bar, text="Start watching", bootstyle="success", width=15,
                                      command=self.toggle_watch)
        self.watch_button.pack(side="left")
        tb.Label(bar, text="Refresh").pack(side="left", padx=(12, 4))
        self.rate = tb.Combobox(bar, state="readonly", width=7, values=list(RATES))
        self.rate.set(self.app.settings.field("mem.rate", "5 / s"))
        self.rate.pack(side="left")
        self.rate.bind("<<ComboboxSelected>>", lambda e: self.app.settings.set_field("mem.rate", self.rate.get()))
        self.watch_status = tb.Label(bar, text="Not watching.", bootstyle="secondary")
        self.watch_status.pack(side="left", padx=12)
        frame = tb.Frame(right)
        frame.pack(fill="both", expand=True, pady=(4, 4))
        columns = (("name", "Watch", 170, "w"), ("adrs", "Address", 90, "w"), ("type", "Type", 60, "w"),
                   ("value", "Value", 150, "e"), ("hex", "Hex", 200, "w"))
        self.watch_list = tb.Treeview(frame, columns=[c[0] for c in columns], show="headings", height=10)
        for key, text, width, anchor in columns:
            self.watch_list.heading(key, text=text, anchor=anchor)
            self.watch_list.column(key, width=width, anchor=anchor, stretch=key in ("name", "hex"))
        self.watch_list.tag_configure("changed", foreground="#f0ad4e")
        self.watch_list.tag_configure("error", foreground="#e74c3c")
        scroll = tb.Scrollbar(frame, orient="vertical", command=self.watch_list.yview)
        self.watch_list.configure(yscrollcommand=scroll.set)
        self.watch_list.pack(side="left", fill="both", expand=True)
        scroll.pack(side="left", fill="y")
        self.watch_list.bind("<Double-1>", self.watch_double)
        self.watch_list.bind("<<TreeviewSelect>>", lambda e: self.watch_selected())
        row = tb.Frame(right)
        row.pack(fill="x")
        tb.Label(row, text="Type").pack(side="left")
        self.type_choice = tb.Combobox(row, state="readonly", width=7, values=list(TYPES))
        self.type_choice.pack(side="left", padx=(4, 8))
        self.type_choice.bind("<<ComboboxSelected>>", lambda e: self.set_type())
        tb.Button(row, text="Change the value...", bootstyle="warning-outline", command=self.edit_value).pack(
            side="left")
        tb.Button(row, text="Remove", bootstyle="secondary-outline", command=self.remove_watch).pack(side="left",
                                                                                                    padx=4)
        tb.Button(row, text="Clear", bootstyle="secondary-outline", command=self.clear_watch).pack(side="left")
        row = tb.Frame(right)
        row.pack(fill="x", pady=(6, 0))
        tb.Label(row, text="Add an address").pack(side="left")
        self.manual = tb.Entry(row, width=22)
        self.manual.pack(side="left", padx=4)
        self.manual.bind("<Return>", lambda e: self.add_manual())
        tb.Button(row, text="Add", bootstyle="secondary-outline", command=self.add_manual).pack(side="left")
        tb.Label(row, text="0x8001F000, @player+8", bootstyle="secondary").pack(side="left", padx=6)

    def _build_tools(self, tab, s):
        top = tb.Frame(tab)
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

        tb.Label(tab, text="Result").pack(anchor="w")
        self.result = MonoText(tab, height=8)
        self.result.pack(fill="both", expand=True)
        self.action = "peek"

    # ---------------------------------------------------------------- symbols

    def load_symbols(self):
        path = self.sym.get()
        self.syms, self.by_name = [], {}
        if path and os.path.isfile(path):
            try:
                self.syms = symbols.load(path)
            except (OSError, ValueError, struct.error) as e:
                self.sym_status.configure(text="Can't read the symbols: %s" % e)
            else:
                data = sum(s.kind == "data" for s in self.syms)
                self.sym_status.configure(text="%d variables and %d other symbols in %s" % (
                    data, len(self.syms) - data, os.path.basename(path)))
                self.sym.remember()
        else:
            self.sym_status.configure(text="Choose the symbol file of the program running (the .elf, .map, or the "
                                           "Net Yaroze executable) to list its variables." if not path else
                                      "Symbol file not found.")
        for s in self.syms:
            self.by_name.setdefault(s.name, s)
        self.fill_symbols()
        self.watch_resolve()

    def fill_symbols(self):
        self.sym_list.delete(*self.sym_list.get_children())
        text = self.search.get().strip().lower()
        shown = [s for s in self.syms if (s.kind == "data" or self.functions.get()) and text in s.name.lower()]
        for i, s in enumerate(shown[:MAX_LIST]):
            self.sym_list.insert("", "end", iid=str(i), values=(s.name, "%08X" % s.adrs, s.size_text()))
        self.shown_syms = shown[:MAX_LIST]

    def selected_symbol(self):
        sel = self.sym_list.selection()
        return self.shown_syms[int(sel[0])] if sel else None

    def symbol_selected(self):
        """The symbol chosen fills the Peek fields too."""
        s = self.selected_symbol()
        if s:
            self.peek_adrs.delete(0, "end")
            self.peek_adrs.insert(0, "@" + s.name)
            self.peek_size.set(str(min(max(s.size, 4), 4096)))

    def resolve(self, text):
        """'0x8001F000', '8001F000', '@name', '@name+0x10' -> address, or None."""
        text = text.strip()
        try:
            if text.startswith("@"):
                name, _, offset = text[1:].partition("+")
                s = self.by_name.get(name.strip())
                return s.adrs + (int(offset, 0) if offset else 0) if s else None
            return int(text, 16)
        except ValueError:
            return None

    # ---------------------------------------------------------------- watch list

    def watch_load(self):
        for w in self.app.settings.field("mem.watch", []):
            if isinstance(w, dict) and "name" in w:
                self.watch_add(w["name"], w.get("adrs"), w.get("type", "s32"), save=False)

    def watch_save(self):
        self.app.settings.set_field("mem.watch", [{"name": w["name"], "adrs": w["adrs"], "type": w["type"]}
                                                  for w in self.watch])

    def watch_add(self, name, adrs, typ, save=True):
        if any(w["name"] == name for w in self.watch):
            return
        w = {"name": name, "adrs": adrs, "type": typ, "value": None, "changed": 0}
        w["row"] = self.watch_list.insert("", "end", values=self.watch_values(w))
        self.watch.append(w)
        if save:
            self.watch_save()

    def watch_resolve(self):
        """The watched symbols' addresses from the symbol file (they move when the program is rebuilt)."""
        for w in self.watch:
            if w["name"].startswith("@") or not w["name"][:1].isalpha() and w["name"][:1] != "_":
                adrs = self.resolve(w["name"])
            else:
                s = self.by_name.get(w["name"])
                adrs = s.adrs if s else w["adrs"]
            if adrs is not None:
                w["adrs"] = adrs
            self.watch_list.item(w["row"], values=self.watch_values(w))

    def watch_values(self, w):
        value, hexa = "", ""
        if w["value"] is not None:
            data = w["value"]
            fmt, size = TYPES[w["type"]]
            if fmt is None:
                hexa = " ".join("%02X" % b for b in data)
                value = "".join(chr(b) if 32 <= b < 127 else "." for b in data)
            elif len(data) >= size:
                v = struct.unpack(fmt, data[:size])[0]
                value = ("%.4g" % v) if w["type"] == "float" else str(v)
                hexa = "0x%0*X" % (size * 2, struct.unpack("<" + "BHI"[(size > 1) + (size > 2)], data[:size])[0])
        adrs = "%08X" % w["adrs"] if w["adrs"] is not None else "?"
        return (w["name"], adrs, w["type"], value, hexa)

    def add_selected(self):
        s = self.selected_symbol()
        if s:
            self.watch_add(s.name, s.adrs, default_type(s.size))

    def add_manual(self):
        text = self.manual.get().strip()
        adrs = self.resolve(text)
        if adrs is None:
            return self.app.error("Give an address (0x8001F000) or a symbol of the symbol file (@player, @table+8).")
        self.watch_add(text, adrs, "s32")
        self.manual.delete(0, "end")
        return None

    def selected_watch(self):
        sel = self.watch_list.selection()
        return next((w for w in self.watch if w["row"] == sel[0]), None) if sel else None

    def watch_selected(self):
        w = self.selected_watch()
        if w:
            self.type_choice.set(w["type"])

    def set_type(self):
        w = self.selected_watch()
        if w:
            w["type"] = self.type_choice.get()
            self.watch_list.item(w["row"], values=self.watch_values(w))
            self.watch_save()

    def remove_watch(self):
        w = self.selected_watch()
        if w:
            self.watch_list.delete(w["row"])
            self.watch.remove(w)
            self.watch_save()

    def clear_watch(self):
        self.watch_list.delete(*self.watch_list.get_children())
        self.watch = []
        self.watch_save()

    def watch_double(self, event):
        if self.watch_list.identify_column(event.x) == "#4":     # The value: changed
            self.edit_value()

    def edit_value(self):
        """A new value written to the variable (as its type), while watching."""
        w = self.selected_watch()
        if not w:
            return self.app.error("Select a variable of the watch list.")
        if not self.live:
            return self.app.error("Start watching first (the program must run).")
        fmt, size = TYPES[w["type"]]
        current = self.watch_values(w)[3 if fmt else 4]
        text = simpledialog.askstring("Change the value", "%s (%s):" % (w["name"], w["type"] if fmt else
                                      "bytes in hexadecimal"), initialvalue=current, parent=self)
        if text is None:
            return None
        try:
            if fmt is None:
                data = bytes.fromhex(text)
            elif w["type"] == "float":
                data = struct.pack(fmt, float(text))
            else:
                v = int(text, 0)
                data = struct.pack(fmt.lower() if v < 0 else fmt.upper(), v)
        except (ValueError, struct.error):
            return self.app.error("Not a valid %s value: %s" % (w["type"], text))
        self.app.live_send("write %08X %s" % (w["adrs"], data.hex().upper()))
        return None

    # ---------------------------------------------------------------- live link (ps1transfer -memlive, shared)

    def toggle_watch(self):
        if self.live or self.want_watch:
            self.stop_watch()
            self.app.live_release()     # (kept while the remote controller uses it)
            return None
        if self.app.live_task() == FRAME_CAPTURE:
            return self.app.error("The frame capture runs (Frame Inspector): stop it first to watch variables.")
        self.want_watch = True
        if self.app.live_task() == LIVE_LINK and self.app.live_ready:
            self.start_watch("1")
        else:
            self.watch_status.configure(text="Connecting...")
            folder = self.app.settings.field("console.pcdrv", "") or (os.path.dirname(self.sym.get())
                                                                      if self.sym.get() else "")
            self.app.live_start(cwd=folder if folder and os.path.isdir(folder) else None)
        return None

    def start_watch(self, running):
        self.want_watch = False
        self.live = True
        self.pending = 0
        self.watch_button.configure(text="Stop watching", bootstyle="danger")
        self.watch_status.configure(text="Watching the running program." if running == "1" else
                                    "No program running (the PS1 DEV CART menu).")
        self.poll()

    def stop_watch(self):
        self.live = self.want_watch = False
        self.pending = 0
        if self.poll_job:
            self.after_cancel(self.poll_job)
            self.poll_job = None
        self.watch_button.configure(text="Start watching", bootstyle="success")
        self.watch_status.configure(text="Not watching.")

    def using_live(self):
        return self.live or self.want_watch

    def live_stopped(self):
        self.stop_watch()

    def live_line(self, line):
        fields = line.split()
        if line.startswith("MEMLIVE ready"):
            if self.want_watch:
                self.start_watch(fields[-1])
        elif not self.live:
            return
        elif line.startswith("MEMLIVE disconnected"):
            self.watch_status.configure(text="Console disconnected.")
        elif line.startswith("MEMREAD") and len(fields) == 3:
            self.pending = max(0, self.pending - 1)
            self.read_done(int(fields[1], 16), bytes.fromhex(fields[2]))
        elif line.startswith("MEMWROTE"):
            self.watch_status.configure(text="Written: %s bytes at %s" % (fields[2], fields[1]))
        elif line.startswith("MEMERR"):
            self.pending = max(0, self.pending - 1)
            self.watch_status.configure(text=line[7:])

    def poll(self):
        """The watched values read (the next ones once these are answered)."""
        self.poll_job = None
        if not self.live:
            return
        if not self.pending:
            for w in self.watch:
                if w["adrs"] is not None:
                    self.app.live_send("read %08X %d" % (w["adrs"], TYPES[w["type"]][1]))
                    self.pending += 1
        self.poll_job = self.after(RATES.get(self.rate.get(), 200), self.poll)

    def read_done(self, adrs, data):
        if self.peek_live and self.peek_live[0] == adrs and self.peek_live[1] == len(data):
            self.peek_live = None
            self.show_dump(adrs, data)
            return
        now = time.time()
        for w in self.watch:
            if w["adrs"] == adrs and TYPES[w["type"]][1] == len(data):
                if w["value"] is not None and w["value"] != data:
                    w["changed"] = now
                w["value"] = data
                self.watch_list.item(w["row"], values=self.watch_values(w),
                                     tags=("changed",) if now - w["changed"] < 1.0 else ())

    def show_dump(self, adrs, data):
        self.result.clear()
        for i in range(0, len(data), 16):
            chunk = data[i:i + 16]
            text = " ".join("%02X" % b for b in chunk).ljust(48)
            ascii_ = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
            self.result.append("%08X  %s  %s\n" % (adrs + i, text, ascii_))
        if len(data) in (1, 2, 4):
            v = int.from_bytes(data, "little")
            signed = int.from_bytes(data, "little", signed=True)
            self.result.append("Value: %u (0x%X), signed %d\n" % (v, v, signed))

    # ---------------------------------------------------------------- peek, poke, upload, download

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
        if self.live:       # Through the watch link (the cartridge is in use)
            adrs = self.resolve(self.peek_adrs.get())
            try:
                size = min(max(int(self.peek_size.get()), 1), 4096)
            except ValueError:
                size = 64
            if adrs is None:
                return self.app.error("Unknown address or symbol: %s" % self.peek_adrs.get())
            self._save_fields()
            self.peek_live = (adrs, size)
            self.app.live_send("read %08X %d" % (adrs, size))
            return None
        self._run("peek", "Peek")
        return None

    def poke(self):
        if not self.poke_adrs.get().strip() or not self.poke_value.get().strip():
            return self.app.error("Give the address and the value to write.")
        if self.live:
            adrs = self.resolve(self.poke_adrs.get())
            try:
                v = int(self.poke_value.get().strip(), 0)
                data = (v & 0xFFFFFFFF).to_bytes(4, "little")[:int(self.poke_size.get())]
            except ValueError:
                return self.app.error("Give a number (decimal, or 0x for hexadecimal).")
            if adrs is None:
                return self.app.error("Unknown address or symbol: %s" % self.poke_adrs.get())
            self._save_fields()
            self.app.live_send("write %08X %s" % (adrs, data.hex().upper()))
            self.result.clear()
            self.result.append("Writing %d (0x%X), %d byte(s) at 0x%08X\n" % (v, v & 0xFFFFFFFF, len(data), adrs))
            return None
        self._run("poke", "Poke")
        return None

    def upload(self):
        if not os.path.isfile(self.up_file.get()) or not self.up_adrs.get().strip():
            return self.app.error("Choose the file to upload and the address.")
        self.up_file.remember()
        self._run("upload", "Upload")
        return None

    def download(self):
        if not self.down_file.get() or not self.down_adrs.get().strip() or not self.down_size.get().strip():
            return self.app.error("Choose the file to write, the address and the size.")
        self.down_file.remember()
        self._run("download", "Download")
        return None
