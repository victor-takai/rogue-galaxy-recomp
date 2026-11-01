#include "vu1_native.h"

#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

#include "gif_events.h"

// The strip programs, piece by piece (byte addresses):
//   the MSCAL at 0x320 only ends; each mesh chunk is UNPACKed and then run by
//   an MSCNT from 0x328, through
//   A 0x328  the chunk header at TOP (PRIM, vertex count, vertex tag, the
//            sections' offsets), the GIF header of the output (template
//            quad 0, PRIM, PRMODE from quad 87.x, the vertex tag with quad
//            86), the guard band (quad 109 = 17 with xy times 16.xy, 110 = 18);
//   B 0x960  int16 positions, normals and UVs to floats: x / 32768 times the
//            mesh scales in quads 77, 78, 79 (the skinning B then sums the
//            position over the vertex's bones);
//   F 0x39d0 nothing unless quad 88.xy is set (scrolling UVs), or a
//            sphere map from the normals;
//   C 0x10e0 the vertex colors: the int16 color / 32768 * 256 times
//            quad 82 * quad 42 + quad 83 * 128 (82.w scaled by 81.w), or
//            that color alone, or lights on the normal;
//   E 0x2620 with quad 81.x & 2 only: clipping, before D;
//   D 0x1900 per vertex: clip = position * quads 68-71 (the matrix lands in
//            screen space), Q = 1/w, XYZ in 12.4, F the fog clamp(40.x +
//            40.w * w, 40.y, 40.z) in XYZF2's w, ST = UV * Q, the color
//            clamped to quad 39; ADC as its Cull says; then FOGCOL (quad 41
//            behind tag quad 19, its low word zeroed with quad 87.y & 0x10)
//            is kicked, then the chunk.
// The other programs (Kind) are written out below, each by itself:
// buildOutline, buildShadow, runOverlay, runBase, buildParticles,
// buildProjector.
// The arithmetic runs in the VU's rounding (toward zero), set around each
// entry point (VuRounding): the compiler must not move float work across it.
#pragma STDC FENV_ACCESS ON

namespace rg::vu1native
{
namespace
{

constexpr uint32_t kEntry = 0x320u;
constexpr uint32_t kOverlayEntry = 0x960u;
constexpr uint32_t kBaseEntry = 0x000u;

// Round toward zero while it lives, as VU1Interpreter::run sets it for the
// VU1: the products, sums and the divide then come out as the VU1's, bit for
// bit, done in its order. (Round to nearest left Z a unit off at times, which
// a pass drawn at nearly the same depth turned into missing pixels.) And
// denormals as zero, in and out (DAZ, FTZ), as the VU has them: the overlay
// sprites add a denormal to -2^24, which must stay -2^24.
class VuRounding
{
public:
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    VuRounding() : m_saved(_mm_getcsr()) { _mm_setcsr((m_saved & ~0x6000u) | 0x6000u | 0x8040u); }
    ~VuRounding() { _mm_setcsr(m_saved); }

private:
    uint32_t m_saved;
#else
    VuRounding() : m_saved(std::fegetround()) { std::fesetround(FE_TOWARDZERO); }
    ~VuRounding() { std::fesetround(m_saved); }

private:
    int m_saved;
#endif
};

struct Piece
{
    uint32_t start;
    uint32_t end;
    uint64_t hash;
};

// Slots A and F, the same in every strip program met so far.
constexpr Piece kCommon[] = {
    {0x0320u, 0x05d8u, 0x07151c2597283286ull},
};
constexpr const char *kCommonNames[] = {"A"};

// The F pieces (A calls F after B, before C): d3f6f631 scrolls the UVs by
// quad 88.xy (declined when set), 150031ee replaces them by a sphere map.
struct UvPiece
{
    Piece f;
    Uv uv;
};
constexpr UvPiece kUvs[] = {
    {{0x39d0u, 0x3a68u, 0xd3f6f6312a13ff1cull}, Uv::Plain},
    {{0x39d0u, 0x3b70u, 0x150031ee1534d185ull}, Uv::SphereMap},
};

// The B pieces. A calls B at 0x960, or with quad 76.z set at 0x960 plus
// the chunk's TOP+1.w pairs: af62ff64 has the one entry; 26a933a5 has four,
// 0 one bone, 2 two bones, 4 and 6 four (skin()).
// 542b235c (the outlines') has the first two, its one-bone path positions
// only.
struct DecodePiece
{
    Piece b;
    Decode decode;
    uint8_t maxBones;
};
constexpr DecodePiece kDecodes[] = {
    {{0x0960u, 0x0a38u, 0xaf62ff64ac67b45full}, Decode::Plain, 0},
    {{0x0960u, 0x1020u, 0x26a933a58f99a9daull}, Decode::Skinned, 4},
    {{0x0960u, 0x0f30u, 0x542b235c4775a710ull}, Decode::Skinned, 2},
    // Positions only, one entry (a shadow volume B).
    {{0x0960u, 0x09e0u, 0xa293001f25f94532ull}, Decode::Plain, 0},
};

// The outline program's A (header with PRIM 1, lines; calls B, then C) and
// C (3689bac9, which also fills slot D's space).
constexpr Piece kOutlineA = {0x0320u, 0x04e8u, 0x3f6df595be6e31b7ull};
constexpr Piece kOutlineC = {0x10e0u, 0x1e70u, 0x3689bac97cae3c73ull};

// The base piece: the setup each object's draw starts with (runBase()).
constexpr Piece kBase = {0x0000u, 0x02c0u, 0x12ad73d26e1badecull};

// The overlay sprites: B 6e42dac6 entered by an MSCAL at its start.
constexpr Piece kOverlayB = {0x0960u, 0x0b38u, 0x6e42dac662887054ull};

// The particle program's A (header with PRIM 4; calls D only) and D
// (8ecadb3a).
constexpr Piece kParticleA = {0x0320u, 0x04a8u, 0x0b1031aeb003ccb0ull};
constexpr Piece kParticleD = {0x1900u, 0x2890u, 0x8ecadb3a83d5e584ull};

// The projected texture program: A 4beba151 (calls B, F, E with 81.x & 2,
// then C), F 15be21a5 (the UVs and color) and C 3b890d00 (the rest).
constexpr Piece kProjectorA = {0x0320u, 0x05c0u, 0x4beba151fbf23f60ull};
constexpr Piece kProjectorF = {0x39d0u, 0x3af8u, 0x15be21a50caaa048ull};
constexpr Piece kProjectorC = {0x10e0u, 0x1b00u, 0x3b890d00703404b6ull};

// The shadow volume program's A (header with PRIM 5, fans; calls B, then C)
// and C (dc8dbfe4).
constexpr Piece kShadowA = {0x0320u, 0x0500u, 0x821b638f42fcaa6eull};
constexpr Piece kShadowC = {0x10e0u, 0x1938u, 0xdc8dbfe4905cb635ull};

// The C pieces: both build the material color, quad 82 * 42 + 83 * 128
// (82.w scaled by 81.w), or with 87.y & 0x200 82 * 43 and no ambient;
// 70362be4 multiplies it by each vertex's int16 color / 32768 * 256 (and
// has a loop of its own with 87.y & 0x100, not read yet), 780d503e gives
// it to every vertex as it is.
struct ColorPiece
{
    Piece c;
    Color color;
};
constexpr ColorPiece kColors[] = {
    {{0x10e0u, 0x1328u, 0x70362be426b17f94ull}, Color::PerVertex},
    {{0x10e0u, 0x11b0u, 0x780d503e649c9116ull}, Color::Material},
    {{0x10e0u, 0x1240u, 0x91f542d069307d6cull}, Color::Lit},
    // 91f542d0's lighting times the vertex colors (87.y & 0x100: a loop of
    // its own, not read).
    {{0x10e0u, 0x14a0u, 0x2460e5281034f152ull}, Color::LitVertex},
};

// The D pieces. (a786efabfa469ada is also the name of its compiled routine,
// the VIF1 thread's busiest in docs/STATUS.md.)
struct Finish
{
    Piece d;
    Cull cull;
    bool depthBounds;
};
constexpr Finish kFinishes[] = {
    {{0x1900u, 0x20c8u, 0xa786efabfa469adaull}, Cull::GuardBandAndBackface, false},
    {{0x1900u, 0x1c38u, 0x126a58a39a8f944aull}, Cull::GuardBand, false},
    {{0x1900u, 0x1bc0u, 0x4b7b81f2fe1beeefull}, Cull::None, false},
    // No culling; z times 87.z, at most 1.y; the bounds into 98 and 99.
    {{0x1900u, 0x1b98u, 0xb8ea8ed53805b0c4ull}, Cull::None, true},
    // The guard band (its z before the scale), and that depth and bounds.
    {{0x1900u, 0x1cb8u, 0x634107b0f8a88c88ull}, Cull::GuardBand, true},
    // The guard band and backfaces, and that depth and bounds; triangle
    // lists have a path of their own (declined).
    {{0x1900u, 0x21e0u, 0x93cfd2f5a1056991ull}, Cull::GuardBandAndBackface, true},
};

// The program of these pieces, made once and kept: its name says what each
// piece does ("strip, guard band, one color").
const Program *intern(const Program &shape)
{
    static std::mutex mutex;
    static std::deque<Program> programs;
    static std::deque<std::string> names;
    const std::lock_guard<std::mutex> lock(mutex);
    for (const Program &program : programs)
    {
        if (program.cull == shape.cull && program.color == shape.color && program.depthBounds == shape.depthBounds &&
            program.decode == shape.decode && program.maxBones == shape.maxBones && program.kind == shape.kind &&
            program.uv == shape.uv)
        {
            return &program;
        }
    }
    std::string &name = names.emplace_back(shape.kind == Kind::Outline  ? "outline"
                                           : shape.kind == Kind::Shadow ? "shadow volume"
                                           : shape.kind == Kind::Overlay ? "overlay sprites"
                                           : shape.kind == Kind::Base    ? "object setup"
                                           : shape.kind == Kind::Particles ? "particles"
                                           : shape.kind == Kind::Projector ? "projected texture"
                                                                        : "strip");
    name += shape.kind != Kind::Strip                  ? ""
            : shape.cull == Cull::GuardBandAndBackface ? ", guard band and backfaces"
            : shape.cull == Cull::GuardBand            ? ", guard band"
                                                       : "";
    name += shape.depthBounds ? ", depth and bounds" : "";
    name += shape.color == Color::Material && shape.kind == Kind::Strip ? ", one color"
            : shape.color == Color::Lit                                  ? ", lit"
            : shape.color == Color::LitVertex                            ? ", lit vertex colors"
                                                                         : "";
    name += shape.uv == Uv::SphereMap ? ", sphere map" : "";
    name += shape.decode == Decode::Skinned ? ", skinned" : "";
    Program &program = programs.emplace_back(shape);
    program.name = name.c_str();
    return &program;
}

uint64_t fnv(const uint8_t *bytes, uint32_t size)
{
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t i = 0; i < size; ++i)
    {
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    return hash;
}

bool matches(const uint8_t *code, const Piece &piece)
{
    return fnv(code + piece.start, piece.end - piece.start) == piece.hash;
}

// The entry of `table` whose piece (the member `piece`) is in `code`, or null.
template <typename Entry, size_t N>
const Entry *present(const uint8_t *code, const Entry (&table)[N], Piece Entry::*piece)
{
    for (const Entry &entry : table)
    {
        if (matches(code, entry.*piece))
        {
            return &entry;
        }
    }
    return nullptr;
}

struct Vec4
{
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};

class VuData
{
public:
    explicit VuData(const uint8_t *data) : m_data(data) {}

