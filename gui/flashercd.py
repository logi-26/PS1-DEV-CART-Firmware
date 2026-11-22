"""
PS1 DEV CART GUI: the flasher CD (repairing or installing a cartridge without a working firmware), made from the
image without licence data shipped with the Manager (ps1-tools/ar_flash/ar_flash_nolicense.bin: the flasher with its
built-in ROM, its first 16 sectors empty) and the licence sectors of one of the user's own PlayStation disc images:
the Sony licence data isn't published with the firmware, each user takes it from a disc of their own.

The licence is the data track's first 16 sectors (the licence text in sector 4, the PlayStation logo after it). Each
sector carries its own error detection and correction, so they are copied as they are (their 2336 bytes after the
sync and header, the header of the image kept): from a .bin/.img or a .cue's data track (2352 or 2336-byte sectors),
or made again as Mode 2 Form 1 sectors from an .iso's 2048 bytes of data.

  python flashercd.py DISC_IMAGE OUT_FOLDER     (the same from the command line)

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import sys

import discimage

RAW = 2352
LICENCE_SECTORS = 16
LICENCE_TEXT = b"Sony Computer Entertainment"
NAME = "ar_flash"
HERE = os.path.dirname(os.path.abspath(__file__))


def default_image():
    """The image without licence: next to the Manager's executable (release zip), or the repository's. "" if none."""
    places = [os.path.join(HERE, "..", "ps1-tools", "ar_flash", NAME + "_nolicense.bin")]
    if getattr(sys, "frozen", False):
        exe_dir = os.path.dirname(sys.executable)
        places = [os.path.join(exe_dir, NAME + "_nolicense.bin"),
                  os.path.join(exe_dir, "..", "..", "..", "ps1-tools", "ar_flash", NAME + "_nolicense.bin")]
    return next((os.path.normpath(p) for p in places if os.path.isfile(p)), "")


def rom_version(image):
    """The firmware version of the ROM built in the flasher image ("Firmware: x.y"), "" if not found."""
    with open(image, "rb") as f:
        data = f.read()
    i = data.find(b"Firmware: ")
    return data[i + 10:i + 20].split(b"\0")[0].decode(errors="replace") if i >= 0 else ""


def licence_sectors(disc):
    """The 16 licence sectors of a disc image, 2336 bytes each (subheader, data, error correction). ValueError if it
    isn't a PlayStation disc image."""
    with discimage.Image(disc) as img:
        if img.size < LICENCE_SECTORS * 2048 or LICENCE_TEXT not in img.read(4 * 2048, 2048):
            raise ValueError("%s isn't a PlayStation disc image (no licence text in its sector 4)."
                             % os.path.basename(disc))
        if img.raw:
            out = b""
            for i in range(LICENCE_SECTORS):
                sector = img._raw(i * img.sector, img.sector)
                out += sector[16:] if img.sector == RAW else sector     # (2336: no sync and header)
            if img.sector == RAW and img.data_off != 24:
                raise ValueError("%s is a Mode 1 disc: not a PlayStation disc." % os.path.basename(disc))
            return out
        # 2048-byte sectors (.iso): the sectors made again as a disc has them: 0-11 Mode 2 Form 1 (subheader: data),
        # 12-15 empty Form 2 sectors
        import strmovie
        datas = [img.read(i * 2048, 2048) for i in range(12)] + [bytes(2324)] * 4
        return strmovie.mode2_sectors([(0, 0, 8, 0)] * 12 + [(0, 0, 0x20, 0)] * 4, datas, [1] * 12 + [2] * 4)


def make(disc, out_dir, base=None):
    """The flasher CD in out_dir (ar_flash.bin + ar_flash.cue) with the licence of disc. Returns the .cue's path."""
    base = base or default_image()
    if not base or not os.path.isfile(base):
        raise ValueError("The flasher image without licence (%s_nolicense.bin) wasn't found." % NAME)
    with open(base, "rb") as f:
        image = bytearray(f.read())
    if len(image) % RAW or len(image) < 32 * RAW or any(image[i * RAW + 24:i * RAW + 24 + 2048].count(0) != 2048
                                                         for i in range(LICENCE_SECTORS)):
        raise ValueError("%s isn't the flasher image without licence." % os.path.basename(base))
    lic = licence_sectors(disc)
    for i in range(LICENCE_SECTORS):
        image[i * RAW + 16:(i + 1) * RAW] = lic[i * 2336:(i + 1) * 2336]   # (the image's sync and header kept)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, NAME + ".bin"), "wb") as f:
        f.write(image)
    cue = os.path.join(out_dir, NAME + ".cue")
    with open(cue, "w", newline="\r\n") as f:
        f.write('FILE "%s.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' % NAME)
    return cue


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        print("Flasher CD:", make(sys.argv[1], sys.argv[2]))
    except ValueError as e:
        sys.exit(str(e))
