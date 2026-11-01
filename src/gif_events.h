#pragma once

#include <cstdint>
#include <string>
#include <vector>

// What a GS packet does, as a list of events: each register it writes, and
// each vertex it kicks with the vertex state at that moment. Two packets that
// give the same events draw the same thing, however they were laid out (PACKED
// or REGLIST, one tag or several), which is what a native replacement of a VU1
// program has to match.
namespace rg::gif
{

struct GifEvent
{
    enum class Kind : uint8_t
    {
        Register, // a write to a register that is not vertex state
        Vertex,   // XYZ2, XYZF2 (drawing kick) or XYZ3, XYZF3 (no kick)
    };
    Kind kind = Kind::Register;
    // Register: its address and value.
    uint8_t reg = 0;
    uint64_t value = 0;
    // Vertex: whether it kicks a primitive, and the vertex state.
    bool drawing = false;
    bool fog = false; // from XYZF*, with its F
    uint16_t prim = 0;
    uint16_t x = 0, y = 0; // 12.4 fixed point
    uint32_t z = 0;
    uint8_t f = 0;
    uint8_t rgba[4] = {};
    float q = 1.0f, s = 0.0f, t = 0.0f;
    uint16_t u = 0, v = 0; // 10.4 fixed point
};

struct GifDecodeStats
{
    uint32_t tags = 0;
    uint32_t imageQwords = 0;
    bool truncated = false;
};

// The vertex state the GS keeps from one packet to the next.
struct GifState
{
    uint16_t prim = 0;
    uint8_t rgba[4] = {};
    float q = 1.0f, s = 0.0f, t = 0.0f;
    uint16_t u = 0, v = 0;
    uint8_t f = 0;
};

// Appends the events of one packet: a chain of GIF tags up to EOP or the end
// of the bytes. IMAGE data is counted, not decoded.
GifDecodeStats decodeGifPacket(const uint8_t *data, uint32_t size, GifState &state, std::vector<GifEvent> &out);

// Field tolerances for compare(): differences up to these count as equal.
struct Tolerance
{
    uint32_t xy = 0;    // in 1/16 pixel
    uint32_t z = 0;     // in Z units
    uint32_t rgba = 0;  // per channel
    uint32_t uv = 0;    // in 1/16 texel
    uint32_t f = 0;
    float stq = 0.0f;   // relative
};

// Largest differences found, and how many events differ beyond the tolerance.
struct Comparison
{
    uint64_t events = 0;
    uint64_t vertices = 0;
    uint64_t mismatched = 0;     // events differing beyond the tolerance
    uint64_t kindMismatches = 0; // different kind, register, or drawing/no-kick
    int64_t countDifference = 0; // candidate events minus reference events
    uint32_t maxXy = 0;
    uint32_t maxZ = 0;
    uint32_t maxRgba = 0;
    uint32_t maxUv = 0;
    float maxStq = 0.0f;
    int64_t firstMismatch = -1;
    void add(const Comparison &other);
};
Comparison compare(const std::vector<GifEvent> &reference, const std::vector<GifEvent> &candidate,
                   const Tolerance &tolerance);

std::string describe(const GifEvent &event);

} // namespace rg::gif
