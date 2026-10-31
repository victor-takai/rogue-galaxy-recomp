import sys,struct,collections,json
sys.path.insert(0,__import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from hd6 import HD6
D='/home/nathan/Projects/rogue-galaxy-recomp/disc/'
hd=HD6(D+'DATA0_0.HD6'); dat=open(D+'DATA0_0.DAT','rb')
c=collections.Counter(); ex=collections.defaultdict(set); bad=0; total=0
for n,o,s in hd.entries:
    if not n or s<0x200: continue
    if n.rsplit('.',1)[-1].lower() in ('wav','snd','stb','img','mvi','mwv','txt'): continue
    dat.seek(o); b=dat.read(s); i=0
    while True:
        i=b.find(b' MDS',i)
        if i<0: break
        if i%4==0 and i+0x40<=len(b) and struct.unpack_from('<I',b,i+4)[0]==0xc8:
            num,first=struct.unpack_from('<II',b,i+0x20)
            p=i+first; ok=True
            for k in range(min(num,64)):
                if p+0x84>len(b) or b[p:p+4]!=b' MDT': ok=False; break
                cls,=struct.unpack_from('<i',b,p+0x68); mode,=struct.unpack_from('<H',b,p+0x6c)
                nxt,=struct.unpack_from('<i',b,p+0x80)
                c[(mode,cls)]+=1
                if len(ex[(mode,cls)])<6: ex[(mode,cls)].add(n)
                total+=1
                if nxt==0: break
                p+=nxt
            if not ok: bad+=1
        i+=4
print('nodes',total,'bad chains',bad)
for k,v in sorted(c.items(),key=lambda x:-x[1]): print('mode',k[0],'class',k[1],v,sorted(ex[k])[:5])
