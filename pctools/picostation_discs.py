#!/usr/bin/env python3
"""
PS1 DEV CART: a game's discs put in one folder on a PicoStation SD card, so the PicoStation changes disc when the lid
is opened and closed (RC5: the next .cue of the folder the game was started from) and the cartridge's PicoStation
screen shows the game once (its discs to choose from when it is opened).

Folders side by side whose names only differ by "(Disc N)" ("Metal Gear Solid (Disc 1)", "Metal Gear Solid (Disc 2)")
become one folder ("Metal Gear Solid") holding all their files (.cue, .bin, .cov, .cht: their names don't change, so
each .cue still finds its .bin files). The cover beside the folders ("Metal Gear Solid (Disc 1).cov", see
picostation_covers.py) becomes "Metal Gear Solid.cov" (disc 1's); the other discs' are deleted (a copy of the one in
the folder). A game whose files would clash in the new folder is left as it is.

    python picostation_discs.py G:\\ --dry-run      shows what it would do
    python picostation_discs.py G:\\                does it

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""

import argparse
import os
import re
import shutil

DISC = re.compile(r"\s*\((?:Disc|CD)\s*(\d+)\)\s*$", re.I)


def main():
    parser = argparse.ArgumentParser(description="A game's disc folders merged into one (PicoStation SD card).")
    parser.add_argument("sd", help="the SD card (drive or folder)")
    parser.add_argument("--dry-run", action="store_true", help="only show what would be done")
    args = parser.parse_args()

    merged = 0
    for parent, dirs, _files in os.walk(args.sd):
        dirs[:] = sorted(d for d in dirs if not d.startswith(".") and d != "System Volume Information")
        games = {}
        for d in dirs:
            m = DISC.search(d)
            if m:
                games.setdefault(DISC.sub("", d), []).append((int(m.group(1)), d))
        for base, discs in sorted(games.items()):
            if len(discs) < 2:
                continue
            discs.sort()
            target = os.path.join(parent, base)
            moves = []
            for _n, d in discs:
                for f in sorted(os.listdir(os.path.join(parent, d))):
                    moves.append((os.path.join(parent, d, f), os.path.join(target, f)))
            names = [os.path.basename(b) for _a, b in moves]
            if len(set(n.lower() for n in names)) != len(names) or \
               (os.path.exists(target) and any(os.path.exists(b) for _a, b in moves)):
                print("%s: left as it is (files of the same name)" % base)
                continue
            print("%s: %s -> %s\\ (%d files)" % (base, " + ".join(d for _n, d in discs), base, len(moves)))
            merged += 1
            if args.dry_run:
                continue
            os.makedirs(target, exist_ok=True)
            for a, b in moves:
                shutil.move(a, b)
            for _n, d in discs:
                os.rmdir(os.path.join(parent, d))
            # The covers beside the folders: disc 1's beside the new one, the others' deleted
            for k, (_n, d) in enumerate(discs):
                cov = os.path.join(parent, d + ".cov")
                if os.path.exists(cov):
                    if k == 0 and not os.path.exists(target + ".cov"):
                        os.replace(cov, target + ".cov")
                    else:
                        os.remove(cov)
        dirs[:] = [d for d in dirs if os.path.isdir(os.path.join(parent, d))]
    print("\n%d games%s" % (merged, " (dry run: nothing changed)" if args.dry_run else ""))


if __name__ == "__main__":
    main()
