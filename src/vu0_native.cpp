#include "vu0_native.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <xmmintrin.h>

#include "native_renderer.h"
#include "ps2_runtime.h"

// The game's one VU0 micro program, done natively (docs/VU1-NATIVE.md,
// "VU0"). Read from the code (tools/vu1/vudis.py over the image the main
// executable uploads by VIF0: pairs 0-111 from file offset 0x3459a0) and
// written pair by pair; no recording was taken. Every VCALLMS in the game
// is `vcallms 0` in MatFromSRTVu0 (0x13bc90) and MatRotateVu0 (0x13bc20).
//
// What it does: a matrix from scale vf1, rotation angles vf2 (x, y, z, in
// radians) and translation vf3, into vf4-vf7 (rows). It adds vf2 into vf0
// (a write the VU discards) only for the zero flags, and skips the
// rotations whose angles are zero: all three, x and y, or z. Sine and
// cosine are a polynomial after a range reduction (the 12582912 trick, in
// the VU's rounding toward zero).
//
// The arithmetic is the interpreter's (ps2_vu1_upper.inl): operands with a
// denormal as a signed zero and an infinity or NaN as the largest float,
// results the same, products and sums rounded apart, toward zero. The
// program is scheduled so that no instruction reads a result before the
// pipeline has it, so pair order is the order here. It leaves every
// register the program writes (vf4-vf7, vf10-vf16, ACC, I, vi1-vi4) as the
// interpreter does; not the MAC, status or clip flags (neither caller reads
// them).
#pragma STDC FENV_ACCESS ON