    Vec4 quad(uint32_t q) const
    {
        Vec4 v;
        std::memcpy(&v, bytes(q), sizeof(v));
        return v;
    }
    int32_t word(uint32_t q, uint32_t lane) const
    {
        int32_t v;
        std::memcpy(&v, bytes(q) + lane * 4u, sizeof(v));
        return v;
    }
    // ILW reads the low 16 bits of a word.
    uint32_t half(uint32_t q, uint32_t lane) const { return static_cast<uint32_t>(word(q, lane)) & 0xFFFFu; }
    const uint8_t *bytes(uint32_t q) const { return m_data + (q & 1023u) * 16u; }

private:
    const uint8_t *m_data;
};

float itof15(int32_t v)
{
    return static_cast<float>(v) / 32768.0f;
}

float fromBits(uint32_t bits)
{
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// How many bones the skinning B's entry sums (A enters B at 0x960 plus
// TOP+1.w pairs when quad 76.z is set): 1, 2 or 4 (entries 4 and 6 branch
// to the same code), or 0 for another entry.
uint32_t bonesOf(const Program &program, const VuData &vu, uint32_t top)
{
    const uint32_t entry = vu.half(76, 2) != 0u ? vu.half(top + 1u, 3) : 0u;
    const uint32_t bones = entry == 0u ? 1u : entry == 2u ? 2u : entry == 4u || entry == 6u ? 4u : 0u;
    return bones <= program.maxBones ? bones : 0u;
}

// The skinning B: the position (w 1) scaled by each bone's weight (an int
// / 255), times the bone's matrix (quads 900 + 2 * index, rows 0-3, xyz),
// summed in ACC bone by bone and row by row. The indices are the low halves
// of the quad at `weights`, the weights the words of the next.
void skin(const VuData &vu, uint32_t weights, uint32_t bones, float position[3])
{
    const float scale = fromBits(0x3b808081u); // the program's I: 1/255
    float acc[3] = {};
    bool first = true;
    for (uint32_t b = 0; b < bones; ++b)
    {
        const float w = static_cast<float>(vu.word(weights + 1u, b)) * scale;
        const float scaled[4] = {position[0] * w, position[1] * w, position[2] * w, w};
        const uint32_t matrix = 900u + ((vu.half(weights, b) * 2u) & 0xFFFFu);
        for (uint32_t row = 0; row < 4u; ++row)
        {
            const Vec4 m = vu.quad(matrix + row);
            const float lanes[3] = {m.x, m.y, m.z};
            for (int i = 0; i < 3; ++i)
            {
                const float product = lanes[i] * scaled[row];
                acc[i] = first ? product : acc[i] + product;
            }
            first = false;
        }
    }
    std::memcpy(position, acc, sizeof(acc));
}

void putQuad(std::vector<uint8_t> &packet, const void *bytes)
{
    const auto *b = static_cast<const uint8_t *>(bytes);
    packet.insert(packet.end(), b, b + 16);
}

void putWords(std::vector<uint8_t> &packet, uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
    const uint32_t words[4] = {x, y, z, w};
    putQuad(packet, words);
}

void putFloats(std::vector<uint8_t> &packet, float x, float y, float z, float w)
{
    const float words[4] = {x, y, z, w};
    putQuad(packet, words);
}

// Vertex k's position as B leaves it: the int16 / 32768 times quad 77, then
// skinned over `bones` bones (0: not skinned).
void decodePosition(const VuData &vu, uint32_t top, uint32_t k, uint32_t bones, float position[3])
{
    const uint32_t positions = top + 2u;
    const Vec4 scale = vu.quad(77);
    position[0] = itof15(vu.word(positions + k, 0)) * scale.x;
    position[1] = itof15(vu.word(positions + k, 1)) * scale.y;
    position[2] = itof15(vu.word(positions + k, 2)) * scale.z;
    if (bones != 0u)
    {
        skin(vu, positions + vu.half(top + 1u, 2) + k * 2u, bones, position);
    }
}

// The skinning B's normals: as skin(), rows 0-2 only, of the normal scaled
// by each weight.
void skinNormal(const VuData &vu, uint32_t weights, uint32_t bones, float normal[3])
{
    const float scale = fromBits(0x3b808081u);
    float acc[3] = {};
    bool first = true;
    for (uint32_t b = 0; b < bones; ++b)
    {
        const float w = static_cast<float>(vu.word(weights + 1u, b)) * scale;
        const float scaled[3] = {normal[0] * w, normal[1] * w, normal[2] * w};
        const uint32_t matrix = 900u + ((vu.half(weights, b) * 2u) & 0xFFFFu);
        for (uint32_t row = 0; row < 3u; ++row)
        {
            const Vec4 m = vu.quad(matrix + row);
            const float lanes[3] = {m.x, m.y, m.z};
            for (int i = 0; i < 3; ++i)
            {
                const float product = lanes[i] * scaled[row];
                acc[i] = first ? product : acc[i] + product;
            }
            first = false;
        }
    }
    std::memcpy(normal, acc, sizeof(acc));
}

// Vertex k's normal as B leaves it: the int16 (at TOP+2 plus TOP.w) / 32768
// times quad 78, then skinned.
void decodeNormal(const VuData &vu, uint32_t top, uint32_t k, uint32_t bones, float normal[3])
{
    const uint32_t normals = top + 2u + vu.half(top, 3);
    const Vec4 scale = vu.quad(78);
    normal[0] = itof15(vu.word(normals + k, 0)) * scale.x;
    normal[1] = itof15(vu.word(normals + k, 1)) * scale.y;
    normal[2] = itof15(vu.word(normals + k, 2)) * scale.z;
    if (bones != 0u)
    {
        skinNormal(vu, top + 2u + vu.half(top + 1u, 2) + k * 2u, bones, normal);
    }
}

// The lit C (91f542d0): four directional lights, their directions the lanes
// of quads 100-102, their colors quads 33-36, an ambient 37, all times the
// material (quad 82, its w by 81.w). Each light's term is clamped at 0; the
// sum goes into the vertex's RGBA slot, as floats, for D to clamp.
struct Lights
{
    Vec4 direction[3];
    Vec4 color[4];
    Vec4 ambient;

    explicit Lights(const VuData &vu)
    {
        const Vec4 material = vu.quad(82);
        const float alpha = material.w * vu.quad(81).w;
        for (uint32_t i = 0; i < 3u; ++i)
        {
            direction[i] = vu.quad(100u + i);
        }
        for (uint32_t i = 0; i < 4u; ++i)
        {
            const Vec4 c = vu.quad(33u + i);
            color[i] = {c.x * material.x, c.y * material.y, c.z * material.z, 0.0f};
        }
        const Vec4 a = vu.quad(37);
        ambient = {a.x * material.x, a.y * material.y, a.z * material.z, a.w * alpha};
    }

    Vec4 shade(const float n[3]) const
    {
        const float *d0 = &direction[0].x, *d1 = &direction[1].x, *d2 = &direction[2].x;
        float d[4];
        for (int i = 0; i < 4; ++i)
        {
            d[i] = std::fmax(d0[i] * n[0] + d1[i] * n[1] + d2[i] * n[2], 0.0f);
        }
        const float *c0 = &color[0].x, *c1 = &color[1].x, *c2 = &color[2].x, *c3 = &color[3].x;
        const float *base = &ambient.x;
        float out[3];
        for (int i = 0; i < 3; ++i)
        {
            out[i] = base[i] * 1.0f + c0[i] * d[0] + c1[i] * d[1] + c2[i] * d[2] + c3[i] * d[3];
        }
        return {out[0], out[1], out[2], ambient.w};
    }
};

// The sphere map F (150031ee): the normal through the view's rotation
// (quads 24-26, xy, times 64-66), then (uv + 1) / 2.
struct SphereMap
{
    float rows[3][2];

    explicit SphereMap(const VuData &vu)
    {
        const Vec4 r24 = vu.quad(24), r25 = vu.quad(25), r26 = vu.quad(26);
        for (uint32_t i = 0; i < 3u; ++i)
        {
            const Vec4 m = vu.quad(64u + i);
            rows[i][0] = r24.x * m.x + r25.x * m.y + r26.x * m.z;
            rows[i][1] = r24.y * m.x + r25.y * m.y + r26.y * m.z;
        }
    }

    void uv(const float n[3], float &u, float &v) const
    {
        u = (rows[0][0] * n[0] + rows[1][0] * n[1] + rows[2][0] * n[2] + 1.0f) * 0.5f;
        v = (rows[0][1] * n[0] + rows[1][1] * n[1] + rows[2][1] * n[2] + 1.0f) * 0.5f;
    }
};

// A chunk read out of VU1 data memory: the FOGCOL packet, the first quads of
// the program's output (GIF header, PRIM, PRMODE, the vertex tag) and the
// strip as a mesh.
struct Chunk
{
    std::vector<uint8_t> fog;
    uint8_t header[4][16] = {};
    GSNativeMesh mesh;
};

void readChunk(const Program &program, const VuData &vu, uint32_t top, Chunk &chunk)
{
    // FOGCOL (quad 41) behind the tag in quad 19, NLOOP 1 and EOP.
    uint32_t fogTag[4];
    std::memcpy(fogTag, vu.bytes(19), sizeof(fogTag));
    putWords(chunk.fog, 0x8001u, fogTag[1], fogTag[2], fogTag[3]);
    putQuad(chunk.fog, vu.bytes(41));
    if (vu.half(87, 1) & 0x10u)
    {
        std::memset(chunk.fog.data() + 16u, 0, 4u);
    }

    // The chunk: its header at TOP, its sections after it.
    const uint32_t prim = vu.half(top, 0);
    const uint32_t count = vu.half(top, 1);
    const uint32_t positions = top + 2u;
    const uint32_t uvs = positions + vu.half(top + 1u, 0);
    const uint32_t colors = positions + vu.half(top + 1u, 1);
    const uint32_t primWords[4] = {prim, 0u, 0u, 0u};
    const uint32_t prmodeWords[4] = {vu.half(87, 0), 0u, 27u, 0u};
    uint32_t tag[4];
    std::memcpy(tag, vu.bytes(86), sizeof(tag));
    tag[0] = vu.half(top, 2);
    std::memcpy(chunk.header[0], vu.bytes(0), 16u);
    std::memcpy(chunk.header[1], primWords, 16u);
    std::memcpy(chunk.header[2], prmodeWords, 16u);
    std::memcpy(chunk.header[3], tag, 16u);

    GSNativeMesh::Header &h = chunk.mesh.header;
    for (uint32_t row = 0; row < 4u; ++row)
    {
        const Vec4 m = vu.quad(68u + row);
        h.matrix[row][0] = m.x;
        h.matrix[row][1] = m.y;
        h.matrix[row][2] = m.z;
        h.matrix[row][3] = m.w;
    }
    const Vec4 fog = vu.quad(40), colorMax = vu.quad(39);
    const Vec4 viewport = vu.quad(16), band = vu.quad(17), bandOffset = vu.quad(18);
    std::memcpy(h.fog, &fog, sizeof(h.fog));
    std::memcpy(h.colorMax, &colorMax, sizeof(h.colorMax));
    h.bandScale[0] = band.x * viewport.x;
    h.bandScale[1] = band.y * viewport.y;
    h.bandScale[2] = band.z;
    h.bandScale[3] = band.w;
    std::memcpy(h.bandOffset, &bandOffset, sizeof(h.bandOffset));
    h.winding = vu.quad(111).z * ((vu.half(87, 1) & 0x1000u) ? 1.0f : -1.0f);
    h.guardBand = program.cull != Cull::None ? 1u : 0u;
    h.backface = program.cull == Cull::GuardBandAndBackface ? 1u : 0u;
    h.vertexCount = count;
    if (program.depthBounds)
    {
        h.depthScale = vu.quad(87).z;
        h.depthMax = vu.quad(1).y;
    }
    h.triangleList = (prim & 7u) == 3u ? 1u : 0u;

    const Vec4 uvScale = vu.quad(79);
    Vec4 material = vu.quad(82);
    material.w *= vu.quad(81).w;
    const Vec4 light = vu.quad(42);
    const Vec4 ambient = vu.quad(83);
    Vec4 colorBase{material.x * light.x + ambient.x * 128.0f, material.y * light.y + ambient.y * 128.0f,
                   material.z * light.z + ambient.z * 128.0f, material.w * light.w};
    // Either C with 87.y & 0x200: quad 82 times 43, no ambient.
    if ((program.color == Color::PerVertex || program.color == Color::Material) && (vu.half(87, 1) & 0x200u))
    {
        const Vec4 other = vu.quad(43);
        colorBase = {material.x * other.x, material.y * other.y, material.z * other.z, material.w * other.w};
    }
    const bool perVertex = program.color == Color::PerVertex;
    const uint32_t bones = program.decode == Decode::Skinned ? bonesOf(program, vu, top) : 0u;
    const bool lit = program.color == Color::Lit || program.color == Color::LitVertex;
    const bool normals = lit || program.uv == Uv::SphereMap;
    const Lights lights(vu);
    const SphereMap sphere(vu);
    chunk.mesh.vertices.resize(count);
    for (uint32_t k = 0; k < count; ++k)
    {
        GSNativeMesh::Vertex &v = chunk.mesh.vertices[k];
        float position[3];
        decodePosition(vu, top, k, bones, position);
        v.x = position[0];
        v.y = position[1];
        v.z = position[2];
        v.u = itof15(vu.word(uvs + k, 0)) * uvScale.x;
        v.v = itof15(vu.word(uvs + k, 1)) * uvScale.y;
        v.r = perVertex ? colorBase.x * itof15(vu.word(colors + k, 0)) * 256.0f : colorBase.x;
        v.g = perVertex ? colorBase.y * itof15(vu.word(colors + k, 1)) * 256.0f : colorBase.y;
        v.b = perVertex ? colorBase.z * itof15(vu.word(colors + k, 2)) * 256.0f : colorBase.z;
        v.a = perVertex ? colorBase.w * itof15(vu.word(colors + k, 3)) * 256.0f : colorBase.w;
        if (normals)
        {
            float normal[3];
            decodeNormal(vu, top, k, bones, normal);
            if (lit)
            {
                const Vec4 c = lights.shade(normal);
                v.r = c.x;
                v.g = c.y;
                v.b = c.z;
                v.a = c.w;
                if (program.color == Color::LitVertex)
                {
                    v.r = c.x * (itof15(vu.word(colors + k, 0)) * 256.0f);
                    v.g = c.y * (itof15(vu.word(colors + k, 1)) * 256.0f);
                    v.b = c.z * (itof15(vu.word(colors + k, 2)) * 256.0f);
                    v.a = c.w * (itof15(vu.word(colors + k, 3)) * 256.0f);
                }
            }
            if (program.uv == Uv::SphereMap)
            {
                sphere.uv(normal, v.u, v.v);
            }
        }
    }
}

// The outline C. With TOP.x 19 the chunk's points come in fours: an edge
// (the second and third) and the far vertices of its two triangles (the
// first and fourth); otherwise in twos, the edge alone. Each is transformed
// (quads 68-71) and divided. The edge's vertices carry ADC unless both ends
// are inside the guard band and, in fours, the two triangles face the same
// way on screen (the z of their cross products agree in sign: a
// silhouette). It is drawn as four lines, the edge moved by +-h in x, then
// in y: h = alpha / 2, alpha = clamp(10/9 - w / 360, 0, 1) of its first
// end, which is also its RGBA's (16, 16, 16, alpha * 64). The vertex tag's
// NLOOP is the edge count, also written to quad 1023.x.
uint32_t outlinePoints(const VuData &vu, uint32_t top)
{
    return vu.half(top, 0) == 19u ? 4u : 2u;
}

uint32_t outlineLoopWord(const VuData &vu, uint32_t top)
{
    const float share = outlinePoints(vu, top) == 4u ? 0.25f : 0.5f;
    return static_cast<uint32_t>(gsNativeFtoi(static_cast<float>(vu.half(top, 1)) * share + 32768.0f, 1.0f));
}

void buildOutline(const Program &program, const VuData &vu, uint32_t top,
                  std::vector<std::vector<uint8_t>> &packets)
{
    const uint32_t count = vu.half(top, 1);
    const uint32_t points = outlinePoints(vu, top);
    const uint32_t first = points == 4u ? 1u : 0u; // the edge's first end
    const uint32_t bones = program.decode == Decode::Skinned ? bonesOf(program, vu, top) : 0u;
    packets.clear();
    std::vector<uint8_t> &packet = packets.emplace_back();
    packet.reserve(64u + count / points * 144u);
    putQuad(packet, vu.bytes(0));
    putWords(packet, 1u, 0u, 0u, 0u);
    putWords(packet, vu.half(87, 0), 0u, 27u, 0u);
    uint32_t tag[4];
    std::memcpy(tag, vu.bytes(86), sizeof(tag));
    tag[0] = outlineLoopWord(vu, top);
    putQuad(packet, tag);

    float m[4][4];
    for (uint32_t row = 0; row < 4u; ++row)
    {
        std::memcpy(m[row], vu.bytes(68u + row), sizeof(m[row]));
    }
    // The guard band as A leaves it: 109 = 17 with xy times 16.xy, 110 = 18;
    // a point p is inside when every |p * 109 + 110 * 109| <= 1.
    const Vec4 viewport = vu.quad(16), band = vu.quad(17), offset = vu.quad(18);
    const float bandScale[3] = {band.x * viewport.x, band.y * viewport.y, band.z};
    const float bandOffset[3] = {offset.x * bandScale[0], offset.y * bandScale[1], offset.z * bandScale[2]};
    const float fadeBase = fromBits(0x3f8e38e4u);  // 10/9
    const float fadeSlope = fromBits(0xbb360b61u); // -1/360

    for (uint32_t edge = 0; edge < count / points; ++edge)
    {
        float s[4][3];
        float w1 = 0.0f;
        for (uint32_t j = 0; j < points; ++j)
        {
            float p[3];
            decodePosition(vu, top, edge * points + j, bones, p);
            float clip[4];
            for (int i = 0; i < 4; ++i)
            {
                clip[i] = p[0] * m[0][i] + p[1] * m[1][i] + p[2] * m[2][i] + m[3][i];
            }
            const float q = gsNativeReciprocal(clip[3]);
            for (int i = 0; i < 3; ++i)
            {
                s[j][i] = clip[i] * q;
            }
            if (j == first)
            {
                w1 = clip[3];
            }
        }
        bool outside = false;
        for (uint32_t j = first; j <= first + 1u; ++j)
        {
            for (int i = 0; i < 3; ++i)
            {
                const float g = s[j][i] * bandScale[i] + bandOffset[i];
                outside = outside || g > 1.0f || g < -1.0f;
            }
        }
        bool hidden = outside;
        if (points == 4u)
        {
            const float ax = s[1][0] - s[0][0], ay = s[1][1] - s[0][1];
            const float bx = s[2][0] - s[0][0], by = s[2][1] - s[0][1];
            const float cx = s[2][0] - s[1][0], cy = s[2][1] - s[1][1];
            const float ex = s[3][0] - s[1][0], ey = s[3][1] - s[1][1];
            const float near = ax * by - bx * ay;
            const float far = cx * ey - ex * cy;
            hidden = hidden || near * far < 0.0f;
        }
        const float alpha = std::fmin(std::fmax(fadeSlope * w1 + fadeBase, 0.0f), 1.0f);
        const float h = 0.5f * alpha;
        putWords(packet, 16u, 16u, 16u, static_cast<uint32_t>(gsNativeFtoi(alpha * 64.0f, 1.0f)));
        // MFIR sign-extends ADC's 0x8000 into the word (0xffff8000, an
        // exponent of 255), and adding the offset's 0 clamps that to -Fmax,
        // as the VU does: ADC, and fog 0xff.
        const uint32_t adc = hidden ? 0xff7fffffu : 0u;
        const float dx[8] = {h, h, -h, -h, 0.0f, 0.0f, 0.0f, 0.0f};
        const float dy[8] = {0.0f, 0.0f, 0.0f, 0.0f, -h, -h, h, h};
        for (uint32_t v = 0; v < 8u; ++v)
        {
            const float *end = s[first + (v & 1u)];
            putWords(packet, static_cast<uint32_t>(gsNativeFtoi(end[0] + dx[v], 16.0f)),
                     static_cast<uint32_t>(gsNativeFtoi(end[1] + dy[v], 16.0f)),
                     static_cast<uint32_t>(gsNativeFtoi(end[2] + 0.0f, 16.0f)), adc);
        }
    }
}

// The GIF packet at quad `q` as XGKICK sends it: tags and their data up to
// the one with EOP.
std::vector<uint8_t> gifPacket(const VuData &vu, uint32_t q)
{
    std::vector<uint8_t> packet;
    for (uint32_t tags = 0; tags < 64u; ++tags)
    {
        const uint32_t lo = static_cast<uint32_t>(vu.word(q, 0)), hi = static_cast<uint32_t>(vu.word(q, 1));
        const uint32_t loops = lo & 0x7FFFu, flag = (hi >> 26) & 3u;
        const uint32_t regs = ((hi >> 28) & 0xFu) == 0u ? 16u : (hi >> 28) & 0xFu;
        const uint32_t quads = flag == 0u ? loops * regs : flag == 1u ? (loops * regs + 1u) / 2u : loops;
        for (uint32_t i = 0; i <= quads; ++i)
        {
            putQuad(packet, vu.bytes(q + i));
        }
        q += quads + 1u;
        if (lo & 0x8000u)
        {
            break;
        }
    }
    return packet;
}

// out = rows * v as the VU's ACC sums it: ((r0 v.x + r1 v.y) + r2 v.z) + r3 v.w.
Vec4 transform(const Vec4 rows[4], const Vec4 &v)
{
    const float *r0 = &rows[0].x, *r1 = &rows[1].x, *r2 = &rows[2].x, *r3 = &rows[3].x;
    float out[4];
    for (int i = 0; i < 4; ++i)
    {
        out[i] = r0[i] * v.x + r1[i] * v.y + r2[i] * v.z + r3[i] * v.w;
    }
    return {out[0], out[1], out[2], out[3]};
}

bool clipsOwnW(const Vec4 &v)
{
    const float w = std::fabs(v.w);
    return v.x > w || v.x < -w || v.y > w || v.y < -w || v.z > w || v.z < -w;
}

// The z of OPMULA a, b then OPMSUB b, a: a.x b.y - b.x a.y.
float crossZ(const Vec4 &a, const Vec4 &b)
{
    return a.x * b.y - b.x * a.y;
}

Vec4 sub(const Vec4 &a, const Vec4 &b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z, 0.0f};
}

// The shadow volume C. The chunk's points come in threes, a triangle of
// the caster. Its world normal (quads 60-63 the model matrix) against the
// light (quad 34) must exceed 1e-4, else nothing is drawn. Its corners
// (front) and the same pushed away from the light (back: the model matrix
// times quads 30-33) go through the view (quads 8-11 times 24-27, x and y
// scaled by 16) and, unless any falls outside its own w, through the
// projection (quads 12-15, 12.x by 16.z and 13.y by 16.w) and the divide.
// It draws the volume as five fans of four: three sides, the front cap and
// the back cap, each after the A+D packet at quad 38 if it faces the screen
// (its z cross product is positive), at quad 35 if not (the blend that adds
// or takes away).
void buildShadow(const Program &program, const VuData &vu, uint32_t top,
                 std::vector<std::vector<uint8_t>> &packets)
{
    packets.clear();
    const uint32_t count = vu.half(top, 1);
    const uint32_t bones = program.decode == Decode::Skinned ? bonesOf(program, vu, top) : 0u;
    Vec4 model[4], light[4], view[4], projection[4], away[4], camera[4];
    for (uint32_t i = 0; i < 4u; ++i)
    {
        model[i] = vu.quad(60u + i);
        away[i] = vu.quad(30u + i);
        camera[i] = vu.quad(8u + i);
    }
    const Vec4 viewport = vu.quad(16);
    for (uint32_t i = 0; i < 4u; ++i)
    {
        light[i] = transform(away, model[i]);
        view[i] = transform(camera, vu.quad(24u + i));
        view[i].x *= viewport.x;
        view[i].y *= viewport.y;
        projection[i] = vu.quad(12u + i);
    }
    projection[0].x *= viewport.z;
    projection[1].y *= viewport.w;
    const Vec4 toLight = vu.quad(34);
    const float lit = fromBits(0x38d1b717u); // 1e-4

    uint8_t header[4][16];
    const uint32_t prim[4] = {5u, 0u, 0u, 0u};
    const uint32_t prmode[4] = {vu.half(87, 0), 0u, 27u, 0u};
    uint32_t tag[4];
    std::memcpy(tag, vu.bytes(86), sizeof(tag));
    tag[0] = 0x8004u;
    std::memcpy(header[0], vu.bytes(0), 16u);
    std::memcpy(header[1], prim, 16u);
    std::memcpy(header[2], prmode, 16u);
    std::memcpy(header[3], tag, 16u);
    const std::vector<uint8_t> facing = gifPacket(vu, 38), backing = gifPacket(vu, 35);

    for (uint32_t first = 0; first < count; first += 3u)
    {
        Vec4 world[3], pushed[3];
        for (uint32_t j = 0; j < 3u; ++j)
        {
            float p[3];
            decodePosition(vu, top, first + j, bones, p);
            const Vec4 point{p[0], p[1], p[2], 1.0f};
            world[j] = transform(model, point);
            pushed[j] = transform(light, point);
        }
        const Vec4 a = sub(world[1], world[0]), b = sub(world[2], world[0]);
        const float nx = a.y * b.z - b.y * a.z, ny = a.z * b.x - b.z * a.x, nz = a.x * b.y - b.x * a.y;
        if (!(toLight.x * nx + toLight.y * ny + toLight.z * nz > lit))
        {
            continue;
        }
        Vec4 corners[6];
        bool outside = false;
        for (uint32_t j = 0; j < 3u; ++j)
        {
            corners[j] = transform(view, world[j]);
            corners[3u + j] = transform(view, pushed[j]);
        }
        for (const Vec4 &corner : corners)
        {
            outside = outside || clipsOwnW(corner);
        }
        if (outside)
        {
            continue;
        }
        Vec4 screen[6];
        uint32_t fixed[6][4];
        for (uint32_t j = 0; j < 6u; ++j)
        {
            Vec4 &s = screen[j] = transform(projection, corners[j]);
            const float q = gsNativeReciprocal(s.w);
            s.x *= q;
            s.y *= q;
            s.z *= q;
            const float lanes[4] = {s.x, s.y, s.z, s.w};
            for (int i = 0; i < 4; ++i)
            {
                fixed[j][i] = static_cast<uint32_t>(gsNativeFtoi(lanes[i], 16.0f));
            }
        }
        // The fans: front 0-2, back 3-5, and the corners each takes, and
        // the two edges whose cross product says which way it faces.
        constexpr uint8_t kFans[5][4] = {{0, 3, 4, 1}, {1, 4, 5, 2}, {2, 5, 3, 0}, {0, 1, 2, 2}, {5, 4, 3, 3}};
        constexpr uint8_t kFacing[5][3] = {{0, 3, 4}, {1, 4, 5}, {2, 5, 3}, {0, 1, 2}, {5, 4, 3}};
        for (uint32_t f = 0; f < 5u; ++f)
        {
            const Vec4 &origin = screen[kFacing[f][0]];
            const float z = crossZ(sub(screen[kFacing[f][1]], origin), sub(screen[kFacing[f][2]], origin)) * 1.0f;
            packets.push_back(z > 0.0f ? facing : backing);
            std::vector<uint8_t> &fan = packets.emplace_back();
            fan.reserve(128u);
            for (const auto &quad : header)
            {
                putQuad(fan, quad);
            }
            for (uint32_t v = 0; v < 4u; ++v)
            {
                putQuad(fan, fixed[kFans[f][v]]);
            }
        }
    }
}

// The base piece (12ad73d2), run by an MSCAL at 0x000 before each object's
// chunks: it kicks the object's state packet at quad 19, then leaves in VU1
// memory what the strip programs read. With M the model rows (quads 60-63)
// and W = quads 24-27 times each: 68-71 = quads 4-7 times W (to the GS
// window), 72-75 = quads 8-11 times W (to clip space); 64-66 = the model
// rows 60-62 normalized (ERLENG: 1 / sqrt(x x + y y + z z), as the
// runtime's EFU has it), their w kept; 100-102 = the lights (quads 30-32)
// times those; 111.z = 1, or -1 when the model rows' determinant is
// negative (the winding of the strips' backface test).
void runBase(uint8_t *data, std::vector<std::vector<uint8_t>> *packets)
{
    const VuData vu(data);
    if (packets)
    {
        packets->push_back(gifPacket(vu, 19));
    }
    const auto store = [data](uint32_t q, const Vec4 &v) { std::memcpy(data + q * 16u, &v, 16u); };
    Vec4 model[4], view[4], window[4], clip[4], lights[3];
    for (uint32_t i = 0; i < 4u; ++i)
    {
        model[i] = vu.quad(60u + i);
        view[i] = vu.quad(24u + i);
        window[i] = vu.quad(4u + i);
        clip[i] = vu.quad(8u + i);
    }
    for (uint32_t i = 0; i < 3u; ++i)
    {
        lights[i] = vu.quad(30u + i);
    }
    const Vec4 &r0 = model[0], &r1 = model[1], &r2 = model[2];
    const float cx = r0.y * r1.z - r1.y * r0.z, cy = r0.z * r1.x - r1.z * r0.x, cz = r0.x * r1.y - r1.x * r0.y;
    const float determinant = cx * r2.x + cy * r2.y + cz * r2.z;
    for (uint32_t i = 0; i < 4u; ++i)
    {
        const Vec4 w = transform(view, model[i]);
        store(68u + i, transform(window, w));
        store(72u + i, transform(clip, w));
    }
    for (uint32_t i = 0; i < 3u; ++i)
    {
        const Vec4 &r = model[i];
        const float length = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
        const float scale = length != 0.0f ? 1.0f / length : length;
        const Vec4 unit{r.x * scale, r.y * scale, r.z * scale, r.w};
        store(64u + i, unit);
        const Vec4 v{unit.x, unit.y, unit.z, 0.0f};
        Vec4 light;
        float *out = &light.x;
        const float *l0 = &lights[0].x, *l1 = &lights[1].x, *l2 = &lights[2].x;
        for (int lane = 0; lane < 4; ++lane)
        {
            out[lane] = l0[lane] * v.x + l1[lane] * v.y + l2[lane] * v.z;
        }
        store(100u + i, light);
    }
    const float sign = determinant < 0.0f ? -1.0f : 1.0f;
    std::memcpy(data + 111u * 16u + 8u, &sign, 4u);
}

// The particle D (8ecadb3a). Per particle: a position (two quads: xyz, then
// x and y scales and an angle), a color (floats, times 128, within 0-128),
// two corner offsets and two UVs (xy of two quads each). The quad faces the
// eye (quad 28): its axes are e x (0, 1, 0) and e x that, normalized (e
// from the particle to the eye), scaled, and turned by the angle, whose
// cosine and sine come from a polynomial in a triangle wave of the angle /
// 2 pi. Its corners (offset x, offset y) go through the window matrix
// (quads 12-15 times 8-11 times 24-27) and the divide, and through clip
// space (8-11 times 24-27, x and y times 16.xy): one corner outside its w
// sets ADC on all four, else the XYZF2 word w is 0x7fff (fog 0xff). Out:
// the FOGCOL packet, then the header and per particle PRIM 4 (its low word
// only: the rest of the quad is the buffer's) and four vertices of ST,
// RGBA, XYZF2.
float particleCosine(float angle)
{
    const float p = std::fabs(angle) * fromBits(0xbe22f987u); // -1 / 2 pi
    const float magic = fromBits(0x4b400000u);               // 1.5 * 2^23
    const float fraction = ((p - magic) + magic) - p;
    const float t = std::fabs(fraction - 0.5f) - 0.25f;
    const float t2 = t * t, t4 = t2 * t2, t8 = t4 * t4;
    const float a3 = t * fromBits(0xc2255de0u) * t2;
    const float a7 = t * fromBits(0xc2992661u) * t2 * t4;
    const float a5 = t * fromBits(0x42a33457u) * t4;
    const float a1 = t * fromBits(0x40c90fdau);
    const float a9 = t * fromBits(0x421ed7b7u) * t8;
    return a3 + a7 + a5 + a1 + a9;
}

// 1 / sqrt(|x|), as the runtime's RSQRT gives it (the most float at 0).
float vuReciprocalRoot(float x)
{
    const float root = std::sqrt(std::fabs(x));
    return root != 0.0f ? 1.0f / root : std::numeric_limits<float>::max();
}

// 1 / x, as the runtime's DIV gives it.
float vuReciprocal(float x)
{
    return gsNativeReciprocal(x);
}

void buildParticles(const VuData &vu, uint32_t top, std::vector<std::vector<uint8_t>> &packets)
{
    packets.clear();
    std::vector<uint8_t> &fog = packets.emplace_back();
    uint32_t fogTag[4];
    std::memcpy(fogTag, vu.bytes(19), sizeof(fogTag));
    putWords(fog, 0x8001u, fogTag[1], fogTag[2], fogTag[3]);
    putQuad(fog, vu.bytes(41));
    if (vu.half(87, 1) & 0x10u)
    {
        std::memset(fog.data() + 16u, 0, 4u);
    }

    const uint32_t count = vu.half(top, 1);
    const uint32_t positions = top + 2u;
    const uint32_t offsets = positions + vu.half(top, 3);
    const uint32_t uvs = positions + vu.half(top + 1u, 0);
    const uint32_t colors = positions + vu.half(top + 1u, 1);
    // Where A puts the output (TOP + 76.x), the vertices 4 quads on.
    const uint32_t output = top + vu.half(76, 0) + 4u;
    std::vector<uint8_t> &packet = packets.emplace_back();
    packet.reserve(64u + count * 208u);
    putQuad(packet, vu.bytes(0));
    putWords(packet, 4u, 0u, 0u, 0u);
    putWords(packet, vu.half(87, 0), 0u, 27u, 0u);
    uint32_t tag[4];
    std::memcpy(tag, vu.bytes(86), sizeof(tag));
    tag[0] = vu.half(top, 2);
    putQuad(packet, tag);

    Vec4 camera[4], window[4], clip[4], model[4];
    for (uint32_t i = 0; i < 4u; ++i)
    {
        camera[i] = vu.quad(8u + i);
        model[i] = vu.quad(60u + i);
    }
    for (uint32_t i = 0; i < 4u; ++i)
    {
        window[i] = vu.quad(12u + i);
    }
    const Vec4 viewport = vu.quad(16);
    Vec4 rows[4];
    for (uint32_t i = 0; i < 4u; ++i)
    {
        clip[i] = transform(camera, vu.quad(24u + i));
        rows[i] = transform(window, clip[i]);
        clip[i].x *= viewport.x;
        clip[i].y *= viewport.y;
    }
    const Vec4 eye = vu.quad(28);
    const float halfPi = fromBits(0x3fc90fd8u);

    for (uint32_t i = 0; i < count; ++i)
    {
        const Vec4 p = vu.quad(positions + 2u * i), shape = vu.quad(positions + 2u * i + 1u);
        const Vec4 world = transform(model, {p.x, p.y, p.z, 1.0f});
        const float ex = eye.x - world.x, ey = eye.y - world.y, ez = eye.z - world.z;
        const float rx = 0.0f + ez, ry = 0.0f, rz = 0.0f - ex;
        const float rr = rx * rx + ry * ry + rz * rz;
        const float ux = ey * rz - ry * ez, uy = ez * rx - rz * ex, uz = ex * ry - rx * ey;
        const float uu = ux * ux + uy * uy + uz * uz;
        const float rq = vuReciprocalRoot(rr), uq = vuReciprocalRoot(uu);
        const float rs[3] = {rx * rq * shape.x, ry * rq * shape.x, rz * rq * shape.x};
        const float us[3] = {ux * uq * shape.y, uy * uq * shape.y, uz * uq * shape.y};
        const float c = 0.0f - particleCosine(shape.z - halfPi), s = particleCosine(shape.z);
        float a[3], b[3];
        for (int l = 0; l < 3; ++l)
        {
            a[l] = rs[l] * s - us[l] * c;
            b[l] = rs[l] * c + us[l] * s;
        }
        const Vec4 o0 = vu.quad(offsets + 2u * i), o1 = vu.quad(offsets + 2u * i + 1u);
        const Vec4 uv0 = vu.quad(uvs + 2u * i), uv1 = vu.quad(uvs + 2u * i + 1u);
        const float corners[4][2] = {{o0.x, o0.y}, {o1.x, o0.y}, {o0.x, o1.y}, {o1.x, o1.y}};
        const float texels[4][2] = {{uv0.x, uv0.y}, {uv1.x, uv0.y}, {uv0.x, uv1.y}, {uv1.x, uv1.y}};

        const Vec4 color = vu.quad(colors + i);
        const float lanes[4] = {color.x, color.y, color.z, color.w};
        uint32_t rgba[4];
        for (int l = 0; l < 4; ++l)
        {
            rgba[l] = static_cast<uint32_t>(
                gsNativeFtoi(std::fmax(std::fmin(lanes[l] * 128.0f, 128.0f), 0.0f), 1.0f));
        }
        uint32_t st[4][4], xyz[4][4];
        bool outside = false;
        for (uint32_t v = 0; v < 4u; ++v)
        {
            float k[3];
            for (int l = 0; l < 3; ++l)
            {
                k[l] = b[l] * corners[v][1] + (&world.x)[l] + a[l] * corners[v][0];
            }
            const Vec4 corner{k[0], k[1], k[2], 1.0f};
            const Vec4 screen = transform(rows, corner);
            outside = outside || clipsOwnW(transform(clip, corner));
            const float q = vuReciprocal(screen.w);
            const float stq[4] = {texels[v][0] * q, texels[v][1] * q, 1.0f * q, 1.0f * q};
            std::memcpy(st[v], stq, sizeof(stq));
            xyz[v][0] = static_cast<uint32_t>(gsNativeFtoi(screen.x * q, 16.0f));
            xyz[v][1] = static_cast<uint32_t>(gsNativeFtoi(screen.y * q, 16.0f));
            xyz[v][2] = static_cast<uint32_t>(gsNativeFtoi(screen.z * q, 16.0f));
        }
        const uint32_t adc = outside ? 0x8000u : 0x7fffu;
        // PRIM: ISW writes the low word only; the rest is what the buffer held.
        uint32_t prim[4];
        std::memcpy(prim, vu.bytes(output + 13u * i), sizeof(prim));
        prim[0] = 4u;
        putQuad(packet, prim);
        for (uint32_t v = 0; v < 4u; ++v)
        {
            xyz[v][3] = adc;
            putQuad(packet, st[v]);
            putQuad(packet, rgba);
            putQuad(packet, xyz[v]);
        }
    }
}

// The projected texture program. F: each vertex's UV is its position through
// quads 30-33 times the model rows (60-63; x, y and w lanes), divided by w,
// and its color (128, 128, 128, 82.w * 81.w * 128), unclamped. C, a strip
// D of its own: the transform (68-71), the divide, z times 87.z at most 1.y,
// fog; vertex j's ADC from the guard band of j and of the two before it
// (the origin before the first; the earlier two tested with their scaled
// z), and the triangle's facing, skipped when cross * winding > 0. The
// XYZF2 word w is then its low half with ADC: the fog's for every vertex
// but the first, whose low half is what the buffer held.
void buildProjector(const Program &program, const VuData &vu, uint32_t top,
                    std::vector<std::vector<uint8_t>> &packets)
{
    Chunk chunk;
    readChunk(program, vu, top, chunk);
    packets.clear();
    packets.push_back(std::move(chunk.fog));
    std::vector<uint8_t> &packet = packets.emplace_back();
    const uint32_t count = vu.half(top, 1);
    packet.reserve(64u + count * 48u);
    for (const auto &quad : chunk.header)
    {
        putQuad(packet, quad);
    }
    Vec4 model[4], projector[4], window[4];
    for (uint32_t i = 0; i < 4u; ++i)
    {
        model[i] = vu.quad(60u + i);
        projector[i] = vu.quad(30u + i);
        window[i] = vu.quad(68u + i);
    }
    Vec4 texture[4];
    for (uint32_t i = 0; i < 4u; ++i)
    {
        texture[i] = transform(projector, model[i]);
    }
    const uint32_t rgba[4] = {128u, 128u, 128u,
                              static_cast<uint32_t>(gsNativeFtoi(vu.quad(82).w * vu.quad(81).w * 128.0f, 1.0f))};
    const Vec4 fog = vu.quad(40), viewport = vu.quad(16), band = vu.quad(17), offset = vu.quad(18);
    const float bandScale[3] = {band.x * viewport.x, band.y * viewport.y, band.z};
    const float bandOffset[3] = {offset.x * bandScale[0], offset.y * bandScale[1], offset.z * bandScale[2]};
    const auto outside = [&](const float p[3]) {
        bool out = false;
        for (int i = 0; i < 3; ++i)
        {
            const float g = p[i] * bandScale[i] + bandOffset[i];
            out = out || g > 1.0f || g < -1.0f;
        }
        return out;
    };
    float winding = vu.quad(111).z * ((vu.half(87, 1) & 0x1000u) ? -1.0f : 1.0f);
    const float depthScale = vu.quad(87).z, depthMax = vu.quad(1).y;
    const uint32_t output = top + vu.half(76, 0) + 4u;
    const uint32_t bones = program.decode == Decode::Skinned ? bonesOf(program, vu, top) : 0u;
    // The two vertices before (scaled z), and whether each is outside.
    float before[2][3] = {};
    bool outBefore[2] = {outside(before[0]), outside(before[1])};
    for (uint32_t j = 0; j < count; ++j)
    {
        float position[3];
        decodePosition(vu, top, j, bones, position);
        const Vec4 point{position[0], position[1], position[2], 1.0f};
        const Vec4 t = transform(texture, point);
        const float tq = vuReciprocal(t.w);
        const float u = t.x * tq, v = t.y * tq;
        const Vec4 c = transform(window, point);
        const float q = vuReciprocal(c.w);
        const float raw[3] = {c.x * q, c.y * q, c.z * q};
        const float scaled[3] = {raw[0], raw[1], std::fmin(raw[2] * depthScale, depthMax)};
        const float f = std::fmax(std::fmin(fog.x * 1.0f + fog.w * c.w, fog.y), fog.z);
        const float ax = before[1][0] - before[0][0], ay = before[1][1] - before[0][1];
        const float bx = scaled[0] - before[1][0], by = scaled[1] - before[1][1];
        if (j != 0u)
        {
            winding = 0.0f - winding;
        }
        const float facing = (ax * by - bx * ay) * winding;
        const bool hidden = outside(raw) || outBefore[0] || outBefore[1] || facing > 0.0f;
        const uint32_t fogWord = static_cast<uint32_t>(gsNativeFtoi(f, 16.0f));
        const uint32_t low = j == 0u ? static_cast<uint32_t>(vu.word(output + 2u, 3)) : fogWord;
        const uint32_t w = (low & 0xFFFFu) | (hidden ? 0x8000u : 0u);
        const float stq[4] = {u * q, v * q, 1.0f * q, 0.0f};
        putQuad(packet, stq);
        putQuad(packet, rgba);
        putWords(packet, static_cast<uint32_t>(gsNativeFtoi(scaled[0], 16.0f)),
                 static_cast<uint32_t>(gsNativeFtoi(scaled[1], 16.0f)),
                 static_cast<uint32_t>(gsNativeFtoi(scaled[2], 16.0f)), w);
        std::memcpy(before[0], before[1], sizeof(before[0]));
        std::memcpy(before[1], scaled, sizeof(scaled));
        outBefore[0] = outBefore[1];
        outBefore[1] = outside(scaled);
    }
}

// The overlay B (6e42dac6), run by its own MSCAL: screen sprites over the
// bounds the last depth-and-bounds program left in quads 98 and 99, one
// pixel wider each way, within the rectangle of quads 140 and 141, in
// columns of 32 pixels. Each column is the GIF tag of quad 144 (two
// sprites' UV, XYZF2: the corners plus quad 142, plus 143.xy) in one of two
// buffers, 145 and 165, kicked in turn; then 144, made an empty EOP tag, is
// kicked to end. Nothing is kicked when the box is empty. It runs on `data`
// as the VU1 would, leaving what it leaves there, and appends its packets
// to `packets` if given.
void runOverlay(uint8_t *data, std::vector<std::vector<uint8_t>> *packets)
{
    const VuData vu(data);
    const auto store = [data](uint32_t q, const void *bytes) { std::memcpy(data + q * 16u, bytes, 16u); };
    std::memcpy(data + 145u * 16u, data + 144u * 16u, 16u);
    std::memcpy(data + 165u * 16u, data + 144u * 16u, 16u);
    const uint32_t end = 0x8000u;
    std::memcpy(data + 144u * 16u, &end, 4u);

    const Vec4 lo = vu.quad(98), hi = vu.quad(99), rectLo = vu.quad(140), rectHi = vu.quad(141);
    const Vec4 a{std::fmax(rectLo.x, lo.x - 1.0f), std::fmax(rectLo.y, lo.y - 1.0f), std::fmax(rectLo.z, lo.z), 0.0f};
    const Vec4 b{std::fmin(rectHi.x, hi.x + 1.0f), std::fmin(rectHi.y, hi.y + 1.0f), std::fmin(rectHi.z, hi.z), 0.0f};
    if (b.x - a.x < 0.0f || b.y - a.y < 0.0f)
    {
        return;
    }
    const Vec4 offset = vu.quad(142);
    const Vec4 xyOffset{vu.quad(143).x, vu.quad(143).y, 0.0f, 0.0f};
    float left = (b.x - a.x) - 1.0f;
    Vec4 from = a;
    uint32_t tag = 145u, other = 165u;
    for (;;)
    {
        const Vec4 to{std::fmin(from.x + 32.0f, b.x), b.y, b.z, b.w};
        const Vec4 corners[4] = {{from.x + offset.x, from.y + offset.y, from.z + offset.z, from.w + offset.w},
                                 {from.x + xyOffset.x, from.y + xyOffset.y, from.z + xyOffset.z, from.w + xyOffset.w},
                                 {to.x + offset.x, to.y + offset.y, to.z + offset.z, to.w + offset.w},
                                 {to.x + xyOffset.x, to.y + xyOffset.y, to.z + xyOffset.z, to.w + xyOffset.w}};
        for (uint32_t i = 0; i < 4u; ++i)
        {
            const uint32_t fixed[4] = {static_cast<uint32_t>(gsNativeFtoi(corners[i].x, 16.0f)),
                                       static_cast<uint32_t>(gsNativeFtoi(corners[i].y, 16.0f)),
                                       static_cast<uint32_t>(gsNativeFtoi(corners[i].z, 16.0f)),
                                       static_cast<uint32_t>(gsNativeFtoi(corners[i].w, 16.0f))};
            store(tag + 1u + i, fixed);
        }
        if (packets)
        {
            packets->push_back(gifPacket(vu, tag));
        }
        left -= 32.0f;
        from.x += 32.0f;
        std::swap(tag, other);
        if (left < 0.0f)
        {
            break;
        }
    }
    if (packets)
    {
        packets->push_back(gifPacket(vu, 144));
    }
}

} // namespace

const Program *identify(const uint8_t *code, uint32_t startPC)
{
    if (startPC == kBaseEntry && matches(code, kBase))
    {
        Program shape{nullptr, Cull::None, Color::Material, false, Decode::Plain, 0, Kind::Base};
        return intern(shape);
    }
    if (startPC == kOverlayEntry && matches(code, kOverlayB))
    {
        Program shape{nullptr, Cull::None, Color::Material, false, Decode::Plain, 0, Kind::Overlay};
        return intern(shape);
    }
    if (startPC != kEntry)
    {
        return nullptr;
    }
    if (matches(code, kProjectorA) && matches(code, kProjectorF) && matches(code, kProjectorC))
    {
        const DecodePiece *decode = present(code, kDecodes, &DecodePiece::b);
        if (!decode)
        {
            return nullptr;
        }
        Program shape{nullptr, Cull::GuardBandAndBackface, Color::Material, false, decode->decode,
                      decode->maxBones, Kind::Projector};
        return intern(shape);
    }
    if (matches(code, kParticleA) && matches(code, kParticleD))
    {
        Program shape{nullptr, Cull::None, Color::Material, false, Decode::Plain, 0, Kind::Particles};
        return intern(shape);
    }
    // The programs whose C draws by itself, by their A and C.
    constexpr struct
    {
        const Piece &a;
        const Piece &c;
        Kind kind;
    } kOwnDrawing[] = {{kOutlineA, kOutlineC, Kind::Outline}, {kShadowA, kShadowC, Kind::Shadow}};
    for (const auto &own : kOwnDrawing)
    {
        if (matches(code, own.a))
        {
            const DecodePiece *decode = present(code, kDecodes, &DecodePiece::b);
            if (!decode || !matches(code, own.c))
            {
                return nullptr;
            }
            Program shape{nullptr, Cull::None, Color::Material, false, decode->decode, decode->maxBones, own.kind};
            return intern(shape);
        }
    }
    for (const Piece &piece : kCommon)
    {
        if (!matches(code, piece))
        {
            return nullptr;
        }
    }
    const DecodePiece *decode = present(code, kDecodes, &DecodePiece::b);
    const ColorPiece *color = present(code, kColors, &ColorPiece::c);
    const Finish *finish = present(code, kFinishes, &Finish::d);
    const UvPiece *uv = present(code, kUvs, &UvPiece::f);
    if (!decode || !color || !finish || !uv)
    {
        return nullptr;
    }
    Program shape{nullptr,          finish->cull,     color->color, finish->depthBounds,
                  decode->decode,   decode->maxBones, Kind::Strip,  uv->uv};
    return intern(shape);
}

std::string describePieces(const uint8_t *code, uint32_t startPC)
{
    if (const Program *program = identify(code, startPC); program && program->kind != Kind::Strip)
    {
        return std::string("all: ") + program->name;
    }
    if (startPC != kEntry)
    {
        return "-";
    }
    std::string out;
    for (size_t i = 0; i < std::size(kCommon); ++i)
    {
        out += matches(code, kCommon[i]) ? kCommonNames[i] : "-";
        out += ' ';
    }
    const DecodePiece *decode = present(code, kDecodes, &DecodePiece::b);
    out += !decode                             ? "B:? "
           : decode->decode == Decode::Skinned ? "B:skinned "
                                               : "B:plain ";
    const UvPiece *uv = present(code, kUvs, &UvPiece::f);
    out += !uv ? "F:? " : uv->uv == Uv::SphereMap ? "F:sphere map " : "F:scroll ";
    const char *color = "C:?";
    for (const ColorPiece &candidate : kColors)
    {
        if (matches(code, candidate.c))
        {
            color = candidate.color == Color::PerVertex   ? "C:vertex colors"
                    : candidate.color == Color::Lit       ? "C:lit"
                    : candidate.color == Color::LitVertex ? "C:lit vertex colors"
                                                          : "C:one color";
        }
    }
    const char *finish = "D:?";
    for (const Finish &candidate : kFinishes)
    {
        if (matches(code, candidate.d))
        {
            finish = candidate.depthBounds && candidate.cull == Cull::GuardBandAndBackface
                         ? "D:guard band and backfaces, depth and bounds"
                     : candidate.depthBounds && candidate.cull == Cull::GuardBand ? "D:guard band, depth and bounds"
                     : candidate.depthBounds                        ? "D:depth and bounds"
                     : candidate.cull == Cull::GuardBandAndBackface ? "D:guard band and backfaces"
                     : candidate.cull == Cull::GuardBand            ? "D:guard band"
                                                                    : "D:no culling";
        }
    }
    return out + color + " " + finish;
}

const char *declineChunk(const Program &program, const uint8_t *data, uint32_t top, bool clips)
{
    const VuData vu(data);
    if (program.kind == Kind::Overlay || program.kind == Kind::Base)
        return nullptr;
    if (program.decode == Decode::Plain && vu.half(76, 2) != 0u)
        return "quad 76.z set (an entry into B other than its first)";
    if (program.decode == Decode::Skinned && bonesOf(program, vu, top) == 0u)
        return "an entry into the skinning B not read";
    if (program.kind == Kind::Outline)
    {
        const uint32_t count = vu.half(top, 1);
        if (count == 0u || count % outlinePoints(vu, top) != 0u)
            return "an outline chunk not of whole edges";
        return nullptr;
    }
    if (program.kind == Kind::Shadow)
    {
        const uint32_t count = vu.half(top, 1);
        if (count == 0u || count % 3u != 0u)
            return "a shadow chunk not of whole triangles";
        return nullptr;
    }
    if (program.kind == Kind::Particles)
        return vu.half(top, 1) == 0u ? "no particles" : nullptr;
    if (program.kind == Kind::Projector)
    {
        if (vu.half(81, 0) & 2u)
            return "clipping (slot E)";
        if ((vu.half(top, 0) & 7u) == 3u)
            return "triangle list (C's own path)";
        return vu.half(top, 1) == 0u ? "no vertices" : nullptr;
    }
    if ((vu.half(81, 0) & 2u) && !clips)
        return "clipping (slot E)";
    // PRIM from the chunk header. Only the backface piece reads it, with a
    // path of its own for triangle lists; the others run lists through
    // their strip code, whose ADC window of three vertices lands on each
    // triangle.
    const uint32_t primType = vu.half(top, 0) & 7u;
    if (primType == 3u && program.cull == Cull::GuardBandAndBackface)
        return "triangle list (D's own path)";
    if (primType != 3u && primType != 4u)
        return "neither strip nor triangle list";
    const uint32_t flags = vu.half(87, 1);
    if ((flags & 0x100u) && (program.color == Color::PerVertex || program.color == Color::LitVertex))
        return "quad 87.y & 0x100 (C's other loop)";
    if (program.uv == Uv::Plain && (vu.quad(88).x != 0.0f || vu.quad(88).y != 0.0f))
        return "scrolling UVs (slot F)";
    return nullptr;
}

namespace
{

// The bounds a depth-and-bounds chunk leaves: quads 98 and 99 as they were,
// widened lane by lane by each vertex's projected x, y, z and fog.
void boundsAfter(const Program &program, const uint8_t *data, uint32_t top, Vec4 &least, Vec4 &most)
{
    const VuData vu(data);
    least = vu.quad(98);
    most = vu.quad(99);
    Chunk chunk;
    readChunk(program, vu, top, chunk);
    for (const GSNativeMesh::Vertex &v : chunk.mesh.vertices)
    {
        const GSNativeMeshPoint p = gsNativeMeshPoint(chunk.mesh.header, v);
        const float lanes[4] = {p.px, p.py, p.pz, p.pf};
        float *lo = &least.x, *hi = &most.x;
        for (int i = 0; i < 4; ++i)
        {
            lo[i] = std::fmin(lo[i], lanes[i]);
            hi[i] = std::fmax(hi[i], lanes[i]);
        }
    }
}

} // namespace

void applyMemoryEffects(const Program &program, uint8_t *data, uint32_t top)
{
    if (program.kind == Kind::Overlay)
    {
        const VuRounding rounding;
        runOverlay(data, nullptr);
        return;
    }
    if (program.kind == Kind::Base)
    {
        const VuRounding rounding;
        runBase(data, nullptr);
        return;
    }
    if (program.kind == Kind::Outline)
    {
        const VuRounding rounding;
        const uint32_t word = outlineLoopWord(VuData(data), top);
        std::memcpy(data + 1023u * 16u, &word, sizeof(word));
        return;
    }
    if (!program.depthBounds)
    {
        return;
    }
    const VuRounding rounding;
    Vec4 least, most;
    boundsAfter(program, data, top, least, most);
    std::memcpy(data + 98u * 16u, &least, sizeof(least));
    std::memcpy(data + 99u * 16u, &most, sizeof(most));
}

bool memoryEffectsMatch(const Program &program, const uint8_t *before, uint32_t top, const uint8_t *after)
{
    if (program.kind == Kind::Overlay)
    {
        const VuRounding rounding;
        std::vector<uint8_t> copy(before, before + 1024u * 16u);
        runOverlay(copy.data(), nullptr);
        return std::memcmp(copy.data() + 144u * 16u, after + 144u * 16u, 26u * 16u) == 0;
    }
    if (program.kind == Kind::Base)
    {
        const VuRounding rounding;
        std::vector<uint8_t> copy(before, before + 1024u * 16u);
        runBase(copy.data(), nullptr);
        const auto same = [&](uint32_t first, uint32_t quads) {
            return std::memcmp(copy.data() + first * 16u, after + first * 16u, quads * 16u) == 0;
        };
        return same(64u, 3u) && same(68u, 8u) && same(100u, 3u) &&
               std::memcmp(copy.data() + 111u * 16u + 8u, after + 111u * 16u + 8u, 4u) == 0;
    }
    if (program.kind == Kind::Outline)
    {
        const VuRounding rounding;
        return outlineLoopWord(VuData(before), top) == static_cast<uint32_t>(VuData(after).word(1023, 0));
    }
    if (!program.depthBounds)
    {
        return true;
    }
    const VuRounding rounding;
    Vec4 least, most;
    boundsAfter(program, before, top, least, most);
    const VuData vu(after);
    const Vec4 vuLeast = vu.quad(98), vuMost = vu.quad(99);
    const float *a = &least.x, *b = &vuLeast.x, *c = &most.x, *d = &vuMost.x;
    for (int i = 0; i < 4; ++i)
    {
        const float scale = std::fmax(1.0f, std::fmax(std::fabs(a[i]), std::fabs(c[i])));
        if (std::fabs(a[i] - b[i]) > 1e-4f * scale || std::fabs(c[i] - d[i]) > 1e-4f * scale)
        {
            return false;
        }
    }
    return true;
}

void buildChunk(const Program &program, const uint8_t *data, uint32_t top,
                std::vector<std::vector<uint8_t>> &packets)
{
    const VuRounding rounding;
    if (program.kind == Kind::Outline)
    {
        buildOutline(program, VuData(data), top, packets);
        return;
    }
    if (program.kind == Kind::Shadow)
    {
        buildShadow(program, VuData(data), top, packets);
        return;
    }
    if (program.kind == Kind::Overlay)
    {
        std::vector<uint8_t> copy(data, data + 1024u * 16u);
        packets.clear();
        runOverlay(copy.data(), &packets);
        return;
    }
    if (program.kind == Kind::Base)
    {
        packets.clear();
        packets.push_back(gifPacket(VuData(data), 19));
        return;
    }
    if (program.kind == Kind::Particles)
    {
        buildParticles(VuData(data), top, packets);
        return;
    }
    if (program.kind == Kind::Projector)
    {
        buildProjector(program, VuData(data), top, packets);
        return;
    }
    Chunk chunk;
    readChunk(program, VuData(data), top, chunk);
    packets.clear();
    packets.push_back(std::move(chunk.fog));

    // The output as D writes it: the header, then ST, RGBAQ and XYZF2 per
    // vertex, ADC as the program's culling says (ISW keeps only the low half
    // of the word it sets).
    const GSNativeMesh &mesh = chunk.mesh;
    const uint32_t count = static_cast<uint32_t>(mesh.vertices.size());
    std::vector<uint8_t> &packet = packets.emplace_back();
    packet.reserve(64u + count * 48u);
    for (const auto &quad : chunk.header)
    {
        putQuad(packet, quad);
    }
    std::vector<GSNativeMeshPoint> points(count);
    for (uint32_t k = 0; k < count; ++k)
    {
        const GSNativeMeshPoint &p = points[k] = gsNativeMeshPoint(mesh.header, mesh.vertices[k]);
        putFloats(packet, p.s, p.t, p.q, 0.0f);
        putWords(packet, static_cast<uint32_t>(p.rgba[0]), static_cast<uint32_t>(p.rgba[1]),
                 static_cast<uint32_t>(p.rgba[2]), static_cast<uint32_t>(p.rgba[3]));
        putWords(packet, static_cast<uint32_t>(p.x16), static_cast<uint32_t>(p.y16), static_cast<uint32_t>(p.z16),
                 static_cast<uint32_t>(p.f16));
    }
    if (program.cull != Cull::None)
    {
        // The culling pieces clear the clip flags, then set each vertex's
        // ADC from the last three judgments: the first vertex's from its
        // own. (It never draws; it is in the packet.)
        if (count != 0u && points[0].outside)
        {
            const uint32_t w = (static_cast<uint32_t>(points[0].f16) & 0xFFFFu) | 0x8000u;
            std::memcpy(packet.data() + 64u + 2u * 16u + 12u, &w, 4u);
        }
        float winding = mesh.header.winding;
        for (uint32_t k = 1; k < count; ++k)
        {
            const bool skip = gsNativeMeshSkips(mesh.header, points.data(), k, winding);
            winding = -winding;
            const uint32_t w = (static_cast<uint32_t>(points[k].f16) & 0xFFFFu) | (skip ? 0x8000u : 0u);
            std::memcpy(packet.data() + 64u + (k * 3u + 2u) * 16u + 12u, &w, 4u);
        }
    }
}

void buildMesh(const Program &program, const uint8_t *data, uint32_t top,
               std::vector<std::vector<uint8_t>> &packets, GSNativeMesh &mesh)
{
    if (!hasMesh(program))
    {
        buildChunk(program, data, top, packets);
        mesh.vertices.clear();
        mesh.header.vertexCount = 0u;
        return;
    }
    const VuRounding rounding;
    Chunk chunk;
    readChunk(program, VuData(data), top, chunk);
    packets.clear();
    packets.push_back(std::move(chunk.fog));
    // PRIM and PRMODE behind the program's GIF tag, made the packet's end.
    std::vector<uint8_t> &state = packets.emplace_back();
    uint32_t tag[4];
    std::memcpy(tag, chunk.header[0], sizeof(tag));
    tag[0] |= 0x8000u;
    putQuad(state, tag);
    putQuad(state, chunk.header[1]);
    putQuad(state, chunk.header[2]);
    // The Z exactly as the program has it: the GPU rounds to nearest.
    for (GSNativeMesh::Vertex &v : chunk.mesh.vertices)
    {
        v.depth = gsNativeMeshDepth(chunk.mesh.header, v);
    }
    mesh = std::move(chunk.mesh);
}

Mode modeFromEnvironment()
{
    const char *value = std::getenv("RG_VU1_NATIVE");
    if (!value || std::strcmp(value, "0") == 0)
        return Mode::Off;
    if (std::strcmp(value, "check") == 0)
        return Mode::Check;
    if (std::strcmp(value, "skip") == 0)
        return Mode::Skip;
    if (std::strcmp(value, "gpu") == 0)
        return Mode::Gpu;
    return Mode::Replace;
}

void Monitor::declined(const Program &program, const char *reason)
{
    ++m_declined[std::string(program.name) + ": " + reason];
}

const char *Monitor::check(const Program &program, const std::vector<std::vector<uint8_t>> &vu,
                           const std::vector<std::vector<uint8_t>> &native)
{
    const auto events = [](const std::vector<std::vector<uint8_t>> &packets) {
        std::vector<rg::gif::GifEvent> out;
        rg::gif::GifState state;
        for (const auto &packet : packets)
        {
            rg::gif::decodeGifPacket(packet.data(), static_cast<uint32_t>(packet.size()), state, out);
        }
        return out;
    };
    rg::gif::Tolerance tolerance;
    tolerance.xy = 1u;
    tolerance.z = 16u;
    tolerance.rgba = 1u;
    tolerance.stq = 1e-5f;
    const auto reference = events(vu);
    const auto candidate = events(native);
    const rg::gif::Comparison result = rg::gif::compare(reference, candidate, tolerance);
    ++m_checked;
    if (result.mismatched == 0u && result.countDifference == 0)
    {
        return nullptr;
    }
    ++m_differ;
    const char *kind = result.countDifference != 0     ? "vertex count"
                       : result.kindMismatches != 0u   ? "ADC or register"
                       : result.maxXy > tolerance.xy   ? "position"
                       : result.maxZ > tolerance.z     ? "Z"
                       : result.maxRgba > tolerance.rgba ? "color"
                       : result.maxUv > tolerance.uv   ? "UV"
                       : result.maxStq > tolerance.stq ? "STQ"
                                                       : "fog";
    ++m_kinds[std::string(program.name) + ": " + kind];
    if (m_shown++ < 5u)
    {
        const size_t at = static_cast<size_t>(result.firstMismatch);
        std::fprintf(stderr, "[vu1native] %s differs at event %zu of %zu (%+lld events):\n  vu1    %s\n  native %s\n",
                     program.name, at, reference.size(), static_cast<long long>(result.countDifference),
                     at < reference.size() ? rg::gif::describe(reference[at]).c_str() : "-",
                     at < candidate.size() ? rg::gif::describe(candidate[at]).c_str() : "-");
    }
    return kind;
}

void Monitor::tick()
{
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now < m_next)
    {
        return;
    }
    if (m_next == 0)
    {
        m_next = now + 2;
        return;
    }
    m_next = now + 2;
    std::string line;
    for (const auto &[name, count] : m_native)
    {
        line += " " + name + "=" + std::to_string(count);
    }
    std::string left;
    for (const auto &[reason, count] : m_declined)
    {
        left += " [" + reason + "]=" + std::to_string(count);
    }
    std::fprintf(stderr, "[vu1native] %s: native chunks%s; left to the VU1%s", 
                 m_mode == Mode::Check  ? "check"
                 : m_mode == Mode::Skip ? "skip"
                 : m_mode == Mode::Gpu  ? "gpu"
                                        : "replace",
                 line.empty() ? " none" : line.c_str(), left.empty() ? " none" : left.c_str());
    if (m_mode == Mode::Check)
    {
        std::fprintf(stderr, "; checked %llu, differ %llu", static_cast<unsigned long long>(m_checked),
                     static_cast<unsigned long long>(m_differ));
        for (const auto &[kind, count] : m_kinds)
        {
            std::fprintf(stderr, " [%s]=%llu", kind.c_str(), static_cast<unsigned long long>(count));
        }
    }
    std::fprintf(stderr, "\n");
}

} // namespace rg::vu1native
