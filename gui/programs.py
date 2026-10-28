"""
PS1 DEV CART GUI: the kinds of program files (Run page).

- "psx": a PS-X EXE (PSn00bSDK, PsyQ... also a Net Yaroze game converted with yarexe, which includes the library);
- "ecoff": a Net Yaroze executable as the Net Yaroze compiler makes it (run with the library, ps1transfer -e -y);
- "auto": a siocons auto script (its "local load" program and "local dload" data files, ps1transfer -auto -y).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os

ECOFF_MAGIC = b"\x62\x01"       # MIPS little endian
AUTO_MAX_DEPTH = 4


def file_kind(path):
    """'psx', 'ecoff', otherwise 'auto'."""
    try:
        with open(path, "rb") as f:
            head = f.read(8)
    except OSError:
        return "auto"
    if head == b"PS-X EXE":
        return "psx"
    if head[:2] == ECOFF_MAGIC:
        return "ecoff"
    return "auto"


def needs_libps(path):
    """A Net Yaroze game as the Net Yaroze tools make it: the library is uploaded first."""
    return file_kind(path) in ("ecoff", "auto")


def _relative(script, name):
    """A file named in an auto script: relative to the folder of the script ('\\' or '/')."""
    name = name.replace("\\", os.sep).replace("/", os.sep)
    return name if os.path.isabs(name) else os.path.join(os.path.dirname(script), name)


def auto_files(script, depth=0):
    """The files an auto script loads (as ps1transfer -auto reads it): the program, the data files, the scripts it
    calls, in their order. Files that can't be read are listed too (the start reports them)."""
    files = [script]
    if depth >= AUTO_MAX_DEPTH:
        return files
    try:
        with open(script, "r", errors="replace") as f:
            lines = f.readlines()
    except OSError:
        return files
    for line in lines:
        w = line.split()
        if w and w[0] == "local":
            w = w[1:]
        if len(w) >= 2 and w[0] == "load":
            files.append(_relative(script, w[1]))
        elif len(w) >= 3 and w[0] == "dload":
            files.append(_relative(script, w[1]))
        elif len(w) >= 2 and w[0] == "auto":
            files += auto_files(_relative(script, w[1]), depth + 1)
    return files


def program_name(path):
    """The name shown for a game: its file name, or for an auto script the name of its folder."""
    if not path:
        return ""
    if file_kind(path) == "auto":
        return os.path.basename(os.path.dirname(os.path.abspath(path))) or "auto"
    return os.path.splitext(os.path.basename(path))[0]
