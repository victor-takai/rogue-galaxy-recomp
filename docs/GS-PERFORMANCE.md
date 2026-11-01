# GS performance ledger

Every finding and candidate for the GS side (the SDL GPU backend in
`thirdparty/dq8-gfx`, the front end, the VIF1 and GIF threads), with what
was measured and where. Goal: an iGPU at 1080p and Android handhelds
upscaled. A candidate is only closed with a reason that holds for that
hardware; a small share of power on a desktop card (RX 6700 XT) is not one.

Measured with `DQ8_GFX_PROFILE_CYCLE` (see `sdlgpu_backend.cpp`,
`tools/bench/gpubusy.sh`): the game steps through categories of GPU work,
skipping each in turn, and the card's power draw says what it cost. Run-to-run
noise is about 1.5 W. All numbers: Salgin's first scene, 4x, 60 fps,
baseline 72-85 W, 2026-10-28.

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
| 1 | What the EE-built draws are (the 36 W block) | **done in part**: 28.5 W is ~20 full-frame passes (clears, copies, blends). Next: classify each exactly |
| 1a | Replace whole-target clears (untextured, opaque, constant colour and depth) with a render-pass load-op clear | open; `clearsWhole` already detects a sprite that replaces every pixel |
| 1b | Replace 1:1 opaque full-frame copies with GPU copy commands | open; needs the classification in 1 |
| 1c | Write traffic of the whole-target draws: 72 Mpx x 8 B = ~0.6 GB a frame, ~35 GB/s at 60 fps (arithmetic, not measured) | the iGPU argument for 1a-1c |
| 1d | Bloom/effect buffer (0x2C40, 192x192; 66 draws, 21 Mpx, `feedback` step ~10 W) at a lower scale than the scene | open; the backend already handles targets of different scale |
| 2 | Run effect buffers (192x192 bloom, blur) at a lower scale than the scene | open; needs 1 |
| 3 | Skip or merge redundant full-target passes | open; needs 1 |
| 4 | Region-limited feedback copies (copy the sampled rectangle, not the surface) | **reopened for iGPUs**: 27 GB/s is a third of their memory bandwidth. Closed on the 6700 XT only |
| 5 | Hardware bilinear for common 3D draws (one fetch, linear sampler) | **reopened for iGPUs**: 3-6% on the 6700 XT, relatively more where TMU and ALU are scarce |
| 6 | Early depth with alpha-tested draws (`discard` blocks early-Z) | `roguegalaxy-earlyz` built, not run |
| 7 | Why `#6bcb` costs 40x more per pixel than `#0970` | open |
| 8 | PCSX2 power at the same scene and internal resolution (is any of this waste?) | not run; needs the user |
| 9 | CPU side | measured: at 4x in the scene the GS worker is 66% of one core (48-76%), VIF1 thread 50%, GIF stage 17%; 807 draws, 192 passes, 38 flushes, 5.2 submissions a frame. On a core 2x slower the worker alone is 130%. Next: which functions |
| 10 | ARM: sse2neon in `gs_vram`, `ee_fpu` rounding (patches 0003, 0004 assume MXCSR), `_mm_getcsr` in `vif1_thread` | open; compile an aarch64 build first |
| 11 | A game-aware renderer replacing the GS state machine, one VU1 program at a time | open; needs the attribution above (done for programs, not yet for the EE-built draws) |
| 12 | Android packaging (SDL3 activity, data path, touch/pad input) | open |

## How to add a row

Run the cycle (README of the branch, or `grep -n DQ8_GFX_PROFILE_CYCLE
thirdparty/dq8-gfx/src/gfx/backends/sdlgpu/sdlgpu_backend.cpp`), then put the
numbers here with the date. Close a candidate only with a measurement or a
reason that holds for an iGPU or a handheld.
