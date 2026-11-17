"""Walk a guest thread's stack in a memory dump: stackwalk.py DUMPDIR SP RA [--depth N]

DUMPDIR holds rdram.bin from a memory dump (guest RAM, offset =
guest address). SP and RA are the thread's registers as dumpmem.ps1 printed
them ("ctx at ... sp 017678A0 ra 0015DEE0"), hex. The function containing RA
is the innermost frame whose prologue is known (a kernel stub such as
WaitSema pushes no frame); its saved $ra and frame size come from the
recompiled source build/generated/main/<Name>_<addr>_0x<addr>.cpp (the
"addiu $sp, $sp, -N" and "sd $ra, OFF($sp)" of its prologue), the caller's
return address is read at SP+OFF, the caller's frame starts at SP+N, and so
on up to the thread's entry (a return address of 0) or the first frame the
prologues cannot explain. Each step checks that the return address points
just after a jal/jalr in the function it lands in.

Functions are named from llvm-nm on build/run/exe-worker.exe (or --exe); a
guest address maps to the greatest function start at or below it.
"""
import argparse
import bisect
import os
import re
import shutil
import struct
import subprocess
import sys


def load_functions(exe):
    nm = shutil.which("llvm-nm") or "llvm-nm"
    out = subprocess.run([nm, "--defined-only", "--no-demangle", exe], capture_output=True, text=True).stdout
    funcs = {}
    for line in out.splitlines():
        m = re.search(r" [tT] (?:_Z\d+)?([A-Za-z][A-Za-z0-9_]*?)_0x([0-9a-fA-F]+)", line)
        if m:
            a = int(m.group(2), 16)
            if a < 0x2000000 and (a not in funcs or len(m.group(1)) < len(funcs[a])):
                funcs[a] = m.group(1)
    return sorted(funcs), funcs


class Walker:
    def __init__(self, dumpdir, exe, generated):
        self.ram = open(os.path.join(dumpdir, "rdram.bin"), "rb").read()
        self.starts, self.names = load_functions(exe)
        self.generated = generated
        self.lines = {}

    def word(self, address):
        if address + 4 > len(self.ram):
            return None
        return struct.unpack_from("<I", self.ram, address)[0]

    def function(self, address):
        i = bisect.bisect_right(self.starts, address) - 1
        return (self.starts[i], self.names[self.starts[i]]) if i >= 0 else (None, None)

    def source_lines(self, start):
        if start in self.lines:
            return self.lines[start]
        name = self.names[start]
        # a name may carry the address twice (Name_15de90_0x15de90) or once
        path = None
        for candidate in (f"{name}_0x{start:x}.cpp", f"{name}_{start:x}_0x{start:x}.cpp"):
            p = os.path.join(self.generated, candidate)
            if os.path.exists(p):
                path = p
                break
        table = {}
        if path:
            for line in open(path, encoding="utf-8", errors="replace"):
                m = re.match(r"\s*// 0x([0-9a-f]+): 0x[0-9a-f]+\s+(\S+)\s*(.*)", line)
                if m:
                    table[int(m.group(1), 16)] = (m.group(2), m.group(3).strip())
        self.lines[start] = table
        return table

    def prologue(self, start):
        """(frame size, $ra slot) from the prologue, or (0, None) for a leaf."""
        table = self.source_lines(start)
        size, slot = 0, None
        for address in sorted(table):
            op, args = table[address]
            if address - start > 0x60:
                break
            if op == "addiu" and args.startswith("$sp, $sp, -"):
                size = int(args.split("-")[-1], 16)
            if op in ("sd", "sw", "sq") and args.startswith("$ra,"):
                slot = int(args.split(",")[1].split("(")[0].strip(), 16)
        return size, slot

    def is_call_site(self, ra):
        start, _ = self.function(ra)
        if start is None:
            return False
        op, _args = self.source_lines(start).get(ra - 8, ("", ""))
        return op in ("jal", "jalr")

    def walk(self, sp, ra, depth):
        chain = []
        for _ in range(depth):
            start, name = self.function(ra)
            if start is None:
                chain.append((sp, ra, "?", "no function at this address"))
                break
            size, slot = self.prologue(start)
            note = ""
            if not self.is_call_site(ra):
                note = "not after a jal/jalr"
            chain.append((sp, ra, f"{name} (+0x{ra - start:x})", note))
            if slot is None:
                chain.append((sp, 0, "", "this function saves no $ra: a leaf or an unknown prologue; stopping"))
                break
            saved = self.word(sp + slot)
            if saved is None or saved == 0:
                chain.append((sp + size, saved or 0, "", "return address 0: the thread's entry"))
                break
            ra = saved
            sp = sp + size
        return chain


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("dumpdir")
    ap.add_argument("sp", type=lambda v: int(v, 16))
    ap.add_argument("ra", type=lambda v: int(v, 16))
    ap.add_argument("--exe", default="build/run/exe-worker.exe")
    ap.add_argument("--generated", default="build/generated/main")
    ap.add_argument("--depth", type=int, default=40)
    args = ap.parse_args()
    walker = Walker(args.dumpdir, args.exe, args.generated)
    for sp, ra, where, note in walker.walk(args.sp, args.ra, args.depth):
        print(f"sp 0x{sp:08X} ra 0x{ra:08X} {where} {note}".rstrip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
