"""A PS-X EXE's code, for research (pad play: games' pad and random number routines).
  python exetool.py EXE info                  header, BIOS calls (A0/B0/C0 nn: the stubs), library strings
  python exetool.py EXE callers ADDR...       the jal's to each address
  python exetool.py EXE dis FROM TO           disassembly
  python exetool.py EXE refs ADDR             lui/addiu or lui/load-store pairs that make the address (in 0x40 words)
  python exetool.py EXE find HEXWORD...       where these 32-bit words are
"""
import re
import struct
import sys

from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN


class Exe:
    def __init__(self, path):
        d = open(path, "rb").read()
        self.pc = struct.unpack_from("<I", d, 0x10)[0]
        self.load = struct.unpack_from("<I", d, 0x18)[0]
        self.body = d[0x800:]
        n = len(self.body) // 4
        self.w = struct.unpack("<%dI" % n, self.body[:n * 4])
        self.md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)

    def adr(self, i):
        return self.load + 4 * i

    def bios_calls(self):
        calls = {}
        for i, w in enumerate(self.w[:-2]):
            if w in (0x240A00A0, 0x240A00B0, 0x240A00C0):
                for j in (i + 1, i + 2):
                    if self.w[j] >> 16 == 0x2409:
                        calls.setdefault("%X%02X" % (w & 0xFF, self.w[j] & 0xFFFF), []).append(self.adr(i))
        return calls

    def callers(self, t):
        return [self.adr(i) for i, w in enumerate(self.w)
                if w >> 26 == 3 and (0x80000000 | ((w & 0x3FFFFFF) << 2)) == t]

    def dis(self, a, b):
        for i in self.md.disasm(self.body[a - self.load:b - self.load], a):
            print("%x: %s %s" % (i.address, i.mnemonic, i.op_str))

    def refs(self, target):
        hi, lo = (target + 0x8000) >> 16 & 0xFFFF, target & 0xFFFF
        out = []
        for i, w in enumerate(self.w):
            if w >> 26 == 0x0F and (w & 0xFFFF) == hi:
                r = (w >> 16) & 31
                for j in range(i + 1, min(i + 0x40, len(self.w))):
                    x = self.w[j]
                    if (x >> 21) & 31 == r and (x & 0xFFFF) == lo and x >> 26 in (0x09, 0x20, 0x21, 0x23, 0x24,
                                                                                  0x25, 0x28, 0x29, 0x2B):
                        out.append(self.adr(j))
                        break
        return out


def main():
    e = Exe(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "info":
        print("pc %08X load %08X size %X" % (e.pc, e.load, len(e.body)))
        for k, v in sorted(e.bios_calls().items()):
            print(k, " ".join("%08X" % a for a in v[:4]))
        for m in re.finditer(rb"[ -~]{6,}", e.body):
            s = m.group().decode()
            if re.search(r"(?i)pad|rand|library|sony|psy-q|psyq|version|tap|vsync", s):
                print("%08X %s" % (e.load + m.start(), s[:80]))
    elif cmd == "callers":
        for t in sys.argv[3:]:
            print(t, " ".join("%08X" % c for c in e.callers(int(t, 16))))
    elif cmd == "dis":
        e.dis(int(sys.argv[3], 16), int(sys.argv[4], 16))
    elif cmd == "refs":
        print(" ".join("%08X" % a for a in e.refs(int(sys.argv[3], 16))))
    elif cmd == "find":
        for h in sys.argv[3:]:
            v = int(h, 16)
            print(h, " ".join("%08X" % e.adr(i) for i, w in enumerate(e.w) if w == v))


if __name__ == "__main__":
    main()
