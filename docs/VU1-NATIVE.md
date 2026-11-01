# VU1 programs, natively

How a VU1 program of the game moves off the VU1: read it from its own code,
write it as C++ that builds a `GSNativeMesh` (or, for what is not a mesh,
the packets), check it against the recording and the game, then draw it on
the GPU. Every program the recordings hold went this way on 2026-10-28
("Status"); this is the recipe for the pieces still unseen. The plan
around it is `docs/NATIVE-RENDERER.md` (phase 1).

## What a program is

The model programs are not monolithic. Micro memory holds six code slots,
and the game uploads one piece into each per draw (`VuMicrocodeTableInitMaybe_152830`
picks them; RGDecomp's notes on the tables are a guide, not a source):

| Slot | Pair | Byte address | What the pieces read so far do |
|---|---|---|---|
| base | 0x000 | 0x0000 | Per-frame matrix products into quads 64-72 (no kicks; the `:0` rows) |
| A | 0x064 | 0x0320 | The draw entry. An MSCAL here only ends; each chunk runs as an MSCNT from 0x328: reads the chunk header, writes the GIF header, calls the others by `JALR` |
| B | 0x12C | 0x0960 | Vertex decode (int16 to float); also an entry of its own for the screen overlay sprites (the `:960` rows) |
| C | 0x21C | 0x10E0 | Vertex colors (and, on other paths, lighting) |
| D | 0x320 | 0x1900 | Transform, divide, fixed point, fog, culling into ADC, the kicks |
| E | 0x4C4 | 0x2620 | Clipping, only with quad 81.x & 2 |
| F | 0x73A | 0x39D0 | UV scrolling, only with quad 88.xy set |

A program is the set of pieces present at its MSCAL. `src/vu1_native.cpp`
recognizes pieces by the FNV-1a hash of each piece's byte range (the range
its coverage reached), so no game code is kept, and an image with the same
pieces is the same program whatever else micro memory holds.

## The whole list, and how samples are gathered

**The census** (`tools/vu1/census.py`) finds every piece in the executable:
each is a DMA packet in the ELF's data whose VIF codes are an MPG to its
slot's address (pieces over 256 pairs are several MPGs, joined). It prints
each piece's slot, size, address and hash, and its name where
`src/vu1_native.cpp` knows it; with `--recording` files, how many recorded
program starts held it. On 2026-10-28: 56 uploads, 7 pieces native, 35
never run in the field. Some are not work: the 2-pair upload is the empty
piece of every slot, the 112-pair base piece at 0x445820 is VU0's program
(and likely the 20-pair one after it), and slot E never needs porting.

