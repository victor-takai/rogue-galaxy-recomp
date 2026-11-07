#include "gfx/gs/gs_vram.h"

#include "runtime/gs/gs_types.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

// The run writers' column kernels use SSE2/SSSE3 wherever the build has them:
// every x64 build does (ps2_runtime adds -msse4.1) and arm64 gets them through
// sse2neon, as the rest of the runtime does. DQ8_GS_VRAM_SCALAR selects the
// portable kernels instead; they store the same bytes, more slowly.
#if !defined(DQ8_GS_VRAM_SCALAR) && (defined(__SSSE3__) || defined(USE_SSE2NEON))
#define DQ8_GS_VRAM_SIMD 1
#if defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <tmmintrin.h>
#endif
#else
#define DQ8_GS_VRAM_SIMD 0
#endif

namespace dq8::gfx {

namespace {

// GSMem's swizzle tables are computed once at runtime, not constant-initialised.
void ensureLookupTables() {
    static std::once_flag once;
    std::call_once(once, [] { GSMem::InitLookupTables(); });
}

// GSMem's block and column tables, copied from ps2_gs_memory.cpp. The run
// writers below address memory in closed form rather than through the
// megabyte-sized page tables GSMem builds from these; the block tables are
// still looked up (they are tiny) and the column tables stay so that the
// closed forms can be checked against them at compile time.
using P8Traits = GSMem::PixelStorageTraits<GSMem::P8>;
using P4Traits = GSMem::PixelStorageTraits<GSMem::P4>;
using C32Traits = GSMem::PixelStorageTraits<GSMem::C32>;
using Z32Traits = GSMem::PixelStorageTraits<GSMem::Z32>;

constexpr C32Traits::BlockLookupTableT kBlockTableC32{{
    {0, 1, 4, 5, 16, 17, 20, 21},
    {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29},
    {10, 11, 14, 15, 26, 27, 30, 31},
}};

constexpr Z32Traits::BlockLookupTableT kBlockTableZ32{{
    {24, 25, 28, 29, 8, 9, 12, 13},
    {26, 27, 30, 31, 10, 11, 14, 15},
    {16, 17, 20, 21, 0, 1, 4, 5},
    {18, 19, 22, 23, 2, 3, 6, 7},
}};

// Shared by C32 and Z32.
constexpr C32Traits::ColumnLookupTableT kColumnTable32{{
    {0, 1, 4, 5, 8, 9, 12, 13},
    {2, 3, 6, 7, 10, 11, 14, 15},
    {16, 17, 20, 21, 24, 25, 28, 29},
    {18, 19, 22, 23, 26, 27, 30, 31},
    {32, 33, 36, 37, 40, 41, 44, 45},
    {34, 35, 38, 39, 42, 43, 46, 47},
    {48, 49, 52, 53, 56, 57, 60, 61},
    {50, 51, 54, 55, 58, 59, 62, 63},
}};

constexpr P8Traits::BlockLookupTableT kBlockTableP8{{
    {0, 1, 4, 5, 16, 17, 20, 21},
    {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29},
    {10, 11, 14, 15, 26, 27, 30, 31},
}};

constexpr P4Traits::BlockLookupTableT kBlockTableP4{{
    {0, 2, 8, 10},
    {1, 3, 9, 11},
    {4, 6, 12, 14},
    {5, 7, 13, 15},
    {16, 18, 24, 26},
    {17, 19, 25, 27},
    {20, 22, 28, 30},
    {21, 23, 29, 31},
}};

constexpr P8Traits::ColumnLookupTableT kColumnTable8{{
    {0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54},
    {8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62},
    {33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23},
    {41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31},
    {96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86},
    {104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94},
    {65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119},
    {73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127},
    {128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182},
    {136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190},
    {161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151},
    {169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159},
    {224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214},
    {232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222},
    {193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247},
    {201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255},
}};

constexpr P4Traits::ColumnLookupTableT kColumnTable4{{
    {0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110},
    {16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126},
    {65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47},
    {81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63},
    {192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174},
    {208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190},
    {129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239},
    {145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255},
    {256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362, 260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366},
    {272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378, 276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382},
    {321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299, 325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303},
    {337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315, 341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319},
    {448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426, 452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430},
    {464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442, 468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446},
    {385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491, 389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495},
    {401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507, 405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511},
}};

// Where a texel lives inside its 256-byte block.
//
// The three layouts share a skeleton. A block is four 64-byte columns, one
// per group of texel rows (four rows for 8- and 4-bit, two for 32-bit), and a
// column is eight 8-byte pieces: at 0, 16, 32 and 48 for its even rows, at 8,
// 24, 40 and 56 for its odd rows. Within a row, which piece a texel takes and
// where in it is a fixed shuffle of x:
//   PSMCT32/Z32  piece x / 2, pixel x % 2: a row fills its four pieces.
//   PSMT8        piece (x / 2) % 4, byte 2 * (x / 8) + 4 * (x % 2): a row
//                fills the even bytes of its pieces and the row two below
//                the odd ones.
//   PSMT4        piece (x / 2) % 4, byte x / 8 + 4 * (x % 2): a row fills one
//                nibble of every byte of its pieces, the low one for the
//                column's first two rows, and the row two below the other.
// In the 8- and 4-bit layouts, rows 2 and 3 of an even column and rows 0 and
// 1 of an odd one also have their pieces swapped in pairs, 0 with 2 and 1
// with 3 (flip32). matchesColumnTables() proves all of this against GSMem's
// tables, so a mistake here fails to compile rather than to render.

// The piece swap for texel row y, as an XOR on the piece's offset.
constexpr uint32_t flip32(uint32_t y) {
    return (((y >> 1) ^ (y >> 2)) & 1u) * 32u;
}

// PSMT4: the bytes of row y's pieces start here.
constexpr uint32_t rowBytes4(uint32_t y) {
    return 64u * ((y >> 2) & 3u) + 8u * (y & 1u);
}

// PSMT4: byte of texel (x, y) within its block, and the shift of its nibble.
constexpr uint32_t texelByte4(uint32_t x, uint32_t y) {
    x &= 31u;
    return rowBytes4(y) + ((((x & 6u) << 3) | ((x & 1u) << 2) | (x >> 3)) ^ flip32(y));
}

constexpr uint32_t shift4(uint32_t y) {
    return (y & 2u) * 2u;
}

// PSMT8: the bytes of row y's pieces start here, and texel (x, y) is this
// byte of its block.
constexpr uint32_t rowBytes8(uint32_t y) {
    return 64u * ((y >> 2) & 3u) + 8u * (y & 1u) + ((y >> 1) & 1u);
}

constexpr uint32_t texelByte8(uint32_t x, uint32_t y) {
    x &= 15u;
    return rowBytes8(y) + ((((x & 6u) << 3) | ((x & 1u) << 2) | ((x & 8u) >> 2)) ^ flip32(y));
}

// PSMCT32/Z32: first byte of pixel (x, y) within its block.
constexpr uint32_t texelByte32(uint32_t x, uint32_t y) {
    x &= 7u;
    return 64u * ((y >> 1) & 3u) + 8u * (y & 1u) + 16u * (x >> 1) + 4u * (x & 1u);
}

constexpr bool matchesColumnTables() {
    for (uint32_t y = 0; y < 16u; ++y)
        for (uint32_t x = 0; x < 32u; ++x)
            if (kColumnTable4[y][x] != texelByte4(x, y) * 2u + shift4(y) / 4u)
                return false;
    for (uint32_t y = 0; y < 16u; ++y)
        for (uint32_t x = 0; x < 16u; ++x)
            if (kColumnTable8[y][x] != texelByte8(x, y))
                return false;
    for (uint32_t y = 0; y < 8u; ++y)
        for (uint32_t x = 0; x < 8u; ++x)
            if (kColumnTable32[y][x] != texelByte32(x, y) / 4u)
                return false;
    return true;
}
static_assert(matchesColumnTables(), "the closed-form block layout must reproduce GSMem's column tables");

// The same shuffles of x as tables, for the paths that write one texel at a
// time: a load beats the five or six bit operations per texel the closed
// forms cost there.
constexpr std::array<uint8_t, 32> kPieceByte4 = [] {
    std::array<uint8_t, 32> bytes{};
    for (uint32_t x = 0; x < 32u; ++x)
        bytes[x] = static_cast<uint8_t>(texelByte4(x, 0u));
    return bytes;
}();

constexpr std::array<uint8_t, 16> kPieceByte8 = [] {
    std::array<uint8_t, 16> bytes{};
    for (uint32_t x = 0; x < 16u; ++x)
        bytes[x] = static_cast<uint8_t>(texelByte8(x, 0u));
    return bytes;
}();

// ---------------------------------------------------------------------------
// Column kernels.
//
// A "group" is the texels of one row that share a block (32 for PSMT4, 16
// for PSMT8, 8 for 32-bit), a "pair" is two such rows from an even row and a
// "band" is four from a column boundary. A group of PSMT4 or PSMT8 fills
// half of each byte it touches, so it is a read-modify-write of four 8-byte
// pieces; a pair's pieces are adjacent, so it is four 16-byte ones; a band
// fills a whole 64-byte column, so it is four plain stores. The source is the
// host's packed row order: `s` points at the first row's first texel and the
// rows are `stride` bytes apart.

inline uint64_t load64(const uint8_t *p) {
    uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

inline void store64(uint8_t *p, uint64_t v) {
    std::memcpy(p, &v, sizeof v);
}

// PSMCT32/Z32: a row's eight pixels are its four pieces in source order.
void writeGroup32(uint8_t *block, uint32_t y, const uint8_t *s) {
    uint8_t *row = block + texelByte32(0u, y);
    store64(row, load64(s));
    store64(row + 16, load64(s + 8));
    store64(row + 32, load64(s + 16));
    store64(row + 48, load64(s + 24));
}

#if DQ8_GS_VRAM_SIMD

inline __m128i loadu(const uint8_t *p) {
    return _mm_loadu_si128(reinterpret_cast<const __m128i *>(p));
}

inline void storeu(uint8_t *p, __m128i v) {
    _mm_storeu_si128(reinterpret_cast<__m128i *>(p), v);
}

// Replaces the bytes of an 8-byte piece outside `keep`.
inline void merge8(uint8_t *p, __m128i v, __m128i keep) {
    const __m128i old = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(p));
    _mm_storel_epi64(reinterpret_cast<__m128i *>(p), _mm_or_si128(_mm_and_si128(old, keep), v));
}

// 32 PSMT4 texels as 16 bytes, low nibble first. A run that starts on a high
// nibble (an odd dsax, or an odd width on an odd row) is shifted down by one
// nibble; its 17th byte belongs to the run, so the read stays in bounds.
template <bool Odd>
inline __m128i loadNibbles(const uint8_t *s) {
    if constexpr (!Odd)
        return loadu(s);
    const __m128i low = _mm_set1_epi8(0x0f);
    return _mm_or_si128(_mm_and_si128(_mm_srli_epi16(loadu(s), 4), low),
                        _mm_andnot_si128(low, _mm_slli_epi16(loadu(s + 1), 4)));
}

// Sorts a PSMT4 group into its pieces, nibbles in the low half of each byte:
// `a` holds pieces 0 and 1, `b` pieces 2 and 3. Piece c takes the low nibbles
// of source bytes c, 4 + c, 8 + c, 12 + c and then their high nibbles, which
// is a transpose of the 4x4 byte matrix followed by an unpack.
inline void pieces4(__m128i v, __m128i &a, __m128i &b) {
    const __m128i low = _mm_set1_epi8(0x0f);
    v = _mm_shuffle_epi8(v, _mm_setr_epi8(0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15));
    const __m128i lo = _mm_and_si128(v, low);
    const __m128i hi = _mm_and_si128(_mm_srli_epi16(v, 4), low);
    a = _mm_unpacklo_epi32(lo, hi);
    b = _mm_unpackhi_epi32(lo, hi);
}

template <bool Odd>
void writeGroup4(uint8_t *block, uint32_t y, const uint8_t *s) {
    __m128i a, b;
    pieces4(loadNibbles<Odd>(s), a, b);
    const __m128i low = _mm_set1_epi8(0x0f);
    __m128i keep = _mm_andnot_si128(low, _mm_set1_epi8(-1));
    if ((y & 2u) != 0u) {
        a = _mm_slli_epi16(a, 4);
        b = _mm_slli_epi16(b, 4);
        keep = low;
    }
    uint8_t *row = block + rowBytes4(y);
    const uint32_t flip = flip32(y);
    merge8(row + (0u ^ flip), a, keep);
    merge8(row + (16u ^ flip), _mm_srli_si128(a, 8), keep);
    merge8(row + (32u ^ flip), b, keep);
    merge8(row + (48u ^ flip), _mm_srli_si128(b, 8), keep);
}

// Rows 0 and 1 of the band take the low nibbles, 2 and 3 the high ones of
// the pieces two positions over; which half of the column holds which pair
// of pieces alternates between even and odd columns (flip32).
template <bool Odd>
void writeBand4(uint8_t *block, uint32_t y, const uint8_t *s, uint32_t stride) {
    __m128i a0, b0, a1, b1, a2, b2, a3, b3;
    pieces4(loadNibbles<Odd>(s), a0, b0);
    pieces4(loadNibbles<Odd>(s + stride), a1, b1);
    pieces4(loadNibbles<Odd>(s + 2u * stride), a2, b2);
    pieces4(loadNibbles<Odd>(s + 3u * stride), a3, b3);
    uint8_t *column = block + 64u * ((y >> 2) & 3u);
    const uint32_t flip = flip32(y);
    storeu(column + (0u ^ flip),
           _mm_or_si128(_mm_unpacklo_epi64(a0, a1), _mm_slli_epi16(_mm_unpacklo_epi64(b2, b3), 4)));
    storeu(column + (16u ^ flip),
           _mm_or_si128(_mm_unpackhi_epi64(a0, a1), _mm_slli_epi16(_mm_unpackhi_epi64(b2, b3), 4)));
    storeu(column + (32u ^ flip),
           _mm_or_si128(_mm_unpacklo_epi64(b0, b1), _mm_slli_epi16(_mm_unpacklo_epi64(a2, a3), 4)));
    storeu(column + (48u ^ flip),
           _mm_or_si128(_mm_unpackhi_epi64(b0, b1), _mm_slli_epi16(_mm_unpackhi_epi64(a2, a3), 4)));
}

// Replaces the bytes of two adjacent pieces outside `keep`.
inline void merge16(uint8_t *p, __m128i v, __m128i keep) {
    storeu(p, _mm_or_si128(_mm_and_si128(loadu(p), keep), v));
}

template <bool Odd>
void writePair4(uint8_t *block, uint32_t y, const uint8_t *s, uint32_t stride) {
    __m128i a0, b0, a1, b1;
    pieces4(loadNibbles<Odd>(s), a0, b0);
    pieces4(loadNibbles<Odd>(s + stride), a1, b1);
    __m128i p0 = _mm_unpacklo_epi64(a0, a1), p1 = _mm_unpackhi_epi64(a0, a1);
    __m128i p2 = _mm_unpacklo_epi64(b0, b1), p3 = _mm_unpackhi_epi64(b0, b1);
    const __m128i low = _mm_set1_epi8(0x0f);
    __m128i keep = _mm_andnot_si128(low, _mm_set1_epi8(-1));
    if ((y & 2u) != 0u) {
        p0 = _mm_slli_epi16(p0, 4);
        p1 = _mm_slli_epi16(p1, 4);
        p2 = _mm_slli_epi16(p2, 4);
        p3 = _mm_slli_epi16(p3, 4);
        keep = low;
    }
    uint8_t *column = block + 64u * ((y >> 2) & 3u);
    const uint32_t flip = flip32(y);
    merge16(column + (0u ^ flip), p0, keep);
    merge16(column + (16u ^ flip), p1, keep);
    merge16(column + (32u ^ flip), p2, keep);
    merge16(column + (48u ^ flip), p3, keep);
}

// A PSMT8 group's 16 texels in piece order: piece c takes source bytes 2c,
// 8 + 2c, 2c + 1, 9 + 2c.
inline __m128i gather8(const uint8_t *s) {
    return _mm_shuffle_epi8(loadu(s), _mm_setr_epi8(0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15));
}

void writeGroup8(uint8_t *block, uint32_t y, const uint8_t *s) {
    const __m128i r = gather8(s), zero = _mm_setzero_si128();
    // The column's rows 2 and 3 take the odd bytes of each piece.
    const bool odd = (y & 2u) != 0u;
    const __m128i a = odd ? _mm_unpacklo_epi8(zero, r) : _mm_unpacklo_epi8(r, zero);
    const __m128i b = odd ? _mm_unpackhi_epi8(zero, r) : _mm_unpackhi_epi8(r, zero);
    const __m128i keep = _mm_set1_epi16(static_cast<short>(odd ? 0x00ff : 0xff00));
    uint8_t *row = block + 64u * ((y >> 2) & 3u) + 8u * (y & 1u);
    const uint32_t flip = flip32(y);
    merge8(row + (0u ^ flip), a, keep);
    merge8(row + (16u ^ flip), _mm_srli_si128(a, 8), keep);
    merge8(row + (32u ^ flip), b, keep);
    merge8(row + (48u ^ flip), _mm_srli_si128(b, 8), keep);
}

void writePair8(uint8_t *block, uint32_t y, const uint8_t *s, uint32_t stride) {
    const __m128i r0 = gather8(s), r1 = gather8(s + stride), zero = _mm_setzero_si128();
    const __m128i p = _mm_unpacklo_epi32(r0, r1), ph = _mm_unpackhi_epi32(r0, r1);
    const bool odd = (y & 2u) != 0u;
    const __m128i p0 = odd ? _mm_unpacklo_epi8(zero, p) : _mm_unpacklo_epi8(p, zero);
    const __m128i p1 = odd ? _mm_unpackhi_epi8(zero, p) : _mm_unpackhi_epi8(p, zero);
    const __m128i p2 = odd ? _mm_unpacklo_epi8(zero, ph) : _mm_unpacklo_epi8(ph, zero);
    const __m128i p3 = odd ? _mm_unpackhi_epi8(zero, ph) : _mm_unpackhi_epi8(ph, zero);
    const __m128i keep = _mm_set1_epi16(static_cast<short>(odd ? 0x00ff : 0xff00));
    uint8_t *column = block + 64u * ((y >> 2) & 3u);
    const uint32_t flip = flip32(y);
    merge16(column + (0u ^ flip), p0, keep);
    merge16(column + (16u ^ flip), p1, keep);
    merge16(column + (32u ^ flip), p2, keep);
    merge16(column + (48u ^ flip), p3, keep);
}

// Row r's piece c and row r + 2's piece c ^ 2 interleave byte by byte.
void writeBand8(uint8_t *block, uint32_t y, const uint8_t *s, uint32_t stride) {
    const __m128i r0 = gather8(s), r1 = gather8(s + stride);
    const __m128i r2 = gather8(s + 2u * stride), r3 = gather8(s + 3u * stride);
    const __m128i p01 = _mm_unpacklo_epi32(r0, r1), p01h = _mm_unpackhi_epi32(r0, r1);
    const __m128i p23 = _mm_unpacklo_epi32(r2, r3), p23h = _mm_unpackhi_epi32(r2, r3);
    uint8_t *column = block + 64u * ((y >> 2) & 3u);
    const uint32_t flip = flip32(y);
    storeu(column + (0u ^ flip), _mm_unpacklo_epi8(p01, p23h));
    storeu(column + (16u ^ flip), _mm_unpackhi_epi8(p01, p23h));
    storeu(column + (32u ^ flip), _mm_unpacklo_epi8(p01h, p23));
    storeu(column + (48u ^ flip), _mm_unpackhi_epi8(p01h, p23));
}

#else // DQ8_GS_VRAM_SIMD

// PSMT4. The sixteen source bytes form a 4x4 byte matrix whose columns are
// the pieces: piece c takes the low nibbles of source bytes c, 4 + c, 8 + c,
// 12 + c and then their high nibbles. Two delta swaps transpose the matrix
// in a pair of 64-bit words.
template <bool Odd>
void writeGroup4(uint8_t *block, uint32_t y, const uint8_t *s) {
    uint64_t q0 = load64(s), q1 = load64(s + 8);
    if constexpr (Odd) {
        // The run starts on a high nibble: shift the 33 nibbles down by one.
        // The 17th byte belongs to the run, so the read stays in bounds.
        const uint64_t q2 = s[16];
        q0 = (q0 >> 4) | (q1 << 60);
        q1 = (q1 >> 4) | (q2 << 60);
    }
    uint64_t t = ((q0 >> 16) ^ q1) & 0x0000ffff0000ffffull;
    q0 ^= t << 16;
    q1 ^= t;
    t = ((q0 >> 24) ^ q0) & 0x00000000ff00ff00ull;
    q0 ^= t | (t << 24);
    t = ((q1 >> 24) ^ q1) & 0x00000000ff00ff00ull;
    q1 ^= t | (t << 24);
    constexpr uint64_t lo = 0x000000000f0f0f0full, hi = 0x0f0f0f0f00000000ull;
    const uint32_t shift = shift4(y);
    const uint64_t keep = shift != 0u ? 0x0f0f0f0f0f0f0f0full : 0xf0f0f0f0f0f0f0f0ull;
    const uint64_t pieces[4] = {
        ((q0 & lo) | ((q0 << 28) & hi)) << shift,
        (((q0 >> 32) & lo) | ((q0 >> 4) & hi)) << shift,
        ((q1 & lo) | ((q1 << 28) & hi)) << shift,
        (((q1 >> 32) & lo) | ((q1 >> 4) & hi)) << shift,
    };
    uint8_t *row = block + rowBytes4(y);
    const uint32_t flip = flip32(y);
    for (uint32_t c = 0; c < 4u; ++c) {
        uint8_t *p = row + ((16u * c) ^ flip);
        store64(p, (load64(p) & keep) | pieces[c]);
    }
}

// PSMT8: piece c takes source bytes 2c, 8 + 2c, 2c + 1, 9 + 2c in its even
// bytes, or its odd ones for the column's rows 2 and 3.
void writeGroup8(uint8_t *block, uint32_t y, const uint8_t *s) {
    const uint64_t q0 = load64(s), q1 = load64(s + 8);
    const uint32_t shift = ((y >> 1) & 1u) * 8u;
    const uint64_t keep = shift != 0u ? 0x00ff00ff00ff00ffull : 0xff00ff00ff00ff00ull;
    uint8_t *row = block + 64u * ((y >> 2) & 3u) + 8u * (y & 1u);
    const uint32_t flip = flip32(y);
    for (uint32_t c = 0; c < 4u; ++c) {
        const uint64_t a = (q0 >> (16u * c)) & 0xffffu, b = (q1 >> (16u * c)) & 0xffffu;
        const uint64_t piece = (a & 0xffu) | ((b & 0xffu) << 16) | ((a & 0xff00u) << 24) | ((b & 0xff00u) << 40);
        uint8_t *p = row + ((16u * c) ^ flip);
        store64(p, (load64(p) & keep) | (piece << shift));
    }
}

#endif // DQ8_GS_VRAM_SIMD

// ---------------------------------------------------------------------------
// Formats: geometry plus the texel, group, pair and band writers for one
// PSM. `i` is the texel's index in the run, which locates it in the packed
// source.

struct FormatT4 {
    static constexpr uint32_t kPageW = 128u, kPageH = 128u, kBlockW = 32u, kBlockH = 16u;
    static constexpr bool kBands = DQ8_GS_VRAM_SIMD != 0;
    static constexpr const P4Traits::BlockLookupTableT &kBlockTable = kBlockTableP4;

