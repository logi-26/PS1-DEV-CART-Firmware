import sys
from capstone import *
d=open('ra/SLES_009.49','rb').read()
a,b=int(sys.argv[1],16),int(sys.argv[2],16)
md=Cs(CS_ARCH_MIPS,CS_MODE_MIPS32|CS_MODE_LITTLE_ENDIAN)
o=a-0x80010000+0x800
for i in md.disasm(d[o:o+(b-a)],a): print('%x: %s %s'%(i.address,i.mnemonic,i.op_str))
