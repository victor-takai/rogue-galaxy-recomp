# GS backend audit, 2026-10-29

Where the field walk's frame goes on the GPU and the CPU, with every VU1
program native (`RG_VU1_NATIVE=gpu`), what was cut, and what is left.
RX 6700 XT; the walk and measure of `tools/vu1/oracle/native-perf.sh`
(File 01's street, 60 fps mode).

## Method

At 8x the walk is GPU-bound (99% busy), so frame rate is a direct measure
of GPU work. A temporary probe in `sdlgpu_backend.cpp` (not committed)
waited until the party stood still, then skipped one category of draws
for 5 s at a time and logged the frame rate of each step, with a baseline
first and last; it also counted passes, snapshot copies and the draws of
one frame by target and state. The uncommitted `DQ8_GFX_PROFILE_CYCLE` in
the main checkout does the same with power; this one reads frame rate.

## Done

| Change | Commit | 8x fps | 3x draws/s | 3x GS backend |
|---|---|---|---|---|
| Before (gpu mode, a draw per mesh) | 6d4e848 | 37.8 | 110,789 | 71% |
| Meshes of one state share a draw | e625ba1 | 38.9 | 51,248 | 56% |
| Meshes wholly off screen not drawn | cac9414 | 39.0 | 46,014 | 55% |
| Snapshots copy only what is read | 4f2f822 | standing: 41.7 -> 43.4 (the walk window not run) | | |
| Source-alpha blends in one pass (dual-source) | d1c6a8c | 44.2 | 38,397 | 56% |
| Upscaled texture taps without integer divides | 000e310 | 48.8 | 38,393 | 56% |
| Upscaled taps read directly inside a clamp | 1a8b8b8 | 55.9 | 38,404 | 55% |
| The fast path for CT24 and CT16 views too | 4e48219 | (timed: 53.5 -> 55.4) | | |
| A texture at its own size: each axis wraps once | 24792e0 | 59.9 (the 60 fps cap) | 38,393 | 53% |
| The blur buffers at scale 2, pages moved between scales on the GPU | "The blur buffers at scale 2" | 59.9; GPU busy 99% -> 84%, 185 -> 160 W | 38,555 | 49% |

The VU1's own mode (`0`) gets the GS-side ones as well (55.6 fps at 8x
before the last two). At 3x the GPU's power went 61.3 -> 46.1 W over the
day. At 8x the walk now holds 60 fps with the GPU still 99% busy, so the
GPU timer, not the frame rate, measures what follows. At 3x both modes hold 60 fps; `gpu` keeps the VIF1
thread at 18% of the wall against 45%, and the GS backend at 56%
against 66%.

Tried and dropped: a fragment shader without the alpha test's `discard`
for draws whose test always passes, so the depth test could run first
(GS-PERFORMANCE.md candidate 6). No change in frame rate (43.5 against
43.4 fps); not committed.

## GPU time by label (DQ8_GFX_GPU_TIMES)

Skipping categories cannot separate a draw's cost from the stalls around
it, and read both wrong once (it said the passes were the cost; forcing
three times as many cost 1 ms). `DQ8_GFX_GPU_TIMES=1` (patched SDL,
`thirdparty/dq8-gfx/README.md`) times the GPU's work by label instead.
It found textured full-target draws at about a sixth of an untextured
one's rate a pixel: the shader's four taps for an upscaled target were
mostly arithmetic (eight integer divides, then the wrap and sub-texel
round trip). The two shader commits above took a full-frame textured
blend from 0.69 to 0.28 ms.

After them, 8x, standing, about 53 fps with the timestamps (ms a frame):

| Label | ms |
|---|---|
| per-object composites and other textured blends into the frame | 2.0 |
| the scene's meshes | 1.8 |
| full-frame textured blends into the frame (5) | 1.4 |
| the two full-frame copies into 02a40 | 1.2 |
| blur and bloom draws | 1.6 |
| snapshot copies (blur) | 0.9 |
| shadow volumes (02a00 3D blend) | 1.0 |
| importing pages between aliased targets | 1.0 |
| feedback copies of sampled targets | 0.7 |
| patches from local memory | 0.9 |
| draw data upload | 0.3 |

