"""
Sound tester host build: the console's own VAB and SEQ player (ps1-tools/soundtest/soundtest.c, everything before
main) compiled for the PC with the SPU replaced by memory, to test it against the reference player (seqref.py) without
a console. The source is copied with these changes only: the PSn00bSDK includes dropped, SPU_REG / SPU_VOICE / the DMA
status pointed at host memory, two hooks (each note on, each restart at the music's end), main cut off. seqhost.c is
the program around it.

Built with the 32-bit MinGW of buildtools (as ps1transfer: 32-bit pointers, as on the PS1).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
SOURCE = os.path.join(ROOT, "ps1-tools", "soundtest", "soundtest.c")
BUILD = os.path.join(HERE, "build")
EXE = os.path.join(BUILD, "seqhost.exe")
GCC_DIR = os.path.join(ROOT, "buildtools", "mingw32", "bin")


def _replace(text, old, new, count=1):
    if text.count(old) != count:
        raise RuntimeError("soundtest.c changed: %r found %d times (expected %d): update build_host.py"
                           % (old, text.count(old), count))
    return text.replace(old, new)


def patched_source():
    with open(SOURCE, encoding="utf-8") as f:
        text = f.read().replace("\r\n", "\n")
    text = text[:text.index("\nint\tmain(void)")]
    for inc in ("<psxgpu.h>", "<psxetc.h>", "<psxapi.h>", "<psxspu.h>", '"psyq_compat.h"', '"devcart_ui.h"'):
        text = _replace(text, "#include %s\n" % inc, "")
    text = _replace(text, "#define\tSPU_VOICE(_v_)\t((volatile uint16_t *)(0x1F801C00 + (_v_) * 16))",
                    "#define\tSPU_VOICE(_v_)\t(&host_spu[(_v_) * 8])")
    text = _replace(text, "#define\tSPU_REG(_a_)\t(*((volatile uint16_t *)(_a_)))",
                    "#define\tSPU_REG(_a_)\t(host_spu[((_a_) - 0x1F801C00) / 2])")
    text = _replace(text, "*((volatile uint32_t *)0x1F8010C8)", "host_dma")
    text = _replace(text, "\t\t\t\tseq_notes++;\n", "\t\t\t\tseq_notes++;\n\t\t\t\tHostNote(c, d1, d2);\n")
    text = _replace(text, "NoteOn(c, channels[c].prog, d1, d2);",
                    "{ NoteOn(c, channels[c].prog, d1, d2); HostNoteDone(c, d1); }")
    text = _replace(text, "\t\t\t\tSeqStart();\n\t\t\t\tseq_playing = 1;",
                    "\t\t\t\thost_restarts++;\n\t\t\t\tSeqStart();\n\t\t\t\tseq_playing = 1;")
    return text


def build(force=False):
    """The host program (built again when soundtest.c or the harness changed). Returns its path."""
    os.makedirs(BUILD, exist_ok=True)
    sources = [SOURCE, os.path.join(HERE, "seqhost.c"), os.path.abspath(__file__)]
    if not force and os.path.isfile(EXE) and os.path.getmtime(EXE) >= max(os.path.getmtime(s) for s in sources):
        return EXE
    with open(os.path.join(BUILD, "soundtest_host.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write(patched_source())
    env = dict(os.environ, PATH=GCC_DIR + os.pathsep + os.environ.get("PATH", ""))
    cmd = [os.path.join(GCC_DIR, "gcc.exe"), "-m32", "-O1", "-g", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
           "-Wno-unused-but-set-variable", "-Wno-int-to-pointer-cast", "-Wno-pointer-to-int-cast",
           "-I", BUILD, os.path.join(HERE, "seqhost.c"), "-o", EXE]
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("seqhost build failed:\n" + r.stdout + r.stderr)
    if r.stderr.strip():
        print(r.stderr, file=sys.stderr)
    return EXE


if __name__ == "__main__":
    print(build(force=True))
