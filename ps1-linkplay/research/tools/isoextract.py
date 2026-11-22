import sys,struct,os
bin_,out=sys.argv[1],sys.argv[2]
f=open(bin_,'rb')
def sec(n):
    f.seek(n*2352); d=f.read(2352)
    return d[24:24+2048]
def rd(lba,size):
    r=b''
    for i in range((size+2047)//2048): r+=sec(lba+i)
    return r[:size]
pvd=sec(16)
root=pvd[156:190]
lba=struct.unpack('<I',root[2:6])[0]; sz=struct.unpack('<I',root[10:14])[0]
def ls(lba,sz):
    d=rd(lba,sz);p=0;res=[]
    while p<len(d):
        l=d[p]
        if l==0:
            p=(p//2048+1)*2048; continue
        L=struct.unpack('<I',d[p+2:p+6])[0];S=struct.unpack('<I',d[p+10:p+14])[0]
        fl=d[p+25];n=d[p+33:p+33+d[p+32]].decode('latin1')
        res.append((n,L,S,fl)); p+=l
    return res
ents=ls(lba,sz)
os.makedirs(out,exist_ok=True)
for n,L,S,fl in ents:
    print(n,L,S,fl)
    if not fl&2 and S<8_000_000 :
        open(os.path.join(out,n.split(';')[0]),'wb').write(rd(L,S))
