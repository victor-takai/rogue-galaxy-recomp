# VU1 oracle

The step-by-step recipe for moving a program off the VU1, the pieces known
and the status per scene are in [docs/VU1-NATIVE.md](../../../docs/VU1-NATIVE.md).

The check for phase 1 of [NATIVE-RENDERER.md](../../../docs/NATIVE-RENDERER.md):
a native replacement for a family of VU1 programs is right when it draws what
the program drew. The game records every VU1 program run in full
(`RG_VU1_REPLAY`, `src/debug/vu1_replay.h`): the code image, the data memory
and registers it started with, the GS packets it kicked and the registers it
ended with. `rg_vu1_oracle` runs each candidate on those inputs, without the
game, and compares what it draws with what the game's run kicked.

What is compared is the events of the packets (`gif_events.h`): every
register write, and every vertex with the vertex state at its kick (PRIM,
XY in 12.4, Z, fog, RGBA, STQ, UV). Two packets laid out differently that
draw the same thing compare equal.

## Use

```bash
# The tool (not part of the default build)
cmake --build build/game --target rg_vu1_oracle

# A recording: File 01 from a copy of the card, 40 lists from list 2700
tools/vu1/oracle/capture.sh field1 2700 40

# Replay it: both VU1 engines, the first differing run of each program shown
build/game/src/rg_vu1_oracle build/run/field1.vu1r --candidate all --show diff
```

Options: `--candidate compiled|interpreter|all`; `--runs N` reads only the
first N runs; `--program HASH:PC` counts only one program; `--show RUN`
prints one run's events beside the recorded ones, from its first difference;
`--tol-xy`, `--tol-z`, `--tol-rgba`, `--tol-uv` (in the GS's own units:
1/16 pixel, Z units, levels, 1/16 texel) and `--tol-stq` (relative) widen
what counts as equal. Exit status 1 when anything differs.

A row of the table is a program: the code image (FNV-1a of the 16 KiB of
micro memory) and the entry of the MSCAL that started it. The MSCNT
continuations count toward it (`mscnt`); in the field they kick most of the
vertices, so every recorded run starts from its own recorded data memory.
`same` and `differ` count runs; `regs` counts runs whose VF, VI, ACC, Q, P
and I registers ended differently; the `max` columns are the largest
differences seen, within the tolerance or not.

## The first recording (2026-10-28)

Salgin, File 01, lists 2700-2739 at 1x in the 30 fps mode: 67,640 runs
(49,440 continuations), 231,504 packets, 1.86 M vertices, 29 code images,
1.29 GB. Both engines reproduce every run exactly, packets and end
registers (5.7 s for both). So a recording is a lossless input: a native
candidate that differs is wrong, not the replay.

The comparison was checked against known changes: a byte altered in a
recorded packet is the one run reported; the matrix quads 68-75 of a run that
kicks vertices scaled by 1% leave its XY as they were and move its Q and
STQ by 1% (a homogeneous scale cancels in the divide), which is what it
reports.

Programs by vertices kicked in that window (`:320` is the draw entry, pair
0x64; `:960` is pair 0x12C; `:0` is the per-frame base program, which kicks
nothing):

| Program | Runs | Vertices |
|---|---|---|
| `9f727028f73861c3:320` | 20,720 | 617,640 |
| `3dd4ba5d80f01760:320` | 6,680 | 319,040 |
| `cfb586576df9c3d2:320` | 10,680 | 278,360 |
| `30da21d7b6d089bf:320` | 7,080 | 152,360 |
| `0f44ab11d3953380:320` | 1,400 | 89,600 |
| `3856a013e86478a4:320` | 2,160 | 83,520 |

## Adding a native candidate

Derive from `Candidate` in `rg_vu1_oracle.cpp`: `accepts()` says which
programs it replaces (by `Run::programHash` and `programPC`), `run()` turns
a run's recorded inputs (`Run::data`, `Run::state`, `RunHeader`) into
`GifEvent`s. Register it beside the two VU1 engines. Its runs are compared
with the recording and the others are left out of its table.

## Reading a program

`--coverage FILE` (with `--program`) single-steps the interpreter through the
program's runs and writes how often each instruction pair ran; the replay
still has to match, so the coverage is the game's. `tools/vu1/vudis.py CODE
--coverage FILE` disassembles the code image with operands, only what ran,
each pair with its count. A code image comes out of a recording's CODE record.

## The first native program (2026-10-28)

`native_strip.cpp` (`--candidate native`) is `9f727028f73861c3:320`, the
heaviest program of the recording, read from its 886 pairs that ran: the
MSCAL only ends, each chunk runs as an MSCNT through slots A (header, GIF
header), B (int16 positions, normals, UVs to floats), C (vertex colors), D
(transform, fog, ST, guard band and backface into ADC) and, with quad
81.x & 2, E (clipping). It takes slots A, B, C, D and F as they are in that
image and leaves E, triangle lists, scrolling UVs and three flag branches the
field never took to the VU1.

On the recording: all 17,800 runs it takes (12,840 chunks, 469,400 vertices)
match within 1/16 pixel, 1 Z unit and 4.4e-7 of STQ, every ADC bit and
register write exact; the 2,920 chunks that clip are left to the VU1. Without
tolerances the differences are the float rounding (the VU truncates).

Of the other heavy programs, none has the same A, B, C, D and F; all have
the same E and F, most the same A, and they differ in B, C and D:

| Program | A | B | C | D |
|---|---|---|---|---|
| `3dd4ba5d80f01760` | x | x | x | x |
| `cfb586576df9c3d2` | = | = | = | x |
| `30da21d7b6d089bf` | = | x | x | x |
| `0f44ab11d3953380` | x | x | x | x |
| `3856a013e86478a4` | = | = | = | x |
| `a953fc53a82da5ff` | = | = | x | = |

So the native side is per slot piece, composed, as the code is.

## In the game, and on the GPU (2026-10-28)

The native code moved to `src/vu1_native.{h,cpp}`, where the game uses it
(`RG_VU1_NATIVE`, docs/DEVELOPMENT.md) and this oracle wraps it
(`native.cpp`); the GIF decoder moved to `src/gif_events.{h,cpp}`. It knows
three D pieces now (no culling, the guard band, the guard band and
backfaces), found by a hash of each slot's code, so any image with those
pieces is done: four programs in the recording, 21,120 chunks.

`tools/vu1/oracle/native-run.sh NAME MODE` logs the same frame of the
field walk in each mode; `tools/bench/vram2png.py` draws two frames and
their difference. On that frame against the VU1: `skip` (the VU1 not run
for those chunks) differs in 3 pixels by more than 8 levels; `gpu` (strips
transformed by `gs_native.vert`, the clipping chunks included, since the GPU
clips) in 54. Triangle lists (PRIM 3), which the guard-band piece runs
through its strip code, followed: as meshes on the GPU too, 57 pixels, and
no chunk of these programs left on the VU1 (the backface piece's own list
path, which the field never ran, is still declined).
