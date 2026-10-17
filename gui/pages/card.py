"""
PS1 DEV CART GUI: Memory card page (backup/restore with the PS1, save browser with the save icons and tools).

The save browser works on a card image file (the image tools don't use the cartridge, they are run at once, also
while a console runs) or on the card in slot 1 or 2 of the PS1 ("slot1"/"slot2" in place of the image file: the
card tool runs in server mode and only reads/writes the sectors needed; writes are confirmed on the PS1).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import re
import shutil
import tkinter as tk
from tkinter import filedialog

import ttkbootstrap as tb

from pages.base import Page
from settings import config_dir
from widgets import FileField, labeled

CARD_TYPES = [("Memory card images", "*.mcd *.mcr *.gme *.bin"), ("All files", "*.*")]
SAVE_TYPES = [("Single saves", "*.mcs"), ("All files", "*.*")]
SOURCES = ["Image file", "Card in slot 1", "Card in slot 2"]

# "  block  1: BENETYA-0001MYGAME   1 block(s)  "NET YAROZE SAVE""
SAVE_LINE = re.compile(r'^\s*block\s+(\d+):\s+(\S+)\s+(\d+) block\(s\)\s*(?:"(.*)")?')
ICON_LINE = re.compile(r"^\s*icon:\s*(.+\.png)\s*$")
SUMMARY_LINE = re.compile(r"^\d+ save\(s\), \d+/15 blocks used")


class CardPage(Page):
    title = "Memory card"

    def build(self):
        s = self.app.settings
        tb.Label(self, text="Memory card", font=("", 16, "bold")).pack(anchor="w", pady=(0, 8))

        box = self.section("Whole card")
        box.pack(fill="x", pady=(0, 8))
        row = tb.Frame(box)
        row.pack(fill="x", pady=(0, 4))
        f, self.slot = labeled(row, "Slot", tb.Combobox, values=("1", "2"), state="readonly", field_width=4)
        self.slot.set(s.field("card.slot", "1"))
        f.pack(side="left")
        tb.Label(row, text="Backup: about 12 s. Restore: about 30 s, confirm on the PS1 with the pad.",
                 bootstyle="secondary").pack(side="left", padx=12)
        self.backup_file = FileField(box, self.app, "card.backup", "Backup to", mode="save", filetypes=CARD_TYPES)
        self.backup_file.pack(fill="x", pady=2)
        self.restore_file = FileField(box, self.app, "card.restore", "Restore from", filetypes=CARD_TYPES)
        self.restore_file.pack(fill="x", pady=2)
        row = tb.Frame(box)
        row.pack(fill="x", pady=(6, 0))
        tb.Button(row, text="Back up the card", bootstyle="primary", command=self.backup).pack(side="left")
        tb.Button(row, text="Restore the card...", bootstyle="warning", command=self.restore).pack(side="left", padx=6)
        self.track(box, "backup")

        box = self.section("Saves")
        box.pack(fill="both", expand=True)
        row = tb.Frame(box)
        row.pack(fill="x")
        self.source = tb.Combobox(row, values=SOURCES, state="readonly", width=15)
        self.source.set(s.field("card.source", SOURCES[0]))
        self.source.bind("<<ComboboxSelected>>", lambda e: self._source_changed())
        self.source.pack(side="left", padx=(0, 6))
        self.image = FileField(row, self.app, "card.image", "Image", filetypes=CARD_TYPES)
        self.image.pack(side="left", fill="x", expand=True)
        self.open_button = tb.Button(row, text="Open", bootstyle="primary", command=self.open_saves)
        self.open_button.pack(side="left", padx=(6, 0))
        tb.Button(row, text="New blank image...", bootstyle="secondary-outline", command=self.new_image).pack(
            side="left", padx=(6, 0))
        self.hint = tb.Label(box, text="", bootstyle="secondary")
        self.hint.pack(anchor="w")

        self.app.style.configure("Card.Treeview", rowheight=36)
        row = tb.Frame(box)
        row.pack(fill="both", expand=True, pady=6)
        self.table = tb.Treeview(row, columns=("block", "name", "title", "size"), show="tree headings", height=8,
                                 style="Card.Treeview", selectmode="browse")
        self.table.heading("#0", text="Icon")
        self.table.column("#0", width=60, stretch=False, anchor="center")
        for col, text, width in (("block", "Block", 60), ("name", "File name", 220), ("title", "Title", 320),
                                 ("size", "Blocks", 70)):
            self.table.heading(col, text=text, anchor="w")
            self.table.column(col, width=width, anchor="w")
        self.table.pack(side="left", fill="both", expand=True)
        buttons = tb.Frame(row)
        buttons.pack(side="left", fill="y", padx=(6, 0))
        for text, command in (("Export...", self.export), ("Import...", self.import_save),
                              ("Copy to...", self.copy_to), ("Delete", self.delete)):
            b = tb.Button(buttons, text=text, width=10, bootstyle="secondary-outline", command=command)
            b.pack(pady=1)
            if text == "Copy to...":
                self.copy_button = b
        self.summary = tb.Label(box, text="", bootstyle="secondary")
        self.summary.pack(anchor="w")
        self.icons = []
        self.icon_dir = os.path.join(config_dir(), "card_icons")
        self.action = "backup"
        self._source_changed(load=False)
        if not self._live() and self.image.get() and os.path.isfile(self.image.get()):
            self.after(500, self.open_saves)

    # ---------------------------------------------------------------- whole card

    def args(self):
        if self.action == "saves":
            return ["-mclist", self._target(), "-icons", self.icon_dir] if self._live() else None
        if self.action == "restore":
            return ["-mcrestore", self.restore_file.get(), "-slot", self.slot.get()] if self.restore_file.get() else None
        return ["-mcbackup", self.backup_file.get(), "-slot", self.slot.get()] if self.backup_file.get() else None

    def backup(self):
        self.action = "backup"
        if not self.backup_file.get():
            return self.app.error("Choose the image file to write.")
        self.backup_file.remember()
        self.app.settings.set_field("card.slot", self.slot.get())
        self.app.run(self.args(), "Card backup", long_running=True, console=True,
                     cwd=os.path.dirname(self.backup_file.get()) or None, on_done=self._backup_done)

    def _backup_done(self, code, output):
        if code == 0 and os.path.isfile(self.backup_file.get()):
            self.source.set(SOURCES[0])
            self._source_changed(load=False)
            self.image.set(self.backup_file.get())
            self.open_saves()

    def restore(self):
        self.action = "restore"
        if not os.path.isfile(self.restore_file.get()):
            return self.app.error("Choose the image file to write to the card.")
        if not self.app.confirm("Write %s to the memory card in slot %s? Everything on the card is replaced "
                                "(you confirm again on the PS1)." % (os.path.basename(self.restore_file.get()),
                                                                     self.slot.get())):
            return
        self.restore_file.remember()
        self.app.settings.set_field("card.slot", self.slot.get())
        self.app.run(self.args(), "Card restore", long_running=True, console=True)

    # ---------------------------------------------------------------- saves (image file or card slot)

    def _live(self):
        return self.source.get() != SOURCES[0]

    def _target(self):
        """ps1transfer name of what is browsed: the image file, or slot1/slot2."""
        return "slot%s" % self.source.get()[-1] if self._live() else self.image.get()

    def _describe(self):
        return "the card in slot %s" % self.source.get()[-1] if self._live() else os.path.basename(self.image.get())

    def _source_changed(self, load=True):
        self.app.settings.set_field("card.source", self.source.get())
        live = self._live()
        for child in self.image.winfo_children():
            try:
                child.configure(state="disabled" if live else "normal")
            except tk.TclError:
                pass
        self.open_button.configure(text="Read the card" if live else "Open")
        self.hint.configure(text="Only the directory and the saves' title/icon sectors are read; a save written or "
                                 "deleted is confirmed on the PS1 with the pad." if live else "")
        self.table.delete(*self.table.get_children())
        self.summary.configure(text="")
        if load and live:
            self.open_saves()

    def _run(self, args, name, on_done=None):
        """Image: at once (no cartridge). Card slot: with the cartridge (the card tool runs on the PS1)."""
        if self._live() or any(a in ("slot1", "slot2") for a in args):
            self.app.run(args, name, on_done=lambda code, out: on_done(code, out) if on_done else None)
        else:
            code, lines = self.app.run_offline(args, name)
            if on_done:
                on_done(code, lines)

    def _image_ok(self):
        if self._live():
            return True
        if not os.path.isfile(self.image.get()):
            self.app.error("Open a memory card image first.")
            return False
        return True

    def _selected_block(self):
        sel = self.table.selection()
        if not sel:
            self.app.error("Select a save in the table.")
            return None
        return self.table.item(sel[0], "values")

    def open_saves(self):
        if not self._image_ok():
            return
        self.action = "saves"
        if not self._live():
            self.image.remember()
        shutil.rmtree(self.icon_dir, ignore_errors=True)
        os.makedirs(self.icon_dir, exist_ok=True)
        if self._live():
            self.summary.configure(text="Reading the card in slot %s..." % self.source.get()[-1])
        self._run(["-mclist", self._target(), "-icons", self.icon_dir], "Card saves", on_done=self._show_saves)

    def _show_saves(self, code, lines):
        from PIL import Image, ImageTk
        self.table.delete(*self.table.get_children())
        self.icons = []
        item = None
        summary = ""
        for line in lines:
            m = SAVE_LINE.match(line)
            if m:
                item = self.table.insert("", "end", values=(m.group(1), m.group(2), m.group(4) or "", m.group(3)))
                continue
            m = ICON_LINE.match(line)
            if m and item and os.path.isfile(m.group(1)):
                with Image.open(m.group(1)) as im:
                    icon = ImageTk.PhotoImage(im.convert("RGBA").resize((32, 32), Image.NEAREST))
                self.icons.append(icon)
                self.table.item(item, image=icon)
            if SUMMARY_LINE.match(line.strip()):
                summary = line.strip()
        if code == 0:
            self.summary.configure(text=summary)
        else:
            errors = [l for l in lines if l.strip() and not l.startswith(("Resetting", "Starting", "Uploading"))]
            self.summary.configure(text=errors[-1] if errors else "Can't read the saves.")

    def _refresh(self, code, lines):
        if code == 0:
            self.open_saves()

    def new_image(self):
        path = filedialog.asksaveasfilename(defaultextension=".mcd", filetypes=CARD_TYPES, parent=self)
        if path:
            if self.app.run_offline(["-mcformat", path], "New card image")[0] == 0:
                self.source.set(SOURCES[0])
                self._source_changed(load=False)
                self.image.set(os.path.normpath(path))
                self.open_saves()

    def export(self):
        save = self._selected_block()
        if not self._image_ok() or not save:
            return
        out = filedialog.asksaveasfilename(defaultextension=".mcs", initialfile=save[1] + ".mcs",
                                           filetypes=SAVE_TYPES, parent=self)
        if out:
            self._run(["-mcexport", self._target(), save[0], os.path.normpath(out)], "Export save")

    def import_save(self):
        if not self._image_ok():
            return
        mcs = filedialog.askopenfilename(filetypes=SAVE_TYPES, parent=self)
        if not mcs:
            return
        if self._live() and not self.app.confirm("Write %s to %s? Only its blocks and the directory are written "
                                                 "(you confirm again on the PS1)." % (os.path.basename(mcs),
                                                                                      self._describe())):
            return
        self._run(["-mcimport", self._target(), os.path.normpath(mcs)], "Import save", on_done=self._refresh)

    def copy_to(self):
        save = self._selected_block()
        if not self._image_ok() or not save:
            return
        menu = tk.Menu(self, tearoff=0)
        menu.add_command(label="Card image file...", command=lambda: self._copy(save, None))
        for slot in ("1", "2"):
            if self._target() != "slot" + slot:
                menu.add_command(label="Card in slot %s" % slot, command=lambda s=slot: self._copy(save, "slot" + s))
        x, y = self.copy_button.winfo_rootx(), self.copy_button.winfo_rooty() + self.copy_button.winfo_height()
        menu.tk_popup(x, y)

    def _copy(self, save, dst):
        if not dst:
            dst = filedialog.askopenfilename(title="Card image to copy the save to", filetypes=CARD_TYPES, parent=self)
            if not dst:
                return
            dst = os.path.normpath(dst)
        elif not self.app.confirm("Copy %s to the card in slot %s? (you confirm on the PS1)" % (save[1], dst[-1])):
            return
        self._run(["-mccopy", self._target(), save[0], dst], "Copy save")

    def delete(self):
        save = self._selected_block()
        if not self._image_ok() or not save:
            return
        if self._live():
            question = "Delete %s from the card in slot %s? (you confirm again on the PS1)" % (save[1],
                                                                                              self.source.get()[-1])
        else:
            question = "Delete %s from %s? (a .bak copy of the image is kept)" % (save[1], self._describe())
        if self.app.confirm(question):
            self._run(["-mcdelete", self._target(), save[0]], "Delete save", on_done=self._refresh)
