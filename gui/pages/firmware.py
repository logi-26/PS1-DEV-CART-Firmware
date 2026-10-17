"""
PS1 DEV CART GUI: Firmware page (cartridge information, flashing a ROM with a typed confirmation and a readback check).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import glob
import os
import sys

import ttkbootstrap as tb
from ttkbootstrap.dialogs import Messagebox

from devices import GUI_DIR, parse_info
from pages.base import Page
from widgets import FileField

ROM_SIZE = 128 * 1024
ROM_TYPES = [("Cartridge ROM", "*.rom *.bin"), ("All files", "*.*")]
MONITOR_OFFSET = 0x1C800        # Debug monitor in the ROM (monitor/monitor.h MONITOR_ROM_ADRS)

FEATURES = {0x01: "live memory/VRAM access", 0x02: "device ID", 0x04: "big packets while a program runs"}
INFO_ROWS = (("device", "Cartridge (USB serial)"), ("state", "State"), ("firmware", "Firmware"),
             ("firmware_version", "Firmware version"), ("features", "Features"), ("monitor", "Debug monitor"),
             ("monitor_version", "Monitor version"), ("escaped_answers", "Escaped answers"),
             ("debug_session", "Debug session"), ("tty_mode", "TTY mode"))


def default_rom():
    """The firmware ROM: next to the Manager's executable (release zip: ps1devcart-x.y.rom, the newest name first), or
    the repository's cartrom/ps1devcart.rom. "" if none."""
    places = [os.path.join(GUI_DIR, "..", "cartrom", "ps1devcart.rom")]
    if getattr(sys, "frozen", False):
        exe_dir = os.path.dirname(sys.executable)
        places = sorted(glob.glob(os.path.join(exe_dir, "ps1devcart*.rom")), reverse=True) +             [os.path.join(exe_dir, "..", "..", "cartrom", "ps1devcart.rom")]
    return next((os.path.normpath(p) for p in places if os.path.isfile(p)), "")


def describe_rom(path):
    """(valid, text) for a ROM file."""
    try:
        with open(path, "rb") as f:
            rom = f.read()
    except OSError as e:
        return False, "Can't read the file: %s" % e
    if len(rom) != ROM_SIZE or rom[0x84:0x84 + 44] != b"Licensed by Sony Computer Entertainment Inc.":
        return False, "Not a PS1 DEV CART ROM (128KB with the cartridge boot header)."
    parts = ["PS1 DEV CART ROM"]
    i = rom.find(b"Firmware: ")
    if i >= 0:
        version = rom[i + 10:i + 20].split(b"\0")[0].decode(errors="replace")
        parts.append("firmware %s" % version)
    else:
        parts.append("firmware without version (original or older)")
    if rom[MONITOR_OFFSET:MONITOR_OFFSET + 4] == b"DBG1":
        parts.append("debug monitor v%d" % rom[MONITOR_OFFSET + 8])
    else:
        parts.append("no debug monitor")
    return True, ", ".join(parts)


