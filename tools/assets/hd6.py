#!/usr/bin/env python3
"""Level-5 HD6 index reader: list and extract the files of a DATA*.DAT archive."""
import struct, sys, os

def leb(b, i):
    v = s = 0
    while True:
        c = b[i]; i += 1
        v |= (c & 0x7F) << s; s += 7
        if not c & 0x80: return v, i

class HD6:
    def __init__(self, hd6_path):
        d = open(hd6_path, 'rb').read()
        (magic, hdr, a, nfrag, _, name_off, name_size, _, _, nfiles, ft_off, _, ft_end) = struct.unpack('<13I', d[:0x34])
        assert d[:4] == b'HD6\0'
        pool_end = name_off
        frags, cur = [], 0
        pool = d[hdr:pool_end]
        frags = pool.split(b'\0')
        self.frags = [f.decode('latin1') for f in frags]
        names = d[name_off:name_off + name_size]
        self.entries = []
        for i in range((ft_end - ft_off) // 8):
            v = struct.unpack_from('<Q', d, ft_off + i * 8)[0]
            noff = v & 0x1FFFF
            doff = ((v >> 17) & 0x1FFFFF) * 1024
            size = (v >> 38) * 4
            j, parts = noff, []
            while True:
                n, j = leb(names, j)
                if n == 0: break
                parts.append(self.frags[n])
            self.entries.append((''.join(parts), doff, size))

def main():
    h = HD6(sys.argv[1])
    for n, o, s in h.entries: print(f'{o:10d} {s:9d} {n}')
if __name__ == '__main__': main()
