#!/usr/bin/env python3
"""Rogue Galaxy for the Steam Deck: the installer.

One file, to run once in desktop mode (double-click, "Execute"). It asks for
your disc image and where to put the game, downloads the game's sources,
builds them on the Deck (in a container, 10 to 30 minutes), and adds the game
to Steam. Afterwards it is not needed any more: the game starts on its own
home screen, which updates it, in game mode.

Needs what SteamOS has: python3 with GTK 4 (PyGObject), curl, git, distrobox.

For tests: --iso FILE, --dir FOLDER, --zip ARCHIVE (a release's source
archive instead of the download), --no-steam, --start (skip the pages),
--shots FOLDER (a PNG of each page; --tour steps through the pages). RG_UPDATE_REPO=owner/name and
RG_UPDATE_API=URL point the release lookup elsewhere; RG_SETUP_REF=BRANCH is
the branch cloned when the repository has no release yet.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import threading
import urllib.request
import zipfile
from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gio, GLib, Gtk  # noqa: E402

REPO = os.environ.get("RG_UPDATE_REPO", "thibault-lgm/deck-galaxy")
API = os.environ.get("RG_UPDATE_API", f"https://api.github.com/repos/{REPO}/releases?per_page=10")
REF = os.environ.get("RG_SETUP_REF", "main")
NEED_GB = 24  # the extracted disc, the generated code, the build
ISO_BYTES = 6_967_721_984  # Redump's image of the USA release

CSS = b"""
window, .page { background: #060a1e; color: #f2f3fc; }
.title { font-size: 26px; font-weight: 700; color: #f2f3fc; }
.subtitle { font-size: 14px; color: #9ea8cc; }
.muted { color: #9ea8cc; }
.good { color: #70de8e; }
.warn { color: #f6b640; }
.bad { color: #ff7b72; }
.card { background: #0c112e; border: 1px solid #2a3263; border-radius: 12px; padding: 14px; }
button { background: #1a2046; color: #f2f3fc; border: 1px solid #2a3263; border-radius: 8px;
         padding: 8px 18px; box-shadow: none; background-image: none; }
button:hover { background: #232b5c; }
button:disabled { color: #5f6890; background: #10152f; }
button.suggested-action { background: #f4cb52; color: #1a1500; border-color: #f4cb52; font-weight: 700; }
button.suggested-action:disabled { background: #4a4224; color: #8a8160; border-color: #4a4224; }
progressbar trough { background: #1a2046; min-height: 10px; border-radius: 6px; }
progressbar progress { background: #f4cb52; min-height: 10px; border-radius: 6px; }
checkbutton { color: #f2f3fc; }
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
        except Failure as error:
            GLib.idle_add(self.ui.on_failed, str(error))
            return
        except Exception as error:  # a bug here must still reach the window
            GLib.idle_add(self.ui.on_failed, f"Unexpected error: {error}")
            return
        GLib.idle_add(self.ui.on_done)

    # -- running a program, its output line by line
    def stream(self, command: list[str], cwd: Path, on_line=None) -> int:
        self.log("$ " + " ".join(command))
        process = subprocess.Popen(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL, text=True, bufsize=1, errors="replace")
        assert process.stdout is not None
        for raw in process.stdout:
            line = raw.rstrip()
            self.log(line)
            if on_line:
                on_line(line)
        return process.wait()

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
    def steam(self) -> None:
        if not self.cfg.steam:
            return
        self.say("Adding the game to Steam", 0.98)
        desktop = Path.home() / ".local/share/applications/rogue-galaxy.desktop"
        tool = shutil.which("steamos-add-to-steam")
        if not tool or not desktop.exists():
            self.log("Could not add the game to Steam by itself: add rogue-galaxy.sh as a non-Steam game.")
            return
        self.stream([tool, str(desktop)], self.cfg.dir)


# ---------------------------------------------------------------- the window


class Window(Gtk.ApplicationWindow):
    PAGES = ["welcome", "disc", "where", "options", "install", "done"]

    def __init__(self, app: Gtk.Application, cfg: argparse.Namespace):
        super().__init__(application=app, title="Rogue Galaxy: install")
        self.cfg = cfg
        self.set_default_size(880, 640)
        provider = Gtk.CssProvider()
        provider.load_from_data(CSS)
        Gtk.StyleContext.add_provider_for_display(self.get_display(), provider,
                                                  Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        self.stack = Gtk.Stack(transition_type=Gtk.StackTransitionType.SLIDE_LEFT_RIGHT, vexpand=True,
                               halign=Gtk.Align.CENTER, width_request=800)
        self.back = Gtk.Button(label="Back")
        self.next = Gtk.Button(label="Continue")
        self.next.add_css_class("suggested-action")
        self.back.connect("clicked", lambda *_: self.go(-1))
        self.next.connect("clicked", lambda *_: self.on_next())
        buttons = Gtk.Box(spacing=10, halign=Gtk.Align.END, margin_top=8)
        buttons.append(self.back)
        buttons.append(self.next)
        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, margin_top=28, margin_bottom=22,
                       margin_start=36, margin_end=36, spacing=12)
        root.add_css_class("page")
        root.append(self.stack)
        root.append(buttons)
        self.set_child(root)

        self.index = 0
        self.job: Job | None = None
        self.pulse_id = 0
        self.build_pages()
        self.show_page(0)
        if cfg.start:
            GLib.idle_add(self.begin)
        if cfg.shots and cfg.tour:
            for i in range(1, 4):
                GLib.timeout_add(1500 * i, lambda n=i: self.show_page(n) and False)

    # -- page building
    def heading(self, title: str, subtitle: str) -> Gtk.Box:
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6, vexpand=True)
        label = Gtk.Label(label=title, xalign=0)
        label.add_css_class("title")
        sub = Gtk.Label(label=subtitle, xalign=0, wrap=True)
        sub.add_css_class("subtitle")
        box.append(label)
        box.append(sub)
        box.append(Gtk.Box(margin_top=8))
        return box

    def card(self) -> Gtk.Box:
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        box.add_css_class("card")
        return box

    def build_pages(self) -> None:
        # welcome
        page = self.heading("Rogue Galaxy for the Steam Deck",
                            "This installs the game from your own disc image. It takes 10 to 30 minutes, "
                            "needs about 24 GB, and an internet connection. Afterwards you play from Steam in "
                            "game mode, and the game updates itself from its home screen.")
        card = self.card()
        for line in ("1. Pick your Rogue Galaxy (USA) disc image, an uncompressed .iso.",
                     "2. Pick where to install the game.",
                     "3. Wait while it is built on your Deck. You can keep using the Deck."):
            card.append(Gtk.Label(label=line, xalign=0))
        page.append(card)
        self.stack.add_named(page, "welcome")

        # disc
        page = self.heading("Your disc image",
                            "Rogue Galaxy, USA release (SCUS-97490), as an uncompressed .iso. Nothing is "
                            "uploaded: the image is read on this Deck only.")
        card = self.card()
        self.iso_label = Gtk.Label(label="No image chosen", xalign=0, wrap=True)
        self.iso_state = Gtk.Label(label="", xalign=0, wrap=True)
        choose = Gtk.Button(label="Choose the .iso...", halign=Gtk.Align.START)
        choose.connect("clicked", lambda *_: self.pick_file(self.set_iso, "Your Rogue Galaxy (USA) disc image"))
        card.append(choose)
        card.append(self.iso_label)
        card.append(self.iso_state)
        page.append(card)
        self.stack.add_named(page, "disc")

        # where
        page = self.heading("Where to install", "The game, its saves and its build go in one folder.")
        card = self.card()
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
        # the roomiest allowed one starts selected, the SD card over the internal storage
        usable = [(b, p) for b, p in self.where_buttons if b.get_sensitive()]
        if usable:
            usable[-1][0].set_active(True)
        other = Gtk.Button(label="Another folder...", halign=Gtk.Align.START)
        other.connect("clicked", lambda *_: self.pick_folder(self.set_folder))
        card.append(other)
        self.where_note = Gtk.Label(label="", xalign=0, wrap=True)
        card.append(self.where_note)
        page.append(card)
        self.stack.add_named(page, "where")
        self.custom_dir: Path | None = None

        # options
        page = self.heading("Options", "All of this can wait: the home screen can add it later.")
        card = self.card()
        self.steam_check = Gtk.CheckButton(label="Add Rogue Galaxy to Steam (play it from game mode)")
        self.steam_check.set_active(self.cfg.steam)
        card.append(self.steam_check)
        self.japanese_label = Gtk.Label(label="Japanese voices and movies: no image chosen (optional)", xalign=0)
        jp = Gtk.Button(label="Add the Japanese Director's Cut image...", halign=Gtk.Align.START)
        jp.connect("clicked", lambda *_: self.pick_file(self.set_japanese, "Rogue Galaxy Japanese Director's Cut"))
        self.european_label = Gtk.Label(label="French, German, Spanish and Italian texts: no image chosen (optional)",
                                        xalign=0)
        eu = Gtk.Button(label="Add the European image...", halign=Gtk.Align.START)
        eu.connect("clicked", lambda *_: self.pick_file(self.set_european, "Rogue Galaxy European disc"))
        for widget in (jp, self.japanese_label, eu, self.european_label):
            card.append(widget)
        page.append(card)
        self.stack.add_named(page, "options")

        # install
        page = self.heading("Installing", "Keep the Deck plugged in. You can leave this window open and do "
                                          "something else.")
        card = self.card()
        self.step_label = Gtk.Label(label="Starting", xalign=0)
        self.bar = Gtk.ProgressBar()
        self.fail_label = Gtk.Label(label="", xalign=0, wrap=True)
        self.fail_label.add_css_class("bad")
        card.append(self.step_label)
        card.append(self.bar)
        card.append(self.fail_label)
        page.append(card)
        expander = Gtk.Expander(label="Details")
        self.log_view = Gtk.TextView(editable=False, monospace=True, wrap_mode=Gtk.WrapMode.CHAR)
        scroll = Gtk.ScrolledWindow(min_content_height=170, child=self.log_view)
        expander.set_child(scroll)
        page.append(expander)
        self.log_scroll = scroll
        self.stack.add_named(page, "install")

        # done
        page = self.heading("Done", "Rogue Galaxy is installed.")
        card = self.card()
        self.done_label = Gtk.Label(label="", xalign=0, wrap=True)
        card.append(self.done_label)
        page.append(card)
        self.stack.add_named(page, "done")

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
        self.where_note.set_text(f"{self.custom_dir}   ({free:.0f} GB free"
                                 + ("" if enough else ", not enough") + ")")
        self.update_buttons()

    def set_japanese(self, path: Path) -> None:
        self.cfg.japanese = path
        self.japanese_label.set_text(f"Japanese voices and movies: {path.name}")

    def set_european(self, path: Path) -> None:
        self.cfg.european = path
        self.european_label.set_text(f"European texts: {path.name}")

    # -- navigation
    def chosen_dir(self) -> Path | None:
        if self.custom_dir and not any(b.get_active() for b, _ in self.where_buttons):
            return self.custom_dir if free_gb(self.custom_dir) >= NEED_GB else None
        for row, path in self.where_buttons:
            if row.get_active():
                return path
        return None

    def update_buttons(self) -> None:
        name = self.PAGES[self.index]
        self.back.set_visible(name in ("disc", "where", "options"))
        self.next.set_visible(name != "install")
        ok = True
        if name == "disc":
            ok = self.cfg.iso is not None or self.cfg.dir_ready
        if name == "where":
            ok = self.chosen_dir() is not None
        self.next.set_sensitive(ok)
        self.next.set_label({"options": "Install", "done": "Close"}.get(name, "Continue"))

    def show_page(self, index: int) -> None:
        self.index = index
        name = self.PAGES[index]
        self.stack.set_visible_child_name(name)
        self.update_buttons()
        if self.cfg.shots:
            GLib.timeout_add(500, self.shoot, name)

    def go(self, step: int) -> None:
        self.show_page(max(0, min(len(self.PAGES) - 1, self.index + step)))

    def on_next(self) -> None:
        name = self.PAGES[self.index]
        if name == "options":
            self.begin()
        elif name == "done":
            self.get_application().quit()
        else:
            self.go(1)

    def begin(self) -> None:
        self.cfg.dir = self.cfg.dir or self.chosen_dir()
        self.cfg.steam = self.steam_check.get_active() if not self.cfg.start else self.cfg.steam
        self.show_page(self.PAGES.index("install"))
        self.pulse_id = GLib.timeout_add(120, self.pulse)
        self.job = Job(self.cfg, self)
        self.job.start()

    # -- the job's callbacks (on the main thread)
    def pulse(self) -> bool:
        if self.pulsing:
            self.bar.pulse()
        return True

    pulsing = True

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
        self.pulsing = False
        self.fail_label.set_text(message)
        self.step_label.set_text("The install stopped")
        self.next.set_visible(True)
        self.next.set_label("Close")
        self.next.set_sensitive(True)
        self.index = self.PAGES.index("done")  # Close quits
        self.back.set_visible(False)
        if self.cfg.shots:
            GLib.timeout_add(500, self.shoot, "failed")
        return False

    def on_done(self) -> bool:
        self.pulsing = False
        self.bar.set_fraction(1.0)
        steam = ("It is in your Steam library, under Non-Steam. Switch to game mode and play. "
                 if self.cfg.steam else "Add rogue-galaxy.sh to Steam as a non-Steam game, then switch to game "
                                        "mode and play. ")
        self.done_label.set_text(steam + "The game opens on a home screen with Play and Update; this installer "
                                         "is not needed any more.")
        self.show_page(self.PAGES.index("done"))
        return False

    # -- a PNG of a page, for checking the look without a screen
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
    cfg = ap.parse_args()
    if cfg.start and not cfg.dir:
        ap.error("--start needs --dir")
    # An already extracted disc in the folder needs no image.
    cfg.dir_ready = bool(cfg.dir and (cfg.dir / "disc" / "manifest.json").exists())
    return App(cfg).run([sys.argv[0]])


if __name__ == "__main__":
    sys.exit(main())
