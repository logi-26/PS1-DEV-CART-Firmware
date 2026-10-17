"""
PS1 DEV CART GUI: a window for ps1transfer (run programs, console, memory, screenshots, debugging, tools).

The GUI builds the ps1transfer command line of each action (shown at the bottom, it can be copied) and runs it,
so every feature of ps1transfer and the firmware is used the same way as from the command line.
Run: python ps1devcart_gui.py (needs: pip install -r requirements.txt)

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import subprocess
import sys

import ttkbootstrap as tb
from ttkbootstrap.dialogs import Messagebox

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from devices import default_ps1transfer, list_devices, parse_info, run_quick  # noqa: E402
from pages import PAGES  # noqa: E402
from runner import Runner  # noqa: E402
from settings import Settings  # noqa: E402
from widgets import MonoText  # noqa: E402

APP_VERSION = "1.0"     # PS1 DEV CART release (the same as the firmware, ps1transfer and the PS1 tools)
APP_TITLE = "PS1 Dev Cart Manager " + APP_VERSION

STATE_STYLES = {"menu": "info", "running": "success", "stopped": "warning", "crashed": "danger", "unknown": "secondary"}


class App(tb.Window):
    version = APP_VERSION

    def __init__(self):
        self.settings = Settings()
        try:
            super().__init__(title=APP_TITLE, themename=self.settings.get("theme") or "bootstrap-dark",
                             minsize=(980, 640))
        except Exception:  # noqa: BLE001 (theme of another ttkbootstrap version)
            super().__init__(title=APP_TITLE, themename="bootstrap-dark", minsize=(980, 640))
        if self.settings.get("geometry"):
            self.geometry(self.settings.get("geometry"))
        else:
            self.geometry("1180x800")
        self.protocol("WM_DELETE_WINDOW", self.on_close)

        self.runner = Runner(self, self._on_text, self._on_progress, self._on_task, self._on_pcdrv)
        self.devices = []
        self.pages = {}
        self.current = None
        self.task_stopped = False   # The running task was stopped by the user
        self.server_folder = None   # Folder served to the PS1 by the running task (PCDRV)

        self._build_top()
        self._build_body()
        self._build_bottom()
        self.show_page(self.settings.field("app.page", "Run"))
        self.after(200, self.refresh_devices)
        self.after(400, self._update_preview)

    # ---------------------------------------------------------------- layout

    def _build_top(self):
        top = tb.Frame(self, padding=(10, 8))
        top.pack(fill="x")
        tb.Label(top, text="Cartridge").pack(side="left")
        self.device = tb.Combobox(top, state="readonly", width=16)
        self.device.pack(side="left", padx=(6, 4))
        self.device.bind("<<ComboboxSelected>>", lambda e: self._device_changed())
        tb.Button(top, text="Refresh", bootstyle="secondary-outline", command=self.refresh_devices).pack(side="left")
        self.state = tb.Label(top, text="  -  ", bootstyle="inverse-secondary", padding=(8, 2))
        self.state.pack(side="left", padx=(12, 6))
        self.fw = tb.Label(top, text="", bootstyle="secondary")
        self.fw.pack(side="left")
        tb.Button(top, text="Reset the PS1", bootstyle="warning-outline", command=self.reset_ps1).pack(side="right")
        self.stop_button = tb.Button(top, text="Stop task", bootstyle="danger", command=self.stop_task,
                                     state="disabled")
        self.stop_button.pack(side="right", padx=6)
        self.task_label = tb.Label(top, text="", bootstyle="secondary")
        self.task_label.pack(side="right", padx=6)
        self.server_label = tb.Label(top, text="", bootstyle="info")     # File server folder while it runs
        self.server_label.pack(side="right", padx=6)
        tb.Separator(self).pack(fill="x")

    def _build_body(self):
        body = tb.Frame(self)
        body.pack(fill="both", expand=True)
        side = tb.Frame(body, padding=(8, 10))
        side.pack(side="left", fill="y")
        tb.Separator(body, orient="vertical").pack(side="left", fill="y")
        self.side_buttons = {}
        self.content = tb.Frame(body)
        self.content.pack(side="left", fill="both", expand=True)
        for cls in PAGES:
            page = cls(self.content, self)
            page.place(relx=0, rely=0, relwidth=1, relheight=1)
            self.pages[cls.title] = page
            text = cls.title if cls.phase == 1 else cls.title + " (soon)"
            b = tb.Button(side, text=text, width=19, bootstyle="secondary-link",
                          command=lambda t=cls.title: self.show_page(t))
            b.pack(fill="x", pady=1)
            self.side_buttons[cls.title] = b

    def _build_bottom(self):
        bottom = tb.Frame(self, padding=(10, 6))
        bottom.pack(fill="x")
        row = tb.Frame(bottom)
        row.pack(fill="x")
        tb.Label(row, text=">").pack(side="left")
        self.preview = tb.Entry(row, font=("Consolas", 10))
        self.preview.pack(side="left", fill="x", expand=True, padx=4)
        tb.Button(row, text="Copy", bootstyle="secondary-outline", command=self._copy_preview).pack(side="left")
        self.progress = tb.Progressbar(bottom, bootstyle="info", maximum=100)
        self.progress.pack(fill="x", pady=(6, 4))
        self.log = MonoText(bottom, height=6)
        self.log.pack(fill="x")

    # ---------------------------------------------------------------- pages

    def show_page(self, title):
        if title not in self.pages:
            title = "Run"
        self.current = self.pages[title]
        self.current.tkraise()
        self.current.on_show()
        for t, b in self.side_buttons.items():
            b.configure(bootstyle="primary" if t == title else "secondary-link")
        self.settings.set_field("app.page", title)

    def _update_preview(self):
        args = self.current.args() if self.current else None
        if args and self.settings.field("console.pcdrvany", False) and self._file_server_folder(args, None):
            args = args + ["-pcdrvany"]
        text =subprocess.list2cmdline(["ps1transfer"] + self._device_args() + args) if args else ""
        # (focus_get() fails while a combobox list is open: compare the Tk names)
        if self.preview.get() != text and str(self.tk.call("focus")) != str(self.preview):
            self.preview.delete(0, "end")
            self.preview.insert(0, text)
        self.after(400, self._update_preview)

    def _copy_preview(self):
        self.clipboard_clear()
        self.clipboard_append(self.preview.get())

    # ---------------------------------------------------------------- ps1transfer

    def exe(self):
        return self.settings.get("ps1transfer") or default_ps1transfer()

    def serial(self):
        return self.device.get().split(" ")[0] if self.device.get() else ""

    def _device_args(self, device=None):
        serial = device or self.serial()
        return ["-n", serial] if serial else []

    def run_offline(self, args, name):
        """Runs a ps1transfer command that doesn't use the cartridge (e.g. card image tools), at once.
        Returns (exit code, output lines)."""
        self.log.append("\n> %s\n" % subprocess.list2cmdline(["ps1transfer"] + args))
        code, out = run_quick(self.exe(), args)
        lines = [l for l in out.replace("\r", "").split("\n") if l and not l.startswith("PS1 DEV CART Transfer tool")]
        self.log.append("".join(l + "\n" for l in lines))
        self.log.append("(%s: %s)\n" % (name, "done" if code == 0 else "failed (exit code %s)" % code))
        return code, lines

    def run(self, args, name, long_running=False, console=False, quiet=False, cwd=None, on_line=None, on_done=None,
            device=None, stdin=False, log_filter=None):
        """Runs ps1transfer with the selected cartridge. A running task (e.g. the console) is stopped first, after
        a confirmation. stdin: the task takes commands (self.runner.send), log_filter: lines kept in the log."""
        def again():
            self.run(args, name, long_running, console, quiet, cwd, on_line, on_done, device, stdin, log_filter)

        if self.runner.busy:
            task = self.runner.task
            if task.quiet:      # Status query: run once it has finished
                self.after(200, again)
                return
            if quiet:
                return
            if not task.long_running:
                self.error("%s is still running, wait until it has finished." % task.name)
                return
            if Messagebox.yesno("%s is running. Stop it and continue?" % task.name, APP_TITLE, parent=self) not in (
                    "Yes", "yes", True):
                return
            self.runner.stop()
            self.after(300, again)
            return
        full = [self.exe()] + self._device_args(device) + args + ["-progress"]
        server = self._file_server_folder(args, cwd)
        if server:
            full.append("-pcdrvlog")
            if self.settings.field("console.pcdrvany", False):
                full.append("-pcdrvany")
            self.pages["Console"].files_started()
        self.server_folder = server     # Shown in the top bar while the task runs
        self.progress.configure(value=0)
        if not quiet:
            self.log.append("\n> %s\n" % subprocess.list2cmdline(["ps1transfer"] + self._device_args(device) + args))
            if console:
                self.pages["Console"].append("\n--- %s ---\n" % name)

        def done(code, output):
            if on_done:
                on_done(code, output)
            if not quiet:
                result = "stopped" if self.task_stopped else "done" if code == 0 else "failed (exit code %s)" % code
                self.log.append("(%s: %s)\n" % (name, result))
                self.after(300, self.refresh_status)

        self.task_stopped = False
        self.runner.start(full, name, long_running=long_running, quiet=quiet, cwd=cwd, on_line=on_line, on_done=done,
                          console=console, stdin=stdin, log_filter=log_filter)

    @staticmethod
    def _file_server_folder(args, cwd):
        """The folder served to the PS1 (PCDRV) if the task runs the console (-c, which also serves the files), or
        None. ps1transfer serves the -p folder, or its working directory."""
        if "-c" not in args:
            return None
        i = args.index("-c")
        if i + 1 < len(args) and args[i + 1] in ("off", "sio", "link"):
            return None
        if "-p" in args and args.index("-p") + 1 < len(args):
            folder = args[args.index("-p") + 1]
        else:
            folder = cwd or os.getcwd()
        return os.path.abspath(os.path.join(cwd or "", folder))

    def stop_task(self):
        task = self.runner.task
        if task and task.name == "Card restore" and not self.confirm(
                "Stop the card restore? The memory card would be left half written (restore it again after)."):
            return
        if self.runner.busy:
            self.task_stopped = True
            self.runner.stop()

    def reset_ps1(self):
        self.run(["-r"], "Reset")

    def _on_text(self, task, text):
        self.log.append(text)
        if task.console:
            self.pages["Console"].append(text)

    def _on_pcdrv(self, task, line):
        self.pages["Console"].files_event(line)

    def _on_progress(self, done, total):
        self.progress.configure(value=100.0 * done / total if total else 0)

    def _on_task(self, task):
        if task and not task.quiet:
            self.task_label.configure(text=task.name + "...")
            self.server_label.configure(text="File server: %s" % _short_path(self.server_folder)
                                        if self.server_folder else "")
            self.stop_button.configure(state="normal")
            if task.long_running:
                self._show_state({"state": "running"}, None, keep_firmware=True)
        elif not task:
            self.task_label.configure(text="")
            self.server_label.configure(text="")
            self.stop_button.configure(state="disabled")
            self.progress.configure(value=0)

    # ---------------------------------------------------------------- cartridges

    def refresh_devices(self):
        if self.runner.busy:
            return
        self.devices = list_devices(self.exe())
        values = [d["serial"] + ("  (in use)" if d["in_use"] else "") for d in self.devices]
        self.device.configure(values=values)
        wanted = self.settings.get("device")
        pick = next((v for v in values if v.split(" ")[0] == wanted), values[0] if values else "")
        changed = pick.split(" ")[0] != self.device.get().split(" ")[0]
        self.device.set(pick)
        if changed:
            self._notify_device()
        if not values:
            self._show_state({"state": "unknown"}, "No cartridge found")
            return
        self.refresh_status()

    def _device_changed(self):
        self.settings.set("device", self.serial())
        self._notify_device()
        self.refresh_status()

    def _notify_device(self):
        for page in self.pages.values():
            page.on_device_changed()

    def refresh_status(self):
        if not self.devices:
            return
        if self.runner.busy:
            if self.runner.task.long_running:
                self._show_state({"state": "running"}, "")
            return
        self.run(["-info"], "Status", quiet=True,
                 on_done=lambda code, out: self._show_state(parse_info("\n".join(out))))

    def _show_state(self, info, text=None, keep_firmware=False):
        state = info.get("state", "unknown")
        self.state.configure(text="  %s  " % (text or state.capitalize()),
                             bootstyle="inverse-" + STATE_STYLES.get(state, "secondary"))
        if keep_firmware:
            return
        fw = info.get("firmware")
        if fw == "devcart":
            self.fw.configure(text="Firmware %s" % info.get("firmware_version", "PS1 DEV CART"))
        elif fw:
            self.fw.configure(text="Original firmware")
        else:
            self.fw.configure(text="")

    # ---------------------------------------------------------------- misc

    def set_theme(self, theme):
        try:
            self.style.theme_use(theme)
            self.settings.set("theme", theme)
        except Exception as e:  # noqa: BLE001 (unknown theme name)
            self.error(str(e))

    def confirm(self, message):
        return Messagebox.yesno(message, APP_TITLE, parent=self) in ("Yes", "yes", True)

    def error(self, message):
        Messagebox.show_error(message, APP_TITLE, parent=self)

    def on_close(self):
        if self.runner.busy and not self.runner.task.quiet:
            if Messagebox.yesno("%s is running. Stop it and quit?" % self.runner.task.name, APP_TITLE,
                                parent=self) not in ("Yes", "yes", True):
                return
            self.runner.stop()
        self.settings.set("geometry", self.geometry())
        self.settings.save()
        self.destroy()


def _short_path(path, parts=2):
    """The last folders of a path: C:\\a\\b\\c\\d -> ...\\c\\d"""
    names = os.path.normpath(path).split(os.sep)
    return path if len(names) <= parts + 1 else os.path.join("...", *names[-parts:])


if __name__ == "__main__":
    App().mainloop()
