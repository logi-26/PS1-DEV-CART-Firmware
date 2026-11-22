#!/usr/bin/env python3
"""
PS1 SLI 3 SCREENS on three PCSX-Redux windows, no console: the PS1 DEV CART repo's demo (demos/sli3), each screen an
emulator instead of a PlayStation, for recording the flight from one computer.

    python sli_redux3.py [--screens 3] [--layout row|monitors|fullscreen] [--width 640] [--redux C:\\path\\pcsx-redux.exe]

What it starts:
- the hub (sli_hub.py, the repo's, unchanged) in its own window: it keeps the screens in step, and "freeze" / "go"
  typed there stop every screen on the same frame and go on;
- one PCSX-Redux per screen, each with its own settings (.redux1, .redux2... here, made from your usual ones, which
  aren't changed): serial port (SIO1) listening on its own local port, raw, the dynarec on, no menu bar, and its window
  placed: left to right in a row on the main monitor (the default: 640 x 480 each, 1920 x 480 for three), one per
  monitor in a window ("--layout monitors", left to right as the monitors are arranged), or one per monitor full
  screen ("--layout fullscreen": borderless, each window made to cover its monitor once it is open. PCSX-Redux's own
  full screen can't do it: it changes the monitor's mode, 60 Hz then, and a full screen window that loses the focus
  minimises itself, while only one of the three can have it);
- the bridges between each emulator's serial port and the hub (as ps1transfer -net is for a console). Each one asks
  the hub for its own place in the screen's hello (the program asks for any): the windows are always left, middle and
  right in that order.

Close the emulators (or Ctrl+C here) to stop. Python 3 only, nothing to install.
"""
import argparse
import copy
import glob
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROGRAM = os.path.join(HERE, "sli.ps-exe")
HUB = os.path.join(HERE, "sli_hub.py")
SERIAL_PORT = 6722          # The first emulator's serial port, the next ones follow
PKT, SYNC, HELLO = 10, 0xA5, 0xFFFF


def crc(p):
    """CRC-16 (CCITT) of a packet's first 8 bytes (as sli.c and sli_hub.py)."""
    c = 0xFFFF
    for b in p[:8]:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else (c << 1)
            c &= 0xFFFF
    return c


def find_redux(given):
    """PCSX-Redux: --redux, PCSX_REDUX, on the PATH, the PSX.Dev extension's copy for VS Code, or C:\\pcsx-redux-*."""
    candidates = [given, os.environ.get("PCSX_REDUX"), shutil.which("pcsx-redux.exe"),
                  os.path.join(os.environ.get("APPDATA", ""), r"Code\User\globalStorage\grumpycoders.psx-dev\pcsx-redux\pcsx-redux.exe")]
                  
    candidates += sorted(glob.glob(r"C:\pcsx-redux*\pcsx-redux.exe"), reverse=True)
    for c in candidates:
        if c and os.path.isfile(c):
            return c
    sys.exit("PCSX-Redux not found: give it with --redux, or set PCSX_REDUX to its pcsx-redux.exe.")


def base_settings(redux):
    """Your usual PCSX-Redux settings (BIOS, controller...): next to it when portable, else in AppData."""
    for path in (os.path.join(os.path.dirname(redux), "pcsx.json"),
                 os.path.join(os.environ.get("APPDATA", ""), "pcsx-redux", "pcsx.json")):
        if os.path.isfile(path):
            with open(path, encoding="utf-8") as f:
                return json.load(f)
    return {}


def monitors():
    """The monitors' areas (x, y, w, h), left to right, the main one if they can't be listed."""
    try:
        import ctypes
        from ctypes import wintypes
        found = []
        proc = ctypes.WINFUNCTYPE(ctypes.c_int, wintypes.HMONITOR, wintypes.HDC, ctypes.POINTER(wintypes.RECT),
                                  wintypes.LPARAM)

        def each(handle, dc, rect, data):
            r = rect.contents
            found.append((r.left, r.top, r.right - r.left, r.bottom - r.top))
            return 1
        ctypes.windll.user32.SetProcessDPIAware()
        ctypes.windll.user32.EnumDisplayMonitors(None, None, proc(each), 0)
        return sorted(found) or [(0, 0, 1920, 1080)]
    except Exception:  # noqa: BLE001 (not Windows: one 1920 x 1080 screen)
        return [(0, 0, 1920, 1080)]