    // The band and pair kernels read all their rows at one nibble parity,
    // which an odd width would alternate.
    static bool bandRows(uint32_t width) { return (width & 1u) == 0u; }

    static void texel(uint8_t *block, uint32_t x, uint32_t y, const uint8_t *src, uint32_t i) {
        const uint8_t packed = src[i >> 1u];
        const uint32_t value = (i & 1u) ? (packed >> 4u) : (packed & 0x0fu);
        uint8_t &byte = block[rowBytes4(y) + (kPieceByte4[x & 31u] ^ flip32(y))];
        const uint32_t shift = shift4(y);
        byte = static_cast<uint8_t>((byte & ~(0x0fu << shift)) | (value << shift));
    }
    static void group(uint8_t *block, uint32_t y, const uint8_t *src, uint32_t i) {
        if (i & 1u)
            writeGroup4<true>(block, y, src + (i >> 1u));
        else
            writeGroup4<false>(block, y, src + (i >> 1u));
    }
#if DQ8_GS_VRAM_SIMD
    template <uint32_t Rows>
    static void rows(uint8_t *block, uint32_t y, const uint8_t *src, uint32_t i, uint32_t width) {
        const uint8_t *s = src + (i >> 1u);
        if constexpr (Rows == 4u) {
            if (i & 1u)
                writeBand4<true>(block, y, s, width / 2u);
            else
                writeBand4<false>(block, y, s, width / 2u);
        } else {
            if (i & 1u)
                writePair4<true>(block, y, s, width / 2u);
            else
                writePair4<false>(block, y, s, width / 2u);
        }
    }
#endif
};

struct FormatT8 {
    static constexpr uint32_t kPageW = 128u, kPageH = 64u, kBlockW = 16u, kBlockH = 16u;
    static constexpr bool kBands = DQ8_GS_VRAM_SIMD != 0;
    static constexpr const P8Traits::BlockLookupTableT &kBlockTable = kBlockTableP8;