**Harvest mode** collects what the census cannot: real inputs for each
piece, to check it by. `tools/vu1/oracle/harvest-play.sh` runs this build
with your own card and settings while `RG_VU1_HARVEST=build/harvest` keeps
a few samples of every program it has no file for yet (a file each,
`<image>_<entry>.vu1r`, the replay format) and `RG_VU1_NATIVE=check` checks
the native ones; a `[harvest]` line names each new program with the pieces
it has. A checked chunk that differs is saved there too
(`mismatch_<image>_<list>_<n>.vu1r`, at most 8 a program and kind of
difference, with the data memory after the run: the native candidate then
also checks what the program leaves in VU1 memory, a line of its own when
that differs), and replays in the oracle like any recording: what differed at
a boss fight comes back as data. The `[vu1native]` line counts the
differences by program and kind (position, Z, fog, color, UV, STQ, ADC or a
register, vertex count). It is cheap to leave on (a program is known by 64 bytes at each
slot's start), and a later session skips what an earlier one sampled. The
field walk gave 22 files, 12 MB; battles, events, menus and other planets
are where the missing pieces are. Every file replays exactly
(`rg_vu1_oracle build/harvest/*.vu1r --candidate all`), and the census and
the oracle's `--inventory` read the directory as one recording.

## The loop

Everything runs in a build of this branch (`cmake --build build/game
--target roguegalaxy rg_vu1_oracle`) on Linux; `disc/` holds the game.

1. **Get samples.** The harvest directory (above), or a whole scene:
   `tools/vu1/oracle/capture.sh NAME [FROM_LIST] [LISTS]` records every VU1
   run of 40 lists (frames) to `build/run/NAME.vu1r`, about 32 MB a frame,
   from a copy of the memory card (the field walk by default; other scenes
   need their own saves and input scripts, `RG_INPUT`, see
   `tools/bench/walk.sh`). `tools/vu1/census.py --recording FILES` shows
   which pieces they reach. For a program whose output is VU1 memory,
   record with `RG_VU1_REPLAY_AFTER=1` (the memory after each run): the
   oracle's native candidate then checks what it leaves there
   (`memoryEffectsMatch`) on every run (`build/run/fieldafter.vu1r`, 10
   lists of the walk, 600 MB).
2. **Check the recording replays.** `rg_vu1_oracle NAME.vu1r --candidate
   all`: both engines must reproduce every run (the `differ` column 0).
3. **Pick the next program.** `rg_vu1_oracle NAME.vu1r --inventory` lists
   the programs by their share of the vertices, with the known pieces each
   has (`A B C F D:...`, a `-` or `D:?` for a piece not known). Take the
   heaviest, or the one that needs only one unknown piece.
4. **Get its coverage.** `rg_vu1_oracle NAME.vu1r --program HASH:320
   --coverage build/spike/HASH.cov` single-steps the interpreter through
   its runs (still matching the recording) and writes the pairs that ran.
5. **Read it.** `tools/vu1/vudis.py NAME.vu1r --image HASH --coverage
   build/spike/HASH.cov` disassembles what ran, with counts. `--from 1900
   --to 2620` for one slot. Read the unknown pieces only; diff a slot
   against a known program's (`vudis.py ... | cut -c1-5,16-`, then `diff`)
   to see whether it is new or a variant. The D pieces are software
   pipelined: follow one vertex through the loop's registers, and read the
   prologue too (see "Lessons").
6. **Name the piece.** Hash its byte range (FNV-1a, as `fnv()` in
   `src/vu1_native.cpp`; any small script) and add it: a new D piece to
   `kFinishes` with its `Program`, a piece of another slot as a new table
   beside `kCommon`. A piece that changes the per-vertex arithmetic (lighting
   from normals, skinning) extends `GSNativeMesh` (patch 0021: the vertex
   or the header), `gsNativeMeshPoint` (the CPU's arithmetic, shared by the
   packet path and the fallback) and `shaders/gs_native.vert` (the GPU's),
   in step.
7. **Write it.** `readChunk` in `src/vu1_native.cpp` reads the chunk into a
   `GSNativeMesh`; `declineChunk` names every case not done (a flag branch the
   coverage never reached is declined, never guessed).
8. **Check it offline.** `rg_vu1_oracle NAME.vu1r --candidate native
   --tol-xy 1 --tol-z 16 --tol-rgba 1 --tol-stq 1e-5 --show diff`: every
   run it takes in `same`. Exact matching is not the goal (the VU truncates;
   the GPU will not), the tolerances above are. `--time` for its cost.
9. **Check it in the game.** `RG_VU1_NATIVE=check` compares it with the VU1
   live, over whatever the run reaches (a `[vu1native]` line every 2 s, the
   first differences in full). Then frames: `tools/vu1/oracle/native-run.sh
   NAME MODE` with `0`, `skip` and `gpu` logs the same frame of the walk
   (30 fps mode, deterministic), `tools/bench/cmpvram.py A B` counts bytes,
   `tools/bench/vram2png.py OUT.png A.txt.vram B.txt.vram` draws both and the
   difference. Accepted so far: `skip` within a few pixels of `0`, `gpu`
   within about 0.05% of pixels off by more than 8 levels.
10. **Measure it.** `tools/vu1/oracle/native-perf.sh NAME MODE SCALE`: frame
    rate, the GS backend's and the VIF1 thread's share of the wall, GPU busy
    and power over the last 25 s of the walk.
11. **Record it here**: the status table below, and the commit.

## VU1 data memory, as the strip programs use it

Quadwords (16 bytes); `TOP` is the chunk's buffer (VIF TOP, `XTOP`).

| Quad | Use |
|---|---|
| `TOP+0` | Chunk header: x PRIM, y vertex count, z the vertex tag's NLOOP and EOP, w the normals' offset |
| `TOP+1` | x the UVs' offset, y the colors' offset (from `TOP+2`) |
| `TOP+2...` | Positions (int16 xyz), then normals, UVs (int16 xy), colors (int16 xyzw), one quad a vertex each |
| 0 | GIF tag of the output's header (A+D for PRIM and PRMODE) |
| 4-7, 8-11, 24-27, 60-63 | Viewport, projection, view, model (`bc49172`) |
| 16, 17, 18 | Guard band: 109 = 17 with xy times 16.xy (0.999 with clipping), 110 = 18 |
| 19 | GIF tag (yzw) of the FOGCOL packet |
| 39 | Color clamp |
| 40 | Fog: clamp(x + w * W, at most y, at least z) |
| 41 | FOGCOL (A+D); its low word zeroed with 87.y & 0x10 |
| 42, 82, 83 | Color: 82 * 42 + 83 * 128, 82.w scaled by 81.w |
| 68-71 | Object to GS window (row vectors; D) |
| 72-75 | Object to clip space (E) |
| 76 | x the output's offset from TOP; z another branch of A (never set in the field) |
| 77, 78, 79 | Scales of positions, normals, UVs |
| 81 | x bit 1: clipping (E); w: alpha scale |
| 86 | The vertex GIF tag (yzw) |
| 87 | x PRMODE; y flags: 0x10 FOGCOL, 0x100 and 0x200 C's other paths, 0x1000 winding |
| 88 | xy UV scroll (F) |
| 103, 104 | Written by A: section pointers (x positions, y normals, z UVs, w colors), count, output |
| 105-110 | Written by A: guard band and E's constants |
| 111 | z the backface sign |

## Pieces known

| Slot | Bytes | Hash | What |
|---|---|---|---|
| base | 0x0000-0x02c0 | `12ad73d26e1badec` | The object setup (the `:0` rows, `runBase`), run by an MSCAL before each object: kicks the state packet at quad 19, writes 64-66 (model rows normalized by ERLENG), 68-75 (quads 4-7 and 8-11 times 24-27 times the model), 100-102 (lights 30-32 in object space), 111.z (the determinant's sign). Packets only; its memory is what every strip program reads |
| A | 0x0320-0x05d8 | `07151c2597283286` | The strip driver |
| B | 0x0960-0x0a38 | `af62ff64ac67b45f` | int16 positions, normals, UVs |
| C | 0x10e0-0x1328 | `70362be426b17f94` | Vertex colors (87.y & 0x100 and 0x200 declined) |
| C | 0x10e0-0x11b0 | `780d503e649c9116` | One material color for every vertex (with 87.y & 0x200: 82 * 43, no ambient) |
| F | 0x39d0-0x3a68 | `d3f6f6312a13ff1c` | UV scroll (declined when active) |
| B | 0x0960-0x1020 | `26a933a58f99a9da` | Skinning: entries for 1, 2 and 4 bones (A enters at 0x960 plus TOP+1.w pairs with 76.z set); per bone the position (w 1) times weight / 255 times the matrix at quads 900 + 2 * index |
| B | 0x0960-0x0f30 | `542b235c4775a710` | The outlines' skinning: 1 bone (positions only) and 2 bones |
| B | 0x0960-0x09e0 | `a293001f25f94532` | Positions only, one entry (shadow volumes) |
| A | 0x0320-0x04e8 | `3f6df595be6e31b7` | The outline driver: header with PRIM 1 (lines), calls B and C, no D, E or F |
| C | 0x10e0-0x1e70 | `3689bac97cae3c73` | The toon outlines (`buildOutline`): edges as four offset lines; with TOP.x 19 a silhouette test from the two adjacent triangles. Packets only: `gpu` mode sends them as `skip` does |
| A | 0x0320-0x04a8 | `0b1031aeb003ccb0` | The particle driver: header with PRIM 4, calls D only |
| D | 0x1900-0x2890 | `8ecadb3a83d5e584` | The particles (`buildParticles`): camera-facing quads, scaled and turned (cosine and sine from a polynomial in a triangle wave), four vertices of ST, RGBA, XYZF2 each; one corner outside clip space hides all four. Packets only |
| A | 0x0320-0x05c0 | `4beba151fbf23f60` | The projected texture driver: as the strip A, but C last and no D |
| F | 0x39d0-0x3af8 | `15be21a50caaa048` | Projected UVs (position through quads 30-33 times the model, divided by w) and the color (128, 128, 128, 82.w * 81.w * 128) |
| C | 0x10e0-0x1b00 | `3b890d00703404b6` | The projected texture's transform and kick (`buildProjector`): scaled, capped z; guard band and backfaces re-tested per vertex over its triangle; triangle lists declined. Packets only |
| A | 0x0320-0x0500 | `821b638f42fcaa6e` | The shadow volume driver: header with PRIM 5 (fans), calls B and C |
| C | 0x10e0-0x1938 | `dc8dbfe4905cb635` | The shadow volumes (`buildShadow`): each triangle lit (normal . light > 1e-4) and inside is extruded into five fans, each after the A+D packet at quad 38 (facing) or 35. Packets only |
| C | 0x10e0-0x1240 | `91f542d069307d6c` | Lighting: four directional lights (lanes of quads 100-102) on the normal, each clamped at 0, colors 33-36 and ambient 37 times the material 82 (`Lights`) |
| C | 0x10e0-0x14a0 | `2460e5281034f152` | `91f542d0`'s lighting times each vertex's color / 32768 * 256 (87.y & 0x100 declined) |
| F | 0x39d0-0x3b70 | `150031ee1534d185` | Sphere map: UV = (normal through quads 24-26 times 64-66, xy, + 1) / 2 (`SphereMap`) |
| B | 0x0960-0x0b38 | `6e42dac662887054` | The overlay sprites (the `:960` rows), run by an MSCAL of their own (`drawsAtCall`): 32-pixel columns over the bounds in quads 98/99, one pixel wider, within quads 140/141, with the tag at 144 in buffers 145 and 165. Packets only; leaves 144-169 as the VU1 does |
| D | 0x1900-0x20c8 | `a786efabfa469ada` | Guard band and backfaces; its own triangle-list path is declined. Also the name of its compiled routine, the VIF1 thread's busiest (`docs/STATUS.md`) |
| D | 0x1900-0x1c38 | `126a58a39a8f944a` | Guard band; strips and lists |
| D | 0x1900-0x1bc0 | `4b7b81f2fe1beeef` | No culling; strips and lists |
| D | 0x1900-0x21e0 | `93cfd2f5a1056991` | Guard band and backfaces with `b8ea8ed5`'s depth and bounds; its triangle lists are declined |
| D | 0x1900-0x1cb8 | `634107b0f8a88c88` | The guard band (its z before the scale) with `b8ea8ed5`'s depth and bounds |
| D | 0x1900-0x1b98 | `b8ea8ed53805b0c4` | No culling; z times 87.z, at most 1.y; the vertices' bounds (x, y, z, fog) widen quads 98 and 99, which the `:960` sprite programs read, so `skip` and `gpu` write them too (`applyMemoryEffects`) |

E (clipping) is never ported: the GPU clips the mesh (`gs_native.vert`
outputs homogeneous positions), so a clipping chunk goes to the GPU like any
other. The packet paths (`1`, `skip`) leave clipping chunks to the VU1.

## Status (2026-10-28): every recorded program

Every program the recordings hold is native: the field walk
(`field1.vu1r`), its 10-list capture with the memory after each run
(`fieldafter.vu1r`) and the harvest of one evening's play (battles, a boss
and events among it). Each agrees with the VU1 at zero tolerance
in the oracle, packets and VU1 memory; in-game `check` on the walk compared
1,541,024 chunks, none different. In `gpu` mode the walk runs no VU1
program at all.

| Kind (`Kind`) | Programs | Field vertices | Pieces | In `gpu` mode |
|---|---|---|---|---|
| Strips | 9f727028, cfb58657, 30da21d7, ... (most) | about 78% | A 07151c25; B plain or skinned; C vertex colors, one color, lit, lit vertex colors; D five; F scroll or sphere map | Mesh, the GPU transforms and clips |
| Outlines | 3dd4ba5d and 5 more | 17.1% | A 3f6df595, B 542b235c, C 3689bac9 | Packets |
| Shadow volumes | 0f44ab11 and 11 more | 5.0% | A 821b638f, B 542b235c or a293001f, C dc8dbfe4 | Packets |
| Overlay sprites (`:960`) | 4cb64ec1 and 28 more | 0.2% | B 6e42dac6 at its own MSCAL | Packets, VU1 memory |
| Object setup (`:0`) | before every object | none | base 12ad73d2 | Packets, VU1 memory |
| Particles | c56c50af and 4 more (harvest) | none | A 0b1031ae, D 8ecadb3a | Packets |
| Projected texture | a8ff1228 (harvest) | none | A 4beba151, F 15be21a5, C 3b890d00 | Packets |

Left to the VU1, chunk by chunk:

- **Clipping** (slot E: 6e420ef8, and 860dd144 in the projected texture)
  in the packet modes (`check`, `1`, `skip`). The GPU path takes the strip
  programs' clipping chunks and draws them clipped; it never needs E.
- **Paths no recording took**: the triangle lists of the backface D pieces
  (a786efab, 93cfd2f5) and of the projected texture's C; 87.y & 0x100 in
  the vertex-color C pieces (70362be4, 2460e528).
- **Pieces no recording reached** (`census.py`): A 8855d9e7, c8143ab4,
  964be3bc, cf7a85a5 (at 0x0c8); B 36d686cd, 89123290, 857fbbc2; C
  127ca0b4, e3bca52a; D 1bd1b5c0, 2feaa9a1, 312d076b; E 56d6683a, ab37fe79,
  c9e1cecc; F 0594a063, 9df36d6a, a04c11ce, ad209cd3, b1d29245. (2aa11602,
  in every slot, is the empty piece; base f1d259d7 is VU0's.) A program
  with one of them is left whole to the VU1. Harvest mode collects their
  samples where they run: other planets, more battles and events, menus.

## Speed with every program native (2026-10-28, RX 6700 XT)

The same walk and measure as below (`native-perf.sh`, last 25 s, 60 fps
mode):

| Mode | Scale | fps | GS backend | VIF1 | GIF stage | draws/s | GPU busy | Power |
|---|---|---|---|---|---|---|---|---|
| `0` | 3x | 59.9 | 63% | 45% | 16% | 45,806 | 73% | 61.1 W |
| `skip` | 3x | 59.9 | 64% | 37% | 16% | 46,013 | 75% | 60.9 W |
| `gpu` | 3x | 59.9 | 71% | 18% | 7% | 110,789 | 73% | 62.0 W |
| `0` | 8x | 39.0 | 188% | 31% | 78% | 47,776 | 99% | 183.8 W |
| `gpu` | 8x | 37.8 | 162% | 13% | 94% | 117,891 | 99% | 177.4 W |

- `gpu` takes the VIF1 thread from 45% of the wall to 18% at 3x, and the
  GIF stage from 16% to 7%: the VU1 and the GS front end no longer see
  the meshes.
- Draw calls go up 2.4x (a draw per mesh, and the packet programs'
  draws around them); the GS backend's share goes up with them.
- At 8x every mode is GPU-bound (99% busy), and `gpu` is 3% slower: the
  extra draw calls cost there too. Fill and the post passes dominate
  (`docs/GS-PERFORMANCE.md`).

Then (2026-10-29) the meshes of one draw state share a draw call (their
transforms in a storage buffer, `thirdparty/dq8-gfx/README.md`), and a
mesh wholly off the scissor or behind the eye is not drawn. On the walk,
per frame: 769 meshes of 25,000 vertices; 196 of them (7,000 vertices)
off screen; 379 the same bytes as the frame before, none repeated within
one.

| Mode | Scale | fps | GS backend | VIF1 | GIF stage | draws/s | GPU busy | Power |
|---|---|---|---|---|---|---|---|---|
| `gpu`, merged | 3x | 59.9 | 56% | 18% | 8% | 51,248 | 72% | 62.3 W |
| `gpu`, merged and culled | 3x | 59.9 | 55% | 18% | 7% | 46,014 | 72% | 61.3 W |
| `gpu`, merged | 8x | 38.9 | 180% | 13% | 86% | 51,795 | 99% | 181.6 W |
| `gpu`, merged and culled | 8x | 39.0 | 180% | 13% | 86% | 48,672 | 99% | 183.4 W |

- The native strips take about a third of the draws they did (14,820 for
  42,600 meshes a stats interval), and `gpu` makes no more draws than the
  VU1 did; the GS backend is 55% of the wall at 3x, against 63% with the
  VU1 and 71% before.
- Culling takes a quarter of the meshes but few draws: most off-screen
  meshes had shared a draw already. It saves their upload and vertex work.
- At 8x `gpu` is level with the VU1 again (39.0 fps): the GPU's fill is
  the limit, not the meshes.

## Speed (2026-10-28, RX 6700 XT, the four programs above native)

Per chunk, on the recording (`rg_vu1_oracle field1.vu1r --time`):

| | µs a run |
|---|---|
| Compiled VU1, `cfb586576df9c3d2` (same 10,680 runs) | 2.93 |
| Native packets, same runs | 1.57 |
| Compiled VU1, `9f727028f73861c3` (all 20,720 runs, clipping ones included) | 6.58 |
| Native packets, its 17,800 non-clipping runs | 1.71 |
| Native mesh (`gpu` mode's CPU part), every chunk taken | 0.26 |

The compiled VU1 programs take about 8 ms a frame of this scene, the four
native ones about 4.8 ms of it; as meshes they cost about 0.17 ms.

In the game, the last 25 s of the walk in the 60 fps mode
(`native-perf.sh`):

| Mode | Scale | fps | GS backend | VIF1 | GIF stage | draws/s | GPU busy | Power |
|---|---|---|---|---|---|---|---|---|
| `0` | 3x | 59.9 | 65% | 46% | 17% | 45,644 | 68% | 65.0 W |
| `skip` | 3x | 59.9 | 65% | 44% | 17% | 45,990 | 73% | 63.3 W |
| `gpu` | 3x | 59.9 | 62% | 27% | 13% | 78,958 | 69% | 63.7 W |
| `0` | 8x | 38.8 | 188% | 31% | 77% | 47,937 | 99% | 182.2 W |
| `skip` | 8x | 38.9 | 188% | 28% | 78% | 48,182 | 99% | 182.9 W |
| `gpu` | 8x | 39.0 | 187% | 18% | 87% | 85,709 | 99% | 182.2 W |

- At 3x the frame rate is at the cap; `gpu` takes the VIF1 thread from 46%
  to 27% of the wall (about 3.2 ms a frame of headroom) with 4 of 21
  programs native. `skip` saves little: building the packets and running
  them through the GS front end costs about what the VU1 did.
- At 8x every mode is GPU-bound (99% busy, 182 W, 39 fps): native strips
  draw the same pixels. What costs there is fill, the post passes above all
  (`docs/GS-PERFORMANCE.md`), which is phase 3 of the plan, not this.
- `gpu` mode issues a draw call per mesh (draws/s up 72%); see above for
  the merged draws.

## Lessons

- **Read the prologue.** The cull D pieces also test the strip's second
  vertex against a "previous" vertex of zero registers, and set its ADC;
  it never draws, but it is in the packet. The oracle found it.
- **The cull face is a sign convention.** The GPU culled the wrong face on
  the first run (the inside of buildings showed); one flip
  (`DQ8_GFX_NATIVE_CULL_FLIP` swaps it without a rebuild). Check a frame
  of each new culling piece.
- **Decline in the right order.** A chunk that clips was counted as
  "clipping" before "triangle list", which hid the lists in the oracle;
  `gpu` mode found them. Run the in-game modes, not only the oracle.
- **Flag reads have latency.** `FMEQ` after an FMAC reads its flags four
  pairs later; in the D pieces the distance is exact, so the plain reading
  holds, but check it in a new piece.
- **The VU truncates.** Products and sums round toward zero. The native
  code runs under the same rounding (`VuRounding`, MXCSR round toward zero,
  around each entry point) and does the arithmetic in the VU's order, which
  makes it bit-exact: the oracle at zero tolerance agrees on every run. The
  GPU cannot round that way, so the mesh carries each vertex's Z from the
  CPU (`GSNativeMesh::Vertex::depth`); a pass drawn over another at the same
  depth lost pixels without it.
- **The VU has no denormals.** `VuRounding` also sets DAZ and FTZ: the
  overlay sprites add a denormal to -2^24, which rounds toward zero to
  -2^24 + 1 on x86 and stays -2^24 on the VU. In-game `check` found it as
  a VU1 memory difference, in two chunks of 1.4 million.
- **The culling pieces set the first vertex's ADC** from its own guard band
  judgment; the oracle's recordings never had a first vertex outside it.
  Run `check` in the game after the oracle passes.
- **VU1 memory is output too.** A piece that writes data memory a later
  program reads (the bounds of `b8ea8ed5`) must do so natively when the VU1
  is skipped; check mode compares it (`memoryEffectsMatch`).
- **Commands over SSH run in zsh** on the build machine: `set -- $pair`
  does not split, and an empty argument vanishes. Wrap loops in `bash -c`.
