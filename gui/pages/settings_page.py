"""
PS1 DEV CART GUI: Settings page (ps1transfer path, theme; the PS1 menu's settings saved in the cartridge:
screensavers, the PicoStation menu's game details; the cartridge's name).

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
# The PS1 menu's screensavers (ps1transfer -saver MENU PICO, saved in the cartridge; -info: saver_menu, saver_pico):
# "off", or the minutes and ",logo" (the bouncing PS1 DEV CART logo) or ",blank" (the display off)
SAVERS = (("menu", "PS1 DEV CART menu"), ("pico", "PicoStation menu"))
SAVER_TIMES = (("3 minutes", "3"), ("5 minutes", "5"), ("10 minutes", "10"))
SAVER_STYLES = (("Screen off", "blank"), ("Bouncing logo", "logo"))
FEATURE_SCREENSAVER = 0x40
# Where ps1transfer -rename saves the EEPROM backups (rename.c)
BACKUP_DIR = os.path.join(os.environ.get("LOCALAPPDATA", "."), "PS1-DEV-CART", "eeprom-backups")


LABEL_W = 21       # The label column of every section (the fields line up)
WRAP = 760


class SettingsPage(Page):
    title = "Settings"

    def build(self):
        tb.Label(self, text="Settings", font=("", 16, "bold")).pack(anchor="w", pady=(0, 8))
        # (scrolls: the sections are taller than a small window)
        body = self.body = tb.ScrolledFrame(self, autohide=True)
        body.pack(fill="both", expand=True)
        self._build_manager(body)
        self._build_menu(body)
        self._build_rename(body)
        box = self.section("About", body)
        box.pack(fill="x", pady=(0, 4), padx=(0, 12))
        tb.Label(box, text="PS1 Dev Cart Manager %s." % self.app.version, wraplength=WRAP, justify="left").pack(anchor="w")

    def _note(self, box, text, row, pady=(0, 8)):
        """A section's explanation, over its fields."""
        tb.Label(box, text=text, wraplength=WRAP, justify="left").grid(row=row, column=0, columnspan=5, sticky="w",
                                                                       pady=pady)

    def _label(self, box, text, row, **kw):
        tb.Label(box, text=text, width=LABEL_W, anchor="w", **kw).grid(row=row, column=0, sticky="w", pady=3)

    # ---------------------------------------------------------------- the Manager (saved on this computer)

    def _build_manager(self, parent):
        box = self.section("Manager", parent)
        box.pack(fill="x", pady=(0, 12), padx=(0, 12))
        box.columnconfigure(4, weight=1)
        self.exe = FileField(box, self.app, "settings.ps1transfer", "ps1transfer",
                             filetypes=[("Programs", "*.exe"), ("All files", "*.*")], label_width=LABEL_W)
        self.exe.set(self.app.settings.get("ps1transfer") or "")
        self.exe.grid(row=0, column=0, columnspan=5, sticky="ew", pady=3)
        tb.Label(box, text="Empty: %s" % default_ps1transfer(), bootstyle="secondary").grid(
            row=1, column=1, columnspan=4, sticky="w")
        self._label(box, "Theme", 2)
        self.theme = tb.Combobox(box, values=self.app.style.theme_names(), state="readonly", width=30)
        self.theme.set(self.app.settings.get("theme"))
        self.theme.grid(row=2, column=1, columnspan=3, sticky="w", pady=(8, 3))
        self.theme.bind("<<ComboboxSelected>>", lambda e: self.app.set_theme(self.theme.get()))
        row = tb.Frame(box)
        row.grid(row=3, column=1, columnspan=4, sticky="w", pady=(8, 0))
        tb.Button(row, text="Save", bootstyle="primary", command=self.save).pack(side="left")
        tb.Label(row, text="Settings file: %s" % self.app.settings.path, bootstyle="secondary").pack(side="left", padx=8)

    def save(self):
        self.app.settings.set("ps1transfer", self.exe.get())
        self.exe.remember()
        self.app.settings.save()
        self.app.refresh_devices()

    # ---------------------------------------------------------------- the PS1 menu (saved in the cartridge)

    def _build_menu(self, parent):
        box = self.section("PS1 menu (saved in the cartridge)", parent)
        box.pack(fill="x", pady=(0, 12), padx=(0, 12))
        box.columnconfigure(4, weight=1)    # (the explanations' width there: the fields line up with the other sections)
        self._note(box, "Screensavers: what the PS1 shows when no button is pressed for a while on the cartridge's "
                        "menu or its PicoStation menu, the screen turned off or the PS1 DEV CART logo bouncing around "
                        "a black screen (any button puts the menu back).", 0)
        self.saver_rows = {}
        for i, (key, label) in enumerate(SAVERS):
            self._label(box, label, 1 + i)
            on = tb.BooleanVar(value=True)
            tb.Checkbutton(box, text="On", variable=on, bootstyle="round-toggle",
                           command=self._saver_states).grid(row=1 + i, column=1, sticky="w", padx=(0, 12))
            time = tb.Combobox(box, values=[t[0] for t in SAVER_TIMES], state="readonly", width=12)
            time.set(SAVER_TIMES[0][0])
            time.grid(row=1 + i, column=2, sticky="w", padx=(0, 8), pady=3)
            style = tb.Combobox(box, values=[t[0] for t in SAVER_STYLES], state="readonly", width=16)
            style.set(SAVER_STYLES[0][0])
            style.grid(row=1 + i, column=3, sticky="w", pady=3)
            self.saver_rows[key] = (on, time, style)
        self._note(box, "Game details: the PicoStation menu's list shows the selected game's discs and cheats under "
                        "its cover, read from the SD card after a second on the game (one or two seconds each). "
                        "Turned off, only its cover, game ID and region are shown: faster scrolling.", 3, pady=(12, 6))
        self._label(box, "Game details", 4)
        self.pico_details = tb.BooleanVar(value=True)
        tb.Checkbutton(box, text="Show the discs and cheats of the selected game", variable=self.pico_details,
                       bootstyle="round-toggle").grid(row=4, column=1, columnspan=4, sticky="w")
        row = tb.Frame(box)
        row.grid(row=5, column=1, columnspan=4, sticky="w", pady=(12, 0))
        tb.Button(row, text="Save to the cartridge", bootstyle="primary", command=self.save_menu).pack(side="left")
        self.menu_text = tb.Label(row, text="", bootstyle="secondary")
        self.menu_text.pack(side="left", padx=8)
        tb.Label(box, text="The cartridge's menu must be running. Flashing the cartridge sets them back to: "
                           "screensavers on, 3 minutes, screen off; game details shown.",
                 bootstyle="secondary", wraplength=WRAP, justify="left").grid(row=6, column=0, columnspan=5,
                                                                              sticky="w", pady=(8, 0))

    def save_menu(self):
        if not self.app.serial():
            return self.app.error("No cartridge selected.")
        args = ["-saver", self._saver_spec("menu"), self._saver_spec("pico"),
                "-picodetails", "on" if self.pico_details.get() else "off"]
        self.app.run(args, "PS1 menu settings", on_done=lambda code, out: self._saved(code, out, self.menu_text))

    # ---------------------------------------------------------------- the cartridge's name

    def _build_rename(self, parent):
        box = self.section("Cartridge name", parent)
        box.pack(fill="x", pady=(0, 12), padx=(0, 12))
        box.columnconfigure(4, weight=1)
        self._note(box, "The name is the cartridge's USB serial number (what -n and the list at the top use, and what "
                        "the PS1 menu shows). Only the cartridge's USB chip is changed: its settings are saved to a "
                        "backup first, and checked after the change (put back if anything differs).", 0)
        self._label(box, "New name", 1)
        self.new_name = tb.Entry(box, width=20)
        self.new_name.grid(row=1, column=1, sticky="w", pady=3)
        self.new_name.bind("<KeyRelease>", lambda e: self._check_rename())
        tb.Label(box, text="1 to 15 letters A-Z and digits 0-9", bootstyle="secondary").grid(
            row=1, column=2, columnspan=3, sticky="w", padx=8)
        self._label(box, "Confirm", 2)
        self.rename_confirm = tb.Entry(box, width=20)
        self.rename_confirm.grid(row=2, column=1, sticky="w", pady=3)
        self.rename_confirm.bind("<KeyRelease>", lambda e: self._check_rename())
        self.rename_confirm_label = tb.Label(box, text="", bootstyle="secondary")
        self.rename_confirm_label.grid(row=2, column=2, columnspan=3, sticky="w", padx=8)
        self.rename_text = tb.Label(box, text="", bootstyle="secondary")
        self.rename_text.grid(row=3, column=1, columnspan=4, sticky="w")
        self.rename_button = tb.Button(box, text="Rename the cartridge", bootstyle="danger", command=self.rename,
                                       state="disabled")
        self.rename_button.grid(row=4, column=1, sticky="w", pady=(6, 0))
        self._label(box, "Backups", 5)
        row = tb.Frame(box)
        row.grid(row=5, column=1, columnspan=4, sticky="w", pady=(12, 0))
        tb.Button(row, text="Put back a backup...", bootstyle="secondary", command=self.restore).pack(side="left")
        tb.Label(row, text=BACKUP_DIR, bootstyle="secondary").pack(side="left", padx=8)
        self._check_rename()

    def on_show(self):
        self._check_rename()
        self._read_savers()

    def on_device_changed(self):
        # (the confirmation typed was for the other cartridge)
        self.rename_confirm.delete(0, "end")
        self._check_rename()
        self._read_savers()

    def _saver_states(self):
        for on, time, style in self.saver_rows.values():
            state = "readonly" if on.get() else "disabled"
            time.configure(state=state)
            style.configure(state=state)

    def _saver_spec(self, key):
        on, time, style = self.saver_rows[key]
        if not on.get():
            return "off"
        minutes = dict(SAVER_TIMES)[time.get()]
        return "%s,%s" % (minutes, dict(SAVER_STYLES)[style.get()])

    def _set_saver(self, key, spec):
        on, time, style = self.saver_rows[key]
        on.set(spec != "off")
        if spec != "off":
            minutes, _, kind = spec.partition(",")
            time.set(next((t[0] for t in SAVER_TIMES if t[1] == minutes), SAVER_TIMES[0][0]))
            style.set(next((t[0] for t in SAVER_STYLES if t[1] == (kind or "blank")), SAVER_STYLES[0][0]))

    def _read_savers(self):
        """The settings saved in the cartridge (-info, at its menu)."""
        if not self.app.serial():
            self.menu_text.configure(text="No cartridge selected.", bootstyle="secondary")
            return
        self.app.run(["-info"], "PS1 menu settings", quiet=True,
                     on_done=lambda code, out: self._savers_read(parse_info("\n".join(out))))

    def _savers_read(self, info):
        if info.get("saver_menu") and info.get("saver_pico"):
            for key, _ in SAVERS:
                self._set_saver(key, info["saver_" + key])
            self._saver_states()
            self.menu_text.configure(text="As saved in the cartridge.", bootstyle="secondary")
            if info.get("pico_details"):
                self.pico_details.set(info["pico_details"] != "off")
        elif info.get("state") not in (None, "menu", "unknown"):
            self.menu_text.configure(text="Read when the cartridge's menu runs (a program is running now).",
                                      bootstyle="secondary")
        elif info.get("firmware") == "devcart" and not int(info.get("features", "0"), 16) & FEATURE_SCREENSAVER:
            self.menu_text.configure(text="The cartridge's firmware is older: flash a newer one for these settings.",
                                      bootstyle="warning")
        else:
            self.menu_text.configure(text="", bootstyle="secondary")

    def _saved(self, code, output, label):
        if code == 0 and any(l.strip() == "saver=ok" for l in output):
            label.configure(text="Saved in the cartridge.", bootstyle="success")
        else:
            last = [l for l in output if l.strip() and "=" not in l][-2:]
            label.configure(text=" ".join(last) or "Not saved.", bootstyle="danger")

    def on_devices_listed(self):
        self._check_rename()

    def _check_rename(self):
        serial = self.app.serial()
        name = self.new_name.get().strip()
        self.rename_confirm_label.configure(text="Type the current name, %s, to confirm" % serial if serial else
                                            "No cartridge selected")
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
