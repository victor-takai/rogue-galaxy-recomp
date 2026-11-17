#!/bin/sh
# The game's in-window update (src/self_update.cpp starts it, from the home
# screen's Update button): downloads the newest release's source archive,
# checks its checksum, unpacks it over this folder and builds it, as
# install.sh does. Progress goes to build/update.progress, one line,
# "<fraction 0..1, or -1> <text>" (or "error <text>"), which the game shows.
# Output goes to build/update.log. Nothing here touches disc/ or saves; the
# running game keeps its binary until the new one is linked.
#
# Tests: RG_UPDATE_API=URL reads the release list from there (file:// works),
# RG_UPDATE_REPO=owner/name picks the repository.
cd "$(dirname "$(readlink -f "$0")")" || exit 1
mkdir -p build
repo=${RG_UPDATE_REPO:-thibault-lgm/deck-galaxy}
api=${RG_UPDATE_API:-https://api.github.com/repos/$repo/releases?per_page=10}
stage=build/update-staging
archive=build/update-download.zip

progress() { printf '%s %s\n' "$1" "$2" >build/update.progress.tmp && mv -f build/update.progress.tmp build/update.progress; }
fail() {
    printf 'error %s\n' "$1" >build/update.progress
    echo "update: $1" >&2
    rm -rf "$stage" "$archive" build/update.rc
    exit 1
}

echo "update: $(date)"
progress -1 "Looking for the newest release"
curl --silent --fail --location --max-time 20 --user-agent RogueGalaxyRecompiled \
    --header 'Accept: application/vnd.github+json' "$api" -o build/update-release.json \
    || fail "Could not reach GitHub"
# The first release of the list: its tag, the source archive and its checksum.
set -- $(python3 -I - build/update-release.json <<'PY'
import json, sys
releases = json.load(open(sys.argv[1]))
release = releases[0] if releases else {}
assets = {a["name"]: a["browser_download_url"] for a in release.get("assets", [])}
zips = [n for n in assets if n.startswith("RogueGalaxy-") and n.endswith(".zip")]
if zips:
    name = sorted(zips)[-1]
    print(release.get("tag_name", ""), assets[name], assets.get(name + ".sha256", "-"))
PY
)
tag=$1 zip_url=$2 sum_url=$3
[ -n "$zip_url" ] || fail "The release has no source archive"
[ "$sum_url" != "-" ] && [ -n "$sum_url" ] || fail "The release has no checksum"

progress 0 "Downloading $tag"
size=$(curl --silent --location --head "$zip_url" | tr -d '\r' | awk 'tolower($1) == "content-length:" { n = $2 } END { print n + 0 }')
curl --silent --fail --location "$zip_url" -o "$archive" &
dl=$!
while kill -0 "$dl" 2>/dev/null; do
    have=$(stat -c %s "$archive" 2>/dev/null || echo 0)
    if [ "$size" -gt 0 ]; then
        progress "$(awk -v h="$have" -v s="$size" 'BEGIN { printf "%.3f", 0.1 * h / s }')" "Downloading $tag"
    fi
    sleep 0.5
done
wait "$dl" || fail "The download failed"

progress 0.1 "Checking the download"
want=$(curl --silent --fail --location "$sum_url" | awk '{ print $1 }')
got=$(sha256sum "$archive" | awk '{ print $1 }')
[ -n "$want" ] && [ "$want" = "$got" ] || fail "The download does not match its checksum"

progress 0.12 "Unpacking"
rm -rf "$stage"
python3 -I - "$archive" "$stage" <<'PY' || fail "Could not unpack the archive"
import sys, zipfile
zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])
PY
root=$(find "$stage" -mindepth 1 -maxdepth 1 -type d | head -n 1)
[ -n "$root" ] && [ -f "$root/install.sh" ] || fail "The archive is not a game release"
cp -a "$root"/. . || fail "Could not copy the new files"
rm -rf "$stage" "$archive"

# The build: install.sh in the container, its ninja lines as the progress.
progress 0.15 "Building"
rm -f build/update.rc
{ sh install.sh --container 2>&1; echo $? >build/update.rc; } | while IFS= read -r line; do
    printf '%s\n' "$line"
    case "$line" in
        "["*/*"]"*)
            counts=${line#\[}
            counts=${counts%%\]*}
            progress "$(awk -v n="${counts%/*}" -v m="${counts#*/}" 'BEGIN { if (m > 0) printf "%.3f", 0.15 + 0.85 * n / m; else print 0.15 }')" "Building $counts"
            ;;
        "== "*) progress -1 "${line#== }" ;;
    esac
done
[ "$(cat build/update.rc 2>/dev/null)" = 0 ] || fail "The build failed: see build/update.log"
rm -f build/update.rc build/update-release.json
progress 1 "Updated to $tag"
