"""
PS1 DEV CART GUI: Profiler page (ps1transfer -prof -json, see ps1transfer/profiler.c).

Runs a program (a PS-X EXE, or a Net Yaroze game: auto script or ECOFF executable, with libps.exe) or attaches to
the running one, and shows every second what the profiler measures: what limits the frame rate (CPU, GPU, VSync,
CD-ROM) with hints, CPU load and what the program waits for, GPU busy, frames per second against the budget of the
video mode, DMA activity, RAM used and free, stack, SPU voices, and the functions where the time goes with their
caller. A double click on a function marks it as waiting (not CPU load), e.g. the VSync loop of a program without
symbols.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import json
import os

import ttkbootstrap as tb

from pages.base import Page
from programs import file_kind
from widgets import FileField, PS1_EXE_TYPES, labeled

RATE = "1000"       # Samples a second (root counter 0)
HISTORY = 60        # Seconds in the chart
RAM_START, RAM_PROGRAM, RAM_KB = 0x80000000, 0x80010000, 2048     # PlayStation RAM, programs load at 0x80010000
DMA = (("gpu", "GPU"), ("spu", "SPU"), ("cd", "CD-ROM"), ("mdec", "MDEC"), ("otc", "OT clear"))
PROGRAM_TYPES = [("PS1 executables, Net Yaroze auto script", "*.exe *.ps-exe *.psx *.cpe auto main")] + \
    PS1_EXE_TYPES[1:]
# Where the time goes: working (CPU load), then what the program waits for (ps1transfer "wait" keys)
TIME = (("work", "CPU", "#e5534b"), ("vsync", "VSync", "#57ab5a"), ("gpu", "GPU (DrawSync)", "#4c9be8"),
        ("cd", "CD-ROM", "#c69026"), ("spu", "SPU", "#b083f0"), ("other", "Other waits", "#768390"))
WAITS = {1: "VSync", 2: "GPU", 3: "CD", 4: "SPU", 5: "yes"}     # Waiting function (ps1transfer WAIT_*)
VERDICT_STYLE = {"ok": "success", "capped": "info", "vsync": "warning", "gpu": "warning", "cd": "warning",
                 "cpu": "danger", "unknown": "secondary", "noframes": "secondary", "none": "secondary"}
VERDICT_TITLE = {"ok": "Full frame rate", "capped": "Capped by VSync", "vsync": "Missing the VBlank",
                 "gpu": "GPU-bound", "cd": "Waiting for the CD-ROM", "cpu": "CPU-bound",
                 "unknown": "CPU load not known", "noframes": "No frames", "none": ""}


class ProfilerPage(Page):
    title = "Profiler"
    needs_devcart = "all"

    def build(self):
        s = self.app.settings
        self.history = []       # (cpu, gpu, fps) per second
        self.last = None        # Last PROF data

        tb.Label(self, text="PS1 DEV CART Profiler", font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="Uses symbols from (elf, map or sym) next to the program's own executable. "
                            "Double click a function to mark it as waiting.",
                 wraplength=800, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 6))

        box = self.section("Program")
        box.pack(fill="x", pady=(0, 6))
        self.exe = FileField(box, self.app, "prof.exe", "Executable", filetypes=PROGRAM_TYPES)
        if not self.exe.get():
            self.exe.set(s.field("run.exe", ""))
        self.exe.pack(fill="x")

        bar = tb.Frame(self)
        bar.pack(fill="x", pady=(0, 6))
        tb.Button(bar, text="Run and profile", bootstyle="success", width=16, command=self.start_run).pack(side="left")
        tb.Button(bar, text="Profile the running program", bootstyle="info-outline",
                  command=self.start_attach).pack(side="left", padx=8)
        tb.Button(bar, text="Stop", bootstyle="danger-outline", width=10, command=self.app.stop_task).pack(side="left")
        self.status = tb.Label(bar, text="", bootstyle="secondary")
        self.status.pack(side="left", padx=12)

        # Figures of the last second, a detail line under each
        stats = tb.Frame(self)
        stats.pack(fill="x", pady=(0, 6))
        self.values, self.details = {}, {}
        for key, text in (("cpu", "CPU"), ("gpu", "GPU"), ("fps", "Frames/s"), ("ram", "RAM used"),
                          ("free", "RAM free"), ("stack", "Stack"), ("spu", "SPU voices"), ("dropped", "Dropped")):
            cell = tb.Frame(stats, padding=(0, 0, 18, 0))
            cell.pack(side="left", anchor="n")     # (top: the detail lines have different lengths)
            tb.Label(cell, text=text, bootstyle="secondary").pack(anchor="w")
            self.values[key] = tb.Label(cell, text="-", font=("", 15, "bold"))
            self.values[key].pack(anchor="w")
            self.details[key] = tb.Label(cell, text="", bootstyle="secondary")
            self.details[key].pack(anchor="w")

        # What limits the frame rate, and advice from the whole run: under the functions and the charts (packed
        # first at the bottom, so it keeps its height when the window is small)
        verdict = tb.Labelframe(self, text="Diagnosis", padding=(10, 4))
        verdict.pack(side="bottom", fill="x", pady=(6, 0))
        self.verdict_title = tb.Label(verdict, text="-", font=("", 11, "bold"))
        self.verdict_title.pack(anchor="w")
        self.verdict = tb.Label(verdict, text="", wraplength=900, justify="left")
        self.verdict.pack(anchor="w")
        self.hints = tb.Label(verdict, text="", wraplength=900, justify="left", bootstyle="secondary")
        self.hints.pack(anchor="w", pady=(2, 0))
        self.session = tb.Label(verdict, text="", bootstyle="secondary")
        self.session.pack(anchor="w")
        verdict.bind("<Configure>", lambda e: [w.configure(wraplength=max(300, e.width - 30))
                                               for w in (self.verdict, self.hints)])

        # Functions where the program waits (not CPU load), above the diagnosis (also by a double click)
        _, self.idle = labeled(self, "Waiting:", tb.Entry)
        self.idle.master.pack(side="bottom", fill="x", pady=(6, 0))
        self.idle.insert(0, s.field("prof.idle", ""))

        panes = tb.Panedwindow(self, orient="horizontal")
        panes.pack(fill="both", expand=True)
        left = tb.Frame(panes)
        right = tb.Frame(panes)
        panes.add(left, weight=3)
        panes.add(right, weight=2)
        self.table = tb.Treeview(left, columns=("name", "now", "total", "samples", "wait", "caller"),
                                 show="headings", selectmode="browse")
        for col, text, width, anchor in (("name", "Function", 200, "w"), ("now", "Last second", 80, "e"),
                                         ("total", "Since start", 80, "e"), ("samples", "Samples", 70, "e"),
                                         ("wait", "Waiting", 60, "center"), ("caller", "Called from", 170, "w")):
            self.table.heading(col, text=text, anchor=anchor)
            self.table.column(col, width=width, anchor=anchor, stretch=(col in ("name", "caller")))
        self.table.pack(side="left", fill="both", expand=True)
        scroll = tb.Scrollbar(left, orient="vertical", command=self.table.yview)
        scroll.pack(side="left", fill="y")
        self.table.configure(yscrollcommand=scroll.set)
        self.table.bind("<Double-1>", self.toggle_idle)

        self.chart = tb.Canvas(right, height=110, highlightthickness=0)
        self.chart.pack(fill="x", padx=(8, 0))
        self.hw = tb.Canvas(right, height=230, highlightthickness=0)
        self.hw.pack(fill="both", expand=True, padx=(8, 0), pady=(6, 0))
        self.chart.bind("<Configure>", lambda e: self.draw())
        self.hw.bind("<Configure>", lambda e: self.draw())

    # ---------------------------------------------------------------- run

    def _common(self):
        """Profiler arguments: 1000 samples a second on root counter 0 (ps1transfer's defaults)."""
        self.app.settings.set_field("prof.idle", self.idle.get().strip())
        args = ["-prof", RATE, "-json"]
        if self.idle.get().strip():
            args += ["-idle", self.idle.get().strip().replace(" ", "")]
        return args

    def libps(self):
        """libps.exe entered on the Run Yaroze page."""
        return self.app.settings.field("run.libps", "").strip()

    def _program(self, exe):
        """Arguments running the program: a PS-X EXE, or a Net Yaroze game (auto script or ECOFF executable) with
        the library of the Run Yaroze page. The free RAM is always filled first (RAM use measured)."""
        kind = file_kind(exe)
        if kind == "psx":
            return ["-r", "-e", "-f", exe]
        return ["-r"] + (["-auto", exe] if kind == "auto" else ["-e", "-f", exe]) + ["-y", self.libps()]

    def args(self):
        exe = self.exe.get()
        return (self._program(exe) if exe else []) + ["-prof", RATE]

    def start_run(self):
        exe = self.exe.get()
        if not exe:
            self.app.error("Choose the program to profile.")
            return
        program = self._program(exe)
        if "-y" in program and not os.path.isfile(self.libps()):
            self.app.error("This is a Net Yaroze game: it needs libps.exe, the Net Yaroze library.\n\n"
                           "Enter the libps.exe path on the Run Yaroze page first.")
            return
        self.exe.remember()
        self.ram_note = None
        self._start(program + self._common())

    def start_attach(self):
        self.ram_note = "not measured"      # (the program was started without filling the free RAM)
        self._start(self._common())

    def _start(self, args):
        self.history = []
        self.last = None
        self.table.delete(*self.table.get_children())
        for v in self.values.values():
            v.configure(text="-")
        for v in self.details.values():
            v.configure(text="")
        for v in (self.verdict, self.hints, self.session):
            v.configure(text="")
        self.verdict_title.configure(text="-", bootstyle="default")
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
        elif line.startswith("Profiling:") or line.startswith("RAM use measured") or line.startswith("Profiler:"):
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

    def marked_pct(self, data):
        """Time of the last second in the functions marked as waiting on this page (not known by ps1transfer yet)."""
        marks = self.marked()
        return sum(r[2] for r in data.get("recent", []) if r[0] in marks and not r[3])

    def cpu(self, data):
        """CPU load, without the functions marked as waiting on this page."""
        return max(0.0, data.get("cpu", 0.0) - self.marked_pct(data))

    def show(self, data, again=False):
        self.last = data
        cpu = self.cpu(data)
        known = data.get("cpu_known", True) or self.marked_pct(data) > 0
        frame = data.get("frame", {})
        fps = data.get("fps", 0.0)
        if not again:
            self.history = (self.history + [(cpu, data.get("gpu", 0.0), fps)])[-HISTORY:]

        self.values["cpu"].configure(text=("%.0f%%" % cpu) if known else "%.0f%%?" % cpu,
                                     bootstyle="danger" if known and cpu > 95 else "default")
        self.details["cpu"].configure(text=("%.1f ms a frame" % (frame["ms"] * cpu / 100)) if known and
                                      frame.get("ms") else ("" if known else "waits not known"))
        session = data.get("session", {})
        self.values["gpu"].configure(text="%.0f%%" % data.get("gpu", 0))
        self.details["gpu"].configure(text=("peak %.0f%%" % session["gpu_max"]) if session else "")
        target = frame.get("target", 60)
        self.values["fps"].configure(text="%.1f" % fps,
                                     bootstyle="success" if fps >= target * 0.95 else "warning" if fps else "default")
        video = data.get("video")
        mode = ("PAL" if video["pal"] else "NTSC") if video else "%d Hz?" % target
        self.details["fps"].configure(text=("%.1f ms / %.1f ms (%s)" % (frame["ms"], frame["budget_ms"], mode))
                                      if frame.get("ms") else mode)
        ram = data.get("ram")
        if ram:
            # Of the PlayStation's 2MB: everything below the free RAM measured (kernel 64KB, the program with its
            # variables) counts as used, plus the free RAM the program has written (the stack, above it, isn't counted)
            program = (ram["lo"] - RAM_PROGRAM) // 1024
            used = (ram["lo"] - RAM_START) // 1024 + ram["used"] // 1024
            free = ram.get("free", ram["hi"] - ram["lo"] - ram["used"]) // 1024
            self.values["ram"].configure(text="{:,} / {:,} KB".format(used, RAM_KB))
            self.details["ram"].configure(text="kernel 64 KB + program {:,} KB + {:,} KB written".format(
                program, ram["used"] // 1024))
            self.values["free"].configure(text="{:,} KB".format(free), bootstyle="danger" if free < 64 else "default")
            self.details["free"].configure(text="largest block {:,} KB".format(ram["gap"] // 1024))
        else:
            self.values["ram"].configure(text=getattr(self, "ram_note", None) or "-")
            self.details["ram"].configure(text="")
            self.values["free"].configure(text="-", bootstyle="default")
            self.details["free"].configure(text="")
        self.values["stack"].configure(text=("%d B" % data["stack"]) if data.get("stack") else "-")
        self.details["stack"].configure(text=("peak %d B" % data["stack_peak"]) if data.get("stack_peak") else "")
        spu = data.get("spu")
        self.values["spu"].configure(text=("%d / 24" % spu["playing"]) if spu else "-")
        self.details["spu"].configure(text=("%.0f%%, peak %d (%.0f%%)" % (spu["playing"] * 100 / 24, spu["peak"],
                                                                          spu["peak"] * 100 / 24))
                                      if spu and "peak" in spu else "")
        self.values["dropped"].configure(text=str(data.get("dropped", 0)))
        self.details["dropped"].configure(text="%.0f samples/s" % data.get("rate", 0))
        if not data.get("symbols"):
            self.status.configure(text="No symbols: put the .elf, .map or .sym next to the program (addresses "
                                       "grouped by 64 bytes)")

        verdict = data.get("verdict", {})
        kind = verdict.get("kind", "none")
        self.verdict_title.configure(text=VERDICT_TITLE.get(kind, kind) or "-",
                                     bootstyle=VERDICT_STYLE.get(kind, "default"))
        self.verdict.configure(text=verdict.get("text", ""))
        self.hints.configure(text="\n".join("- " + h for h in data.get("hints", [])))
        if session.get("seconds"):
            self.session.configure(text="After loading (%d s): %.1f fps on average, %.1f at the lowest; peaks: CPU "
                                        "%.0f%%, GPU %.0f%%" % (session["seconds"], session["fps_avg"],
                                                               session["fps_min"], session["cpu_max"],
                                                               session["gpu_max"]))

        marks = self.marked()
        now = {r[0]: r[2] for r in data.get("recent", [])}
        self.table.delete(*self.table.get_children())
        for row in data.get("top", []):
            name, count, pct, idle = row[:4]
            caller = ("%s (%.0f%%)" % (row[4], row[5])) if len(row) > 5 and row[4] else ""
            wait = WAITS.get(idle, "yes") if idle else ("marked" if name in marks else "")
            self.table.insert("", "end", values=(name, "%.1f%%" % now[name] if name in now else "",
                                                 "%.1f%%" % pct, count, wait, caller))
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
        target = ((self.last or {}).get("frame") or {}).get("target", 60)
        colors = (("#e5534b", "CPU %"), ("#4c9be8", "GPU %"), ("#57ab5a", "fps (%d = top)" % target))
        for i, (color, label) in enumerate(colors):
            c.create_text(4 + i * 110, h - 6, text=label, fill=color, anchor="w", font=("", 8))
            if len(self.history) > 1:
                pts = []
                for j, values in enumerate(self.history):
                    v = values[i] / (float(target) if i == 2 else 100.0)
                    pts += [1 + (w - 2) * (j + HISTORY - len(self.history)) / (HISTORY - 1),
                            (h - 14) - min(1.0, v) * (h - 16)]
                c.create_line(*pts, fill=color, width=2)

        c = self.hw
        c.delete("all")
        w, h = max(c.winfo_width(), 100), max(c.winfo_height(), 100)
        data = self.last or {}

        # Where the time goes: CPU load, then the waits (the functions marked on this page as "other")
        wait = dict(data.get("wait", {}))
        wait["other"] = wait.get("other", 0.0) + (self.marked_pct(data) if data else 0.0)
        parts = [("work", self.cpu(data) if data else 0.0)] + [(key, wait.get(key, 0.0)) for key, _, _ in TIME[1:]]
        c.create_text(0, 6, text="Where the time goes", fill=fg, anchor="w", font=("", 9, "bold"))
        x, y = 0.0, 16
        for (key, pct), (_, label, color) in zip(parts, TIME):
            width = (w - 2) * min(pct, 100) / 100
            if width >= 1:
                c.create_rectangle(1 + x, y, 1 + x + width, y + 12, fill=color, width=0)
            x += width
        c.create_rectangle(1, y, w - 1, y + 12, outline=grid)
        legend_x, legend_y = 0, y + 22
        for (key, pct), (_, label, color) in zip(parts, TIME):
            text = "%s %.0f%%" % (label, pct)
            if legend_x + 8 + len(text) * 6 > w:
                legend_x, legend_y = 0, legend_y + 13
            c.create_rectangle(legend_x, legend_y - 4, legend_x + 8, legend_y + 4, fill=color, width=0)
            c.create_text(legend_x + 11, legend_y, text=text, fill=fg, anchor="w", font=("", 8))
            legend_x += 20 + len(text) * 6

        dma = data.get("dma", {})
        rows = [(label, dma.get(key, 0.0)) for key, label in DMA] + [("CD busy", data.get("cd", 0.0))]
        y0 = legend_y + 16
        c.create_text(0, y0, text="DMA busy (% of the time)", fill=fg, anchor="w", font=("", 9, "bold"))
        for i, (label, pct) in enumerate(rows):
            y = y0 + 14 + i * 13
            c.create_text(0, y, text=label, fill=fg, anchor="w", font=("", 8))
            c.create_rectangle(60, y - 4, w - 40, y + 4, outline=grid)
            c.create_rectangle(60, y - 4, 60 + (w - 100) * min(pct, 100) / 100, y + 4, fill="#4c9be8", width=0)
            c.create_text(w - 2, y, text="%.0f%%" % pct, fill=fg, anchor="e", font=("", 8))
        y0 += 14 + len(rows) * 13 + 6
        spu = data.get("spu")
        c.create_text(0, y0, text="SPU voices (envelope)" + ("" if not spu or spu.get("on") else ": SPU off"),
                      fill=fg, anchor="w", font=("", 9, "bold"))
        bw = (w - 4) / 24.0
        top, bottom = y0 + 10, max(h - 12, y0 + 24)
        for i in range(24):
            x = 2 + i * bw
            c.create_rectangle(x + 1, top, x + bw - 1, bottom, outline=grid)
            env = spu["env"][i] if spu else 0
            if env:
                c.create_rectangle(x + 1, bottom - (bottom - top) * env / 32767.0, x + bw - 1, bottom,
                                   fill="#57ab5a", width=0)
            c.create_text(x + bw / 2, bottom + 8, text=str(i), fill=fg, font=("", 7))
