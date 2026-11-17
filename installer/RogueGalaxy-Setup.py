#!/usr/bin/env python3
"""Rogue Galaxy for the Steam Deck: the installer.

One file, to run once in desktop mode (double-click, "Execute"). One page
asks for your disc image, where to put the game and the options; then it
downloads the game's sources, builds them on the Deck (in a container, 10 to 30 minutes), and adds the game
to Steam. Afterwards it is not needed any more: the game starts on its own
home screen, which updates it, in game mode.

Needs what SteamOS has: python3 with GTK 4 (PyGObject), curl, git, distrobox.

For tests: --iso FILE, --dir FOLDER, --zip ARCHIVE (a release's source
archive instead of the download), --no-steam, --start (skip the pages),
--shots FOLDER (a PNG of the window; --tour also shows the European option
and the install page). RG_UPDATE_REPO=owner/name and
RG_UPDATE_API=URL point the release lookup elsewhere; RG_SETUP_REF=BRANCH is
the branch cloned when the repository has no release yet.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import urllib.request
import zipfile
from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("Gsk", "4.0")
gi.require_version("Graphene", "1.0")
from gi.repository import Gdk, Gio, GLib, Graphene, Gsk, Gtk  # noqa: E402

REPO = os.environ.get("RG_UPDATE_REPO", "touyoo0/deck-galaxy")
API = os.environ.get("RG_UPDATE_API", f"https://api.github.com/repos/{REPO}/releases?per_page=10")
REF = os.environ.get("RG_SETUP_REF", "steamdeck-only")
NEED_GB = 24  # the extracted disc, the generated code, the build
ISO_BYTES = 6_967_721_984  # Redump's image of the USA release

CSS = b"""
window { background: #04061a; color: #f2f3fc; }
.brand { font-size: 40px; font-weight: 800; letter-spacing: 7px; color: #f4cb52;
         text-shadow: 0 0 18px rgba(244,203,82,0.55), 0 3px 0 #6b3d00; }
.tagline { font-size: 12px; font-weight: 700; letter-spacing: 4px; color: #ff8c7a; }
.title { font-size: 24px; font-weight: 700; color: #f2f3fc; }
.subtitle { font-size: 14px; color: #b4bde0; }
.muted { color: #9ea8cc; }
.section { font-size: 17px; font-weight: 700; color: #f2f3fc; }
.badge { background-image: linear-gradient(#ffdf7a, #f0a82a); color: #1a1500; font-weight: 800;
         border-radius: 15px; min-width: 30px; min-height: 30px;
         box-shadow: 0 0 14px rgba(244,203,82,0.6); }
.good { color: #70de8e; }
.warn { color: #f6b640; }
.bad { color: #ff7b72; }
.card { background: rgba(8,12,42,0.74); border: 1px solid rgba(244,203,82,0.30); border-radius: 14px;
        padding: 12px; box-shadow: 0 10px 34px rgba(0,0,0,0.5); }
button { background: rgba(28,40,110,0.65); color: #f2f3fc; border: 1px solid rgba(127,212,255,0.4);
         border-radius: 9px; padding: 8px 18px; box-shadow: none; background-image: none; }
button:hover { background: rgba(48,66,160,0.8); border-color: #7fd4ff; }
button:disabled { color: #5f6890; background: rgba(16,21,47,0.7); border-color: rgba(127,212,255,0.12); }
button.suggested-action { background-image: linear-gradient(#ffdf7a, #f0a82a); color: #1a1500; border-color: #ffe9a0;
                          font-size: 17px; font-weight: 800; padding: 12px 44px; border-radius: 12px;
                          box-shadow: 0 0 22px rgba(244,203,82,0.45); }
button.suggested-action:hover { background-image: linear-gradient(#ffeaa0, #ffbe3a); }
button.suggested-action:disabled { background-image: none; background: rgba(74,66,36,0.75); color: #8a8160;
                                   border-color: transparent; box-shadow: none; }
progressbar trough { background: rgba(26,32,70,0.9); min-height: 14px; border-radius: 8px;
                     border: 1px solid rgba(127,212,255,0.3); }
progressbar progress { background-image: linear-gradient(90deg, #ffb13b, #ffe37a); min-height: 14px;
                       border-radius: 8px; box-shadow: 0 0 14px rgba(244,203,82,0.7); }
checkbutton { color: #f2f3fc; }
check, radio { background: rgba(16,22,58,0.9); border: 1px solid rgba(127,212,255,0.5); }
check:checked, radio:checked { background: #f4cb52; color: #1a1500; border-color: #ffe9a0; }
expander title { color: #b4bde0; }
textview, textview text { background: #04071a; color: #9ea8cc; font-family: monospace; font-size: 12px; }
"""


# ---------------------------------------------------------------- helpers


def free_gb(path: Path) -> float:
    probe = path
    while not probe.exists() and probe != probe.parent:
        probe = probe.parent
    return shutil.disk_usage(probe).free / 2**30


def storage_choices() -> list[tuple[str, Path]]:
    """Where the game can go: the Deck's own storage, and each card or drive."""
    choices = [("Internal storage", Path.home() / "Games" / "RogueGalaxy")]
    seen = set()
    for base in (Path("/run/media"), Path("/run/media/deck")):
        if not base.is_dir():
            continue
        for entry in sorted(base.iterdir()):
            if entry.is_dir() and os.path.ismount(entry) and entry.resolve() not in seen:
                seen.add(entry.resolve())
                label = "SD card" if entry.name.upper().startswith("SD") or entry.name == "mmcblk0p1" else entry.name
                choices.append((label, entry / "Games" / "RogueGalaxy"))
    return choices


def looks_like_iso(path: Path) -> str | None:
    """None when it may be the disc image, else what is wrong with it."""
    if not path.is_file():
        return "That file does not exist."
    if path.suffix.lower() != ".iso":
        return "It must be an uncompressed .iso (turn a .chd or .cso back into an .iso first)."
    size = path.stat().st_size
    if size < 4 * 2**30:
        return "That is too small for a PlayStation 2 DVD image."
    with open(path, "rb") as f:
        f.seek(0x8001)
        if f.read(5) != b"CD001":
            return "That is not a disc image."
    return None


class Failure(Exception):
    pass


class Cancelled(Exception):
    pass


# ---------------------------------------------------------------- the job


class Job(threading.Thread):
    """Everything after the pages: fetch, build, extras, Steam."""

    # What each of install.sh's / setup.py's "== ..." lines is worth on the
    # bar, and where its ninja counter ("[12/340]") fills it.
    PHASES = [
        ("== Creating the build container", 0.04, 0.10),
        ("== Installing the build tools", 0.10, 0.16),
        ("== Building in the container", 0.16, 0.16),
        ("== verify", 0.16, 0.17),
        ("== extract", 0.17, 0.24),
        ("== patches", 0.24, 0.25),
        ("== build ps2_recomp", 0.25, 0.34),
        ("== recompile", 0.34, 0.38),
        ("== configure build/game", 0.38, 0.40),
        ("== build the game", 0.40, 0.94),
    ]

    def __init__(self, cfg: argparse.Namespace, ui: "Window"):
        super().__init__(daemon=True)
        self.cfg = cfg
        self.ui = ui
        self.lo, self.hi = 0.0, 0.0
        self.tail: list[str] = []
        self.cancelled = False
        self.killed = threading.Event()  # the build's processes are gone
        self.proc: subprocess.Popen | None = None
        folder = Path(cfg.dir) if cfg.dir else None
        # A folder this install creates (new, or empty) goes away if it is cancelled; one with an
        # earlier install in it stays as it was.
        self.fresh = bool(folder and not (folder / "install.sh").exists()
                          and (not folder.exists() or not any(folder.iterdir())))

    def say(self, text: str, fraction: float | None = None) -> None:
        GLib.idle_add(self.ui.on_progress, text, fraction)

    def log(self, line: str) -> None:
        GLib.idle_add(self.ui.on_log, line)
        self.tail = (self.tail + [line])[-40:] if line.strip() else self.tail
        try:  # kept for a report: <folder>/build/setup.log
            (self.cfg.dir / "build").mkdir(parents=True, exist_ok=True)
            with open(self.cfg.dir / "build" / "setup.log", "a", encoding="utf-8") as f:
                f.write(line + "\n")
        except OSError:
            pass

    def why(self) -> str:
        """The last lines of output that look like an explanation."""
        lines = [l for l in self.tail if l.strip() and not l.startswith(("[", "== ", "$ "))]
        return " ".join(lines[-2:])[:300]

    def run(self) -> None:
        try:
            self.fetch()
            self.build()
            self.extras()
            self.steam()
        except Cancelled:
            self.killed.wait(90)
            self.cleanup()
            GLib.idle_add(self.ui.on_cancelled)
            return
        except Failure as error:
            GLib.idle_add(self.ui.on_failed, str(error))
            return
        except Exception as error:  # a bug here must still reach the window
            GLib.idle_add(self.ui.on_failed, f"Unexpected error: {error}")
            return
        GLib.idle_add(self.ui.on_done)

    # -- running a program, its output line by line
    def stream(self, command: list[str], cwd: Path, on_line=None) -> int:
        if self.cancelled:
            raise Cancelled
        self.log("$ " + " ".join(command))
        # Its own process group, so that cancelling can end it and what it started.
        process = subprocess.Popen(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL, text=True, bufsize=1, errors="replace",
                                   start_new_session=True)
        self.proc = process
        assert process.stdout is not None
        for raw in process.stdout:
            line = raw.rstrip()
            self.log(line)
            if on_line:
                on_line(line)
        status = process.wait()
        if self.cancelled:
            raise Cancelled
        return status

    # -- cancelling: end the build, in the container too, then delete what it made
    def cancel(self) -> None:
        """From another thread: stop everything. run() then cleans up."""
        self.cancelled = True
        try:
            self.kill_build()
        finally:
            self.killed.set()

    def kill_build(self) -> None:
        folder = str(Path(self.cfg.dir).resolve()) if self.cfg.dir else ""
        if self.proc and self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except OSError:
                pass
        if not folder:
            return
        # The build runs in the distrobox container, which outlives this window: end every process whose
        # working directory is inside the install folder. The container sees this computer's folders under
        # /run/host, and shares its process list (if it did not, the same is done from inside it).
        folders = (folder, "/run/host" + folder)
        for _ in range(3):
            for entry in Path("/proc").iterdir():
                if not entry.name.isdigit() or int(entry.name) == os.getpid():
                    continue
                try:
                    cwd = os.readlink(entry / "cwd")
                except OSError:
                    continue
                if any(cwd == f or cwd.startswith(f + "/") for f in folders):
                    try:
                        os.kill(int(entry.name), signal.SIGKILL)
                    except OSError:
                        pass
            time.sleep(0.5)
        script = ('for p in /proc/[0-9]*; do c=$(readlink "$p/cwd" 2>/dev/null); for d in "$@"; do case "$c" in '
                  '"$d"|"$d"/*) [ "${p#/proc/}" != "$$" ] && kill -KILL "${p#/proc/}" 2>/dev/null;; esac; done; done')
        if shutil.which("distrobox"):
            try:
                subprocess.run(["distrobox", "enter", "rg-build", "--", "sh", "-c", script, "sh", *folders],
                               cwd=Path.home(), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, timeout=45)
            except (OSError, subprocess.SubprocessError):
                pass

    def cleanup(self) -> None:
        """What a cancelled install leaves: the folder it created, the partial download, the menu entry."""
        folder = Path(self.cfg.dir) if self.cfg.dir else None
        if not folder:
            return
        (folder.parent / f".{folder.name}-download.zip").unlink(missing_ok=True)
        if not self.fresh:
            return
        entry = Path.home() / ".local/share/applications/rogue-galaxy.desktop"
        try:
            if entry.exists() and str(folder) in entry.read_text(errors="replace"):
                entry.unlink()
        except OSError:
            pass
        shutil.rmtree(folder, ignore_errors=True)

    # -- 1. the sources
    def fetch(self) -> None:
        target: Path = self.cfg.dir
        self.say("Getting the game's files", 0.0)
        if target.exists() and any(target.iterdir()) and not (target / "install.sh").exists():
            raise Failure(f"{target} is not empty. Pick an empty folder, or one with an earlier install.")
        target.mkdir(parents=True, exist_ok=True)
        if (target / "install.sh").exists():
            self.log("An earlier install is here: going on with it.")
            return
        archive = self.cfg.zip
        if not archive:
            archive = self.download_release(target)
        if archive:
            self.unpack(Path(archive), target)
            return
        self.say(f"No release yet: cloning the repository ({REF})", None)
        status = self.stream(["git", "clone", "--recurse-submodules", "--branch", REF,
                              f"https://github.com/{REPO}.git", str(target)], target.parent)
        if status != 0 or not (target / "install.sh").exists():
            raise Failure("Could not download the game's files. Is the Deck online?")

    def download_release(self, target: Path) -> Path | None:
        try:
            request = urllib.request.Request(API, headers={"Accept": "application/vnd.github+json",
                                                           "User-Agent": "RogueGalaxySetup"})
            with urllib.request.urlopen(request, timeout=20) as response:
                releases = json.load(response)
        except Exception as error:
            self.log(f"Release list unavailable: {error}")
            return None
        release = releases[0] if releases else {}
        assets = {a["name"]: a["browser_download_url"] for a in release.get("assets", [])}
        zips = sorted(n for n in assets if n.startswith("RogueGalaxy-") and n.endswith(".zip"))
        if not zips or zips[-1] + ".sha256" not in assets:
            return None
        name = zips[-1]
        tag = release.get("tag_name", "")
        destination = target.parent / f".{target.name}-download.zip"
        self.say(f"Downloading {tag}", 0.0)
        digest = hashlib.sha256()
        with urllib.request.urlopen(assets[name], timeout=30) as response, open(destination, "wb") as out:
            total = int(response.headers.get("Content-Length") or 0)
            done = 0
            while chunk := response.read(1 << 20):
                if self.cancelled:
                    raise Cancelled
                out.write(chunk)
                digest.update(chunk)
                done += len(chunk)
                if total:
                    self.say(f"Downloading {tag}", 0.04 * done / total)
        with urllib.request.urlopen(assets[name] + ".sha256", timeout=30) as response:
            want = response.read().decode().split()[0]
        if want != digest.hexdigest():
            destination.unlink(missing_ok=True)
            raise Failure("The download does not match its checksum. Try again.")
        return destination

    def unpack(self, archive: Path, target: Path) -> None:
        self.say("Unpacking", 0.04)
        with zipfile.ZipFile(archive) as z:
            members = [i for i in z.infolist() if not i.is_dir()]
            for info in members:
                if self.cancelled:
                    raise Cancelled
                name = info.filename.split("/", 1)[1] if "/" in info.filename else ""
                if not name:
                    continue
                path = target / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(z.read(info))
                path.chmod(((info.external_attr >> 16) & 0o777) or 0o644)
        if not (target / "install.sh").exists():
            raise Failure("That archive is not a game release.")
        # What update.sh keeps to update only what changed, later.
        (target / "build").mkdir(exist_ok=True)
        try:
            revision = (target / "REVISION").read_text().strip()
            changed = int((target / "FILES").stat().st_ctime)
            (target / "build" / "unpacked-archive").write_text(f"{revision} {changed}\n")
            (target / "build" / "submodules-installed").write_text((target / "SUBMODULES").read_text())
        except OSError:
            pass
        if str(archive).endswith("-download.zip"):
            archive.unlink(missing_ok=True)

    # -- 2. the build
    def build(self) -> None:
        target: Path = self.cfg.dir
        self.say("Starting the build", 0.04)
        command = ["sh", "install.sh", "--container"]
        if self.cfg.iso:
            command += ["--iso", str(self.cfg.iso)]
        self.lo, self.hi = 0.04, 0.16

        def on_line(line: str) -> None:
            if line.startswith("["):
                counts = line[1:].split("]", 1)[0]
                if "/" in counts:
                    try:
                        n, m = (int(x) for x in counts.split("/", 1))
                    except ValueError:
                        return
                    if m:
                        self.say(f"Building {n}/{m}", self.lo + (self.hi - self.lo) * n / m)
                return
            for prefix, lo, hi in self.PHASES:
                if line.startswith(prefix):
                    self.lo, self.hi = lo, hi
                    self.say(line[3:].split(" (")[0].strip().capitalize(), lo if lo == hi else None)
                    return

        status = self.stream(command, target, on_line)
        if status != 0 or not (target / "build/game/src/roguegalaxy").exists():
            raise Failure(f"The build failed: {self.why()}\nRunning this installer again goes on where it "
                          "stopped. The whole log is in build/setup.log.")

    # -- 3. the optional extras (another disc each)
    def extras(self) -> None:
        for flag, iso, command, label in (
            ("japanese", self.cfg.japanese, "extract-japanese", "Japanese voices and movies"),
            ("european", self.cfg.european, "extract-european", "European texts"),
        ):
            if not iso:
                continue
            self.say(f"Adding the {label}", 0.95)
            status = self.stream(["distrobox", "enter", "rg-build", "--", "python3", "setup.py", command,
                                  "--iso", str(iso)], self.cfg.dir)
            if status != 0:
                self.log(f"The {label} could not be added; the game works without them.")

    # -- 4. Steam
    def in_steam(self, script: Path) -> bool:
        """Whether some Steam account already has this script as a non-Steam game."""
        for vdf in (Path.home() / ".local/share/Steam/userdata").glob("*/config/shortcuts.vdf"):
            try:
                data = vdf.read_bytes()
            except OSError:
                continue
            # The same file may be named by another path (/run/media/SD, /run/media/deck/SD).
            for match in re.finditer(rb"\x01[Ee]xe\x00(.*?)\x00", data):
                exe = Path(match.group(1).decode(errors="replace").strip('"'))
                try:
                    if exe.exists() and script.exists() and os.path.samefile(exe, script):
                        return True
                except OSError:
                    continue
        return False

    def steam(self) -> None:
        if not self.cfg.steam:
            return
        script = self.cfg.dir / "rogue-galaxy.sh"
        if self.in_steam(script):
            self.log("The game is already in Steam: not added again.")
            return
        self.say("Adding the game to Steam", 0.98)
        # install.sh wrote the menu entry (named "Rogue Galaxy", with its icon);
        # Steam takes its name, program and folder from it, with no question
        # asked, as long as it is running (desktop mode).
        desktop = Path.home() / ".local/share/applications/rogue-galaxy.desktop"
        tool = shutil.which("steamos-add-to-steam")
        if not tool or not desktop.exists():
            self.log("Could not add the game to Steam by itself: add rogue-galaxy.sh as a non-Steam game.")
            return
        self.stream([tool, str(desktop)], self.cfg.dir)


# ---------------------------------------------------------------- the sky
# The window's background, drawn here (no game art: it is all gradients):
# a deep-space sky with nebulae, twinkling stars, a ringed planet with its
# moon, and now and then a shooting star.


def rgba(r: float, g: float, b: float, a: float = 1.0) -> Gdk.RGBA:
    return Gdk.RGBA(red=r / 255, green=g / 255, blue=b / 255, alpha=a)


def stops(*pairs) -> list:
    out = []
    for offset, color in pairs:
        stop = Gsk.ColorStop()
        stop.offset = offset
        stop.color = color
        out.append(stop)
    return out


def rect(x: float, y: float, w: float, h: float) -> Graphene.Rect:
    return Graphene.Rect().init(x, y, w, h)


def point(x: float, y: float) -> Graphene.Point:
    return Graphene.Point().init(x, y)


def circle(cx: float, cy: float, r: float) -> Gsk.RoundedRect:
    box = Gsk.RoundedRect()
    box.init_from_rect(rect(cx - r, cy - r, 2 * r, 2 * r), r)
    return box


def art_files() -> list[Path]:
    """The theme's pictures: installer/art next to this file (the AppImage carries it),
    and ~/.local/share/RogueGalaxy/theme for pictures of your own."""
    found: list[Path] = []
    for folder in (Path(__file__).resolve().parent / "art", Path.home() / ".local/share/RogueGalaxy/theme"):
        if folder.is_dir():
            found += sorted(p for p in folder.iterdir() if p.suffix.lower() in (".jpg", ".jpeg", ".png"))
    return found


class Sky(Gtk.Widget):
    __gtype_name__ = "RogueGalaxySky"
    PERIOD, FADE = 9.0, 1.6  # seconds a picture stays, and its cross-fade

    def __init__(self) -> None:
        super().__init__(hexpand=True, vexpand=True)
        self.art: list[Gdk.Texture] = []
        for path in art_files():
            try:
                self.art.append(Gdk.Texture.new_from_filename(str(path)))
            except GLib.Error:
                continue
        # wide pictures first: they fill the window, the tall ones become a poster
        self.art.sort(key=lambda tex: tex.get_width() / tex.get_height() < 1.25)
        rnd = random.Random(7)
        colors = ((255, 255, 255), (255, 255, 255), (170, 205, 255), (255, 226, 180))
        self.stars = [(rnd.random(), rnd.random(), rnd.choice((1, 1, 1, 2, 2, 3)), rnd.random() * 6.28,
                       0.6 + rnd.random() * 1.8, rnd.choice(colors)) for _ in range(260)]
        self.t0 = time.monotonic()
        GLib.timeout_add(40 if self.art else 66, self.tick)

    def tick(self) -> bool:
        self.queue_draw()
        return True

    def ring(self, snap: Gtk.Snapshot, cx: float, cy: float, radius: float, front: bool) -> None:
        snap.save()
        snap.translate(point(cx, cy))
        snap.rotate(-18)
        if front:  # the half of the ring that passes in front of the planet
            snap.push_clip(rect(-radius * 4, 0, radius * 8, radius * 4))
        for scale, width, color in ((2.25, 2.0, rgba(255, 228, 160, 0.9)), (2.0, 11.0, rgba(255, 190, 110, 0.25)),
                                    (1.7, 3.0, rgba(255, 214, 150, 0.65))):
            rx, ry = radius * scale, radius * scale * 0.2
            corner = Graphene.Size().init(rx, ry)
            box = Gsk.RoundedRect()
            box.init(rect(-rx, -ry, 2 * rx, 2 * ry), corner, corner, corner, corner)
            snap.append_border(box, [width] * 4, [color] * 4)
        if front:
            snap.pop()
        snap.restore()

    def picture(self, snap: Gtk.Snapshot, tex: Gdk.Texture, w: float, h: float, progress: float,
                alpha: float) -> None:
        """One artwork: a wide one fills the window; a tall one is a poster on the right, over a dimmed
        copy of itself. It drifts in slowly (progress 0 to 1)."""
        tw, th = tex.get_width(), tex.get_height()
        zoom = 1.0 + 0.07 * progress
        flt = Gsk.ScalingFilter.TRILINEAR
        snap.push_clip(rect(0, 0, w, h))
        snap.push_opacity(alpha)
        if tw / th >= 1.25:
            scale = max(w / tw, h / th) * zoom
            dw, dh = tw * scale, th * scale
            snap.append_scaled_texture(tex, flt, rect((w - dw) / 2, (h - dh) / 2, dw, dh))
        else:
            scale = max(w / tw, h / th)
            dw, dh = tw * scale, th * scale
            snap.push_opacity(0.4)
            snap.append_scaled_texture(tex, flt, rect((w - dw) / 2, (h - dh) / 2, dw, dh))
            snap.pop()
            scale = h / th * zoom
            dw, dh = tw * scale, th * scale
            snap.append_scaled_texture(tex, flt, rect(w - dw - 20, (h - dh) / 2, dw, dh))
        snap.pop()
        snap.pop()

    def draw_art(self, snap: Gtk.Snapshot, w: float, h: float, t: float) -> None:
        count = len(self.art)
        index = int(t // self.PERIOD) % count
        local = t % self.PERIOD
        snap.append_color(rgba(3, 5, 22), rect(0, 0, w, h))
        fading = count > 1 and t >= self.PERIOD  # the first picture appears at once
        if fading and local < self.FADE:  # the previous one, finishing under the new one
            self.picture(snap, self.art[(index - 1) % count], w, h, 1.0, 1.0)
        self.picture(snap, self.art[index], w, h, local / self.PERIOD,
                     min(1.0, local / self.FADE) if fading else 1.0)
        # dark on the left, where the window's cards are; the picture shows on the right
        full = rect(0, 0, w, h)
        snap.append_linear_gradient(full, point(0, 0), point(w, 0),
                                    stops((0, rgba(3, 5, 22, 0.92)), (0.42, rgba(3, 5, 22, 0.60)),
                                          (0.68, rgba(3, 5, 22, 0.12)), (1, rgba(3, 5, 22, 0))))
        snap.append_linear_gradient(full, point(0, 0), point(0, h),
                                    stops((0, rgba(3, 5, 22, 0.35)), (0.25, rgba(3, 5, 22, 0)),
                                          (1, rgba(3, 5, 22, 0.6))))

    def do_snapshot(self, snap: Gtk.Snapshot) -> None:
        w, h = float(self.get_width()), float(self.get_height())
        if w < 2 or h < 2:
            return
        t = time.monotonic() - self.t0
        if self.art:
            self.draw_art(snap, w, h, t)
        else:
            self.draw_space(snap, w, h, t)

    def draw_space(self, snap: Gtk.Snapshot, w: float, h: float, t: float) -> None:
        """No picture found: the drawn sky."""
        full = rect(0, 0, w, h)
        snap.append_linear_gradient(full, point(0, 0), point(0, h),
                                    stops((0, rgba(3, 5, 22)), (0.55, rgba(9, 8, 42)), (1, rgba(32, 12, 54))))
        for fx, fy, size, (r, g, b, a) in ((0.12, 0.18, 0.60, (40, 110, 255, 0.24)),
                                           (0.88, 0.30, 0.65, (190, 40, 200, 0.22)),
                                           (0.50, 1.05, 0.75, (255, 120, 60, 0.20))):
            snap.append_radial_gradient(full, point(fx * w, fy * h), size * w, size * w * 0.75, 0, 1,
                                        stops((0, rgba(r, g, b, a)), (1, rgba(r, g, b, 0))))
        for x, y, size, phase, speed, (r, g, b) in self.stars:
            alpha = 0.35 + 0.65 * (0.5 + 0.5 * math.sin(t * speed + phase))
            snap.append_color(rgba(r, g, b, alpha), rect(x * w, y * h, size, size))
            if size == 3:
                snap.append_radial_gradient(rect(x * w - 8, y * h - 8, 16, 16), point(x * w + 1, y * h + 1), 8, 8, 0, 1,
                                            stops((0, rgba(r, g, b, 0.35 * alpha)), (1, rgba(r, g, b, 0))))

        # the planet, to the right, with its ring passing behind then in front
        radius = min(w, h) * 0.27
        cx, cy = w * 0.78, h * 0.62
        snap.append_radial_gradient(rect(cx - radius * 1.6, cy - radius * 1.6, radius * 3.2, radius * 3.2),
                                    point(cx, cy), radius * 1.6, radius * 1.6, 0, 1,
                                    stops((0, rgba(255, 150, 80, 0)), (0.62, rgba(255, 140, 80, 0)),
                                          (0.66, rgba(255, 160, 90, 0.35)), (1, rgba(255, 140, 80, 0))))
        self.ring(snap, cx, cy, radius, False)
        snap.push_rounded_clip(circle(cx, cy, radius))
        snap.append_radial_gradient(rect(cx - radius, cy - radius, 2 * radius, 2 * radius),
                                    point(cx - radius * 0.4, cy - radius * 0.45), radius * 1.8, radius * 1.8, 0, 1,
                                    stops((0, rgba(255, 200, 125)), (0.28, rgba(226, 104, 62)),
                                          (0.62, rgba(112, 40, 100)), (1, rgba(10, 5, 30))))
        for offset, height, alpha in ((0.18, 0.05, 0.10), (0.34, 0.09, 0.08), (0.52, 0.04, 0.12),
                                      (0.66, 0.08, 0.07)):
            snap.append_color(rgba(255, 230, 190, alpha),
                              rect(cx - radius, cy - radius + offset * 2 * radius, 2 * radius, height * 2 * radius))
        snap.pop()
        self.ring(snap, cx, cy, radius, True)
        mx, my, mr = cx - radius * 1.55, cy - radius * 1.05, radius * 0.16
        snap.push_rounded_clip(circle(mx, my, mr))
        snap.append_radial_gradient(rect(mx - mr, my - mr, 2 * mr, 2 * mr), point(mx - mr * 0.4, my - mr * 0.4),
                                    mr * 1.7, mr * 1.7, 0, 1,
                                    stops((0, rgba(230, 240, 255)), (0.5, rgba(120, 140, 200)),
                                          (1, rgba(12, 14, 40))))
        snap.pop()

        # a shooting star every 11 seconds
        step = (t % 11.0) / 1.1
        if step < 1.0:
            snap.save()
            snap.translate(point(w * (0.15 + 0.4 * ((int(t / 11.0) * 0.37) % 1)) + step * w * 0.35,
                                 h * 0.08 + step * h * 0.28))
            snap.rotate(34)
            snap.append_linear_gradient(rect(-150, 0, 150, 2), point(-150, 0), point(0, 0),
                                        stops((0, rgba(255, 255, 255, 0)), (1, rgba(255, 255, 255, 1 - step))))
            snap.restore()


# ---------------------------------------------------------------- the window


class Window(Gtk.ApplicationWindow):
    """One page for everything to choose, then the same window shows the install."""

    def __init__(self, app: Gtk.Application, cfg: argparse.Namespace):
        super().__init__(application=app, title="Rogue Galaxy: install")
        self.cfg = cfg
        self.custom_dir: Path | None = None
        self.job: Job | None = None
        self.pulse_id = 0
        self.set_default_size(1100, 760)
        self.running = False
        self.connect("close-request", self.on_close_request)
        provider = Gtk.CssProvider()
        provider.load_from_data(CSS)
        Gtk.StyleContext.add_provider_for_display(self.get_display(), provider,
                                                  Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        self.stack = Gtk.Stack(transition_type=Gtk.StackTransitionType.CROSSFADE, vexpand=True, hexpand=True)
        self.stack.add_named(self.build_setup(), "setup")
        self.stack.add_named(self.build_install(), "install")
        root = Gtk.Overlay(child=Sky())
        root.add_overlay(self.stack)
        self.set_child(root)
        self.update_buttons()
        if cfg.shots:
            GLib.timeout_add(700, self.shoot, "setup")
            if cfg.tour:
                GLib.timeout_add(1500, self.tour_european)
                GLib.timeout_add(3000, self.tour_install)
        if cfg.start:
            GLib.idle_add(self.begin)
        if cfg.test_cancel:  # a test: cancel as the dialog's second button would
            GLib.timeout_add_seconds(cfg.test_cancel, lambda: self.cancel_install() or False)

    # -- building blocks
    def section(self, number: int, title: str, hint: str) -> Gtk.Box:
        """A card with a numbered heading; the content is appended to it."""
        card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        card.add_css_class("card")
        head = Gtk.Box(spacing=12)
        badge = Gtk.Label(label=str(number), valign=Gtk.Align.CENTER)
        badge.add_css_class("badge")
        texts = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2, hexpand=True)
        name = Gtk.Label(label=title, xalign=0)
        name.add_css_class("section")
        note = Gtk.Label(label=hint, xalign=0, wrap=True, max_width_chars=58)
        note.add_css_class("muted")
        texts.append(name)
        texts.append(note)
        head.append(badge)
        head.append(texts)
        card.append(head)
        return card

    def header(self, text: str | None = None) -> Gtk.Box:
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        brand = Gtk.Label(label="ROGUE GALAXY", xalign=0)
        brand.add_css_class("brand")
        tagline = Gtk.Label(label="FOR THE STEAM DECK   \u00b7   INSTALLER", xalign=0)
        tagline.add_css_class("tagline")
        box.append(brand)
        box.append(tagline)
        if text:
            sub = Gtk.Label(label=text, xalign=0, wrap=True, max_width_chars=58, margin_top=8)
            sub.add_css_class("subtitle")
            box.append(sub)
        return box

    def beside_art(self, column: Gtk.Widget) -> Gtk.Box:
        """The column on the left at its own width; the rest of the window shows the picture."""
        page = Gtk.Box()
        page.append(column)
        page.append(Gtk.Box(hexpand=True))
        return page

    def build_setup(self) -> Gtk.Widget:
        content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10,
                          margin_top=14, margin_bottom=6, margin_start=56, margin_end=24)
        content.append(self.header("Built on this Deck from your own disc image; nothing leaves it. Afterwards you "
                                   "play from Steam, and the game updates itself."))

        # 1. the disc image
        card = self.section(1, "Your disc image", "USA release, serial SCUS-97490, an uncompressed .iso (about 7 GB).")
        row = Gtk.Box(spacing=12)
        choose = Gtk.Button(label="Choose the .iso...")
        choose.connect("clicked", lambda *_: self.pick_file(self.set_iso, "Your Rogue Galaxy (USA) disc image"))
        self.iso_label = Gtk.Label(label="No image chosen", xalign=0, hexpand=True, wrap=True, max_width_chars=58)
        self.iso_label.add_css_class("muted")
        row.append(choose)
        row.append(self.iso_label)
        self.iso_state = Gtk.Label(label="", xalign=0, wrap=True, max_width_chars=58, visible=False)
        card.append(row)
        card.append(self.iso_state)
        if self.cfg.dir_ready:
            self.iso_label.set_text("Already extracted in the folder: no image needed.")
        content.append(card)

        # 2. where
        card = self.section(2, "Where to install", "The game, its saves and its build go in one folder.")
        self.where_buttons: list[tuple[Gtk.CheckButton, Path]] = []
        group = None
        for label, path in storage_choices():
            free = free_gb(path)
            row = Gtk.CheckButton(label=f"{label}: {path}   ({free:.0f} GB free)")
            if group is None:
                group = row
            else:
                row.set_group(group)
            if free < NEED_GB:
                row.set_sensitive(False)
                row.set_label(row.get_label() + "   (not enough room)")
            card.append(row)
            self.where_buttons.append((row, path))
        usable = [(b, p) for b, p in self.where_buttons if b.get_sensitive()]
        if usable:  # the roomiest allowed one starts selected, the SD card over the internal storage
            usable[-1][0].set_active(True)
        for row, _ in self.where_buttons:
            row.connect("toggled", lambda *_: self.update_buttons())
        other = Gtk.Button(label="Another folder...", halign=Gtk.Align.START)
        other.connect("clicked", lambda *_: self.pick_folder(self.set_folder))
        self.where_note = Gtk.Label(label="", xalign=0, wrap=True, max_width_chars=58, visible=False)
        card.append(other)
        card.append(self.where_note)
        content.append(card)

        # 3. options
        card = self.section(3, "Options", "Both can wait: the home screen can add them later.")
        self.steam_check = Gtk.CheckButton(label="Add Rogue Galaxy to Steam (play it from game mode)")
        self.steam_check.set_active(self.cfg.steam)
        self.european_check = Gtk.CheckButton(label="Add the European disc (SCES-54552)")
        self.european_check.set_active(bool(self.cfg.european))
        self.european_check.connect("toggled", lambda *_: self.on_european_toggled())
        row = Gtk.Box(spacing=12, margin_start=28)
        eu = Gtk.Button(label="Choose the European .iso...")
        eu.connect("clicked", lambda *_: self.pick_file(self.set_european, "Rogue Galaxy European disc"))
        self.european_label = Gtk.Label(label=self.cfg.european.name if self.cfg.european else
                                        "No image chosen. French, German, Spanish and Italian texts.",
                                        xalign=0, hexpand=True, wrap=True, max_width_chars=58)
        self.european_label.add_css_class("muted")
        row.append(eu)
        row.append(self.european_label)
        self.european_reveal = Gtk.Revealer(transition_type=Gtk.RevealerTransitionType.SLIDE_DOWN,
                                            child=row, reveal_child=self.european_check.get_active())
        card.append(self.steam_check)
        card.append(self.european_check)
        card.append(self.european_reveal)
        content.append(card)

        scroll = Gtk.ScrolledWindow(hscrollbar_policy=Gtk.PolicyType.NEVER, vexpand=True, child=content)
        bar = Gtk.Box(spacing=12, margin_top=10, margin_bottom=18, margin_start=56, margin_end=24)
        self.summary = Gtk.Label(label=f"About {NEED_GB} GB and 10 to 30 minutes. Keep the Deck plugged in.",
                                 xalign=0, hexpand=True, wrap=True, max_width_chars=58)
        self.summary.add_css_class("muted")
        self.install_button = Gtk.Button(label="Install")
        self.install_button.add_css_class("suggested-action")
        self.install_button.connect("clicked", lambda *_: self.begin())
        bar.append(self.summary)
        bar.append(self.install_button)
        left = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, hexpand=False, width_request=720)
        left.append(scroll)
        left.append(bar)
        return self.beside_art(left)

    def build_install(self) -> Gtk.Widget:
        content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12, width_request=720, vexpand=True,
                          hexpand=False, margin_top=22, margin_bottom=18, margin_start=56, margin_end=24)
        content.append(self.header())
        self.install_title = Gtk.Label(label="Installing", xalign=0, margin_top=14)
        self.install_title.add_css_class("title")
        self.install_sub = Gtk.Label(label="Keep the Deck plugged in. You can leave this window open and do "
                                           "something else.", xalign=0, wrap=True, max_width_chars=58)
        self.install_sub.add_css_class("subtitle")
        content.append(self.install_title)
        content.append(self.install_sub)
        card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        card.add_css_class("card")
        self.step_label = Gtk.Label(label="Starting", xalign=0)
        self.bar = Gtk.ProgressBar()
        self.fail_label = Gtk.Label(label="", xalign=0, wrap=True, max_width_chars=58, visible=False)
        self.fail_label.add_css_class("bad")
        self.done_label = Gtk.Label(label="", xalign=0, wrap=True, max_width_chars=58, visible=False)
        card.append(self.step_label)
        card.append(self.bar)
        card.append(self.fail_label)
        card.append(self.done_label)
        content.append(card)
        expander = Gtk.Expander(label="Details", vexpand=True)
        self.log_view = Gtk.TextView(editable=False, monospace=True, wrap_mode=Gtk.WrapMode.CHAR)
        self.log_scroll = Gtk.ScrolledWindow(min_content_height=200, child=self.log_view)
        expander.set_child(self.log_scroll)
        content.append(expander)
        self.close_button = Gtk.Button(label="Close", halign=Gtk.Align.END, visible=False)
        self.close_button.add_css_class("suggested-action")
        self.close_button.connect("clicked", lambda *_: self.get_application().quit())
        content.append(self.close_button)
        return self.beside_art(content)

    # -- choosing files
    def pick_file(self, callback, title: str) -> None:
        dialog = Gtk.FileDialog(title=title)
        dialog.open(self, None, lambda d, r: self.picked(d, r, callback, False))

    def pick_folder(self, callback) -> None:
        dialog = Gtk.FileDialog(title="Install the game in")
        dialog.select_folder(self, None, lambda d, r: self.picked(d, r, callback, True))

    def picked(self, dialog, result, callback, folder: bool) -> None:
        try:
            file = dialog.select_folder_finish(result) if folder else dialog.open_finish(result)
        except GLib.Error:
            return
        if file and file.get_path():
            callback(Path(file.get_path()))

    def set_iso(self, path: Path) -> None:
        problem = looks_like_iso(path)
        self.cfg.iso = None if problem else path
        self.iso_label.set_text(str(path))
        self.iso_state.set_text(problem or "This looks like a disc image. It is checked in full while installing.")
        self.iso_state.set_visible(True)
        for cls in ("good", "bad"):
            self.iso_state.remove_css_class(cls)
        self.iso_state.add_css_class("bad" if problem else "good")
        self.update_buttons()

    def set_folder(self, path: Path) -> None:
        self.custom_dir = path / "RogueGalaxy" if any(path.iterdir()) else path
        free = free_gb(self.custom_dir)
        for row, _ in self.where_buttons:
            row.set_active(False)
        enough = free >= NEED_GB
        self.where_note.set_visible(True)
        self.where_note.set_text(f"{self.custom_dir}   ({free:.0f} GB free"
                                 + ("" if enough else ", not enough") + ")")
        self.update_buttons()

    def set_european(self, path: Path) -> None:
        self.cfg.european = path
        self.european_label.set_text(path.name)
        self.update_buttons()

    def on_european_toggled(self) -> None:
        self.european_reveal.set_reveal_child(self.european_check.get_active())
        self.update_buttons()

    # -- what is chosen
    def chosen_dir(self) -> Path | None:
        if self.custom_dir and not any(b.get_active() for b, _ in self.where_buttons):
            return self.custom_dir if free_gb(self.custom_dir) >= NEED_GB else None
        for row, path in self.where_buttons:
            if row.get_active():
                return path
        return None

    def update_buttons(self) -> None:
        missing = []
        if self.cfg.iso is None and not self.cfg.dir_ready:
            missing.append("a disc image (SCUS-97490)")
        if self.chosen_dir() is None and not self.cfg.dir:
            missing.append("a folder")
        if self.european_check.get_active() and self.cfg.european is None:
            missing.append("the European disc (SCES-54552), or untick it")
        self.install_button.set_sensitive(not missing)
        self.summary.set_text("Still to do: " + ", ".join(missing) + "." if missing else
                              f"Ready. About {NEED_GB} GB and 10 to 30 minutes; keep the Deck plugged in.")

    def begin(self) -> None:
        self.cfg.dir = self.cfg.dir or self.chosen_dir()
        if not self.cfg.start:
            self.cfg.steam = self.steam_check.get_active()
        if not self.european_check.get_active():
            self.cfg.european = None
        self.stack.set_visible_child_name("install")
        self.pulse_id = GLib.timeout_add(120, self.pulse)
        self.job = Job(self.cfg, self)
        self.running = True
        self.job.start()

    # -- closing the window while it builds: ask, then really stop
    def on_close_request(self, *_) -> bool:
        if not self.running or self.cancelling:
            return self.cancelling  # not building: close; being cancelled: wait for the end
        job = self.job
        where = job.cfg.dir if job else None
        kept = ("The folder " + str(where) + " and everything built in it so far will be deleted."
                if job and job.fresh else "Your earlier install in this folder stays as it was.")
        dialog = Gtk.AlertDialog(message="Cancel the installation?",
                                 detail="The build will be stopped. " + kept,
                                 buttons=["Keep installing", "Cancel the installation"],
                                 cancel_button=0, default_button=0, modal=True)
        dialog.choose(self, None, self.on_close_answer)
        return True

    def on_close_answer(self, dialog, result) -> None:
        try:
            choice = dialog.choose_finish(result)
        except GLib.Error:
            return
        if choice == 1:
            self.cancel_install()

    cancelling = False

    def cancel_install(self) -> None:
        if not self.running or self.cancelling or not self.job:
            return
        self.cancelling = True
        self.step_label.set_text("Cancelling: stopping the build and deleting what it made")
        self.pulsing = True
        threading.Thread(target=self.job.cancel, daemon=True).start()

    def on_cancelled(self) -> bool:
        self.running = False
        self.get_application().quit()
        return False

    # -- the job's callbacks (on the main thread)
    pulsing = True

    def pulse(self) -> bool:
        if self.pulsing:
            self.bar.pulse()
        return True

    def on_progress(self, text: str, fraction: float | None) -> bool:
        self.step_label.set_text(text)
        if fraction is None:
            self.pulsing = True
        else:
            self.pulsing = False
            self.bar.set_fraction(max(0.0, min(1.0, fraction)))
        return False

    def on_log(self, line: str) -> bool:
        buffer = self.log_view.get_buffer()
        buffer.insert(buffer.get_end_iter(), line + "\n")
        if buffer.get_line_count() > 2000:
            buffer.delete(buffer.get_start_iter(), buffer.get_iter_at_line(500))
        adjustment = self.log_scroll.get_vadjustment()
        adjustment.set_value(adjustment.get_upper())
        return False

    def on_failed(self, message: str) -> bool:
        self.running = False
        self.pulsing = False
        self.fail_label.set_text(message)
        self.fail_label.set_visible(True)
        self.step_label.set_text("The install stopped")
        self.close_button.set_visible(True)
        if self.cfg.shots:
            GLib.timeout_add(500, self.shoot, "failed")
        return False

    def on_done(self) -> bool:
        self.running = False
        self.pulsing = False
        self.bar.set_fraction(1.0)
        self.install_title.set_text("Done")
        self.install_sub.set_text("Rogue Galaxy is installed.")
        self.step_label.set_text("")
        steam = ("It is in your Steam library, under Non-Steam. Switch to game mode and play. "
                 if self.cfg.steam else "Add rogue-galaxy.sh to Steam as a non-Steam game, then switch to game "
                                        "mode and play. ")
        self.done_label.set_text(steam + "The game opens on a home screen with Play and Update; this installer "
                                         "is not needed any more.")
        self.done_label.set_visible(True)
        self.close_button.set_visible(True)
        return False

    # -- a PNG of the window, for checking the look without a screen
    def tour_european(self) -> bool:
        self.european_check.set_active(True)
        GLib.timeout_add(700, self.shoot, "setup-european")
        return False

    def tour_install(self) -> bool:
        self.stack.set_visible_child_name("install")
        GLib.timeout_add(500, self.shoot, "install")
        return False

    def shoot(self, name: str) -> bool:
        from gi.repository import Graphene  # noqa: PLC0415

        paintable = Gtk.WidgetPaintable.new(self)
        width, height = self.get_width(), self.get_height()
        snapshot = Gtk.Snapshot()
        paintable.snapshot(snapshot, width, height)
        node = snapshot.to_node()
        renderer = self.get_native().get_renderer()
        if node and renderer:
            texture = renderer.render_texture(node, Graphene.Rect().init(0, 0, width, height))
            Path(self.cfg.shots).mkdir(parents=True, exist_ok=True)
            texture.save_to_png(str(Path(self.cfg.shots) / f"{name}.png"))
        return False


class App(Gtk.Application):
    def __init__(self, cfg: argparse.Namespace):
        super().__init__(application_id="io.github.deckgalaxy.Setup", flags=Gio.ApplicationFlags.NON_UNIQUE)
        self.cfg = cfg

    def do_activate(self) -> None:
        Window(self, self.cfg).present()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--iso", type=Path)
    ap.add_argument("--dir", type=Path)
    ap.add_argument("--zip", type=Path)
    ap.add_argument("--japanese", type=Path)
    ap.add_argument("--european", type=Path)
    ap.add_argument("--no-steam", dest="steam", action="store_false")
    ap.add_argument("--start", action="store_true", help="skip the pages and install now (needs --dir)")
    ap.add_argument("--shots", help="save a PNG of each page in this folder")
    ap.add_argument("--tour", action="store_true", help="with --shots, show each page in turn")
    ap.add_argument("--test-cancel", type=int, default=0, metavar="SECONDS",
                    help="cancel the install after this many seconds (a test)")
    cfg = ap.parse_args()
    if cfg.start and not cfg.dir:
        ap.error("--start needs --dir")
    # An already extracted disc in the folder needs no image.
    cfg.dir_ready = bool(cfg.dir and (cfg.dir / "disc" / "manifest.json").exists())
    return App(cfg).run([sys.argv[0]])


if __name__ == "__main__":
    sys.exit(main())
