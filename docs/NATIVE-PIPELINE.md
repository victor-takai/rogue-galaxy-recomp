# The native pipeline: no GS when the flag is on

## The goal

With the native pipeline on (`RG_NATIVE_PIPELINE=1`, under the F1 menu's
"Native renderer" switch; the switch alone keeps the GS), the GS does not
exist as a concept on the renderer's side. There is no GS local memory, no GS registers, no GS
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
- **2D as window-space draws** (step 5b, `src/native_2d.h`): every GIF
  packet on its way to the GS goes through a converter first. Its
  primitives (sprites, triangles, strips, fans; points and lines as thin
  quads; fogged vertices keep their F) become meshes in the GS's window
  coordinates, which `rg::nativeproducer::piece` makes mesh draws under
  the draw-state mirror's state; the rest of the packet (register writes,
  uploads) goes on to the GS, with a PRIM write of each mesh's attributes
  just before it, so the mirror holds the mesh's state when it comes. The
  GS draws nothing of a converted packet. A piece the producer refuses
  (counted by why in its stats line) goes to the GS as a native mesh.
  `RG_NATIVE_2D=0` turns the converter off.
- **Object layers** (step 5c): stage E's per-object composites
  (`ModelDrawOffscreenComposite`) draw each character into an offscreen
  buffer (its head: a clear, the model) and lay it over the frame (its
  tail). Hooks mark each object's head and tail packets; the VIF1 thread
  routes the head's draws into an object layer of the renderer's own,
  cleared over the head's clear box (`kRouteLayer`), and makes the tail's
  sprites read that layer (`kObjectTail`). The GS's buffer, which shadow
  volumes and uploads also use, is never read for it, so no boxes. A tail
  the builder or the producer refuses puts the layer back into the buffer
  first and goes on as packets. VU1 kicks are placed in a tail by where the
  MSCAL that ran them sits in the list (patch 0032, `vif1Cursor`).
  `RG_NATIVE_COMPOSITE=0` turns the batching off.
- **Uploads** (steps 3b and 6): an upload the producer already made a
  record of is withheld from the GS (its transfer registers and its image
  data, in the packet and in the continuations after it); the rest of the
  packet goes on. The data withheld is checked against the record's hash
  of the guest memory it was made from (uploads up to 16 KiB;
  `RG_NATIVE_UPLOAD_CHECK=all` every one, `=0` none): the game writes a
  few palettes and textures after the texture hook, and such a record is
  made again from what was sent (`replaceUpload`). A host upload nobody
  took is gathered on its way to the GS and kept by the producer as an
  image per base and format (`hostUpload`), which pieces reading that
  memory sample; a local copy (`TRXDIR 2`) is mirrored between what the
  producer holds (`noteLocalCopy`).

- **The stubs' registers** (step 7a): libgraph's stubs (`sceGsPutDrawEnv`,
  `sceGsSwapDBuff`'s environments and clear) write the GS's registers
  directly. Under the pipeline's 2D those writes are taken (patch 0034's
  register sink) and go through the converter as one A+D packet, so the
  mirror follows them and the clear is a 2D piece.
- **Depth and colour reads** (steps 7b, 7c; `src/native_capture.cpp`): the
  game's `sceGsExecStoreImage` reads are answered from the renderer
  straight into the game's buffer, with no GS transfer and no path sync. A
  depth read (the lens flare's 8x8 block of Z) comes from the renderer's
  depth a frame or two late (`readDepth`; it needs D32_FLOAT depth). A
  colour read (the fades' and menus' captured frame) comes from the target
  drawn at that address, or the scene, as the last frame drawn left it
  (`captureTarget`, `native::beginColorRead`), answered on the presenting
  thread. Targets clamp alpha above 0x80, so a capture's alpha does too.
- **The pipeline's own present** (step 7d): the runtime's loop calls a host
  present hook (patch 0033) that shows the frame each flip's `Present`
  closed, through the backend's `presentNativeFrame`, instead of the GS's
  display registers; with no flip for 200 ms the last frame is shown again.
  Flip pacing is off under the pipeline: nothing waits on the GS queue.
- **No GS** (step 6's absorber, step 7's tripwire): what is left of a
  packet after the converter and the upload withholding goes nowhere; the
  game still gets SIGNAL, FINISH and LABEL in the privileged registers, set
  as the GS would (`RG_NATIVE_ABSORB=0` sends it to the GS). The GS backend
  is installed with nothing around it (no worker, no pacing) behind a
  tripwire (`src/gs_tripwire.h`): any call that still reaches it is logged
  once with a backtrace and, by default, dropped (`RG_NATIVE_GS=abort`
  stops there, `=pass` lets it through). The backend still makes its GS
  memory and caches at start, unused; not making them needs the backend's
  own changes, not carried over. `tools/native-check.sh NAME` checks a
  run's log (`build/run/NAME.log`) against these.

Under the pipeline the screen shows the renderer's frame alone. Known
limits: targets clamp alpha above 0x80; 7b needs D32_FLOAT depth; 7c is
served from the last frame drawn; the title screen has 2D pieces whose
source is unknown; the Flash Sword's trail is parked (it draws wrong on
the archive branch too). A mesh into a buffer the game never named has
no target and is dropped with the absorbed packets (counted as a target
miss in the `[native-producer]` line), not sent to the tripwire.

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
effects and 2D pieces made and why not).
