"""The Japanese Director's Cut's battle balance, as files for the US game.

The Director's Cut (SCPS-17013) changed the numbers of four tables of the
param pack: monster_param (the enemies: attributes, money, knockdown),
encount_group_param (how many enemies come together), btl_eff_param3 (the
attacks' effects) and fixed_encount_param (the story's fixed battles). Each
holds the same ops in the same order as the US table; `build` gives the
Japanese numbers the US strings, so the names stay English, and writes each
under the US table's name with a list, tables.txt, that the game reads
(src/dc_balance.h).

It also changed 76 enemies' files (chara\\mob\\*.chr: their attacks' reach,
timing and kind, a few models) and 98 AI scripts (chara\\spt\\*.stb, the
party's too). Those hold no text and the US executable reads them as they
are, but most are larger than the US ones, so `build_files` packs them, each
on sectors of its own, into files.dat, listed in files.idx (archive path,
first sector, size), which the game serves in place of the US files.
"""

from __future__ import annotations

import hashlib
import re
import zlib
from pathlib import Path

from . import archive, cfgbin
from .iso9660 import IsoImage

SECTOR = 2048
# The archives holding the enemies' files, in the order the game looks.
FILE_ARCHIVES = ("DATA0_0", "DATA1_0")
FILE_PATTERN = re.compile(r"^chara\\(mob\\e\d+\.chr|spt\\[^\\]+\.stb)$")
ARCHIVE = "DATA0_0"
US_PACK = "rc\\param\\param_2_en.pac"
JA_PACK = "rc\\param\\param_2_ja.pac"
# The US table's name and the Japanese one's, in their packs.
TABLES = [
    ("monster_param_EN.cfg.bin", "monster_param_JA.cfg.bin"),
    ("encount_group_param_EN.cfg.bin", "encount_group_param_JA.cfg.bin"),
    ("btl_eff_param3.cfg.bin", "btl_eff_param3.cfg.bin"),
    ("fixed_encount_param_EN.cfg.bin", "fixed_encount_param_JA.cfg.bin"),
]


def _pack_from_disc(disc: Path, name: str) -> bytes:
    index = archive.hd6_index((disc / f"{ARCHIVE}.HD6").read_bytes())
    offset, size = index[name]
    with open(disc / f"{ARCHIVE}.DAT", "rb") as f:
        f.seek(offset)
        return f.read(size)


def _pack_from_iso(iso: IsoImage, name: str) -> bytes:
    files = {e.path.upper(): e for e in iso.entries() if not e.is_dir}
    index = archive.hd6_index(iso.read_bytes(files[f"{ARCHIVE}.HD6"]))
    offset, size = index[name]
    return iso.read_range(files[f"{ARCHIVE}.DAT"], offset, size)


def japanese_pack(iso: IsoImage) -> bytes:
    return _pack_from_iso(iso, JA_PACK)


def pack_sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def build(ja_pack: bytes, disc: Path, out: Path) -> list[str]:
    """Writes the tables and tables.txt to `out`; returns a line on each."""
    us = dict(archive.pac_entries(_pack_from_disc(disc, US_PACK)))
    ja = dict(archive.pac_entries(ja_pack))
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    report = []
    for us_name, ja_name in TABLES:
        if us_name not in us or ja_name not in ja:
            raise ValueError(f"{us_name if us_name not in us else ja_name} is not in its pack")
        base, numbers = us[us_name], ja[ja_name]
        merged, changed, kept = cfgbin.merge_numbers(base, numbers)
        if cfgbin.numbers_view(merged) != cfgbin.numbers_view(numbers) and kept == 0:
            raise ValueError(f"{us_name}: the numbers did not all come over")
        if cfgbin.strings_view(merged) != cfgbin.strings_view(base):
            raise ValueError(f"{us_name}: the strings changed")
        if len(merged) > len(base):
            raise ValueError(f"{us_name}: larger than the US table ({len(merged)} > {len(base)})")
        part = out / (us_name + ".part")
        part.write_bytes(merged)
        part.replace(out / us_name)
        rows.append(f"{us_name} {len(base)} {zlib.crc32(base):08x} {len(merged)}")
        report.append(f"{us_name}: {changed} lines with the Japanese numbers"
                      + (f", {kept} kept (strings elsewhere)" if kept else ""))
    text = (
        "# The Japanese Director's Cut's battle balance (setup.py director-cut-balance).\n"
        "# file, the US table's size and CRC-32, the file's size\n" + "\n".join(rows) + "\n"
    )
    (out / "tables.txt").write_text(text, encoding="utf-8", newline="\n")
    return report


