#include "dc_balance.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <smmintrin.h>

#include "ps2_runtime.h"
#include "scene_timing.h"

namespace rg::dcbalance
{
namespace
{

constexpr uint32_t kDataTableRunCfg = 0x003683E0u;
constexpr uint32_t kCharaGetStatF3C = 0x003B02F0u;
constexpr uint32_t kKnockdownResistance = 1u; // CharaGetStatF3C's index
constexpr uint32_t kBossTable = 0x00474B20u;     // class 2501
constexpr uint32_t kBossPartTable = 0x004746C0u; // class 2502
constexpr float kNeverKnockedDown = 10.0f;

struct Table
{
    std::string name;
    uint32_t usSize = 0;
    uint32_t usCrc = 0;
    std::vector<uint8_t> data;
    bool told = false;
};

std::atomic<bool> g_preferred{false};
std::vector<Table> g_tables;
PS2Runtime::RecompiledFunction g_runCfg = nullptr;
PS2Runtime::RecompiledFunction g_getStatF3C = nullptr;

uint32_t crc32(const uint8_t *data, size_t size)
{
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
            {
                c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
    {
        c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

uint32_t readWord(const uint8_t *rdram, uint32_t address)
{
    uint32_t value = 0;
    std::memcpy(&value, rdram + (address & 0x1FFFFFFFu), sizeof(value));
    return value;
}

// disc/JA/balance/tables.txt: "# ..." comments, then one table a line: its
// file name, the US table's size and CRC-32 (hex), the file's size.
bool loadTables(const std::filesystem::path &folder)
{
    std::ifstream list(folder / "tables.txt");
    if (!list)
    {
        return false;
    }
    std::vector<Table> tables;
    std::string line;
    while (std::getline(list, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        std::istringstream fields(line);
        Table table;
        std::string crc;
        size_t size = 0;
        if (!(fields >> table.name >> table.usSize >> crc >> size))
        {
            std::fprintf(stderr, "[rg] Director's Cut balance: a line of tables.txt is not understood: %s\n",
                         line.c_str());
            return false;
        }
        table.usCrc = static_cast<uint32_t>(std::strtoul(crc.c_str(), nullptr, 16));
        std::ifstream file(folder / table.name, std::ios::binary);
        table.data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        if (!file.good() && !file.eof())
        {
            table.data.clear();
        }
        // A table runs in place of the US one, in its buffer: never larger.
        if (table.data.size() != size || size == 0 || size > table.usSize)
        {
            std::fprintf(stderr, "[rg] Director's Cut balance: %s is missing or not whole; run "
                                 "Install-Undub.bat again\n",
                         table.name.c_str());
            return false;
        }
        tables.push_back(std::move(table));
    }
    if (tables.empty())
    {
        return false;
    }
    g_tables = std::move(tables);
    return true;
}

void dataTableRunCfg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t buffer = getRegU32(ctx, 5) & 0x1FFFFFFFu;
    const uint32_t size = getRegU32(ctx, 6);
    for (Table &table : g_tables)
    {
        if (size != table.usSize || buffer == 0 || buffer > PS2_RAM_SIZE || size > PS2_RAM_SIZE - buffer)
        {
            continue;
        }
        if (crc32(rdram + buffer, size) != table.usCrc)
        {
            continue;
        }
        std::memcpy(rdram + buffer, table.data.data(), table.data.size());
        // a2's low word, the upper half kept (a sign-extended size).
        ctx->r[6] = _mm_set_epi64x(_mm_extract_epi64(ctx->r[6], 1), static_cast<int64_t>(table.data.size()));
        if (!table.told)
        {
            table.told = true;
            std::fprintf(stderr, "[rg] Director's Cut balance: %s\n", table.name.c_str());
        }
        break;
    }
    g_runCfg(rdram, ctx, runtime);
}

void charaGetStatF3C(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t object = getRegU32(ctx, 4);
    const uint32_t index = getRegU32(ctx, 5);
    g_getStatF3C(rdram, ctx, runtime);
    if (index != kKnockdownResistance || object == 0)
    {
        return;
    }
    const uint32_t functions = readWord(rdram, object);
    if (functions == kBossTable || functions == kBossPartTable)
    {
        static bool told = false;
        if (!told)
        {
            told = true;
            std::fprintf(stderr, "[rg] Director's Cut balance: a %s, knockdown resistance %.2f, never knocked down\n",
                         functions == kBossTable ? "boss" : "boss part", ctx->f[0]);
        }
        ctx->f[0] = kNeverKnockedDown;
    }
}

} // namespace

void install(PS2Runtime &runtime)
{
    const char *env = std::getenv("RG_DIRECTOR_CUT_BALANCE");
    if (env && *env)
    {
        setEnabled(*env != '0');
    }
    if (!g_preferred.load(std::memory_order_relaxed))
    {
        return;
    }
    const std::filesystem::path folder = PS2Runtime::getIoPaths().cdRoot / "JA" / "balance";
    if (!loadTables(folder))
    {
        std::fprintf(stderr,
                     "[rg] the Director's Cut balance needs its tables in disc/JA/balance: run Install-Undub.bat "
                     "with your Japanese Director's Cut (or python setup.py director-cut-balance --iso <it>); "
                     "the balance stays the US one\n");
        return;
    }
    if (!rg::timing::hookFunction(runtime, kDataTableRunCfg, &dataTableRunCfg, g_runCfg))
    {
        std::fprintf(stderr, "[rg] the Director's Cut balance is off: its tables' hook is missing\n");
        return;
    }
    if (!rg::timing::hookFunction(runtime, kCharaGetStatF3C, &charaGetStatF3C, g_getStatF3C))
    {
        std::fprintf(stderr, "[rg] Director's Cut balance: bosses can still be knocked down (hook missing)\n");
    }
    std::fprintf(stderr, "[rg] Director's Cut balance on: %zu tables from %s, bosses never knocked down\n",
                 g_tables.size(), folder.string().c_str());
}

void setEnabled(bool on)
{
    g_preferred.store(on, std::memory_order_relaxed);
}

} // namespace rg::dcbalance
