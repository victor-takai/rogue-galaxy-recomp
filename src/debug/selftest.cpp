#include "debug/selftest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#if defined(RG_HAS_SDLGPU)
#include "gfx/gs/gs_vram.h"
#endif
#include "runtime/gs/ps2_gs_memory.h"

namespace rg::debug
{
namespace
{

#if defined(RG_HAS_SDLGPU)
// The run writers added to the vendored GsVram against GSMem's own
// per-pixel writes: same bytes for random rectangles, bases and data.
bool vramRunWriters()
{
    GSMem::InitLookupTables();
    constexpr uint32_t kSize = 4u * 1024u * 1024u;
    std::vector<uint8_t> expected(kSize), actual(kSize);
    dq8::gfx::GsVram vram;
    vram.attach(actual.data(), kSize);
    std::mt19937 rng(1234);
    for (int round = 0; round < 400; ++round)
    {
        const int mode = round % 4; // T8, T4, CT32, Z32
        const bool t4 = mode == 1;
        const bool wide = mode >= 2;
        const bool z = mode == 3;
        for (size_t i = 0; i < kSize; ++i)
        {
            expected[i] = actual[i] = static_cast<uint8_t>(rng());
        }
        const uint32_t bw = 1u + rng() % 16u;
        const uint32_t base = rng() % 0x3000u;
        const uint32_t dsax = rng() % 300u;
        const uint32_t dsay = rng() % 300u;
        const uint32_t rrw = 1u + rng() % 200u;
        const uint32_t rrh = 1u + rng() % 64u;
        // Start part-way into the rectangle, as a later IMAGE payload does.
        const uint32_t total = rrw * rrh;
        uint32_t start = rng() % total;
        if (t4)
        {
            start &= ~1u;
        }
        const uint32_t count = total - start;
        std::vector<uint8_t> src(t4 ? (count + 1u) / 2u : wide ? count * 4u : count);
        for (auto &b : src)
        {
            b = static_cast<uint8_t>(rng());
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t pixel = start + i;
            const uint32_t x = dsax + pixel % rrw;
            const uint32_t y = dsay + pixel / rrw;
            if (t4)
            {
                const uint8_t packed = src[i / 2u];
                GSMem::WriteP4(expected.data(), base, bw, x, y, (i & 1u) ? packed >> 4u : packed & 0x0fu);
            }
            else if (wide)
            {
                uint32_t value = 0u;
                std::memcpy(&value, src.data() + i * 4u, sizeof(value));
                if (z)
                {
                    GSMem::WriteZ32(expected.data(), base, bw, x, y, value);
                }
                else
                {
                    GSMem::WriteCT32(expected.data(), base, bw, x, y, value);
                }
            }
            else
            {
                GSMem::WriteP8(expected.data(), base, bw, x, y, src[i]);
            }
        }
        const uint32_t x0 = dsax + start % rrw;
        const uint32_t y0 = dsay + start / rrw;
        if (t4)
        {
            vram.writeRunT4(base, bw, dsax, dsax + rrw, x0, y0, src.data(), count);
        }
        else if (z)
        {
            vram.writeRunZ32(base, bw, dsax, dsax + rrw, x0, y0, src.data(), count);
        }
        else if (wide)
        {
            vram.writeRunCT32(base, bw, dsax, dsax + rrw, x0, y0, src.data(), count);
        }
        else
        {
            vram.writeRunT8(base, bw, dsax, dsax + rrw, x0, y0, src.data(), count);
        }
        if (std::memcmp(expected.data(), actual.data(), kSize) != 0)
        {
            std::fprintf(stderr, "[selftest] %s run differs: base=0x%x bw=%u pos=(%u,%u) %ux%u start=%u\n",
                         t4 ? "T4" : z ? "Z32" : wide ? "CT32" : "T8", base, bw, dsax, dsay, rrw, rrh, start);
            return false;
        }
    }
    std::printf("[selftest] GsVram T4/T8/CT32/Z32 run writers match GSMem over 400 rectangles\n");

    // The T4 row reader against GSMem::ReadP4 over the random memory left above.
    std::vector<uint8_t> row(512);
    for (int round = 0; round < 2000; ++round)
    {
        const uint32_t bw = 1u + rng() % 16u;
        const uint32_t base = rng() % 0x3000u;
        const uint32_t x = rng() % 600u, y = rng() % 600u, count = 1u + rng() % 512u;
        vram.readRowT4(base, bw, x, y, count, row.data());
        for (uint32_t i = 0; i < count; ++i)
        {
            if (row[i] != GSMem::ReadP4(actual.data(), base, bw, x + i, y))
            {
                std::fprintf(stderr, "[selftest] T4 row read differs: base=0x%x bw=%u (%u,%u)+%u\n", base, bw, x, y, i);
                return false;
            }
        }
    }
    std::printf("[selftest] GsVram T4 row reader matches GSMem over 2000 rows\n");

    // The T8, CT32 and Z32 row readers against GSMem's per-pixel reads and
    // its row readers, over the same memory: rows that start inside a block,
    // cross pages and run past 4 MiB. With DQ8_GFX_NO_BLOCK_READS set they
    // are GSMem's own, so nothing would be checked: refuse rather than pass.
    if (std::getenv("DQ8_GFX_NO_BLOCK_READS"))
    {
        std::fprintf(stderr, "[selftest] DQ8_GFX_NO_BLOCK_READS is set, so the block row readers cannot be "
                             "checked; run without it\n");
        return false;
    }
    std::vector<uint8_t> ours(512u * 4u), theirs(512u * 4u);
    for (int round = 0; round < 3000; ++round)
    {
        const int mode = round % 3; // T8, CT32, Z32
        const uint32_t bw = 1u + rng() % 16u;
        const uint32_t base = rng() % 0x3000u;
        const uint32_t x = rng() % 600u, y = rng() % 600u, count = 1u + rng() % 512u;
        const uint32_t bytes = mode == 0 ? 1u : 4u;
        if (mode == 0)
        {
            vram.readRowT8(base, bw, x, y, count, ours.data());
            GSMem::ReadRowP8(actual.data(), base, bw, x, y, count, theirs.data());
        }
        else if (mode == 1)
        {
            vram.readRowCT32(base, bw, x, y, count, ours.data());
            GSMem::ReadRowCT32(actual.data(), base, bw, x, y, count, theirs.data());
        }
        else
        {
            vram.readRowZ32(base, bw, x, y, count, ours.data());
            GSMem::ReadRowZ32(actual.data(), base, bw, x, y, count, theirs.data());
        }
        bool same = std::memcmp(ours.data(), theirs.data(), size_t(count) * bytes) == 0;
        for (uint32_t i = 0; same && i < count; ++i)
        {
            uint32_t value = 0u;
            std::memcpy(&value, ours.data() + size_t(i) * bytes, bytes);
            const uint32_t expected = mode == 0   ? GSMem::ReadP8(actual.data(), base, bw, x + i, y)
                                      : mode == 1 ? GSMem::ReadCT32(actual.data(), base, bw, x + i, y)
                                                  : GSMem::ReadZ32(actual.data(), base, bw, x + i, y);
            same = value == expected;
        }
        if (!same)
        {
            std::fprintf(stderr, "[selftest] %s row read differs: base=0x%x bw=%u (%u,%u) count %u\n",
                         mode == 0 ? "T8" : mode == 1 ? "CT32" : "Z32", base, bw, x, y, count);
            return false;
        }
    }
    std::printf("[selftest] GsVram T8/CT32/Z32 row readers match GSMem over 3000 rows\n");
    return true;
}
#endif

} // namespace

bool runSelfTestIfRequested(int &status)
{
    const char *which = std::getenv("RG_SELFTEST");
    if (!which || !*which)
    {
        return false;
    }
    bool ok = true;
#if defined(RG_HAS_SDLGPU)
    if (std::string(which) == "gs" || std::string(which) == "all")
    {
        ok = vramRunWriters() && ok;
    }
#endif
    status = ok ? 0 : 1;
    return true;
}

} // namespace rg::debug
