# GS performance ledger

Every finding and candidate for the GS side (the SDL GPU backend in
`thirdparty/dq8-gfx`, the front end, the VIF1 and GIF threads), with what
was measured and where. Goal: an iGPU at 1080p and Android handhelds
upscaled. A candidate is only closed with a reason that holds for that
hardware; a small share of power on a desktop card (RX 6700 XT) is not one.

Measured with `DQ8_GFX_PROFILE_CYCLE` (see `sdlgpu_backend.cpp`,
`tools/bench/gpubusy.sh`; both on the contributor's machine, not in this
repository): the game steps through categories of GPU work, skipping each
in turn, and the card's power draw says what it cost. Run-to-run noise is
about 1.5 W. All numbers: Salgin's first scene, 4x, 60 fps, baseline
72-85 W, 2026-10-28.

## Measured

| What | Power left out | Notes |
|---|---|---|
| every draw | 42-50 W (53-59%) | with them gone the card idles at 34 W: passes, copies and uploads cost almost nothing |
| 3D scene (draws into a depth target) | 27-38 W (37-45%) | blended 11-19 W, opaque 14-24 W |
| draws covering half their target or more ("big") | 29-32 W (37-40%) | 102 a frame, 120 Mpx of bounding boxes |
| draws no VU1 program made | 36 W (46%) | 219 draws, 93 Mpx, 73 Mpx of it "big"; built by the EE |
| of those, the ones covering half *their own target* or more | 27.6-28.5 W (35-37%) | 72 Mpx, ~86 draws. By what they do: composites 44% (41 draws into the 192x192 bloom buffer, 17 Mpx; 4 full frames, 14.7), clears 27% (~10 draws, 19.3 Mpx), copies 22% (3 full frames, 11.0; 25 into the bloom buffer, 4.1), fades 8%. About 13 genuinely full-frame passes plus ~66 smaller draws. An upper bound on what fixing them saves: skipping a depth clear or a copy also changes what later fragments do. (An earlier version of this row said "~20 full-frame draws"; that divided by one frame and was wrong) |
| of those, flat / other depth-tested | 6.4 W / 1.1 W | |
| blur and bloom draws ("feedback") | 7-12 W (10-15%) | 192x192 target at 0x2C40: 69 draws, 16 Mpx |
| VU1 program `#6bcb` (pc 0x160) | 8.2 W (10%) | 6-8 Mpx, 86 draws of ~15 vertices; atest mode 6 (the others use 5) |
| VU1 program `#e35a` | 2.6-4 W | 50 Mpx, 67 draws of ~155 vertices |
| VU1 program `#0970` (untextured, blended, atest) | 1-2.6 W | 80-100 Mpx, 310 draws: cheap per pixel |
| feedback snapshot copies | 1.7-5 W (2-6%) | 117 a frame, 99% full-surface; 27 GB/s at 4x, 73 GB/s at 8x |
| one texel instead of four in the 3D filter | 2.7-4.7 W (3-6%) | `onetap` |
| flat draws (no depth, no snapshot) | 4-7 W | sprites, UI |

Other figures: 170 render passes, 36 flushes, 5.5 submissions a frame; the
backend is 62% of wall time at 4x and 130-190% at 8x.

## Candidates

