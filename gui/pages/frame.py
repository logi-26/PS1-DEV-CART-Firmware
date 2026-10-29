"""
PS1 DEV CART GUI: Frame Inspector page (the GPU commands of a frame, on its screenshot).

"Run with frame capture" starts the program in a debug session (ps1transfer -frame, see the end of
ps1transfer/gdbserver.c). "Capture a frame" stops it when it hands its ordering tables to the GPU (DrawOTag, found in
its symbol file, between two VSync calls: one frame; without it, when it starts a GPU DMA) and reads every packet of
the tables: each primitive is listed
(polygons, sprites, tiles, lines, fills, draw modes...), with its colors, vertices, texture page and CLUT, and
outlined on the frame it drew (read from VRAM). A click on the frame selects the primitive drawn there (the last one
drawn: on top).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import struct
import tempfile
import tkinter as tk

import ttkbootstrap as tb

import gpu
import symbols
from livelink import FRAME_CAPTURE
from pages.base import Page
from pages.memory import SYMBOL_TYPES
from widgets import FileField, MonoText, PS1_EXE_TYPES

FILTERS = {"All": None, "Polygons": ("poly",), "Sprites and tiles": ("rect",), "Lines": ("line",),
           "Fills, transfers": ("fill", "copy"), "Draw modes": ("env",)}
DRAW_FUNCTIONS = ("DrawOTag", "DrawOTagEnv")
CODE_CHECK = 16         # Bytes of DrawOTag/VSync compared between an ELF symbol file and the executable


def exe_code(path):
    """A PS-X EXE's code: (load address, bytes), None if it isn't one (e.g. a Net Yaroze executable)."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    if data[:8] != b"PS-X EXE" or len(data) < 0x800:
        return None
    t_addr, t_size = struct.unpack_from("<2I", data, 0x18)
    return t_addr, data[0x800:0x800 + t_size]


def elf_code(path, adrs, size):
    """The bytes an ELF loads at adrs (its program headers), None if not loaded there or not an ELF."""
    try:
        with open(path, "rb") as f:
            d = f.read()
        if d[:4] != b"\x7fELF":
            return None
        phoff, = struct.unpack_from("<I", d, 0x1C)
        phentsize, phnum = struct.unpack_from("<2H", d, 0x2A)
        for i in range(phnum):
            typ, off, vaddr, _, filesz = struct.unpack_from("<5I", d, phoff + i * phentsize)
            if typ == 1 and vaddr <= adrs and adrs + size <= vaddr + filesz:
                return d[off + adrs - vaddr:off + adrs - vaddr + size]
    except (OSError, struct.error):
        pass
    return None


def draw_symbols(path, exe):
    """DrawOTag (or DrawOTagEnv) and VSync of a symbol file checked against the executable: (draw, vsync, problem),
    problem None if they are its own (in its code, the same code for an ELF)."""
    try:
        found = {s.name: s.adrs for s in symbols.load(path) if s.name in DRAW_FUNCTIONS + ("VSync",)}
    except (OSError, ValueError, struct.error):
        return None, None, "%s can't be read as a symbol file" % os.path.basename(path)
    draw = next((found[n] for n in DRAW_FUNCTIONS if n in found), None)
    vsync = found.get("VSync")
    code = exe_code(exe)
    if code:
        t_addr, text = code
        for name, adrs in (("DrawOTag", draw), ("VSync", vsync)):
            if adrs is None:
                continue
            if not t_addr <= adrs < t_addr + len(text) - CODE_CHECK:
                return None, None, ("%s isn't the symbol file of %s (its %s at 0x%08X is outside the program)" %
                                    (os.path.basename(path), os.path.basename(exe), name, adrs))
            elf = elf_code(path, adrs, CODE_CHECK)
            if elf is not None and elf != text[adrs - t_addr:adrs - t_addr + CODE_CHECK]:
                return None, None, ("%s isn't the symbol file of %s (another build: its %s code differs)" %
                                    (os.path.basename(path), os.path.basename(exe), name))
    return draw, vsync, None
OUTLINE, SELECTED = "#00d0ff", "#ffe000"


