#!/usr/bin/env python3
"""
PS1 DEV CART: the game IDs of a PicoStation SD card, for the cartridge's PicoStation screen (sdbrowser.h).

The cartridge can only read what the PicoStation (firmware RC5) sends it, and only a game's cover (.cov) per game. So
each game's ID (its serial: the executable named in its SYSTEM.CNF, e.g. SLUS_001.06, as the cartridge reads it when
it boots a disc) is written in its cover file, after the image: the cartridge then finds the game's cheats (Triangle).

For each .cue file of the card:
  - the ID is read from the game's data track (its first .bin): ISO 9660, SYSTEM.CNF, "BOOT = cdrom:\\NAME;1"
    (PSX.EXE without SYSTEM.CNF, like the BIOS);
  - its cover, "name.cov" next to the .cue (512-byte palette, then 128x128 pixels: 16896 bytes), gets 16 bytes after
    the image: "PSID" and the ID (12 bytes, 0 padded). The PicoStation sends them with the cover (its 8 parts are
    16912 bytes) and ignores them;
  - a game without a cover gets a blank one, marked "PSIX" (the cartridge shows "NO COVER"; the PicoStation's own menu
    shows it black);
  - a folder holding only that game, with its own cover beside it ("folder.cov"), gets the ID too.
Run it again after adding games: the IDs already written are replaced.

    python picostation_ids.py G:\\              the SD card (its drive or folder)
    python picostation_ids.py G:\\ --dry-run    only shows what it would do
    python picostation_ids.py G:\\ --no-blank   only the covers already there (no blank ones made)

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""

import argparse
import os
import re
import sys

COVER_IMAGE = 512 + 128 * 128      # Palette and pixels
TAIL = 16                          # "PSID"/"PSIX", the ID (12 bytes)
SERIAL = 12
RAW_SECTOR = 2352


def cue_data_track(cue_path):
    """The first .bin of a .cue (its data track): (path, mode: 1 or 2), None if there is none."""
    folder = os.path.dirname(cue_path)
    name = None
    with open(cue_path, "r", errors="replace") as f:
        for line in f:
            m = re.match(r'\s*FILE\s+"(.+)"\s+BINARY', line, re.I) or re.match(r'\s*FILE\s+(\S+)\s+BINARY', line, re.I)
            if m and name is None:
                name = m.group(1)
            m = re.match(r"\s*TRACK\s+\d+\s+MODE(\d)/2352", line, re.I)
            if m and name:
                return os.path.join(folder, name), int(m.group(1))
    return None


def read_sector(f, lba, mode):
    """2048 bytes of user data of a raw sector (Mode 1: after 16 bytes, Mode 2 form 1: after 24)."""
    f.seek(lba * RAW_SECTOR)
    raw = f.read(RAW_SECTOR)
    if len(raw) < RAW_SECTOR:
        raise ValueError("sector %d past the end of the image" % lba)
    start = 16 if (raw[15] == 1 or mode == 1) else 24
    return raw[start:start + 2048]


def iso_file(f, mode, name):
    """A file of the ISO 9660 root directory: its bytes, None if it isn't there."""
    pvd = read_sector(f, 16, mode)
    if pvd[1:6] != b"CD001":
        raise ValueError("no ISO 9660 volume")
    root = pvd[156:156 + 34]
    lba = int.from_bytes(root[2:6], "little")
    size = int.from_bytes(root[10:14], "little")
    for off in range(0, size, 2048):
        sector = read_sector(f, lba + off // 2048, mode)
        i = 0
        while i + 33 < 2048 and sector[i]:
            rec = sector[i:i + sector[i]]
            rec_name = rec[33:33 + rec[32]].decode("ascii", "replace").split(";")[0].rstrip(".")
            if rec_name.upper() == name:
                flba = int.from_bytes(rec[2:6], "little")
                fsize = int.from_bytes(rec[10:14], "little")
                data = b"".join(read_sector(f, flba + k, mode) for k in range((fsize + 2047) // 2048))
                return data[:fsize]
            i += sector[i]
    return None


def game_id(cue_path):
    """The game's ID: the executable SYSTEM.CNF boots (PSX.EXE without it), as the cartridge's BootDisc reads it."""
    track = cue_data_track(cue_path)
    if not track:
        raise ValueError("no data track in the .cue")
    path, mode = track
    with open(path, "rb") as f:
        cnf = iso_file(f, mode, "SYSTEM.CNF")
    if cnf is None:
        return "PSX.EXE"
    for line in cnf.decode("ascii", "replace").splitlines():
        m = re.match(r"\s*BOOT\s*=\s*(\S+)", line, re.I)
        if m:
            boot = m.group(1)
            name = re.split(r"[:\\/]", boot)[-1].split(";")[0]
            return name[:SERIAL].upper()    # (some discs: "slus_008.18"; the cheat lists: upper case)
    raise ValueError("SYSTEM.CNF without BOOT")


def write_cover(cov_path, serial, dry_run, blank=True):
    """The ID after the cover's image (a blank cover made if there is none, unless not blank). Returns what was
    done."""
    tail_id = serial.encode("ascii", "replace")[:SERIAL].ljust(SERIAL, b"\0")
    if os.path.exists(cov_path):
        data = open(cov_path, "rb").read()
        if len(data) < COVER_IMAGE:
            return "cover too small (%d bytes, not a .cov): left as it is" % len(data)
        image = data[:COVER_IMAGE]
        marker = b"PSIX" if data[COVER_IMAGE:COVER_IMAGE + 4] == b"PSIX" else b"PSID"
        new = image + marker + tail_id
        what = "ID written in its cover" if marker == b"PSID" else "ID written in its blank cover"
        if new == data:
            return "already done"
    elif not blank:
        return "no cover (none made: --no-blank)"
    else:
        new = bytes(COVER_IMAGE) + b"PSIX" + tail_id
        what = "no cover: a blank one made for its ID"
    if not dry_run:
        with open(cov_path, "wb") as f:
            f.write(new)
    return what


def main():
    parser = argparse.ArgumentParser(description="Writes each game's ID in its cover file, for the PS1 DEV CART.")
    parser.add_argument("sd", help="the SD card (drive or folder)")
    parser.add_argument("--dry-run", action="store_true", help="only show what would be done")
    parser.add_argument("--no-blank", action="store_true", help="only the covers already there (no blank ones made)")
    args = parser.parse_args()
    if not os.path.isdir(args.sd):
        sys.exit("Not a folder: %s" % args.sd)

    games = errors = 0
    for folder, dirs, files in os.walk(args.sd):
        dirs[:] = sorted(d for d in dirs if not d.startswith(".") and d != "System Volume Information")
        cues = sorted(f for f in files if f.lower().endswith(".cue"))
        for cue in cues:
            cue_path = os.path.join(folder, cue)
            label = os.path.relpath(cue_path, args.sd)
            try:
                serial = game_id(cue_path)
            except (OSError, ValueError) as e:
                print("%-60s ID not found: %s" % (label, e))
                errors += 1
                continue
            games += 1
            print("%-60s %-12s %s" % (label, serial,
                write_cover(os.path.splitext(cue_path)[0] + ".cov", serial, args.dry_run, not args.no_blank)))
            # The folder's own cover beside it (a folder holding only this game)
            folder_cov = folder.rstrip("\\/") + ".cov"
            if len(cues) == 1 and os.path.abspath(folder) != os.path.abspath(args.sd) and os.path.exists(folder_cov):
                print("%-60s %-12s %s" % (os.path.relpath(folder_cov, args.sd), serial,
                    write_cover(folder_cov, serial, args.dry_run)))
    print("\n%d games%s%s" % (games, (", %d without an ID" % errors) if errors else "",
        " (dry run: nothing written)" if args.dry_run else ""))


if __name__ == "__main__":
    main()
