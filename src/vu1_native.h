#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "runtime/gs/gs_types.h"

// VU1 programs done natively: what a program would kick for a mesh chunk,
// computed from the same VU1 data memory, for docs/NATIVE-RENDERER.md's phase 1.
//
// The game's model programs are composed per draw from pieces in six code
// slots (A at pair 0x64, B 0x12C, C 0x21C, D 0x320, E 0x4C4, F 0x73A). A
// program is recognized by the FNV-1a hash of each piece's code, so no game
// code is kept here. Every piece was read from the code with
// tools/vu1/vudis.py over a recording's coverage and is checked against the
// VU1 with tools/vu1/oracle; see its README.
//
// Done: every program the recordings hold (docs/VU1-NATIVE.md, "Status"):
// the strip programs, composed from a piece per slot (A; B plain or skinned;
// C vertex colors, one color, lit, lit vertex colors; D: none, guard band,
// guard band and backfaces, each with or without the scaled depth and
// bounds; F scroll or sphere map), and the programs whose pieces draw by
// themselves (Kind): the outlines, the shadow volumes, the overlay sprites,
// the object setup, the particles and the projected texture. Not done, so
// left to the VU1 chunk by chunk: clipping (slot E) where the GPU does not
// clip, the paths no recording took, and the pieces no recording reached.
//
// The arithmetic per vertex is the runtime's (gsNativeMeshPoint in
// runtime/gs/gs_types.h), whichever way a chunk goes out.
namespace rg::vu1native
{

// How the D piece sets ADC on a strip's vertices.
enum class Cull : uint8_t
{
    None,
    GuardBand,
    GuardBandAndBackface,
};

// What the C piece gives each vertex.
enum class Color : uint8_t
{
    PerVertex, // the vertex's int16 color times the material color
    Material,  // the material color alone
    Lit,       // four directional lights on the normal, and an ambient
    LitVertex, // that, times the vertex's int16 color / 32768 * 256
};

// What the F piece does to the UVs.
enum class Uv : uint8_t
{
    Plain,     // nothing, unless quad 88.xy scrolls them (declined)
    SphereMap, // replaced by the normal in view space, as (n.xy + 1) / 2
};

// What the program draws: the A piece decides (it calls the others).
enum class Kind : uint8_t
{
    Strip,   // triangle strips and lists: A, B, C, D, F (E clips)
    Outline, // the toon outlines: A, B, then C draws the silhouette edges as lines
    Shadow,  // the shadow volumes: A, B, then C extrudes the lit triangles as fans
    Overlay, // the screen sprites over the last object's bounds: B's own entry, its MSCAL
    Base,    // the per-object setup at 0x000: the state packet, matrices and lights into VU1 memory
    Particles, // camera-facing quads, rotated and scaled: A, then D draws them
    Projector, // strips with a projected texture: A, B, F (UVs, color), C transforms and kicks
};

// How the B piece decodes a vertex's position.
enum class Decode : uint8_t
{
    Plain,   // int16 / 32768 times quad 77
    Skinned, // that, then summed over 1, 2 or 4 bones (A's entry into B, 76.z)
};

// A program: the behavior of each of its pieces. identify() composes it from
// the pieces present, so each combination is one Program, alive for good.
struct Program
{
    const char *name;
    Cull cull;
    Color color;
    // The projected z scaled by quad 87.z and capped by quad 1.y, and the
    // vertices' bounds kept in quads 98 (least) and 99 (most).
    bool depthBounds = false;
    Decode decode = Decode::Plain;
    // The most bones the B piece has an entry for (Skinned).
    uint8_t maxBones = 0;
    Kind kind = Kind::Strip;
    Uv uv = Uv::Plain;
};

// Whether buildMesh can make the program's chunks a GSNativeMesh; if not,
// the GPU path sends buildChunk's packets.
inline bool hasMesh(const Program &program)
{
    return program.kind == Kind::Strip;
}

// Whether the program draws in its MSCAL (the overlay sprites) rather than
// in the MSCNTs after it (every other: the MSCAL only ends). Its chunk is
// then the MSCAL's run.
inline bool drawsAtCall(const Program &program)
{
    return program.kind == Kind::Overlay || program.kind == Kind::Base;
}

// The program whose MSCAL enters at `startPC` (in bytes) with `code` (all of
// micro memory) in place, or null. Hashes about 6 KiB: callers cache it per
// code image.
const Program *identify(const uint8_t *code, uint32_t startPC);

// Why the chunk an MSCNT runs (its header at `top`, in quadwords) is left to
// the VU1, or null when it is done natively. `clips`: what draws it clips
// triangles itself (the GPU), so slot E's chunks are done too.
const char *declineChunk(const Program &program, const uint8_t *data, uint32_t top, bool clips = false);

// Which of the pieces this file knows the code at `startPC` has, slot by
// slot, for rg_vu1_oracle --inventory: "A F B:plain C:vertex colors D:guard
// band" when all of a program, "A F B:? C:one color D:?" when B and D are
// other pieces.
std::string describePieces(const uint8_t *code, uint32_t startPC);

// What the program leaves in VU1 data memory that a later run, or the EE,
// may read: the bounds in quads 98 and 99 for the programs with
// `depthBounds`. `apply` writes it where the VU1 did not run (skip, gpu);
// `matches` says whether the VU1 left the same (check), from the data
// memory before the run and after it.
void applyMemoryEffects(const Program &program, uint8_t *data, uint32_t top);
bool memoryEffectsMatch(const Program &program, const uint8_t *before, uint32_t top, const uint8_t *after);

// The GS packets the program kicks for that chunk, in order.
void buildChunk(const Program &program, const uint8_t *data, uint32_t top,
                std::vector<std::vector<uint8_t>> &packets);

// The chunk as a mesh: the packets that set the GS state it draws under
// (FOGCOL, PRIM and PRMODE), to go first, and the strip itself, for
// GS::submitNativeMesh.
void buildMesh(const Program &program, const uint8_t *data, uint32_t top,
               std::vector<std::vector<uint8_t>> &packets, GSNativeMesh &mesh);

// RG_VU1_NATIVE, in the game (src/vif1_thread.cpp), for the chunks done
// natively: `check` runs the VU1 as before and compares its packets with the
// native ones; `1` runs the VU1 (its state stays as it was) and sends the
// native packets instead of its own; `skip` sends the native packets without
// running the VU1; `gpu` sends the chunk as a mesh the renderer transforms,
// without running the VU1. Unset or 0: off.
enum class Mode : uint8_t
{
    Off,
    Check,
    Replace,
    Skip,
    Gpu,
};
Mode modeFromEnvironment();

// What the VIF1 thread did with each strip chunk, and in check mode how the
// VU1's packets compare with the native ones (within 1/16 pixel, 16 Z units,
// one color level and 1e-5 of STQ); a line every two seconds, and the first
// few differences in full.
class Monitor
{
public:
    explicit Monitor(Mode mode) : m_mode(mode) {}

    void native(const Program &program) { ++m_native[program.name]; }
    void declined(const Program &program, const char *reason);
    // What differs (the first of: vertex count, ADC or a register, position,
    // Z, color, UV, STQ, fog), or null when nothing does beyond the tolerance.
    const char *check(const Program &program, const std::vector<std::vector<uint8_t>> &vu,
                      const std::vector<std::vector<uint8_t>> &native);
    // A difference found outside the packets (VU1 memory), counted as check's.
    void noteKind(const Program &program, const char *kind)
    {
        ++m_differ;
        ++m_kinds[std::string(program.name) + ": " + kind];
    }
    // Prints the line when it is due.
    void tick();

private:
    Mode m_mode;
    std::map<std::string, uint64_t> m_native;
    std::map<std::string, uint64_t> m_declined;
    std::map<std::string, uint64_t> m_kinds;
    uint64_t m_checked = 0u;
    uint64_t m_differ = 0u;
    uint32_t m_shown = 0u;
    int64_t m_next = 0;
};

} // namespace rg::vu1native
