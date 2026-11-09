"""
Host builds of console / ps1transfer C code for the tools' tests: functions taken from their source as they are (by
name), put in a test program, compiled with the 32-bit MinGW of buildtools (as on the PS1: 32-bit pointers), built
again when a source changes.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import hashlib
import os
import re
import subprocess

from toolhost import HERE, ROOT

BUILD = os.path.join(HERE, "build")
GCC = os.path.join(ROOT, "buildtools", "mingw32", "bin", "gcc.exe")


def source(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as f:
        return f.read().replace("\r\n", "\n")


def function(rel, name):
    """The text of a function (from its definition line to its closing brace at the line start)."""
    text = source(rel)
    m = re.search(r"^[^\n;{}]*\b%s\s*\([^;{]*\)\s*\n\{" % re.escape(name), text, re.M)
    if not m:
        raise RuntimeError("%s: function %s not found (update the test)" % (rel, name))
    end = text.index("\n}\n", m.start())
    return text[m.start():end + 3]


def block(rel, start, end):
    """The text between two markers (start included, end excluded)."""
    text = source(rel)
    i = text.index(start)
    return text[i:text.index(end, i)]


def build(name, code, cflags=()):
    """A program from C code: its path (built again when the code changes)."""
    os.makedirs(BUILD, exist_ok=True)
    digest = hashlib.sha1((code + repr(cflags)).encode()).hexdigest()[:12]
    exe = os.path.join(BUILD, "%s_%s.exe" % (name, digest))
    if os.path.isfile(exe):
        return exe
    src = os.path.join(BUILD, "%s.c" % name)
    with open(src, "w", encoding="utf-8", newline="\n") as f:
        f.write(code)
    env = dict(os.environ, PATH=os.path.dirname(GCC) + os.pathsep + os.environ.get("PATH", ""))
    r = subprocess.run([GCC, "-m32", "-O1", "-g", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                        "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast"] + list(cflags) + [src, "-o", exe],
                       env=env, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("%s build failed:\n%s%s" % (name, r.stdout, r.stderr))
    return exe
