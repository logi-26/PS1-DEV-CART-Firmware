"""
PS1 DEV CART GUI: ps1transfer location, connected cartridges ("ps1transfer -l") and cartridge state ("ps1transfer -info").

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import re
import subprocess
import sys

# No console window for ps1transfer on Windows
NO_WINDOW = 0x08000000 if sys.platform == "win32" else 0

GUI_DIR = os.path.dirname(os.path.abspath(__file__))


def default_ps1transfer():
    """ps1transfer next to the GUI program (single file build), in the repository's ps1transfer folder, else from the PATH."""
    name = "ps1transfer.exe" if sys.platform == "win32" else "ps1transfer"
    places = [os.path.join(GUI_DIR, "..", "ps1transfer", name)]
    if getattr(sys, "frozen", False):      # PyInstaller build: next to the .exe, or gui/dist -> ../../ps1transfer
        exe_dir = os.path.dirname(sys.executable)
        places = [os.path.join(exe_dir, name), os.path.join(exe_dir, "..", "..", "ps1transfer", name)]
    for path in places:
        if os.path.isfile(path):
            return os.path.normpath(path)
    return name


def run_quick(exe, args, timeout=20):
    """Runs ps1transfer and returns (exit code, output)."""
    try:
        p = subprocess.run([exe] + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                           timeout=timeout, creationflags=NO_WINDOW, cwd=os.path.dirname(os.path.abspath(exe)) or None)
        return p.returncode, p.stdout.decode(errors="replace")
    except (OSError, subprocess.TimeoutExpired) as e:
        return -1, str(e)


# "-z 0  -n A10PUCVT     FT245R USB FIFO (in use)"
DEVICE_LINE = re.compile(r"^-z\s+(\d+)\s+-n\s+(\S+)\s*(.*?)(\s+\(in use\))?\s*$")


def list_devices(exe):
    """Connected cartridges: list of dicts (index, serial, desc, in_use)."""
    code, out = run_quick(exe, ["-l"])
    devices = []
    for line in out.splitlines():
        m = DEVICE_LINE.match(line.strip())
        if m:
            devices.append({"index": int(m.group(1)), "serial": m.group(2), "desc": m.group(3),
                            "in_use": bool(m.group(4))})
    return devices


def parse_info(text):
    """key=value lines of "ps1transfer -info"."""
    info = {}
    for line in text.splitlines():
        if "=" in line and not line.startswith(" "):
            key, _, value = line.partition("=")
            info[key.strip()] = value.strip()
    return info