    static bool bandRows(uint32_t) { return true; }

    static void texel(uint8_t *block, uint32_t x, uint32_t y, const uint8_t *src, uint32_t i) {
        block[rowBytes8(y) + (kPieceByte8[x & 15u] ^ flip32(y))] = src[i];
    }
    static void group(uint8_t *block, uint32_t y, const uint8_t *src, uint32_t i) {
        writeGroup8(block, y, src + i);
    }
#if DQ8_GS_VRAM_SIMD
    template <uint32_t Rows>
    static void rows(uint8_t *block, uint32_t y, const uint8_t *src, uint32_t i, uint32_t width) {
        if constexpr (Rows == 4u)
            writeBand8(block, y, src + i, width);
        else
            writePair8(block, y, src + i, width);
    }
#endif
};

// Four bytes a pixel fill whole bytes, so a row of groups is already plain
// copies and bands would gain little.
struct Format32 {
    static constexpr uint32_t kPageW = 64u, kPageH = 32u, kBlockW = 8u, kBlockH = 8u;
    static constexpr bool kBands = false;

    static bool bandRows(uint32_t) { return false; }

    static void texel(uint8_t *block, uint32_t x, uint32_t y, const uint8_t *src, uint32_t i) {
        std::memcpy(block + texelByte32(x, y), src + i * 4u, 4u);
    }
    static void group(uint8_t *block, uint32_t y, const uint8_t *src, uint32_t i) {
        writeGroup32(block, y, src + i * 4u);
    }
    template <uint32_t Rows>
    static void rows(uint8_t *, uint32_t, const uint8_t *, uint32_t, uint32_t) {}
};

struct FormatCT32 : Format32 {
    static constexpr const C32Traits::BlockLookupTableT &kBlockTable = kBlockTableC32;
};

struct FormatZ32 : Format32 {
    static constexpr const Z32Traits::BlockLookupTableT &kBlockTable = kBlockTableZ32;
};

// ---------------------------------------------------------------------------
// The run writer. GSMem's address of a texel is page * PixelsPerPage plus a
// table entry that is (block + blockInPage) * PixelsPerBlock + columnOffset,
// where block, the base's offset inside its page, may carry into the next
// page. This finds the same block (as bytes, masked to the 4 MiB as GSMem
// masks each texel) and lets the kernels place texels inside it.

template <class F>
struct BlockMap {
    uint8_t *data;
    uint32_t block;
    uint32_t basePage;
    uint32_t pagesPerRow;

