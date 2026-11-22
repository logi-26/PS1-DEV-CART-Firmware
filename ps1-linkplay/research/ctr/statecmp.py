"""statecmp.py RAM...: compare CTR's game state in RAM dumps taken at the same game frame on different emulators:
each driver's struct (gGT+0x24EC drivers[8], 0x638 bytes each: position, speed, items, laps...) and the battle /
race totals in gGT. Prints the differing bytes per driver (first dump = reference)."""
import struct
import sys

DRIVER_SIZE = 0x638


def load(path):
    r = open(path, "rb").read()
    u32 = lambda a: struct.unpack_from("<I", r, a & 0x1FFFFF)[0]
    g = u32(0x8008D2AC)
    drivers = []
    for i in range(8):
        p = u32(g + 0x24EC + 4 * i)
        drivers.append(r[p & 0x1FFFFF:(p & 0x1FFFFF) + DRIVER_SIZE] if p else None)
    totals = r[(g + 0x1D80) & 0x1FFFFF:(g + 0x1EC8) & 0x1FFFFF]
    pos = [struct.unpack_from("<3i", d, 0x2D4) if d else None for d in drivers]
    return drivers, totals, pos


ref = load(sys.argv[1])
print(sys.argv[1], "positions", [p for p in ref[2] if p])
for path in sys.argv[2:]:
    cur = load(path)
    out = []
    for i, (a, b) in enumerate(zip(ref[0], cur[0])):
        if a is None or b is None:
            continue
        diff = [o for o in range(DRIVER_SIZE) if a[o] != b[o]]
        if diff:
            out.append("driver %d: %d bytes at %s" % (i, len(diff), " ".join("%x" % o for o in diff[:12])))
    tdiff = [o for o in range(len(ref[1])) if ref[1][o] != cur[1][o]]
    if tdiff:
        out.append("totals: %d bytes at gGT+%s" % (len(tdiff), " ".join("%x" % (0x1D80 + o) for o in tdiff[:12])))
    print(path, "SAME" if not out else "\n  " + "\n  ".join(out))
