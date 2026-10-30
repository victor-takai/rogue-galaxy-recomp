#!/usr/bin/env python3
"""The release's source archive: the repository and its submodules in one zip.

    python tools/release/source_zip.py [--out build/release] [--ref REF | --worktree]

GitHub's own "Source code" archives leave the submodules out
(thirdparty/PS2Recomp, thirdparty/imgui), and Install.bat cannot build
without them, so a release carries this archive instead:
RogueGalaxy-<version>.zip, one folder RogueGalaxy/ holding the committed
files, each submodule at its recorded commit, and REVISION, the commit the
About window names (no .git comes along). --ref archives another commit
than HEAD (a release's tag, to make its archive again). With --worktree it takes the
working tree's tracked and new files instead of the commit, to try an
install before committing. Only source: never disc data or a build.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import re
import subprocess
import sys
import tarfile
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FOLDER = "RogueGalaxy"
# Folders that hold game data or builds; .gitignore keeps them out, and this
# makes sure.
NEVER = ("disc/", "build/", "mc0/", "mc1/", "textures/", "toolchain/")
LARGEST = 20 << 20


def git(*args: str, cwd: Path = ROOT) -> bytes:
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True).stdout


def archive(commit: str, cwd: Path = ROOT) -> bytes:
    """A commit's files as a tar. Without core.autocrlf, whatever the
    packager's git says: the framework has no .gitattributes, so this
    machine's autocrlf=true gave it CRLF files, and outside a repository
    `git apply` matched patches/ps2recomp (LF) to them only on a git that
    converts too: with autocrlf=false the first patch failed (2026-10-27).
    LF files take the patches under any setting."""
    return git("-c", "core.autocrlf=false", "archive", "--format=tar", commit, cwd=cwd)


def gitlinks(worktree: bool, ref: str) -> list[tuple[str, str]]:
    """The submodules as (path, recorded commit), from the index or a commit."""
    text = git("ls-files", "-s") if worktree else git("ls-tree", "-r", ref)
    links = []
    for line in text.decode().splitlines():
        meta, path = line.split("\t", 1)
        fields = meta.split()
        if fields[0] == "160000":
            links.append((path, fields[1] if worktree else fields[2]))
    return links


def add(zf: zipfile.ZipFile, name: str, data: bytes, mtime: float) -> None:
    if name.startswith(NEVER) or len(data) > LARGEST:
        sys.exit(f"refusing {name} ({len(data)} bytes): game data or a build has no place in the source archive")
    # A zip's times have no time zone, and Explorer reads them as local: the
    # packager's local time came out hours ahead west of it, and a CMake
    # input dated after now keeps build.ninja stale, so ninja regenerates it
    # 100 times and fails (DQ8-Recompiled #36; 0.1.0's first archive, 2026-10-27).
    # A day before, in UTC, is in the past in every time zone.
    info = zipfile.ZipInfo(f"{FOLDER}/{name}", time.gmtime(max(mtime - 86400, 315532800))[:6])
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = (0o755 if name.endswith(".sh") else 0o644) << 16
    zf.writestr(info, data, compresslevel=9)


def add_archive(zf: zipfile.ZipFile, tar_bytes: bytes, prefix: str) -> int:
    count = 0
    with tarfile.open(fileobj=io.BytesIO(tar_bytes)) as tar:
        for member in tar:
            if member.isfile():
                add(zf, prefix + member.name, tar.extractfile(member).read(), member.mtime)
                count += 1
    return count


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=str(ROOT / "build" / "release"))
    ap.add_argument("--ref", default="HEAD", help="the commit to archive (default HEAD)")
    ap.add_argument("--worktree", action="store_true", help="the working tree's files instead of a commit's")
    args = ap.parse_args()
    if args.worktree and args.ref != "HEAD":
        ap.error("--ref and --worktree exclude each other")

    if args.worktree:
        project = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    else:
        project = git("show", f"{args.ref}:CMakeLists.txt").decode()
    version = re.search(r"project\(RogueGalaxyRecomp\s+VERSION\s+(\d+\.\d+\.\d+)", project).group(1)
    revision = git("describe", "--tags", "--always", args.ref).decode().strip()
    dirty = git("status", "--porcelain", "--untracked-files=no", "--ignore-submodules=dirty").strip()
    if dirty and not args.worktree and args.ref == "HEAD":
        print("note: the working tree has changes this archive leaves out (HEAD only; --worktree takes them)")
    if args.worktree and dirty:
        revision += "-dirty"

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    target = out / f"RogueGalaxy-{version}.zip"
    links = gitlinks(args.worktree, args.ref)
    count = 0
    with zipfile.ZipFile(target, "w") as zf:
        if args.worktree:
            skip = {path for path, _ in links}
            listed = git("ls-files", "-z", "--cached", "--others", "--exclude-standard").decode().split("\0")
            for name in sorted(set(filter(None, listed)) - skip):
                path = ROOT / name
                if path.is_file():
                    add(zf, name, path.read_bytes(), path.stat().st_mtime)
                    count += 1
        else:
            count += add_archive(zf, archive(args.ref), "")
        for path, commit in links:
            count += add_archive(zf, archive(commit, ROOT / path), path + "/")
        add(zf, "REVISION", (revision + "\n").encode(), time.time())
        count += 1

    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    print(f"{target}: {count} files, {target.stat().st_size / 2**20:.1f} MiB, revision {revision}")
    print(f"sha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
