"""
PS1 DEV CART: the room left in each place the firmware has, after a build (ci/build_firmware.sh). Fails when one is
over (the builds stop on most of them already: this also catches a check removed by mistake) and warns when one has
less than WARN_BYTES left. On GitHub Actions the table goes to the job's summary.

  python ci/check_sizes.py

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WARN_BYTES = 256

ROM_SIZE = 0x20000
PAK_MAX = 0xAC00                    # cartrom/Makefile: the firmware's first part, 0x400-0xB000
TFS_ADRS, MONITOR_ADRS = 0x10000, 0x19000   # cartrom/rom.s: menu images then the firmware's second part, up to the monitor
MONITOR_MAX = 0x5F00                # monitor/monitor.ld: 0x19000-0x1EF00
HOOK_START, HOOK_END = 0x8000C160, 0x8000D800   # kernel_hook/ps-exe_noheader_kernel_hook.ld: its "ram" region
FW_RAM_END = 0x80200000             # The firmware runs at the top of the 2 MB of RAM (Makefile: TLOAD_ADDR)


def path(*p):
    return os.path.join(ROOT, *p)


def size(*p):
    return os.path.getsize(path(*p))


def read(*p):
    with open(path(*p), encoding="utf-8", errors="replace") as f:
        return f.read()


def hook_end():
    """The end of what the kernel hook puts in its region (its map's output sections there)."""
    end = HOOK_START
    for m in re.finditer(r"^(\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)", read("kernel_hook", "kernel_hook.map"), re.M):
        a, n = int(m.group(2), 16), int(m.group(3), 16)
        if HOOK_START <= a < HOOK_END + 0x1000 and a < HOOK_END:
            end = max(end, a + n)
    return end


def firmware_range():
    """Where the firmware is in RAM: its load address (TLOAD_ADDR, its .text) and its end (_end), from its map."""
    text = read("ps1devcart.map")
    start = re.search(r"^\.text\s+0x([0-9a-f]+)", text, re.M)
    end = re.search(r"^\s+0x([0-9a-f]+)\s+_end = \.", text, re.M)
    if not start or not end:
        raise SystemExit("ps1devcart.map: .text or _end not found")
    return int(start.group(1), 16), int(end.group(1), 16)


def main():
    rows, failed, warned = [], False, False

    def row(place, used, room):
        nonlocal failed, warned
        left = room - used
        state = "over" if left < 0 else ("low" if left < WARN_BYTES else "ok")
        failed |= left < 0
        warned |= 0 <= left < WARN_BYTES
        rows.append((place, used, room, left, state))

    rom = size("cartrom", "ps1devcart.rom")
    if rom != ROM_SIZE:     # (rom_rounder pads it to a power of 2: bigger than the flash when something overflowed)
        print("::error::cartrom/ps1devcart.rom is %d bytes, not the flash's %d" % (rom, ROM_SIZE))
        failed = True
    # The packed firmware: before the cheats (PAK_MAX at most, the packer cuts it there), the rest after the menu images
    tfs = (size("cartrom", "tim_packed.tfs") + 3) & ~3
    row("Firmware, packed (0x400-0xB000, then after the menu images)",
        size("cartrom", "ps1devcart.pak") + size("cartrom", "ps1devcart.pak2"),
        PAK_MAX + (MONITOR_ADRS - TFS_ADRS) - tfs)
    row("Menu images, packed (from 0x10000)", tfs, MONITOR_ADRS - TFS_ADRS)
    row("Debug monitor (0x19000-0x1EF00)", size("monitor_rom.bin"), MONITOR_MAX)
    row("Kernel hook (0x8000C160-0x8000D800)", hook_end() - HOOK_START, HOOK_END - HOOK_START)
    fw_start, fw_end = firmware_range()
    row("Firmware in RAM (0x%08X-0x80200000)" % fw_start, fw_end - fw_start, FW_RAM_END - fw_start)

    lines = ["| Place | Used | Room | Left | |", "|---|---:|---:|---:|---|"]
    for place, used, room, left, state in rows:
        lines.append("| %s | %d | %d | %d | %s |" % (place, used, room, left,
                                                    {"ok": "", "low": "low", "over": "**OVER**"}[state]))
    text = "\n".join(lines)
    print(text)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write("### Firmware room left (bytes)\n\n" + text + "\n\n")
    for place, _u, _r, left, state in rows:
        if state == "low":
            print("::warning::%s: only %d bytes left" % (place, left))
        elif state == "over":
            print("::error::%s: %d bytes over" % (place, -left))

    # The ROM in the repository built from its sources? (a warning: a ROM committed before its last source change)
    r = subprocess.run(["git", "diff", "--quiet", "--", "cartrom/ps1devcart.rom"], cwd=ROOT)
    if r.returncode:
        print("::warning::cartrom/ps1devcart.rom built here differs from the one in the repository "
              "(sources changed since it was built, or another toolchain)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
