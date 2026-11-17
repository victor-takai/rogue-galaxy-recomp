#!/usr/bin/env bash
# The installer as an AppImage: one file to download, make executable (or
# double-click) and run in desktop mode.
#
#     tools/release/build_appimage.sh [OUT_DIR]      (default build/release)
#
# Run it on x86_64 Linux (the Deck, or the build container). The AppImage is
# thin on purpose: it carries installer/RogueGalaxy-Setup.py and starts it
# with the system's python3, which on SteamOS already has GTK 4 (PyGObject);
# bundling GTK would add over 100 MB for nothing. Output:
# RogueGalaxy-Setup-x86_64.AppImage and its .sha256.
#
# appimagetool is downloaded once into build/appimage/ and checked against
# the checksum below.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${1:-$ROOT/build/release}"
WORK="$ROOT/build/appimage"
TOOL_URL="https://github.com/AppImage/appimagetool/releases/download/1.9.0/appimagetool-x86_64.AppImage"
TOOL_SHA256="46fdd785094c7f6e545b61afcfb0f3d98d8eab243f644b4b17698c01d06083d1"

[ "$(uname -m)" = x86_64 ] || { echo "x86_64 only" >&2; exit 1; }
mkdir -p "$WORK" "$OUT"

TOOL="$WORK/appimagetool"
if [ ! -x "$TOOL" ]; then
    curl -fL --retry 3 -o "$TOOL.part" "$TOOL_URL"
    got="$(sha256sum "$TOOL.part" | cut -d' ' -f1)"
    if [ -n "$TOOL_SHA256" ] && [ "$got" != "$TOOL_SHA256" ]; then
        echo "appimagetool checksum mismatch: $got" >&2
        rm -f "$TOOL.part"
        exit 1
    fi
    echo "appimagetool sha256: $got"
    chmod +x "$TOOL.part" && mv "$TOOL.part" "$TOOL"
fi

APPDIR="$WORK/RogueGalaxy-Setup.AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/share/rogue-galaxy"
cp "$ROOT/installer/RogueGalaxy-Setup.py" "$APPDIR/usr/share/rogue-galaxy/"

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
# The system's Python and GTK 4: leave the AppImage's own environment out of
# the way of what the installer starts (git, curl, distrobox).
HERE="$(dirname "$(readlink -f "$0")")"
exec /usr/bin/python3 "$HERE/usr/share/rogue-galaxy/RogueGalaxy-Setup.py" "$@"
EOF
chmod +x "$APPDIR/AppRun"

cat > "$APPDIR/rogue-galaxy-setup.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Rogue Galaxy Setup
Comment=Install Rogue Galaxy on the Steam Deck
Exec=AppRun
Icon=rogue-galaxy-setup
Categories=Game;
Terminal=false
EOF

# A plain icon (no game art): a gold disc on the installer's dark blue.
python3 - "$APPDIR/rogue-galaxy-setup.png" <<'EOF'
import struct, sys, zlib
n = 256
rows = []
for y in range(n):
    row = bytearray([0])
    for x in range(n):
        inside = (x - 128) ** 2 + (y - 128) ** 2 < 84 ** 2
        row += bytes((244, 203, 82) if inside else (6, 10, 30))
    rows.append(bytes(row))
def chunk(tag, data):
    body = tag + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))
png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))
open(sys.argv[1], "wb").write(png)
EOF

# Without FUSE (a container) the tool runs extracted.
ARCH=x86_64 "$TOOL" --appimage-extract-and-run "$APPDIR" "$OUT/RogueGalaxy-Setup-x86_64.AppImage"
( cd "$OUT" && sha256sum RogueGalaxy-Setup-x86_64.AppImage > RogueGalaxy-Setup-x86_64.AppImage.sha256 )
echo "$OUT/RogueGalaxy-Setup-x86_64.AppImage"
