#!/bin/sh
# Rogue Galaxy Recompiled on Linux and the Steam Deck: what Install.bat does
# on Windows. Builds the game from your own disc image (setup.py install:
# verify, extract, recompile, build), then adds a menu entry.
#
#   ./install.sh [--iso FILE] [--container | --native]
#
# With every build tool on the PATH it builds right here. Without them (the
# Steam Deck's SteamOS has a read-only root and no compiler) it builds inside
# a distrobox container named rg-build, an Ubuntu 24.04 that it sets up; the
# game itself needs nothing from the container and runs on the host.
# Takes about 10 to 30 minutes, and 20 GB of disk (the extracted disc, the
# generated code, the build).
cd "$(dirname "$(readlink -f "$0")")" || exit 1

iso=
mode=auto
while [ $# -gt 0 ]; do
    case "$1" in
        --iso) iso=$2; shift ;;
        --iso=*) iso=${1#--iso=} ;;
        --container) mode=container ;;
        --native) mode=native ;;
        -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done

say() { printf '\n== %s\n' "$*"; }
die() { printf 'Error: %s\n' "$*" >&2; exit 1; }

# The disc image: given, or found in the usual folders, or asked for.
if [ -z "$iso" ] && ! [ -e disc/SCUS_974.90 ]; then
    for f in ./*.iso "$HOME"/*.iso "$HOME"/Downloads/*.iso "$HOME"/Games/*.iso; do
        [ -f "$f" ] && { iso=$f; break; }
    done
    if [ -n "$iso" ]; then
        printf 'Use the disc image %s ? [Y/n] ' "$iso"
        read -r answer </dev/tty
        case "$answer" in [nN]*) iso= ;; esac
    fi
    if [ -z "$iso" ]; then
        printf 'Path of your Rogue Galaxy (USA) .iso: '
        read -r iso </dev/tty
    fi
fi
if [ -n "$iso" ]; then
    iso=$(readlink -f "$iso")
    [ -f "$iso" ] || die "No such file: $iso"
    set -- --iso "$iso"
fi

# The framework and Dear ImGui are submodules.
if [ -d .git ] && ! [ -e thirdparty/PS2Recomp/ps2xRuntime ]; then
    say "Fetching the submodules"
    git submodule update --init --recursive || die "git submodule update failed"
fi

missing=
for tool in clang clang++ cmake ninja python3 git make nasm xz pkg-config glslc; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done

if [ "$mode" = auto ]; then
    if [ -z "$missing" ]; then
        mode=native
    elif command -v distrobox >/dev/null 2>&1; then
        mode=container
    else
        die "Missing build tools:$missing
Install them (Debian/Ubuntu: clang lld cmake ninja-build python3 git make nasm
xz-utils pkg-config glslc, plus the X11, Wayland, Vulkan, udev, GL and audio
development packages; docs/BUILDING.md lists them) and run this again."
    fi
fi

if [ "$mode" = native ]; then
    say "Building here (CC=${CC:-clang})"
    python3 setup.py install "$@" || die "The build failed."
else
    command -v distrobox >/dev/null 2>&1 || die "distrobox is not installed."
    if ! distrobox list 2>/dev/null | grep -q '[| ]rg-build[ |]'; then
        say "Creating the build container rg-build (Ubuntu 24.04)"
        distrobox create --yes --name rg-build --image docker.io/library/ubuntu:24.04 \
            || die "Could not create the container."
    fi
    say "Installing the build tools in the container"
    # Run the whole list again every time: apt skips what is there already.
    distrobox enter rg-build -- sh -c '
        sudo apt-get update -qq &&
        sudo apt-get install -y -qq clang lld cmake ninja-build git python3 \
            pkg-config glslc make nasm xz-utils \
            libvulkan-dev mesa-vulkan-drivers vulkan-tools libgl-dev libegl-dev \
            libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev \
            libxi-dev libxfixes-dev libxss-dev libxkbcommon-dev libwayland-dev \
            libdecor-0-dev libudev-dev libasound2-dev libpulse-dev libdbus-1-dev' \
        || die "Installing the packages in the container failed."
    say "Building in the container"
    case "$iso" in
        "$HOME"/*|"") ;;
        *) echo "Note: $iso is outside your home folder; if the container cannot read it, copy it into your home and run this again." ;;
    esac
    distrobox enter rg-build -- env CC=clang CXX=clang++ python3 setup.py install "$@" \
        || die "The build failed."
fi

[ -x build/game/src/roguegalaxy ] || die "The build finished without build/game/src/roguegalaxy."
chmod +x rogue-galaxy.sh

# A menu entry (desktop mode and the application list).
here=$(pwd)
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$apps"
cat >"$apps/rogue-galaxy.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Rogue Galaxy
Comment=Rogue Galaxy Recompiled
Exec=$here/rogue-galaxy.sh
Path=$here
Terminal=false
Categories=Game;
EOF

say "Done"
cat <<EOF
Start it with  $here/rogue-galaxy.sh  or from the application menu.

On a Steam Deck, play in game mode, not from desktop mode (there Steam gives
the controller its mouse-and-keyboard layout):
  1. In desktop mode open Steam, Games > Add a Non-Steam Game to My Library,
     Browse, pick  $here/rogue-galaxy.sh  (choose "All Files" in the filter).
  2. Back to game mode: Rogue Galaxy is in the library, under Non-Steam.
Back + Start opens the game's menu (F1).
EOF
