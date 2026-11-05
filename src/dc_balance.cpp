#include "dc_balance.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
constexpr uint32_t kDirTableFindFile = 0x00182F70u; // (table, entry, path), one archive's index
constexpr uint32_t kSceCdSearchFile = 0x00129AE0u;  // (file, path), the runtime's stub
constexpr uint32_t kKnockdownResistance = 1u;       // CharaGetStatF3C's index
constexpr uint32_t kBossTable = 0x00474B20u;        // class 2501
constexpr uint32_t kBossPartTable = 0x004746C0u;    // class 2502
constexpr float kNeverKnockedDown = 10.0f;
constexpr uint32_t kSectorSize = 2048u;
constexpr const char *kFilesDat = "cdrom0:\\JA\\BALANCE\\FILES.DAT;1";

struct Table
{
    std::string name;
    uint32_t usSize = 0;
    uint32_t usCrc = 0;
    std::vector<uint8_t> data;
    bool told = false;
};

struct File
{
    uint32_t sector = 0; // from the start of files.dat
    uint32_t size = 0;
};

std::atomic<bool> g_preferred{false};
std::vector<Table> g_tables;
// files.idx: archive path (lower case, backslashes) -> where in files.dat.
std::unordered_map<std::string, File> g_files;
std::mutex g_filesMutex;
bool g_filesLooked = false;
uint32_t g_filesLbn = 0; // files.dat's first sector, as the runtime numbers the disc folder
std::unordered_set<std::string> g_filesTold;
PS2Runtime *g_runtime = nullptr;
PS2Runtime::RecompiledFunction g_runCfg = nullptr;
PS2Runtime::RecompiledFunction g_getStatF3C = nullptr;
PS2Runtime::RecompiledFunction g_findFile = nullptr;

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

void writeWord(uint8_t *rdram, uint32_t address, uint32_t value)
{
    std::memcpy(rdram + (address & 0x1FFFFFFFu), &value, sizeof(value));
}

// An archive path as the index names it: lower case, backslashes, no device,
// version or leading separator.
std::string normalize(const char *path)
{
    std::string out;
    for (const char *p = path; *p && *p != ';'; ++p)
    {
        out += *p == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    }
    const size_t start = out.find_first_not_of('\\');
    return start == std::string::npos ? std::string() : out.substr(start);
}

// disc/JA/balance/files.idx: "# ..." comments, then one file a line: its
// archive path, its first sector in files.dat and its size, tab-separated.
bool loadFiles(const std::filesystem::path &folder)
{
    std::ifstream list(folder / "files.idx");
    if (!list)
    {
        return false;
    }
    std::error_code ec;
    const uint64_t datSize = std::filesystem::file_size(folder / "files.dat", ec);
    if (ec)
    {
        return false;
    }
    std::unordered_map<std::string, File> files;
    std::string line;
    while (std::getline(list, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        const size_t tab1 = line.find('\t');
        const size_t tab2 = tab1 == std::string::npos ? std::string::npos : line.find('\t', tab1 + 1);
        if (tab2 == std::string::npos)
        {
            return false;
        }
        File file;
        file.sector = static_cast<uint32_t>(std::strtoul(line.c_str() + tab1 + 1, nullptr, 10));
        file.size = static_cast<uint32_t>(std::strtoul(line.c_str() + tab2 + 1, nullptr, 10));
        if (file.size == 0 || static_cast<uint64_t>(file.sector) * kSectorSize + file.size > datSize)
        {
            return false;
        }
        files[normalize(line.substr(0, tab1).c_str())] = file;
    }
    if (files.empty())
    {
        return false;
    }
    g_files = std::move(files);
    return true;
}

// The sector number the runtime gives files.dat: the call the game makes to
// open an archive, made once at the first lookup (at boot, before any stream
// plays: the search moves the runtime's streaming position).
uint32_t filesLbn(uint8_t *rdram, R5900Context *caller)
{
    PS2Runtime::RecompiledFunction search = g_runtime->lookupFunction(kSceCdSearchFile);
    const uint32_t scratch = search ? g_runtime->guestMalloc(0x200u, 16u) : 0u;
    if (!scratch)
    {
        return 0;
    }
    const size_t length = std::strlen(kFilesDat);
    std::memset(rdram + (scratch & 0x1FFFFFFFu), 0, 0x200);
    std::memcpy(rdram + ((scratch + 0x40u) & 0x1FFFFFFFu), kFilesDat, length + 1);
    R5900Context local = *caller;
    local.r[4] = _mm_set_epi64x(0, scratch);
    local.r[5] = _mm_set_epi64x(0, scratch + 0x40u);
    search(rdram, &local, g_runtime);
    const uint32_t lbn = readWord(rdram, scratch); // sceCdlFILE: lsn, size, name
    g_runtime->guestFree(scratch);
    return lbn;
}

// The game asks an archive's index for a file (the DATA archives' HD6); a file
// of files.idx gets its place in files.dat instead, read like any archive's.
void dirTableFindFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t entry = getRegU32(ctx, 5);
    const uint32_t pathAddress = getRegU32(ctx, 6);
    if (pathAddress != 0 && entry != 0)
    {
        const std::string name = normalize(reinterpret_cast<const char *>(rdram + (pathAddress & 0x1FFFFFFFu)));
        std::lock_guard<std::mutex> lock(g_filesMutex);
        if (!g_filesLooked)
        {
            g_filesLooked = true;
            g_filesLbn = filesLbn(rdram, ctx);
            if (!g_filesLbn)
            {
                std::fprintf(stderr, "[rg] Director's Cut balance: files.dat cannot be opened; the enemies' "
                                     "attacks and AI stay the US ones\n");
            }
        }
        const auto found = g_filesLbn ? g_files.find(name) : g_files.end();
        if (found != g_files.end())
        {
            // The index entry: +4 the first sector, +8 the size.
            writeWord(rdram, entry + 0x0u, 0);
            writeWord(rdram, entry + 0x4u, g_filesLbn + found->second.sector);
            writeWord(rdram, entry + 0x8u, found->second.size);
            writeWord(rdram, entry + 0xCu, 0);
            writeWord(rdram, entry + 0x10u, 0);
            if (g_filesTold.insert(name).second)
            {
                std::fprintf(stderr, "[rg] Director's Cut balance: %s\n", name.c_str());
            }
            setReturnU32(ctx, 1);
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
    }
    g_findFile(rdram, ctx, runtime);
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
    g_runtime = &runtime;
    if (!loadFiles(folder))
    {
        std::fprintf(stderr, "[rg] Director's Cut balance: the enemies' attacks and AI stay the US ones, "
                             "files.idx/files.dat missing or not whole: run Install-Undub.bat again\n");
    }
    else if (!rg::timing::hookFunction(runtime, kDirTableFindFile, &dirTableFindFile, g_findFile))
    {
        g_files.clear();
        std::fprintf(stderr, "[rg] Director's Cut balance: the enemies' attacks and AI stay the US ones "
                             "(hook missing)\n");
    }
    std::fprintf(stderr,
                 "[rg] Director's Cut balance on: %zu tables and %zu enemy files and AI scripts from %s, "
                 "bosses never knocked down\n",
                 g_tables.size(), g_files.size(), folder.string().c_str());
}

void setEnabled(bool on)
{
    g_preferred.store(on, std::memory_order_relaxed);
}

} // namespace rg::dcbalance