def windows(count, layout, width):
    """Each screen's window (x, y, w, h), left to right."""
    screens = monitors()
    if layout == "fullscreen" and len(screens) >= count:
        return list(screens[:count])        # (the window on its monitor: full screen there)
    if layout == "monitors" and len(screens) >= count:
        # One per monitor, 4:3 as large as fits, centred (the monitors left to right)
        out = []
        for x, y, w, h in screens[:count]:
            ww = min(w, (h - 80) * 4 // 3)
            wh = ww * 3 // 4
            out.append((x + (w - ww) // 2, y + (h - wh) // 2, ww, wh))
        return out
    main = next((m for m in screens if m[0] == 0 and m[1] == 0), screens[0])
    height = width * 3 // 4
    left = main[0] + max(0, (main[2] - width * count) // 2)
    top = main[1] + max(40, (main[3] - height) // 2)
    return [(left + i * width, top, width, height) for i in range(count)]


def settings_for(base, index, window):
    """An emulator's settings: the usual ones, its serial port listening (raw), full speed, only the picture shown."""
    s = copy.deepcopy(base)
    emu = s.setdefault("emulator", {})
    emu["Dynarec"] = True       # (the interpreter runs at about 45 fps: every screen would wait)
    debug = emu.setdefault("Debug", {})
    debug.update({"GdbServer": False, "SIO1Server": True, "SIO1ServerPort": SERIAL_PORT + index, "SIO1Client": False,
                  "SIO1Mode": 1})
    x, y, w, h = window
    gui = s.setdefault("gui", {})
    for key in list(gui):       # (no debug window: the assembly view, the log... would open as windows of their own)
        if key.startswith("Show") and isinstance(gui[key], bool):
            gui[key] = False
    gui.update({"ShowMenu": False, "FullWindowRender": True, "Fullscreen": False,
                                    "WindowMaximized": False, "WindowPosX": x, "WindowPosY": y,
                                    "WindowSizeX": w, "WindowSizeY": h})
    return s


def borderless(pid, rect):
    """Full screen without PCSX-Redux's own: once the emulator's window is open, no title bar or border, and covering
    its monitor (kept on top: the taskbar doesn't come over it)."""
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    user32.SetProcessDPIAware()     # (the monitors' real pixels)
    user32.GetWindowLongW.argtypes = (wintypes.HWND, ctypes.c_int)
    user32.GetWindowLongW.restype = ctypes.c_long
    user32.SetWindowLongW.argtypes = (wintypes.HWND, ctypes.c_int, ctypes.c_long)
    user32.SetWindowPos.argtypes = (wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                    ctypes.c_uint)
    found = []
    proc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)

    def each(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            title = ctypes.create_unicode_buffer(64)
            user32.GetWindowTextW(hwnd, title, 64)
            if title.value.startswith("PCSX-Redux"):
                found.append(hwnd)
        return True
    for _ in range(120):        # (until the window is there: up to a minute)
        found.clear()
        user32.EnumWindows(proc(each), 0)
        if found:
            break
        time.sleep(0.5)
    if not found:
        return
    time.sleep(1)               # (the emulator places its window first)
    hwnd = found[0]
    style = user32.GetWindowLongW(hwnd, -16)                # GWL_STYLE
    style &= ~(0x00C00000 | 0x00040000 | 0x00020000 | 0x00010000 | 0x00080000)  # Caption, frame, buttons, menu
    user32.SetWindowLongW(hwnd, -16, style)
    x, y, w, h = rect
    user32.SetWindowPos(hwnd, wintypes.HWND(-1), x, y, w, h, 0x0020 | 0x0040)  # HWND_TOPMOST, FRAMECHANGED, SHOWWINDOW


def bridge(index, hub_port, stop):
    """Emulator index's serial port <-> the hub. Its hellos ask for place index (left to right)."""
    game = None
    for _ in range(120):        # (until the emulator listens: up to a minute)
        if stop.is_set():
            return
        try:
            game = socket.create_connection(("127.0.0.1", SERIAL_PORT + index), timeout=2)
            break
        except OSError:
            time.sleep(0.5)
    if not game:
        print("Screen %d: the emulator's serial port doesn't answer." % (index + 1))
        return
    try:
        hub = socket.create_connection(("127.0.0.1", hub_port), timeout=5)
    except OSError:
        print("Screen %d: can't connect to the hub (port %d)." % (index + 1, hub_port))
        game.close()
        return
    for sock in (game, hub):
        sock.settimeout(None)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print("Screen %d: connected to the hub." % (index + 1))

    def to_hub():
        buf = b""
        try:
            while not stop.is_set():
                data = game.recv(4096)
                if not data:
                    break
                buf += data
                out = bytearray()
                while buf:
                    if buf[0] != SYNC:
                        out.append(buf[0])
                        buf = buf[1:]
                    elif len(buf) < PKT:
                        break
                    elif crc(buf) == struct.unpack_from("<H", buf, 8)[0]:
                        p = bytearray(buf[:PKT])
                        if (p[1] | (p[2] << 8)) == HELLO and p[5] == 0xFF:     # (the place asked for: this window's)
                            p[5] = index
                            struct.pack_into("<H", p, 8, crc(p))
                        out += p
                        buf = buf[PKT:]
                    else:
                        out.append(buf[0])
                        buf = buf[1:]
                if out:
                    hub.sendall(bytes(out))
        except OSError:
            pass
        stop_pair()

    def to_game():
        try:
            while not stop.is_set():
                data = hub.recv(4096)
                if not data:
                    break
                game.sendall(data)
        except OSError:
            pass
        stop_pair()

    def stop_pair():
        for sock in (game, hub):
            try:
                sock.close()
            except OSError:
                pass

    threads = [threading.Thread(target=to_hub, daemon=True), threading.Thread(target=to_game, daemon=True)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    print("Screen %d: disconnected." % (index + 1))


def main():
    ap = argparse.ArgumentParser(description="PS1 SLI on PCSX-Redux windows")
    ap.add_argument("--screens", type=int, default=3)
    ap.add_argument("--layout", choices=("row", "monitors", "fullscreen"), default="row",
                    help="row: side by side on the main monitor (default); monitors: one screen per monitor, in a "
                         "window; fullscreen: one screen per monitor, full screen")
    ap.add_argument("--width", type=int, default=640, help="each window's width in a row (4:3)")
    ap.add_argument("--redux", help="pcsx-redux.exe")
    ap.add_argument("--hub-port", type=int, default=7040)
    args = ap.parse_args()
    if args.layout != "row" and len(monitors()) < args.screens:
        print("Only %d monitor(s) for %d screens: side by side on the main monitor instead." % (len(monitors()),
                                                                                              args.screens))

    redux = find_redux(args.redux)
    base = base_settings(redux)
    #print("PCSX-Redux: %s" % redux)
    hub = subprocess.Popen([sys.executable, HUB, "--screens", str(args.screens), "--port", str(args.hub_port)],
                           cwd=HERE, creationflags=getattr(subprocess, "CREATE_NEW_CONSOLE", 0))
    time.sleep(1)

    stop = threading.Event()
    emulators, bridges = [], []
    for i, window in enumerate(windows(args.screens, args.layout, args.width)):
        folder = os.path.join(HERE, ".redux%d" % (i + 1))
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, "pcsx.json"), "w", encoding="utf-8") as f:
            json.dump(settings_for(base, i, window), f, indent=2)
        emulators.append(subprocess.Popen([redux, "-portable", folder, "-loadexe", PROGRAM, "-run"], cwd=folder))
        print("Screen %d: PCSX-Redux at %d,%d (%d x %d), serial port %d" % ((i + 1,) + window + (SERIAL_PORT + i,)))
        t = threading.Thread(target=bridge, args=(i, args.hub_port, stop), daemon=True)
        t.start()
        bridges.append(t)
        if args.layout == "fullscreen" and len(monitors()) >= args.screens:
            threading.Thread(target=borderless, args=(emulators[-1].pid, window), daemon=True).start()
    print("The screens start together once all of them are connected (see the hub window). Close the emulators, or")
    print("Ctrl+C here, to stop.")
    try:
        while any(e.poll() is None for e in emulators):
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    stop.set()
    for e in emulators:
        if e.poll() is None:
            e.terminate()
    hub.terminate()


if __name__ == "__main__":
    main()
