#!/usr/bin/env python3
"""The release page's text, from the version's section of CHANGELOG.md.

    python tools/release/release_notes.py VERSION [--out FILE] [--changelog FILE]

Writes build/release/RogueGalaxy-<version>-notes.md, for
`gh release create v<version> ... --notes-file`. The CHANGELOG's lines are
wrapped by hand, and GitHub's release page keeps every single newline as a
break, so the lines of a paragraph or list item are joined. Relative links
become links into the repository at the version's tag (v<version>), so they
keep pointing at what that release shipped, and the F1 menu's picture goes
on top, as on the README. Run it on the tagged CHANGELOG:
`git show v<version>:CHANGELOG.md > FILE` and `--changelog FILE`.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REPO = "https://github.com/victor-takai/rogue-galaxy-recomp"
RAW = "https://raw.githubusercontent.com/victor-takai/rogue-galaxy-recomp"
PICTURE = "docs/images/menu-game.jpg"


def section(text: str, version: str) -> list[str]:
    """The lines under `## <version>`, up to the next `## ` heading."""
    match = re.search(rf"^## {re.escape(version)}(?=[\s(]|$)", text, re.MULTILINE)
    if not match:
        sys.exit(f"no '## {version}' section in the changelog")
    newline = text.find("\n", match.end())
    start = len(text) if newline < 0 else newline + 1
    following = re.search(r"^## ", text[start:], re.MULTILINE)
    end = start + following.start() if following else len(text)
    return text[start:end].splitlines()


def unwrap(lines: list[str]) -> str:
    """Join the continuation lines of each paragraph and list item."""
    blocks: list[str] = []
    for line in lines:
        stripped = line.strip()
        starts_item = bool(re.match(r"^(- |\d+\. |### |\*\*)", stripped))
        if not stripped:
            blocks.append("")
        elif blocks and blocks[-1] and not starts_item and not blocks[-1].startswith("### "):
            blocks[-1] += " " + stripped
        else:
            blocks.append(stripped)
    return "\n".join(blocks).strip() + "\n"


def absolute_links(body: str, tag: str) -> str:
    """Relative Markdown links, anchors kept, into the repository at the tag."""
    return re.sub(r"\]\((?!https?://|#)([^)#]+)(#[^)]*)?\)",
                  lambda m: f"]({REPO}/blob/{tag}/{m.group(1)}{m.group(2) or ''})", body)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("version", help="the version, as its CHANGELOG heading has it (0.2.0)")
    parser.add_argument("--changelog", type=Path, default=ROOT / "CHANGELOG.md")
    parser.add_argument("--out", type=Path, help="default build/release/RogueGalaxy-<version>-notes.md")
    args = parser.parse_args()

    tag = "v" + args.version
    text = args.changelog.read_text(encoding="utf-8")
    body = absolute_links(unwrap(section(text, args.version)), tag)
    picture = (f'<p align="center"><img src="{RAW}/{tag}/{PICTURE}" '
               f'width="70%" alt="The F1 menu\'s Game page"></p>\n\n')
    out = args.out or ROOT / "build" / "release" / f"RogueGalaxy-{args.version}-notes.md"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(picture + body, encoding="utf-8", newline="\n")
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
