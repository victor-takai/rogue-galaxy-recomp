import re,sys,subprocess,os,collections
import os
R=os.environ.get('RGDECOMP_SRC','RGDecomp/game/src')  # a checkout of RGDecomp (port branch)
O='/home/nathan/Projects/rogue-galaxy-recomp-oracle'
# census: address(+0x10 of pointer) -> (slot, hash8, runs)
out=subprocess.run(['python3','-I',O+'/tools/vu1/census.py','--recording']+[O+'/build/harvest/'+f for f in os.listdir(O+'/build/harvest') if f.endswith('.vu1r')],capture_output=True,text=True,cwd=O).stdout
cen={}
for l in out.splitlines():
    m=re.match(r'(base|[A-F])\s+0x\w+\s+\d+\s+0x([0-9a-f]{8})\s+([0-9a-f]{16})\s+(\S+)',l)
    if m: cen[int(m.group(2),16)]=(m.group(1),m.group(3)[:8],m.group(4))
def piece(ptr):
    a=ptr+0x10
    return cen.get(a,(None,'%x?'%ptr,None))
def sym(n): return int(n[2:],16)
def arr(name):
    f=f'{R}/main_data/data2c_{name[2:]}.c'
    if not os.path.exists(f): return None
    s=open(f).read()
    m=re.search(r'= \{(.*?)\};',s,re.S)
    vals=[]
    for t in re.findall(r'\(u32\)(?:\(\(u8 ?\*\)&(D_\w+) \+ (0x\w+)u?\)|&(D_\w+))',m.group(1)):
        if t[0]: vals.append(sym(t[0])+int(t[1].rstrip('u'),16))
        else: vals.append(sym(t[2]))
    return vals
src=open(R+'/main/gfx/VuMicrocodeTableInitMaybe_152830.c').read()
a={};b={}
def cp(dst,srcs):
    for j in range(12):
        for k in range(4):
            a[(dst,j,k)]=a.get((srcs,j,k)); b[(dst,j,k)]=b.get((srcs,j,k))
for l in src.splitlines():
    m=re.match(r'\s*p->([ab])\[(\d)\]\[(\d+)\]\[(\d)\] = (&?)(D_\w+);',l)
    if m:
        t,s,j,k,amp,n=m.groups(); s,j,k=int(s),int(j),int(k)
        (a if t=='a' else b)[(s,j,k)]=(sym(n),bool(amp)); continue
    m=re.search(r'p->a\[(\d)\]\[j|c1|c3\]\[\w+\] = p->a\[0\]',l)
    m=re.search(r'p->a\[([123])\]\[\w+\]\[\w+\] = p->a\[0\]',l)
    if m:
        d=int(m.group(1))
        for j in range(12):
            for k in range(4):
                a[(d,j,k)]=a.get((0,j,k)); b[(d,j,k)]=b.get((0,j,k))
unseen=set(sys.argv[1:])
res=collections.defaultdict(list)
for (s,j,k),v in sorted(a.items()):
    if v is None: continue
    p=piece(v[0])
    if p[1] in unseen or p[2]=='-': res[p[1]].append(f'set {s} row {j} slot a[{k}]')
for (s,j,k),v in sorted(b.items()):
    if v is None: continue
    ptr,amp=v
    g=arr('D_%08X'%ptr)
    if g is None: continue
    # b[set][row][k]: k=0 -> lod group (info[3]), 1 -> k4 (info[4]) , 2 -> k5 (info[5])
    for i,pp in enumerate(g):
        p=piece(pp)
        if p[1] in unseen or p[2]=='-': res[p[1]].append(f'set {s} row {j} group b[{k}] index {i}')
for h in sorted(res):
    c=collections.Counter()
    sets=sorted(set(x.split()[1] for x in res[h]))
    print(h,len(res[h]),'refs; sets',','.join(sets))
    seen=set()
    for x in res[h]:
        key=' '.join(x.split()[2:])
        if key in seen: continue
        seen.add(key)
        if len(seen)<=6: print('   ',x)
