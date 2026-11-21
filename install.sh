#!/bin/sh
# Rogue Galaxy Recompiled on Linux and the Steam Deck: what Install.bat does
# on Windows. Builds the game from your own disc image (setup.py install:
# verify, extract, recompile, build), then adds a menu entry.
#
#   ./install.sh [--iso FILE] [--container | --native] [--play]
#
# With every build tool on the PATH it builds right here. Without them (the
# Steam Deck's SteamOS has a read-only root and no compiler) it builds inside
# a distrobox container named rg-build, an Ubuntu 24.04 that it sets up; the
# game itself needs nothing from the container and runs on the host.
# Takes about 10 to 30 minutes, and 20 GB of disk (the extracted disc, the
# generated code, the build). --play starts the game when it is built.
# Started without a terminal (a launcher's Play, Steam, a file manager), it
# opens one to build in, then starts the game; once built, it starts the game.
cd "$(dirname "$(readlink -f "$0")")" || exit 1
here=$(pwd)

iso=
mode=auto
play=
others=
for arg; do
    case "$arg" in
        --play) play=1 ;;
        -h|--help) sed -n '2,15p' "$here/install.sh" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) others=1 ;;
    esac
done

say() { printf '\n== %s\n' "$*"; }
# In a window this script opened, the message stays until Enter.
die() {
    printf 'Error: %s\n' "$*" >&2
    if [ -n "$RG_OWN_WINDOW" ]; then
        printf '\nPress Enter to close this window.'
        read -r _ </dev/tty
    fi
    exit 1
}
# Without a terminal the message needs a window of its own: a dialog, else a
# notification, else xmessage.
gui_error() {
    printf 'Rogue Galaxy: %s\n' "$1" >&2
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --error "$1" --title "Rogue Galaxy" 2>/dev/null
    elif command -v zenity >/dev/null 2>&1; then
        zenity --error --text="$1" --title="Rogue Galaxy" 2>/dev/null
    elif command -v notify-send >/dev/null 2>&1; then
        notify-send -u critical "Rogue Galaxy" "$1" 2>/dev/null
    elif command -v xmessage >/dev/null 2>&1; then
        xmessage -center "Rogue Galaxy: $1" 2>/dev/null
    fi
}

# The game built from these sources, with its disc (disc/manifest.json comes
# last, once the whole disc is out; without it setup.py extracts again). A
# release archive names its commit in REVISION: one unpacked over an older
# install (an update, as QuiverLauncher does it: new files replace old ones,
# build/ stays) is not ready until it is built again. The same test is in
# rogue-galaxy.sh.
ready() {
    [ -x build/game/src/roguegalaxy ] && [ -e disc/SCUS_974.90 ] && [ -e disc/manifest.json ] || return 1
    ! [ -f REVISION ] || [ "$(cat REVISION)" = "$(cat build/installed-revision 2>/dev/null)" ]
}
# The script that opened this window still waits for it: not ended, not a
# zombie (whose command line is empty), not a new process given its number.
opener_waits() {
    if [ -r "/proc/$RG_OWN_WINDOW/cmdline" ]; then
        tr '\0' '\n' <"/proc/$RG_OWN_WINDOW/cmdline" | grep -q 'install\.sh$'
    else
        kill -0 "$RG_OWN_WINDOW" 2>/dev/null
    fi
}
# An install or update is running in this folder: the script holds
# build/install.lock, a build in the container build/container.lock (below).
# The same test is in rogue-galaxy.sh.
busy() {
    command -v flock >/dev/null 2>&1 || return 1
    for lock in build/install.lock build/container.lock; do
        [ -e "$lock" ] && ! flock -n "$lock" true && return 0
    done
    return 1
}

