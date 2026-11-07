import re,sys,subprocess
import os
O=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
def listing(rec,img,lo,hi):
    out=subprocess.run(['python3','-I',O+'/tools/vu1/vudis.py',rec,'--image',img,'--all','--from',lo,'--to',hi],capture_output=True,text=True,cwd=O).stdout
    pairs={}
    for l in out.splitlines():
        m=re.match(r'([0-9a-f]+)\s+\d*\s*(I)?\s*(.*?)\s*\|\s*(.*)$',l)
        if m: pairs[int(m.group(1),16)]=(m.group(3).strip(),m.group(4).strip())
    return pairs
C='xyzw'
class S:
    def __init__(s):
        s.vf=[[ '0','0','0','1' ] if i==0 else ['%s%d%s'%('vf',i,c) for c in C] for i in range(32)]
        s.vf[0]=['0','0','0','1']
        s.vi={0:'0'}; s.P=s.Q=s.I='?'; s.pP=s.pQ=None; s.acc=['0']*4; s.stores=[]
    def src(s,tok):
        if tok=='q': return [s.Q]*4
        if tok=='i': return [s.I]*4
        m=re.match(r'vf(\d+)([xyzw])?$',tok); r=int(m.group(1))
        return [s.vf[r][C.index(m.group(2))]]*4 if m.group(2) else list(s.vf[r])
    def dest(s,r,mask,vals):
        new=list(s.vf[r])
        for c in mask: new[C.index(c)]=vals[C.index(c)]
        return new
