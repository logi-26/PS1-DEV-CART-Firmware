"""
PS1 DEV CART GUI: Hot reload page (files sent to the running program: TIM to VRAM, sounds to SPU RAM, data to RAM).

On the console (a part of the page): the target of the selected file picked from what the running program has,
instead of typed. A TIM: the
textures it draws (read from its GPU packets in RAM and its VRAM, see assetscan.textures_in_use), the ones of the same
size and colors first; its position and its CLUT's ("x,y,cx,cy": the TIM's CLUT sent where the texture's is). A sound:
the samples in SPU RAM (spu.py's map, the voices playing them), the ones it fits in first.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import time
from tkinter import filedialog

import ttkbootstrap as tb

import assetscan
import spu as spumod
from pages.base import Page
from settings import config_dir
from widgets import ImageView, labeled

MAX_FILES = 16
HOT_TYPES = [("Hot reload files", "*.tim *.vag *.vb *.bin"), ("All files", "*.*")]


def describe(path, target):
    """Where the file goes, for the table."""
    ext = os.path.splitext(path)[1].lower()
    if ext == ".tim":
        parts = target.split(",") if target else []
        if len(parts) == 4:
            return "VRAM at %s,%s, its CLUT at %s,%s" % tuple(parts)
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
    needs_devcart = "all"

    def build(self):
        tb.Label(self, text=self.title, font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="Send textures, sounds and data to the running program without restarting it. "
                            "With Watch, a file is sent again each time it is "
                            "saved, and the printf of the program is shown.",
                 wraplength=780, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 8))

        row = tb.Frame(self)
        row.pack(fill="x")
        # Columns: file name, target, where it goes, full path (hidden, shown as a tooltip-like status below)
        self.table = tb.Treeview(row, columns=("file", "target", "where", "path"), displaycolumns=("file", "target",
                                 "where"), show="headings", height=6, selectmode="browse")
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
        tb.Label(edit, text="TIM: empty (its own position), x,y, or x,y,cx,cy (its CLUT too)   Sound: SPU address "
                 "(e.g. 0x1010)   Data: RAM address (0x80...)", bootstyle="secondary").pack(side="left", padx=8)

        self.console = ConsolePanel(self)
        bar = tb.Frame(self)
        bar.pack(side="bottom", fill="x", pady=(6, 0))
        self.console.pack(fill="both", expand=True, pady=(4, 0))
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
            self.console.show_for(values[3])

    def set_target(self, target=None):
        sel = self.table.selection()
        if not sel:
            return self.app.error("Select a file in the table.")
        path = self.table.item(sel[0], "values")[3]
        if target is None:
            target = self.target.get().strip()
        else:
            self.target.delete(0, "end")
            self.target.insert(0, target)
        self.table.item(sel[0], values=(os.path.basename(path), target, describe(path, target), path))
        self._save()
        return None

    # ---------------------------------------------------------------- On the console

    def console_parts(self, path):
        """What the console is read for this file: "ram,vram" (a TIM: textures), "spu" (a sound), None (data)."""
        ext = os.path.splitext(path)[1].lower()
        return "ram,vram" if ext == ".tim" else "spu" if ext in (".vag", ".vb") else None

    def cached(self, parts):
        return getattr(self, "console_cache", {}).get(parts, (0, None))[1]

    def read_console(self, path, parts):
        folder = os.path.join(config_dir(), "hotreload", parts.replace(",", "_"))
        os.makedirs(folder, exist_ok=True)
        for name in ("ram.bin", "vram.bin", "spu.bin"):
            if os.path.exists(os.path.join(folder, name)):
                os.remove(os.path.join(folder, name))
        self.console.busy("Reading the %s of the running program (a few seconds)..." % (
            "RAM and VRAM" if parts != "spu" else "SPU RAM"))

        def done(code, output):
            self.after(0, self._console_read, path, parts, folder, output)
        self.app.run(["-snapshot", folder, parts], "Read the console", on_done=done)

    def _console_read(self, path, parts, folder, output):
        try:
            snap = assetscan.Snapshot(folder)
        except (OSError, ValueError):
            why = [l for l in output if "No program" in l or "error" in l.lower() or "too old" in l]
            snap = None
            self.console.busy(why[-1] if why else "The console couldn't be read: start your program first.")
            return
        if parts == "spu" and not snap.spu:
            self.console.busy("The SPU RAM couldn't be read (the firmware's debug monitor version 10 is needed).")
            return
        self.console_cache = getattr(self, "console_cache", {})
        self.console_cache[parts] = (time.time(), snap)
        self.console.show_for(path)

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


class ConsolePanel(tb.Labelframe):
    """What the running program has, for the file selected in the table: its textures (a TIM) or its SPU RAM samples
    (a sound). Read the console fills it, Use (or a double click) sets the file's target."""

    def __init__(self, page):
        super().__init__(page, text="On the console", padding=8)
        self.page, self.path, self.parts, self.items = page, None, None, []
        self.is_tim = True
        row = tb.Frame(self)
        row.pack(fill="x")
        self.read_button = tb.Button(row, text="Read the console", bootstyle="info", command=self.read)
        self.read_button.pack(side="left")
        self.use_button = tb.Button(row, text="Use for the selected file", bootstyle="primary", command=self.use)
        self.use_button.pack(side="left", padx=6)
        self.note = tb.Label(row, text="", bootstyle="warning", wraplength=620, justify="left")
        self.note.pack(side="left", padx=10)
        self.info = tb.Label(self, text="Select a TIM or a sound in the table, then Read the console: the textures the "
                             "running program draws, or the samples in its SPU RAM, are listed here to choose from.",
                             bootstyle="secondary", wraplength=1000, justify="left")
        self.info.pack(anchor="w", pady=(6, 4))
        body = tb.Frame(self)
        body.pack(fill="both", expand=True)
        self.list = tb.Treeview(body, columns=("match", "what", "where", "used"), show="headings", selectmode="browse",
                                height=8)
        for c, w in (("match", 40), ("what", 330), ("where", 170), ("used", 200)):
            self.list.column(c, width=w, stretch=(c == "what"))
        sb = tb.Scrollbar(body, orient="vertical", command=self.list.yview)
        self.list.configure(yscrollcommand=sb.set)
        self.list.pack(side="left", fill="both", expand=True)
        sb.pack(side="left", fill="y")
        self.list.bind("<<TreeviewSelect>>", lambda e: self.selected())
        self.list.bind("<Double-1>", lambda e: self.use())
        self.preview = ImageView(body, width=240)
        self.preview.pack(side="left", fill="both", padx=(8, 0))
        self.headings()

    def headings(self):
        for c, text in (("match", ""), ("what", "Texture" if self.is_tim else "Sample"),
                        ("where", "VRAM / CLUT" if self.is_tim else "SPU RAM"), ("used", "Used by")):
            self.list.heading(c, text=text, anchor="w")

    def clear(self, text):
        self.items = []
        self.list.delete(*self.list.get_children())
        self.preview.show(None)
        self.note.configure(text="")
        self.info.configure(text=text)

    def busy(self, text):
        self.clear(text)

    def show_for(self, path):
        """The list for this file: from the console read last (for its kind), or how to read it."""
        self.path, self.parts = path, self.page.console_parts(path)
        if not self.parts:
            return self.clear("%s: data goes to a RAM address of your program (type it above)." %
                              os.path.basename(path))
        self.is_tim = self.parts != "spu"
        self.headings()
        if not os.path.isfile(path):
            return self.clear("File not found: %s" % path)
        snap = self.page.cached(self.parts)
        if not snap:
            return self.clear("%s: Read the console to list the %s." % (
                os.path.basename(path), "textures the running program draws" if self.is_tim else
                "samples in its SPU RAM"))
        return self.refill(snap)

    def read(self):
        if not self.path:
            sel = self.page.table.selection()
            if not sel:
                return self.page.app.error("Select a TIM or a sound in the table first.")
            self.show_for(self.page.table.item(sel[0], "values")[3])
        if not self.parts:
            return self.page.app.error("Data files go to a RAM address of your program: type it.")
        return self.page.read_console(self.path, self.parts)

    # The file

    def file_tim(self):
        """(bits, width in pixels, height, has a CLUT) of the TIM file, None if it isn't one."""
        with open(self.path, "rb") as f:
            d = f.read()
        t = assetscan.tim_at(d, 0)
        if not t:
            return None
        _, mode, img, clut = t
        bits = (4, 8, 16, 24)[mode]
        return bits, img[3] * {4: 4, 8: 2, 16: 1, 24: 2 / 3}[bits], img[4], clut is not None

    def file_size(self):
        """The bytes the sound puts in SPU RAM (a VAG: without its 48-byte header)."""
        with open(self.path, "rb") as f:
            d = f.read()
        return len(d) - 48 if d[:4] == b"VAGp" and len(d) > 48 else len(d)

    def refill(self, snap):
        self.clear("")
        if self.is_tim:
            ft = self.file_tim()
            if not ft:
                self.info.configure(text="%s isn't a TIM file." % os.path.basename(self.path))
                return
            fbits, fw, fh, fclut = ft
            if not snap.ram or not snap.vram:
                self.info.configure(text="The RAM and VRAM couldn't be read.")
                return
            for a in assetscan.textures_in_use(snap.ram, snap.vram):
                _, mode, img, clut = assetscan.tim_at(a.data, 0)
                bits = (4, 8, 16, 16)[mode]
                w, h = img[3] * {4: 4, 8: 2, 16: 1}[bits], img[4]
                match = 2 if (bits == fbits and (w, h) == (fw, fh)) else 1 if (bits == fbits and fw <= w and fh <= h) \
                    else 0
                target = "%d,%d" % (img[1], img[2])
                if clut and fclut:
                    target += ",%d,%d" % (clut[1], clut[2])
                self.items.append((match, a, target, "%dx%d %d-bit" % (w, h, bits),
                                   ("%d,%d / %d,%d" % (img[1], img[2], clut[1], clut[2])) if clut else
                                   "%d,%d" % (img[1], img[2]), a.info.split("drawn by ")[-1], (w, h, bits)))
            what = "%s: %dx%d %d-bit%s" % (os.path.basename(self.path), fw, fh, fbits, " with a CLUT" if fclut else "")
            self.info.configure(text="%s. The textures the program draws now (from its drawing commands): the ones "
                                "of the same size and colors first (=), then the ones it fits in (<). Your TIM goes "
                                "where the one chosen is, its CLUT where that texture's CLUT is." % what)
        else:
            size = self.file_size()
            if not snap.spu:
                self.info.configure(text="The SPU RAM couldn't be read.")
                return
            s = spumod.Spu(snap.spu)
            for r in s.regions:
                if r.kind != "sample" or (r.size < 256 and not r.voices):
                    continue
                match = 2 if r.size == size else 1 if size <= r.size else 0
                users = ("voice" + ("s " if len(r.voices) > 1 else " ") + ", ".join(str(v) for v in r.voices)) \
                    if r.voices else "no voice now"
                self.items.append((match, r, "0x%X" % r.start, "sample %d: %d bytes (%.2f s at 22050 Hz)%s" % (
                    r.index, r.size, r.size / 16 * 28 / 22050, ", loops" if r.loops else ""),
                    "0x%05X-0x%05X" % (r.start, r.end), users, r.size))
            self.info.configure(text="%s: %d bytes of sound data. The samples in the program's SPU RAM: the ones of "
                                "the same size first (=), then the ones it fits in (<). A bigger file overwrites what "
                                "comes after the sample." % (os.path.basename(self.path), size))
        self.items.sort(key=lambda it: (-it[0], -(it[6][0] * it[6][1] if self.is_tim else it[6])))
        for i, it in enumerate(self.items):
            self.list.insert("", "end", iid=str(i), values=({2: "=", 1: "<", 0: ""}[it[0]], it[3], it[4], it[5]))
        if not self.items:
            self.info.configure(text=self.info.cget("text") + "  Nothing found: is your program running (and drawing, "
                                "or with its sounds loaded)? Read the console again.")

    def current(self):
        sel = self.list.selection()
        return self.items[int(sel[0])] if sel else None

    def selected(self):
        it = self.current()
        if not it:
            return
        if self.is_tim:
            self.preview.show(assetscan.tim_image(it[1].data))
            ft = self.file_tim()
            w, h, bits = it[6]
            self.note.configure(text="" if it[0] else "Your TIM (%dx%d %d-bit) isn't this texture's size or colors "
                                "(%dx%d %d-bit): it overwrites what is next to it, or shows wrongly." % (
                                    ft[1], ft[2], ft[0], w, h, bits))
        else:
            self.note.configure(text="" if it[0] else "Your sound is bigger than this sample (%d bytes, %d here): it "
                                "overwrites what comes after it." % (self.file_size(), it[6]))

    def use(self):
        it = self.current()
        if not it:
            return self.page.app.error("Choose a %s in the list." % ("texture" if self.is_tim else "sample"))
        self.page.set_target(it[2])
        self.page.path_label.configure(text="%s: %s" % (os.path.basename(self.path), describe(self.path, it[2])))
        return None
