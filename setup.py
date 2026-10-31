#!/usr/bin/env python3
"""Rogue Galaxy Recompiled: disc and recompiler-input tooling.

    python setup.py verify   --iso "Rogue Galaxy (USA).iso" [--full]
    python setup.py extract  --iso "Rogue Galaxy (USA).iso" [--out disc] [--force]
    python setup.py extract-japanese --iso "Rogue Galaxy - Director's Cut (Japan).iso" [--disc disc]
    python setup.py info     [--disc disc]
    python setup.py overlays  [--disc disc] [--out build/overlays]
    python setup.py check     [--disc disc]
    python setup.py recompile [--disc disc] [--recomp path/to/ps2_recomp]

`extract-japanese` copies the voices and the movies that differ from the US
ones out of the Japanese Director's Cut into disc/JA, for the Japanese dub.
`verify` checks a dump against config/<version>/hashes.json without writing
anything. `extract` unpacks both DVD layers into one tree (the runtime boots
the ELF from that folder) and hashes every file while copying. `info` decodes
the MWo3 overlay table from an extracted tree. `overlays` wraps each code
overlay as an ELF for ps2_recomp. `check` verifies the function maps and
recompiler configs against the extracted disc and the pinned framework.
`recompile` runs check and overlays, then ps2_recomp for every image into
build/generated/<image>/, which CMake then builds into the game.

Maintainers: `make-hashes` regenerates hashes.json from a known-good dump;
`maps --rgdecomp <checkout>` regenerates the function maps; `vu1-manifest`
describes a VU1 program recording (build/vu1-profile) as
config/<version>/vu1-programs.txt, which `recompile` expands from the user's
disc; `vu-blocks-manifest` does the same for the VU block recordings
(build/vu-blocks) as config/<version>/vu-blocks.txt.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import tomllib
import zlib
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))

from disc.iso9660 import SECTOR, Entry, IsoImage  # noqa: E402
from funcmap import check as fmcheck  # noqa: E402
from funcmap import funcmap  # noqa: E402
from funcmap.images import load_main, load_overlay  # noqa: E402
from funcmap.runtime_calls import RuntimeCalls  # noqa: E402
from mwo3 import mwo3  # noqa: E402

DEFAULT_VERSION = "SCUS_974.90"
QUICK_LIMIT = 32 << 20  # files up to this size are hashed by plain `verify`
FRAMEWORK = ROOT / "thirdparty" / "PS2Recomp"


def config_dir(version: str) -> Path:
    return ROOT / "config" / version


def load_toml(path: Path) -> dict:
    with open(path, "rb") as f:
        return tomllib.load(f)


def load_hashes(version: str) -> dict:
    path = config_dir(version) / "hashes.json"
    if not path.is_file():
        sys.exit(f"no known hashes for {version} ({path})")
    return json.loads(path.read_text(encoding="utf-8"))


def unique_files(iso: IsoImage) -> dict[str, list[Entry]]:
    """Every file path on the disc, mapped to its copies (layer 0 first)."""
    files: dict[str, list[Entry]] = {}
    for e in iso.entries():
        if not e.is_dir:
            files.setdefault(e.path, []).append(e)
    return files


def hash_entry(iso: IsoImage, entry: Entry) -> str:
    h = hashlib.sha256()
    for chunk in iso.iter_chunks(entry):
        h.update(chunk)
    return h.hexdigest()


def image_digests(path: Path) -> dict:
    md5, sha1, sha256, crc = hashlib.md5(), hashlib.sha1(), hashlib.sha256(), 0
    with open(path, "rb") as f:
        while chunk := f.read(8 << 20):
            md5.update(chunk)
            sha1.update(chunk)
            sha256.update(chunk)
            crc = zlib.crc32(chunk, crc)
    return {"crc32": f"{crc:08x}", "md5": md5.hexdigest(), "sha1": sha1.hexdigest(), "sha256": sha256.hexdigest()}


def check_layout(iso: IsoImage, known: dict) -> list[str]:
    problems = []
    k = known["iso"]
    if iso.size != k["size"]:
        problems.append(f"image is {iso.size} bytes, expected {k['size']}")
    starts = [v.base for v in iso.volumes]
    if starts != k["volume_starts"]:
        problems.append(f"ISO9660 volumes start at sectors {starts}, expected {k['volume_starts']}")
    return problems


def cmd_verify(args: argparse.Namespace) -> int:
    known = load_hashes(args.version)
    with IsoImage(args.iso) as iso:
        problems = check_layout(iso, known)
        files = unique_files(iso)
        expected = known["files"]
        for path in sorted(set(expected) - set(files)):
            problems.append(f"missing from image: {path}")
        for path in sorted(set(files) - set(expected)):
            problems.append(f"not in hashes.json: {path}")
        checked = 0
        for path, info in sorted(expected.items()):
            copies = files.get(path)
            if not copies:
                continue
            layers = [c.layer for c in copies]
            if layers != info["layers"]:
                problems.append(f"{path}: on layers {layers}, expected {info['layers']}")
            if copies[0].size != info["size"]:
                problems.append(f"{path}: {copies[0].size} bytes, expected {info['size']}")
                continue
            if info["size"] <= QUICK_LIMIT:
                if hash_entry(iso, copies[0]) != info["sha256"]:
                    problems.append(f"{path}: sha256 mismatch")
                checked += 1
    print(f"layout and sizes checked for {len(expected)} files; {checked} small files hashed")
    if args.full:
        print("hashing the whole image (about a minute)...")
        got = image_digests(Path(args.iso))
        for algo in ("crc32", "md5", "sha1"):
            if got[algo] != known["iso"][algo]:
                problems.append(f"image {algo} {got[algo]} != Redump {known['iso'][algo]}")
        if not problems:
            print(f"image matches Redump: crc32 {got['crc32']}, sha1 {got['sha1']}")
    for p in problems:
        print("PROBLEM:", p)
    print("OK" if not problems else f"{len(problems)} problem(s)")
    return 1 if problems else 0


def cmd_extract(args: argparse.Namespace) -> int:
    known = load_hashes(args.version)
    out = Path(args.out)
    if out.exists() and any(out.iterdir()) and not args.force:
        sys.exit(f"{out} is not empty; pass --force to overwrite")
    # The manifest says the folder holds the disc (install skips extracting
    # then): written last, and only when nothing is wrong.
    (out / "manifest.json").unlink(missing_ok=True)
    problems = []
    manifest = {"version": args.version, "image": str(Path(args.iso).resolve()), "files": {}}
    with IsoImage(args.iso) as iso:
        problems += check_layout(iso, known)
        files = unique_files(iso)
        total = sum(c[0].size for c in files.values())
        done = 0
        # Into a pipe (the installer's log) a line every 5% instead of one
        # line rewritten in place, which a pipe gets only at the end.
        tty = sys.stdout.isatty()
        shown = -1
        for path, copies in sorted(files.items(), key=lambda kv: kv[1][0].lba):
            first = copies[0]
            dest = out / path
            dest.parent.mkdir(parents=True, exist_ok=True)
            part = dest.with_name(dest.name + ".part")
            h = hashlib.sha256()
            with open(part, "wb") as f:
                iso.copy_to(first, f, h.update)
            part.replace(dest)
            digest = h.hexdigest()
            info = known["files"].get(path)
            if info is None:
                problems.append(f"not in hashes.json: {path}")
            elif digest != info["sha256"]:
                problems.append(f"{path}: sha256 mismatch")
            manifest["files"][path] = {
                "size": first.size,
                "sha256": digest,
                "copies": [{"layer": c.layer, "lba": c.lba, "layer_lba": c.layer_lba} for c in copies],
            }
            done += first.size
            percent = done * 100 // max(total, 1)
            if tty:
                print(f"\r{percent:3d}%  {path:<40.40}", end="", flush=True)
            elif percent // 5 != shown:
                shown = percent // 5
                print(f"{percent:3d}%", flush=True)
        if tty:
            print()
        manifest["volumes"] = [
            {"layer": v.layer, "base": v.base, "blocks": v.blocks, "volume_id": v.volume_id} for v in iso.volumes
        ]
    for path in sorted(set(known["files"]) - set(manifest["files"])):
        problems.append(f"missing from image: {path}")
    print(f"extracted {len(manifest['files'])} files ({total / 2**30:.2f} GiB) to {out}")
    if (out / known["elf"]).is_file():
        _, ovl_problems = overlay_table(out, known["elf"], quiet=True)
        problems += ovl_problems
    for p in problems:
        print("PROBLEM:", p)
    if problems:
        print(f"{len(problems)} problem(s); {out / 'manifest.json'} not written")
        return 1
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print("OK")
    return 0


def cmd_extract_japanese(args: argparse.Namespace) -> int:
    """Voices and movies of the Japanese Director's Cut, into <disc>/JA."""
    known = load_toml_json(config_dir(args.version) / "japanese.json")
    disc = Path(args.disc)
    out = disc / "JA"
    us_manifest = {}
    if (disc / "manifest.json").is_file():
        us_manifest = json.loads((disc / "manifest.json").read_text(encoding="utf-8"))["files"]
    with IsoImage(args.iso) as iso:
        files = unique_files(iso)
        for name, want in known["indexes"].items():
            copies = files.get(name)
            if not copies or hash_entry(iso, copies[0]) != want:
                sys.exit(
                    f"{args.iso} is not the Japanese Director's Cut (SCPS-17013): {name} does not match. "
                    "Only that release has voices that line up with the US game."
                )
        wanted = list(known["voice_files"])
        # A movie the US disc has the same bytes of needs no second copy.
        for path in sorted(files):
            if not path.upper().endswith(".MVI"):
                continue
            us = us_manifest.get(path)
            if us is not None and us["sha256"] == hash_entry(iso, files[path][0]):
                continue
            wanted.append(path)
        total = sum(files[p][0].size for p in wanted)
        done = 0
        manifest = {"image": str(Path(args.iso).resolve()), "files": {}}
        for path in wanted:
            entry = files[path][0]
            dest = out / path
            dest.parent.mkdir(parents=True, exist_ok=True)
            part = dest.with_name(dest.name + ".part")
            h = hashlib.sha256()
            with open(part, "wb") as f:
                iso.copy_to(entry, f, h.update)
            part.replace(dest)
            manifest["files"][path] = {"size": entry.size, "sha256": h.hexdigest()}
            done += entry.size
            print(f"{done * 100 // max(total, 1):3d}%  {path}", flush=True)
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"extracted {len(wanted)} files ({total / 2**30:.2f} GiB) to {out}")
    return 0


