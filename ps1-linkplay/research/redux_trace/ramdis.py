import sys, struct
from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN
r = open(sys.argv[3] if len(sys.argv) > 3 else ".menu/menu.ram", "rb").read()
a, b = int(sys.argv[1], 16), int(sys.argv[2], 16)
md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)
for i in md.disasm(r[a & 0x1fffff:b & 0x1fffff], a):
    print("%x: %s %s" % (i.address, i.mnemonic, i.op_str))
