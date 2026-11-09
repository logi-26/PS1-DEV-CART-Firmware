"""
Movie tester host build: the console's movie player (ps1-tools/videotest/videotest.c, up to its screen drawing)
compiled for the PC, run by vthost.c a vertical blank at a time as its main loop runs it, with the hardware replaced:
the file server reads the PC's files, the MDEC decodes in a time given (and the frames' bytes are checked), the SPU
plays the sound ring (its interrupt included) into a sound file, the GPU uploads are checked against the VRAM.

The source is copied with these changes only: the SDK includes dropped (vthost_sdk.h instead), the hardware registers
pointed at host memory, hooks (a frame decoded, a frame shown, the copy of a sector into a frame buffer checked), the
screen drawing and main cut off. Built with the 32-bit MinGW of buildtools.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
SOURCE = os.path.join(ROOT, "ps1-tools", "videotest", "videotest.c")
BUILD = os.path.join(HERE, "build")
EXE = os.path.join(BUILD, "vthost.exe")
GCC_DIR = os.path.join(ROOT, "buildtools", "mingw32", "bin")


def _replace(text, old, new, count=1):
    if text.count(old) != count:
        raise RuntimeError("videotest.c changed: %r found %d times (expected %d): update build_host.py"
                           % (old, text.count(old), count))
    return text.replace(old, new)


def patched_source():
    with open(SOURCE, encoding="utf-8") as f:
        text = f.read().replace("\r\n", "\n")
    text = text[:text.index("/****************************************/\n// Screen")]
    for inc in ("<psxgpu.h>", "<psxgte.h>", "<psxetc.h>", "<psxapi.h>", "<psxspu.h>", "<psxpress.h>",
                '"psyq_compat.h"', '"devcart_ui.h"', '"psxpcdrv.h"'):
        text = _replace(text, "#include %s\n" % inc, "")
    text = _replace(text, "#define\tSPU_VOICE(_v_)\t((volatile uint16_t *)(0x1F801C00 + (_v_) * 16))",
                    "#define\tSPU_VOICE(_v_)\t(&host_spu[(_v_) * 8])")
    text = _replace(text, "#define\tSPU_REG(_a_)\t(*((volatile uint16_t *)(_a_)))",
                    "#define\tSPU_REG(_a_)\t(host_spu[((_a_) - 0x1F801C00) / 2])")
    text = _replace(text, "#define\tVT_IRQ_MASK\t\t(*((volatile uint32_t *)0x1F801074))", "#define\tVT_IRQ_MASK\t\thost_irq_mask")
    text = _replace(text, "#define\tHBLANK_COUNTER\t(*((volatile uint32_t *)0x1F801110))", "#define\tHBLANK_COUNTER\thost_lines")
    text = _replace(text, "#define\tHBLANK_MODE\t\t(*((volatile uint32_t *)0x1F801114))", "#define\tHBLANK_MODE\t\thost_lines_mode")
    # Hooks
    text = _replace(text, "\t\tmemcpy((uint8_t *)frame_data[asm_buf] + index * VIDEO_PAYLOAD, d + VIDEO_HEADER, n);",
                    "\t\tHostCopy(asm_buf, index * VIDEO_PAYLOAD, d + VIDEO_HEADER, n);")
    text = _replace(text, "\tslot_state[slot] = SLOT_DECODING;\n", "\tslot_state[slot] = SLOT_DECODING;\n"
                    "\tHostDecode(slot, w, h);\n")
    text = _replace(text, "\t\tframes_shown++;\n", "\t\tframes_shown++;\n\t\tHostShown(r);\n")
    return text


def build(force=False):
    os.makedirs(BUILD, exist_ok=True)
    sources = [SOURCE, os.path.join(HERE, "vthost.c"), os.path.join(HERE, "vthost_sdk.h"), os.path.abspath(__file__)]
    if not force and os.path.isfile(EXE) and os.path.getmtime(EXE) >= max(os.path.getmtime(s) for s in sources):
        return EXE
    with open(os.path.join(BUILD, "videotest_host.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write(patched_source())
    env = dict(os.environ, PATH=GCC_DIR + os.pathsep + os.environ.get("PATH", ""))
    cmd = [os.path.join(GCC_DIR, "gcc.exe"), "-m32", "-O1", "-g", "-Wall", "-Wno-unused-function",
           "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-Wno-int-to-pointer-cast",
           "-Wno-pointer-to-int-cast", "-I", BUILD, "-I", HERE, os.path.join(HERE, "vthost.c"), "-o", EXE]
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("vthost build failed:\n" + r.stdout + r.stderr)
    if r.stderr.strip():
        print(r.stderr, file=sys.stderr)
    return EXE


if __name__ == "__main__":
    print(build(force=True))
