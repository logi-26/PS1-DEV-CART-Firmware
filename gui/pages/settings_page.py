"""
PS1 DEV CART GUI: Settings page (ps1transfer path, theme, the cartridge's name).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import re
from tkinter import filedialog

import ttkbootstrap as tb
from ttkbootstrap.dialogs import Messagebox

from devices import default_ps1transfer, parse_info
from pages.base import Page
from settings import config_dir
from widgets import FileField

NAME_RE = re.compile(r"^[A-Za-z0-9]{1,15}$")
# Where ps1transfer -rename saves the EEPROM backups (rename.c)
BACKUP_DIR = os.path.join(os.environ.get("LOCALAPPDATA", "."), "PS1-DEV-CART", "eeprom-backups")


class SettingsPage(Page):
    title = "Settings"

    def build(self):
        tb.Label(self, text="Settings", font=("", 16, "bold")).pack(anchor="w", pady=(0, 8))

        box = self.section("ps1transfer")
        box.pack(fill="x", pady=(0, 8))
        self.exe = FileField(box, self.app, "settings.ps1transfer", "Program",
                             filetypes=[("Programs", "*.exe"), ("All files", "*.*")])
        self.exe.set(self.app.settings.get("ps1transfer") or "")
        self.exe.pack(fill="x", pady=2)
        tb.Label(box, text="Empty: %s" % default_ps1transfer(), bootstyle="secondary").pack(anchor="w")

        box = self.section("Appearance")
        box.pack(fill="x", pady=(0, 8))
        row = tb.Frame(box)
        row.pack(fill="x")
        tb.Label(row, text="Theme", width=14, anchor="w").pack(side="left")
        self.theme = tb.Combobox(row, values=self.app.style.theme_names(), state="readonly", width=30)
        self.theme.set(self.app.settings.get("theme"))
        self.theme.pack(side="left")
        self.theme.bind("<<ComboboxSelected>>", lambda e: self.app.set_theme(self.theme.get()))

        tb.Button(self, text="Save", bootstyle="primary", command=self.save).pack(anchor="w", pady=(4, 0))
        tb.Label(self, text="Settings file: %s" % self.app.settings.path, bootstyle="secondary").pack(
            anchor="w", pady=(16, 0))

        self._build_rename()

        box = self.section("About")
        box.pack(fill="x", pady=(16, 0))
        tb.Label(box, text="PS1 Dev Cart Manager %s." % self.app.version, wraplength=760, justify="left").pack(anchor="w")

    def save(self):
        self.app.settings.set("ps1transfer", self.exe.get())
        self.exe.remember()
        self.app.settings.save()
        self.app.refresh_devices()

    # ---------------------------------------------------------------- the cartridge's name

    def _build_rename(self):
        box = self.section("Cartridge name")
        box.pack(fill="x", pady=(16, 0))
        tb.Label(box, text="The name is the cartridge's USB serial number (what -n and the list at the top use, and "
                           "what the PS1 menu shows). Only the cartridge's USB chip is changed: its settings are "
                           "saved to a backup first, and checked after the change (put back if anything differs).",
                 wraplength=760, justify="left").pack(anchor="w", pady=(0, 6))
        row = tb.Frame(box)
        row.pack(fill="x", pady=2)
        tb.Label(row, text="New name", width=14, anchor="w").pack(side="left")
        self.new_name = tb.Entry(row, width=18)
        self.new_name.pack(side="left")
        self.new_name.bind("<KeyRelease>", lambda e: self._check_rename())
        tb.Label(row, text="1 to 15 letters A-Z and digits 0-9", bootstyle="secondary").pack(side="left", padx=8)
        row = tb.Frame(box)
        row.pack(fill="x", pady=2)
        self.rename_confirm_label = tb.Label(row, text="", width=24, anchor="w")
        self.rename_confirm_label.pack(side="left")
        self.rename_confirm = tb.Entry(row, width=18)
        self.rename_confirm.pack(side="left")
        self.rename_confirm.bind("<KeyRelease>", lambda e: self._check_rename())
        self.rename_button = tb.Button(row, text="Rename the cartridge", bootstyle="danger", command=self.rename,
                                       state="disabled")
        self.rename_button.pack(side="left", padx=6)
        self.rename_text = tb.Label(box, text="", bootstyle="secondary")
        self.rename_text.pack(anchor="w", pady=(2, 0))
        row = tb.Frame(box)
        row.pack(fill="x", pady=(8, 0))
        tb.Button(row, text="Put back a backup...", bootstyle="secondary", command=self.restore).pack(side="left")
        tb.Label(row, text="Backups: %s" % BACKUP_DIR, bootstyle="secondary").pack(side="left", padx=8)
        self._check_rename()

    def on_show(self):
        self._check_rename()

    def on_device_changed(self):
        # (the confirmation typed was for the other cartridge)
        self.rename_confirm.delete(0, "end")
        self._check_rename()

    def on_devices_listed(self):
        self._check_rename()

    def _check_rename(self):
        serial = self.app.serial()
        name = self.new_name.get().strip()
        self.rename_confirm_label.configure(text="Type %s to confirm" % serial if serial else "No cartridge selected")
        if not name:
            text = ""
        elif not NAME_RE.match(name):
            text = "The name has 1 to 15 letters A-Z and digits 0-9 only."
        elif name == serial:
            text = "The cartridge is already named %s." % name
        elif any(d["serial"].lower() == name.lower() for d in getattr(self.app, "devices", [])
                 if d["serial"] != serial):
            text = "Another cartridge connected is already named %s." % name
        else:
            text = ""
        self.rename_text.configure(text=text, bootstyle="danger" if text else "secondary")
        ok = serial and name and not text and self.rename_confirm.get().strip() == serial
        self.rename_button.configure(state="normal" if ok else "disabled")

    def args(self):
        name = self.new_name.get().strip()
        return ["-rename", name] if name else None

    def rename(self):
        serial, name = self.app.serial(), self.new_name.get().strip()
        if not self.app.confirm("Rename the cartridge %s to %s?\n\nIts USB chip's settings are saved to a backup "
                                "first. The cartridge reconnects with its new name." % (serial, name)):
            return
        self.rename_confirm.delete(0, "end")
        self._check_rename()
        self.app.run(["-rename", name], "Rename the cartridge", on_done=lambda c, o: self._renamed(c, o, serial))

    def restore(self):
        serial = self.app.serial()
        if not serial:
            return self.app.error("No cartridge selected.")
        path = filedialog.askopenfilename(parent=self, title="EEPROM backup to put back",
                                          initialdir=BACKUP_DIR if os.path.isdir(BACKUP_DIR) else None,
                                          filetypes=[("EEPROM backups", "*.txt"), ("All files", "*.*")])
        if not path:
            return
        if not self.app.confirm("Put the backup %s back on the cartridge %s?\n\nIts USB chip gets the settings "
                                "and the name saved there." % (os.path.basename(path), serial)):
            return
        self.app.run(["-renamerestore", os.path.normpath(path)], "Put back an EEPROM backup",
                     on_done=lambda c, o: self._renamed(c, o, serial))

    def _renamed(self, code, output, old):
        info = parse_info("\n".join(output))
        name = info.get("serial")
        if code == 0 and info.get("rename") == "ok" and name:
            # The new name is the one chosen at the top of the window
            self.app.settings.set("device", name)
            self.app.settings.save()
            self.new_name.delete(0, "end")
            text = "The cartridge %s is now named %s (checked)." % (old, name)
            if info.get("backup"):
                text += "\n\nBackup of its previous settings:\n%s" % info["backup"]
            Messagebox.show_info(text, "PS1 Dev Cart Manager", parent=self)
        else:
            last = [l for l in output if l.strip() and "=" not in l][-3:]
            Messagebox.show_error("The cartridge wasn't renamed:\n\n%s" % "\n".join(last), "PS1 Dev Cart Manager",
                                  parent=self)
        self.app.refresh_devices()
        self._check_rename()
