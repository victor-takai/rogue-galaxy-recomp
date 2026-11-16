"""Writes an executable's resources, its icon and manifest, as a .res file.

    python make_resources.py ICON.ico MANIFEST.manifest OUT.res   (the launcher)
    python make_resources.py MANIFEST.manifest OUT.res            (the game: no icon)

The linker (lld) takes a .res as it is. It is written here rather than by the
toolchain's resource compiler, windres: on the machine this was made on, the
antivirus (Malwarebytes) quarantines llvm-mingw's windres the moment it runs.
The icon is the first resource of its kind, so Explorer shows it; the manifest
is resource 1 of type 24 (RT_MANIFEST), which Windows reads at start.
"""

import struct
import sys
from pathlib import Path
from xml.etree import ElementTree

RT_ICON = 3
RT_GROUP_ICON = 14
RT_MANIFEST = 24
LANGUAGE = 0x0409
# MOVEABLE | PURE | DISCARDABLE, as rc marks them; Windows ignores them.
FLAGS = 0x1030


def resource(kind: int, name: int, data: bytes) -> bytes:
    """One resource: its header (type and name as ordinals), its data, padding."""
    header = struct.pack(
        "<IIHHHHIHHII", len(data), 32, 0xFFFF, kind, 0xFFFF, name, 0, FLAGS, LANGUAGE, 0, 0
    )
    return header + data + b"\0" * (-len(data) % 4)


def icon_resources(icon_path: Path) -> bytes:
    """The icon's images, one resource each, and the group that lists them."""
    icon = icon_path.read_bytes()
    reserved, kind, count = struct.unpack_from("<HHH", icon, 0)
    if reserved != 0 or kind != 1 or count == 0:
        sys.exit(f"{icon_path}: not an icon")
    out = b""
    group = struct.pack("<HHH", 0, 1, count)
    for index in range(count):
        width, height, colours, _, planes, bits, size, offset = struct.unpack_from(
            "<BBBBHHII", icon, 6 + 16 * index
        )
        # Each image is a resource of its own; the group lists them by number.
        out += resource(RT_ICON, index + 1, icon[offset : offset + size])
        group += struct.pack(
            "<BBBBHHIH", width, height, colours, 0, planes or 1, bits or 32, size, index + 1
        )
    return out + resource(RT_GROUP_ICON, 1, group)


def main() -> None:
    args = [Path(arg) for arg in sys.argv[1:]]
    if len(args) not in (2, 3):
        sys.exit(__doc__)
    *icon_path, manifest_path, out_path = args
    manifest = manifest_path.read_bytes()
    # A manifest Windows cannot read keeps the program from starting at all.
    try:
        ElementTree.fromstring(manifest)
    except ElementTree.ParseError as error:
        sys.exit(f"{manifest_path}: not a manifest ({error})")
    # The empty resource that opens every 32-bit .res file.
    out = struct.pack("<IIHHHHIHHII", 0, 32, 0xFFFF, 0, 0xFFFF, 0, 0, 0, 0, 0, 0)
    if icon_path:
        out += icon_resources(icon_path[0])
    out += resource(RT_MANIFEST, 1, manifest)
    out_path.write_bytes(out)


if __name__ == "__main__":
    main()
