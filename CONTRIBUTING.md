# Contributing

Thanks for helping. This page is the rules; how the project is built and
changed is in [docs/README.md](docs/README.md).

## Never game files

Never attach, commit or link a disc image, files from `disc\`, a BIOS or a
texture pack, in an issue, a pull request or a comment. The one exception is a
save, as the `disc\mc0` folder zipped, when an issue asks for it.

## Issues

Open one with a form: bug, performance, feature or question. A bug needs the
version (F1 > Rogue Galaxy > About) and `build\last-run.log`.

An issue stays open until a release ships its fix. When the fix is merged, the
issue gets the `fixed` label and a comment; it is closed on the day a release
with the fix comes out. An issue waiting on its author (`waiting`) may be
closed after two weeks without an answer, and reopened when the answer comes.

## Pull requests

- **Risky features come off by default.** Anything that makes the game look or
  play differently from the console, or that is still being proven (a new
  renderer path, a speed trick), goes behind an option or an environment
  variable, off by default.
- **A change to the default path brings a switch back.** If what players get
  with every option at its default changes (the picture, timing, speed,
  saves), the pull request also adds a way to get the old behaviour, so both
  can be compared in one build.
- **Renderer changes pass the frame-compare gate.** The scripted walk
  (`tools/bench/walk.sh`), and a battle when the change touches effects, must
  give frames byte-identical to main's at 1x and 3x with the change switched
  off, and with it on unless the picture is meant to change (then show it
  before and after). The maintainer runs the gate on the pull request's head
  merged with main.
- **One concern per pull request.** Stacks are welcome: each pull request
  based on the previous one, its description saying its place (2 of 3, based
  on #N).
- **Answer a review with new commits** on top. No force-push or rebase unless
  you are asked to: the reviewer then reads only what changed.
- **Name issues with "Refs #N"**, never "Fixes #N" (that would close the issue
  at the merge).
- **Fill in the template**: whether the default path changes, what you tested
  on (OS, GPU, internal scale and the pull request's head commit), what you did
  not test, and images before and after when the change is visible.

Pull requests are merged with a merge commit, after the maintainer's review
and tests.
