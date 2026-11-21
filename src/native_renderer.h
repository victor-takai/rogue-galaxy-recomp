#pragma once

// The F1 menu's "Native renderer" switch (settings.ini's native_renderer,
// off unless set; RG_NATIVE_RENDERER=0 or 1 for a run). It is what the
// native side of the renderer goes behind as it is developed: the game's
// VU1 programs computed natively and drawn as GPU meshes, and the post
// effects drawn by the renderer instead of as the GS packets the game
// builds for them. Off, everything goes through the GS emulation, as
// before. Here it turns on RG_VU1_NATIVE=gpu (src/vu1_native.h) and the
// VU0's matrix program native (RG_VU0_NATIVE, src/vu0_native.cpp), which
// their variables still override, and RG_NATIVE_PIPELINE=1 and
// RG_VU1_CLIP_ON_GPU=1 (src/vif1_thread.cpp) work only with it on.
//
// graphics.cpp hands the menu's setting over when the menu applies its
// settings, before the game starts. The first read of enabled() keeps it for
// the run, so a change takes effect at the next start, and code that sets
// itself up once (hooks, threads) sees the same answer as code that asks
// every frame.
namespace rg::nativerenderer
{

// The menu's setting (any thread); only reads after it count.
void setPreferred(bool on);

// Whether this run uses the native renderer: RG_NATIVE_RENDERER when set,
// otherwise the menu's setting at the first call.
bool enabled();

} // namespace rg::nativerenderer
