#!/usr/bin/env python3
"""
PS1 DEV CART: PicoStation cheat files (.cht) for the games of an SD card, from the Manager's cheat database
(gui/data/cheats.sqlite). The PicoStation's menu (firmware RC5) and the cartridge's PicoStation screen (Triangle)
both list them.

The .cht format (PicoStation RC5, from its menu): text, next to the game's .cue ("name.cht"), at most 6KB (the 3
parts the PicoStation sends) and 256 cheats:
    # a comment (lines starting with # ; / " are left out)
    [Infinite Health]          a cheat, on when the game boots ("[*name]": off)
    80012345 0063              its codes: GameShark, 8 hex digits, a space, 4 hex digits
    D0012346 0001
Only the cheats the cartridge's code engine does are written (as the Manager puts on the cartridge), off at first.

The game: its ID read from its disc image (picostation_ids.py), its cheats from the database by it.

    python picostation_cheats.py G:\\                       every game of the card with cheats in the database
    python picostation_cheats.py G:\\ --except-rom ROM      not the games whose cheats are in this cartridge ROM
    python picostation_cheats.py G:\\ --dry-run             only shows what it would do
    Existing .cht files are kept, unless --replace.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "gui"))
import picostation_ids              # noqa: E402
import cheatdb                      # noqa: E402
import codeengine                   # noqa: E402

CHT_SIZE = 3 * 2048                 # What the PicoStation sends
CHT_CHEATS = 256


def cht_text(db, serial, title):
    """The .cht file of a game: (text, cheats written), None if it has none the code engine does."""
    out = ["# %s (%s): cheats from the PS1 DEV CART Manager's database" % (title, serial)]
    size = len(out[0]) + 2
    n = 0
    for _id, name, codes, _by_title in cheatdb.cheats_of(db, serial):
        lines = []
        for line in codes.split("\n"):
            m = re.match(r"^([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{4})$", line.strip())
            if not m:
                break
            lines.append((int(m.group(1), 16), int(m.group(2), 16)))
        else:
            if not lines or len(lines) > 255 or any(c >> 24 not in cheatdb.ENGINE_TYPES for c, _v in lines):
                continue
            if codeengine.safe_codes(lines) != lines:   # (a serial code writing past the RAM)
                continue
            name = re.sub(r"[\[\]\r\n]", "", name).strip()[:40] or "Cheat"
            block = ["[*%s]" % name] + ["%08X %04X" % (c, v) for c, v in lines]
            block_size = sum(len(l) + 2 for l in block)
            if size + block_size > CHT_SIZE or n == CHT_CHEATS:
                break
            out += block
            size += block_size
            n += 1
    return ("\r\n".join(out) + "\r\n", n) if n else None


def main():
    parser = argparse.ArgumentParser(description="PicoStation cheat files (.cht) from the Manager's cheat database.")
    parser.add_argument("sd", help="the SD card (drive or folder)")
    parser.add_argument("--except-rom", help="a cartridge ROM: its games' cheats left out (already on it)")
    parser.add_argument("--replace", action="store_true", help="replace the .cht files already there")
    parser.add_argument("--dry-run", action="store_true", help="only show what would be done")
    args = parser.parse_args()

    db = cheatdb.connect(os.path.join(HERE, "..", "gui", "data", "cheats.sqlite"))
    skip = set()
    if args.except_rom:
        rom = open(args.except_rom, "rb").read()
        skip = {s.upper() for s, _t, _n, _b in
                cheatdb.read_rom_cheats(rom[cheatdb.ROM_CHEATS:cheatdb.ROM_CHEATS + cheatdb.ROM_CHEATS_SIZE])}

    written = 0
    for folder, dirs, files in os.walk(args.sd):
        dirs[:] = sorted(d for d in dirs if not d.startswith(".") and d != "System Volume Information")
        for cue in sorted(f for f in files if f.lower().endswith(".cue")):
            cue_path = os.path.join(folder, cue)
            label = os.path.relpath(cue_path, args.sd)
            try:
                serial = picostation_ids.game_id(cue_path)
            except (OSError, ValueError) as e:
                print("%-60s ID not found: %s" % (label, e))
                continue
            if serial in skip:
                print("%-60s %-12s its cheats are on the cartridge" % (label, serial))
                continue
            title = cheatdb.game(db, serial)[0] or os.path.splitext(cue)[0]
            cht = cht_text(db, serial, title)
            if not cht:
                print("%-60s %-12s no cheats in the database" % (label, serial))
                continue
            cht_path = os.path.splitext(cue_path)[0] + ".cht"
            if os.path.exists(cht_path) and not args.replace:
                print("%-60s %-12s has a .cht already (--replace)" % (label, serial))
                continue
            print("%-60s %-12s %d cheats, %d bytes" % (label, serial, cht[1], len(cht[0])))
            written += 1
            if not args.dry_run:
                with open(cht_path, "w", newline="", encoding="ascii", errors="replace") as f:
                    f.write(cht[0])
    print("\n%d .cht files%s" % (written, " (dry run: nothing written)" if args.dry_run else ""))


if __name__ == "__main__":
    main()
