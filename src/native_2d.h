#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/gs/gs_types.h"

// The native pipeline's 2D (docs/NATIVE-PIPELINE.md, step 5b): every GIF
// packet on its way to the GS goes through the converter first. Its
// primitives (sprites, triangles, strips, fans; points and lines as thin
// quads) become meshes in the GS's window coordinates, a triangle list
// each, for rg::nativeproducer::piece; what is left of the packet (its
// register writes, uploads) goes on to the GS as packets, in order with
// the meshes. Each mesh is preceded by a PRIM write of its attributes, so
// the draw-state mirror (src/draw_state_mirror.h) holds the mesh's draw
// state when the mesh comes.
//
// The converter carries the GS's vertex state from packet to packet, per
// sending thread (the VIF1 thread and the game thread take turns at the
// GS). Only with the native pipeline on; RG_NATIVE_2D=0 turns it off.
namespace rg::native2d
{

bool enabled();

struct Piece
{
    std::vector<uint8_t> packet; // a packet the converter made, when not empty
    // Or a packet of the input as it is (an upload), in place: valid while
    // the input is.
    const uint8_t *view = nullptr;
    uint32_t viewSize = 0u;
    GSNativeMesh mesh; // otherwise
    // The mesh's UVs are texels (FST), not normalised coordinates.
    bool texelUvs = false;
    bool isPacket() const { return view != nullptr || !packet.empty(); }
    const uint8_t *bytes() const { return view ? view : packet.data(); }
    uint32_t size() const { return view ? viewSize : static_cast<uint32_t>(packet.size()); }
};

struct Span
{
    const uint8_t *data;
    uint32_t size;
};

// The packets, in order, as pieces: register-only packets and uploads, and
// meshes. False, with `why`, when they hold something the converter cannot
// express (a reserved primitive type): then they go on as they are.
bool build(const Span *packets, size_t count, std::vector<Piece> &out, std::string &why);

// A packet going to the GS as it is (not through build): the vertex state
// the converter carries follows it, as the GS's does.
void observe(const uint8_t *data, uint32_t size);

} // namespace rg::native2d