class FirmwarePage(Page):
    title = "Firmware"

    def build(self):
        tb.Label(self, text="Firmware", font=("", 16, "bold")).pack(anchor="w", pady=(0, 8))

        box = self.section("Cartridge")
        box.pack(fill="x", pady=(0, 8))
        self.info = tb.Treeview(box, columns=("key", "value"), show="", height=len(INFO_ROWS), selectmode="none")
        self.info.column("key", width=200, anchor="w")
        self.info.column("value", width=500, anchor="w")
        self.info.pack(fill="x")
        tb.Button(box, text="Read", bootstyle="primary", command=self.read_info).pack(anchor="w", pady=(6, 0))
        self.track(box, "info")

        box = self.section("Flash a ROM")
        box.pack(fill="x")
        tb.Label(box, text="Writes the whole cartridge flash, then reads it back to check it. Don't switch the PS1 "
                           "off or unplug the cartridge while it runs (about 20 s). If it goes wrong, the recovery "
                           "CD (ar_flash.bin/.cue) or the flasher run from the original firmware can flash it again.",
                 wraplength=780, justify="left", bootstyle="warning").pack(anchor="w", pady=(0, 6))
        self.rom = FileField(box, self.app, "fw.rom", "ROM file", filetypes=ROM_TYPES,
                             on_change=self._check)
        if not self.rom.get() or not os.path.isfile(self.rom.get()):
            default = default_rom()
            if default:
                self.rom.set(default)
        self.rom.pack(fill="x", pady=2)
        self.rom_text = tb.Label(box, text="", bootstyle="secondary")
        self.rom_text.pack(anchor="w", pady=(0, 6))
        row = tb.Frame(box)
        row.pack(fill="x")
        self.confirm_label = tb.Label(row, text="Type the cartridge serial number to confirm:")
        self.confirm_label.pack(side="left")
        self.confirm = tb.Entry(row, width=16)
        self.confirm.pack(side="left", padx=6)
        self.confirm.bind("<KeyRelease>", lambda e: self._check())
        self.flash_button = tb.Button(row, text="Flash the cartridge", bootstyle="danger", command=self.flash,
                                      state="disabled")
        self.flash_button.pack(side="left", padx=6)
        self.track(box, "flash")
        self.action = "info"
        self._check()

    def on_show(self):
        self._check()

    def on_device_changed(self):
        # The confirmation typed was for the other cartridge, and the information shown was its own
        self.confirm.delete(0, "end")
        self.info.delete(*self.info.get_children())
        self._check()

    def _check(self):
        if not hasattr(self, "flash_button"):	# Default ROM set while the page is built: checked at the end of build()
            return
        valid, text = describe_rom(self.rom.get()) if self.rom.get() else (False, "Choose the ROM file.")
        self.rom_text.configure(text=text, bootstyle="secondary" if valid else "danger")
        serial = self.app.serial()
        self.confirm_label.configure(text="Type %s to confirm:" % serial if serial else "No cartridge selected")
        ok = valid and serial and self.confirm.get().strip() == serial
        self.flash_button.configure(state="normal" if ok else "disabled")

    def args(self):
        if self.action == "flash":
            return ["-flash", self.rom.get()] if self.rom.get() else None
        return ["-info"]

    def read_info(self):
        self.action = "info"
        self.app.run(["-info"], "Cartridge information", on_done=self._show_info)

    def _show_info(self, code, output):
        info = parse_info("\n".join(output))
        self.info.delete(*self.info.get_children())
        for key, label in INFO_ROWS:
            value = info.get(key)
            if value is None:
                continue
            if key == "features":
                bits = int(value, 16)
                names = [name for bit, name in FEATURES.items() if bits & bit]
                value = "%s (%s)" % (value, ", ".join(names) if names else "none")
            elif key in ("monitor", "debug_session", "escaped_answers"):
                value = "yes" if value == "1" else "no"
            elif key == "tty_mode":
                value = {"0": "USB", "1": "BIOS", "2": "serial port", "3": "USB link"}.get(value, value)
            self.info.insert("", "end", values=(label, value))

    def flash(self):
        self.action = "flash"
        valid, text = describe_rom(self.rom.get())
        if not valid:
            return self.app.error(text)
        if not self.app.confirm("Flash %s to the cartridge %s?\n\n%s" % (os.path.basename(self.rom.get()),
                                                                       self.app.serial(), text)):
            return
        self.rom.remember()
        self.confirm.delete(0, "end")
        self._check()
        self.app.run(self.args(), "Flash", on_done=self._flashed)

    def _flashed(self, code, output):
        if code == 0:
            Messagebox.show_info("The cartridge was flashed and checked.", "PS1 Dev Cart Manager", parent=self)
        else:
            last = [l for l in output if l.strip()][-3:]
            Messagebox.show_error("Flashing failed:\n\n%s" % "\n".join(last), "PS1 Dev Cart Manager", parent=self)
        self.read_info()
