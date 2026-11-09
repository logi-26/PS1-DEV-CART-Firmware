"""
Helpers of the tools' tests: paths, ps1transfer run on the PC (its commands that need no console), temporary files.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
GUI = os.path.join(ROOT, "gui")
TMP = os.path.join(HERE, "build", "tmp")
PS1TRANSFER = os.path.join(ROOT, "ps1transfer", "ps1transfer.exe")
sys.path.insert(0, HERE)
sys.path.insert(0, GUI)
os.makedirs(TMP, exist_ok=True)


def ps1transfer(*args, timeout=60):
    """ps1transfer with these arguments: (exit code, output). A timeout: (None, output so far)."""
    try:
        r = subprocess.run([PS1TRANSFER] + [str(a) for a in args], capture_output=True, text=True, timeout=timeout,
                           errors="replace")
        return r.returncode, r.stdout + r.stderr
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        return None, out


def tmp_path(name):
    fd, path = tempfile.mkstemp(suffix="_" + name, dir=TMP)
    os.close(fd)
    os.remove(path)
    return path


def write(path, data):
    with open(path, "wb") as f:
        f.write(data)
    return path


def read(path):
    with open(path, "rb") as f:
        return f.read()
