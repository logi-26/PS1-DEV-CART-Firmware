"""raminfo.py RAM...: CTR race settings in RAM dumps (dd_auto.lua dump()): mode flags, level, players, laps,
characters, battle setup, cup."""
import struct
import sys

for path in sys.argv[1:]:
    r = open(path, "rb").read()

    def u32(a):
        return struct.unpack_from("<I", r, a & 0x1FFFFF)[0]

    def u8(a):
        return r[a & 0x1FFFFF]

    g = u32(0x8008D2AC)
    print(path)
    print("  gGT %08x mode1 %08x mode2 %08x level %d currLEV %d players %d/%d bots %d/%d laps %d"
          % (g, u32(g), u32(g + 8), u32(g + 0x1A10), u32(g + 0x1EB0), u8(g + 0x1CA8), u8(g + 0x1CA9),
             u8(g + 0x1CAA), u8(g + 0x1CAB), u8(g + 0x1D33)))
    print("  characters", struct.unpack_from("<8h", r, 0x86E84))
    name = r[(g + 0x1A14) & 0x1FFFFF:(g + 0x1A14 + 0x24) & 0x1FFFFF].split(b"\0")[0]
    print("  level name", name)
    print("  battle: lifeLimit %d killLimit %d eventTime %d weapons %x teams %s flags %x numTeams %d numWeapons %d"
          % (u32(g + 0x1D88), u32(g + 0x1D8C), u32(g + 0x1D84), u32(g + 0x1DA0),
             list(r[(g + 0x1DA4) & 0x1FFFFF:(g + 0x1DA8) & 0x1FFFFF]), u32(g + 0x1DD8), u32(g + 0x1DDC),
             u32(g + 0x1DF0)))
    print("  cup %d track %d difficulty %d" % (u32(g + 0x1E58), u32(g + 0x1E5C), u32(g + 0x1EBC)))
