#include "dc_balance.h"

#include <algorithm>
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
#include "return_hook.h"
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
// CATCH, the Director's Cut's character flag (see dc_balance.h).
constexpr uint32_t kParseFlagNames = 0x001CA700u;     // (text, table, found), the names' values ORed
constexpr uint32_t kCharaFlagsLowTable = 0x0044C1F0u; // the flags word's low half
constexpr uint32_t kCharaJumpToTarget = 0x00211070u;  // act.cfg's lock-on jump
constexpr uint32_t kActionMgrFindById = 0x00233B50u;  // its target's character...
constexpr uint32_t kJumpTargetFound = 0x00211150u;    // ...returning here
constexpr uint32_t kCharaFlags = 0x3D0u;              // u64, rebuilt every frame
constexpr uint32_t kTextLanguage = 0x00481528u;       // the code a path's `?` becomes ("EN")
constexpr uint64_t kFly = 1ull << 51;                 // ENEMY_FLY's high half, 0x80000
constexpr uint64_t kCatch = 0x8000u;                  // the low half's bit the US leaves free

struct Table
{
    std::string name;
    std::string language; // its text's code from the name (monster_param_FR.cfg.bin), "" for every text
    uint32_t usSize = 0;   // the table it replaces: US English, or a European text's
    uint32_t usCrc = 0;
    std::vector<uint8_t> data;
    bool told = false;
};

struct File
{
    uint32_t sector = 0; // from the start of files.dat
    uint32_t size = 0;
};

// The menu's three switches (dc_balance.h).
std::atomic<bool> g_balance{false};
std::atomic<bool> g_enemyBehavior{false};
std::atomic<bool> g_partyAi{false};
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
PS2Runtime::RecompiledFunction g_parseFlags = nullptr;
PS2Runtime::RecompiledFunction g_jumpToTarget = nullptr;
PS2Runtime::RecompiledFunction g_findAction = nullptr;
uint32_t g_flyLent = 0; // the character given FLY for one lock-on jump's test

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

// Guest addresses from the game's registers, checked against main RAM: one
// outside it (the scratchpad, a bad pointer) reads 0, writes nothing.
bool inRam(uint32_t address, uint32_t size)
{
    const uint32_t at = address & 0x1FFFFFFFu;
    return at <= PS2_RAM_SIZE && size <= PS2_RAM_SIZE - at;
}

uint32_t readWord(const uint8_t *rdram, uint32_t address)
{
    uint32_t value = 0;
    if (inRam(address, sizeof(value)))
    {
        std::memcpy(&value, rdram + (address & 0x1FFFFFFFu), sizeof(value));
    }
    return value;
}

// A NUL-terminated guest string, up to `limit` characters.
std::string guestString(const uint8_t *rdram, uint32_t address, size_t limit)
{
    std::string out;
    for (uint32_t at = address; out.size() < limit && inRam(at, 1u); ++at)
    {
        const char c = static_cast<char>(rdram[at & 0x1FFFFFFFu]);
        if (c == '\0')
        {
            break;
        }
        out += c;
    }
    return out;
}

// A table's text language from its name, as the setup step names them after
// the param pack's entries: "EN" for monster_param_EN.cfg.bin, "FR" for
// monster_param_FR.cfg.bin, "" for a table every text shares (btl_eff_param3).
std::string languageOf(const std::string &name)
{
    const size_t dot = name.find(".cfg.bin");
    if (dot == std::string::npos || dot < 3 || name[dot - 3] != '_')
    {
        return {};
    }
    std::string code = name.substr(dot - 2, 2);
    for (char &c : code)
    {
        if (!std::isalpha(static_cast<unsigned char>(c)))
        {
            return {};
        }
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return code;
}

// disc/JA/balance/tables.txt: "# ..." comments, then one table a line: its
// file name, the size and CRC-32 (hex) of the table it replaces, the file's
// size. A line or a file that is not right is left out, with a line in the log.
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
            continue;
        }
        table.usCrc = static_cast<uint32_t>(std::strtoul(crc.c_str(), nullptr, 16));
        table.language = languageOf(table.name);
        std::ifstream file(folder / table.name, std::ios::binary);
        table.data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        if (!file.good() && !file.eof())
        {
            table.data.clear();
        }
        // A table runs in place of the one it replaces, in its buffer: never larger.
        // One that is not whole is left out; the others still apply.
        if (table.data.size() != size || size == 0 || size > table.usSize)
        {
            std::fprintf(stderr, "[rg] Director's Cut balance: %s is missing or not whole, left out; run "
                                 "Install-Undub.bat again\n",
                         table.name.c_str());
            continue;
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
    if (inRam(address, sizeof(value)))
    {
        std::memcpy(rdram + (address & 0x1FFFFFFFu), &value, sizeof(value));
    }
}

