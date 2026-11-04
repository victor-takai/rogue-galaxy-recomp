"""Read-only ISO9660 reader for PS2 DVD images, dual-layer (DVD9) dumps included.

A PS2 DVD9 image holds two independent ISO9660 volumes: layer 0 starting at
sector 0 and layer 1 starting near the layer break. Directory records on layer 1
are relative to the start of that volume, so every entry carries both its
absolute image sector (`lba`) and the sector its own volume stores
(`layer_lba`).

Usage as a script lists every file:

    python tools/disc/iso9660.py "Rogue Galaxy (USA).iso"
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Callable, Iterator

SECTOR = 2048
PVD_SECTOR = 16
_PVD_MAGIC = b"\x01CD001\x01"
_COPY_CHUNK = 8 << 20


@dataclass(frozen=True)
class Volume:
    layer: int
    base: int  # absolute sector of this volume's sector 0
    volume_id: str
    blocks: int  # volume space size in sectors, from the PVD


@dataclass(frozen=True)
class Entry:
    path: str  # "BIN/NORMAL.BIN": '/' separators, no leading slash, no ";1"
    layer: int
    lba: int  # absolute sector in the image
    layer_lba: int  # sector as stored in the layer's own directory records
    size: int
    is_dir: bool


class IsoImage:
    def __init__(self, path: str | Path):
        self.path = Path(path)
        self.size = self.path.stat().st_size
        self.total_sectors = self.size // SECTOR
        self._f: BinaryIO = open(self.path, "rb")
        self.volumes = [self._read_volume(0, 0)]
        layer1 = self._find_layer1(self.volumes[0].blocks)
        if layer1 is not None:
            self.volumes.append(self._read_volume(1, layer1))

    def close(self) -> None:
        self._f.close()

    def __enter__(self) -> "IsoImage":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _read(self, sector: int, count: int = 1) -> bytes:
        self._f.seek(sector * SECTOR)
        return self._f.read(count * SECTOR)

    def _read_volume(self, layer: int, base: int) -> Volume:
        pvd = self._read(base + PVD_SECTOR)
        if pvd[:7] != _PVD_MAGIC:
            raise ValueError(f"no ISO9660 primary volume descriptor at sector {base + PVD_SECTOR}")
        return Volume(
            layer=layer,
            base=base,
            volume_id=pvd[40:72].decode("ascii", "replace").strip(),
            blocks=struct.unpack_from("<I", pvd, 80)[0],
        )

    def _find_layer1(self, layer0_blocks: int) -> int | None:
        if self.total_sectors <= layer0_blocks + PVD_SECTOR:
            return None
        # Layer 1's descriptor is normally 16 sectors past the layer break, but
        # some discs start the layer-1 volume 16 sectors *before* the break
        # (Rogue Galaxy: break 1646704, descriptor at 1646704). Look in a window
        # around the break first, then scan the rest of the image.
        for pvd in range(layer0_blocks - 2 * PVD_SECTOR, layer0_blocks + 4 * PVD_SECTOR):
            if self._read(pvd)[:7] == _PVD_MAGIC:
                return pvd - PVD_SECTOR
        chunk = 4096
        for start in range(layer0_blocks, self.total_sectors, chunk):
            buf = self._read(start, chunk)
            for i in range(0, len(buf), SECTOR):
                if buf[i:i + 7] == _PVD_MAGIC:
                    return start + i // SECTOR - PVD_SECTOR
        return None

    def walk(self, volume: Volume) -> Iterator[Entry]:
        pvd = self._read(volume.base + PVD_SECTOR)
        root = pvd[156:156 + 34]
        yield from self._walk_dir(
            volume,
            struct.unpack_from("<I", root, 2)[0],
            struct.unpack_from("<I", root, 10)[0],
            "",
        )

    def _walk_dir(self, volume: Volume, lba: int, size: int, prefix: str) -> Iterator[Entry]:
        data = self._read(volume.base + lba, (size + SECTOR - 1) // SECTOR)
        off = 0
        while off < len(data):
            length = data[off]
            if length == 0:
                # records never straddle sectors; the rest of this one is padding
                off = (off // SECTOR + 1) * SECTOR
                continue
            rec = data[off:off + length]
            off += length
            name = rec[33:33 + rec[32]]
            if name in (b"\x00", b"\x01"):
                continue
            ext_lba = struct.unpack_from("<I", rec, 2)[0]
            ext_size = struct.unpack_from("<I", rec, 10)[0]
            path = prefix + name.decode("ascii", "replace").split(";")[0]
            is_dir = bool(rec[25] & 2)
            yield Entry(path, volume.layer, volume.base + ext_lba, ext_lba, ext_size, is_dir)
            if is_dir:
                yield from self._walk_dir(volume, ext_lba, ext_size, path + "/")

    def entries(self) -> list[Entry]:
        return [e for v in self.volumes for e in self.walk(v)]

    def read_bytes(self, entry: Entry) -> bytes:
        self._f.seek(entry.lba * SECTOR)
        return self._f.read(entry.size)

    def read_range(self, entry: Entry, offset: int, size: int) -> bytes:
        """`size` bytes of `entry` from `offset`, such as one file of a DATA archive."""
        if offset < 0 or size < 0 or offset + size > entry.size:
            raise ValueError(f"{entry.path}: {size} bytes at {offset} pass its end ({entry.size})")
        self._f.seek(entry.lba * SECTOR + offset)
        data = self._f.read(size)
        if len(data) != size:
            raise IOError(f"image ends inside {entry.path}")
        return data

    def iter_chunks(self, entry: Entry) -> Iterator[bytes]:
        left = entry.size
        pos = entry.lba * SECTOR
        while left:
            self._f.seek(pos)
            chunk = self._f.read(min(left, _COPY_CHUNK))
            if not chunk:
                raise IOError(f"image ends inside {entry.path}")
            yield chunk
            pos += len(chunk)
            left -= len(chunk)

    def copy_to(self, entry: Entry, out: BinaryIO, update: Callable[[bytes], None] | None = None) -> None:
        for chunk in self.iter_chunks(entry):
            out.write(chunk)
            if update is not None:
                update(chunk)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[-1].strip())
        return 2
    with IsoImage(argv[1]) as iso:
        for v in iso.volumes:
            print(f"layer {v.layer}: base sector {v.base}, {v.blocks} sectors, id '{v.volume_id}'")
        for e in iso.entries():
            kind = "<DIR>" if e.is_dir else str(e.size)
            print(f"L{e.layer} {e.lba:>8} {kind:>12}  {e.path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