    uint8_t *operator()(uint32_t x, uint32_t y) const {
        const uint32_t page = basePage + (y / F::kPageH) * pagesPerRow + x / F::kPageW;
        const uint32_t inPage = F::kBlockTable[(y % F::kPageH) / F::kBlockH][(x % F::kPageW) / F::kBlockW];
        return data + (((page * 32u + block + inPage) * 256u) & (GSMem::MEMORY_SIZE - 1u));
    }
};

template <class F>
BlockMap<F> blockMap(uint8_t *data, uint32_t base, uint32_t bw) {
    return {data, base % 32u, base / 32u, (bw * 64u) / F::kPageW};
}

// Texels xs to xe of row y, all within one block, one at a time.
template <class F>
void writeTexels(uint8_t *block, uint32_t y, uint32_t xs, uint32_t xe, const uint8_t *src, uint32_t i) {
    for (; xs < xe; ++xs, ++i)
        F::texel(block, xs, y, src, i);
}

// Texels xs to xe of row y: whole groups through the kernel, the ends one at
// a time. Groups are block-aligned, so neither end spans two blocks.
template <class F>
void writeRowPiece(const BlockMap<F> &at, uint32_t y, uint32_t xs, uint32_t xe, const uint8_t *src, uint32_t i) {
    constexpr uint32_t kGroup = F::kBlockW;
    const uint32_t xa = std::min(xe, (xs + kGroup - 1u) & ~(kGroup - 1u));
    if (xs < xa) {
        writeTexels<F>(at(xs, y), y, xs, xa, src, i);
        i += xa - xs;
        xs = xa;
    }
    for (; xe - xs >= kGroup; xs += kGroup, i += kGroup)
        F::group(at(xs, y), y, src, i);
    if (xs < xe)
        writeTexels<F>(at(xs, y), y, xs, xe, src, i);
}

// Rows y to y + Rows - 1 from dsax to rowEnd, y a multiple of Rows: whole
// groups through the band (four rows) or pair (two) kernel, the ends of each
// row one texel at a time.
template <class F, uint32_t Rows>
void writeRows(const BlockMap<F> &at, uint32_t y, uint32_t dsax, uint32_t rowEnd, const uint8_t *src, uint32_t i) {
    constexpr uint32_t kGroup = F::kBlockW;
    const uint32_t width = rowEnd - dsax;
    const uint32_t xa = std::min(rowEnd, (dsax + kGroup - 1u) & ~(kGroup - 1u));
    const uint32_t xb = xa + ((rowEnd - xa) / kGroup) * kGroup;
    for (uint32_t r = 0; r < Rows; ++r) {
        const uint32_t ir = i + r * width;
        if (dsax < xa)
            writeTexels<F>(at(dsax, y + r), y + r, dsax, xa, src, ir);
        if (xb < rowEnd)
            writeTexels<F>(at(xb, y + r), y + r, xb, rowEnd, src, ir + (xb - dsax));
    }
    for (uint32_t x = xa; x < xb; x += kGroup)
        F::template rows<Rows>(at(x, y), y, src, i + (x - dsax), width);
}

// `count` texels from (x, y), wrapping to dsax at rowEnd, as `count` calls
// to GSMem's writer would store them. With a thousand uploads a frame the
// per-texel path alone was a quarter of the GS worker; now a texture that
// arrives in whole rows goes through the band kernel four rows at a time
// (an IMAGE payload normally carries the whole texture), its remainder
// through the pair and group kernels, with per-texel writes only at the
// ends of rows narrower than a group or not aligned to one.
template <class F>
void writeRun(uint8_t *data, uint32_t base, uint32_t bw, uint32_t dsax, uint32_t rowEnd, uint32_t x, uint32_t y,
              const uint8_t *src, uint32_t count) {
    const BlockMap<F> at = blockMap<F>(data, base, bw);
    uint32_t i = 0;
    while (i < count) {
        if (x >= rowEnd) {
            x = dsax;
            ++y;
            if (dsax >= rowEnd)
                return; // an empty row never advances
            continue;
        }
        if constexpr (F::kBands) {
            const uint32_t width = rowEnd - dsax;
            if (x == dsax && width >= F::kBlockW && F::bandRows(width)) {
                const uint64_t left = count - i;
                if ((y & 3u) == 0u && left >= 4ull * width) {
                    writeRows<F, 4u>(at, y, dsax, rowEnd, src, i);
                    i += 4u * width;
                    y += 4u;
                    continue;
                }
                if ((y & 1u) == 0u && left >= 2ull * width) {
                    writeRows<F, 2u>(at, y, dsax, rowEnd, src, i);
                    i += 2u * width;
                    y += 2u;
                    continue;
                }
            }
        }
        const uint32_t n = std::min(count - i, rowEnd - x);
        writeRowPiece<F>(at, y, x, x + n, src, i);
        i += n;
        x += n;
    }
}

} // namespace

void GsVram::writeRunT8(uint32_t base, uint32_t bw, uint32_t dsax, uint32_t rowEnd, uint32_t x, uint32_t y,
                        const uint8_t *src, uint32_t count) {
    if (!attached())
        return;
    writeRun<FormatT8>(m_data, base, bw, dsax, rowEnd, x, y, src, count);
}

void GsVram::writeRunCT32(uint32_t base, uint32_t bw, uint32_t dsax, uint32_t rowEnd, uint32_t x, uint32_t y,
                          const uint8_t *src, uint32_t count) {
    if (!attached())
        return;
    writeRun<FormatCT32>(m_data, base, bw, dsax, rowEnd, x, y, src, count);
}

void GsVram::writeRunZ32(uint32_t base, uint32_t bw, uint32_t dsax, uint32_t rowEnd, uint32_t x, uint32_t y,
                         const uint8_t *src, uint32_t count) {
    if (!attached())
        return;
    writeRun<FormatZ32>(m_data, base, bw, dsax, rowEnd, x, y, src, count);
}

void GsVram::readRowT4(uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t count, uint8_t *out) const {
    if (!attached())
        return;
    const BlockMap<FormatT4> at = blockMap<FormatT4>(const_cast<uint8_t *>(m_data), base, bw);
    const uint32_t shift = shift4(y), flip = flip32(y);
    for (uint32_t i = 0; i < count;) {
        const uint8_t *row = at(x, y) + rowBytes4(y);
        const uint32_t n = std::min(count - i, FormatT4::kBlockW - (x % FormatT4::kBlockW));
        for (uint32_t k = 0; k < n; ++k, ++x, ++i)
            out[i] = static_cast<uint8_t>((row[kPieceByte4[x & 31u] ^ flip] >> shift) & 0x0fu);
    }
}

template <class F>
void appendBlocksOf(const uint8_t *data, uint32_t base, uint32_t bw, uint32_t width, uint32_t height, std::vector<uint8_t> &out) {
    const BlockMap<F> at = blockMap<F>(const_cast<uint8_t *>(data), base, bw);
    for (uint32_t y = 0; y < height; y += F::kBlockH)
        for (uint32_t x = 0; x < width; x += F::kBlockW) {
            const uint8_t *block = at(x, y);
            out.insert(out.end(), block, block + 256u);
        }
}

void GsVram::appendBlocks(bool four, uint32_t base, uint32_t bw, uint32_t width, uint32_t height,
                          std::vector<uint8_t> &out) const {
    if (!attached())
        return;
    if (four)
        appendBlocksOf<FormatT4>(m_data, base, bw, width, height, out);
    else
        appendBlocksOf<FormatT8>(m_data, base, bw, width, height, out);
}

void GsVram::writeRunT4(uint32_t base, uint32_t bw, uint32_t dsax, uint32_t rowEnd, uint32_t x, uint32_t y,
                        const uint8_t *src, uint32_t count) {
    if (!attached())
        return;
    writeRun<FormatT4>(m_data, base, bw, dsax, rowEnd, x, y, src, count);
}

GsPageExtent gsPageExtent(uint32_t psm) {
    switch (psm & 0x3fu) {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    case GS_PSM_Z32:
    case GS_PSM_Z24:
    case GS_PSM_T8H:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
        return {64u, 32u};
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        return {64u, 64u};
    case GS_PSM_T8:
        return {128u, 64u};
    case GS_PSM_T4:
        return {128u, 128u};
    default:
        return {};
    }
}

bool gsIsIndexedPsm(uint32_t psm) {
    switch (psm & 0x3fu) {
    case GS_PSM_T8:
    case GS_PSM_T4:
    case GS_PSM_T8H:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
        return true;
    default:
        return false;
    }
}

bool gsIsDepthPsm(uint32_t psm) {
    switch (psm & 0x3fu) {
    case GS_PSM_Z32:
    case GS_PSM_Z24:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        return true;
    default:
        return false;
    }
}

bool gsIsColorFramePsm(uint32_t psm) {
    switch (psm & 0x3fu) {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        return true;
    default:
        return false;
    }
}

uint32_t gsBitsPerPixel(uint32_t psm) {
    return static_cast<uint32_t>(
        GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(psm & 0x3fu)));
}

