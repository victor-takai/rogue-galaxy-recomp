import sys,struct,collections,json
sys.path.insert(0,__import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from hd6 import HD6
D='/home/nathan/Projects/rogue-galaxy-recomp/disc/'
MAGIC=b' MDS'
out=[]   # (archive, file, mds_off, idx, type, flags, m52)
nfiles=nmds=0
for arc in sys.argv[1:]:
    h=HD6(D+arc+'.HD6'); dat=open(D+arc+'.DAT','rb')
    for name,off,size in h.entries:
        if not name or size<0x200: continue
        ext=name.rsplit('.',1)[-1].lower()
        if ext in ('wav','snd','stb','img','mvi','mwv','txt'): continue
        dat.seek(off); b=dat.read(size); nfiles+=1
        i=0
        while True:
            i=b.find(MAGIC,i)
            if i<0: break
            if i%4==0 and i+0x40<=len(b):
                hdr=struct.unpack_from('<I',b,i+4)[0]
                cnt,stride,base=struct.unpack_from('<III',b,i+0x2c)
                if hdr==0xc8 and stride==0x60 and 0<cnt<2000 and i+base+cnt*0x60<=len(b):
                    nmds+=1
                    for k in range(cnt):
                        d=i+base+k*0x60
                        t,fl=struct.unpack_from('<II',b,d+0x40)
                        out.append((arc,name,i,k,t,fl,b[d+0x52]))
            i+=4
print(nfiles,'files scanned',nmds,'mds',len(out),'materials',file=sys.stderr)
json.dump(out,open(sys.argv[-1] if sys.argv[-1].endswith('.json') else 'mats.json','w'))
c=collections.Counter(t for *_,t,fl,m in out)
print(sorted(c.items()))
