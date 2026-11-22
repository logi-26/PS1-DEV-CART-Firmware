import struct
from capstone import *
d=open('ra/SLES_009.49','rb').read()
md=Cs(CS_ARCH_MIPS,CS_MODE_MIPS32|CS_MODE_LITTLE_ENDIAN)
w=struct.unpack('<%dI'%((len(d)-0x800)//4),d[0x800:0x800+((len(d)-0x800)//4)*4])
T={0x80155a4c:'sio_control',0x801554ec:'sio_read',0x801557d0:'sio_write',0x8015548c:'sio_read_async?',0x8015576c:'sio_write_async?',0x80155300:'sio_init',0x80155048:'f48',0x8015507c:'f7c'}
for t,n in T.items():
    if t in(0x80155048,0x8015507c): continue
    cs=[i for i,x in enumerate(w) if x>>26==3 and (0x80000000|((x&0x3FFFFFF)<<2))==t]
    print('==',n,hex(t),[hex(0x80010000+i*4) for i in cs])
    if n=='sio_control' or len(cs)<6:
        for i in cs:
            a=0x80010000+i*4; o=i*4+0x800
            print('  --',hex(a))
            for ins in md.disasm(d[o-28:o+8],a-28):
                if ins.mnemonic!='nop': print('    %x %s %s'%(ins.address,ins.mnemonic,ins.op_str))