def _archive_files(read_hd6, pattern=FILE_PATTERN) -> dict[str, tuple[str, int, int]]:
    """path -> (archive, offset, size), the first archive holding each path."""
    found: dict[str, tuple[str, int, int]] = {}
    for name in FILE_ARCHIVES:
        hd6 = read_hd6(name)
        if hd6 is None:
            continue
        for path, (offset, size) in archive.hd6_index(hd6).items():
            if pattern.match(path) and path not in found:
                found[path] = (name, offset, size)
    return found


def _iso_files(iso: IsoImage) -> tuple[dict, dict]:
    entries = {e.path.upper(): e for e in iso.entries() if not e.is_dir}
    files = _archive_files(lambda n: iso.read_bytes(entries[f"{n}.HD6"]) if f"{n}.HD6" in entries else None)
    return entries, files


def changed_files(iso: IsoImage, disc: Path) -> dict[str, str]:
    """For maintainers: the Japanese enemy files and AI scripts whose bytes differ from
    the US disc's, path -> sha256 (the list in japanese.json, `balance_files`)."""
    entries, ja = _iso_files(iso)
    us = _archive_files(lambda n: (disc / f"{n}.HD6").read_bytes() if (disc / f"{n}.HD6").is_file() else None)
    out = {}
    for path in sorted(ja):
        if path not in us:
            continue
        name, offset, size = ja[path]
        data = iso.read_range(entries[f"{name}.DAT"], offset, size)
        uname, uoffset, usize = us[path]
        with open(disc / f"{uname}.DAT", "rb") as f:
            f.seek(uoffset)
            if f.read(usize) == data:
                continue
        out[path] = hashlib.sha256(data).hexdigest()
    return out


def build_files(iso: IsoImage, known: dict[str, str], out: Path) -> list[str]:
    """Writes files.dat and files.idx to `out` with the Japanese files `known` names
    (path -> sha256); returns a line on what it wrote."""
    entries, ja = _iso_files(iso)
    missing = [p for p in known if p not in ja]
    if missing:
        raise ValueError(f"{missing[0]} is not on the Japanese disc")
    out.mkdir(parents=True, exist_ok=True)
    dat_part, idx_part = out / "files.dat.part", out / "files.idx.part"
    rows = []
    total = 0
    with open(dat_part, "wb") as dat:
        for path in sorted(known):
            name, offset, size = ja[path]
            data = iso.read_range(entries[f"{name}.DAT"], offset, size)
            if hashlib.sha256(data).hexdigest() != known[path]:
                raise ValueError(f"{path} on the Japanese disc is not the Director's Cut's")
            sector = dat.tell() // SECTOR
            dat.write(data + b"\0" * (-len(data) % SECTOR))
            rows.append(f"{path}\t{sector}\t{len(data)}")
            total += len(data)
    idx_part.write_text(
        "# The Japanese Director's Cut's enemy files and AI scripts (setup.py director-cut-balance):\n"
        "# archive path, first sector in files.dat, size\n" + "\n".join(rows) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    dat_part.replace(out / "files.dat")
    idx_part.replace(out / "files.idx")
    return [f"files.dat: {len(rows)} enemy files and AI scripts, {total / 1048576:.1f} MiB"]
