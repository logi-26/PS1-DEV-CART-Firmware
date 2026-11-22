"""ramdiff.py PREFIX N: .menu/PREFIX0.ram .. PREFIX<N>.ram; words whose values step by a constant (a cursor)."""
import os, struct, sys
D = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".menu")
p, n = sys.argv[1], int(sys.argv[2])
rams = [open(os.path.join(D, "%s%d.ram" % (p, i)), "rb").read() for i in range(n + 1)]
for size, fmt in ((4, "<i"), (2, "<h"), (1, "<b")):
    for off in range(0, 0x200000, size):
        v = [struct.unpack_from(fmt, r, off)[0] for r in rams]
        d = [v[i + 1] - v[i] for i in range(n)]
        if len(set(v)) >= 3 and all(abs(x) <= 0x40 for x in d) and len(set(d)) <= 2 and 0 not in d[:2]:
            print("%08x size %d: %s" % (0x80000000 + off, size, v))
