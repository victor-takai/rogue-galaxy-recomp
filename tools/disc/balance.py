"""The Japanese Director's Cut's battle balance, as tables for the US game.

The Director's Cut (SCPS-17013) changed the numbers of four tables of the
param pack: monster_param (the enemies: attributes, money, knockdown),
encount_group_param (how many enemies come together), btl_eff_param3 (the
attacks' effects) and fixed_encount_param (the story's fixed battles). Each
holds the same ops in the same order as the US table; `build` gives the
Japanese numbers the US strings, so the names stay English, and writes each
under the US table's name with a list, tables.txt, that the game reads
(src/dc_balance.h).
"""

from __future__ import annotations

import hashlib
import zlib
from pathlib import Path

from . import archive, cfgbin
from .iso9660 import IsoImage

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
