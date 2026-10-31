#!/usr/bin/env python3
"""Straight-line symbolic run of VU1 code, printed as named temporaries.

  ssa.py CODE.bin LO HI RANGE... [--mem NAME=...]

CODE is a 16 KiB micro-memory image (tools/vu1/piece.py). RANGEs ("10e0-141f
1420-15e0 ...", byte addresses, inclusive) are the pairs to execute in order,
a loop listed once per iteration. Registers start as vfNx etc., data memory
reads are `m[addr].c`, vi registers hold expressions. P and Q results become
visible at WAITP/WAITQ or the next issue of that unit, as the VU does. Stores
are printed at the end; temporaries (tN) are listed with what they compute.
"""
import re, sys, subprocess, os
O = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
C = 'xyzw'
defs = []
def mk(e):
    if len(e) <= 28 or re.fullmatch(r'[\w.\[\]+\-]+', e):
        return e
    n = 't%d' % len(defs)
    defs.append((n, e))
    return n
def listing(code, lo, hi):
    out = subprocess.run(['python3', '-I', O + '/tools/vu1/vudis.py', code, '--all', '--from', lo, '--to', hi],
                         capture_output=True, text=True, cwd=O).stdout
    pairs = {}
    for l in out.splitlines():
        m = re.match(r'([0-9a-f]+)\s+\d*\s*(I(?=\s))?\s*(.*?)\s*\|\s*(.*)$', l)
        if m: pairs[int(m.group(1), 16)] = (m.group(3).strip(), m.group(4).strip())
    return pairs
class S:
    def __init__(s):
        s.vf = [['vf%d%s' % (i, c) for c in C] for i in range(32)]
        s.vf[0] = ['0', '0', '0', '1']
        s.vi = {i: 'vi%d' % i for i in range(16)}; s.vi[0] = '0'
        s.P = s.Q = s.I = '?'; s.pP = s.pQ = None; s.acc = ['0'] * 4; s.stores = []
    def src(s, tok):
        if tok == 'q': return [s.Q] * 4
        if tok == 'i': return [s.I] * 4
        if tok == 'ACC': return list(s.acc)
        m = re.match(r'vf(\d+)([xyzw])?$', tok)
        r = int(m.group(1))
        return [s.vf[r][C.index(m.group(2))]] * 4 if m.group(2) else list(s.vf[r])
    def put(s, r, mask, vals):
        for c in mask: s.vf[r][C.index(c)] = mk(vals[C.index(c)])
def address(s, base, off):
    b = s.vi[int(base)] if base != '0' else '0'
    return '%s+%s' % (b, off) if b != '0' else str(off)