class FramePage(Page):
    title = "Frame Inspector"
    needs_devcart = "all"

    def build(self):
        self.running = False        # The frame capture session answers
        self.sym_note = ""          # Symbol file problem (another program's), shown in the status
        self.prims = []             # gpu.Prim of the frame captured
        self.shown = []             # The ones in the list (filter)
        self.env = None             # Draw area (x1, y1, x2, y2) and offset (ox, oy)
        self.vram = None            # (x, y, w, h, PIL image) of the frame drawn
        self.selected = None
        self.photo = None
        self.view = None            # Canvas mapping: (scale, left, top)
        self.draw = None            # DrawOTag address (symbol file), None: the DMA way
        self.vsync = None           # VSync address (symbol file): the frame's limits
        self.vram_file = os.path.join(tempfile.gettempdir(), "ps1devcart_frame.bin")

        tb.Label(self, text=self.title, font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="The GPU commands of a frame: run the program with frame capture, then capture a frame. "
                            "Each primitive is listed and outlined on the frame it drew; click the frame to find the "
                            "primitive drawn there. The symbol file lets it stop at DrawOTag (any SDK).",
                 wraplength=930, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 6))
        box = self.section("Program")
        box.pack(fill="x", pady=(0, 6))
        self.exe = FileField(box, self.app, "frame.exe", "Executable", filetypes=PS1_EXE_TYPES)
        self.exe.pack(fill="x", pady=2)
        self.sym = FileField(box, self.app, "frame.sym", "Symbol file", filetypes=SYMBOL_TYPES)
        self.sym.pack(fill="x", pady=2)
        bar = tb.Frame(box)
        bar.pack(fill="x", pady=(6, 0))
        tb.Button(bar, text="Run with frame capture", bootstyle="success", command=self.run).pack(side="left")
        tb.Button(bar, text="Stop", bootstyle="danger-outline", width=8, command=self.app.stop_task).pack(
            side="left", padx=6)
        self.remote_pad = tk.BooleanVar(value=self.app.settings.field("frame.pad", True))
        tb.Checkbutton(bar, text="Remote controller", variable=self.remote_pad, bootstyle="round-toggle").pack(
            side="left", padx=(10, 0))
        self.status = tb.Label(bar, text="Not running. (Empty symbol file: the .elf/.map next to the program)",
                               bootstyle="secondary", wraplength=560, justify="left")
        self.status.pack(side="left", padx=12)

        bar = tb.Frame(self)
        bar.pack(fill="x", pady=(0, 4))
        self.capture_button = tb.Button(bar, text="Capture a frame", bootstyle="primary", command=self.capture,
                                        state="disabled")
        self.capture_button.pack(side="left")
        tb.Label(bar, text="Show").pack(side="left", padx=(16, 4))
        self.filter = tb.Combobox(bar, state="readonly", width=16, values=list(FILTERS))
        self.filter.set("All")
        self.filter.pack(side="left")
        self.filter.bind("<<ComboboxSelected>>", lambda e: self.fill_list())
        self.outline_all = tk.BooleanVar(value=True)
        tb.Checkbutton(bar, text="Outline all", variable=self.outline_all, bootstyle="round-toggle",
                       command=self.draw_frame).pack(side="left", padx=(12, 0))
        self.info = tb.Label(bar, text="", bootstyle="secondary")
        self.info.pack(side="left", padx=12)

        panes = tb.Panedwindow(self, orient="horizontal")
        panes.pack(fill="both", expand=True)
        left = tb.Frame(panes)
        panes.add(left, weight=2)
        columns = (("n", "#", 45, "e"), ("name", "Command", 210, "w"), ("color", "Color", 70, "w"),
                   ("pos", "Position", 80, "w"))
        self.tree = tb.Treeview(left, columns=[c[0] for c in columns], show="headings")
        for key, text, width, anchor in columns:
            self.tree.heading(key, text=text, anchor=anchor)
            self.tree.column(key, width=width, anchor=anchor, stretch=key == "name")
        scroll = tb.Scrollbar(left, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=scroll.set)
        self.tree.pack(side="left", fill="both", expand=True)
        scroll.pack(side="left", fill="y")
        self.tree.bind("<<TreeviewSelect>>", lambda e: self.tree_selected())
        right = tb.Frame(panes, padding=(8, 0, 0, 0))
        panes.add(right, weight=3)
        self.details = MonoText(right, height=6)       # (packed first: the frame takes the space left)
        self.details.pack(side="bottom", fill="x", pady=(6, 0))
        self.canvas = tk.Canvas(right, highlightthickness=0, background="#000000", cursor="crosshair")
        self.canvas.pack(fill="both", expand=True)
        self.canvas.bind("<Configure>", lambda e: self.draw_frame())
        self.canvas.bind("<Button-1>", self.canvas_click)
        self.app.live_listen(self)

    # ---------------------------------------------------------------- session

    def auto_symbol_file(self):
        """The symbol file next to the program (same name), None if there is none."""
        for ext in (".elf", ".map", ".sym", ".ELF", ".MAP", ".SYM"):
            path = os.path.splitext(self.exe.get())[0] + ext
            if os.path.isfile(path):
                return path
        return None

    def find_symbols(self):
        """DrawOTag and VSync for the capture, from the symbol file given (checked against the program) or the one
        next to the program; self.sym_note: what went wrong, shown to the user."""
        self.draw = self.vsync = None
        self.sym_note = ""
        given, auto = self.sym.get(), self.auto_symbol_file()
        if given:
            self.draw, self.vsync, problem = draw_symbols(given, self.exe.get())
            if not problem:
                return
            self.sym_note = problem
            if auto and os.path.normcase(os.path.abspath(auto)) != os.path.normcase(os.path.abspath(given)):
                draw, vsync, problem = draw_symbols(auto, self.exe.get())
                if not problem:
                    self.draw, self.vsync = draw, vsync
                    self.sym_note += ": %s used instead." % os.path.basename(auto)
                    return
            self.sym_note += "."
        elif auto:
            self.draw, self.vsync, problem = draw_symbols(auto, self.exe.get())
            self.sym_note = problem + "." if problem else ""

    def args(self):
        if not self.exe.get():
            return None
        return ["-frame", "-f", self.exe.get()] + (["-pad"] if self.remote_pad.get() else [])

    def run(self):
        if not os.path.isfile(self.exe.get()):
            return self.app.error("Choose the executable (PS-X EXE) to run.")
        self.exe.remember()
        self.sym.remember()
        self.app.settings.set_field("frame.pad", self.remote_pad.get())
        self.find_symbols()
        self.status.configure(text="Starting the program in a debug session...", bootstyle="secondary")
        self.app.live_start(self.args(), FRAME_CAPTURE, cwd=os.path.dirname(self.exe.get()))
        return None

    def capture(self):
        self.capture_button.configure(state="disabled")
        self.status.configure(text="Capturing (the program stops at %s)..." % (
            "DrawOTag 0x%08X" % self.draw if self.draw else "its next GPU DMA"))
        self.app.live_send("capture %s %s\t%s" % ("%08X" % self.draw if self.draw else "-",
                                                  "%08X" % self.vsync if self.vsync else "-", self.vram_file))

    def using_live(self):
        return self.running

    def live_stopped(self):
        if self.running:
            self.status.configure(text="Stopped.")
        self.running = False
        self.capture_button.configure(state="disabled")

    def live_line(self, line):
        if self.app.live_task() != FRAME_CAPTURE:
            return
        fields = line.split()
        if line.startswith("FRMLIVE ready"):
            self.running = True
            self.capture_button.configure(state="normal")
            self.status.configure(text="Running: capture a frame. %s%s" % (
                "Stops at DrawOTag." if self.draw else "No DrawOTag in a symbol file of the program: stops at the "
                "GPU DMA (lists queued by the library are missed: partial frames).",
                "\n" + self.sym_note if self.sym_note else ""),
                bootstyle="secondary" if self.draw and not self.sym_note else "warning")
        elif line.startswith("FRMBEGIN"):
            self.prims, self.env, self.vram, self.selected = [], None, None, None
        elif line.startswith("FRMPKT") and len(fields) >= 4:
            try:
                self.prims += gpu.decode(int(fields[1]), int(fields[2], 16), [int(v, 16) for v in fields[3:]])
            except ValueError:
                pass
        elif line.startswith("FRMENV") and len(fields) >= 7 and self.env is None:
            v = [int(x) for x in fields[1:7]]
            self.env = (v[0], v[1], v[2], v[3], v[4], v[5])
        elif line.startswith("FRMVRAM") and len(fields) >= 6:
            self.load_vram([int(x) for x in fields[1:5]], line.split(None, 5)[5])
        elif line.startswith("FRMEND"):
            self.capture_button.configure(state="normal")
            self.status.configure(text="Frame captured." + ("\n" + self.sym_note if self.sym_note else ""))
            self.fill_list()
            self.draw_frame()
        elif line.startswith("FRMERR"):
            self.capture_button.configure(state="normal" if self.running else "disabled")
            self.status.configure(text=line[7:])

    def load_vram(self, rect, path):
        from PIL import Image
        x, y, w, h = rect
        try:
            with open(path, "rb") as f:
                data = f.read()
            pixels = struct.unpack("<%dH" % (w * h), data[:w * h * 2])
        except (OSError, struct.error):
            return
        img = Image.new("RGB", (w, h))
        img.putdata([((v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3) for v in pixels])
        self.vram = (x, y, w, h, img)

    # ---------------------------------------------------------------- list, frame

    def fill_list(self):
        self.tree.delete(*self.tree.get_children())
        kinds = FILTERS.get(self.filter.get())
        self.shown = [p for p in self.prims if kinds is None or p.kind in kinds]
        for i, p in enumerate(self.shown):
            pos = "%d,%d" % p.points[0] if p.points else ""
            self.tree.insert("", "end", iid=str(i), values=(self.prims.index(p) + 1, p.name, p.color_text(), pos))
        counts = {}
        for p in self.prims:
            counts[p.kind] = counts.get(p.kind, 0) + 1
        self.info.configure(text="%d commands: %d polygons, %d sprites/tiles, %d lines" % (
            len(self.prims), counts.get("poly", 0), counts.get("rect", 0), counts.get("line", 0)) +
            ("  |  draw area %d,%d-%d,%d, offset %d,%d" % self.env if self.env else ""))

    def tree_selected(self):
        sel = self.tree.selection()
        if sel:
            self.select(self.shown[int(sel[0])], from_tree=True)

    def select(self, prim, from_tree=False):
        self.selected = prim
        if not from_tree and prim in self.shown:
            iid = str(self.shown.index(prim))
            self.tree.selection_set(iid)
            self.tree.see(iid)
        self.details.clear()
        ox, oy = self.offset()
        lines = ["%s  (#%d, list %d, packet at %08X)" % (prim.name, self.prims.index(prim) + 1, prim.list_no,
                                                         prim.adrs)]
        if prim.colors:
            lines.append("Colors: " + " ".join("#%02X%02X%02X" % c for c in prim.colors))
        if prim.points:
            lines.append("Vertices: " + " ".join("%d,%d" % pt for pt in prim.points) +
                         ("   (VRAM: %s)" % " ".join("%d,%d" % (x + ox, y + oy) for x, y in prim.points)))
        if prim.details:
            lines.append(", ".join(prim.details))
        lines.append("Words: " + " ".join("%08X" % w for w in prim.words[:16]))
        self.details.append("\n".join(lines) + "\n")
        self.draw_frame()

    def offset(self):
        return (self.env[4], self.env[5]) if self.env else (0, 0)

    def to_canvas(self, x, y, vram=False):
        """Drawing coordinates (or VRAM ones) to the canvas."""
        scale, left, top = self.view
        ox, oy = (0, 0) if vram else self.offset()
        x0, y0 = self.vram[:2] if self.vram else (self.env[0], self.env[1]) if self.env else (0, 0)
        return left + (x + ox - x0) * scale, top + (y + oy - y0) * scale

    def outline(self, p, color, width):
        if p.kind == "fill" and p.vram_rect:
            x, y, w, h = p.vram_rect
            pts = [self.to_canvas(x, y, True), self.to_canvas(x + w, y + h, True)]
            self.canvas.create_rectangle(*pts[0], *pts[1], outline=color, width=width)
        elif len(p.points) >= 2:
            pts = [c for pt in p.points for c in self.to_canvas(*pt)]
            if p.kind == "line":
                self.canvas.create_line(*pts, fill=color, width=width)
            else:
                self.canvas.create_polygon(*pts, outline=color, fill="", width=width)

    def draw_frame(self):
        from PIL import Image, ImageTk
        c = self.canvas
        c.delete("all")
        cw, ch = max(c.winfo_width(), 1), max(c.winfo_height(), 1)
        if self.vram:
            w, h, img = self.vram[2], self.vram[3], self.vram[4]
        elif self.env:
            w, h, img = self.env[2] - self.env[0] + 1, self.env[3] - self.env[1] + 1, None
        else:
            self.view = None
            c.create_text(cw // 2, ch // 2, text="No frame captured", fill="#808080")
            return
        scale = min(cw / w, ch / h)
        left, top = (cw - w * scale) / 2, (ch - h * scale) / 2
        self.view = (scale, left, top)
        if img is not None:
            self.photo = ImageTk.PhotoImage(img.resize((max(1, int(w * scale)), max(1, int(h * scale))),
                                                       Image.NEAREST if scale >= 1 else Image.LANCZOS))
            c.create_image(left, top, image=self.photo, anchor="nw")
        if self.outline_all.get():
            for p in self.shown:
                if p.kind in ("poly", "rect", "line"):
                    self.outline(p, OUTLINE, 1)
        if self.selected is not None:
            self.outline(self.selected, SELECTED, 2)

    def canvas_click(self, event):
        """The primitive drawn at this place (the last one drawn: on top)."""
        if not self.view:
            return
        scale, left, top = self.view
        ox, oy = self.offset()
        x0, y0 = self.vram[:2] if self.vram else (self.env[0], self.env[1]) if self.env else (0, 0)
        x, y = (event.x - left) / scale + x0 - ox, (event.y - top) / scale + y0 - oy
        for p in reversed(self.shown):
            if p.kind in ("poly", "rect", "line") and gpu.inside(p, x, y):
                self.select(p)
                return
