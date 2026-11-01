# VU1 recon: what the game's 3D path gives a temporal upscaler

2026-10-28. The question: can DLSS, FSR 2/3 or ray tracing be added without
replacing the GS emulation? The GS stream alone is not enough (it sees
vertices after VU1 has projected them: no matrices, normals or lights), but the
VU1 programs read everything needed from VU1 data memory, and that memory is
ours. This file records what was measured, what is built, and what is left.

Work so far: [PR #2](https://github.com/notavolleyball-dev/rogue-galaxy-recomp/pull/2)
(classifier, capture, report) and branch `vu1-jitter-motion-vectors` (jitter,
camera plumbing, this file).

## Tools

| | |
|---|---|
| `tools/vu1/analyze.py` | Classifies the recorded VU1 images in `build/vu1-manifest` (matrix multiplies, divides, clips, unpackers, fixed quadword slots). `--dump HASH:PC` prints a disassembly. |
| `RG_VU1_CAPTURE=<file>` | `src/vif1_thread.cpp`: at each program start, the first `RG_VU1_CAPTURE_MAX` (4000) starts keep all 16 KiB of VU1 data memory and the EE address the model matrix came from; the next `RG_VU1_CAPTURE_LISTS` (1500) DMA lists keep only the matrices and two mesh hashes. Slows the game; one null check without the variable. |
| `tools/vu1/capture_report.py` | Reads a capture: per-program varying quads, the matrix check, object identity, and how well a draw pairs with itself in the next list. |
| `RG_JITTER=1` (`RG_JITTER_SCALE=n`) | Sub-pixel Halton(2,3) jitter, 8 phases, on the 3D scene's draws only (see below). |
| `RG_CAMERA_LOG=N` | Every N presents, the cameras the scene's draws ran under, busiest first. Needs `RG_JITTER=1` so draws carry a camera. |

```
RG_VU1_CAPTURE_LISTS=8000 RG_VU1_CAPTURE=build/vu1.cap build/game/src/roguegalaxy disc/SCUS_974.90
python3 tools/vu1/capture_report.py build/vu1.cap
RG_JITTER=1 RG_CAMERA_LOG=60 build/game/src/roguegalaxy disc/SCUS_974.90 2> build/camera.log
```

## What VU1 data memory holds (verified)

Quadwords of 16 bytes, matrices as four row quads, row-vector convention
(`v * M`; the programs do `MULAx/MADDAy/MADDAz/MADDw`).

| Quads | Contents | Changes |
|---|---|---|
| 4-7 | Viewport transform (clip to GS coordinates) | per camera |
| 8-11 | Projection | per camera |
| 24-27 | View (rotation rows, camera translation in 27) | per frame |
| 60-63 | Model to world | per object |
| 64-66 | Model rotation, orthonormal (probably for normals) | per object |
| 72-75 | Model x view x projection (clip space) | per object |
| 68-71 | The same, with the viewport applied | per object |
| 103-104 | Section pointers (103) and vertex count (104 x) of the mesh | per draw |

- `60-63 x 24-27 x 8-11 = 72-75` held on every draw start checked: 2,936 in the
  first capture, 74,649 in the second (relative error at most 1e-3; the
  large captures sampled 50,000 starts, worst 1.9e-4).
- The pc 0x000 program (86 pairs) builds 64-75 from the inputs; the pc 0x1900
  programs (83-249 pairs) and pc 0xb00 read 68-71, divide by w and convert XY
  with `FTOI4` (GS 12.4). The 600-pair programs at 0x2620 and 0x28d8 (13
  divides, 16 clips, 6 `XGKICK`) probably do lighting; not read.
- The main projection: focal 1.5377 x 2.7337, near 1, far about 60,134. A
  second state (focal 1.4464 x 2.5714, view translation (0, 0, -50)) is a menu
  or preview scene. Other captures showed near 5, far 5000.
- Vertices in the 0x1900 stream are model-space floats (a character-sized mesh
  reads about 33 units tall). Normals, UVs, colors, skin weights: not decoded.
  The first two quads at TOP are either pointer words (`4 2c 802c 2d`,
  `5a 87 b4`) or screen-space floats, depending on which of the two buffers.

## Motion

From 973,784 draws over 2,309 consecutive DMA lists:

- About 75% of draws do not move between consecutive lists (median 0.0 units,
  90th percentile 1.1, 99th 7.7). For them, camera-only reprojection is exact.
- 0.7% moved more than 50 units.

### Object identity

- **The EE address of the matrix upload is not an object ID.** The lists
  alternate between two buffers (addresses recur every 2 lists, offset about
  0xF0000 but not constant), and objects shift within a buffer as others
  appear.
- **A mesh shape key works:** program tag, vertex count (quad 104 x), and the
  distances between the four section pointers (quad 103). Absolute pointers and
  the quads at TOP must not be in it: they change with buffer placement and
  camera, and made 40-50% of the animated programs' draws look new.
- Pairing each draw with its nearest pose in the next list (translation plus
  rotation rows, each candidate used once), over 2,309 lists:

| Program | Draws | No counterpart | Exact | Confident | Uncertain |
|---|---|---|---|---|---|
| `8000dd3b` | 280k | 1% | 47% | 49% | 2.5% |
| `8320e35a` (world) | 197k | 1% | 95% | 4% | 0.4% |
| `81606bcb` | 138k | 0% | 18% | 81% | 0.0% |
| `8320beb9` | 101k | 2% | 58% | 38% | 2.6% |
| `83203ef4` (characters) | 81k | 1% | 20% | 78% | 1.1% |
| all | 974k | 1% | 52% | 45% | 1.3% |

  "Confident" means the best pose is clearly nearer than the next, moved under
  2 units, or the rival sits at the same pose (either pick gives the same
  motion). That last rule was added after the first pass, which had
  `81606bcb` at 54% uncertain; those were sub-draws sharing one pose.
  This is internal consistency, not ground truth: the matcher prefers small
  motion, so a very fast object could pair with the wrong neighbor.

## Cameras

`RG_CAMERA_LOG` over a play session (field, a menu, battle transitions):

- Usually one new camera per frame (60 per 60 presents), sometimes two (120).
  The second is unidentified.
- A static camera keeps one id for many frames (one id carried 5,400-11,500
  draws): previous camera = current camera, zero motion for static geometry.
- Walking moves the view translation by a few units a frame, smoothly.
- 600-2,000 scene draws a frame per main camera.

## What is built

- **Jitter.** `sdlgpu_backend.cpp` adds a Halton(2,3) offset (8 phases, up to
  half a host pixel times `RG_JITTER_SCALE`) to `draw.vertexUniforms.adjust`,
  for draws from a VU1 program that test depth. HUD and dialog boxes did not
  move; the world's textures shimmer, as expected without temporal
  accumulation. The game tags draws with their program only when `RG_JITTER`
  or `DQ8_GFX_PROFILE_CYCLE` is set.
- **Camera plumbing.** `patches/ps2recomp/0020-gs-source-camera.patch` adds
  `SceneCamera` (viewport, projection, view), a `CameraRegistry` (the newest
  4096), and `GSPrimitiveBatch::sourceCamera`. `src/vif1_thread.cpp` registers
  the camera at each VU1 program start and tags packets through the GIF stage
  (its header grew from 16 to 32 bytes). The backend keeps it on `DrawBatch`.

## What is not built: the motion-vector pass

Per pixel of the scene: depth to a world position, through the previous
frame's camera, difference to a motion-vector target. Exact for static
geometry (about 75% of draws, most of the world). For moving objects, a
per-object transform from the matched previous pose; uncertain or unmatched
draws fall back to camera-only vectors and will ghost.

Things to settle first:

1. **The GS depth and position mapping.** Reconstructing a world position needs
   the inverse of view x projection x viewport at the scene's GS coordinates
   (XYOFFSET, the 2048 center, the Z scale and offset in quads 6-7, and the
   backend's depth normalization). The quads read as: X = 393.66 x - 2048 z,
   Z_h = 268 z + 16,700,265 w, W_h = -z, but this was not checked against
   rendered output. **Debug first**: a view of the reconstructed world position
   as a repeating pattern. If it stays pinned to surfaces while the camera
   moves, the math is right.
2. **Depth precision** with near 1 and far 60,134.
3. **Which camera is the scene's** when a frame has two: the busiest per
   present is the working rule.
4. **Pass order.** The scene is two passes (pass 80 into 0x0, 83 draws; pass 81
   into 0x2A00, 325 blended draws) before the bloom blur chains and the
   per-object composites (`GS-PASS-GRAPH.md`). The upscaler has to take the
   scene before those. Pass 81's sprites draw into a second target; how to give
   them vectors is open.
5. **Depth sampling in SDL GPU.** The scene's depth surface has to be readable
   by a pass.
6. **FSR 2/3 and DLSS want native Vulkan handles and command buffers.** SDL's
   GPU API may not expose them; not checked. A hand-written TAA/FSR-style pass
   in ordinary shaders avoids the question and doubles as the proof that the
   vectors are right.
7. **Texture LOD bias** and sampler settings for a jittered, lower-resolution
   scene.

Then: an upscaler (or TAA first), then ray tracing, which needs the vertex
format (normals, UVs, colors, skinning) and a decision about the baked or
VU1-computed lighting. Not started.

## Estimates

Jitter and camera plumbing are done. The motion-vector pass with a debug view:
a few days. A TAA resolve on top: a few more. An upscaler SDK: depends on item
6. Ray tracing: weeks to months, mostly vertex decoding, material recovery and
lighting.