# Started by a launcher rather than from a terminal (QuiverLauncher's Play
# runs the first script it finds, this one, with no terminal and no
# arguments; so does a file manager): nowhere to show the build or ask for
# the disc image. With arguments (a script, a test) it runs where it is, as
# Install.bat does on Windows.
if [ -z "$RG_OWN_WINDOW" ] && [ -z "$others" ] && ! [ -t 0 ] && ! [ -t 1 ] \
    && { [ -n "$DISPLAY" ] || [ -n "$WAYLAND_DISPLAY" ]; }; then
    ready && exec sh ./rogue-galaxy.sh
    if busy; then
        gui_error "An install or update is already running in this folder, in another window. Let it end there."
        exit 1
    fi
    # The build in a terminal window. RG_OWN_WINDOW tells it which process
    # waits for it; most terminals return when it ends. Through sh, as the
    # scripts call each other: an archive unpacked without its modes leaves
    # them not runnable.
    set -- env RG_OWN_WINDOW=$$ sh "$here/install.sh" "$@"
    # The window it opens leaves its mark, build/own-window.<this PID>. A
    # terminal that could not start (an old gnome-terminal without --wait, no
    # display for it) says so only by its status: the next one found is
    # tried.
    mark=build/own-window.$$
    rm -f "$mark"
    found=
    status=0
    for term in konsole gnome-terminal ptyxis kgx xfce4-terminal xdg-terminal-exec \
        alacritty kitty foot x-terminal-emulator xterm; do
        command -v "$term" >/dev/null 2>&1 || continue
        found=1
        status=0
        case "$term" in
            konsole|kgx|alacritty|x-terminal-emulator|xterm) "$term" -e "$@" ;;
            gnome-terminal) gnome-terminal --wait -- "$@" ;;
            ptyxis) ptyxis -- "$@" ;;
            xfce4-terminal) xfce4-terminal --disable-server -x "$@" ;;
            *) "$term" "$@" ;;
        esac || status=$?
        { [ "$status" -eq 0 ] || [ -e "$mark" ]; } && break
    done
    if [ -z "$found" ]; then
        gui_error "Rogue Galaxy is not built yet, and no terminal program was found to build it in.
Open a terminal in $here and run  sh install.sh"
        exit 1
    fi
    ready && exec sh ./rogue-galaxy.sh
    # The window that ran showed its own error.
    if [ "$status" -ne 0 ] && ! [ -e "$mark" ]; then
        gui_error "No window could be opened to build the game in. Open a terminal in $here and run  sh install.sh"
        exit 1
    fi
    rm -f "$mark"
    exit 0
fi

while [ $# -gt 0 ]; do
    case "$1" in
        --iso) iso=$2; shift ;;
        --iso=*) iso=${1#--iso=} ;;
        --container) mode=container ;;
        --native) mode=native ;;
        --play) ;;
        *) die "Unknown option: $1 (see --help)" ;;
    esac
    shift
done

if [ -n "$RG_OWN_WINDOW" ]; then
    printf '%s\n' "Rogue Galaxy Recompiled: building the game from your disc image." \
        "It takes 10 to 30 minutes and about 20 GB. Keep this window open."
    mkdir -p build && : >"build/own-window.$RG_OWN_WINDOW"
fi

# One install or update at a time in a folder: a second one (Play pressed
# again, or rogue-galaxy.sh while one runs) would build over it, and the game
# would start half built; rogue-galaxy.sh looks at it too. Held on file
# descriptor 8 until this script ends, which no program it runs gets (8>&-):
# one that outlived the install (a dbus-daemon a dialog started, podman's
# conmon) would keep the folder locked.
if command -v flock >/dev/null 2>&1; then
    mkdir -p build
    exec 8>build/install.lock
    flock -n 8 || die "An install or update is already running in this folder, in another window. Let it end there."
    # A build left running in the container after its window closed holds
    # build/container.lock (below).
    if [ -e build/container.lock ] && ! flock -n build/container.lock true; then
        die "A build is still running in this folder, in the build container. Let it end, then run this again."
    fi
fi

# In a window this script opened (a launcher started it), the questions come
# as dialogs where there is a dialog program: a file picker for the disc
# image, and no typing on the Steam Deck in game mode.
dialogs=
if [ -n "$RG_OWN_WINDOW" ]; then
    command -v kdialog >/dev/null 2>&1 && dialogs=kdialog
    [ -z "$dialogs" ] && command -v zenity >/dev/null 2>&1 && dialogs=zenity
fi