void gsMarkPages(GsPageSet &pages,
                 uint32_t baseBlock,
                 uint32_t bufferWidth,
                 uint32_t psm,
                 uint32_t width,
                 uint32_t height,
                 uint32_t x,
                 uint32_t y) {
    if (width == 0u || height == 0u)
        return;
    const GsPageExtent extent = gsPageExtent(psm);
    if (!extent.valid()) {
        pages.set();
        return;
    }
    const uint32_t pixelsPerRow = std::max<uint32_t>(bufferWidth, 1u) * 64u;
    const uint32_t pagesPerRow = std::max<uint32_t>(pixelsPerRow / extent.width, 1u);
    const uint32_t lastColumn = (x + width - 1u) / extent.width;
    const uint32_t lastRow = (y + height - 1u) / extent.height;
    const uint32_t basePage = baseBlock >> 5u;

    // A block base inside a page pushes part of the rectangle into the next
    // page: the blocks whose table entry plus the base offset passes 31. With
    // the format's block table that is exact; marking the next page for
    // every cell took in the page after a 16-colour palette at the end of
    // one, and with it the render target living there (Rogue Galaxy:
    // added; the game writes its palettes at block offsets 0x1c..0x1f).
    const uint32_t baseOffset = baseBlock & 31u;
    const GsBlockExtent block = gsBlockExtent(psm);
    if (baseOffset != 0u && block.valid()) {
        const uint32_t lastBlockColumn = (x + width - 1u) / block.width;
        const uint32_t lastBlockRow = (y + height - 1u) / block.height;
        const uint32_t blocksPerPageRow = extent.width / block.width;
        const uint32_t blocksPerPageColumn = extent.height / block.height;
        for (uint32_t row = y / block.height; row <= lastBlockRow; ++row) {
            for (uint32_t column = x / block.width; column <= lastBlockColumn; ++column) {
                const uint32_t page = basePage + (row / blocksPerPageColumn) * pagesPerRow + column / blocksPerPageRow;
                const uint32_t id = baseOffset + gsBlockInPage(psm, column % blocksPerPageRow, row % blocksPerPageColumn);
                pages.set((page + (id >> 5u)) % kGsPageCount);
            }
        }
        return;
    }

    for (uint32_t row = y / extent.height; row <= lastRow; ++row) {
        for (uint32_t column = x / extent.width; column <= lastColumn; ++column) {
            const uint32_t page = basePage + row * pagesPerRow + column;
            pages.set(page % kGsPageCount);
            if (baseOffset != 0u)
                pages.set((page + 1u) % kGsPageCount);
        }
    }
}