namespace rg::vu0native
{
namespace
{

constexpr uint32_t kProgramPairs = 113u; // 0-111 and the pair after the E bit
// FNV-1a 64 of those 904 bytes as the executable uploads them (the pair
// after the E bit, 112, is outside the upload: zero in a fresh VU0). Other
// code there, or another program, leaves the run to the VU0.
constexpr uint64_t kProgramHash = 0x0ee26b23d4d2eb69ull;
uint64_t g_seenGeneration = ~0ull;
bool g_matches = false;
uint64_t g_runs = 0u;

uint64_t fnv(const uint8_t *bytes, uint32_t size)
{
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

// Round toward zero while it lives, as VU1Interpreter::run sets MXCSR (it
// does no DAZ or FTZ: denormals are handled by the operand and result
// rules below).
class Rounding
{
public:
    Rounding() : m_saved(_mm_getcsr()) { _mm_setcsr((m_saved & ~0x6000u) | 0x6000u); }
    ~Rounding() { _mm_setcsr(m_saved); }

private:
    unsigned m_saved;
};

uint32_t bitsOf(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float fromBits(uint32_t bits)
{
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// normalizeOperand: a denormal is a zero of its sign, an infinity or NaN the
// largest float of its sign.
float in(float value)
{
    const uint32_t bits = bitsOf(value);
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    if (exponent == 0u)
        return fromBits(bits & 0x80000000u);
    if (exponent == 0xFFu)
        return fromBits((bits & 0x80000000u) | 0x7F7FFFFFu);
    return value;
}

// normalizeResult, without the flags.
float out(float value)
{
    return in(value);
}

// Destination masks, as the VU's dest field: x is the highest bit.
constexpr int X = 8, Y = 4, Z = 2, W = 1;
constexpr int XY = X | Y, XYZ = X | Y | Z, XYZW = 15, YZW = Y | Z | W, XZW = X | Z | W, XYW = X | Y | W,
              YW = Y | W, ZW = Z | W, XZ = X | Z;
constexpr int kX = 0, kY = 1, kZ = 2, kW = 3;

struct Vu
{
    float vf[32][4];
    float acc[4];
    float i;
    uint16_t vi[16];

    // Writes the lanes of `mask` (vf0 is never written: it stays 0, 0, 0, 1).
    void put(float *dst, int mask, const float r[4], bool normalize = true)
    {
        if (dst == vf[0])
            return;
        for (int c = 0; c < 4; ++c)
            if (mask & (8 >> c))
                dst[c] = normalize ? out(r[c]) : r[c];
    }
    // d = s + t[bc], or s + t lane by lane (bc < 0).
    void add(float *d, int mask, const float *s, const float *t, int bc = -1)
    {
        float r[4];
        for (int c = 0; c < 4; ++c)
            r[c] = in(s[c]) + in(t[bc < 0 ? c : bc]);
        put(d, mask, r);
    }
    void sub(float *d, int mask, const float *s, const float *t, int bc)
    {
        float r[4];
        for (int c = 0; c < 4; ++c)
            r[c] = in(s[c]) - in(t[bc]);
        put(d, mask, r);
    }
    void subI(float *d, int mask, const float *s)
    {
        float r[4];
        for (int c = 0; c < 4; ++c)
            r[c] = in(s[c]) - in(i);
        put(d, mask, r);
    }
    void mul(float *d, int mask, const float *s, const float *t, int bc = -1)
    {
        float r[4];
        for (int c = 0; c < 4; ++c)
            r[c] = in(s[c]) * in(t[bc < 0 ? c : bc]);
        put(d, mask, r);
    }
    void mulI(float *d, int mask, const float *s)
    {
        float r[4];
        for (int c = 0; c < 4; ++c)
            r[c] = in(s[c]) * in(i);
        put(d, mask, r);
    }
    // ACC = s * t (MULA), ACC + s * t (MADDA), ACC - s * t (MSUBA); into d
    // instead of ACC for MADD and MSUB.
    void productSum(float *d, int mask, const float *s, float t, int c, int sign, float r[4])
    {
        (void)d;
        (void)mask;
        const float product = in(s[c]) * t;
        r[c] = sign == 0 ? product : sign > 0 ? in(acc[c]) + product : in(acc[c]) - product;
    }
    void fma(float *d, int mask, const float *s, const float *t, int bc, int sign)
    {
        float r[4];
        for (int c = 0; c < 4; ++c)
            productSum(d, mask, s, in(t[bc < 0 ? c : bc]), c, sign, r);
        put(d, mask, r);
    }
    void fmaI(float *d, int mask, const float *s, int sign)
    {
        float r[4];
        for (int c = 0; c < 4; ++c)
            productSum(d, mask, s, in(i), c, sign, r);
        put(d, mask, r);
    }
    void abs(float *d, int mask, const float *s)
    {
        float r[4];
        for (int c = 0; c < 4; ++c)
            r[c] = std::fabs(in(s[c]));
        put(d, mask, r, false);
    }
    void max(float *d, int mask, const float *s, const float *t, int bc)
    {
        float r[4];
        const float b = in(t[bc]);
        for (int c = 0; c < 4; ++c)
            r[c] = in(s[c]) > b ? in(s[c]) : b;
        put(d, mask, r, false);
    }
};

// The sine and cosine block on the lanes of `m` of vf11 (angles, the cosine
// lanes shifted by -pi/2 first), with `abs` the register ABS writes and the
// polynomial's temporaries as the two copies name them (pairs 0x0b0-0x160
// and 0x240-0x2e8): result into `result`.
struct SinCos
{
    int m;
    int absReg, sq, p7, p3, p5, sq2, p9, x8, result;
};

void sinCos(Vu &u, const SinCos &k)
{
    float(&vf)[32][4] = u.vf;
    float *v11 = vf[11];
    // ABS vf_abs, vf11; MAXw vf11, vf0, vf0w (1, 1, 1, 1).
    u.abs(vf[k.absReg], k.m, v11);
    u.max(v11, k.m, vf[0], vf[0], kW);
    // MULAi ACC, abs, I(-1/2pi) | LOI 12582912
    u.fmaI(u.acc, k.m, vf[k.absReg], 0);
    u.i = 12582912.0f;
    // MSUBAi ACC, vf11, I; MADDAi ACC, vf11, I | LOI -1/2pi
    u.fmaI(u.acc, k.m, v11, -1);
    u.fmaI(u.acc, k.m, v11, +1);
    u.i = -0.1591549962759018f;
    // MSUBAi ACC, abs, I | LOI 0.5
    u.fmaI(u.acc, k.m, vf[k.absReg], -1);
    u.i = 0.5f;
    // MSUBi vf11, vf11, I | LOI 0.25
    u.fmaI(v11, k.m, v11, -1);
    u.i = 0.25f;
    // ABS vf11, vf11; SUBi vf11, vf11, I
    u.abs(v11, k.m, v11);
    u.subI(v11, k.m, v11);
}

void polynomial(Vu &u, const SinCos &k)
{
    float(&vf)[32][4] = u.vf;
    float *v11 = vf[11];
    // MUL sq, vf11, vf11 | LOI -76.57
    u.mul(vf[k.sq], k.m, v11, v11);
    u.i = -76.57495880126953f;
    // MULi p7, vf11, I | LOI -41.34
    u.mulI(vf[k.p7], k.m, v11);
    u.i = -41.3416748046875f;
    // MULi p3, vf11, I | LOI 81.60
    u.mulI(vf[k.p3], k.m, v11);
    u.i = 81.60222625732422f;
    // MULi p5, vf11, I
    u.mulI(vf[k.p5], k.m, v11);
    // MUL sq2, sq, sq; MUL p7, p7, sq
    u.mul(vf[k.sq2], k.m, vf[k.sq], vf[k.sq]);
    u.mul(vf[k.p7], k.m, vf[k.p7], vf[k.sq]);
    // MULA ACC, p3, sq | LOI 39.71
    u.fma(u.acc, k.m, vf[k.p3], vf[k.sq], -1, 0);
    u.i = 39.71065902709961f;
    // MULi p9, vf11, I (p9 is sq's register in the first copy)
    u.mulI(vf[k.p9], k.m, v11);
    // MUL x8, sq2, sq2
    u.mul(vf[k.x8], k.m, vf[k.sq2], vf[k.sq2]);
    // MADDA ACC, p7, sq2; MADDA ACC, p5, sq2 | LOI 2pi
    u.fma(u.acc, k.m, vf[k.p7], vf[k.sq2], -1, +1);
    u.fma(u.acc, k.m, vf[k.p5], vf[k.sq2], -1, +1);
    u.i = 6.283185005187988f;
    // MADDAi ACC, vf11, I; MADD result, p9, x8
    u.fmaI(u.acc, k.m, v11, +1);
    u.fma(vf[k.result], k.m, vf[k.p9], vf[k.x8], -1, +1);
}

// The program, from pair 0 to the E bit at 0x378.
void run(Vu &u)
{
    float(&vf)[32][4] = u.vf;
    uint16_t(&vi)[16] = u.vi;
    // 0x000-0x018: vf4-vf6 cleared but the diagonal's lanes, vf7 = (vf3, 1).
    u.mul(vf[4], YZW, vf[0], vf[0], kX);
    vf[7][3] = vf[0][3]; // MOVE.w vf7, vf0
    u.mul(vf[5], XZW, vf[0], vf[0], kX);
    vi[1] = 12;
    u.mul(vf[6], XYW, vf[0], vf[0], kX);
    vi[3] = 14;
    u.add(vf[7], XYZ, vf[0], vf[3]);
    vi[2] = 2;
    // 0x020-0x030: the scale on the diagonal.
    u.add(vf[4], X, vf[0], vf[1], kX);
    u.add(vf[5], Y, vf[0], vf[1], kY);
    u.add(vf[6], Z, vf[0], vf[1], kZ);
    // 0x038: ADDx.xyz vf0, vf2, vf0x: discarded, but its zero flags (MAC
    // bits 3, 2, 1 for x, y, z) are what the FMANDs read.
    uint16_t mac = 0u;
    for (int c = 0; c < 3; ++c)
    {
        const float r = in(vf[2][c]) + in(vf[0][0]);
        const uint32_t bits = bitsOf(r);
        const bool zero = (bits & 0x7FFFFFFFu) == 0u || ((bits >> 23) & 0xFFu) == 0u;
        if (zero)
            mac |= static_cast<uint16_t>(8 >> c);
        if (bits & 0x80000000u)
            mac |= static_cast<uint16_t>(0x80 >> c);
    }
    // 0x058-0x080: no rotation at all; x and y none; z none (in the second
    // branch's delay slot, so whichever way it goes).
    vi[4] = mac & vi[3];
    if (vi[4] == vi[3])
        return;
    vi[4] = mac & vi[1];
    const bool skipXY = vi[4] == vi[1];
    vi[3] = mac & vi[2];
    if (!skipXY)
    {
        // 0x088-0x0a8: vf11 = (x - pi/2, x, y - pi/2, y).
        u.add(vf[11], X, vf[0], vf[2], kX);
        u.add(vf[11], Z, vf[0], vf[2], kY);
        u.add(vf[11], Y, vf[0], vf[11], kX);
        u.mul(vf[11], W, vf[0], vf[11], kZ);
        u.i = 1.570796012878418f;
        u.subI(vf[11], XZ, vf[11]);
        u.i = -0.1591549962759018f;
        sinCos(u, {XYZW, 10, 0, 0, 0, 0, 0, 0, 0, 0});
        // 0x0f8: MULx.yw vf10, vf0, vf0x
        u.mul(vf[10], YW, vf[0], vf[0], kX);
        // 0x100-0x160 into vf13 = (cos x, sin x, cos y, sin y).
        polynomial(u, {XYZW, 0, 14, 16, 15, 12, 13, 14, 15, 13});
        // 0x168-0x198: the rotation rows.
        u.add(vf[11], Y, vf[0], vf[0], kW);
        u.mul(vf[11], XZW, vf[0], vf[0], kX);
        u.mul(vf[12], YW, vf[0], vf[0], kX);
        u.add(vf[12], X, vf[0], vf[13], kW);
        u.sub(vf[12], Z, vf[0], vf[13], kZ);
        u.add(vf[10], Z, vf[0], vf[13], kW);
        u.add(vf[10], X, vf[0], vf[13], kZ);
        // 0x1a0-0x210: rows 4-6 rotated about x, then y.
        u.mul(vf[13], Z, vf[6], vf[13], kX);
        u.fma(u.acc, XYZW, vf[12], vf[4], kX, 0);
        u.mul(vf[14], Y, vf[5], vf[13], kX);
        u.mul(vf[5], Y, vf[5], vf[13], kY);
        u.sub(vf[6], Y, vf[0], vf[13], kZ);
        u.fma(u.acc, XYZW, vf[11], vf[4], kY, +1);
        u.add(vf[5], Z, vf[0], vf[14], kY);
        u.mul(vf[6], Z, vf[6], vf[13], kY);
        u.fma(vf[4], XYZW, vf[10], vf[4], kZ, +1);
        u.fma(u.acc, XYZW, vf[12], vf[5], kX, 0);
        u.fma(u.acc, XYZW, vf[11], vf[5], kY, +1);
        u.fma(vf[5], XYZW, vf[10], vf[5], kZ, +1);
        u.fma(u.acc, XYZW, vf[12], vf[6], kX, 0);
        u.fma(u.acc, XYZW, vf[11], vf[6], kY, +1);
        u.fma(vf[6], XYZW, vf[10], vf[6], kZ, +1);
    }
    // 0x218: no z rotation.
    if (vi[3] != 0u)
        return;
    // 0x228-0x238: vf11.xy = (z - pi/2, z).
    u.add(vf[11], X, vf[0], vf[2], kZ);
    u.add(vf[11], Y, vf[0], vf[11], kX);
    u.i = 1.570796012878418f;
    u.subI(vf[11], X, vf[11]);
    u.i = -0.1591549962759018f;
    sinCos(u, {XY, 12, 0, 0, 0, 0, 0, 0, 0, 0});
    // 0x288-0x2e8 into vf11.xy = (cos z, sin z).
    polynomial(u, {XY, 0, 13, 15, 14, 12, 10, 13, 14, 11});
    // 0x2f0-0x328
    u.add(vf[10], Z, vf[0], vf[0], kW);
    u.mul(vf[11], ZW, vf[0], vf[0], kX);
    u.mul(vf[12], ZW, vf[0], vf[0], kX);
    u.add(vf[12], X, vf[0], vf[11], kY);
    u.add(vf[12], Y, vf[0], vf[11], kX);
    u.sub(vf[11], X, vf[0], vf[11], kX);
    u.add(vf[11], Y, vf[0], vf[11], kY);
    u.mul(vf[10], XYW, vf[0], vf[0], kX);
    // 0x330-0x370: rows 4-6 rotated about z.
    for (int row = 4; row <= 6; ++row)
    {
        u.fma(u.acc, XYZW, vf[12], vf[row], kX, 0);
        u.fma(u.acc, XYZW, vf[11], vf[row], kY, +1);
        u.fma(vf[row], XYZW, vf[10], vf[row], kZ, +1);
    }
}

bool hook(R5900Context *ctx, uint8_t *code, uint8_t *data, uint32_t startPC, uint64_t generation)
{
    (void)data;
    // Decided at first use, after the menu's native renderer setting.
    static const bool enabled = [] {
        const char *value = std::getenv("RG_VU0_NATIVE");
        const bool on = value && *value ? std::strcmp(value, "0") != 0 : rg::nativerenderer::enabled();
        std::fprintf(stderr, on ? "[rg] VU0's matrix program native (RG_VU0_NATIVE=0 leaves it to the VU0)\n"
                                : "[rg] VU0's matrix program on the VU0\n");
        return on;
    }();
    if (!enabled || startPC != 0u)
        return false;
    if (generation != g_seenGeneration)
    {
        g_seenGeneration = generation;
        g_matches = fnv(code, kProgramPairs * 8u) == kProgramHash;
    }
    if (!g_matches)
        return false;
    Vu u;
    std::memcpy(u.vf, ctx->vu0_vf, sizeof(u.vf));
    std::memcpy(u.acc, &ctx->vu0_acc, sizeof(u.acc));
    std::memcpy(&u.i, &ctx->vu0_i, sizeof(u.i));
    for (int r = 0; r < 16; ++r)
        u.vi[r] = static_cast<uint16_t>(ctx->vi[r]);
    u.vf[0][0] = u.vf[0][1] = u.vf[0][2] = 0.0f;
    u.vf[0][3] = 1.0f;
    {
        const Rounding rounding;
        run(u);
    }
    std::memcpy(ctx->vu0_vf, u.vf, sizeof(u.vf));
    std::memcpy(&ctx->vu0_acc, u.acc, sizeof(u.acc));
    std::memcpy(&ctx->vu0_i, &u.i, sizeof(u.i));
    for (int r = 1; r < 16; ++r)
        ctx->vi[r] = u.vi[r];
    ++g_runs;
    return true;
}

} // namespace

void install(PS2Runtime &runtime)
{
    // RG_VU0_NATIVE=0 leaves the program to the VU0; otherwise the hook
    // decides at its first call (on with the native renderer, or =1).
    const char *value = std::getenv("RG_VU0_NATIVE");
    if (value && std::strcmp(value, "0") == 0)
        return;
    runtime.setVu0MicroHook(&hook);
}

uint64_t runs()
{
    return g_runs;
}

void runProgram(float vf[32][4], float acc[4], float &i, uint16_t vi[16])
{
    Vu u;
    std::memcpy(u.vf, vf, sizeof(u.vf));
    std::memcpy(u.acc, acc, sizeof(u.acc));
    u.i = i;
    std::memcpy(u.vi, vi, sizeof(u.vi));
    {
        const Rounding rounding;
        run(u);
    }
    std::memcpy(vf, u.vf, sizeof(u.vf));
    std::memcpy(acc, u.acc, sizeof(u.acc));
    i = u.i;
    std::memcpy(vi, u.vi, sizeof(u.vi));
}

} // namespace rg::vu0native
