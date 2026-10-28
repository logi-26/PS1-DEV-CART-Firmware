"""
PS1 DEV CART GUI: TIM / VRAM page (TIM and VRAM viewer with a live palette editor).

The TIM files are shown here in a VRAM view (outlines of each image and CLUT, as the console's viewer draws them) and
one at a time with its palette, which can be edited. "Run on the console" starts the console's TIM viewer
(ps1transfer -tim ... -c -timlive, see ps1transfer/timlive.c): the VRAM view is then read from the console, each
palette change made here is sent at once (the TV picture and the console's own palette editor show it), and the
changes made with the console's palette editor come back here. Without the console the page works on the files alone.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import struct
import tempfile
import time
import tkinter as tk

import ttkbootstrap as tb

from pages.base import Page
from programs import auto_files, file_kind
from widgets import FileList, ImageView

TIM_TYPES = [("TIM images, Net Yaroze auto script", "*.tim auto"), ("All files", "*.*")]
MAX_FILES = 32              # Data block of the console's viewer
VRAM_W, VRAM_H = 1024, 512
MODES = ("4-bit", "8-bit", "16-bit", "24-bit")
OUTLINES = ("#00ff00", "#00a0ff", "#ff00ff", "#ffa000", "#00ffff", "#a0ff00", "#ffffff", "#a078ff")
SWATCH = 17                 # Palette swatch size (pixels)
SEND_DELAY = 40             # ms: palette changes sent together (a slider sends many)
POLL_MS = 1000              # Palettes read back from the console (its own palette editor)
SEL_POLL_MS = 300           # TIM shown on the console (chosen with its pad)
SEL_QUIET = 1.0             # s: the console's selection not taken right after one chosen here
QUIET_AFTER_EDIT = 1.5      # s: no read back right after a change made here
PROTOCOL = ("TIMLIVE", "TIMINFO", "TIMCLUT", "TIMVRAM", "TIMSEL", "TIMAREA")    # Answers of ps1transfer -timlive (not in the log)
NIBBLE_SWAP = bytes(((b & 15) << 4) | (b >> 4) for b in range(256))     # 4-bit TIM: low nibble first


def ps_to_rgb(v):
    """PS1 15-bit color (red bits 0-4, green 5-9, blue 10-14) to 8-bit RGB."""
    return ((v & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, ((v >> 10) & 31) * 255 // 31)


def rgb_hex(v):
    return "#%02x%02x%02x" % ps_to_rgb(v)


class Tim:
    """A TIM file: its image and CLUT blocks, the CLUT colors (edited here) and the original ones."""

    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        with open(path, "rb") as f:
            self.data = bytearray(f.read())
        if len(self.data) < 8 or struct.unpack_from("<I", self.data, 0)[0] != 0x10:
            raise ValueError("not a TIM file")
        flags = struct.unpack_from("<I", self.data, 4)[0]
        self.mode = flags & 3
        offset = 8
        self.clut = None        # (x, y, w, h) in VRAM
        self.colors = []
        if flags & 8:
            length, x, y, w, h = struct.unpack_from("<IHHHH", self.data, offset)
            self.clut = (x, y, w, h)
            self.clut_offset = offset + 12
            self.colors = list(struct.unpack_from("<%dH" % (w * h), self.data, self.clut_offset))
            offset += length
        length, x, y, w, h = struct.unpack_from("<IHHHH", self.data, offset)
        self.img = (x, y, w, h)             # VRAM units (16-bit)
        self.img_offset = offset + 12
        if self.img_offset + w * h * 2 > len(self.data):
            raise ValueError("image data missing")
        self.original = list(self.colors)
        self.original_pos = (self.img[:2], self.clut[:2] if self.clut else None)

    @property
    def width(self):
        """Width in pixels of the image."""
        w = self.img[2]
        return (w * 4, w * 2, w, w * 2 // 3)[self.mode]

    @property
    def rows(self):
        return self.clut[3] if self.clut else 0

    def row(self, r):
        w = self.clut[2]
        return self.colors[r * w:(r + 1) * w]

    def moved(self):
        return (self.img[:2], self.clut[:2] if self.clut else None) != self.original_pos

    def edited(self):
        return self.colors != self.original or self.moved()

    def move(self, kind, x, y):
        """New VRAM position of the image or the CLUT ("img" / "clut")."""
        if kind == "clut":
            self.clut = (x, y) + self.clut[2:]
        else:
            self.img = (x, y) + self.img[2:]

    def image(self, row=0):
        """PIL image of the TIM drawn with a CLUT row."""
        from PIL import Image
        x, y, w, h = self.img
        raw = bytes(self.data[self.img_offset:self.img_offset + w * h * 2])
        if self.mode == 2:
            return Image.frombytes("RGB", (w, h), raw, "raw", "RGB;15")
        if self.mode == 3:
            return Image.frombytes("RGB", (self.width, h), raw, "raw", "RGB", w * 2)
        if self.mode == 0:
            im = Image.frombytes("P", (w * 4, h), raw.translate(NIBBLE_SWAP), "raw", "P;4")
        else:
            im = Image.frombytes("P", (w * 2, h), raw, "raw", "P")
        colors = self.row(min(row, self.rows - 1)) if self.clut else []
        palette = []
        for v in colors:
            palette += ps_to_rgb(v)
        im.putpalette(palette + [0] * (768 - len(palette)))
        return im.convert("RGB")

    def bytes_with_colors(self):
        """The TIM file with the colors and the VRAM positions edited here (x, y in the CLUT and image headers)."""
        data = bytearray(self.data)
        if self.clut:
            struct.pack_into("<%dH" % len(self.colors), data, self.clut_offset, *self.colors)
            struct.pack_into("<HH", data, self.clut_offset - 8, *self.clut[:2])
        struct.pack_into("<HH", data, self.img_offset - 8, *self.img[:2])
        return data

    def save(self, path):
        with open(path, "wb") as f:
            f.write(self.bytes_with_colors())
        if os.path.normcase(os.path.abspath(path)) == os.path.normcase(os.path.abspath(self.path)):
            self.data = self.bytes_with_colors()
            self.original = list(self.colors)
            self.original_pos = (self.img[:2], self.clut[:2] if self.clut else None)


class TimPage(Page):
    title = "VRAM Viewer"

    def build(self):
        self.tims = []              # Tim objects of the file list
        self.selected = 0
        self.entry = 0              # Palette entry edited
        self.row_var = tk.IntVar(value=0)
        self.vram = None            # bytearray of the VRAM shown (from the console, or made from the files)
        self.vram_live = False
        self.live = None            # Tim objects of the console's viewer (its file order), None: not running
        self.pending = set()        # (index, row) to send
        self.send_job = None
        self.last_edit = 0.0
        self.read_sent = 0.0
        self.reading = None         # TIMCLUT lines being received
        self.sel_sent = 0.0         # When a TIM was chosen here (sent to the console)
        self.disconnected = False   # The console stopped answering (ps1transfer: "TIMLIVE disconnected")
        self.text_sent = 0.0        # When the TV text was shown/hidden here
        self.tv_text = tk.BooleanVar(value=self.app.settings.field("tim.tv_text", True))
        self.updating = False       # Sliders set by the program (not a change)
        self.vram_file = os.path.join(tempfile.gettempdir(), "ps1devcart_vram.bin")

        tb.Label(self, text="TIM and VRAM viewer", font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="Click a TIM in the VRAM to select it, right-click and drag to move it, click a color "
                            "to edit it. Run on the console: "
                            "changes appear at once on the TV and in the console's palette editor (and the other way "
                            "round). On the PS1: Select changes the view, Circle opens its palette editor.",
                 wraplength=1000, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 6))
        top = self.section("TIM files")
        top.pack(fill="x", pady=(0, 6))
        # (empty at each start; Add also takes a Net Yaroze auto script: its TIM files)
        self.files = FileList(top, self.app, "view.tims", "TIM files", filetypes=TIM_TYPES, max_files=MAX_FILES,
                              height=3, on_change=lambda: self.load_files(), compact=True, remember=False,
                              expand=self.expand_files)
        self.files.pack(fill="x")
        bar = tb.Frame(top)
        bar.pack(fill="x", pady=(6, 0))
        tb.Button(bar, text="Run on the console", bootstyle="success", command=self.run).pack(side="left")
        tb.Button(bar, text="Stop", bootstyle="danger-outline", width=8, command=self.app.stop_task).pack(
            side="left", padx=6)
        self.refresh_button = tb.Button(bar, text="Refresh VRAM", bootstyle="info-outline", command=self.read_vram,
                                        state="disabled")
        self.refresh_button.pack(side="left")
        tb.Checkbutton(bar, text="Controls on the TV", variable=self.tv_text, bootstyle="round-toggle",
                       command=self.send_text).pack(side="left", padx=(12, 0))
        self.status = tb.Label(bar, text="Not running.", bootstyle="secondary")
        self.status.pack(side="left", padx=12)

        panes = tb.Panedwindow(self, orient="horizontal")
        panes.pack(fill="both", expand=True)
        left = tb.Frame(panes)
        right = tb.Frame(panes, padding=(8, 0, 0, 0))
        panes.add(left, weight=3)
        panes.add(right, weight=2)

        # VRAM view
        self.vram_title = tb.Label(left, text="VRAM (made from the TIM files)", font=("", 10, "bold"))
        self.vram_title.pack(anchor="w")
        self.vram_canvas = tk.Canvas(left, highlightthickness=0, background="#000000")
        self.vram_canvas.pack(fill="both", expand=True, pady=(2, 0))
        self.vram_canvas.bind("<Configure>", lambda e: self.draw_vram())
        self.vram_canvas.bind("<Button-1>", self.vram_click)              # Select (also on the console)
        self.vram_canvas.bind("<ButtonPress-3>", self.vram_press)         # Right button drag: move
        self.vram_canvas.bind("<B3-Motion>", self.vram_drag)
        self.vram_canvas.bind("<ButtonRelease-3>", self.vram_release)
        self.drag = None            # Image or CLUT being moved in the VRAM view
        self.areas = []             # The console viewer's screen buffers and font (x, y, w, h): TIMs can't go there
        self.vram_photo = None
        self.vram_info = tb.Label(left, text="", bootstyle="secondary")
        self.vram_info.pack(anchor="w")
        self.vram_canvas.bind("<Motion>", self.vram_motion)

        # Selected TIM
        head = tb.Frame(right)
        head.pack(fill="x")
        self.choice = tb.Combobox(head, state="readonly", width=30)
        self.choice.pack(side="left", fill="x", expand=True)
        self.choice.bind("<<ComboboxSelected>>", lambda e: self.select(self.choice.current()))
        tb.Label(head, text="CLUT row").pack(side="left", padx=(8, 4))
        self.row_spin = tb.Spinbox(head, from_=0, to=0, width=4, textvariable=self.row_var,
                                   command=self.row_changed)
        self.row_spin.pack(side="left")
        self.row_spin.bind("<Return>", lambda e: self.row_changed())
        self.info = tb.Label(right, text="", bootstyle="secondary", justify="left")
        self.info.pack(anchor="w", pady=(2, 2))
        self.preview = ImageView(right, height=120)
        self.preview.canvas.configure(height=120)
        # Under the preview, packed from the bottom: the editor keeps its place, the preview takes what is left
        bottom = []

        label = tb.Label(right, text="Palette (click a color to edit it)", font=("", 10, "bold"))
        bottom.append((label, dict(anchor="w", pady=(6, 2))))
        # The palette and the color editor side by side
        lower = tb.Frame(right)
        bottom.append((lower, dict(fill="x")))
        self.palette = tk.Canvas(lower, width=16 * SWATCH + 2, height=SWATCH * 2 + 2, highlightthickness=0,
                                 background="#202020")
        self.palette.pack(side="left", anchor="n")
        self.palette.bind("<Button-1>", self.palette_click)
        self.swatch_size = SWATCH

        edit = tb.Frame(lower)
        edit.pack(side="left", fill="x", expand=True, anchor="n", padx=(10, 0))
        self.swatch = tk.Canvas(edit, width=44, height=44, highlightthickness=1, highlightbackground="#808080")
        self.swatch.grid(row=0, column=0, rowspan=3, padx=(0, 8))
        self.scales, self.scale_values = [], []
        for i, name in enumerate(("Red", "Green", "Blue")):
            tb.Label(edit, text=name, width=6).grid(row=i, column=1, sticky="w")
            var = tk.IntVar(value=0)
            scale = tb.Scale(edit, from_=0, to=31, variable=var, command=lambda v, i=i: self.slider(i))
            scale.grid(row=i, column=2, sticky="ew")
            value = tb.Label(edit, text="0", width=3)
            value.grid(row=i, column=3)
            self.scales.append(var)
            self.scale_values.append(value)
        edit.columnconfigure(2, weight=1)
        self.stp = tk.BooleanVar(value=False)
        tb.Checkbutton(edit, text="Semi-transparent (bit 15)", variable=self.stp, command=self.stp_changed).grid(
            row=3, column=1, columnspan=2, sticky="w", pady=(2, 0))
        self.entry_label = tb.Label(edit, text="", bootstyle="secondary")
        self.entry_label.grid(row=4, column=0, columnspan=4, sticky="w")

        buttons = tb.Frame(right)
        bottom.append((buttons, dict(fill="x", pady=(6, 0))))
        for text, command in (("Restore the color", self.restore_entry), ("Restore the row", self.restore_row),
                              ("Restore all", self.restore_all)):
            tb.Button(buttons, text=text, bootstyle="secondary-outline", command=command).pack(side="left",
                                                                                              padx=(0, 4))
        buttons = tb.Frame(right)
        bottom.append((buttons, dict(fill="x", pady=(4, 0))))
        tb.Button(buttons, text="Save a copy (NAME_edit.tim)", bootstyle="primary-outline",
                  command=self.save_copy).pack(side="left", padx=(0, 4))
        tb.Button(buttons, text="Save", bootstyle="primary-outline", command=self.save).pack(side="left")
        tb.Button(buttons, text="Restore the position", bootstyle="secondary-outline",
                  command=self.restore_position).pack(side="left", padx=(4, 0))
        for widget, options in reversed(bottom):
            widget.pack(side="bottom", **options)
        self.preview.pack(fill="both", expand=True)
        self.load_files()

    # ---------------------------------------------------------------- files

    @staticmethod
    def is_tim(path):
        try:
            with open(path, "rb") as f:
                head = f.read(4)
        except OSError:
            return False
        return len(head) == 4 and struct.unpack("<I", head)[0] == 0x10

    def expand_files(self, paths):
        """Files chosen with Add: a TIM file as it is, a Net Yaroze auto script replaced by the TIM files it loads
        ("local dload" lines, also of the scripts it calls)."""
        out, notes = [], []
        for path in paths:
            if self.is_tim(path) or file_kind(path) != "auto":
                out.append(path)
                continue
            found = [f for f in auto_files(path)[1:] if self.is_tim(f)]
            if not found:
                out.append(path)        # (not a TIM, not a script with TIM files: reported by load_files)
                continue
            out += found
            notes.append("%s: %d TIM files" % (os.path.basename(os.path.dirname(os.path.abspath(path))) or
                                                os.path.basename(path), len(found)))
        room = MAX_FILES - len(self.files.files())
        new = [p for p in dict.fromkeys(os.path.normpath(p) for p in out) if p not in self.files.files()]
        if len(new) > room:
            self.app.error("%d TIM files to add, the viewer shows up to %d: the first %d are added." %
                           (len(new), MAX_FILES, max(room, 0)))
            new = new[:max(room, 0)]
        if notes:
            self.status.configure(text="Auto script " + ", ".join(notes))
        return new

    def load_files(self):
        """Tim objects of the file list (the ones already loaded keep their edits)."""
        old = {t.path: t for t in self.tims}
        self.tims, errors = [], []
        for path in self.files.files():
            if path in old:
                self.tims.append(old[path])
                continue
            try:
                self.tims.append(Tim(path))
            except (OSError, ValueError, struct.error) as e:
                errors.append("%s: %s" % (os.path.basename(path), e))
        if errors:
            self.status.configure(text="Not loaded: " + "; ".join(errors))
        self.choice.configure(values=[t.name for t in self.tims])
        if not self.vram_live:
            self.vram = self.compose_vram()
        self.select(min(self.selected, len(self.tims) - 1) if self.tims else 0)

    def compose_vram(self):
        """VRAM made from the TIM files: each image and CLUT block at its position."""
        vram = bytearray(VRAM_W * VRAM_H * 2)
        for t in self.tims:
            x, y, w, h = t.img
            self.blit(vram, x, y, w, h, t.data, t.img_offset)
            if t.clut:
                x, y, w, h = t.clut
                self.blit(vram, x, y, w, h, t.bytes_with_colors(), t.clut_offset)
        return vram

    @staticmethod
    def blit(vram, x, y, w, h, data, offset):
        if x >= VRAM_W or y >= VRAM_H:
            return
        cw = min(w, VRAM_W - x)
        for r in range(min(h, VRAM_H - y)):
            src = offset + r * w * 2
            dst = ((y + r) * VRAM_W + x) * 2
            vram[dst:dst + cw * 2] = data[src:src + cw * 2]

    def current(self):
        return self.tims[self.selected] if 0 <= self.selected < len(self.tims) else None

    # ---------------------------------------------------------------- console

    def args(self):
        files = self.files.files()
        if not files:
            return None
        a = []
        for path in files:
            a += ["-tim", path]
        return a + ["-c", "-timlive"]

    def run(self):
        self.load_files()
        if not self.tims:
            self.app.error("Add the TIM files to show.")
            return
        args = []
        for t in self.tims:
            args += ["-tim", t.path]
        args += ["-c", "-timlive"]
        self.app.settings.set_field("view.server", True)
        self.live = None
        self.areas = []
        self.disconnected = False
        self.status.configure(text="Starting the console's TIM viewer...")
        running = list(self.tims)
        # (the console's palette editor saves/reloads in the folder of the first file, with the file server)
        self.app.run(args, "TIM viewer", long_running=True, stdin=True, cwd=os.path.dirname(self.tims[0].path),
                     on_line=lambda line: self.after(0, self.line, line, running),
                     log_filter=lambda line: not line.startswith(PROTOCOL),
                     on_done=lambda code, out: self.after(0, self.stopped))

    def stopped(self):
        """The console's viewer stopped (Stop, or the cartridge stopped answering): the page is cleared, the edits
        not saved can be saved first."""
        text = "Console disconnected." if self.disconnected else "Stopped."
        self.disconnected = False
        edited = [t for t in self.tims if t.edited()]
        if edited and self.app.confirm("Save the edited TIM files before the page is cleared?\n\n" +
                                       "\n".join(t.name for t in edited)):
            for t in edited:
                try:
                    t.save(t.path)
                except OSError as e:
                    self.app.error("Can't write %s: %s" % (t.path, e))
        self.clear_all(text)

    def on_device_changed(self):
        """No cartridge any more (Refresh): the page is cleared."""
        if not self.app.serial() and self.live is None:
            self.clear_all("No console connected.")

    def clear_all(self, text):
        """The TIM list, the VRAM view, the TIM and its palette cleared (the console was disconnected)."""
        self.live = None
        self.areas = []
        self.vram_live = False
        self.refresh_button.configure(state="disabled")
        self.tims = []
        self.files.clear()          # (load_files: no TIM, nothing selected)
        self.vram = None
        self.draw_vram()
        self.status.configure(text=text)

    def send(self, line):
        return self.live is not None and self.app.runner.send(line)

    def line(self, line, running):
        if line.startswith("TIMLIVE ready"):
            self.live = running
            self.refresh_button.configure(state="normal")
            self.status.configure(text="Live: palette changes go to the console at once.")
            # Changes made here before the start
            for i, t in enumerate(running):
                for r in range(t.rows):
                    if t.row(r) != t.original[r * t.clut[2]:(r + 1) * t.clut[2]]:
                        self.pending.add((i, r))
            self.flush()
            for t in running:           # (moved here, not saved: the console loaded the files as they are)
                if t.moved():
                    self.send_move(t)
            self.send_selection()       # (the TIM chosen here is shown on the console)
            self.send_text()
            self.read_vram()
            self.after(POLL_MS, self.poll)
            self.after(SEL_POLL_MS, self.sel_poll)
        elif line.startswith("TIMLIVE disconnected"):
            self.disconnected = True
        elif line.startswith("TIMAREA"):
            try:
                self.areas.append(tuple(int(v) for v in line.split()[1:5]))
            except ValueError:
                pass
        elif line.startswith("TIMSEL"):
            self.console_selection(line.split()[1:])
        elif line.startswith("TIMCLUT"):
            if line == "TIMCLUT end":
                self.read_done()
            elif self.reading is not None:
                self.reading.append(line.split()[1:])
        elif line.startswith("TIMVRAM ok"):
            try:
                with open(line[len("TIMVRAM ok "):].strip(), "rb") as f:
                    data = bytearray(f.read())
                if len(data) == VRAM_W * VRAM_H * 2:
                    self.vram, self.vram_live = data, True
            except OSError:
                pass
            self.status.configure(text="Live: palette changes go to the console at once.")
            self.draw_vram()
        elif line.startswith("TIMVRAM error") or line.startswith("TIMERR"):
            self.status.configure(text=line[line.index(" ") + 1:])

    def read_vram(self):
        if self.send("vram\t" + self.vram_file):
            self.status.configure(text="Reading the VRAM (about 3 seconds, the console waits meanwhile)...")

    def poll(self):
        """Palettes read back from the console (changed with its palette editor), when nothing is edited here."""
        if self.live is None:
            return
        if self.reading is None and not self.pending and time.time() - self.last_edit > QUIET_AFTER_EDIT:
            self.reading = []
            self.read_sent = time.time()
            if not self.send("read"):
                self.reading = None
        self.after(POLL_MS, self.poll)

    def sel_poll(self):
        """The TIM shown on the console, chosen with its pad (TIMSEL answer)."""
        if self.live is not None and self.send("state"):
            self.after(SEL_POLL_MS, self.sel_poll)

    def console_selection(self, fields):
        # Text shown on the TV (Start on the console)
        if self.live is not None and len(fields) > 2 and fields[2] in ("0", "1") and \
                time.time() - self.text_sent >= SEL_QUIET and (fields[2] == "1") != self.tv_text.get():
            self.tv_text.set(fields[2] == "1")
        if self.live is None or len(fields) < 2 or time.time() - self.sel_sent < SEL_QUIET:
            return      # ("none": a viewer without the shared selection)
        try:
            i, r = int(fields[0]), int(fields[1])
        except ValueError:
            return
        if not 0 <= i < len(self.live) or self.live[i] not in self.tims:
            return
        t, index = self.live[i], self.tims.index(self.live[i])
        if index != self.selected:
            self.select(index, send=False)
        if t.rows and 0 <= r < t.rows and r != self.row_var.get():
            self.row_var.set(r)
            self.show_tim()

    def send_text(self):
        """The console viewer's info panel and controls shown or hidden (also with Start on the console)."""
        self.app.settings.set_field("tim.tv_text", bool(self.tv_text.get()))
        if self.live is not None:
            self.text_sent = time.time()
            self.send("text %d" % (1 if self.tv_text.get() else 0))

    def send_selection(self):
        """The TIM (and CLUT row) chosen here, shown on the console."""
        t = self.current()
        if self.live is not None and t in self.live:
            self.sel_sent = time.time()
            self.send("select %d %d" % (self.live.index(t), self.row_var.get() if t.rows else -1))

    def read_done(self):
        rows, self.reading = self.reading, None
        if rows is None or self.live is None or self.last_edit > self.read_sent:     # (a change made meanwhile)
            return
        changed = False
        for fields in rows:
            try:
                i, r, values = int(fields[0]), int(fields[1]), [int(v, 16) for v in fields[2:]]
            except (ValueError, IndexError):
                continue
            if i >= len(self.live) or not self.live[i].clut:
                continue
            t, w = self.live[i], self.live[i].clut[2]
            if r < t.rows and t.colors[r * w:r * w + len(values)] != values:
                t.colors[r * w:r * w + len(values)] = values
                self.vram_colors(t, r)
                changed = True
        if changed:
            self.show_tim()
            self.draw_vram()

    def flush(self):
        """Sends the changed CLUT rows to the console."""
        self.send_job = None
        if self.live is None:
            self.pending.clear()
            return
        for i, r in sorted(self.pending):
            t = self.live[i]
            self.send("clut %d %d %s" % (i, r, " ".join("%04X" % v for v in t.row(r))))
        self.pending.clear()

    # ---------------------------------------------------------------- display

    def select(self, i, send=True):
        """Selects a TIM here (send: and on the console)."""
        self.selected = max(0, min(i, len(self.tims) - 1))
        t = self.current()
        if t:
            self.choice.current(self.selected)
            self.row_spin.configure(to=max(0, t.rows - 1))
            if self.row_var.get() >= max(1, t.rows):
                self.row_var.set(0)
        else:
            self.choice.set("")
        self.entry = 0
        self.show_tim()
        self.draw_vram()
        if send:
            self.send_selection()

    def row_changed(self):
        t = self.current()
        try:
            r = int(self.row_var.get())
        except (ValueError, tk.TclError):
            r = 0
        self.row_var.set(max(0, min(r, t.rows - 1)) if t and t.rows else 0)
        self.show_tim()
        self.send_selection()

    def show_tim(self):
        t = self.current()
        if not t:
            self.info.configure(text="Add TIM files to the list.")
            self.preview.show(None)
            self.palette.delete("all")
            return
        x, y, w, h = t.img
        text = "%s, %d x %d pixels, image at %d,%d" % (MODES[t.mode], t.width, h, x, y)
        if t.clut:
            text += ", CLUT %d x %d at %d,%d" % (t.clut[2], t.clut[3], t.clut[0], t.clut[1])
        if t.edited():
            text += "  (edited, not saved)"
        self.info.configure(text=text)
        self.preview.show(t.image(self.row_var.get()))
        self.draw_palette()
        self.show_entry()

    def draw_palette(self):
        c = self.palette
        c.delete("all")
        t = self.current()
        if not t or not t.clut:
            c.configure(height=SWATCH)
            c.create_text(4, SWATCH // 2, text="No palette (%s TIM)" % MODES[t.mode] if t else "", fill="#a0a0a0",
                          anchor="w")
            return
        row = t.row(self.row_var.get())
        lines = (len(row) + 15) // 16
        self.swatch_size = SWATCH if lines <= 4 else 12
        size = self.swatch_size
        c.configure(width=16 * size + 2, height=lines * size + 2)
        for n, v in enumerate(row):
            x0, y0 = 1 + (n % 16) * size, 1 + (n // 16) * size
            c.create_rectangle(x0, y0, x0 + size - 2, y0 + size - 2, fill=rgb_hex(v), outline="#404040")
            if v == 0:      # Transparent (not drawn)
                c.create_line(x0, y0, x0 + size - 2, y0 + size - 2, fill="#ff4040")
            if v & 0x8000 and size >= SWATCH:  # Semi-transparent
                c.create_text(x0 + size - 4, y0 + 1, text="s", fill="#ffff00", anchor="ne", font=("", 6))
        x0, y0 = 1 + (self.entry % 16) * size, 1 + (self.entry // 16) * size
        c.create_rectangle(x0 - 1, y0 - 1, x0 + size - 1, y0 + size - 1, outline="#ffffff", width=2)

    def show_entry(self):
        t = self.current()
        self.updating = True
        if t and t.clut:
            v = t.row(self.row_var.get())[min(self.entry, t.clut[2] - 1)]
            for i, var in enumerate(self.scales):
                var.set((v >> (i * 5)) & 31)
                self.scale_values[i].configure(text=str((v >> (i * 5)) & 31))
            self.stp.set(bool(v & 0x8000))
            self.swatch.configure(background=rgb_hex(v))
            self.entry_label.configure(text="Color %d of row %d: %04X%s" % (
                self.entry, self.row_var.get(), v, "  (0000: transparent)" if v == 0 else ""))
        else:
            self.entry_label.configure(text="")
            self.swatch.configure(background="#000000")
        self.updating = False

    def palette_click(self, event):
        t = self.current()
        if not t or not t.clut:
            return
        size = self.swatch_size
        n = (event.y - 1) // size * 16 + (event.x - 1) // size
        if 0 <= n < t.clut[2] and (event.x - 1) // size < 16:
            self.entry = n
            self.draw_palette()
            self.show_entry()

    # ---------------------------------------------------------------- editing

    def set_color(self, value, rows=None):
        """New color of the edited entry (or new rows): shown here, sent to the console."""
        t = self.current()
        if not t or not t.clut:
            return
        w = t.clut[2]
        if rows is None:
            r = self.row_var.get()
            t.colors[r * w + self.entry] = value
            rows = [r]
        for r in rows:
            self.vram_colors(t, r)
            if self.live is not None and t in self.live:
                self.pending.add((self.live.index(t), r))
        self.last_edit = time.time()
        if self.pending and not self.send_job:
            self.send_job = self.after(SEND_DELAY, self.flush)
        self.show_tim()
        self.draw_vram()

    def vram_colors(self, t, r):
        """The CLUT row in the VRAM shown (the console has it there too)."""
        if self.vram is None:
            return
        x, y, w, h = t.clut
        if y + r < VRAM_H and x < VRAM_W:
            n = min(w, VRAM_W - x)
            dst = ((y + r) * VRAM_W + x) * 2
            self.vram[dst:dst + n * 2] = struct.pack("<%dH" % n, *t.row(r)[:n])

    def slider(self, i):
        if self.updating:
            return
        v = 0
        for n, var in enumerate(self.scales):
            v |= (int(float(var.get())) & 31) << (n * 5)
            self.scale_values[n].configure(text=str(int(float(var.get()))))
        self.set_color(v | (0x8000 if self.stp.get() else 0))

    def stp_changed(self):
        self.slider(0)

    def restore_entry(self):
        t = self.current()
        if t and t.clut:
            r = self.row_var.get()
            self.set_color(t.original[r * t.clut[2] + self.entry])

    def restore_row(self):
        t = self.current()
        if t and t.clut:
            r, w = self.row_var.get(), t.clut[2]
            t.colors[r * w:(r + 1) * w] = t.original[r * w:(r + 1) * w]
            self.set_color(None, rows=[r])

    def restore_all(self):
        t = self.current()
        if t and t.clut:
            changed = [r for r in range(t.rows) if t.row(r) != t.original[r * t.clut[2]:(r + 1) * t.clut[2]]]
            t.colors = list(t.original)
            self.set_color(None, rows=changed)

    def save_copy(self):
        t = self.current()
        if t:
            path = os.path.splitext(t.path)[0] + "_edit.tim"
            try:
                t.save(path)
            except OSError as e:
                return self.app.error("Can't write %s: %s" % (path, e))
            self.status.configure(text="Saved %s" % os.path.basename(path))

    def save(self):
        t = self.current()
        if t and self.app.confirm("Overwrite %s with the edited palette?" % t.name):
            try:
                t.save(t.path)
            except OSError as e:
                return self.app.error("Can't write %s: %s" % (t.path, e))
            self.status.configure(text="Saved %s" % t.name)
            self.show_tim()

    # ---------------------------------------------------------------- VRAM view

    def vram_scale(self):
        cw, ch = max(self.vram_canvas.winfo_width(), 64), max(self.vram_canvas.winfo_height(), 32)
        return min(cw / VRAM_W, ch / VRAM_H)

    def draw_vram(self):
        from PIL import Image, ImageTk
        c = self.vram_canvas
        c.delete("all")
        if self.vram is None:
            return
        s = self.vram_scale()
        img = Image.frombytes("RGB", (VRAM_W, VRAM_H), bytes(self.vram), "raw", "RGB;15")
        img = img.resize((max(1, int(VRAM_W * s)), max(1, int(VRAM_H * s))),
                         Image.NEAREST if s >= 1 else Image.BILINEAR)
        self.vram_photo = ImageTk.PhotoImage(img)
        c.create_image(0, 0, image=self.vram_photo, anchor="nw")
        for x in range(64, VRAM_W, 64):     # Texture pages (64 x 256)
            c.create_line(x * s, 0, x * s, VRAM_H * s, fill="#303030")
        c.create_line(0, 256 * s, VRAM_W * s, 256 * s, fill="#303030")
        for i, (x, y, w, h) in enumerate(self.areas):    # The console viewer's screens and font (TIMs can't go there)
            c.create_rectangle(x * s, y * s, (x + w) * s, (y + h) * s, outline="#909090", dash=(1, 3))
            c.create_text(x * s + 3, (y + h) * s - 2, text="viewer font" if i == 2 else "viewer screen",
                          fill="#909090", anchor="sw", font=("", 7))
        for i, t in enumerate(self.tims):
            color, width = OUTLINES[i % len(OUTLINES)], (3 if i == self.selected else 1)
            # (a dark border under each outline: visible on any VRAM content)
            x, y, w, h = t.img
            c.create_rectangle(x * s, y * s, (x + w) * s, (y + h) * s, outline="#000000", width=width + 2)
            c.create_rectangle(x * s, y * s, (x + w) * s, (y + h) * s, outline=color, width=width)
            if t.clut:
                x, y, w, h = t.clut
                c.create_rectangle(x * s, y * s, (x + w) * s, (y + max(h, 1)) * s, outline="#000000", width=width + 2)
                c.create_rectangle(x * s, y * s, (x + w) * s, (y + max(h, 1)) * s, outline=color, width=width,
                                   dash=(3, 2))
            if i == self.selected:
                c.create_text(t.img[0] * s + 3, t.img[1] * s + 2, text=t.name, fill=color, anchor="nw",
                              font=("", 8, "bold"))
        self.vram_title.configure(text="VRAM (read from the console)" if self.vram_live else
                                  "VRAM (made from the TIM files)")

    def vram_at(self, event):
        s = self.vram_scale()
        return int(event.x / s), int(event.y / s)

    # ---------------------------------------------------------------- moving a TIM in VRAM

    def hit(self, x, y):
        """(TIM index, "img" / "clut") at a VRAM position, the selected TIM first, None if none."""
        order = [self.selected] + [i for i in reversed(range(len(self.tims))) if i != self.selected]
        grab = int(3 / self.vram_scale())     # A CLUT is often 1 line high: 3 screen pixels around it count
        for i in order:
            if 0 <= i < len(self.tims):
                t = self.tims[i]
                for kind, rect in (("clut", t.clut), ("img", t.img)):
                    margin = grab if kind == "clut" else 0
                    if rect and rect[0] <= x < rect[0] + rect[2] and \
                            rect[1] - margin <= y < rect[1] + max(rect[3], 1) + margin:
                        return i, kind
        return None

    def vram_click(self, event):
        """Left click: selects the TIM (its CLUT: also the row), shown on the console."""
        x, y = self.vram_at(event)
        found = self.hit(x, y)
        if not found:
            return
        i, kind = found
        t = self.tims[i]
        if kind == "clut":
            self.row_var.set(max(0, min(y - t.clut[1], t.rows - 1)))
        self.select(i)

    def vram_press(self, event):
        """Right button: a drag moves the image or CLUT under it (the selection, here and on the console, stays)."""
        x, y = self.vram_at(event)
        found = self.hit(x, y)
        self.drag = None
        if not found:
            return
        i, kind = found
        t = self.tims[i]
        rect = t.clut if kind == "clut" else t.img
        self.drag = {"tim": t, "kind": kind, "start": (x, y), "from": rect[:2], "to": rect[:2], "ghost": None}

    def vram_drag(self, event):
        d = self.drag
        if not d:
            return
        x, y = self.vram_at(event)
        t, kind = d["tim"], d["kind"]
        w, h = (t.clut if kind == "clut" else t.img)[2:]
        nx = d["from"][0] + x - d["start"][0]
        ny = d["from"][1] + y - d["start"][1]
        if kind == "clut":      # CLUT x: a multiple of 16 (the GPU's CLUT attribute)
            nx = int(round(nx / 16.0)) * 16
        nx = max(0, min(nx, VRAM_W - w))
        ny = max(0, min(ny, VRAM_H - max(h, 1)))
        d["to"] = (nx, ny)
        problem = self.move_problem(t, kind, nx, ny)
        s = self.vram_scale()
        coords = (nx * s, ny * s, (nx + w) * s, (ny + max(h, 1)) * s)
        if d["ghost"] is None:
            d["ghost"] = self.vram_canvas.create_rectangle(*coords, width=2)
        self.vram_canvas.coords(d["ghost"], *coords)
        self.vram_canvas.itemconfigure(d["ghost"], outline="#ff3030" if problem else "#ffffff",
                                       dash=(3, 2) if kind == "clut" else ())
        self.vram_info.configure(text="%s %s to %d,%d%s" % (t.name, "CLUT" if kind == "clut" else "image", nx, ny,
                                                           ": " + problem if problem else ""))

    def vram_release(self, event):
        d, self.drag = self.drag, None
        if not d or d["to"] == d["from"]:
            if d and d["ghost"] is not None:
                self.draw_vram()
            return
        t, kind, (x, y) = d["tim"], d["kind"], d["to"]
        problem = self.move_problem(t, kind, x, y)
        if problem:
            self.status.configure(text="Can't move %s there: %s." % (t.name, problem))
        else:
            self.move_tim(t, kind, x, y)
        self.draw_vram()

    def move_problem(self, t, kind, x, y):
        """Why the image / CLUT of a TIM can't go at x, y (None: it can): outside VRAM, over another TIM, over the
        console viewer's screens or font."""
        w, h = (t.clut if kind == "clut" else t.img)[2:]
        rect = (x, y, w, max(h, 1))
        if x < 0 or y < 0 or x + w > VRAM_W or y + max(h, 1) > VRAM_H:
            return "outside VRAM"

        def overlap(a, b):
            return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + max(b[3], 1) and b[1] < a[1] + a[3]
        for o in self.tims:
            for okind, orect in (("img", o.img), ("clut", o.clut)):
                if orect and not (o is t and okind == kind) and overlap(rect, orect):
                    return "over %s%s" % ("its own " if o is t else o.name + " ",
                                          "CLUT" if okind == "clut" else "image")
        for i, area in enumerate(self.areas):
            if overlap(rect, area):
                return "over the console viewer's %s" % ("font" if i == 2 else "screen")
        return None

    def move_tim(self, t, kind, x, y):
        """Moves the image or CLUT: in the VRAM shown here, on the console (live), saved with the TIM."""
        old = t.clut if kind == "clut" else t.img
        t.move(kind, x, y)
        if self.vram is not None:      # (the console clears the old area too)
            self.blit(self.vram, old[0], old[1], old[2], max(old[3], 1), bytes(old[2] * max(old[3], 1) * 2), 0)
            if t.clut:
                self.blit(self.vram, t.clut[0], t.clut[1], t.clut[2], t.clut[3], t.bytes_with_colors(),
                          t.clut_offset)
            x0, y0, w, h = t.img
            self.blit(self.vram, x0, y0, w, h, t.data, t.img_offset)
        self.send_move(t)
        text = "Moved %s %s to %d,%d (Save writes it in the TIM; a TMD's texture coordinates don't follow)." % (
            t.name, "CLUT" if kind == "clut" else "image", x, y)
        x0, y0, w, h = t.img
        if t.mode < 2 and (x0 // 64 != (x0 + w - 1) // 64 or y0 // 256 != (y0 + h - 1) // 256):
            text += " Warning: the image crosses a texture page."
        self.status.configure(text=text)
        self.show_tim()

    def send_move(self, t):
        if self.live is not None and t in self.live:
            c = t.clut[:2] if t.clut else (-1, -1)
            self.send("move %d %d %d %d %d" % ((self.live.index(t),) + t.img[:2] + c))

    def restore_position(self):
        t = self.current()
        if not t or not t.moved():
            return
        (ix, iy), clut = t.original_pos
        for kind, pos in (("img", (ix, iy)), ("clut", clut)):
            rect = t.clut if kind == "clut" else t.img
            if pos and rect[:2] != pos:
                problem = self.move_problem(t, kind, *pos)
                if problem:
                    return self.status.configure(text="Can't move %s back: %s." % (t.name, problem))
                self.move_tim(t, kind, *pos)
        self.draw_vram()

    def vram_motion(self, event):
        x, y = self.vram_at(event)
        if 0 <= x < VRAM_W and 0 <= y < VRAM_H and self.vram is not None:
            v = struct.unpack_from("<H", self.vram, (y * VRAM_W + x) * 2)[0]
            self.vram_info.configure(text="x %d, y %d (texture page %d, %d): %04X" % (x, y, x // 64, y // 256, v))