GsBlockExtent gsBlockExtent(uint32_t psm) {
    switch (psm & 0x3fu) {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    case GS_PSM_T8H:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
        return {8u, 8u};
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        return {16u, 8u};
    case GS_PSM_T8:
        return {16u, 16u};
    case GS_PSM_T4:
        return {32u, 16u};
    default:
        // The depth formats' block tables are the colour ones mirrored; not
        // needed yet, so they keep the conservative marking.
        return {};
    }
}

uint32_t gsBlockInPage(uint32_t psm, uint32_t column, uint32_t row) {
    switch (psm & 0x3fu) {
    case GS_PSM_CT16:
        return GSPSMCT16::blockTable16[row & 7u][column & 3u];
    case GS_PSM_CT16S:
        return GSPSMCT16::blockTable16S[row & 7u][column & 3u];
    case GS_PSM_T8:
        return GSPSMT8::blockTable8[row & 3u][column & 7u];
    case GS_PSM_T4:
        return GSPSMT4::blockTable4[row & 7u][column & 3u];
    default:
        return GSPSMCT32::blockTable32[row & 3u][column & 7u];
    }
}

void gsMarkCoveredPages(GsPageSet &pages,
                        uint32_t baseBlock,
                        uint32_t bufferWidth,
                        uint32_t psm,
                        uint32_t width,
                        uint32_t height,
                        uint32_t x,
                        uint32_t y) {
    const GsPageExtent extent = gsPageExtent(psm);
    const uint32_t pixelsPerRow = std::max<uint32_t>(bufferWidth, 1u) * 64u;
    // Only where page cells tile the buffer exactly; anything else covers
    // no page with certainty.
    if (!extent.valid() || width == 0u || height == 0u || (baseBlock & 31u) != 0u ||
        pixelsPerRow % extent.width != 0u || x + width > pixelsPerRow)
        return;
    const uint32_t pagesPerRow = pixelsPerRow / extent.width;
    const uint32_t firstColumn = (x + extent.width - 1u) / extent.width;
    const uint32_t endColumn = (x + width) / extent.width;
    const uint32_t firstRow = (y + extent.height - 1u) / extent.height;
    const uint32_t endRow = (y + height) / extent.height;
    for (uint32_t row = firstRow; row < endRow; ++row)
        for (uint32_t column = firstColumn; column < endColumn; ++column)
            pages.set(((baseBlock >> 5u) + row * pagesPerRow + column) % kGsPageCount);
}

