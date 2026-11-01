# A native renderer in place of the GS

A plan for retiring the GS emulation (local memory, page ownership, feedback
snapshots, readbacks, the register-level state machine) and drawing the game
with a renderer that knows what it is drawing: models with their matrices,
named effect buffers, and a 2D layer. Proposed 2026-10-28, for review; nothing
here is implemented except where a section says so.

## Why

The GS path works and is fast on a desktop card, but what it can do is capped
by what reaches it: screen-space vertices with fixed-point Z, textures known
only by a VRAM address, and effects drawn as hundreds of sprites that sample
the buffer they write. From [GS-PASS-GRAPH.md](GS-PASS-GRAPH.md) and
[GS-PERFORMANCE.md](GS-PERFORMANCE.md) (Salgin's first scene, 4x):

| Today | Figure |
|---|---|
| Render passes a frame | 191, of which 2 are the 3D scene |
| Snapshot passes (a copy of the whole target before a feedback draw) | 98 |
| Draws no VU1 program made (post, clears, copies, 2D) | 219 of 805, 36 W of the GPU's 42-50 W of draw work |
| GS worker thread | 66% of a core (130% on a core half as fast) |
| VIF1 thread | 50-90% busy, the compiled VU1 programs 35-56% of it |

The goal in the ledger is an iGPU at 1080p and Android handhelds. The features
people expect from a modern port all need data the GS never sees:

| Feature | What it needs | Blocked today by |
|---|---|---|
| Temporal AA / upscaling | jitter, per-object motion vectors, stable depth | vertices arrive projected; objects have no identity across frames |
| Resolution-independent post (bloom, DoF at their own size) | effects as passes with parameters | effects are sprite chains through aliased VRAM |
| UI at output resolution, 4:3 UI in widescreen | to know a draw is UI | UI and scene are the same GS sprites ("menus and text are stretched", CHANGELOG) |
| MSAA, anisotropic filtering, mip control | native targets and samplers | targets alias texture pages; CT32 viewed as T8 |
| Higher-resolution shadows, PCF | the shadow pass as a pass | the shadow map is a 64x64 / 128x128 GS buffer |
| Texture replacement packs | a stable texture identity | identity is a VRAM address (`gs_replacement_hash` is in flight) |
| Lower power on tile GPUs | few passes | 98 snapshot passes; about 470 MB of tile traffic from the blur alone (arithmetic) |

## What "dropping the GS" means here

- **Goes:** GS local memory as the source of truth (`gs_vram`, page ownership,
  imports, patches, round trips), feedback snapshots and destination copies,
  readbacks into EE memory, CLUT expansion from VRAM, the per-primitive
  register state machine for 3D and post.
- **Stays:** the recompiled game. The EE still builds its DMA chains; we hook
  functions and intercept at program starts, we don't rewrite game logic.
  SDL3 GPU stays the API (Windows, Linux, Android from one backend).
- **Shrinks:** the 2D path. About 4,800 `GsPkt*` call sites draw menus, the
  HUD and fades with GS register writes. They keep arriving as GS packets, but
  a thin translator turns them into a sprite batcher whose textures and
  targets are native objects, with no local-memory model behind them.

## Sources and trust

- **The oracle is this project's GS path.** It renders correctly, and its
  frame tools (`RG_GS_FRAMELOG`, `RG_GS_CAPTURE`, `RG_SELFTEST=gs`,
  byte-identical frames at 1x) are how every native piece is checked.
- **RGDecomp gives names and addresses only.** `functions_*.csv` derive from
  it, and every function this plan hooks is in them. Its descriptions of what
  code does (VU1 row meanings, effect parameters, the native port's design) are
  hypotheses to check here. Its own renderer is not correct yet.
- **Verified in this repo:** VU1 data memory holds projection at quads 8-11,
  view 24-27, model 60-63 and their product 72-75 at every draw start
  captured (`RG_VU1_CAPTURE`, `bc49172`). The microcode manifest
  (`build/vu1-manifest`) shows program starts at pairs 0x64, 0x12C, 0x21C,
  0x320, 0x4C4 and 0x73A. That is a six-slot layout whose pieces the game
  swaps per draw. Across 41 microcode images there are 53 distinct
  (entry, size) signatures, so about ten families of code (`tools/vu1/analyze.py`).
  A draw is matched to itself in the next frame through the model matrix's EE
  source address and the mesh shape (`capture_report.py`: 0.0% uncertain).

## Architecture

Three entry points feed one renderer.

```
EE (recompiled game)
 ├─ model draws ──► DMA chain ──► VIF1 thread ──► at MSCAL 0x64, ported program?
 │                                                 ├─ yes: NativeMesh (VU1 memory → vertex shader)
 │                                                 └─ no:  compiled VU1 → GIF → GS translator
 ├─ effect functions (hooked) ──► NativePass command, ordered by a MARK in the chain
 └─ GsPkt 2D draws ──► GIF ──► sprite batcher (native textures, named targets)
                                           │
                     frame graph: shadow → scene (+velocity) → TAA → post → UI → present
```

### Ordering

Everything must land in the order the game built it. Model draws already do:
they are intercepted on the VIF1 thread at the chain position of their
`MSCAL`. A hooked effect function doesn't call the original. It records its
command in a side table and appends a VIF `MARK` with the command's index to
the chain, so the VIF1 thread submits it at that position. (Phase 0 confirms
the game itself sends no `MARK`.)

### Models: intercept at the program start

The VIF1 thread already knows, at each `MSCAL 0x64`, which pieces are resident
(the program tags in `vif1_thread.cpp`) and what VU1 data memory holds:
matrices, lights and the unpacked vertex chunk. A ported composition becomes
a `NativeMesh` draw:

- **Inputs:** the vertex chunk as raw quadwords (a storage buffer), the
  matrices and light slots as constants, and the composition id. The vertex
  shader decodes the chunk the way the piece in each slot does. That means one
  shader function per piece and one pipeline per family, with the per-draw
  piece choices as specialization constants.
- **GS state:** texture, blend and test come from the context registers the
  chain's `DIRECT` packets set, as the GS path decodes them today, plus the
  GIF tag template the program would kick (read from fixed VU1 quads; to be
  mapped per family).
- **Outputs, two depths:** while GS-path draws share the target (phases 1-3),
  the shader writes the same integer Z the VU1 program computes, so mixed
  draws depth-test against each other. Once every depth consumer is native
  (phase 4), it switches to reversed float depth. Motion vectors come from the
  previous frame's matrices for the same object.

The alternative altitude is hooking `ModelPrepareDrawMaybe` or
`ModelEmitDrawPacketsMaybe` and reading model resources directly, which
would let static meshes live on the GPU. It is the better end state for CPU
cost, but it needs the model formats understood first. The program start
needs nothing but the VU1 memory layout, which we can capture and verify.
Phase 6 revisits it.

**Two steps per family, so features don't wait for CPU savings:**

1. *Replace the output:* the compiled program still runs, keeping every VU1
   side effect the next program reads (data memory, VI registers for an
   `MSCNT` continuation, the quads the base program stores). Its `XGKICK`
   output is discarded and the native draw goes in its place. This unlocks
   every GPU feature and costs no extra CPU.
2. *Skip the run:* a debug mode runs the program after the native draw and
   diffs the state the next start reads. A family whose state is reproduced
   exactly (or not read) stops running the program. This is where the VIF1
   thread's 35-56% goes away.

### Effects: hook the function, not the sprites

Each effect is an EE function with parameters. The native pass takes those
parameters and the scene's targets by name, never VRAM addresses. Candidates
from the function maps; the GS stage letters are from [GS-PASS-GRAPH.md](GS-PASS-GRAPH.md),
and which function makes which stage is phase 0's first job:

| Function | Likely stage | Native form |
|---|---|---|
| `GlareFilterDrawMaybe_17a0d0`, `BloomExtractBlurMaybe_179360` | A (69 single-draw snapshot passes), C | downsample + separable blur at the buffer's own size |
| `DofFilterDrawMaybe_17d150`, `FocusFilterDrawMaybe_17c000` | D | one or two passes reading native depth |
| `ModelDrawOffscreenCompositeMaybe_16aac0` and its callers | E (47 per-object pass pairs) | unknown until classified; possibly one pass with a stencil or object mask |
| `ShadowMapRender_146680`, `ShadowMapBlur_145b60` | F | a shadow pass at a chosen size; blur or PCF |
| `CBlurDrawMaybe_1784e0`, `RefractionCaptureFrame_13ffd0` | ? | copy plus a sampled pass |
| `LensFlareDraw_2ed830`, `DepthProbeGetSlot_13fc80` (8x8 Z reads in `GfxEndFrame_141c20`) | F | native depth sampled in a small compute pass, read a frame late |
| `ScreenColorFilterMaybe_179c60`, `ScreenFade*`, `ScreenFadeBlockCaptureFrame_177ed0` | | full-screen tint or fade; capture kept on the GPU |
| `GfxFlipBuffersMaybe_143650` (41-strip copy to the display buffer) | | a present from the scene target, no copy |

Each pass ships twice: a **parity** version that matches the GS path at 1x,
which is the default, and later an **enhanced** version (HDR intermediate,
resolution-scaled kernels) behind a menu option.

### Textures without local memory

TEX0 names a texture by base address, so something must still map addresses to
textures. It is an address table, not memory: per page, the latest upload
(decoded straight into a native texture keyed by a content hash, the scheme
`gs_replacement_hash` already uses) or the named target that last drew
there, with the view it was drawn as. CLUT loads resolve the same way. The
reinterpretations the game uses today (CT32 read as T8 for channel shuffles,
CT16 views of 0x3800) belong to effects. Once those effects are native, the
census should show none left; any that remain get an explicit case.

### 2D and UI

GS packets that no hooked function or VU1 program made go to a sprite
batcher. It reuses the GS path's color semantics (0x80 as 1.0, the integer
blend). It tags draws by the task that issued them (HUD, menu, fade) through
the same thread-local source tag the profile cycle uses for VU1 programs.
Tagged UI draws at output resolution, anchored to a 4:3 safe area in
widescreen.

## Phases

Each phase ends with a measurement on the reference scenes and leaves the game
fully playable. `RG_RENDER=gs` keeps the whole GS path selectable until phase
5. `RG_NATIVE=models,post,ui,...` turns the native categories on one at a
time, and a debug key flips them live on the same frame.

**Reference scenes:** Salgin's first scene, the town square (`TOWN=1
walk.sh`), a forced battle (battle bench), the boss gate, an event with an
item message and a battle dissolve (screen captures), the item menu, a movie.

### Phase 0: attribution and census (instrumentation only)

- Tag every draw with its origin: the VU1 composition (all six resident slot
  hashes, not just the entry), or the EE function on whose behalf a `DIRECT`
  packet was built. That means hooks around the effect functions above and the
  task boundaries, setting the source tag. Extend the `[frame]` dump and the
  profile cycle to group by it.
- Log the slot compositions a play session uses, with their draw and vertex
  counts. Map the ledger's heavy tags (`#6bcb`, `#e35a`, `#0970`) to
  compositions.
- Census, per frame: texture sources (upload, target, reinterpreted target,
  Z buffer as texture), readbacks by reason, CLUT sources, any `MARK` the game
  sends.
- Open questions to settle here: what stage E's per-object offscreen pairs
  are for; whether a depth-probe read through `depth_reads.cpp` sees the depth
  the GPU drew (the GS backend never reads depth back, so local memory may hold
  a stale Z); what the screen overlay sprite (`ScreenTexOverlayWriteSpriteMaybe_16a890`,
  an `MSCAL` into slot B) relies on in VU1 memory.

**Done when** every draw and texture read in the reference scenes is
attributed. The output is a table in this document of families, functions and
cases, which becomes the work list for phases 1-4.

### Phase 1: native model geometry (output replaced)

- `NativeDrawSink`, a narrow interface on the SDL GPU backend that accepts
  `NativeMesh` and `NativePass` commands into the same target cache and pass
  queue as GS draws. Game-specific code lives in `src/render/`; the backend
  only gains the sink.
- Port families in order of cost from the ledger. Per family: a C++ reference
  decoder plus the shader. An offline harness feeds both the records in
  `build/vu1.cap` and the compiled program, then compares output vertices
  (XYZ in 12.4, Z, STQ, RGBA) within a tolerance for VU float rounding. GPU
  output is read back through a compute pass in the harness.
- Image check at 1x against `RG_RENDER=gs` on the reference frames, with a
  per-pixel tolerance.

**Done when** the families covering at least 95% of VU1 draw starts in the
reference scenes draw natively, with no visible difference at 1x and
measured GPU power within noise of the GS path.

### Phase 2: motion vectors and temporal upscaling

Phase 2 builds on the uncommitted `RG_JITTER` Halton offsets and the
`CameraRegistry` from `0020-gs-source-camera.patch`.

- Object identity across frames: the model matrix's EE source address plus
  the mesh shape, as `capture_report.py` matches them. Keep last frame's
  matrices per identity.
- A velocity target written by native meshes. Non-native draws get
  camera-only motion or are masked from history.
- A TAA / temporal upscale pass at the end of the scene, before post. The
  "scene done" point is the task boundary before the first post task, found in
  phase 0. 3D renders below output resolution, and the existing "internal
  resolution" setting becomes "render scale".

**Done when** TAA is on in the reference scenes with no ghosting on
characters or the camera, as an option in the F1 menu.

### Phase 3: native post and offscreen effects

Port the effect table one function at a time, parity first. Each hook records
a `NativePass` and a `MARK`, and skips the original. Anything the original does
besides drawing (timers, work-texture registration, the `GfxEndFrame` probe
bookkeeping) is reproduced in the hook. The diff of the function's
non-graphics side effects is part of the review.

**Done when** Salgin's first scene runs in at most about 25 render passes
with no snapshot copies or destination copies, and the GPU power measured
with `gpubusy.sh` falls by an amount recorded in the ledger.

### Phase 4: textures and 2D without local memory

- Uploads decode straight into native textures through the address table.
  Replacement packs load by the same key.
- The 2D batcher replaces the GS translator for every remaining packet. UI
  draws at output resolution, with the widescreen layout fix.
- Depth switches to reversed float once phase 0's census confirms that no
  GS-path draw tests against model depth any more.

**Done when** `gs_vram` is no longer allocated in a native run, and a
playthrough route (title, Salgin, the first battles, a boss, menus, a save
and a load, a movie) passes with the frame tools.

### Phase 5: remove the GS path

Delete local memory, page ownership, imports and patches, snapshots,
destination copies, round trips and readbacks from the backend, plus the
`DQ8_GFX_*` switches that exist only for them. Keep `RG_RENDER=gs` for one
release as a fallback, then remove it. The backend's device, window,
present, display filters and menu stay.

### Phase 6: further work (optional, each behind an option)

- Static meshes cached on the GPU, through the model-resource altitude.
- Per-pixel lighting from the same light slots.
- MSAA on the scene, anisotropic filtering, higher-resolution shadows.
- An HDR intermediate for bloom.

## Sizes and order

Relative sizes from the shape of the work, not estimates in days. Phase 1 is
the long pole and needs phase 0's family count.

| Phase | Size | Can start |
|---|---|---|
| 0 Attribution and census | S-M | now |
| 1 Native meshes | L (about ten families, each a decoder, a shader and harness cases) | after 0 |
| 2 TAA | M | after the first ported family |
| 3 Post | M-L (about ten effects, each S-M) | after 0, beside 1 |
| 4 Textures and 2D | M | after 3 |
| 5 Removal | S | after 4 |

Phases 1 and 3 run in parallel; 2 can ship as soon as the heaviest families
are native.

## Risks

- **VU1 side effects.** The next program can depend on what a program leaves
  behind, through data memory, VI registers across `MSCNT`, or the screen
  overlay sprite's jump into slot B. The output-first step and the
  state-diff mode exist for this. A family skips its run only once the diff is
  clean.
- **Coverage tail.** The table allows 785 compositions; the manifest shows far
  fewer in use. Unported compositions keep running the compiled program
  through the GS translator, so the tail costs speed, not correctness, until
  phase 4 needs it gone.
- **Mixed depth.** Until phase 4, native and GS-path draws share an integer
  Z. Any mismatch in the Z formula shows as z-fighting on decals and effects
  drawn over native models; the harness compares Z exactly.
- **Effect side effects.** A skipped original can leave game state behind
  (a fade's timer, a probe slot). Each hook's review lists what the original
  touched besides the chain.
- **A permanent fork of DQ8's backend.** Upstream fixes stop applying once
  phase 5 deletes the parts they touch. That is accepted: the GS parts are
  what goes.
- **Upscalers that need native API handles** (DLSS, XeSS) are out of scope
  while SDL GPU is the API. Temporal upscaling is our own pass.

## Open

- The families: their count, vertex formats, GIF tag templates, and which do
  lighting, skinning or clipping (phase 0, then the harness).
- What stage E's per-object pairs are.
- Whether the lens flare's occlusion is right today (the depth-probe question
  above).
- The 2D layer's end state: keep translating `GsPkt` packets, or hook the
  `GsPkt*` functions for direct native calls. The translator is enough for
  phase 5; direct calls are a later option.
