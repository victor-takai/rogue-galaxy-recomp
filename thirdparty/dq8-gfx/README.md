# Hardware GS backend from DQ8-Recompiled

The SDL3 GPU implementation of PS2Recomp's GS raster backend
(`GSRasterBackend`), taken from
[DQ8-Recompiled](https://github.com/Sinan-Karakaya/DQ8-Recompiled) by Sinan
Karakaya, GPL-3.0 (see `LICENSE`), at commit
`3e74da68d7633db9996457dca4a9388ece0847c0` (2026-10-11). DQ8-Recompiled
builds on the same PS2Recomp fork this project pins.

| Path | Origin |
|---|---|
| `src/gfx/gs/`, `src/gfx/backends/sdlgpu/` | `src/gfx/gs/`, `src/gfx/backends/sdlgpu/`, with the changes below |
| `src/ui/` | `src/ui/`, the in-game menu (Dear ImGui), with the changes below |
| `cmake/Dq8Shaders.cmake`, `Dq8EmbedSpirv.cmake`, `Dq8EmbedText.cmake`, `Dq8EmbedBinary.cmake`, `Dq8HostSimd.cmake` | `cmake/`, unchanged |
| `CMakeLists.txt` | written for this project (DQ8's builds the same targets inside its own tree) |
| `window_icon.cpp` | replaces `sdlgpu_window_icon.cpp`, which embeds DQ8's icon; sets none |

Not taken: DQ8's GS dump tools (`src/gfx/gsdump`) and anything
game-specific. Dear ImGui is the submodule at `../imgui`, the commit DQ8 pins.

## Changes to the menu

The menu is DQ8's with its name taken off: the project name, About text,
links and screenshot prefix are this project's, the crown icon is a disc,
`RG_SETTINGS_FILE` and `RG_UI_SCRIPT` replace `DQ8_SETTINGS_FILE` and
`DQ8_UI_SCRIPT`, and the settings file is `settings.ini` under SDL's
preferences folder for `RogueGalaxy/Recompiled`. Its defaults differ where
this game does: internal resolution 2x, mailbox presentation (vsync holds
SDL's submission lock while the GS worker waits on it) and the line blend
left as the game draws it. The host side (pause, speed, volume, the frame
count the counter uses) is in `src/graphics.cpp`. The Game page lists the
game's own options (`HostServices::gameOptions`, kept under `[game]`, in
the sections `HostServices::gameSections` names):
switches, and numbers shown as sliders (a `GameOption` whose maximum is
above its minimum, `HostServices::setGameNumber`) between - and + buttons
that move them by the option's step, with a Default button. A switch that
is not `remembered` stays out of the file and shows what
`HostServices::gameOptionState` says; `Overlay::setGameOptionValue`,
`setGameNumberValue` and `setSides` take a change made outside the menu
(the game's own Settings screen) and keep it as the menu's own. A `restart` switch changed
since the run started says in red that it waits for the next start. F6,
the game's free camera, cannot be bound to a pad input, as the menu's own
keys cannot, and `Overlay::capturingBinding` tells the host when the menu
is waiting for a key to bind. The window opens with its last size, place
and maximized state (`[window]` `x`, `y`, `maximized`;
`SdlGpuWindowPlacement`). Display > Borders fills the window beside a
picture that leaves it uncovered (`SdlGpuSides`, `sides =` under
`[display]`): black, the picture blurred and at three quarters of its
brightness (halved by bilinear blits to 256 texels wide, then nine taps in
`present_sides.frag`), or an
image the host loads (`SdlGpuBackend::setSideImageLoader`), each scaled to
cover the window and drawn before the picture, which then loads instead
of clearing; the script's `sides black|blurred|image` sets it.

## Changes for Rogue Galaxy

Rogue Galaxy uploads mostly 4- and 8-bit textures every frame and resolves
render targets about ten times a frame, both far more than DQ8; these
changes speed those paths up without changing their results. The first
commit of this folder is upstream as is, so `git log -p` on it shows them.

- `gs/gs_vram`: `writeRunT8`, `writeRunT4`, `writeRunCT32`, `writeRunZ32`
  and `readRowT4`, built on the same `PixelStorageTraits` and layout tables
  as GSMem, so addresses match (the game's `RG_SELFTEST=gs` checks them
  against GSMem). The writers use the layout in closed form (a texel's
  piece, byte and nibble inside its 256-byte block are a fixed shuffle of
  x with a per-row flip; a `static_assert` checks the formulas against the
  column tables) and write whole 64-byte columns at a time with SSE where a
  run covers them: four rows of a PSMT4 texture at once, no read-modify-
  write. Whole-texture uploads went from about 1.8 to 0.05 ns a texel for
  PSMT4 and from 0.53 to 0.05 for PSMT8; CT32 and Z32 2.5x. Only rows
  narrower than one group fall back to texel-by-texel writes.
  `-DDQ8_GS_VRAM_SCALAR` builds the portable kernels, same bytes.
- `gs/gs_transfer.cpp`: host-to-local PSMT8, PSMT4, PSMCT32 and PSMZ32
  transfers write runs instead of one dispatched `write()` per pixel.
- `sdlgpu_textures.cpp`: PSMT4 textures expand through the palette
  `buildPalette` already resolved; the general path re-read every texel's
  CLUT entry from local memory.
- `sdlgpu_targets`: resolves reuse one download transfer buffer and one
  downscale staging texture instead of creating and releasing them each
  time (they wait for their fence, so both are idle again by the next).
- `sdlgpu_backend.cpp`: the feedback snapshot's tail upload reuses one
  transfer buffer mapped with cycling; the FPS readout keeps the window's
  own title in front instead of "DQ8Recomp".
- `sdlgpu_backend.cpp`, `sdlgpu_textures`: a host-to-local transfer no
  longer flushes the queued draws unless they touch its pages (draw into
  them or into a surface they belong to, or sample from them). Rogue Galaxy
  uploads about 350 textures a frame between its draws, and each flush is a
  command buffer submission. The textures queued draws hold were expanded
  when they were queued, and the texture cache now tells the backend before
  it rewrites or releases a texture, so a draw still holding it is flushed
  first. `DQ8_GFX_FLUSH_ON_TRANSFER=1` restores the flush at every transfer.
- `sdlgpu_backend.cpp`, `sdlgpu_targets`: a sprite that replaces every
  pixel of its colour surface (a clear) takes the surface over
  (`GsTargetCache::claimWhole`) instead of importing the pages aliased views
  own and the pending content of local memory, which it would overwrite
  anyway. Rogue Galaxy clears its per-object offscreen buffer before each
  object, and those buffers alias each other and the pages its textures go
  to. `DQ8_GFX_NO_CLAIM=1` imports as before.
- `sdlgpu_backend.cpp`: a textured primitive counts only the texture pages
  it can sample (its UV rectangle, when the coordinates are plain) against
  the queued draws, not the texture's whole 2^TW x 2^TH footprint, which is
  512 lines over the game's 448-line buffers and made every strip of its
  frame copies flush the other targets' queued draws. And a draw that
  ignores depth keeps the depth attachment of the draws around it when
  their depth surface already exists at the size it needs (tested ALWAYS,
  not written), so the clear of an object's offscreen buffer and the object
  share one render pass (`DQ8_GFX_NO_PASSIVE_DEPTH=1` for the old
  behaviour).
- Render-target readbacks (a resolve: the target downloaded into local
  memory, with a fence wait that drains the GPU) are counted by reason in
  the `DQ8_GFX_STATS_EVERY` report and named in `DQ8_GFX_TRACE_TARGETS`,
  which `gsSetTargetTrace` can now switch on for a window of frames (the
  game's `RG_GS_TRACE_BEFORE`). Rogue Galaxy provoked about eight a frame
  in the field, a quarter of the worker's time; these take them away:
  - `gs/gs_vram.cpp`: `gsMarkPages` is exact for a block base inside a
    page, through the format's block table, instead of marking the next
    page for every cell. The game writes its palettes at block offsets
    0x1c..0x1f, and the page after each held a render target.
  - `sdlgpu_targets`, `sdlgpu_backend.cpp`: host writes in CT16, T8 and T4
    patch a live native CT32 target word by word, as CT32 ones did
    (`patchHostWrite` with the format's block layout; a word is counted
    when every one of its bytes, or nibbles, has arrived, across payloads;
    whole blocks of a page go out as runs of 8x8 cells along each cell
    row, since the formats' block order scatters them; the owners of each
    page are found once per write, not once per cell), instead of reading
    back the pages they cover in part. Local copies into such a target
    patch it too, and read back only the source pages whose words local
    memory does not hold. `DQ8_GFX_PATCH_CT32_ONLY=1` keeps the patching to
    CT32. With ownership kept, a target's queued patches grow (every
    texture uploaded into its pages), so what needs only part of it
    current brings up only that part (`refreshRegion`): the GPU expansion
    of an 8-bit texture from a target, and the snapshot padding below.
  - `sdlgpu_textures.cpp`, `sdlgpu_targets`: a T8, T4 or CT16 texture read
    asks for the words it occupies (`markReadPages`), so a texture uploaded
    into pages a target still owns needs no readback; and the pages of a
    target that a draw reaches no texel of (the UV rectangle the backend
    already bounds) are left to it, their texels marked unresolved until a
    draw reaches them. The game declares a 512x512 texture over the 512x416
    it uploads, and the lines below lie in a live target.
  - `sdlgpu_backend.cpp`: a feedback snapshot's padding row (the texel row
    under the surface a bilinear tap at its edge reaches) comes from the
    texture of the target owning that row: copied for a CT32 surface, and
    for a CT16 view made by the reinterpret shader
    (`SdlGpuDevice::recordReinterpret`) from the owner's page row copied
    into a scratch. `DQ8_GFX_NO_GPU_PADDING=1` reads it from local memory
    as before.
- `sdlgpu_device`, and every place that acquired a command buffer: a
  texture upload, a target's patches, a page import, an 8-bit expansion
  from a target, a reinterpretation, a local copy and a clear were each a
  command buffer submission of their own, and each flush of the queued
  draws another. Rogue Galaxy made about 150 submissions a frame in the
  field (it flushes about 70 times a frame: feedback snapshots, transfers
  into pages the queued draws touch, aliased views), and the driver and
  kernel were near half of the GS worker. They are recorded into one
  pending command buffer instead (`SdlGpuDevice::pendingCommands`), in
  the order they happen, which is the order the GPU runs them, as with
  immediate submissions. A flush records the queued draws after them and
  submits once `DQ8_GFX_SUBMIT_DRAWS` draws (96) are in it; a readback, a
  present or the budget for resources SDL cycles per use (local copies,
  and feedback snapshots until they were copied in place, below; 96 MiB)
  sends it sooner. The vertices of
  one submission's flushes append to one buffer instead of cycling it
  every flush, and the upload transfer buffers (textures, patches,
  vertices) come from pools guarded by submission serials
  (`SdlGpuUploadPool`), not per-submission fences or SDL cycling, which
  keeps a copy of a buffer's whole capacity per upload in flight. The
  field's frames go out in 4 to 5 submissions; the stats line shows
  `flushes=` and `submits=`. `DQ8_GFX_SUBMIT_DRAWS=0` submits at every
  flush, as before.

- `sdlgpu_backend.cpp`: a primitive whose draw state is byte-identical to
  the one before it, when that one went through the texture-cache path
  (or untextured) and nothing has happened since, skips the full submit
  path: no target lookup, view preparation, page scans, register decoding
  or texture-cache key lookup, only its bounds, its written pages, the
  texture-cache call for the texels it reaches (which expands the texture
  when it must; anything that would flush or rebuild takes the full path)
  and the append to the open batch (`Impl::submitSameState`). The frontend
  hands over one primitive at a time and an object is thousands of them
  under one state: in Rogue Galaxy's streets 78% of the primitives take
  it (the stats line's `same-state primitives`), and the walk went from
  54.5 to 56.2 frames a second at 3x in the 60 fps mode. A second pass
  keeps the texels acquired under the state so far and skips the cache
  call for a primitive within them whose texture pages no queued draw
  writes, and marks a batch's pages by its bounding box (a superset of the
  full path's union of each primitive's pages), so a primitive inside the
  box marks nothing: the worker's share of the walk went from 96% to 90%
  of wall at the same frame rate, byte-identical frame at 1x.
- `sdlgpu_targets.cpp`: a page import between two targets copies
  consecutive pages along a row of both as one `SDL_CopyGPUTextureToTexture`
  instead of one per page, each with SDL's layout transitions around it:
  56.2 to 57.9 frames a second on the same walk, byte-identical frame at 1x.

- `sdlgpu_textures.cpp`: `GsTextureCache::invalidate` walked every entry
  (up to 2048) for every flush and every host write; it keeps the union
  of all entries' source pages and returns at once when the invalidated
  pages touch none. Byte-identical frame; within the walk's noise in
  frames a second.
- `sdlgpu_backend.cpp`: the stats line counts why the queued draws were
  flushed (`flushes: settle= transfer= snapshot= alias= texread=
  feedback= localcopy= present= rewrite=`). In Rogue Galaxy's streets the
  83 flushes a frame are 46 texture reads of pages queued draws write, 17
  flushes after a recorded feedback draw, 9 transfers into queued pages
  and 8 draws into a view aliasing queued pages: the render pass count
  (about 230 a frame) follows from these.

- `sdlgpu_targets.cpp`: a CPU patch region of 1024 GS pixels or more on
  an upscaled surface (page imports, refreshes) is read out of local
  memory at native size, uploaded to the resolve staging texture and
  blitted into the surface with nearest filtering (`patchNative`), instead
  of being expanded scale x scale on the CPU into the write-combined
  upload buffer: 24 MB a frame at 3x in Rogue Galaxy's streets. The 8x8
  cells of texture uploads keep the CPU path, where a copy pass and a blit
  each would cost more. The worker's share of the 3x walk went from 93% to
  84% of wall; byte-identical frame at 3x in the 30 fps mode.

- `sdlgpu_backend.cpp`, `sdlgpu_targets.h`: a draw sampling a live target
  that is behind only on host writes patched into it lands those patches
  (`GsTargetCache::settlePatches`) and samples the target, instead of
  flushing the queued draws and rebuilding a texture from local memory.
  The order holds: the patches record before the queued draws, which the
  transfer-conflict rule kept off their pages. The long walk's pacing
  window went from 54.6 to 56.5 frames a second at 3x in the 60 fps mode;
  byte-identical frame at 3x in the 30 fps mode. The texture-read flush
  count itself did not move, so the streets' 46 a frame have another
  cause (the trace line now prints the candidate's state).

- `sdlgpu_backend.cpp`: the pages a textured primitive counts against
  queued draws come from its sampled texel rectangle, which carries the
  bilinear half texel past the last row; for a texture the size of a
  buffer that slack lands in the next buffer's pages. Rogue Galaxy copies
  the frame buffer into the back buffer in 41 strips a frame, and reads
  one object buffer while the next is drawn, so every strip met queued
  draws in those pages and flushed. A draw that samples within a live
  target now counts only that target's pages: the texture-read flushes
  went from 47 to 3 a frame, all flushes from 83 to 40, render passes from
  230 to 180, the long walk's pacing window from 56.5 to 58.3 frames a
  second at 3x; byte-identical frames at 1x and 3x in the 30 fps mode.
- `sdlgpu_targets.cpp`, `sdlgpu_backend.cpp`: an upload that replaces
  whole pages a live colour target owns releases those pages to local
  memory (`GsTargetCache::releaseCoveredPages`: not GPU-owned any more,
  imported again before the target's next use) instead of recording a
  patch per block. Rogue Galaxy streams its textures through the pages its
  scene buffer occupied a frame earlier, 400 uploads a frame, and clears
  that buffer whole before drawing it again, so the patches were recorded
  for nothing. The worker's share of the wall in the long street walk went
  from 85% to 64% and the page refreshes halved (2800 to 1300 per 60
  frames, 40 to 16 Mpx). The logged frame's display buffer is identical
  with and without it; the VRAM differs by 915 bytes at 0x2A0000, where a
  128x128 CT32 upload is read back as a 256x256 T8 texture, and those
  bytes are the patched path's own: with `DQ8_GFX_PATCH_CT32_ONLY=1`,
  which reads a target back before any non-CT32 write instead of patching
  it, the VRAM is byte-identical to the released path's. So the block
  patch of a non-CT32 write leaves byte 2 of some words of the page's
  first block row (blocks 0, 2, 8 and 10) stale, which is still to be
  found; it now only matters for a page a write covers in part.
  `DQ8_GFX_NO_COVERED_RELEASE=1` patches every page, as before. The stats
  line counts `covered-releases` and their pages.

One change fixes a result rather than speed: in `sdlgpu_backend.cpp`, a
scissor ending past the buffer (0 to 512 with FBW 8, in Rogue Galaxy's
menus) no longer grows the colour and depth surfaces past FBW pages unless
the geometry reaches there (a clear never does), and a draw's host scissor
and written region are clipped to its surface. Grown surfaces took in the page after the
buffer; texture uploads to it then discarded the depth buffer mid-frame.

Five more fix results:

- `sdlgpu_backend.cpp`: a frame read back to the host and sent back
  unchanged keeps its scale. Rogue Galaxy's screen captures
  (ScreenFadeCaptureFrame: the frozen frame of an event under an item's
  message, the dissolves into and out of battle) read the frame into EE
  memory at GS size and upload it every frame, so at 3x the picture went
  to 1x for as long. A local-to-host read of a scaled CT32 region now keeps
  the region on the GPU and an FNV-1a hash of the bytes the host got; a
  host-to-local upload of the same size and format whose bytes hash the
  same, onto a whole surface of that size at that scale, gets the kept copy
  (`claimWhole`, a blit, `markDrawn`). Bytes the host changed (the field
  menu's darkened 256x256 copy) land as before. The stats line counts
  `round trips`; `DQ8_GFX_NO_ROUND_TRIP=1` leaves them at 1x.
- `sdlgpu_backend.cpp`, `sdlgpu_window.h`: `SdlGpuDisplayOptions::crop`
  leaves GS pixels off each edge of the picture (and half a pixel more at a
  raised scale), before the Sharp filter's passes. Rogue Galaxy leaves a
  border of its frame black or half drawn, which a television's overscan
  hid; its half-pixel effects move it by a texel at 3x and leave a sliver
  of picture past it.
- `sdlgpu_display.cpp`: under Auto a game drawn wide fills the window's
  shape (`sdlGpuWideAspect`, 4:3 to 32:9) instead of 16:9.
- `shaders/gs_draw.frag`: FBA without framebuffer fetch. Only the fetch
  path (Apple's GPUs) set the stored alpha's top bit; the fixed-function
  path stored the draw's alpha as it was. Rogue Galaxy draws a weapon
  into a layer (the Monography Shot: texture alpha 0, FBA on), then copies
  over the frame only the layer's pixels whose alpha is set, after two
  soft-edge passes at about half alpha; the gun got only those and showed
  the scene through it. An exact emitted alpha now gets bit 7 under FBA
  (not on CT24); a scaled one is a source-alpha blend factor, and that
  draw's alpha replay stores it.
- `sdlgpu_backend.cpp`, `sdlgpu_device.cpp`, `shaders/gs_draw.frag`:
  destination reads without framebuffer fetch. A draw with a destination
  alpha test, a factor of destination alpha (/128 on the GS, /255 in a
  fixed-function blend) or a factor past one (FIX, or vertex alpha where
  the texture brings none, above 0x80) uses the `DQ8_DESTINATION_COPY`
  build of the draw shader: a snapshot of its target, of the primitive's
  region, taken before each primitive and bound as a second texture, and
  the fetch path's integer blend, test and masks on it. Rogue Galaxy draws
  one to four a frame: the full-screen test that clears its depth of
  field's mask where it is unset (the fixed path cleared it all), the sun
  flare's visibility turned from alpha into colour in a 64x64 buffer
  (half as bright), and additive sparkles and light shafts at alpha up
  to 0xB4 (clamped at 0x80). The stats line counts `destination reads`;
  `DQ8_GFX_NO_DESTINATION_COPY=1` keeps the approximations.
- `sdlgpu_backend.cpp`: a snapshot copy overwrites its texture in place
  instead of cycling it. Cycled, each copy recorded ahead of a submission
  kept storage of its own, a whole texture of the surface's size, until
  the GPU was done with it; Salgin's streets take about 110 a frame (the
  strips of a blur, channel shuffles), and at 8x they outgrew a 12 GB
  GPU: its memory went from 2.7 to 11.4 GB as the town loaded, the frames
  paged for up to a second and a half, and SDL binding new memory was a
  quarter of the worker. In place, the GPU runs a copy after the draws
  recorded before it (SDL puts a barrier between), the ones that sampled
  the last copy among them, and the draw that samples it is recorded
  right after, as before; the copies no longer count against the pending
  budget. The town bench at 8x went from 29.9 to 57.7 frames a second
  (GPU memory 2.3 GB), a battle at 8x from a median of 48.3 to 57.1 (the
  lowest interval 26 to 50), the town at 3x from 55.0 to 58.4 (the worker
  82% to 66%); the logged frame is byte-identical at 1x and 3x.
  `DQ8_GFX_CYCLE_SNAPSHOTS=1` cycles them as before.
- `sdlgpu_backend.cpp`: the presenting submissions go out under the
  backend lock, after the pending command buffer (`submitAfterPending`).
  SDL's Vulkan backend (3.2) defragments at a submission that presents,
  or at any without a window, once some allocation needed a new memory
  block: it copies every resource of one allocation to new memory within
  that submission and points the handles at the copies. Commands the GS
  thread had recorded and not yet submitted still named the old images,
  so their writes landed there and the copies kept what came before. A
  texture pack under test (Rogue Galaxy's `texture-pack` branch) creates
  about 330 textures as the item menu opens, each uploaded once: in about
  one run in twenty, digits came out missing ("15" as "1", the tabs'
  zeros) or as noise ("28"), with the same draws as a good run's. The
  window thread's present waits for the lock no longer than one GS call;
  the town at 8x showed no difference beyond noise.
- `sdlgpu_regions.h`, `sdlgpu_backend.cpp`, `sdlgpu_textures`: the host's
  larger versions of the 2D art a UV sprite (or a triangle, half of a
  quad) samples, for Rogue Galaxy's traced letters. A whole texture's
  identity is no name for its glyphs: the font sheet declares 512x512
  over rows of other data that change with the scene. The rectangle a
  primitive samples is hashed as a whole texture is
  (`GsTextureCache::regionIdentity`: the indices and the palette window);
  the host says which palettes it wants (the rest are left before their
  texels are hashed), is shown each new rectangle of those with its RGBA,
  and answers with a larger picture, or "pending" while it makes one
  (the region is drawn as it is meanwhile). That picture is a texture of
  its own, kept by content (`GsTextureCache::replacement`; the least
  recently drawn go past 512 MB), drawn bilinear at its own texels per GS
  texel, the UVs moved to the rectangle's corner. A sprite's UVs often span
  a texel more than its pixels, and that last row and column, never shown,
  hold the next piece of art or a descender's end: the host is handed them
  transparent, and the picture is drawn whole. Clamped to the shown texels
  instead (REGION_CLAMP), the last shown row came out again below it, the
  shader keeping the position inside a clamped GS texel. Only
  into the frame (the buffer last shown, or the target last copied into
  it), from indexed textures, above 1x. Borders: a glyph's 4, 8 or 12
  black or dark grey copies around it, marked by the host with their y a
  sixteenth of a pixel up (three when the first is diagonal), are drawn as
  one, where the glyph goes, from the host's border version (the outline
  grown round, with a margin about the region), the others left out;
  without one, all as they are.
  Shadows: a copy marked two sixteenths up, of any colour, is drawn where
  it goes from the host's shadow version (the glyph swept back toward the
  one before it, the same margin), so copies a pixel apart join as the
  GS's blocks do. The host is told each region's place in its texture,
  and a region a texel past its texture's edge (the texel a sprite's UVs
  span past its pixels) is cut to it. The stats line counts `larger
  regions`, `borders` and `shadows`. First written for
  a pack of upscaled pictures (the `texture-pack` branch), from which
  the frame-target test and the clamp come.

Native strips (Rogue Galaxy: added). `SdlGpuBackend::SubmitNativeMesh`
takes a `GSNativeMesh` (a strip a VU1 program would have drawn, as its
inputs; patch 0021) through `submit()` as a stand-in primitive covering the
scissor and sampling the whole texture, with the strip's alpha, so the
target, texture, flush and page decisions are those of any primitive there;
where the stand-in's vertices would go, the strip's go instead, in
`GsGpuVertex` slots read another way (`appendNative`), drawn by
`shaders/gs_native.vert`: the program's transform, its 12.4 and whole-Z
fixed point, fog and color clamp, then the same outputs as `gs_draw.vert`,
with the position homogeneous so the GPU clips where the program dropped
triangles at its guard band or clipped them in software. The strip goes as
an indexed triangle list, in the order the GPU's strip would draw it, and
its transform (matrix, fog, color cap, depth) as a `GsNativeMeshRecord` in a
storage buffer that each vertex names (its `t`), so consecutive meshes of
one state share a draw call like any primitives. Backfaces are culled by the
pipeline (`GsPipelineKey::cullMode`); the triangles alternate winding as the
program's sign does, and a mesh whose sign starts negative has each
triangle turned over, so all cull the same face. A draw that reads its
target per primitive (a snapshot, feedback, a destination read) gets the
triangles expanded on the CPU instead. The stats line counts `native
strips`, and the draws they took. Salgin, 1x: 57 of 229,376 pixels differ
by more than 8 levels from the VU1's frame.

Effect buffers at a lower scale (Rogue Galaxy: added). Rogue Galaxy blurs
in two 192x192 buffers, 0x2C40 (a zoom streak: twenty passes a frame, each
adding a zoomed copy of the buffer to itself) and 0x3800 (a glow: six); at
8x each is 1536x1536, larger than the PS2's frame. `DQ8_GFX_EFFECT_SCALE=N`
(2 unless set; 0 for the scene's scale) makes colour surfaces at
`DQ8_GFX_EFFECT_BASES` (hex, `2c40,3800` unless set), at most 256 pixels
wide, at scale N (`GsTargetCache::setEffectBuffers`). They share pages with
surfaces at the scene's scale (0x2C40 lies inside the per-object buffer at
0x2A00), so pages move between scales GPU-side: `importColorPages` and a
feedback snapshot's padding blit them (nearest) where the scales differ,
instead of resolving to local memory. Field walk at 3x, against the
scene's scale: 2,426 of 229,376 pixels differ, 16 by more than 8 levels.

GPU time by label (Rogue Galaxy: added). `DQ8_GFX_GPU_TIMES=1` labels the
backend's GPU work (each batch by target, 2D or 3D, mesh, feedback,
textured, blended, covering half its target; the snapshot copies; the draw
data upload; the present; everything else as "other") with
`SDL_InsertGPUDebugLabel`, and SDL, patched (`cmake/patches/sdl3-gpu-
timestamps.patch`, Vulkan only), writes a GPU timestamp at each label. When
a command buffer is done the time from each label to the next is added to
that label's total; every two seconds `[gputime]` lines give each label's
GPU milliseconds and count per second, heaviest first. The timestamps cost
about a tenth of the frame at 8x; off, nothing changes.

The target trace numbers the batches from each present (`[target] batch N`),
as a frame log numbers them. `DQ8_GFX_HIDDEN_WINDOW=1` creates the window
hidden and never raises it, for scripted runs that should leave the desktop
alone; the GS work is the same and `DQ8_GFX_SCREENSHOT_DIR` still writes
frames.
