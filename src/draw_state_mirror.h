#pragma once

#include <cstdint>

#include "runtime/gs/gs_types.h"

class GS;

// The GS draw registers as the game sets them, kept on the game's side for
// the native pipeline (docs/NATIVE-PIPELINE.md): a VU1 mesh becomes a mesh
// draw under the material these registers give it (texture, blend, tests,
// target), read where the mesh is made, in order with the packets before it.
//
// It follows every packet the VIF1 thread sends to the GS (observe), and
// reads the front end's registers back after a libgraph stub has written
// them directly (readBack, patch 0031). Off unless the native pipeline is on;
// the GS path does not use it.
namespace rg::drawstate
{

// Whether the mirror is followed (the native pipeline is on).
bool enabled();

// A packet as it goes to the GS: its register writes, in order. Any thread;
// the VIF1 thread and the game thread take turns (the VIF1 thread's lists
// are drained before the game thread sends).
void observe(const uint8_t *data, uint32_t size);

// After something wrote the front end's registers without a packet (the
// libgraph stubs): the mirror takes them from the front end.
void readBack(const GS &gs);

// The draw state of now: what a primitive drawn next would be drawn under.
GSDrawState current();

} // namespace rg::drawstate
