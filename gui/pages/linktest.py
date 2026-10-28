"""
PS1 DEV CART GUI: Link test page (link cable tester started on two consoles, one cartridge each).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import ttkbootstrap as tb
from ttkbootstrap.dialogs import Messagebox

from pages.base import Page
from widgets import labeled


MODE_PSYQ = "Serial (psxcomb)"				# The tester with psxcomb (libcomb compatible) on the cable (-link)
MODE_YAROZE = "Serial (yaroze: tty)"
MODE_USB = "USB (psxcomb)"					# psxcomb's USB link rings (-link usb)
MODE_YAROZE_USB = "USB (yaroze: tty)"		# Net Yaroze tty: driver on the USB link (-link yarozeusb)
MODES = [MODE_PSYQ, MODE_YAROZE, MODE_USB, MODE_YAROZE_USB]
OLD_MODES = {"Serial (PsyQ: libcomb)": MODE_PSYQ, "Serial (Yaroze: tty)": MODE_YAROZE, "USB Link": MODE_USB,
             "USB Link (Yaroze: tty)": MODE_YAROZE_USB}		# (names saved by older versions)
RELAY_MODES = (MODE_USB, MODE_YAROZE_USB)
RELAY_TASK = "USB link relay"				# The relay's task (also started by Run Linked of the Run pages)


class LinkTestPage(Page):
    title = "Link Connection Test"

    def build(self):
        tb.Label(self, text="Link cable tester", font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="Two consoles linked by a serial cable or through their cartridges and this computer",
                 wraplength=780, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 10))
        box = self.section("Consoles")
        box.pack(fill="x")
        f, self.first = labeled(box, "Console 1", tb.Combobox, state="readonly", width=20)
        f.pack(anchor="w", pady=2)
        f, self.second = labeled(box, "Console 2", tb.Combobox, state="readonly", width=20)
        f.pack(anchor="w", pady=2)
        f, self.mode = labeled(box, "Link", tb.Combobox, state="readonly", width=20,
                               values=MODES)
        self.mode.configure(width=26)
        mode = self.app.settings.field("link.mode", MODE_PSYQ)
        mode = OLD_MODES.get(mode, mode)
        self.mode.set(mode if mode in MODES else MODE_PSYQ)
        self.mode.bind("<<ComboboxSelected>>", lambda e: self.app.settings.set_field("link.mode", self.mode.get()))
        f.pack(anchor="w", pady=2)
        tb.Button(box, text="Start on both", bootstyle="primary", command=self.start).pack(anchor="w", pady=(8, 0))
        tb.Label(self, text="Each console is reset first. Run it again on one console to restart its tester.",
                 bootstyle="secondary").pack(anchor="w", pady=(8, 0))

        box = self.section("USB link for your programs")
        box.pack(fill="x", pady=(12, 0))
        tb.Label(box, text="Run your program on both consoles then start the relay", wraplength=760, justify="left",
                 bootstyle="secondary").pack(anchor="w")
        row = tb.Frame(box)
        row.pack(anchor="w", pady=(8, 0))
        tb.Button(row, text="Start the relay", bootstyle="primary", command=self.relay).pack(side="left")
        tb.Button(row, text="Stop the relay", bootstyle="danger-outline", command=self.stop_relay).pack(
            side="left", padx=(8, 0))

    def on_show(self):
        serials = [d["serial"] for d in self.app.devices]
        for combo, i in ((self.first, 0), (self.second, 1)):
            combo.configure(values=serials)
            if combo.get() not in serials:
                combo.set(serials[i] if i < len(serials) else "")

    def args(self):
        return {MODE_YAROZE: ["-link", "yaroze"], MODE_USB: ["-link", "usb"],
                MODE_YAROZE_USB: ["-link", "yarozeusb"]}.get(self.mode.get(), ["-link"])

    def _pair(self):
        a, b = self.first.get(), self.second.get()
        if not a or not b or a == b:
            self.app.error("Choose two different cartridges (connect both to this computer, then Refresh).")
            return None
        return a, b

    def relay(self):
        """USB link relay between the two consoles (their programs already run), until it is stopped."""
        pair = self._pair()
        if pair:
            self.app.run(["-relay", pair[1]], RELAY_TASK, device=pair[0], long_running=True, stdin=True)

    def stop_relay(self):
        """Stops the relay (started here or by Run Linked): asked to quit, it ends between two USB transfers."""
        task = self.app.runner.task
        if task and task.name == RELAY_TASK:
            self.app.stop_task()
        else:
            Messagebox.show_info("The USB link relay isn't running.", self.title, parent=self.app)

    def start(self):
        pair = self._pair()
        if not pair:
            return
        a, b = pair
        args = self.args()

        def second(code, out):
            if code == 0:
                self.app.run(args, "Link test " + b, device=b, on_done=relay if self.mode.get() in RELAY_MODES else None)

        def relay(code, out):
            # USB link: the relay copies the data between the two consoles until it is stopped
            if code == 0:
                self.app.run(["-relay", b], RELAY_TASK, device=a, long_running=True, stdin=True)

        self.app.run(args, "Link test " + a, device=a, on_done=second)
