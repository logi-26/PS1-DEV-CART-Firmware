"""Destruction Derby (SCUS_943.02) helpers: callers of a function, disassembly of a range of the executable."""
import struct, sys
from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN
d = open(__file__.rsplit("\\", 1)[0].rsplit("/", 1)[0] + "/disc/SCUS_943.02", "rb").read()
LOAD = 0x80010000
body = d[0x800:]
W = struct.unpack("<%dI" % (len(body) // 4), body[:len(body) // 4 * 4])
md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)

def callers(t):
    return [LOAD + 4 * i for i, w in enumerate(W) if w >> 26 == 3 and (0x80000000 | ((w & 0x3FFFFFF) << 2)) == t]

def dis(a, b):
    for i in md.disasm(body[a - LOAD:b - LOAD], a):
        print("%x: %s %s" % (i.address, i.mnemonic, i.op_str))

if __name__ == "__main__":
    if sys.argv[1] == "callers":
        for t in sys.argv[2:]:
            print(t, [hex(c) for c in callers(int(t, 16))])
    else:
        dis(int(sys.argv[2], 16), int(sys.argv[3], 16))
