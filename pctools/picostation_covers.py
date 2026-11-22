#!/usr/bin/env python3
"""
PS1 DEV CART: cover images converted to the PicoStation's cover format (.cov), for its menu and the cartridge's
PicoStation screen (sdbrowser.h).

A .cov file: a 256-color palette (PS1 colors, 16 bits little endian, 512 bytes), then 128x128 pixels (one byte each,
rows from the top): 16896 bytes. The image is scaled to fit (its shape kept, black around it; --stretch: the whole
square), transparent parts black, then reduced to 256 colors. Black is written 8000h (opaque black: 0000h is
transparent when the PicoStation's menu draws the cover).

    python picostation_covers.py C:\\Users\\me\\PlayStation
        every image of the folder converted, in the folder "cov" inside it (same names, .cov)

    python picostation_covers.py C:\\Users\\me\\PlayStation --sd G:\\
        the same, then each game of the SD card gets its cover: the image whose name matches the game's (the .cue's,
        or its folder's: "Grand Theft Auto" matches "Grand Theft Auto [NTSC-U].png"; brackets, case and punctuation
        don't count), of the game's region when there are several (its ID: SLUS/SCUS NTSC-U, SLES/SCES PAL,
        SLPS/SCPS... NTSC-J). Written as "name.cov" next to the .cue, with the game's ID after the image (as
        picostation_ids.py does). Covers already on the card are kept, unless --replace. A folder holding one game
        also gets the cover beside it ("folder.cov", a copy): the cartridge finds it at once (a cover in the folder
        only: about 2 seconds a game, the folder looked into). A game without a cover image gets the template
        (no_cover_template.png here: a PlayStation case, "NO COVER"; --template: another image) and its ID: the
        cartridge doesn't look for a cover in vain (as long, 2.5 seconds), and its cheats are found.
    --dry-run: only shows what would be done

Needs Pillow (python -m pip install pillow).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""

import argparse
import os
import re
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is needed: python -m pip install pillow")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import picostation_ids

SIZE = 128
IMAGES = (".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp")


# The cover of games without one (no_cover_template.png: a PlayStation case, its spine and "NO COVER"; any image)
TEMPLATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "no_cover_template.png")


def make_template(path):
    """A plain template, made when no_cover_template.png is missing (drawn 4 times bigger, then reduced): dark
    blue, a disc, "NO COVER"."""
    from PIL import ImageDraw, ImageFont
    s = 4
    img = Image.new("RGB", (128 * s, 128 * s), (14, 16, 28))
    d = ImageDraw.Draw(img)
    cx, cy, r = 64 * s, 54 * s, 26 * s
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(60, 70, 110), outline=(90, 110, 170), width=s)
    d.ellipse((cx - 6 * s, cy - 6 * s, cx + 6 * s, cy + 6 * s), fill=(14, 16, 28), outline=(90, 110, 170), width=s)
    try:
        font = ImageFont.truetype("arialbd.ttf", 12 * s)
    except OSError:
        font = ImageFont.load_default()
    w = d.textlength("NO COVER", font=font)
    d.text(((128 * s - w) / 2, 88 * s), "NO COVER", font=font, fill=(80, 160, 255))
    img.resize((128, 128), Image.LANCZOS).save(path)


def no_cover(template=TEMPLATE):
    """The image of a game without a cover."""
    if not os.path.exists(template):
        make_template(template)
    return Image.open(template).convert("RGB")


def to_cov(path, stretch=False):
    """An image (a file, or a PIL image) as the 16896 bytes of a .cov (palette, pixels)."""
    src = (path if isinstance(path, Image.Image) else Image.open(path)).convert("RGBA")
    back = Image.new("RGBA", src.size, (0, 0, 0, 255))
    src = Image.alpha_composite(back, src).convert("RGB")
    if stretch:
        img = src.resize((SIZE, SIZE), Image.LANCZOS)
    else:
        scale = min(SIZE / src.width, SIZE / src.height)
        w, h = max(1, round(src.width * scale)), max(1, round(src.height * scale))
        img = Image.new("RGB", (SIZE, SIZE), (0, 0, 0))
        img.paste(src.resize((w, h), Image.LANCZOS), ((SIZE - w) // 2, (SIZE - h) // 2))
    q = img.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG)
    pal = q.getpalette()[:256 * 3]
    pal += [0] * (256 * 3 - len(pal))
    out = bytearray()
    for i in range(256):
        r, g, b = pal[i * 3] >> 3, pal[i * 3 + 1] >> 3, pal[i * 3 + 2] >> 3
        c = r | (g << 5) | (b << 10)
        out += (c or 0x8000).to_bytes(2, "little")     # (black: opaque)
    out += q.tobytes()
    assert len(out) == picostation_ids.COVER_IMAGE
    return bytes(out)


def key(name):
    """A name compared without brackets, case and punctuation."""
    name = re.sub(r"[\[(][^\])]*[\])]", " ", name)
    return re.sub(r"[^a-z0-9]+", "", name.lower())


def disc(name):
    """The disc number in a name: "(Disc 2)" or "[CD2]", 0 if none."""
    m = re.search(r"[\[(]\s*(?:Disc|CD)\s*(\d)\s*[\])]", name, re.I)
    return int(m.group(1)) if m else 0


def find_images(by_key, game):
    """The images for a game's name: the same name; its main title (before " - "); an image name its name ends
    with (8 characters at least: "CTR - Crash Team Racing", "Crash Team Racing"). [] if none."""
    k = key(game)
    if k in by_key:
        return by_key[k]
    main = re.sub(r"[\[(][^\])]*[\])]", " ", game).split(" - ")[0]
    if key(main) in by_key:
        return by_key[key(main)]
    ends = [ik for ik in by_key if len(ik) >= 8 and k.endswith(ik)]
    return by_key[max(ends, key=len)] if ends else []


def region(name):
    m = re.search(r"\[(NTSC-U|NTSC-J|PAL)\]", name, re.I) or re.search(r"\((USA|Europe|Japan)\)", name, re.I)
    if not m:
        return None
    return {"usa": "NTSC-U", "europe": "PAL", "japan": "NTSC-J"}.get(m.group(1).lower(), m.group(1).upper())


def id_region(serial):
    """The region of a game ID (its letters): None if unknown."""
    p = serial[:4].upper()
    if p in ("SLUS", "SCUS", "PAPX"):
        return "NTSC-U"
    if p in ("SLES", "SCES", "SCED"):
        return "PAL"
    if p in ("SLPS", "SCPS", "SLPM", "SCPM", "SIPS", "SCZS"):
        return "NTSC-J"
    return None


def main():
    parser = argparse.ArgumentParser(description="Cover images to the PicoStation's .cov format.")
    parser.add_argument("images", help="the folder of cover images")
    parser.add_argument("--out", help="where the .cov files go (default: a 'cov' folder in the images' folder)")
    parser.add_argument("--stretch", action="store_true", help="fill the whole square (the shape not kept)")
    parser.add_argument("--sd", help="the SD card: each game gets its cover")
    parser.add_argument("--replace", action="store_true", help="with --sd: replace the covers already on the card")
    parser.add_argument("--dry-run", action="store_true", help="only show what would be done")
    parser.add_argument("--template", default=TEMPLATE,
        help="with --sd: the image for games without a cover (default: %(default)s, made if missing)")
    parser.add_argument("--no-placeholder", action="store_true", help="with --sd: games without a cover left without")
    args = parser.parse_args()

    files = sorted(f for f in os.listdir(args.images) if f.lower().endswith(IMAGES))
    if not files:
        sys.exit("No images in %s" % args.images)
    out = args.out or os.path.join(args.images, "cov")
    if not args.dry_run:
        os.makedirs(out, exist_ok=True)

    # Every image converted
    covs = {}
    for n, f in enumerate(files, 1):
        base = os.path.splitext(f)[0]
        try:
            data = to_cov(os.path.join(args.images, f), args.stretch)
        except (OSError, ValueError) as e:
            print("%s: not converted (%s)" % (f, e))
            continue
        covs[base] = data
        if not args.dry_run:
            with open(os.path.join(out, base + ".cov"), "wb") as h:
                h.write(data)
        if not n % 50:
            print("%d/%d converted" % (n, len(files)), flush=True)
    print("%d covers%s" % (len(covs), "" if args.dry_run else " in " + out))
    if not args.sd:
        return

    # The SD card's games: the cover of the same name (and region)
    by_key = {}
    for base in covs:
        by_key.setdefault(key(base), []).append(base)
    found = missing = 0

    def beside(folder, cov_path, single):
        """A folder holding one game: its cover copied beside the folder too ("folder.cov"), which the cartridge
        finds at once (the cover in the folder: the folder looked into first, about 2 seconds a game)."""
        if not single or os.path.abspath(folder) == os.path.abspath(args.sd) or not os.path.exists(cov_path):
            return
        outer = folder.rstrip("\\/") + ".cov"
        data = open(cov_path, "rb").read()
        if os.path.exists(outer) and open(outer, "rb").read() == data:
            return
        print("%-60s copied beside its folder" % os.path.relpath(outer, args.sd))
        if not args.dry_run:
            with open(outer, "wb") as h:
                h.write(data)

    for folder, dirs, names in os.walk(args.sd):
        dirs[:] = sorted(d for d in dirs if not d.startswith(".") and d != "System Volume Information")
        cues = sorted(f for f in names if f.lower().endswith(".cue"))
        for cue in cues:
            cue_path = os.path.join(folder, cue)
            label = os.path.relpath(cue_path, args.sd)
            try:
                serial = picostation_ids.game_id(cue_path)
            except (OSError, ValueError):
                serial = None
            game = os.path.splitext(cue)[0]
            # The .cue's name, or its folder's when it is the folder's only game
            candidates = find_images(by_key, game) or \
                (find_images(by_key, os.path.basename(folder)) if len(cues) == 1 else [])
            if not candidates:
                cov_path = os.path.splitext(cue_path)[0] + ".cov"
                old = open(cov_path, "rb").read() if os.path.exists(cov_path) else b""
                missing += 1
                # The template (a blank cover made by picostation_ids.py replaced too)
                if not args.no_placeholder and (not old or old[16896:16900] == b"PSIX" or args.replace):
                    print("%-60s no cover image: the template" % label)
                    if not args.dry_run:
                        with open(cov_path, "wb") as h:
                            h.write(to_cov(no_cover(args.template)))
                        if serial:
                            picostation_ids.write_cover(cov_path, serial, False)
                else:
                    print("%-60s no cover image" % label)
                beside(folder, cov_path, len(cues) == 1)
                continue
            # The best one: the game's disc ([CD2] for disc 2, no disc tag for disc 1), then its region
            want = id_region(serial) if serial else None
            want_disc = disc(game) or disc(os.path.basename(folder))
            chosen = max(candidates, key=lambda c: (
                (disc(c) or 1) == (want_disc or 1), bool(want and region(c) == want), -candidates.index(c)))
            cov_path = os.path.splitext(cue_path)[0] + ".cov"
            if os.path.exists(cov_path) and not args.replace:
                print("%-60s has a cover already (--replace to use %s)" % (label, chosen))
                beside(folder, cov_path, len(cues) == 1)
                continue
            found += 1
            print("%-60s %s" % (label, chosen))
            if not args.dry_run:
                with open(cov_path, "wb") as h:
                    h.write(covs[chosen])
                if serial:
                    picostation_ids.write_cover(cov_path, serial, False)
            beside(folder, cov_path, len(cues) == 1)
    print("\n%d games given a cover, %d without an image%s" % (found, missing,
        " (dry run: nothing written)" if args.dry_run else ""))


if __name__ == "__main__":
    main()
