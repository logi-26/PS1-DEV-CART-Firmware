"""
PS1 DEV CART GUI: a window for ps1transfer (run programs, console, memory, screenshots, debugging, tools).

The GUI builds the ps1transfer command line of each action (shown at the bottom, it can be copied) and runs it,
so every feature of ps1transfer and the firmware is used the same way as from the command line.
Run: python ps1devcart_gui.py (needs: pip install -r requirements.txt)

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import ctypes
import os
import queue
import subprocess
import sys
import threading

import ttkbootstrap as tb
from ttkbootstrap.dialogs import Messagebox

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from devices import default_ps1transfer, list_devices, parse_info, run_quick  # noqa: E402
from livelink import FRAME_CAPTURE, LIVE_LINK, LIVE_PROTOCOL  # noqa: E402
from pages import PAGES  # noqa: E402
from pages.console import ConsolePage  # noqa: E402
from runner import Runner  # noqa: E402
from settings import Settings  # noqa: E402
from widgets import MonoText  # noqa: E402

APP_VERSION = "1.0"     # PS1 DEV CART release (the same as the firmware, ps1transfer and the PS1 tools)
APP_TITLE = "PS1 Dev Cart Manager " + APP_VERSION

def resource(*parts):
    """A file that comes with the GUI (icons/...): next to this script, or in the PyInstaller bundle."""
    return os.path.join(getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__))), *parts)


def title_bar_dark(window, dark):
    """Windows 10 (1809 and later) and 11: the title bar of a window dark or light (DWM's immersive dark mode), like
    the theme. Nothing elsewhere."""
    if sys.platform != "win32":
        return
    try:
        window.update_idletasks()
        hwnd = ctypes.windll.user32.GetParent(window.winfo_id())
        value = ctypes.c_int(1 if dark else 0)
        for attribute in (20, 19):      # DWMWA_USE_IMMERSIVE_DARK_MODE (19 before Windows 10 20H1)
            if not ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, attribute, ctypes.byref(value),
                                                              ctypes.sizeof(value)):
                break
        # Drawn again at once (Windows 10 keeps a shown window's title bar until it is activated): WM_NCACTIVATE,
        # inactive then as it is
        active = ctypes.windll.user32.GetForegroundWindow() == hwnd
        ctypes.windll.user32.SendMessageW(hwnd, 0x86, not active, 0)
        ctypes.windll.user32.SendMessageW(hwnd, 0x86, active, 0)
    except Exception:  # noqa: BLE001 (older Windows: the title bar stays as it is)
        pass


STATE_STYLES = {"menu": "info", "running": "success", "stopped": "warning", "crashed": "danger", "unknown": "secondary"}


# Pages renamed: their earlier names (the last page shown is saved by its name)
OLD_PAGE_NAMES = {"Run Yaroze": "Run Net Yaroze", "Console": "Console & File Server", "Memory": "Memory Peek/Poke",
                  "Hot reload": "Hot Reload", "Screen": "Screen Capture", "Debug": "Debug Server",
                  "Model viewer": "Model & Animation Viewer", "TIM / VRAM": "VRAM Viewer", "Sound": "Sound Tools",
                  "Memory card": "Memory Card Tools", "Link test": "Link Connection Test"}


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
        self._set_icon()
        # The title bars follow the theme: this window's once shown, and the dialogs' (with the icon) when they open
        self.bind("<Map>", lambda e: e.widget is self and title_bar_dark(self, self.dark_theme()), add="+")
        self.bind_class("Toplevel", "<Map>", self._dialog_shown, add="+")

        self.runner = Runner(self, self._on_text, self._on_progress, self._on_task, self._on_pcdrv)
        self.devices = []
        self.firmware = None        # The cartridge's firmware (-info): "devcart", "original", None (not known)
        self.listing = False        # ps1transfer -l runs (refresh_devices)
        self.list_again = False     # Asked again meanwhile: listed once more after it
        self.pages = {}
        self.current = None
        self.task_stopped = False   # The running task was stopped by the user
        self.live_listeners = []    # Pages told about the live link (live_line, live_stopped, using_live)
        self.live_ready = False     # The live link answers (MEMLIVE/FRMLIVE ready)
        self.server_folder = None   # Folder served to the PS1 by the running task (PCDRV)

        self._build_top()
        self._build_body()
        self._build_bottom()
        self.show_page(self.settings.field("app.page", "Run PSX"))
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
            page = cls(self.content, self)  # (placed when shown: Tk lays out and draws only the page shown)
            self.pages[cls.title] = page
            text = cls.title if cls.phase == 1 else cls.title + " (soon)"
            b = tb.Button(side, text=text, width=24, bootstyle="secondary-link",
                          command=lambda t=cls.title: self.show_page(t))
            b.pack(fill="x", pady=1)
            self.side_buttons[cls.title] = b
        self._build_cover()

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
        title = OLD_PAGE_NAMES.get(title, title)    # (the last page, saved by an earlier version)
        if title not in self.pages:     # (e.g. "Run" of earlier versions)
            title = PAGES[0].title
        if self.current and self.current is not self.pages[title]:
            self.current.place_forget()
        self.current = self.pages[title]
        self.current.place(relx=0, rely=0, relwidth=1, relheight=1)
        self._show_firmware_note()
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
            self.pages[ConsolePage.title].files_started()
        self.server_folder = server     # Shown in the top bar while the task runs
        self.progress.configure(value=0)
        if not quiet:
            self.log.append("\n> %s\n" % subprocess.list2cmdline(["ps1transfer"] + self._device_args(device) + args))
            if console:
                self.pages[ConsolePage.title].append("\n--- %s ---\n" % name)

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

    # ---------------------------------------------------------------- live link

    def live_task(self):
        """LIVE_LINK or FRAME_CAPTURE if one runs (it takes commands: live_send), else None."""
        task = self.runner.task
        return task.name if task and task.name in (LIVE_LINK, FRAME_CAPTURE) else None

    def live_listen(self, page):
        if page not in self.live_listeners:
            self.live_listeners.append(page)

    def live_send(self, line):
        return self.live_task() is not None and self.runner.send(line)

    def live_start(self, args=None, name=LIVE_LINK, cwd=None):
        """Starts the live link (default: ps1transfer -memlive, attached to the program running), unless one runs."""
        if self.live_task() == name:
            return
        if cwd is None:
            folder = self.settings.field("console.pcdrv", "")
            cwd = folder if folder and os.path.isdir(folder) else None
        self.run(args or ["-memlive"], name, long_running=True, stdin=True, console=True, cwd=cwd,
                 on_line=lambda line: self.after(0, self._live_line, line),
                 log_filter=lambda line: not line.startswith(LIVE_PROTOCOL),
                 on_done=lambda code, out: self.after(0, self._live_done))

    def live_release(self):
        """A page doesn't use the live link any more: stopped if no other page uses it (not the frame capture)."""
        if self.live_task() == LIVE_LINK and not any(p.using_live() for p in self.live_listeners):
            self.stop_task()

    def _live_line(self, line):
        if line.startswith(("MEMLIVE ready", "FRMLIVE ready")):
            self.live_ready = True
        for page in self.live_listeners:
            page.live_line(line)

    def _live_done(self):
        self.live_ready = False
        for page in self.live_listeners:
            page.live_stopped()

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
            self.pages[ConsolePage.title].append(text)

    def _on_pcdrv(self, task, line):
        self.pages[ConsolePage.title].files_event(line)

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
        """Lists the cartridges with ps1transfer -l in a thread (the window keeps answering while it waits)."""
        if self.runner.busy:
            return
        if self.listing:
            self.list_again = True
            return
        self.listing, self.list_again = True, False
        found = queue.Queue()
        exe = self.exe()
        threading.Thread(target=lambda: found.put(list_devices(exe)), daemon=True).start()
        self.after(50, self._devices_listed, found)

    def _devices_listed(self, found):
        # (the thread's result is taken here, in the GUI thread, like the runner's output)
        try:
            devices = found.get_nowait()
        except queue.Empty:
            self.after(50, self._devices_listed, found)
            return
        self.listing = False
        if self.list_again:
            return self.refresh_devices()
        self.devices = devices
        values = [d["serial"] + ("  (in use)" if d["in_use"] else "") for d in self.devices]
        self.device.configure(values=values)
        wanted = self.settings.get("device")
        pick = next((v for v in values if v.split(" ")[0] == wanted), values[0] if values else "")
        changed = pick.split(" ")[0] != self.device.get().split(" ")[0]
        self.device.set(pick)
        for page in self.pages.values():
            page.on_devices_listed()
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
        self.firmware = None        # (the other cartridge's, known from its status)
        self._show_firmware_note()
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
        fw = info.get("firmware")
        # The original firmware doesn't answer while a program runs (only a reset): known from its last answer
        if state == "unknown" and not fw and not text and not keep_firmware and self.firmware == "original":
            state, text, fw = "running", "Running (original firmware)", "original"
        self.state.configure(text="  %s  " % (text or state.capitalize()),
                             bootstyle="inverse-" + STATE_STYLES.get(state, "secondary"))
        if keep_firmware:
            return
        if fw == "devcart":
            self.fw.configure(text="Firmware %s" % info.get("firmware_version", "PS1 DEV CART"))
        elif fw:
            self.fw.configure(text="Original firmware")
        else:
            self.fw.configure(text="")
        self.firmware = fw
        self._show_firmware_note()

    def _show_firmware_note(self):
        """Pages that need the PS1 DEV CART firmware, with the original one: the page covered by a note (all), or a
        note at its top (running: what it does while a program runs)."""
        original = self.firmware == "original"
        page = self.current
        if original and page and page.needs_devcart == "all":
            self.cover_title.configure(text=page.title)
            self.cover.place(relx=0, rely=0, relwidth=1, relheight=1)
            self.cover.lift()
        else:
            self.cover.place_forget()
        for p in self.pages.values():
            if p.needs_devcart == "running":
                p.show_devcart_note(original)

    def _build_cover(self):
        self.cover = tb.Frame(self.content, padding=12)
        self.cover_title = tb.Label(self.cover, text="", font=("", 16, "bold"))
        self.cover_title.pack(anchor="w")
        tb.Label(self.cover, text="This page needs the PS1 DEV CART firmware.", bootstyle="warning",
                 font=("", 11, "bold")).pack(anchor="w", pady=(8, 4))
        tb.Label(self.cover, text="This cartridge has the original PS1USB firmware, which can't read or control a "
                                  "program while it runs (only reset it). Flash the PS1 DEV CART firmware from the "
                                  "Firmware page: it can be flashed from the original firmware, and the flash is read "
                                  "back and checked.", wraplength=700, justify="left").pack(anchor="w")
        tb.Button(self.cover, text="Open the Firmware page", bootstyle="primary",
                  command=lambda: self.show_page("Firmware")).pack(anchor="w", pady=(12, 0))

    # ---------------------------------------------------------------- misc

    def set_theme(self, theme):
        try:
            self.style.theme_use(theme)
            self.settings.set("theme", theme)
        except Exception as e:  # noqa: BLE001 (unknown theme name)
            self.error(str(e))
        title_bar_dark(self, self.dark_theme())

    def dark_theme(self):
        try:
            return self.style.theme.type == "dark"
        except Exception:  # noqa: BLE001
            return True

    def _set_icon(self):
        """The PS1 DEV CART icon (the cartridge of the VS Code extension's panel), for every window."""
        # (iconphoto's "default" for all the windows shows Windows' generic icon instead: each window gets it)
        self._icons = []
        try:
            self._icons = [tb.PhotoImage(file=resource("icons", "ps1devcart-%d.png" % s)) for s in (256, 48, 32, 16)]
            self.iconphoto(False, *self._icons)
        except Exception:  # noqa: BLE001 (icons missing: ttkbootstrap's)
            pass

    def _dialog_shown(self, event):
        window = event.widget
        if self._icons:
            try:
                window.iconphoto(False, *self._icons)
            except Exception:  # noqa: BLE001
                pass
        title_bar_dark(window, self.dark_theme())

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
