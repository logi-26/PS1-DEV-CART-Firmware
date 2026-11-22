"""Red Alert Retaliation's main menu for Net Play (research, 2026-11-16): the LINK GAME item always shown, renamed
"DEV CART NET PLAY" and never chosen with the pad (the cursor skips it), so that only the Manager starts a link game
(the driver's automatic start sets the menu's choice s4 = 4 itself, without the cursor).

Found in the front end overlay (RAM of .menu/menu.sst, research/redux_trace):
- Main menu 0x80012F10, each pass: LINK GAME's id (item 4, 0x801A9BB4) = -1 (hidden), then _sio_control(0,0,0)
  (0x8001301C, returning to 0x80013024) and id = 4 when its bit 80h is clear. Patched: id = -3 after the call
  (shown: the draw 0x8001B1D4 hides only id -1; skipped by the cursor, below).
- The menus' input 0x8001A934 (every menu): up / down move the cursor past items whose text is 0 or id is -1
  ("bne/beq v0,a1(-1)" at 0x8001AAAC, 0x8001AB10, 0x8001AB8C, 0x8001ABEC). Patched: past any negative id
  (bgez / bltz v0; the game's items have ids 0.. or -1). X gives the id of the item under the cursor.
- A mouse click gives the id of the item under the pointer, without the -1 check: the main menu's "s4 = -1 when -2"
  (0x80013054 "bne s4,v0(-2)") made "s4 = -1 when negative" (bgez s4), so -3 is no choice either.
- The texts: per language a table of 16-bit offsets, then the strings (English 0x80024990, French 0x800251C4,
  German 0x80025A90; 0x8002498C the language's table). Text 6 = LINK GAME. "DEV CART NET PLAY" (17 letters) is
  written over string 0 ("Simon Golding Rules OK", never shown: text 0 is "none") and entry 6 pointed at it; French
  "LIAISON PAR CABLE" has the same length: written over.

  python menu_lock.py           print the keep steps (gui/data/netplay package "keep_steps") and check them
  python menu_lock.py --ram F   also write F: menu.ram with them applied (for Redux)
"""
import os
import struct
import sys

from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN

HERE = os.path.dirname(os.path.abspath(__file__))
RAM = os.path.join(HERE, ".menu", "menu.ram")
NAME = b"DEV CART NET PLAY"
LANGS = {"English": 0x80024990, "French": 0x800251C4, "German": 0x80025A90}
LINK_TEXT = 6


def main():
    ram = bytearray(open(RAM, "rb").read())
    word = lambda a: struct.unpack_from("<I", ram, a & 0x1FFFFF)[0]
    patched = bytearray(ram)

    def put32(a, v):
        struct.pack_into("<I", patched, a & 0x1FFFFF, v)

    # The code
    assert word(0x80013024) == 0x30420080 and word(0x80013030) == 0xAE710000      # andi v0,v0,0x80 / sw s1,(s3)
    put32(0x80013024, 0x2402FFFD)       # addiu v0,zero,-3
    put32(0x80013028, 0xAE620000)       # sw v0,0(s3)
    put32(0x8001302C, 0)
    put32(0x80013030, 0)
    regimm = lambda rs, rt, orig: (1 << 26) | (rs << 21) | (rt << 16) | (orig & 0xFFFF)
    assert word(0x80013054) >> 16 == 0x1682                                         # bne s4,v0
    put32(0x80013054, regimm(20, 1, word(0x80013054)))                              # bgez s4
    for a, bne in ((0x8001AAAC, True), (0x8001AB10, False), (0x8001AB8C, True), (0x8001ABEC, False)):
        assert word(a) >> 16 == (0x1445 if bne else 0x1045), hex(a)                 # bne/beq v0,a1
        put32(a, regimm(2, 1 if bne else 0, word(a)))                               # bgez/bltz v0

    # The texts
    u16 = lambda a: struct.unpack_from("<H", ram, a & 0x1FFFFF)[0]
    for lang, s in LANGS.items():
        link = s + u16(s + 2 * LINK_TEXT)
        old = bytes(ram[link & 0x1FFFFF:]).split(b"\0")[0]
        if len(old) == len(NAME):
            patched[link & 0x1FFFFF:(link & 0x1FFFFF) + len(NAME)] = NAME
        else:
            first = s + u16(s)
            room = len(bytes(ram[first & 0x1FFFFF:]).split(b"\0")[0])
            assert room >= len(NAME), lang
            patched[first & 0x1FFFFF:(first & 0x1FFFFF) + len(NAME) + 1] = NAME + b"\0"
            struct.pack_into("<H", patched, (s + 2 * LINK_TEXT) & 0x1FFFFF, u16(s))
        print("%s: %s -> %s" % (lang, old.decode("latin-1"), NAME.decode()))

    changed = [0x80000000 + a for a in range(0, len(ram), 4) if ram[a:a + 4] != patched[a:a + 4]]
    sets = ["%08X=%08X" % (a, struct.unpack_from("<I", patched, a & 0x1FFFFF)[0]) for a in changed]
    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)
    for a in changed:
        if a < 0x80020000:
            new = next(md.disasm(bytes(patched[a & 0x1FFFFF:(a & 0x1FFFFF) + 4]), a), None)
            print("  %08X: %s %s" % (a, new.mnemonic, new.op_str) if new else "  %08X: nop" % a)
    # The checks: first the idle timer's instruction as found, alone (written last, so a pass broken off is done
    # again): one read a second once done or away from the menus, as each read stops the console's game for a moment
    # (2026-11-17: 9 words read each second on both consoles through the link setup and the game, the link broke 3
    # times in 3); then words of the overlay never written (the main menu's code near the idle timer, a text of each
    # language), read only when the menus were just loaded again
    checks = [0x80013388, 0x80013390, 0x800133A8, 0x80013010, 0x8001A934]
    for s in LANGS.values():
        a = (s + u16(s + 2)) & ~3          # (text 1's first word: MISSIONS...)
        checks.append(a if a not in changed else a + 4)
    assert not set(checks) & set(changed)
    steps = ["w8001338C=12C20006", "w" + "/".join("%08X=%08X" % (a, word(a)) for a in checks)]
    for k in range(0, len(sets), 12):
        steps.append("s" + "/".join(sets[k:k + 12]))
    steps.append("s8001338C=00000000")
    print("%d words written, %d steps" % (len(sets) + 1, len(steps)))
    print(",".join(steps))
    if "--ram" in sys.argv:
        put32(0x8001338C, 0)
        open(sys.argv[sys.argv.index("--ram") + 1], "wb").write(patched)


if __name__ == "__main__":
    main()
