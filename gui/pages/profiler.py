"""
PS1 DEV CART GUI: Profiler page (ps1transfer -prof -json, see ps1transfer/profiler.c).

Runs a program (or attaches to the running one) and shows every second what the profiler measures: CPU load (time
outside the waiting functions such as VSync), GPU busy, frames per second, DMA activity, RAM used, stack, SPU voices,
and the functions where the time goes. A double click on a function marks it as waiting (not CPU load), e.g. the
VSync loop of a program without symbols.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import json

import ttkbootstrap as tb

from pages.base import Page
from widgets import FileField, PS1_EXE_TYPES, labeled

RATES = ("1000", "500", "2000", "250")
COUNTERS = {"0 (default)": 0, "1": 1, "2": 2}
HISTORY = 60        # Seconds in the chart
DMA = (("gpu", "GPU"), ("spu", "SPU"), ("cd", "CD-ROM"), ("mdec", "MDEC"), ("otc", "OT clear"))


class ProfilerPage(Page):
    title = "Profiler"

    def build(self):
        s = self.app.settings
        self.history = []       # (cpu, gpu, fps) per second
        self.last = None        # Last PROF data

        tb.Label(self, text="PS1 DEV CART Profiler", font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="Where the time goes (symbols: the .elf or .map next to the program), CPU/GPU load, "
                            "frames, DMA, RAM, stack and SPU voices, sampled through the cartridge (about 4% of the "
                            "program's time at 1000 samples/s). Double click a function to mark it as waiting.",
                 wraplength=800, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 6))

        box = self.section("Program")
        box.pack(fill="x", pady=(0, 6))
        self.exe = FileField(box, self.app, "prof.exe", "Executable", filetypes=PS1_EXE_TYPES)
        if not self.exe.get():
            self.exe.set(s.field("run.exe", ""))
        self.exe.pack(fill="x")
        opts = tb.Frame(box)
        opts.pack(fill="x", pady=(6, 0))
        self.fill = tb.BooleanVar(value=s.field("prof.fill", True))
        tb.Checkbutton(opts, text="Measure RAM use (fills the free RAM before starting)", variable=self.fill,
                       bootstyle="round-toggle").pack(side="left")
        _, self.rate = labeled(opts, "Samples/s", tb.Combobox, width=10, values=RATES)
        self.rate.master.pack(side="left", padx=(16, 0))
        self.rate.set(s.field("prof.rate", "1000"))
        self.rate.configure(width=6)
        _, self.counter = labeled(opts, "Root counter", tb.Combobox, width=12, values=list(COUNTERS),
                                  state="readonly")
        self.counter.master.pack(side="left", padx=(12, 0))
        self.counter.set(s.field("prof.counter", "0 (default)"))
        self.counter.configure(width=10)
        _, self.idle = labeled(opts, "Waiting", tb.Entry, width=8)
        self.idle.master.pack(side="left", fill="x", expand=True, padx=(12, 0))
        self.idle.insert(0, s.field("prof.idle", ""))

        bar = tb.Frame(self)
        bar.pack(fill="x", pady=(0, 6))
        tb.Button(bar, text="Run and profile", bootstyle="success", width=16, command=self.start_run).pack(side="left")
        tb.Button(bar, text="Profile the running program", bootstyle="info-outline",
                  command=self.start_attach).pack(side="left", padx=8)
        tb.Button(bar, text="Stop", bootstyle="danger-outline", width=10, command=self.app.stop_task).pack(side="left")
        self.status = tb.Label(bar, text="", bootstyle="secondary")
        self.status.pack(side="left", padx=12)

        # Figures of the last second
        stats = tb.Frame(self)
        stats.pack(fill="x", pady=(0, 6))
        self.values = {}
        for key, text in (("cpu", "CPU"), ("gpu", "GPU"), ("fps", "Frames/s"), ("ram", "RAM used"),
                          ("stack", "Stack"), ("spu", "SPU voices"), ("dropped", "Dropped")):
            cell = tb.Frame(stats, padding=(0, 0, 18, 0))
            cell.pack(side="left")
            tb.Label(cell, text=text, bootstyle="secondary").pack(anchor="w")
            self.values[key] = tb.Label(cell, text="-", font=("", 15, "bold"))
            self.values[key].pack(anchor="w")

        panes = tb.Panedwindow(self, orient="horizontal")
        panes.pack(fill="both", expand=True)
        left = tb.Frame(panes)
        right = tb.Frame(panes)
        panes.add(left, weight=3)
        panes.add(right, weight=2)
        self.table = tb.Treeview(left, columns=("name", "now", "total", "samples", "wait"), show="headings",
                                 selectmode="browse")
        for col, text, width, anchor in (("name", "Function", 220, "w"), ("now", "Last second", 80, "e"),
                                         ("total", "Since start", 80, "e"), ("samples", "Samples", 70, "e"),
                                         ("wait", "Waiting", 60, "center")):
            self.table.heading(col, text=text, anchor=anchor)
            self.table.column(col, width=width, anchor=anchor, stretch=(col == "name"))
        self.table.pack(side="left", fill="both", expand=True)
        scroll = tb.Scrollbar(left, orient="vertical", command=self.table.yview)
        scroll.pack(side="left", fill="y")
        self.table.configure(yscrollcommand=scroll.set)
        self.table.bind("<Double-1>", self.toggle_idle)

        self.chart = tb.Canvas(right, height=110, highlightthickness=0)
        self.chart.pack(fill="x", padx=(8, 0))
        self.hw = tb.Canvas(right, height=170, highlightthickness=0)
        self.hw.pack(fill="both", expand=True, padx=(8, 0), pady=(6, 0))
        self.chart.bind("<Configure>", lambda e: self.draw())
        self.hw.bind("<Configure>", lambda e: self.draw())

    # ---------------------------------------------------------------- run

    def _common(self):
        s = self.app.settings
        s.set_field("prof.fill", bool(self.fill.get()))
        s.set_field("prof.rate", self.rate.get())
        s.set_field("prof.counter", self.counter.get())
        s.set_field("prof.idle", self.idle.get().strip())
        rate = self.rate.get().strip() or "1000"
        args = ["-prof", "%s,%d" % (rate, COUNTERS.get(self.counter.get(), 0)), "-json"]
        if self.idle.get().strip():
            args += ["-idle", self.idle.get().strip().replace(" ", "")]
        return args

    def args(self):
        exe = self.exe.get()
        return (["-r", "-e", "-f", exe] if exe else []) + ["-prof", (self.rate.get() or "1000")] + \
            ([] if self.fill.get() else ["-nofill"])

    def start_run(self):
        exe = self.exe.get()
        if not exe:
            self.app.error("Choose the program to profile.")
            return
        self.exe.remember()
        args = ["-r", "-e", "-f", exe] + self._common() + ([] if self.fill.get() else ["-nofill"])
        self._start(args)

    def start_attach(self):
        self._start(self._common())

    def _start(self, args):
        self.history = []
        self.last = None
        self.table.delete(*self.table.get_children())
        for v in self.values.values():
            v.configure(text="-")
        self.status.configure(text="Starting...")
        self.draw()
        # The PROF lines are shown on the page, the program's printf in the log
        self.app.run(args, "Profiler", long_running=True, stdin=True, on_line=self._line,
                     log_filter=lambda line: not line.startswith("PROF "),
                     on_done=lambda code, out: self.status.configure(text="Stopped."))

    def _line(self, line):
        if line.startswith("PROF "):
            try:
                data = json.loads(line[5:])
            except ValueError:
                return
            self.after(0, self.show, data)
        elif line.startswith("Profiling:") or line.startswith("RAM use measured"):
            self.after(0, lambda: self.status.configure(text=line))

    # ---------------------------------------------------------------- display

    def marked(self):
        return {n.strip() for n in self.app.settings.field("prof.idle", "").split(",") if n.strip()}

    def toggle_idle(self, _event=None):
        sel = self.table.selection()
        if not sel:
            return
        name = self.table.item(sel[0], "values")[0]
        marks = self.marked()
        marks ^= {name}
        text = ",".join(sorted(marks))
        self.app.settings.set_field("prof.idle", text)
        self.idle.delete(0, "end")
        self.idle.insert(0, text)
        if self.last:
            self.show(self.last, again=True)

    def cpu(self, data):
        """CPU load, without the functions marked as waiting on this page (not known by ps1transfer yet)."""
        marks = self.marked()
        extra = sum(r[2] for r in data.get("recent", []) if r[0] in marks and not r[3])
        return max(0.0, data.get("cpu", 0.0) - extra)

    def show(self, data, again=False):
        self.last = data
        cpu = self.cpu(data)
        if not again:
            self.history = (self.history + [(cpu, data.get("gpu", 0.0), data.get("fps", 0.0))])[-HISTORY:]
        self.values["cpu"].configure(text="%.0f%%" % cpu, bootstyle="danger" if cpu > 95 else "default")
        self.values["gpu"].configure(text="%.0f%%" % data.get("gpu", 0))
        self.values["fps"].configure(text="%.1f" % data.get("fps", 0))
        ram = data.get("ram")
        self.values["ram"].configure(text=("%d / %d KB" % (ram["used"] // 1024, (ram["hi"] - ram["lo"]) // 1024))
                                     if ram else "-")
        self.values["stack"].configure(text=("%d B" % data["stack"]) if data.get("stack") else "-")
        spu = data.get("spu")
        self.values["spu"].configure(text=("%d / 24" % spu["playing"]) if spu else "-")
        self.values["dropped"].configure(text=str(data.get("dropped", 0)))
        if not data.get("symbols"):
            self.status.configure(text="No symbols: put the .elf or .map next to the program (addresses grouped "
                                       "by 64 bytes)")

        marks = self.marked()
        now = {r[0]: r[2] for r in data.get("recent", [])}
        self.table.delete(*self.table.get_children())
        for name, count, pct, idle in data.get("top", []):
            wait = "yes" if idle else ("marked" if name in marks else "")
            self.table.insert("", "end", values=(name, "%.1f%%" % now[name] if name in now else "",
                                                 "%.1f%%" % pct, count, wait))
        self.draw()

    def draw(self):
        c = self.chart
        c.delete("all")
        w, h = max(c.winfo_width(), 100), max(c.winfo_height(), 60)
        fg = self.app.style.colors.fg if hasattr(self.app, "style") else "#ccc"
        grid = "#555"
        c.create_rectangle(1, 1, w - 1, h - 14, outline=grid)
        for y in (0.25, 0.5, 0.75):
            c.create_line(1, (h - 14) * y, w - 1, (h - 14) * y, fill=grid, dash=(2, 4))
        colors = (("#e5534b", "CPU %"), ("#4c9be8", "GPU %"), ("#57ab5a", "fps (60 = top)"))
        for i, (color, label) in enumerate(colors):
            c.create_text(4 + i * 110, h - 6, text=label, fill=color, anchor="w", font=("", 8))
            if len(self.history) > 1:
                pts = []
                for j, values in enumerate(self.history):
                    v = values[i] / (60.0 if i == 2 else 100.0)
                    pts += [1 + (w - 2) * (j + HISTORY - len(self.history)) / (HISTORY - 1),
                            (h - 14) - min(1.0, v) * (h - 16)]
                c.create_line(*pts, fill=color, width=2)

        c = self.hw
        c.delete("all")
        w, h = max(c.winfo_width(), 100), max(c.winfo_height(), 100)
        data = self.last or {}
        dma = data.get("dma", {})
        rows = [(label, dma.get(key, 0.0)) for key, label in DMA] + [("CD busy", data.get("cd", 0.0))]
        c.create_text(0, 6, text="DMA busy (% of the time)", fill=fg, anchor="w", font=("", 9, "bold"))
        for i, (label, pct) in enumerate(rows):
            y = 20 + i * 13
            c.create_text(0, y, text=label, fill=fg, anchor="w", font=("", 8))
            c.create_rectangle(60, y - 4, w - 40, y + 4, outline=grid)
            c.create_rectangle(60, y - 4, 60 + (w - 100) * min(pct, 100) / 100, y + 4, fill="#4c9be8", width=0)
            c.create_text(w - 2, y, text="%.0f%%" % pct, fill=fg, anchor="e", font=("", 8))
        y0 = 20 + len(rows) * 13 + 6
        spu = data.get("spu")
        c.create_text(0, y0, text="SPU voices (envelope)" + ("" if not spu or spu.get("on") else ": SPU off"),
                      fill=fg, anchor="w", font=("", 9, "bold"))
        bw = (w - 4) / 24.0
        top, bottom = y0 + 10, h - 12
        for i in range(24):
            x = 2 + i * bw
            c.create_rectangle(x + 1, top, x + bw - 1, bottom, outline=grid)
            env = spu["env"][i] if spu else 0
            if env:
                c.create_rectangle(x + 1, bottom - (bottom - top) * env / 32767.0, x + bw - 1, bottom,
                                   fill="#57ab5a", width=0)
            c.create_text(x + bw / 2, h - 4, text=str(i), fill=fg, font=("", 7))