// An archive path as the index names it: lower case, backslashes, no device,
// version or leading separator.
std::string normalize(const std::string &path)
{
    std::string out;
    for (const char c : path)
    {
        if (c == ';')
        {
            break;
        }
        out += c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const size_t start = out.find_first_not_of('\\');
    return start == std::string::npos ? std::string() : out.substr(start);
}

// The party's AI scripts (c001-c009) among the files; the rest are the enemies'.
bool isPartyScript(const std::string &path)
{
    return path.rfind("chara\\spt\\c", 0) == 0;
}

// disc/JA/balance/files.idx: "# ..." comments, then one file a line: its
// archive path, its first sector in files.dat and its size, tab-separated.
// Only the enemies' files, the party's scripts or both are kept.
bool loadFiles(const std::filesystem::path &folder, bool enemies, bool party)
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
        const std::string path = normalize(line.substr(0, tab1));
        if (isPartyScript(path) ? party : enemies)
        {
            files[path] = file;
        }
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
    if (pathAddress != 0 && entry != 0 && inRam(entry, 0x14u))
    {
        const std::string name = normalize(guestString(rdram, pathAddress, 512u));
        std::lock_guard<std::mutex> lock(g_filesMutex);
        if (!g_filesLooked)
        {
            g_filesLooked = true;
            g_filesLbn = filesLbn(rdram, ctx);
            if (!g_filesLbn)
            {
                std::fprintf(stderr, "[rg] Director's Cut: files.dat cannot be opened; the enemies' attacks and "
                                     "AI and the party's AI stay the US ones\n");
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
                std::fprintf(stderr, "[rg] Director's Cut: %s\n", name.c_str());
            }
            setReturnU32(ctx, 1);
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
    }
    g_findFile(rdram, ctx, runtime);
}

uint64_t readDword(const uint8_t *rdram, uint32_t address)
{
    uint64_t value = 0;
    if (inRam(address, sizeof(value)))
    {
        std::memcpy(&value, rdram + (address & 0x1FFFFFFFu), sizeof(value));
    }
    return value;
}

void writeDword(uint8_t *rdram, uint32_t address, uint64_t value)
{
    if (inRam(address, sizeof(value)))
    {
        std::memcpy(rdram + (address & 0x1FFFFFFFu), &value, sizeof(value));
    }
}

// The flag names the Director's Cut's table gives the low half's 0x8000: CATCH
// (new) and ENEMY_FLY (which also keeps FLY in the high half).
constexpr uint32_t kHasCatch = 1u;

void afterParseFlags(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t data)
{
    (void)rdram;
    (void)runtime;
    if (data & kHasCatch)
    {
        // OR into v0 as it is: the original's sign extension stays.
        ctx->r[2] = _mm_or_si128(ctx->r[2], _mm_set_epi64x(0, static_cast<int64_t>(kCatch)));
    }
}

// ParseFlagNames (0x1CA700): the names of a "A|B|C" text looked up in a table
// of {name, value}, their values ORed; one the table lacks adds nothing. Its
// callers with the low-half table pass no `found`, so only the value changes.
void parseFlagNames(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t text = getRegU32(ctx, 4);
    uint32_t data = 0;
    if (text != 0 && getRegU32(ctx, 5) == kCharaFlagsLowTable)
    {
        // The parser's own words: letters, digits and '_', in any case (it
        // compares them lower-cased).
        const std::string names = guestString(rdram, text, 4096u);
        std::string word;
        for (size_t i = 0; i <= names.size(); ++i)
        {
            const char c = i < names.size() ? names[i] : '\0';
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            {
                word += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                continue;
            }
            if (word == "CATCH" || word == "ENEMY_FLY")
            {
                data = kHasCatch;
            }
            word.clear();
        }
    }
    if (data == 0)
    {
        g_parseFlags(rdram, ctx, runtime);
        return;
    }
    rg::hooks::callThen(g_parseFlags, rdram, ctx, runtime, &afterParseFlags, data);
}

// The lock-on jump (CharaJumpToTarget, 0x211070) takes its target's character
// from ActionMgrFindById and homes in only when it has FLY; the Director's
// Cut's version also when it has CATCH. Such a character gets FLY between the
// lookup's return and the jump's end, the test's only read of it. A jump
// whose end never comes (its thread exited while it was unwound) leaves its
// lend behind: the next jump forgets it without writing to that character,
// which may be gone, and whose flags the game rebuilds every frame anyway.
void returnFly(uint8_t *rdram)
{
    if (g_flyLent != 0)
    {
        const uint32_t chara = g_flyLent;
        g_flyLent = 0;
        writeDword(rdram, chara + kCharaFlags, readDword(rdram, chara + kCharaFlags) & ~kFly);
    }
}

void afterFindAction(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t data)
{
    (void)runtime;
    (void)data;
    const uint32_t chara = getRegU32(ctx, 2);
    if (chara == 0)
    {
        return;
    }
    const uint64_t flags = readDword(rdram, chara + kCharaFlags);
    if ((flags & kCatch) != 0 && (flags & kFly) == 0)
    {
        writeDword(rdram, chara + kCharaFlags, flags | kFly);
        g_flyLent = chara;
        static bool told = false;
        if (!told)
        {
            told = true;
            std::fprintf(stderr, "[rg] Director's Cut enemy behavior: a lock-on jump homes in on a CATCH target\n");
        }
    }
}

void actionMgrFindById(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (getRegU32(ctx, 31) != kJumpTargetFound)
    {
        g_findAction(rdram, ctx, runtime);
        return;
    }
    rg::hooks::callThen(g_findAction, rdram, ctx, runtime, &afterFindAction, 0u);
}

void afterJumpToTarget(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t data)
{
    (void)ctx;
    (void)runtime;
    (void)data;
    returnFly(rdram);
}

void charaJumpToTarget(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_flyLent = 0; // a lend whose jump never ended
    rg::hooks::callThen(g_jumpToTarget, rdram, ctx, runtime, &afterJumpToTarget, 0u);
}

// The game's text language: the code its paths' `?` becomes ("EN"; another
// once a text in another language is served, at boot). Its param pack, and so
// its tables, are that language's, with their names: the balance replaces them
// with its own for that language (monster_param_FR.cfg.bin and so on), made at
// install when that text was there. A text with none gets a line in the log.
std::string currentLanguage(const uint8_t *rdram)
{
    const uint32_t code = readWord(rdram, kTextLanguage);
    std::string name = code ? guestString(rdram, code, 4u) : std::string();
    for (char &c : name)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return name.empty() ? std::string("EN") : name;
}

void tellIfNoTables(const std::string &language)
{
    static bool told = false;
    if (told || language == "EN")
    {
        return;
    }
    told = true;
    for (const Table &table : g_tables)
    {
        if (table.language == language)
        {
            return;
        }
    }
    std::fprintf(stderr,
                 "[rg] Director's Cut balance: no tables for the text in %s, so the enemies' numbers stay the US "
                 "ones (the boss rule still applies): Install-European.bat or Update.bat makes them, or "
                 "Install-Undub.bat again\n",
                 language.c_str());
}

void dataTableRunCfg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const std::string language = currentLanguage(rdram);
    tellIfNoTables(language);
    const uint32_t buffer = getRegU32(ctx, 5) & 0x1FFFFFFFu;
    const uint32_t size = getRegU32(ctx, 6);
    for (Table &table : g_tables)
    {
        if ((!table.language.empty() && table.language != language) || size != table.usSize || buffer == 0 || buffer > PS2_RAM_SIZE || size > PS2_RAM_SIZE - buffer)
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

// RG_DIRECTOR_CUT_BALANCE and the two others, over the menu: 1 on, 0 off.
void fromEnv(const char *name, std::atomic<bool> &on)
{
    const char *env = std::getenv(name);
    if (env && *env)
    {
        on.store(*env != '0', std::memory_order_relaxed);
    }
}

// The numbers: the tables, and the boss rule with them. What is on, for the
// log's last line; empty when nothing is.
std::string installBalance(PS2Runtime &runtime, const std::filesystem::path &folder)
{
    if (!loadTables(folder))
    {
        std::fprintf(stderr,
                     "[rg] the Director's Cut balance needs its tables in disc/JA/balance: run Install-Undub.bat "
                     "with your Japanese Director's Cut (or python setup.py director-cut-balance --iso <it>); "
                     "the balance stays the US one\n");
        return {};
    }
    if (!rg::timing::hookFunction(runtime, kDataTableRunCfg, &dataTableRunCfg, g_runCfg))
    {
        g_tables.clear();
        std::fprintf(stderr, "[rg] the Director's Cut balance is off: its tables' hook is missing\n");
        return {};
    }
    const bool bossRule = rg::timing::hookFunction(runtime, kCharaGetStatF3C, &charaGetStatF3C, g_getStatF3C);
    if (!bossRule)
    {
        std::fprintf(stderr, "[rg] Director's Cut balance: bosses can still be knocked down (hook missing)\n");
    }
    std::vector<std::string> languages;
    for (const Table &table : g_tables)
    {
        if (!table.language.empty() &&
            std::find(languages.begin(), languages.end(), table.language) == languages.end())
        {
            languages.push_back(table.language);
        }
    }
    std::string list;
    for (const std::string &language : languages)
    {
        list += (list.empty() ? "" : ", ") + language;
    }
    return "balance (tables for " + (list.empty() ? std::string("every text") : list) + ")" +
           (bossRule ? ", bosses never knocked down" : "");
}

// The enemies' files, the party's scripts or both, served from files.dat.
bool installFiles(PS2Runtime &runtime, const std::filesystem::path &folder, bool enemies, bool party)
{
    const char *what = enemies && party ? "enemy behavior and party AI" : enemies ? "enemy behavior" : "party AI";
    if (!loadFiles(folder, enemies, party))
    {
        std::fprintf(stderr,
                     "[rg] the Director's Cut %s needs files.idx and files.dat in disc/JA/balance: run "
                     "Install-Undub.bat again; it stays the US one\n",
                     what);
        return false;
    }
    if (!rg::timing::hookFunction(runtime, kDirTableFindFile, &dirTableFindFile, g_findFile))
    {
        g_files.clear();
        std::fprintf(stderr, "[rg] the Director's Cut %s is off: the file lookup's hook is missing\n", what);
        return false;
    }
    return true;
}

} // namespace

void install(PS2Runtime &runtime)
{
    fromEnv("RG_DIRECTOR_CUT_BALANCE", g_balance);
    fromEnv("RG_DIRECTOR_CUT_ENEMIES", g_enemyBehavior);
    fromEnv("RG_DIRECTOR_CUT_PARTY_AI", g_partyAi);
    const bool balance = g_balance.load(std::memory_order_relaxed);
    const bool enemies = g_enemyBehavior.load(std::memory_order_relaxed);
    const bool party = g_partyAi.load(std::memory_order_relaxed);
    if (!balance && !enemies && !party)
    {
        return;
    }
    g_runtime = &runtime;
    const std::filesystem::path folder = PS2Runtime::getIoPaths().cdRoot / "JA" / "balance";
    std::vector<std::string> on;
    if (balance)
    {
        std::string what = installBalance(runtime, folder);
        if (!what.empty())
        {
            on.push_back(std::move(what));
        }
    }
    if ((enemies || party) && installFiles(runtime, folder, enemies, party))
    {
        size_t partyScripts = 0;
        for (const auto &file : g_files)
        {
            partyScripts += isPartyScript(file.first) ? 1u : 0u;
        }
        if (enemies)
        {
            std::string what = "enemy behavior (" + std::to_string(g_files.size() - partyScripts) + " files";
            // The jump's hook first: it is the one that takes FLY back.
            if (rg::timing::hookFunction(runtime, kParseFlagNames, &parseFlagNames, g_parseFlags) &&
                rg::timing::hookFunction(runtime, kCharaJumpToTarget, &charaJumpToTarget, g_jumpToTarget) &&
                rg::timing::hookFunction(runtime, kActionMgrFindById, &actionMgrFindById, g_findAction))
            {
                what += ", CATCH";
            }
            else
            {
                std::fprintf(stderr, "[rg] Director's Cut enemy behavior: lock-on jumps home in on flyers only, "
                                     "as in the US (hook missing)\n");
            }
            on.push_back(what + ")");
        }
        if (party)
        {
            on.push_back("party AI (" + std::to_string(partyScripts) + " scripts)");
        }
    }
    if (on.empty())
    {
        return;
    }
    std::string list;
    for (const std::string &what : on)
    {
        list += (list.empty() ? "" : ", ") + what;
    }
    std::fprintf(stderr, "[rg] Director's Cut on, from %s: %s\n", folder.string().c_str(), list.c_str());
}

void setBalance(bool on)
{
    g_balance.store(on, std::memory_order_relaxed);
}

void setEnemyBehavior(bool on)
{
    g_enemyBehavior.store(on, std::memory_order_relaxed);
}

void setPartyAi(bool on)
{
    g_partyAi.store(on, std::memory_order_relaxed);
}

} // namespace rg::dcbalance
