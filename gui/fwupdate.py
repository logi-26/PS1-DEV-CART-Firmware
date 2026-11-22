"""
PS1 DEV CART GUI: what a firmware update brings. The cartridge's firmware (-info) compared with a ROM file's (the one
bundled with the Manager, or chosen on the Firmware page): the features of the versions in between, for the banner of
the Firmware page and the note in the top bar.

Keep FIRMWARE_FEATURES up to date with each firmware version (FIRMWARE_VERSION in ps1devcart.c): only what needs the
firmware on the cartridge (what the Manager or ps1transfer alone does works with any version).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""

# The original PS1USB firmware against PS1 DEV CART 1.0 (the base of all the versions below)
DEVCART_BASE = [
    "memory, VRAM and screenshots while a program runs (Memory, Screen, Frame Inspector pages)",
    "debugging with GDB / VS Code, crash reports, hot reload, the profiler",
    "faster transfers, the device ID, link play over USB (Net Yaroze and psxcomb)",
]

# (version, what it brings), oldest first; SHORT: the same in a few words (a long list: one line a version)
SHORT = {(1, 1): "disc boot with the drive unlock, cheats on the console, multitap controllers",
         (1, 2): "reliable USB on SCPH-7002/7502 consoles",
         (1, 3): "SCPH-7002/7502 debug monitor fix, cheat screen fixes",
         (1, 4): "file server programs in the profiler, menu screensaver",
         (1, 5): "PicoStation screen, controllers for disc games, safe mode (L1 + R1)"}
FIRMWARE_FEATURES = [
    ((1, 1), ["disc boot with the CD drive unlock (START + SELECT on the menu, ps1transfer -bootcd)",
              "cheats: the code engine and the cheat screen on the console (Cheats page)",
              "multitap controllers and the booted game's name (Remote Controller and Cheats pages)"]),
    ((1, 2), ["reliable USB on SCPH-7002/7502 consoles (BIOS 4.x)"]),
    ((1, 3), ["SCPH-7002/7502: the debug monitor's reads (SPU RAM viewer, memory of a stopped program, VRAM)",
              "cheat screen fixes"]),
    ((1, 4), ["the profiler and Frame Inspector with programs that use the file server",
              "the menu's screensaver"]),
    ((1, 5), ["the PicoStation screen: the SD card's games with covers and cheats",
              "controllers from the computer for games booted from their disc (Remote Controller page)",
              "safe mode: L1 + R1 held at boot",
              "more room in the flash for the next features"]),
]


def version_text(v):
    return "%d.%d" % v if v else "?"


def rom_version(path):
    """The firmware version of a ROM file ("Firmware: x.y" in it) as (major, minor), None if not found."""
    try:
        with open(path, "rb") as f:
            rom = f.read()
    except OSError:
        return None
    i = rom.find(b"Firmware: ")
    if i < 0:
        return None
    try:
        return tuple(int(x) for x in rom[i + 10:i + 20].split(b"\0")[0].decode().split(".")[:2])
    except ValueError:
        return None


def missing(firmware, cart_version, new_version):
    """What the ROM's firmware brings to the cartridge: [(version or None for the base, [features])], [] if nothing
    (same or newer firmware, or not known). firmware: "devcart", "original" or None (-info's firmware=)."""
    if not new_version:
        return []
    if firmware == "original":
        out = [(None, DEVCART_BASE)]
        since = (1, 0)
    elif firmware == "devcart" and cart_version:
        out = []
        since = cart_version
    else:
        return []
    out += [(v, f) for v, f in FIRMWARE_FEATURES if since < v <= new_version]
    return out if (firmware == "original" or (out and cart_version < new_version)) else []


def banner_text(firmware, cart_version, new_version, rom_name):
    """The banner's text, "" when the cartridge is up to date (or unknown)."""
    parts = missing(firmware, cart_version, new_version)
    if not parts:
        return ""
    if firmware == "original":
        head = "This cartridge has the original PS1USB firmware. Flashing %s (PS1 DEV CART firmware %s) adds:" % (
            rom_name, version_text(new_version))
    else:
        head = "This cartridge has firmware %s. Flashing %s (firmware %s) adds:" % (
            version_text(cart_version), rom_name, version_text(new_version))
    lines = [head]
    long_list = sum(len(f) for _v, f in parts) > 8
    for v, features in parts:
        if long_list and v is not None:
            lines.append("  • %s: %s" % (version_text(v), SHORT.get(v, "; ".join(features))))
            continue
        for f in features:
            lines.append("  • %s%s" % (f, "" if v is None else "  (%s)" % version_text(v)))
    lines.append("Your cheats, device ID and settings are kept, and the flash is saved on the computer first.")
    return "\n".join(lines)


def top_note(firmware, cart_version, new_version):
    """Short text for the top bar ("" when up to date)."""
    if not missing(firmware, cart_version, new_version):
        return ""
    return "Update to %s available (Firmware page)" % version_text(new_version)
