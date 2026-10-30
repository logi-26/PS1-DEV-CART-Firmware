"""
PS1 DEV CART GUI: the cartridge's code engine (firmware 1.1, see usb.h: CheatTable), shared by the pages using it.

The console applies the table's codes at every interrupt (kernel_hook.c: ApplyCheats). The Cheats page gives the
cheats that are on, the Remote Controller page the multitap slots it drives (80 writes over the game's multitap
buffer): each page sets its own codes (set_codes), the table written holds them all (the multitap's first, never
left out when there are too many).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
HOOK_PARAMS, FEATURE_CHEATS = 0x8000DF70, 0x20      # Features: byte 3 of the hook's parameters
CHEAT_TABLE, CHEAT_MAGIC, CHEAT_MAX = 0x8000D800, 0x53544843, 216
OWNERS = ("multitap", "cheats")     # In the table in this order


def set_codes(app, owner, lines):
    """owner's codes: [(code, value), ...]."""
    app.engine_codes[owner] = list(lines)


def codes(app, owner):
    return app.engine_codes.get(owner, [])


def table(app):
    """The table of every page's codes (8 bytes of zeros: off)."""
    lines = [line for owner in OWNERS for line in app.engine_codes.get(owner, ())][:CHEAT_MAX]
    if not lines:
        return bytes(8)
    return (CHEAT_MAGIC.to_bytes(4, "little") + len(lines).to_bytes(4, "little") +
            b"".join(c.to_bytes(4, "little") + v.to_bytes(2, "little") + bytes(2) for c, v in lines))


def changed(app):
    """The table differs from the one last written to the console."""
    return table(app) != app.engine_sent


def write(app):
    """The table written to the console (live link: "write"). True if sent."""
    data = table(app)
    if not app.live_send("write %08X %s" % (CHEAT_TABLE, data.hex().upper())):
        return False
    app.engine_sent = data
    return True