After all four shader commits, 8x, about 57 fps with the timestamps
(about 17.4 ms of GPU a frame with their cost):

| Label | ms |
|---|---|
| blur and bloom: feedback draws 2.0, their snapshot copies 0.9 | 2.9 |
| full-frame textured blends into the frame (7) | 1.4 |
| per-object composites (CT24 view) | 1.2 |
| shadow volumes | 1.1 |
| importing pages between aliased targets | 1.05 |
| scene meshes | 1.4 |
| feedback copies of sampled targets | 0.7 |
| patches from local memory | 0.9 |

The tables below come from before the shader commits, by skipping
categories; their proportions have moved since.

## Where the frame goes now (8x, standing, 20.0 ms)

Frame time saved by skipping each category (they overlap):

| Skipped | fps | ms saved |
|---|---|---|
| nothing | 50.1 | |
| flat draws (no depth buffer: blur, bloom, sprites, UI) | 69.4 | 5.6 |
| draws into the per-object buffers (02a00 and the others with depth) | 62.8 | 4.1 |
| blur and bloom draws (feedback: sampling their own target) | 58.9 | 3.0 |
| snapshot copies | 53.0 | 1.1 |
| shadow volumes | 52.8 | 1.1 |
| native meshes | 51.7 | 0.6 |
| every 3D draw | 75.6 | 6.8 |
| every draw | 75.6 | 6.8 (the CPU limit at 8x, about 75 fps) |

One frame: 626 draws (91 of them native meshes), 195 render passes, 105
snapshot copies of the blur's buffers, 14 snapshot copies through local
memory's padding path.

## What is left, by size

1. **Chains of draws that each read the one before.** Pass breaks as such
   are cheap: forcing a pass per batch (195 -> about 650 passes a frame)
   cost about 1 ms, 2 us a pass. What costs is work that must wait for the
   work before it. The blur adds shifted copies of a 192x192 buffer to
   itself (offsets like +0.62, -1.62, +1, -2 pixels, bilinear), each copy
   drawn as 32-pixel strips; the strips of one copy already share a
   snapshot and a draw, but each copy reads the whole result of the one
   before, so each needs a snapshot of its own: about 9 copies a blur, 60
   to 100 snapshots a frame, a dependent chain of small copies and draws
   (about 3 ms at 8x with its copies). The per-object composites (about 20
   to 30 a frame: clear a region of 02a00, draw the character against the
   scene's depth, blend the region into the frame) are a chain of the same
   kind, shorter. Shortening either means doing the effect another way: the
   blur's copies summed in one or two passes of a shader of its own, which
   matches the GS's sequential result only to the rounding of each step.
   A sliver rule (a draw whose reads overlap the writes since the snapshot
   by two texels or less reuses it) was tried: 111 -> 92 snapshots a frame,
   119 pixels off by 1 or 2 levels; not kept.
2. **The effect buffers at the scene's scale** (done for the two blur
   buffers: `DQ8_GFX_EFFECT_SCALE`, 2 by default; it costs a few pixels by a
   few levels, see above).
   Originally: The 192x192 blur and bloom
   buffers are 1536x1536 at 8x, larger than the PS2's frame. At a scale of
   2 they would cost a sixteenth in fill and copies. The backend keeps
   every target at one scale, and these buffers alias others' pages, so
   it is a change to the target cache (GS-PERFORMANCE.md candidates 1d
   and 2).
3. **Shadow volumes, 1.1 ms.** 287 draws a frame, untextured, counted by
   alternate add and subtract blends; they cannot share draws without
   reordering a clamped count. Fill bound.
4. **The CPU limit at 8x, about 75 fps**, with every draw skipped. Not the
   limit today.

The alpha replays (dual-source now) were the one large cost that was a
pure duplicate; the rest is work the frame does once. A GPU timer per pass
(SDL 3.2 has no timestamp queries; it would be another patch to it) would
say where the rest of the 20 ms goes better than skipping categories does.