def run(pairs,order,st):
    ret=None
    pend=[]
    for pc in order:
        up,lo=pairs[pc]
        wr=[]
        if lo.startswith('WAITQ') and st.pQ is not None: st.Q=st.pQ; st.pQ=None
        if lo.startswith('WAITP') and st.pP is not None: st.P=st.pP; st.pP=None
        # upper
        ma=re.match(r'(MULA|MADDA|MADD)(x|y|z|w|q|i)?\.(\w+)\s+(ACC|vf\d+),\s*(\S+),\s*(\S+)$',up)
        m=re.match(r'(ADD|SUB|MUL|MAX)(x|y|z|w|q|i)?\.(\w+)\s+vf(\d+),\s*(\S+),\s*(\S+)$',up)
        if ma:
            op,bc,mask,fd,a,b=ma.groups(); A=st.src(a); B=st.src(b)
            prod=['(%s*%s)'%(A[i],B[i]) for i in range(4)]
            if op=='MULA': newacc=prod
            else: newacc=['(%s+%s)'%(st.acc[i],prod[i]) for i in range(4)]
            if fd=='ACC':
                st.accpend=(mask,newacc)
            else:
                wr.append((int(fd[2:]),mask,newacc))
        elif m:
            op,bc,mask,fd,a,b=m.groups(); A=st.src(a); B=st.src(b)
            sym={'ADD':'+','SUB':'-','MUL':'*'}.get(op)
            if op=='MAX': vals=['max(%s,%s)'%(A[i],B[i]) for i in range(4)]
            else: vals=['(%s%s%s)'%(A[i],sym,B[i]) for i in range(4)]
            wr.append((int(fd),mask,vals))
        elif up!='NOP': print('UP?',up)
        if getattr(st,'accpend',None):
            mk,na=st.accpend; st.accpend=None
            for c in mk: st.acc[C.index(c)]=na[C.index(c)]
        # lower
        if lo=='NOP' or lo.startswith('JR'): pass
        elif lo.startswith('LQI'):
            m=re.match(r'LQI\.(\w+)\s+vf(\d+),\s*\(vi(\d+)\+\+\)',lo); mask,fd,vi=m.groups()
            base=st.vi[int(vi)]
            wr.append((int(fd),mask,['mem[%s].%s'%(base,c) for c in C]))
            wr.append(('vi',int(vi),'%s+1'%base))
        elif lo.startswith('ERLENG'):
            m=re.match(r'ERLENG P, vf(\d+)',lo); a=int(m.group(1)); v=st.vf[a]
            wr.append(('pP','(1/sqrt(%s*%s+%s*%s+%s*%s))'%(v[0],v[0],v[1],v[1],v[2],v[2])))
        elif lo.startswith('LQ'):
            m=re.match(r'LQ\.(\w+)\s+vf(\d+),\s*(-?\d+)\(vi(\d+)\)',lo); mask,fd,off,vi=m.groups()
            base=st.vi[int(vi)]
            addr='%s+%s'%(base,off) if base!='0' else off
            wr.append((int(fd),mask,['mem[%s].%s'%(addr,c) for c in C]))
        elif lo.startswith('ILW'):
            m=re.match(r'ILW\.(\w)\s+vi(\d+),\s*(-?\d+)\(vi0\)',lo); c,vi,off=m.groups()
            wr.append(('vi',int(vi),'int(mem[%s].%s)'%(off,c)))
        elif lo.startswith('DIV'):
            m=re.match(r'DIV Q, vf(\d+)([xyzw]), vf(\d+)([xyzw])',lo); a,ac,b,bc=m.groups()
            wr.append(('pQ','(%s/%s)'%(st.vf[int(a)][C.index(ac)],st.vf[int(b)][C.index(bc)])))
        elif lo.startswith('WAITP'):
            if st.pP is not None: st.P=st.pP; st.pP=None
        elif lo.startswith('WAITQ'):
            if st.pQ is not None: st.Q=st.pQ; st.pQ=None
        elif lo.startswith('LOI'): wr.append(('I',lo.split()[1]))
        elif lo.startswith('ESIN'):
            m=re.match(r'ESIN P, vf(\d+)([xyzw])',lo); a,ac=m.groups()
            wr.append(('pP','sin(%s)'%st.vf[int(a)][C.index(ac)]))
        elif lo.startswith('MFP'):
            m=re.match(r'MFP\.(\w+)\s+vf(\d+), P',lo); mask,fd=m.groups(); wr.append((int(fd),mask,[st.P]*4))
        elif lo.startswith('MOVE'):
            m=re.match(r'MOVE\.(\w+)\s+vf(\d+), vf(\d+)',lo); mask,fd,fs=m.groups(); wr.append((int(fd),mask,list(st.vf[int(fs)])))
        elif lo.startswith('IADD'):
            m=re.match(r'IADD vi(\d+), vi(\d+), vi(\d+)',lo); d,a,b=m.groups(); wr.append(('vi',int(d),'(%s+%s)'%(st.vi[int(a)],st.vi[int(b)])))
        elif lo.startswith('SQI'):
            m=re.match(r'SQI\.(\w+)\s+vf(\d+), \(vi(\d+)\+\+\)',lo); mask,fs,vi=m.groups()
            st.stores.append(('store[%s]'%st.vi[int(vi)],mask,[st.vf[int(fs)][C.index(c)] for c in mask]))
            wr.append(('vi',int(vi),'%s+1'%st.vi[int(vi)]))
        elif lo.startswith('IBNE') or lo.startswith('IBEQ'): pass
        else: print('LO?',lo)
        for w in wr:
            if w[0]=='vi': st.vi[w[1]]=w[2]
            elif w[0]=='pQ':
                if st.pQ is not None: st.Q=st.pQ
                st.pQ=w[1]
            elif w[0]=='pP':
                if st.pP is not None: st.P=st.pP
                st.pP=w[1]
            elif w[0]=='I': st.I=w[1]
            else: st.vf[w[0]]=st.dest(w[0],w[1],w[2])
if __name__=='__main__':
    rec,img=sys.argv[1],sys.argv[2]; lo,hi=sys.argv[3],sys.argv[4]
    P=listing(rec,img,lo,hi)
    st=S()
    pcs=sorted(P)
    seq=[]
    for seg in sys.argv[5:]:
        a,b=seg.split('-'); seq+= [p for p in pcs if int(a,16)<=p<=int(b,16)]
    run(P,seq,st)
    for s_ in st.stores: print(s_[0],s_[1],s_[2])