GsVram::GsVram() {
    ensureLookupTables();
}

void GsVram::attach(uint8_t *storage, uint32_t sizeBytes) {
    ensureLookupTables();
    m_data = storage;
    m_size = storage != nullptr ? sizeBytes : 0u;
}

uint32_t GsVram::read(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const {
    if (!attached())
        return 0u;
    // GSMem's accessors take a mutable pointer even to read.
    uint8_t *storage = const_cast<uint8_t *>(m_data);
    using namespace GSMem;
    switch (psm & 0x3fu) {
    case GS_PSM_CT32: return ReadCT32(storage, base, bw, x, y);
    case GS_PSM_CT24: return ReadCT24(storage, base, bw, x, y);
    case GS_PSM_CT16: return ReadCT16(storage, base, bw, x, y);
    case GS_PSM_CT16S: return ReadCT16S(storage, base, bw, x, y);
    case GS_PSM_T8: return ReadP8(storage, base, bw, x, y);
    case GS_PSM_T4: return ReadP4(storage, base, bw, x, y);
    case GS_PSM_T8H: return ReadP8H(storage, base, bw, x, y);
    case GS_PSM_T4HL: return ReadP4HL(storage, base, bw, x, y);
    case GS_PSM_T4HH: return ReadP4HH(storage, base, bw, x, y);
    case GS_PSM_Z32: return ReadZ32(storage, base, bw, x, y);
    case GS_PSM_Z24: return ReadZ24(storage, base, bw, x, y);
    case GS_PSM_Z16: return ReadZ16(storage, base, bw, x, y);
    case GS_PSM_Z16S: return ReadZ16S(storage, base, bw, x, y);
    default: return 0u;
    }
}

void GsVram::write(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) {
    if (!attached())
        return;
    using namespace GSMem;
    switch (psm & 0x3fu) {
    case GS_PSM_CT32: WriteCT32(m_data, base, bw, x, y, value); break;
    case GS_PSM_CT24: WriteCT24(m_data, base, bw, x, y, value); break;
    case GS_PSM_CT16: WriteCT16(m_data, base, bw, x, y, value); break;
    case GS_PSM_CT16S: WriteCT16S(m_data, base, bw, x, y, value); break;
    case GS_PSM_T8: WriteP8(m_data, base, bw, x, y, value); break;
    case GS_PSM_T4: WriteP4(m_data, base, bw, x, y, value); break;
    case GS_PSM_T8H: WriteP8H(m_data, base, bw, x, y, value); break;
    case GS_PSM_T4HL: WriteP4HL(m_data, base, bw, x, y, value); break;
    case GS_PSM_T4HH: WriteP4HH(m_data, base, bw, x, y, value); break;
    case GS_PSM_Z32: WriteZ32(m_data, base, bw, x, y, value); break;
    case GS_PSM_Z24: WriteZ24(m_data, base, bw, x, y, value); break;
    case GS_PSM_Z16: WriteZ16(m_data, base, bw, x, y, value); break;
    case GS_PSM_Z16S: WriteZ16S(m_data, base, bw, x, y, value); break;
    default: break;
    }
}

} // namespace dq8::gfx