def load_toml_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def overlay_table(disc: Path, elf_name: str, quiet: bool = False):
    elf = (disc / elf_name).read_bytes()
    bins = {p.name: p.read_bytes() for p in sorted((disc / "BIN").glob("*.BIN"))}
    headers, problems = mwo3.check(elf, bins)
    if not quiet:
        for fname, h in headers:
            print(
                f"  {h.id}  {fname:<13} load {h.load_addr:#010x}  text {h.text_size:#8x}  data {h.data_size:#7x}"
                f"  bss {h.bss_size:#7x}  mem {h.mem_size:#8x}  ctors {h.ctor_begin:#010x}..{h.ctor_end:#010x}"
            )
    return headers, problems


def cmd_info(args: argparse.Namespace) -> int:
    known = load_hashes(args.version)
    disc = Path(args.disc)
    print((disc / "SYSTEM.CNF").read_text(encoding="ascii", errors="replace").strip())
    print("MWo3 overlays:")
    headers, problems = overlay_table(disc, known["elf"])
    if headers:
        arena_start = min(h.load_addr for _, h in headers)
        arena_end = max(h.mem_end for _, h in headers)
        print(f"arena {arena_start:#010x}..{arena_end:#010x} ({(arena_end - arena_start) / 1024:.1f} KiB)")
    for p in problems:
        print("PROBLEM:", p)
    return 1 if problems else 0