UPPER = re.compile(r'(MULA|MADDA|ADDA|SUBA|MSUBA|MADD|MSUB|ADD|SUB|MUL|MAX|MINI)(x|y|z|w|q|i)?\.(\w+)\s+(ACC|vf\d+),\s*(\S+?)(?:,\s*(\S+))?$')
def run(pairs, order, st):
    for pc in order:
        up, lo = pairs[pc]
        wr = []
        if lo.startswith('WAITQ') and st.pQ is not None: st.Q = st.pQ; st.pQ = None
        if lo.startswith('WAITP') and st.pP is not None: st.P = st.pP; st.pP = None
        # upper
        m = None
        mm = re.match(r'(MULA|MADDA|ADDA|SUBA|MSUBA|MADD|MSUB|ADD|SUB|MUL|MAX|MINI)(x|y|z|w|q|i)?\.(\w+)\s+(ACC|vf\d+),\s*(\S+),\s*(\S+)$', up)
        acc_new = None
        if mm:
            op, bc, mask, fd, a, b = mm.groups()
            A = st.src(a); B = st.src(b)
            if bc: B = [B[C.index(bc)]] * 4 if bc in C else B
            if bc in ('q', 'i'): B = st.src(bc)
            prod = ['(%s*%s)' % (A[i], B[i]) for i in range(4)]
            base = op.rstrip('A') if op.endswith('A') and op not in ('MADD',) else op
            if op in ('MULA', 'MUL'): val = prod
            elif op in ('MADDA', 'MADD'): val = ['(%s+%s)' % (st.acc[i], prod[i]) for i in range(4)]
            elif op in ('MSUBA', 'MSUB'): val = ['(%s-%s)' % (st.acc[i], prod[i]) for i in range(4)]
            elif op in ('ADDA', 'ADD'): val = ['(%s+%s)' % (A[i], B[i]) for i in range(4)]
            elif op in ('SUBA', 'SUB'): val = ['(%s-%s)' % (A[i], B[i]) for i in range(4)]
            elif op == 'MAX': val = ['max(%s,%s)' % (A[i], B[i]) for i in range(4)]
            elif op == 'MINI': val = ['min(%s,%s)' % (A[i], B[i]) for i in range(4)]
            if op.endswith('A') and op not in ('MADD',) :
                acc_new = (mask, val)
            else:
                wr.append((int(fd[2:]), mask, val))
        elif re.match(r'ITOF(\d+)\.(\w+)\s+vf(\d+),\s*vf(\d+)$', up):
            k, mask, fd, fs = re.match(r'ITOF(\d+)\.(\w+)\s+vf(\d+),\s*vf(\d+)$', up).groups()
            wr.append((int(fd), mask, ['itof%s(%s)' % (k, v) for v in st.vf[int(fs)]]))
        elif up != 'NOP' and not up.startswith('FTOI') and not up.startswith('CLIP'):
            print('UP?', up)
        if acc_new:
            mask, val = acc_new
            for c in mask: st.acc[C.index(c)] = mk(val[C.index(c)])
        # lower
        if lo == 'NOP' or lo.startswith(('JR', 'IBNE', 'IBEQ', 'WAIT', 'IAND')): pass
        elif lo.startswith('LQI'):
            m = re.match(r'LQI\.(\w+)\s+vf(\d+),\s*\(vi(\d+)\+\+\)', lo); mask, fd, vi = m.groups()
            base = st.vi[int(vi)]
            wr.append((int(fd), mask, ['m[%s].%s' % (base, c) for c in C]))
            wr.append(('vi', int(vi), '%s+1' % base))
        elif lo.startswith('LQ'):
            m = re.match(r'LQ\.(\w+)\s+vf(\d+),\s*(-?\d+)\(vi(\d+)\)', lo); mask, fd, off, vi = m.groups()
            wr.append((int(fd), mask, ['m[%s].%s' % (address(st, vi, off), c) for c in C]))
        elif lo.startswith('ILW'):
            m = re.match(r'ILW\.(\w)\s+vi(\d+),\s*(-?\d+)\(vi(\d+)\)', lo); c, vi, off, base = m.groups()
            wr.append(('vi', int(vi), 'int(m[%s].%s)' % (address(st, base, off), c)))
        elif lo.startswith('DIV'):
            m = re.match(r'DIV Q, vf(\d+)([xyzw]), vf(\d+)([xyzw])', lo); a, ac, b, bc = m.groups()
            wr.append(('pQ', '(%s/%s)' % (st.vf[int(a)][C.index(ac)], st.vf[int(b)][C.index(bc)])))
        elif lo.startswith('RSQRT'):
            m = re.match(r'RSQRT Q, vf(\d+)([xyzw]), vf(\d+)([xyzw])', lo); a, ac, b, bc = m.groups()
            wr.append(('pQ', '(%s/sqrt(abs(%s)))' % (st.vf[int(a)][C.index(ac)], st.vf[int(b)][C.index(bc)])))
        elif lo.startswith('ERSQRT'):
            m = re.match(r'ERSQRT P, vf(\d+)([xyzw])', lo); a, ac = m.groups()
            wr.append(('pP', '(1/sqrt(abs(%s)))' % st.vf[int(a)][C.index(ac)]))
        elif lo.startswith('SQRT'):
            m = re.match(r'SQRT Q, vf(\d+)([xyzw])', lo); a, ac = m.groups()
            wr.append(('pQ', 'sqrt(abs(%s))' % st.vf[int(a)][C.index(ac)]))
        elif lo.startswith('ERLENG'):
            m = re.match(r'ERLENG P, vf(\d+)', lo); v = st.vf[int(m.group(1))]
            wr.append(('pP', '(1/sqrt(%s*%s+%s*%s+%s*%s))' % (v[0], v[0], v[1], v[1], v[2], v[2])))
        elif lo.startswith('ESIN'):
            m = re.match(r'ESIN P, vf(\d+)([xyzw])', lo); a, ac = m.groups()
            wr.append(('pP', 'sin(%s)' % st.vf[int(a)][C.index(ac)]))
        elif lo.startswith('LOI'): wr.append(('I', lo.split()[1]))
        elif lo.startswith('MFP'):
            m = re.match(r'MFP\.(\w+)\s+vf(\d+), P', lo); mask, fd = m.groups(); wr.append((int(fd), mask, [st.P] * 4))
        elif lo.startswith('MOVE'):
            m = re.match(r'MOVE\.(\w+)\s+vf(\d+), vf(\d+)', lo); mask, fd, fs = m.groups(); wr.append((int(fd), mask, list(st.vf[int(fs)])))
        elif lo.startswith('MR32'):
            m = re.match(r'MR32\.(\w+)\s+vf(\d+), vf(\d+)', lo); mask, fd, fs = m.groups(); v = st.vf[int(fs)]
            wr.append((int(fd), mask, [v[1], v[2], v[3], v[0]]))
        elif lo.startswith('IADDIU') or lo.startswith('ISUBIU'):
            m = re.match(r'I(ADD|SUB)IU vi(\d+), vi(\d+), (-?\w+)', lo); k, d, a, imm = m.groups()
            wr.append(('vi', int(d), '(%s%s%s)' % (st.vi[int(a)], '+' if k == 'ADD' else '-', imm)))
        elif lo.startswith('IADD'):
            m = re.match(r'IADD vi(\d+), vi(\d+), vi(\d+)', lo); d, a, b = m.groups(); wr.append(('vi', int(d), '(%s+%s)' % (st.vi[int(a)], st.vi[int(b)])))
        elif lo.startswith('SQI'):
            m = re.match(r'SQI\.(\w+)\s+vf(\d+), \(vi(\d+)\+\+\)', lo); mask, fs, vi = m.groups()
            st.stores.append(('[%s]' % st.vi[int(vi)], mask, [st.vf[int(fs)][C.index(c)] for c in mask]))
            wr.append(('vi', int(vi), '%s+1' % st.vi[int(vi)]))
        elif lo.startswith('SQ'):
            m = re.match(r'SQ\.(\w+)\s+vf(\d+),\s*(-?\d+)\(vi(\d+)\)', lo); mask, fs, off, vi = m.groups()
            st.stores.append(('[%s]' % address(st, vi, off), mask, [st.vf[int(fs)][C.index(c)] for c in mask]))
        else: print('LO?', lo)
        # the accumulator-less upper results land after the lower reads
        for w in wr:
            if w[0] == 'vi': st.vi[w[1]] = w[2]
            elif w[0] == 'pQ':
                if st.pQ is not None: st.Q = st.pQ
                st.pQ = w[1]
            elif w[0] == 'pP':
                if st.pP is not None: st.P = st.pP
                st.pP = w[1]
            elif w[0] == 'I': st.I = w[1]
            else: st.put(w[0], w[1], w[2])
if __name__ == '__main__':
    code, lo, hi = sys.argv[1], sys.argv[2], sys.argv[3]
    P = listing(code, lo, hi)
    pcs = sorted(P); seq = []
    for seg in sys.argv[4:]:
        a, b = seg.split('-'); seq += [p for p in pcs if int(a, 16) <= p <= int(b, 16)]
    st = S(); run(P, seq, st)
    for n, e in defs: print('%s = %s' % (n, e))
    for a, mk_, v in st.stores: print('store', a, mk_, v)
