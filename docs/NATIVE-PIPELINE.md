# The native pipeline: no GS when the flag is on

## The goal

With the native renderer flag on, the GS does not exist as a concept on the
renderer's side. There is no GS local memory, no GS registers, no GS
addresses or pages, no GIF packets, and no `GSRasterBackend`. The renderer
gets scene data it understands: geometry with its transforms, cameras,
textures and render targets by id, materials as plain enums, and effects
described by what they are for. That pipeline is the foundation for:

- temporal upscaling (DLSS, FSR, a TAA of our own): depth, motion vectors
  and a jittered camera
- modern shadows: a shadow pass from the scene's geometry and lights
- ray tracing and path tracing: world-space geometry, materials, and
  eventually normals
- newer effects: a post chain that reads the scene's targets by name

With the flag off, the GS path stays as it is, as the fallback, until the
native path draws everything right.

## Where the line is

The game is a PS2 game. Its code builds GS packets, names textures by GS
address, and sets display registers. That doesn't change, because we don't
rewrite the game's logic. The line is drawn here:

- **Producers** (game-side code in `src/`, on the game and VIF1 threads)
  may read the game's GS-shaped data in order to understand it: hooks on
  game functions, the VU1 program starts, and the packets the VIF1 thread
  sends. Producers own the tables that turn the game's addresses into ids
  (`src/native_producer.h`).
- **The renderer** (`thirdparty/dq8-gfx/src/gfx/native`, the library
  `dq8_gfx_native`) takes only native records. It never sees a GS address,
  register value, page or packet, and keeps no GS memory. It links SDL3
  alone: a GS header is not on its include path, so this is checked by the
  build.

## The records

All records go through one ordered queue (`native::push`), in the order the
game built them. Every reference is an id (`native_records.h`):

- **Texture** and **Palette**: decoded from the game's own bytes (CT32,
  CT16, T8, T4, the pre-swizzled 8-bit art; palettes in CSM1 order), named
  by content. The renderer expands indexed textures in a shader.
- **Target**: the render targets the game names, by role (scene colour,
  display, offscreen). The renderer picks each target's resolution.
- **Camera**: view, projection and viewport from VU1 data memory at the
  program start (quads 4-11 and 24-27), XYOFFSET folded in.
- **Mesh draw**: a target, a camera, the model matrix, object-space
  vertices and indices, and a material.
- **Material**: texture and palette ids, sampler, blend (an enum of the
  modes the game uses), alpha test, depth test and write, cull, fog.
- **Present**: which target the flip shows.

## What is in so far

- **The renderer** (`native_pipeline.cpp`, `native_meshes.cpp`): keeps
  targets, cameras, textures and palettes by id, and draws mesh draws. The
  sdlgpu backend hosts it on its device and, under the pipeline, presents
  its scene instead of what the GS's display registers name.
- **Textures, palettes and targets** (`src/native_capture.cpp`): hooks on
  `TexBuildUploadPkt` (each palette and mip level, from where the node keeps
  it), `DrawEnvSetFrameFromDesc` and the frame's and display's descriptor
  getters.
- **VU1 meshes as mesh draws**: each mesh a native VU1 program builds
  (`RG_VU1_NATIVE=gpu`, on with the native renderer) is made a mesh draw
  with its camera and model split, under the material of the GS registers
  of that moment (`src/draw_state_mirror.h`: it follows every packet the
  VIF1 thread sends, and reads the registers back after a libgraph stub
  writes them, patch 0031). A mesh made a draw no longer goes to the GS.
- **Present**: the flip pushes a `Present` of the scene target.
- **Effects as sprite draws** (step 5, `src/native_effects.h`): hooks note
  which packet memory each effect function wrote (glare, plane shadow,
  CBlur and CBlur draw, bloom, depth of field, the refraction's capture,
  focus filter, shadow-map blur, colour filter, and the flip's copy into
  the display buffer). The VIF1 thread holds those packets, builds a
  `GSNativeEffect` from their sprites (`src/native_effect.h`: each sprite
  as the GS would draw it, its strips joined), and
  `rg::nativeproducer::effect` makes each op a window-space mesh draw
  between targets, all or nothing per effect. Made, the packets go on with
  their kicks withheld (`XYZ2` as `XYZ3`), so the GS and the draw-state
  mirror end with the registers they would have had. An effect the builder
  or the producer refuses goes on as packets, and the log says why once.
  `RG_NATIVE_<EFFECT>=0` turns one off; `RG_NATIVE_EFFECTS_LOG=1` prints the
  first of each built.

Under the pipeline the screen shows the 3D meshes and the effects. 2D (the
HUD, the menus, text), the per-object composites, local copies and the
game's colour and depth reads still go to the GS, which keeps running
unseen. Known from the archive branch's runs: most of what the effects'
offscreen buffers hold comes from 2D pieces (their clears among them), so
until 2D comes the effects can read buffers the renderer never filled.
Those, the composites as object layers, the pipeline's own present, and a
check that nothing reaches the GS any more, come next.

Memory: the renderer keeps the cameras the producer can still name (the
newest `2 * kCameraWindow`); textures are evicted over a byte budget and
sent again. Palettes are kept for the session: they are few and small
(one per distinct palette seen, 1 KiB at most), and the producer cannot
send one again once the game's copy is gone, so freeing them needs the
producer to keep its colours first.

## Running it

`RG_NATIVE_PIPELINE=1` with the native renderer on (the F1 menu's "Native
renderer", or `RG_NATIVE_RENDERER=1`). It is decided once, at its first use
after the menu's settings (`native::setOuterSwitch` in `graphics.cpp`);
without the native renderer it is ignored and says so. The log says
`[native-pipeline] on`. With `DQ8_GFX_STATS_EVERY`, the stats report gets
the renderer's `[native-pipeline]` line (records by kind, the readiness
counters R1-R5) and the producer's `[native-producer]` line (textures,
palettes and targets made, meshes made and why not, material misses,
effects made and why not).
