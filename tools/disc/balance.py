"""The Japanese Director's Cut's battle balance, as files for the US game.

The Director's Cut (SCPS-17013) changed the numbers of four tables of the
param pack: monster_param (the enemies: attributes, money, knockdown),
encount_group_param (how many enemies come together), btl_eff_param3 (the
attacks' effects) and fixed_encount_param (the story's fixed battles). Each
holds the same ops in the same order as the US table; `build` gives the
Japanese numbers the US strings, so the names stay English, and writes each
under the US table's name with a list, tables.txt, that the game reads
(src/dc_balance.h). With the European texts built into <disc>/EU, the same for
each language's tables (the US numbers with that language's names): the
Japanese numbers with its names. The Japanese tables stay in <out>/ja, so
`rebuild` makes a language's tables when its text comes later.

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

from . import archive, cfgbin, european
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
    """Writes the tables and tables.txt to `out`, for the English text and each European
    one in <disc>/EU, and keeps the Japanese tables in <out>/ja for a language added later;
    returns a line on each table."""
    ja = dict(archive.pac_entries(ja_pack))
    missing = [ja_name for _, ja_name in TABLES if ja_name not in ja]
    if missing:
        raise ValueError(f"{missing[0]} is not in the Japanese pack")
    sources = out / "ja"
    sources.mkdir(parents=True, exist_ok=True)
    for _, ja_name in TABLES:
        part = sources / (ja_name + ".part")
        part.write_bytes(ja[ja_name])
        part.replace(sources / ja_name)
    return _write_tables({ja_name: ja[ja_name] for _, ja_name in TABLES}, disc, out)


def rebuild(disc: Path, out: Path) -> list[str] | None:
    """The tables again, for the text languages there now, from the Japanese tables `build`
    kept in <out>/ja; None when they are not there (a balance made before it kept them)."""
    sources = out / "ja"
    if not all((sources / ja_name).is_file() for _, ja_name in TABLES):
        return None
    return _write_tables({ja_name: (sources / ja_name).read_bytes() for _, ja_name in TABLES}, disc, out)


def text_languages(disc: Path) -> list[str]:
    """The European text languages built into <disc>/EU (setup.py extract-european), as FR."""
    eu = disc / "EU"
    return [code.upper() for code in european.LANGUAGES
            if (eu / f"{code.upper()}.idx").is_file() and (eu / f"{code.upper()}.DAT").is_file()]


def languages_stale(disc: Path, out: Path) -> list[str]:
    """The text languages of <disc>/EU whose tables tables.txt lacks, or lists for other
    bytes than that language's tables now (its text built again since)."""
    listed = _listed(out)
    stale = []
    for code in text_languages(disc):
        try:
            bases = _base_tables(disc, code)
        except (OSError, ValueError):
            stale.append(code)  # the rebuild says what is wrong
            continue
        if any(listed.get(name) != (len(base), zlib.crc32(base)) for name, (_, base) in bases.items()):
            stale.append(code)
    return stale


def _listed(out: Path) -> dict[str, tuple[int, int]]:
    """tables.txt's rows: name -> the size and CRC-32 of the table each replaces."""
    try:
        text = (out / "tables.txt").read_text(encoding="utf-8")
    except OSError:
        return {}
    rows = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 4 and not line.startswith("#"):
            rows[fields[0]] = (int(fields[1]), int(fields[2], 16))
    return rows


def _language_pack(disc: Path, code: str) -> bytes:
    """A European language's param pack, from <disc>/EU/<code>.idx and .DAT."""
    eu = disc / "EU"
    name = european.to_language(US_PACK, code.lower())
    for line in (eu / f"{code}.idx").read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) == 3 and fields[0].lower() == name.lower():
            with open(eu / f"{code}.DAT", "rb") as f:
                f.seek(int(fields[1]) * SECTOR)
                return f.read(int(fields[2]))
    raise ValueError(f"{name} is not in {eu / (code + '.idx')}")


def _base_tables(disc: Path, code: str) -> dict[str, tuple[str, bytes]]:
    """The tables the balance replaces in a text's param pack (EN, the US disc's): name ->
    (the Japanese table's name, the table). A table with no language in its name is the
    same in every text (btl_eff_param3), so it goes with English alone."""
    pack = dict(archive.pac_entries(_pack_from_disc(disc, US_PACK) if code == "EN" else _language_pack(disc, code)))
    tables = {}
    for us_name, ja_name in TABLES:
        name = us_name if code == "EN" else european.entry_to_language(us_name, code)
        if code != "EN" and name == us_name:
            continue
        if name not in pack:
            raise ValueError(f"{name} is not in the {code} param pack")
        tables[name] = (ja_name, pack[name])
    return tables


def _merge(base: bytes, numbers: bytes, name: str) -> tuple[bytes, str]:
    merged, changed, kept = cfgbin.merge_numbers(base, numbers)
    if cfgbin.numbers_view(merged) != cfgbin.numbers_view(numbers) and kept == 0:
        raise ValueError(f"{name}: the numbers did not all come over")
    if cfgbin.strings_view(merged) != cfgbin.strings_view(base):
        raise ValueError(f"{name}: the strings changed")
    if len(merged) > len(base):
        raise ValueError(f"{name}: larger than the table it replaces ({len(merged)} > {len(base)})")
    return merged, (f"{name}: {changed} lines with the Japanese numbers"
                    + (f", {kept} kept (strings elsewhere)" if kept else ""))


def _write_tables(sources: dict[str, bytes], disc: Path, out: Path) -> list[str]:
    """Each table the game may run, the US English one and each European language's, with the
    Japanese numbers and its own strings (names), then tables.txt, which lists them all. A
    language whose tables cannot be made is left out, with a line saying why; English must be."""
    out.mkdir(parents=True, exist_ok=True)
    rows, report = [], []
    for code in ["EN"] + text_languages(disc):
        try:
            made = []
            for name, (ja_name, base) in _base_tables(disc, code).items():
                merged, line = _merge(base, sources[ja_name], name)
                made.append((name, base, merged, line))
        except (OSError, ValueError) as error:
            if code == "EN":
                raise
            report.append(f"{code}: not made ({error}); its text keeps the US numbers")
            continue
        # Written once the language's tables are all made, each in place whole.
        for name, base, merged, line in made:
            part = out / (name + ".part")
            part.write_bytes(merged)
            part.replace(out / name)
            rows.append(f"{name} {len(base)} {zlib.crc32(base):08x} {len(merged)}")
            report.append(line)
    text = (
        "# The Japanese Director's Cut's battle balance (setup.py director-cut-balance).\n"
        "# file, the size and CRC-32 of the table it replaces (US English, or a European\n"
        "# text's), the file's size\n" + "\n".join(rows) + "\n"
    )
    part = out / "tables.txt.part"
    part.write_text(text, encoding="utf-8", newline="\n")
    part.replace(out / "tables.txt")
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