def cmd_make_hashes(args: argparse.Namespace) -> int:
    path = config_dir(args.version) / "hashes.json"
    old = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}
    with IsoImage(args.iso) as iso:
        files = {}
        for p, copies in sorted(unique_files(iso).items()):
            digests = {hash_entry(iso, c) for c in copies}
            if len(digests) != 1:
                sys.exit(f"{p}: copies on different layers differ; hashes.json cannot describe that yet")
            files[p] = {"size": copies[0].size, "sha256": digests.pop(), "layers": [c.layer for c in copies]}
        iso_info = {
            "size": iso.size,
            "sectors": iso.total_sectors,
            "volume_starts": [v.base for v in iso.volumes],
        }
    digests = image_digests(Path(args.iso))
    for algo in ("crc32", "md5", "sha1"):
        # these come from Redump; refuse to overwrite them from a different dump
        if algo in old.get("iso", {}) and old["iso"][algo] != digests[algo]:
            sys.exit(f"image {algo} {digests[algo]} does not match the Redump value {old['iso'][algo]} in {path}")
    iso_info.update(digests)
    data = {
        **{k: v for k, v in old.items() if k not in ("iso", "files", "generated")},
        "generated": date.today().isoformat(),
        "iso": {**old.get("iso", {}), **iso_info},
        "files": files,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {path} ({len(files)} files)")
    return 0


def map_csv(version: str, image: str) -> Path:
    return config_dir(version) / f"functions_{image}.csv"


def cmd_maps(args: argparse.Namespace) -> int:
    spec = load_toml(config_dir(args.version) / "funcmap.toml")
    src = Path(args.rgdecomp) / "config"
    for fname, want in spec["source"]["sha256"].items():
        got = hashlib.sha256((src / fname).read_bytes()).hexdigest()
        if got != want:
            sys.exit(f"{src / fname}: sha256 {got} is not the pinned {want} (RGDecomp {spec['source']['commit'][:7]})")
    for image, ispec in spec["image"].items():
        funcs, log = funcmap.build(image, ispec, funcmap.read_rgdecomp(src / ispec["symbols"]))
        funcmap.write_csv(map_csv(args.version, image), funcs)
        print(f"{image:<8} {len(funcs):>6} functions  ({'; '.join(log) or 'as RGDecomp'})")
    return 0


def write_overlay_elfs(version: str, disc: Path, out: Path) -> None:
    spec = load_toml(config_dir(version) / "funcmap.toml")
    out.mkdir(parents=True, exist_ok=True)
    for image, ispec in spec["image"].items():
        if "overlay" in ispec:
            elf = mwo3.to_elf((disc / ispec["overlay"]).read_bytes())
            (out / f"{image}.elf").write_bytes(elf)
            print(f"wrote {out / (image + '.elf')} ({len(elf)} bytes)")


def cmd_overlays(args: argparse.Namespace) -> int:
    write_overlay_elfs(args.version, Path(args.disc), Path(args.out))
    return 0


def cmd_check(args: argparse.Namespace) -> int:
    cdir = config_dir(args.version)
    spec = load_toml(cdir / "funcmap.toml")
    known = load_hashes(args.version)
    arena = load_toml(cdir / "overlays.toml")["arena"]
    disc = Path(args.disc)

    calls = None
    if (FRAMEWORK / "ps2xRuntime").is_dir():
        calls = RuntimeCalls.load(FRAMEWORK)
    images, maps = {}, {}
    for image, ispec in spec["image"].items():
        if "overlay" in ispec:
            images[image] = load_overlay(image, disc / ispec["overlay"])
        else:
            images[image] = load_main(disc / known["elf"], [tuple(r) for r in ispec["code"]])
        maps[image] = funcmap.read_csv(map_csv(args.version, image))

    report = fmcheck.check_names(maps, calls)
    for image, ispec in spec["image"].items():
        gaps = [tuple(g["range"]) for g in ispec.get("known_gap", [])]
        report.merge(fmcheck.check_layout(images[image], maps[image], gaps))
        h = images[image].header
        table = (h.ctor_begin, h.ctor_end) if h else tuple(ispec.get("ctor_table", (0, 0)))
        if table[1] > table[0]:
            report.merge(fmcheck.check_ctors(images[image], fmcheck.Index(maps[image]), table))
    report.merge(fmcheck.check_control_flow(images, maps, (arena["start"], arena["end"])))
    report.merge(fmcheck.check_pointer_targets(images, maps))
    for image in spec["image"]:
        cfg_path = cdir / "recomp" / f"{image}.toml"
        if not cfg_path.is_file():
            report.problems.append(f"{image}: no recompiler config at {cfg_path.relative_to(ROOT)}")
            continue
        cfg = load_toml(cfg_path)
        if Path(cfg["general"].get("ghidra_output", "")) != map_csv(args.version, image).relative_to(ROOT):
            report.problems.append(f"{image}: {cfg_path.name} does not point ghidra_output at its map")
        report.merge(fmcheck.check_recomp_config(image, cfg, fmcheck.Index(maps[image]), calls))

    for n in report.notes:
        print(" ", n)
    if calls is None:
        print("  (thirdparty/PS2Recomp missing: runtime handler checks skipped)")
    for p in report.problems:
        print("PROBLEM:", p)
    print("OK" if not report.problems else f"{len(report.problems)} problem(s)")
    return 1 if report.problems else 0


def find_recompiler(given: str | None) -> Path:
    candidates = [Path(given)] if given else [
        ROOT / "build" / "tools" / "ps2xRecomp" / "ps2_recomp.exe",
        ROOT / "build" / "tools" / "ps2xRecomp" / "ps2_recomp",
    ]
    for c in candidates:
        if c.is_file():
            return c
    sys.exit("ps2_recomp not found; build it first (python setup.py tools, docs/BUILDING.md) or pass --recomp")


def overlay_info_header(name: str, h: mwo3.Header, source: str) -> str:
    return (
        f"// Generated by setup.py recompile from {source} of your disc. Do not edit.\n"
        "#pragma once\n\n"
        '#include "overlay/overlay_image.h"\n\n'
        "namespace rg::overlay\n{\n"
        f"// {h.name}: what generated/{name}/ was recompiled from.\n"
        f"inline constexpr Header kGeneratedHeader{{{h.id}u, 0x{h.load_addr:08X}u, 0x{h.text_size:X}u, "
        f"0x{h.data_size:X}u, 0x{h.bss_size:X}u, 0x{h.ctor_begin:08X}u, 0x{h.ctor_end:08X}u}};\n"
        "}\n"
    )


def cmd_recompile(args: argparse.Namespace) -> int:
    recomp = find_recompiler(args.recomp)
    print("== check")
    if cmd_check(args):
        sys.exit("check failed; not recompiling")
    print("== overlays")
    write_overlay_elfs(args.version, Path(args.disc), ROOT / "build" / "overlays")
    print("== VU1 programs")
    expand_vu1_manifest(args.version, Path(args.disc))
    print("== VU blocks")
    expand_vu_blocks_manifest(args.version, Path(args.disc))

    spec = load_toml(config_dir(args.version) / "funcmap.toml")
    logs = ROOT / "build" / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    generated = (ROOT / "build" / "generated").resolve()
    failed = 0
    for image, ispec in spec["image"].items():
        cfg_path = config_dir(args.version) / "recomp" / f"{image}.toml"
        out = (ROOT / load_toml(cfg_path)["general"]["output"]).resolve()
        if generated not in out.parents:
            sys.exit(f"{cfg_path.name}: output {out} is not under {generated}; refusing to clear it")
        # ps2_recomp writes into a staging folder; only files whose bytes
        # changed are copied over, so an incremental build recompiles just those.
        stage = out.with_name(out.name + ".new")
        shutil.rmtree(stage, ignore_errors=True)
        cfg_text = cfg_path.read_text(encoding="utf-8")
        cfg_text = re.sub(r'(?m)^output\s*=.*$', f'output = "{stage.relative_to(ROOT).as_posix()}"', cfg_text)
        staged_cfg = logs / f"recomp_{image}.toml"
        staged_cfg.write_text(cfg_text, encoding="utf-8")
        log = logs / f"recomp_{image}.log"
        print(f"== ps2_recomp {image} (log: {log.relative_to(ROOT)})", flush=True)
        with open(log, "w", encoding="utf-8", errors="replace") as f:
            rc = subprocess.run([str(recomp), str(staged_cfg.relative_to(ROOT))], cwd=ROOT,
                                stdout=f, stderr=subprocess.STDOUT).returncode
        text = log.read_text(encoding="utf-8", errors="replace")
        stats = re.search(r"Functions processed: (\d+), recompiled: (\d+), stubs: (\d+), skipped: (\d+), "
                          r"decode failures: (\d+)", text)
        unhandled = re.search(r"Unhandled instructions: (\d+)", text)
        if rc != 0 or not stats or not unhandled:
            print(f"   FAILED (exit {rc}); see the log")
            failed += 1
            continue
        processed, recompiled, stubs, skipped, decode = (int(x) for x in stats.groups())
        print(f"   {processed} functions: {recompiled} recompiled, {stubs} bound to the runtime, "
              f"{skipped} skipped, {decode} decode failures, {unhandled.group(1)} unhandled instructions")
        if skipped or decode or int(unhandled.group(1)):
            failed += 1
        if "overlay" in ispec:
            h = mwo3.parse_header((Path(args.disc) / ispec["overlay"]).read_bytes())
            (stage / "overlay_info.h").write_text(overlay_info_header(image, h, ispec["overlay"]), encoding="utf-8")
        changed, removed = sync_tree(stage, out)
        shutil.rmtree(stage)
        print(f"   {changed} file(s) changed, {removed} removed")
    print("OK" if not failed else f"{failed} image(s) failed")
    return 1 if failed else 0


VU1_TOOL = FRAMEWORK / "tools" / "vu_program_manifest.py"
VU1_PROFILE = ROOT / "build" / "vu1-profile"
VU1_MANIFEST_OUT = ROOT / "build" / "vu1-manifest"


def vu1_game_files(version: str) -> list[str]:
    """Where the game's VU1 microcode sits: the main ELF and the code overlays."""
    spec = load_toml(config_dir(version) / "funcmap.toml")
    return [load_hashes(version)["elf"]] + [i["overlay"] for i in spec["image"].values() if "overlay" in i]


def expand_vu1_manifest(version: str, disc: Path) -> None:
    """Rebuild the VU1 programs the game uses from the committed manifest and the user's disc.

    The manifest (config/<version>/vu1-programs.txt) names where each compiled
    routine's microcode sits in the game files, with a checksum; no game data.
    The result goes to build/vu1-manifest, which CMake compiles into the
    runtime along with build/vu1-profile, anything recorded locally with
    PS2_VU_PROGRAM_PROFILE.
    """
    manifest = config_dir(version) / "vu1-programs.txt"
    if not manifest.is_file():
        print("   no manifest; VU1 programs come only from local recordings")
        return
    shutil.rmtree(VU1_MANIFEST_OUT, ignore_errors=True)
    rc = subprocess.run([sys.executable, "-I", str(VU1_TOOL), "expand", "--manifest", str(manifest),
                         "--game-root", str(disc), "--output", str(VU1_MANIFEST_OUT)]).returncode
    if rc != 0:
        sys.exit("expanding the VU1 manifest failed")
    print(f"   wrote {VU1_MANIFEST_OUT.relative_to(ROOT)}")


def cmd_vu1_manifest(args: argparse.Namespace) -> int:
    disc = Path(args.disc)
    out = config_dir(args.version) / "vu1-programs.txt"
    cmd = [sys.executable, "-I", str(VU1_TOOL), "make", "--output", str(out), "--game-root", str(disc)]
    for f in vu1_game_files(args.version):
        cmd += ["--game", str(disc / f)]
    cmd.append(str(Path(args.profile)))
    return subprocess.run(cmd).returncode


# VU blocks: the micro program blocks the interpreter ran, recorded with
# PS2_VU_AOT_PROFILE into build/vu-blocks/*.txt (VU0's above all, which the
# whole VU1 programs above do not cover) and compiled into the runtime. A
# recording is microcode, so it stays local; the manifest
# (config/<version>/vu-blocks.txt) says where each block's pairs sit in the
# game's files instead, with one checksum over all the blocks (one per block
# would give the shortest pieces' code away), and recompile rebuilds the
# blocks from the player's disc into build/vu-blocks-manifest. Without it a
# fresh install ran VU0 on the interpreter: the battle's game thread was 94%
# busy against 86% (2026-10-27).
VU_BLOCKS_PROFILE = ROOT / "build" / "vu-blocks"
VU_BLOCKS_OUT = ROOT / "build" / "vu-blocks-manifest"
VU_BLOCKS_HEADERS = {b"VU-BLOCKS 1", b"VU-BLOCKS 2", b"VU-BLOCKS 3"}


def fnv64(data: bytes) -> int:
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & (2**64 - 1)
    return value


def vu_block_bytes(words: tuple[int, ...]) -> bytes:
    # A pair is one 64-bit word, the lower instruction in its low half: as it
    # sits in micro memory and in the files that upload it.
    return b"".join(w.to_bytes(8, "little") for w in words)


def read_vu_block_recording(path: Path) -> set[tuple[int, tuple[int, ...]]]:
    lines = path.read_bytes().splitlines()
    if not lines or lines[0].rstrip(b"\r") not in VU_BLOCKS_HEADERS:
        sys.exit(f"{path}: not a VU block recording (code images cannot be described)")
    blocks = set()
    for line in lines[1:]:
        fields = line.split()
        if fields:
            blocks.add((int(fields[0]), tuple(int(w, 16) for w in fields[1:])))
    return blocks


def cmd_vu_blocks_manifest(args: argparse.Namespace) -> int:
    disc = Path(args.disc)
    names = vu1_game_files(args.version)
    files = [(disc / n).read_bytes() for n in names]
    recordings = sorted(Path(args.profile).glob("*.txt"))
    if not recordings:
        sys.exit(f"no recordings in {args.profile}")
    blocks = sorted(set().union(*(read_vu_block_recording(p) for p in recordings)))
    lines = []
    for unit, words in blocks:
        # Each block as the longest pieces found in the files, first file first.
        pieces, start = [], 0
        while start < len(words):
            for end in range(len(words), start, -1):
                chunk = vu_block_bytes(words[start:end])
                found = next(((i, data.find(chunk)) for i, data in enumerate(files) if chunk in data), None)
                if found:
                    pieces.append(f"{found[0]}:{found[1]}:{end - start}")
                    start = end
                    break
            else:
                print(f"block not in the game files: unit {unit}, pair {start} of {len(words)}")
                return 1
        lines.append(f"block {unit} " + " ".join(pieces))
    checksum = fnv64(b"".join(vu_block_bytes(words) for _, words in blocks))
    out = config_dir(args.version) / "vu-blocks.txt"
    out.write_text(
        "# VU micro program blocks compiled into the runtime, located in the game's\n"
        "# files (setup.py vu-blocks-manifest; recompile expands it).\n"
        "# file <index> <name>; block <unit> <file>:<offset>:<pairs>...; fnv64 of all blocks\n"
        + "".join(f"file {i} {n}\n" for i, n in enumerate(names))
        + "".join(line + "\n" for line in lines)
        + f"fnv64 {checksum:016x}\n", newline="\n")
    print(f"wrote {out.relative_to(ROOT)}: {len(blocks)} blocks from {len(recordings)} recording(s)")
    return 0


def expand_vu_blocks_manifest(version: str, disc: Path) -> None:
    manifest = config_dir(version) / "vu-blocks.txt"
    shutil.rmtree(VU_BLOCKS_OUT, ignore_errors=True)
    if not manifest.is_file():
        print("   no manifest; VU blocks come only from local recordings")
        return
    files: dict[str, bytes] = {}
    blocks, checksum = [], None
    for line in manifest.read_text().splitlines():
        fields = line.split()
        if not fields or fields[0].startswith("#"):
            continue
        if fields[0] == "file":
            files[fields[1]] = (disc / fields[2]).read_bytes()
        elif fields[0] == "block":
            raw = b""
            for piece in fields[2:]:
                index, offset, pairs = piece.split(":")
                raw += files[index][int(offset):int(offset) + int(pairs) * 8]
            blocks.append((int(fields[1]), raw))
        elif fields[0] == "fnv64":
            checksum = int(fields[1], 16)
    if checksum is None or fnv64(b"".join(raw for _, raw in blocks)) != checksum:
        sys.exit(f"{manifest.relative_to(ROOT)} does not match this disc's files")
    lengths = {len(raw) // 8 for _, raw in blocks}
    header = "VU-BLOCKS 1" if lengths == {4} else "VU-BLOCKS 2" if lengths <= {4, 8, 12, 16} else "VU-BLOCKS 3"
    VU_BLOCKS_OUT.mkdir(parents=True)
    (VU_BLOCKS_OUT / "blocks.txt").write_text(
        header + "\n" + "".join(
            f"{unit} " + " ".join(f"{int.from_bytes(raw[i:i + 8], 'little'):x}" for i in range(0, len(raw), 8)) + "\n"
            for unit, raw in blocks), newline="\n")
    print(f"   wrote {VU_BLOCKS_OUT.relative_to(ROOT)} ({len(blocks)} blocks)")


def sync_tree(src: Path, dst: Path) -> tuple[int, int]:
    """Make dst hold exactly src's files, rewriting only those that differ."""
    dst.mkdir(parents=True, exist_ok=True)
    new = {p.name for p in src.iterdir() if p.is_file()}
    changed = 0
    for name in new:
        data = (src / name).read_bytes()
        target = dst / name
        if not target.is_file() or target.read_bytes() != data:
            target.write_bytes(data)
            changed += 1
    removed = 0
    for p in dst.iterdir():
        if p.is_file() and p.name not in new:
            p.unlink()
            removed += 1
    return changed, removed


TOOLS_BUILD = ROOT / "build" / "tools"
GAME_BUILD = ROOT / "build" / "game"
COMPILERS = [f"-DCMAKE_C_COMPILER={os.environ.get('CC') or 'clang'}",
             f"-DCMAKE_CXX_COMPILER={os.environ.get('CXX') or 'clang++'}"]
GAME_EXE = "roguegalaxy.exe" if os.name == "nt" else "roguegalaxy"


def run_step(what: str, command: list[str]) -> None:
    print(f"== {what}", flush=True)
    try:
        rc = subprocess.run(command, cwd=ROOT).returncode
    except OSError as e:  # the tool is missing (an antivirus took it, say)
        sys.exit(f"failed: {what}: cannot run {command[0]}: {e}")
    if rc:
        sys.exit(f"failed: {what}")


# A configure that failed leaves CMakeCache.txt but no build.ninja; the next
# run must configure again.
def configured(build: Path) -> bool:
    return (build / "build.ninja").is_file()


# Ninja regenerates build.ninja while a CMake input is dated after it, and an
# input dated after now keeps it so: 100 tries, some 40 minutes, then a
# failure (DQ8-Recompiled #36). A clock set back leaves such files, and so
# does a zip extracted west of where it was made, its times having no time
# zone (0.1.0's first archive, 2026-10-27). The sources get now's time first.
# .claude holds Claude Code's worktrees, each a checkout of its own.
OWN_TIME = {"build", "disc", "toolchain", "textures", ".claude"}


def reset_future_times() -> None:
    now = time.time()
    reset, failed = 0, []
    for folder, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d != ".git" and not (Path(folder) == ROOT and d in OWN_TIME)]
        for name in files:
            if name == ".git":  # a submodule's, which names its repository
                continue
            path = Path(folder) / name
            try:
                if path.stat().st_mtime > now + 60:
                    os.utime(path, (now, now))
                    reset += 1
            except OSError:
                failed.append(path)
    if reset:
        print(f"   {reset} source files were dated in the future (an archive from another time zone, "
              "or the clock); they have now's time")
    if failed:
        print(f"   the time of {len(failed)} files could not be checked or set, {failed[0]} first")


def cmd_tools(args: argparse.Namespace) -> int:
    reset_future_times()
    # The patches first: some change the recompiler, and one built before
    # them generates code without them.
    run_step("patches/ps2recomp", ["cmake", "-P", str(ROOT / "cmake" / "framework_patches.cmake")])
    if not configured(TOOLS_BUILD):
        run_step("configure build/tools", [
            "cmake", "-S", str(FRAMEWORK), "-B", str(TOOLS_BUILD), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
            "-DPS2X_BUILD_RUNTIME=OFF", "-DPS2X_BUILD_TEST=OFF", "-DPS2X_BUILD_STUDIO=OFF", *COMPILERS])
    run_step("build ps2_recomp and ps2_analyzer",
             ["cmake", "--build", str(TOOLS_BUILD), "--target", "ps2_recomp", "ps2_analyzer"])
    return 0


def cmd_build(args: argparse.Namespace) -> int:
    reset_future_times()
    if not configured(GAME_BUILD):
        run_step("configure build/game", [
            "cmake", "-S", str(ROOT), "-B", str(GAME_BUILD), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", *COMPILERS])
    run_step("build the game", ["cmake", "--build", str(GAME_BUILD)])
    launcher = "Rogue Galaxy.exe" if os.name == "nt" else "rogue-galaxy.sh"
    print(f"built {GAME_BUILD / 'src' / GAME_EXE}; start it with \"{launcher}\"")
    return 0


def cmd_portable(args: argparse.Namespace) -> int:
    """The built game in a folder of its own, to copy to another machine (a Steam Deck, through Proton)."""
    out = Path(args.out).resolve()
    game_dir = GAME_BUILD / "src"
    launcher = ROOT / "Rogue Galaxy.exe"
    disc = ROOT / "disc"
    for needed in (game_dir / "roguegalaxy.exe", launcher, disc / "manifest.json"):
        if not needed.is_file():
            sys.exit(f"{needed} is missing: install and build the game first")
    if out.exists() and any(out.iterdir()):
        sys.exit(f"{out} is not empty; pick a new folder")
    (out / "build" / "game" / "src").mkdir(parents=True, exist_ok=True)
    shutil.copy2(launcher, out)
    shutil.copy2(game_dir / "roguegalaxy.exe", out / "build" / "game" / "src")
    for dll in game_dir.glob("*.dll"):
        shutil.copy2(dll, out / "build" / "game" / "src")
    # The memory cards only when asked: the copy's saves then go their own way.
    skip = () if args.saves else ("mc0", "mc1")
    print(f"== copying disc/ (about 6.3 GiB){' with the memory cards' if args.saves else ''}", flush=True)
    shutil.copytree(disc, out / "disc", ignore=lambda folder, names: [n for n in names
                                                                       if Path(folder) == disc and n in skip])
    if (ROOT / "borders").is_dir():
        shutil.copytree(ROOT / "borders", out / "borders")
    print(f"done: {out}\n"
          "On a Steam Deck: copy the folder over, add \"Rogue Galaxy.exe\" to Steam as a non-Steam game, and in its "
          "Properties > Compatibility force a Proton version. It starts in fullscreen there.")
    return 0


def cmd_install(args: argparse.Namespace) -> int:
    """Everything from a dump to a game: verify, extract, tools, recompile, build."""
    disc = ROOT / "disc"
    if (disc / "manifest.json").is_file() and (disc / load_hashes(args.version)["elf"]).is_file():
        print(f"== {disc} already holds the extracted disc; not extracting again")
        if args.iso:
            print(f"   (so {args.iso} is not used; delete {disc / 'manifest.json'} to extract it instead)")
    else:
        if not args.iso:
            sys.exit("pass --iso with your dump of the game (docs/DISC.md)")
        print("== verify the dump", flush=True)
        if cmd_verify(argparse.Namespace(version=args.version, iso=args.iso, full=False)):
            sys.exit("the dump is not the one this project supports (docs/DISC.md)")
        print("== extract it into disc/ (about 6.3 GiB)", flush=True)
        # Over whatever an interrupted or failed extraction left (the
        # manifest is written last, when nothing was wrong); files that are
        # not the disc's, the memory cards, stay as they are.
        if cmd_extract(argparse.Namespace(version=args.version, iso=args.iso, out=str(disc), force=True)):
            sys.exit("the extraction found problems; see above")
    cmd_tools(args)
    print("== recompile", flush=True)
    if cmd_recompile(argparse.Namespace(version=args.version, disc=str(disc), recomp=None)):
        sys.exit("the recompile failed; see build/logs")
    return cmd_build(args)


def main() -> int:
    ap = argparse.ArgumentParser(description="Rogue Galaxy Recompiled: disc and recompiler-input tooling")
    ap.add_argument("--version", default=DEFAULT_VERSION, help=f"config/<version> to use (default {DEFAULT_VERSION})")
    sub = ap.add_subparsers(dest="command", required=True)

    p = sub.add_parser("verify", help="check a dump against known hashes (read-only)")
    p.add_argument("--iso", required=True)
    p.add_argument("--full", action="store_true", help="also hash the whole image against Redump")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("extract", help="unpack both layers into one folder")
    p.add_argument("--iso", required=True)
    p.add_argument("--out", default=str(ROOT / "disc"))
    p.add_argument("--force", action="store_true", help="overwrite a non-empty output folder")
    p.set_defaults(func=cmd_extract)

    p = sub.add_parser("extract-japanese", help="voices and movies of the Japanese Director's Cut, into disc/JA")
    p.add_argument("--iso", required=True)
    p.add_argument("--disc", default=str(ROOT / "disc"))
    p.set_defaults(func=cmd_extract_japanese)

    p = sub.add_parser("info", help="print SYSTEM.CNF and the MWo3 overlay table")
    p.add_argument("--disc", default=str(ROOT / "disc"))
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("overlays", help="wrap each code overlay as an ELF for ps2_recomp")
    p.add_argument("--disc", default=str(ROOT / "disc"))
    p.add_argument("--out", default=str(ROOT / "build" / "overlays"))
    p.set_defaults(func=cmd_overlays)

    p = sub.add_parser("check", help="verify function maps and recompiler configs against the disc")
    p.add_argument("--disc", default=str(ROOT / "disc"))
    p.set_defaults(func=cmd_check)

    p = sub.add_parser("recompile", help="check, wrap overlays, and run ps2_recomp for every image")
    p.add_argument("--disc", default=str(ROOT / "disc"))
    p.add_argument("--recomp", help="ps2_recomp executable (default: build/tools/ps2xRecomp/)")
    p.set_defaults(func=cmd_recompile)

    p = sub.add_parser("tools", help="apply patches/ps2recomp, then build the recompiler in build/tools")
    p.set_defaults(func=cmd_tools)

    p = sub.add_parser("build", help="configure (once) and build the game in build/game")
    p.set_defaults(func=cmd_build)

    p = sub.add_parser("install", help="from a dump to the game: verify, extract, tools, recompile, build")
    p.add_argument("--iso", help="your dump of the game (not needed once disc/ is extracted)")
    p.set_defaults(func=cmd_install)

    p = sub.add_parser("portable", help="copy the built game to a folder of its own (for a Steam Deck, through Proton)")
    p.add_argument("--out", required=True, help="a new or empty folder")
    p.add_argument("--saves", action="store_true", help="also copy the memory cards (disc/mc0, disc/mc1)")
    p.set_defaults(func=cmd_portable)

    p = sub.add_parser("make-hashes", help="(maintainers) regenerate hashes.json from a known-good dump")
    p.add_argument("--iso", required=True)
    p.set_defaults(func=cmd_make_hashes)

    p = sub.add_parser("vu1-manifest", help="(maintainers) describe a VU1 program recording as a manifest")
    p.add_argument("--disc", default=str(ROOT / "disc"))
    p.add_argument("--profile", default=str(VU1_PROFILE), help="PS2_VU_PROGRAM_PROFILE directory")
    p.set_defaults(func=cmd_vu1_manifest)

    p = sub.add_parser("vu-blocks-manifest", help="(maintainers) describe the VU block recordings as a manifest")
    p.add_argument("--disc", default=str(ROOT / "disc"))
    p.add_argument("--profile", default=str(VU_BLOCKS_PROFILE), help="directory of PS2_VU_AOT_PROFILE recordings")
    p.set_defaults(func=cmd_vu_blocks_manifest)

    p = sub.add_parser("maps", help="(maintainers) regenerate functions_*.csv from an RGDecomp checkout")
    p.add_argument("--rgdecomp", required=True, help="RGDecomp checkout at the commit pinned in funcmap.toml")
    p.set_defaults(func=cmd_maps)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
