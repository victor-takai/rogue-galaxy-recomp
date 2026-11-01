# One frame of Salgin's first scene, pass by pass

From `[frame]` lines (`SIGUSR2` with `DQ8_GFX_PROFILE_CYCLE` set; see
`sdlgpu_backend.cpp`), 4x, 2026-10-28. A pass is what the backend submits as
one render pass; a snapshot pass is one that a feedback draw forced, preceded
by a copy of its whole target. The dump holds one frame, and the next one
starts identically at pass 191.

**191 passes, 805 draws, 222 Mpx of draw bounding boxes, 98 snapshot passes.**

| Stage | Passes | Draws | Snapshot passes | Mpx | What it is |
|---|---|---|---|---|---|
| A | 80 | 97 | 69 | 44.1 | A copy of the last frame, a downscale to 192x192 (0x2A00), then a **blur of the 192x192 bloom buffer 0x2C40 as 69 passes of one draw each**, each sampling the buffer it writes; composited onto the frame (0x0), then clears and a full-frame copy to 0xE00 |
| B | 2 | 408 | 0 | 120.1 | **The scene.** Pass 80: 83 draws into 0x0 (81 from VU1 programs), 60 Mpx. Pass 81: 325 draws into 0x2A00 (323 VU1; the `#0970` blended sprites), 60 Mpx |
| C | 37 | 38 | 24 | 7.2 | A second blur: 24 single-draw snapshot passes on a second 192x192 buffer (0x3800), then 12 on 192x384 |
| D | 3 | 4 | 0 | 14.7 | Full-frame composites (0x0, 0x2A40, 0x0) |
| E | 47 | 197 | 0 | 18.2 | **Per-object offscreen pairs**: draw an object into the 512x448 buffer 0x2A00/0x2AC0/etc, then composite it onto 0x0 with a 4-draw pass. About 23 objects |
| F | 22 | 61 | 5 | 17.9 | 64x64 and 128x128 buffers (shadow maps, lens flare probes), composites, small snapshots |

Totals by target size: 129 of the 191 passes go into targets of 256x256 or
less and hold 31.5 Mpx (14% of the pixels); the 62 passes into 512x448
targets hold 190.7 Mpx.

## What it says

- The scene itself (B) is **2 passes**. It is 54% of the box area, and the
  profile put its VU1 geometry at roughly 10 W of 78: cheap per pixel.
- Everything else is **post-processing and bookkeeping**: 189 passes, 84 Mpx,
  and about 36 W in the profile. The blur alone is 105 passes (A + C) and 98
  snapshot copies. These exist because the PS2 drew its blurs as sprites
  sampling the framebuffer they write, which a GPU can only do by copying the
  target before every draw and ending the pass.
- On a tile-based mobile GPU every one of those pass boundaries writes the
  tile memory out to DRAM and reads it back. At 4x the 192x192 bloom buffer is
  768x768 (2.4 MB): 98 snapshot passes is about 470 MB of traffic a frame
  (arithmetic, not measured) from the blur alone.

## Open

- Derive the exact blur from the draws (needs the UV offsets of each
  sprite, not in this dump) and replace the chain with a native blur at the
  buffer's own resolution: ~100 passes become a handful.
- The per-object pairs (E): what the offscreen is for, and whether a draw
  limited to the object's box does the same with the render area set.
- The `SMALL_SCALE` switch (targets of 256x256 or less capped to a lower
  scale) cuts those 129 passes' pixel work but not their count.
