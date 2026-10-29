"""Checks the game's zip reader (src/zip_reader.h) against Python's zipfile.

check_zip.py CHECKER_EXE: writes zips of stored, deflated (levels 1 and 9),
empty, folder-nested and large entries, has the checker (check_zip.cpp)
extract them, and compares every file with the original bytes.
"""
import os
import random
import subprocess
import sys
import tempfile
import zipfile


def main():
    checker = sys.argv[1]
    rng = random.Random(3)
    work = tempfile.mkdtemp()
    files = {
        "pack.json": b'{"format": 1, "name": "zip test"}',
        "empty.bin": b"",
        "random.bin": bytes(rng.getrandbits(8) for _ in range(70000)),
        "text.txt": b"the same words again and again. " * 20000,
        "sub/dir/deep.bin": bytes(rng.choice(b"ab") for _ in range(200000)),
        "ramp.bin": bytes(i % 251 for i in range(3_000_000)),
    }
    bad = 0
    for label, method, level in (("stored", zipfile.ZIP_STORED, None), ("deflate1", zipfile.ZIP_DEFLATED, 1),
                                 ("deflate9", zipfile.ZIP_DEFLATED, 9)):
        path = os.path.join(work, label + ".zip")
        with zipfile.ZipFile(path, "w", method, compresslevel=level) as archive:
            for name, data in files.items():
                archive.writestr(name, data)
            archive.writestr(zipfile.ZipInfo("folder/"), b"")
        out = os.path.join(work, label)
        os.makedirs(out)
        result = subprocess.run([checker, path, out])
        if result.returncode:
            print(label, "the checker failed")
            bad += 1
            continue
        for name, data in files.items():
            got = open(os.path.join(out, name.replace("/", "__")), "rb").read()
            if got != data:
                print(label, name, "differs")
                bad += 1
    # a damaged one must be refused, not crash
    damaged = os.path.join(work, "damaged.zip")
    blob = open(os.path.join(work, "deflate9.zip"), "rb").read()
    open(damaged, "wb").write(blob[: len(blob) // 2])
    out = os.path.join(work, "damaged")
    os.makedirs(out)
    if subprocess.run([checker, damaged, out], stderr=subprocess.DEVNULL).returncode == 0:
        print("a cut zip was accepted")
        bad += 1
    print("3 zips,", bad, "problems")
    return 1 if bad else 0


sys.exit(main())