# The disc image: given, or found in the usual folders, or asked for, unless
# the disc is out whole already (as setup.py install decides).
if [ -z "$iso" ] && ! { [ -e disc/manifest.json ] && [ -e disc/SCUS_974.90 ]; }; then
    for f in ./*.iso "$HOME"/*.iso "$HOME"/Downloads/*.iso "$HOME"/Games/*.iso; do
        [ -f "$f" ] && { iso=$f; break; }
    done
    if [ -n "$iso" ]; then
        case "$dialogs" in
            kdialog) kdialog --title "Rogue Galaxy" --yesno "Use the disc image $iso ?" 2>/dev/null 8>&- || iso= ;;
            zenity) zenity --title="Rogue Galaxy" --question --text="Use the disc image $iso ?" 2>/dev/null 8>&- || iso= ;;
            *)
                printf 'Use the disc image %s ? [Y/n] ' "$iso"
                read -r answer </dev/tty
                case "$answer" in [nN]*) iso= ;; esac ;;
        esac
    fi
    if [ -z "$iso" ]; then
        case "$dialogs" in
            kdialog) iso=$(kdialog --title "Your Rogue Galaxy (USA) disc image" \
                --getopenfilename "$HOME" "*.iso *.ISO|Disc images (*.iso)" 2>/dev/null 8>&-) ;;
            zenity) iso=$(zenity --title="Your Rogue Galaxy (USA) disc image" --file-selection \
                --file-filter="Disc images (*.iso) | *.iso *.ISO" 2>/dev/null 8>&-) ;;
            *)
                printf 'Path of your Rogue Galaxy (USA) .iso: '
                read -r iso </dev/tty ;;
        esac
        [ -n "$iso" ] || die "No disc image given."
    fi
fi
if [ -n "$iso" ]; then
    iso=$(readlink -f "$iso")
    [ -f "$iso" ] || die "No such file: $iso"
    set -- --iso "$iso"
fi

# A release archive unpacked over this folder (QuiverLauncher's update, or
# unzip by hand), once for each unpack: FILES lists its files, and an unpack
# writes it again (its change time, with REVISION, tells one unpack from the
# next, the same archive's too). What the code folders hold beyond them goes,
# as Update.bat does on Windows: what a newer version dropped, which the
# build would still take by pattern (patches/ps2recomp/*.patch, the
# framework's own globs). Kept: a submodule's .git, Python's caches, and the
# patch stamp and the files this version's patches create, which
# framework_patches.cmake removes itself when the framework is back
# unpatched (it is after an unpack). Then every file gets today's date: unzip
# gives them the archive's, older than the build, and the old objects would
# stay (a full build, as QuiverLauncher's unpack makes anyway).
unpacked="$(cat REVISION 2>/dev/null) $(stat -c %Z FILES 2>/dev/null)"
if [ -f FILES ] && [ -f REVISION ] \
    && [ "$unpacked" != "$(cat build/unpacked-archive 2>/dev/null)" ]; then
    mkdir -p build
    {
        cat FILES
        echo thirdparty/PS2Recomp/.rg-patches-applied
        grep '^patches/ps2recomp/[^/]*\.patch$' FILES | while IFS= read -r p; do
            [ -f "$p" ] && awk 'prev == "--- /dev/null" && sub(/^\+\+\+ b\//, "") { print "thirdparty/PS2Recomp/" $0 } { prev = $0 }' "$p"
        done
    } | LC_ALL=C sort >build/files.keep
    find src patches config cmake tools docs debug thirdparty \( -name .git -o -name __pycache__ \) -prune \
        -o -type f ! -name '*.pyc' -print 2>/dev/null | LC_ALL=C sort >build/files.have
    LC_ALL=C comm -23 build/files.have build/files.keep >build/files.stale
    if [ -s build/files.stale ]; then
        say "Removing the files this version no longer has"
        while IFS= read -r f; do
            rm -f "$f" && printf '  %s\n' "$f"
        done <build/files.stale
    fi
    tr '\n' '\0' <FILES | xargs -0 -r touch -c --
    printf '%s\n' "$unpacked" >build/unpacked-archive || die "Could not write build/unpacked-archive."
    rm -f build/files.keep build/files.have build/files.stale
fi

# The framework and Dear ImGui are submodules.
if [ -d .git ] && ! [ -e thirdparty/PS2Recomp/ps2xRuntime ]; then
    say "Fetching the submodules"
    git submodule update --init --recursive 8>&- || die "git submodule update failed"
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
    python3 setup.py install "$@" 8>&- || die "The build failed."
else
    command -v distrobox >/dev/null 2>&1 || die "distrobox is not installed."
    if ! distrobox list 2>/dev/null 8>&- | grep -q '[| ]rg-build[ |]'; then
        say "Creating the build container rg-build (Ubuntu 24.04)"
        distrobox create --yes --name rg-build --image docker.io/library/ubuntu:24.04 8>&- \
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
            libdecor-0-dev libudev-dev libasound2-dev libpulse-dev libdbus-1-dev' 8>&- \
        || die "Installing the packages in the container failed."
    say "Building in the container"
    case "$iso" in
        "$HOME"/*|"") ;;
        *) echo "Note: $iso is outside your home folder; if the container cannot read it, copy it into your home and run this again." ;;
    esac
    # The build holds build/container.lock in the container, while this
    # script keeps build/install.lock: closing this window can leave the
    # build running there (podman keeps it), and the folder stays busy until
    # it ends (-o: not the build's own processes; -E 75: busy).
    distrobox enter rg-build -- flock -n -o -E 75 build/container.lock \
        env CC=clang CXX=clang++ python3 setup.py install "$@" 8>&-
    status=$?
    [ "$status" -ne 75 ] || die "An install or update is already running in this folder, in another window. Let it end there."
    [ "$status" -eq 0 ] || die "The build failed."
fi

[ -x build/game/src/roguegalaxy ] || die "The build finished without build/game/src/roguegalaxy."
chmod +x install.sh rogue-galaxy.sh
if [ -f REVISION ]; then
    cp -f REVISION build/installed-revision || die "Could not write build/installed-revision."
fi

# A menu entry (desktop mode and the application list).
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$apps"
icons="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps"
mkdir -p "$icons" && cp -f src/launcher/rogue-galaxy.png "$icons/rogue-galaxy.png"
# Exec in quotes, for a folder with spaces. The string's own escape (\\ for
# a backslash) comes before the quoting's, so in the quotes " ` $ take two
# backslashes and a backslash four, and % doubles; Path takes the string's
# escape only.
launcher=$(printf '%s\n' "$here/rogue-galaxy.sh" | sed -e 's/\\/\\\\\\\\/g' -e 's/["`$]/\\\\&/g' -e 's/%/%%/g')
folder=$(printf '%s\n' "$here" | sed 's/\\/\\\\/g')
cat >"$apps/rogue-galaxy.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Rogue Galaxy
Comment=Rogue Galaxy Recompiled
Exec="$launcher"
Path=$folder
Terminal=false
Icon=rogue-galaxy
Categories=Game;
EOF

say "Done"
if [ -n "$RG_OWN_WINDOW" ]; then
    # The script that opened this window starts the game once it closes; a
    # terminal that did not wait for this one left the start to it.
    opener_waits && exit 0
    play=1
fi
if [ "$play" = 1 ]; then
    ready || die "The build finished, but the game is not ready to start (disc/manifest.json?)."
    # The install is over: the game must not hold its lock.
    exec 8>&-
    if [ -n "$RG_OWN_WINDOW" ]; then
        # This window closes now, and its hangup must not reach the game:
        # ignored from here, and the game in a session of its own.
        trap '' HUP
        if command -v setsid >/dev/null 2>&1; then
            setsid sh ./rogue-galaxy.sh </dev/null >/dev/null 2>&1 &
        else
            sh ./rogue-galaxy.sh </dev/null >/dev/null 2>&1 &
        fi
        exit 0
    fi
    exec sh ./rogue-galaxy.sh
fi
cat <<EOF
Start it with  $here/rogue-galaxy.sh  or from the application menu.

On a Steam Deck, play in game mode, not from desktop mode (there Steam gives
the controller its mouse-and-keyboard layout):
  1. In desktop mode open Steam, Games > Add a Non-Steam Game to My Library,
     Browse, pick  $here/rogue-galaxy.sh  (choose "All Files" in the filter).
  2. Back to game mode: Rogue Galaxy is in the library, under Non-Steam.
Back + Start opens the game's menu (F1).
EOF