| # | Candidate | Status |
|---|---|---|
| 1 | What the EE-built draws are (the 36 W block) | **done in part**: 28.5 W is ~20 full-frame passes (clears, copies, blends; about 13, as the measured row above corrects). Next: classify each exactly |
| 1a | Replace whole-target clears (untextured, opaque, constant colour and depth) with a render-pass load-op clear | open; `clearsWhole` already detects a sprite that replaces every pixel |
| 1b | Replace 1:1 opaque full-frame copies with GPU copy commands | open; needs the classification in 1 |
| 1c | Write traffic of the whole-target draws: 72 Mpx x 8 B = ~0.6 GB a frame, ~35 GB/s at 60 fps (arithmetic, not measured) | the iGPU argument for 1a-1c |
| 1d | Bloom/effect buffer (0x2C40, 192x192; 66 draws, 21 Mpx, `feedback` step ~10 W) at a lower scale than the scene | **done** (`d911d1e`: `DQ8_GFX_EFFECT_SCALE`, 2 unless set, for 0x2C40 and 0x3800; [GS-AUDIT.md](GS-AUDIT.md)); was: open; the backend already handles targets of different scale |
| 2 | Run effect buffers (192x192 bloom, blur) at a lower scale than the scene | **done** with 1d (`d911d1e`); was: open; needs 1 |
| 3 | Skip or merge redundant full-target passes | open; needs 1 |
| 4 | Region-limited feedback copies (copy the sampled rectangle, not the surface) | **done** (`8a0d39d`: a snapshot copies only the texels its draws read; 255 -> 112 Mpx copied a frame at 8x); was: reopened for iGPUs: 27 GB/s is a third of their memory bandwidth. Closed on the 6700 XT only |
| 5 | Hardware bilinear for common 3D draws (one fetch, linear sampler) | **reopened for iGPUs**: 3-6% on the 6700 XT, relatively more where TMU and ALU are scarce |
| 6 | Early depth with alpha-tested draws (`discard` blocks early-Z) | **tried and dropped**: no change in frame rate; never committed ([GS-AUDIT.md](GS-AUDIT.md)); was: `roguegalaxy-earlyz` built, not run |
| 7 | Why `#6bcb` costs 40x more per pixel than `#0970` | open |
| 8 | PCSX2 power at the same scene and internal resolution (is any of this waste?) | not run; needs the user |
| 9 | CPU side | measured: at 4x in the scene the GS worker is 66% of one core (48-76%), VIF1 thread 50%, GIF stage 17%; 807 draws, 192 passes, 38 flushes, 5.2 submissions a frame. On a core 2x slower the worker alone is 130%. Since (2026-11-09, which functions, by profile): see 9a, 9b |
| 9a | Zerard's central plaza, after the Juraika boss (the "fps halves" of issue 17's save), 4x, 60 fps mode, the user's settings, NVIDIA: 39.7 fps, the GS worker the limit (idle ~4%), a sixth of it waiting for GPU readbacks (`downloadScaled`, 2 texture resolves a frame, 4.5-6.5 ms) and about a quarter in their whole round trip | **done**: the resolves were for **sprites of zero width or zero height** (the GS draws no pixel), six a frame reading the 512x448 buffer 0x2A00 as CT24, which the GPU path refuses (`samplesFitSurface` has no samples to fit) and `primitiveBounds` rounds out to a pixel. `submitOne` now drops a sprite whose vertices share an x or a y (`empty sprites=N` in the stats, `DQ8_GFX_NO_EMPTY_SPRITES=1` turns it off); 45-85 a frame in the street, gate and battle. Plaza 39.7 -> 51.2 fps (old and new alternated on an idle machine), readback wait 0, the VIF1 thread then the limit (92%); battle 54.4/54.7 -> 57.2/54.8; gate unchanged (VIF1-bound). Frames identical (whole GS memory of a logged frame, 30 fps mode: street 1x and 4x, gate and battle 3x and 4x; with the native renderer street 3x, gate 3x and 4x). Next in the worker, diffuse: the driver 15%, memcpy 8%, the per-primitive path (~49k primitives a frame) ~27%, texture checks and decodes ~10% |
| 9b | The same plaza, the native renderer on (59.3 fps, the GS worker and display 95% of the wall): by profile, `regionForPages` called from `GsTargetCache::markDrawn` was a tenth of the worker's stack samples. After each draw call (~800 a frame) markDrawn merges the draw's pages into every other surface sharing them (14 a call, of 57); 93% of those were already due for a whole upload | **done**: a surface whose `needsUpload` already contains its whole region is skipped (`dueWhole`; `regionForPages` never leaves the surface, so the merge changed nothing there), in markDrawn and in `invalidate`'s GPU-copy path, and markDrawn walks only the pages both share; `DQ8_GFX_NO_WHOLE_UPLOAD_SKIP=1` merges into every one, and the stats line counts the `upload merges` left (plaza ~11,300 a frame -> ~700). 4x, 60 fps mode, alternated on an idle machine, two batches (before and after the review's fixes; the second's old path already walks only shared pages): native on, worker 92% -> 83% and 91% -> 87% in the plaza, 86/81% -> 74/76% and 78/78% -> 74/72% at the gate, the battle within noise; native off (the default), plaza 84% -> 80% and gate 76/77% -> 73/73%, fps unchanged (50, VIF1 the limit). The fps here stays at its cap: the gain is room where the worker is the limit. Frames identical (whole GS memory of a logged frame, 30 fps mode: street 1x and 3x, gate 3x, battle 3x and 4x; with the native renderer street 3x and gate 4x; one battle byte at 0x35E181 changes from run to run with or without the switch) |
| 10 | ARM: sse2neon in `gs_vram`, `ee_fpu` rounding (patches 0003, 0004 assume MXCSR), `_mm_getcsr` in `vif1_thread` | open; compile an aarch64 build first |
| 11 | A game-aware renderer replacing the GS state machine, one VU1 program at a time | open; needs the attribution above (done for programs, not yet for the EE-built draws). Since: the plan is [NATIVE-RENDERER.md](NATIVE-RENDERER.md), and every recorded VU1 program is native behind `RG_VU1_NATIVE` ([VU1-NATIVE.md](VU1-NATIVE.md)) |
| 12 | Android packaging (SDL3 activity, data path, touch/pad input) | open |

## How to add a row

Run the cycle (README of the branch, or `grep -n DQ8_GFX_PROFILE_CYCLE
thirdparty/dq8-gfx/src/gfx/backends/sdlgpu/sdlgpu_backend.cpp`), then put the
numbers here with the date. (The cycle and that README were on the
contributor's machine and are not in this repository; the grep finds only a
comment. What is here is `DQ8_GFX_GPU_TIMES`, the GPU's time by label,
[GS-AUDIT.md](GS-AUDIT.md).) Close a candidate only with a measurement or a
reason that holds for an iGPU or a handheld.
