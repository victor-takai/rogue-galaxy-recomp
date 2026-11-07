#include "text_language.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "return_hook.h"
#include "scene_timing.h"
#include "widescreen.h"

namespace rg::textlanguage
{
namespace
{

constexpr uint32_t kDirTableFindFile = 0x00182F70u;       // (table, entry, path), one archive's index
constexpr uint32_t kArchiveFindEntryByName = 0x00181E20u; // (archive, name), an entry of a pack
constexpr uint32_t kTextSymbolTagFind = 0x0026EFE0u;      // (str), the symbol tag at its start
constexpr uint32_t kFontGetGlyphWidth = 0x0026F420u;      // (font, code)
constexpr uint32_t kFontGetCharWidth = 0x0026F4C0u;       // (font, code), from the width table
constexpr uint32_t kFontGetLineHeight = 0x0026F3E0u;      // (font)
constexpr uint32_t kFontGetGlyphRect = 0x0026E040u;       // (out, font, code): x, y, w, h on its page
constexpr uint32_t kSceCdSearchFile = 0x00129AE0u;        // (file, path), the runtime's stub
constexpr uint32_t kTextLanguage = 0x00481528u;           // the string `?` becomes
constexpr uint32_t kLanguageTable = 0x0044AF38u;          // pointers to "EN", "ES", ...
constexpr int kLanguageTableSize = 12;
constexpr uint32_t kDiscLayer = 0x004811E0u; // set by CdDatArchivesOpenOnLayer
constexpr const char *kFolder = "EU";

// The menu's choices, in its order: 0 is the game's English.
constexpr const char *kCodes[] = {"", "FR", "DE", "ES", "IT"};
std::atomic<int> g_preferred{0};
// The one of them the text is in (another code RG_TEXT_LANG picked counts as 0).
std::atomic<int> g_language{0};

// The PAL executable gives its menus and game data more room, for the longer European
// layouts and texts. Found by pairing the two executables' functions and comparing
// their constants; sizes in 16-byte blocks.
constexpr uint32_t kMenuResStackSize = 0x00202610u;  // US 0x1600, PAL 0x1900
constexpr uint32_t kGameDataStackSize = 0x00202620u; // US 0x11A00, PAL 0x12A00
constexpr uint32_t kResGroupStackInit = 0x0042C600u; // (group, parent, index, name, size)
struct Resize
{
    uint32_t us;
    uint32_t pal;
};
// ResGroupStackInit's sizes: the menu resource group 0 (MenuResGroupStacksInit) and
// the main menu's layout (ResGroupLoadByKind, kind 0, main_menu_def5).
constexpr Resize kGroupSizes[] = {{0x1600u, 0x1900u}, {0x2D80u, 0x2F00u}};

PS2Runtime::RecompiledFunction g_original = nullptr;
PS2Runtime::RecompiledFunction g_originalFindEntry = nullptr;
PS2Runtime::RecompiledFunction g_originalSymbolFind = nullptr;
PS2Runtime::RecompiledFunction g_originalGlyphWidth = nullptr;
PS2Runtime::RecompiledFunction g_originalCharWidth = nullptr;
PS2Runtime::RecompiledFunction g_originalLineHeight = nullptr;
PS2Runtime::RecompiledFunction g_originalGlyphRect = nullptr;
PS2Runtime::RecompiledFunction g_originalMenuResSize = nullptr;
PS2Runtime::RecompiledFunction g_originalGameDataSize = nullptr;
PS2Runtime::RecompiledFunction g_originalGroupInit = nullptr;
PS2Runtime::RecompiledFunction g_originalUiLayout = nullptr;
PS2Runtime *g_runtime = nullptr;
bool g_active = false;
std::string g_code; // lower case, "fr"

struct Entry
{
    uint32_t sector = 0; // from the start of the DAT
    uint32_t size = 0;
};

// An archive's index, its names lower case with backslashes: the US disc's
// DATA<layer>_0, or the language's own <LANG>.DAT.
struct Index
{
    bool loaded = false;
    bool ok = false;
    uint32_t baseLbn = 0;
    std::unordered_map<std::string, Entry> files;
};

Index g_us[2];
// The language's files as the US executable wants them (setup.py extract-european,
// tools/disc/european.py): disc/EU/<LANG>.DAT, each file on sectors of its own, and
// disc/EU/<LANG>.idx, a line a file: its archive path, first sector, size.
Index g_text;
std::mutex g_mutex;
int g_logged = 0;
// The Attack Chain's pictures with the language's words (european.py adds the
// pack to texts built since): the PAL places for them only then.
constexpr const char *kChainPack = "chn\\common.pac";
std::atomic<bool> g_chainPictures{false};

uint32_t readWord(const uint8_t *rdram, uint32_t address)
{
    uint32_t value = 0;
    std::memcpy(&value, rdram + (address & 0x1FFFFFFFu), sizeof(value));
    return value;
}

void writeWord(uint8_t *rdram, uint32_t address, uint32_t value)
{
    std::memcpy(rdram + (address & 0x1FFFFFFFu), &value, sizeof(value));
}

std::string normalize(const char *path)
{
    std::string out;
    for (const char *p = path; *p; ++p)
    {
        if (*p == ';')
        {
            break;
        }
        out += *p == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    }
    while (!out.empty() && out.front() == '\\')
    {
        out.erase(out.begin());
    }
    return out;
}

std::string upperCode()
{
    std::string out;
    for (char c : g_code)
    {
        out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

std::filesystem::path languageIndex(const std::string &upper)
{
    return PS2Runtime::getIoPaths().cdRoot / kFolder / (upper + ".idx");
}

// The HD6 index: header, token pool, names as LEB128 token ids, one u64 a file
// (18 bit name offset, 22 bit sector in the DAT, 24 bit size in `unit` bytes),
// as the game reads it in DirTableFindFile.
bool parseHd6(const std::filesystem::path &file, Index &index)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        return false;
    }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (d.size() < 0x34 || std::memcmp(d.data(), "HD6\0", 4) != 0)
    {
        return false;
    }
    auto word = [&](size_t at) {
        uint32_t v = 0;
        std::memcpy(&v, d.data() + at, 4);
        return v;
    };
    const uint32_t tokenOffset = word(4), tokenSize = word(8), tokenCount = word(0xC);
    const uint32_t nameOffset = word(0x14), unit = word(0x20), count = word(0x24), table = word(0x28);
    if (word(0x30) != d.size() || tokenOffset + tokenSize > d.size() || table + 8ull * count > d.size())
    {
        return false;
    }
    std::vector<std::string> tokens;
    size_t at = tokenOffset;
    for (uint32_t i = 0; i < tokenCount && at < tokenOffset + tokenSize; ++i)
    {
        const size_t end = std::find(d.begin() + at, d.begin() + tokenOffset + tokenSize, 0) - d.begin();
        tokens.emplace_back(reinterpret_cast<const char *>(d.data() + at), end - at);
        at = end + 1;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        uint64_t v = 0;
        std::memcpy(&v, d.data() + table + 8ull * i, 8);
        size_t p = nameOffset + (v & 0x3FFFF);
        std::string name;
        while (p < d.size() && d[p])
        {
            uint32_t id = 0;
            int shift = 0;
            while (p < d.size())
            {
                const uint8_t b = d[p++];
                id |= static_cast<uint32_t>(b & 0x7F) << shift;
                shift += 7;
                if (!(b & 0x80))
                {
                    break;
                }
            }
            if (id < tokens.size())
            {
                name += tokens[id];
            }
        }
        Entry entry;
        entry.sector = static_cast<uint32_t>((v >> 18) & 0x3FFFFF);
        entry.size = static_cast<uint32_t>((v >> 40) * unit);
        index.files[normalize(name.c_str())] = entry;
    }
    return true;
}

// The pseudo sector number the runtime gives a file of the disc folder: the
// same call the game makes to open an archive.
uint32_t discSector(uint8_t *rdram, R5900Context *caller, const std::string &path)
{
    PS2Runtime::RecompiledFunction search = g_runtime->lookupFunction(kSceCdSearchFile);
    if (!search)
    {
        return 0;
    }
    static uint32_t scratch = 0;
    if (!scratch)
    {
        scratch = g_runtime->guestMalloc(0x200u, 16u);
    }
    if (!scratch)
    {
        return 0;
    }
    std::memset(rdram + (scratch & 0x1FFFFFFFu), 0, 0x200);
    std::memcpy(rdram + ((scratch + 0x40u) & 0x1FFFFFFFu), path.c_str(), path.size() + 1);
    R5900Context local = *caller;
    local.r[4] = _mm_set_epi64x(0, scratch);
    local.r[5] = _mm_set_epi64x(0, scratch + 0x40u);
    search(rdram, &local, g_runtime);
    return readWord(rdram, scratch);
}

Index &usIndex(uint32_t layer)
{
    Index &index = g_us[layer & 1];
    if (!index.loaded)
    {
        index.loaded = true;
        index.ok = parseHd6(PS2Runtime::getIoPaths().cdRoot / ("DATA" + std::to_string(layer & 1) + "_0.HD6"), index);
    }
    return index;
}

// Read once, at the first lookup (at boot, before any stream plays: the search
// call moves the runtime's streaming position).
Index &textIndex(uint8_t *rdram, R5900Context *ctx)
{
    if (g_text.loaded)
    {
        return g_text;
    }
    g_text.loaded = true;
    const std::string upper = upperCode();
    std::ifstream in(languageIndex(upper));
    std::string line;
    while (in && std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        const size_t tab1 = line.find('\t');
        const size_t tab2 = tab1 == std::string::npos ? std::string::npos : line.find('\t', tab1 + 1);
        if (tab2 == std::string::npos)
        {
            continue;
        }
        Entry entry;
        entry.sector = static_cast<uint32_t>(std::strtoul(line.c_str() + tab1 + 1, nullptr, 10));
        entry.size = static_cast<uint32_t>(std::strtoul(line.c_str() + tab2 + 1, nullptr, 10));
        g_text.files[normalize(line.substr(0, tab1).c_str())] = entry;
    }
    if (!g_text.files.empty())
    {
        g_text.baseLbn = discSector(rdram, ctx, std::string("cdrom0:\\") + kFolder + "\\" + upper + ".DAT;1");
        g_text.ok = g_text.baseLbn != 0;
    }
    g_chainPictures.store(g_text.ok && g_text.files.count(kChainPack) != 0u, std::memory_order_relaxed);
    std::fprintf(stderr, "[rg] %s.idx: %zu files%s\n", upper.c_str(), g_text.files.size(),
                 g_text.ok ? "" : " (not usable)");
    if (g_text.ok && g_text.files.count(kChainPack) == 0u)
    {
        std::fprintf(stderr, "[rg] the Attack Chain keeps its English words: this text was built before "
                             "they came in (run Install-European.bat again)\n");
    }
    return g_text;
}

// The name with the language's code as English: `_fr.`, `\fr\` and `_fr_`, each
// two letters, so the guest's path can be rewritten in place at the same offsets.
std::string englishName(const std::string &name, std::vector<size_t> &at)
{
    std::string english = name;
    for (const std::string &pattern : {"_" + g_code + ".", "\\" + g_code + "\\", "_" + g_code + "_"})
    {
        for (size_t p = english.find(pattern); p != std::string::npos; p = english.find(pattern, p + 1))
        {
            english.replace(p + 1, g_code.size(), "en");
            at.push_back(p + 1);
        }
    }
    return english;
}

// Below: the European lists' widths.
void patchItemCommands(uint8_t *rdram);
void patchItemKindIcons(uint8_t *rdram);
void centrePartyLabel(R5900Context *ctx);

void dirTableFindFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t entryAddress = getRegU32(ctx, 5);
    const uint32_t pathAddress = getRegU32(ctx, 6);
    if (g_active && pathAddress != 0)
    {
        char *path = reinterpret_cast<char *>(rdram + (pathAddress & 0x1FFFFFFFu));
        const std::string name = normalize(path);
        std::lock_guard<std::mutex> lock(g_mutex);
        static const bool trace = std::getenv("RG_TEXT_TRACE") != nullptr;
        if (trace && g_logged < 600 && name.compare(0, 4, "wav\\") != 0)
        {
            ++g_logged;
            std::fprintf(stderr, "[rg] lookup: %s\n", name.c_str());
        }
        Index &text = textIndex(rdram, ctx);
        // The game's first file is found long before any menu opens; by
        // then the text's own files are known usable or not.
        patchItemCommands(rdram);
        patchItemKindIcons(rdram);
        const auto found = text.ok ? text.files.find(name) : text.files.end();
        if (found != text.files.end())
        {
            writeWord(rdram, entryAddress + 0x0, 0);
            writeWord(rdram, entryAddress + 0x4, text.baseLbn + found->second.sector);
            writeWord(rdram, entryAddress + 0x8, found->second.size);
            writeWord(rdram, entryAddress + 0xC, 0);
            writeWord(rdram, entryAddress + 0x10, 0);
            if (trace)
            {
                std::fprintf(stderr, "[rg] text file: %s\n", name.c_str());
            }
            setReturnU32(ctx, 1);
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
        // Not in the language's files (the PAL disc has no words for it): the
        // English one, under its name rewritten in place.
        std::vector<size_t> at;
        const std::string english = englishName(name, at);
        if (!at.empty())
        {
            Index &us = usIndex(readWord(rdram, kDiscLayer));
            if (us.ok && us.files.count(name) == 0 && us.files.count(english) != 0)
            {
                size_t lead = 0;
                while (path[lead] == '\\' || path[lead] == '/')
                {
                    ++lead;
                }
                for (size_t p : at)
                {
                    std::memcpy(path + lead + p, "en", 2);
                }
                if (g_logged < 100)
                {
                    ++g_logged;
                    std::fprintf(stderr, "[rg] in English: %s\n", english.c_str());
                }
            }
        }
    }
    g_original(rdram, ctx, runtime);
}

// An entry of a font pack is asked for by the text language (fonttbl_normal_FR.bin,
// FontTex_name_FR_0.tm2); the PAL font packs, one for the five languages, name them EUR.
void archiveFindEntryByName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t nameAddress = getRegU32(ctx, 5);
    if (g_active && nameAddress != 0)
    {
        char *name = reinterpret_cast<char *>(rdram + (nameAddress & 0x1FFFFFFFu));
        char *base = name;
        for (char *p = name; *p; ++p)
        {
            if (*p == '/')
            {
                base = p + 1;
            }
        }
        const std::string text = base;
        std::string lower;
        for (char c : text)
        {
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lower.rfind("fonttbl_", 0) == 0 || lower.rfind("fonttex_", 0) == 0)
        {
            const size_t at = lower.find("_" + g_code);
            const size_t after = at == std::string::npos ? 0 : at + 1 + g_code.size();
            if (at != std::string::npos && (after == lower.size() || lower[after] == '_' || lower[after] == '.'))
            {
                const std::string european = text.substr(0, at + 1) + "EUR" + text.substr(after);
                if (european.size() < 0x80)
                {
                    std::memcpy(base, european.c_str(), european.size() + 1);
                }
            }
        }
    }
    g_originalFindEntry(rdram, ctx, runtime);
}

// The European texts spell accents and a few marks as tags (<'e>, <degree>). The PAL
// executable matches them in a table of 64 and gives the nth the glyph code 107 + n,
// the cell where its font page draws that mark; the US one has no such table, so the
// tag would print as it is. The names are in the PAL table's order (0x45C420).
constexpr const char *kAccentTags[] = {
    "<9>", "<6>", "<66>", "<99>", "<99G>", "<->", "<-->", "<--->", "<^!>", "<^?>", "<<<>", "<>>>",
    "<degree>", "<OE>", "<oe>", "<`A>", "<'A>", "<^A>", "<:A>", "<,C>", "<`E>", "<'E>", "<^E>", "<:E>",
    "<`I>", "<'I>", "<^I>", "<:I>", "<~N>", "<`O>", "<'O>", "<^O>", "<:O>", "<`U>", "<'U>", "<^U>",
    "<:U>", "<`a>", "<'a>", "<^a>", "<:a>", "<,c>", "<`e>", "<'e>", "<^e>", "<:e>", "<`i>", "<'i>",
    "<^i>", "<:i>", "<~n>", "<`o>", "<'o>", "<^o>", "<:o>", "<~O>", "<~o>", "<`u>", "<'u>", "<^u>",
    "<:u>", "<ss>", "<a>", "<0>",
};
constexpr int kAccentCount = static_cast<int>(sizeof(kAccentTags) / sizeof(kAccentTags[0]));
constexpr uint32_t kAccentFirstCode = 107u;
// The glyph width of each tag in the three fonts, as the PAL table has it. The US width
// table (a byte per font at 0x44E851 + 4 * code) ends at code 106: right after it lies the
// table of the button tags ((X), [r1], ...), so these widths are answered, not written there.
constexpr uint8_t kAccentWidths[kAccentCount][3] ={{6, 6, 4}, {6, 6, 4}, {6, 6, 6}, {6, 6, 6}, {6, 6, 6}, {10, 10, 8}, {14, 12, 12}, {16, 16, 14}, {6, 4, 4}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {6, 6, 6}, {16, 16, 12}, {16, 16, 12}, {14, 16, 12}, {14, 16, 12}, {14, 16, 12}, {14, 16, 12}, {14, 14, 10}, {12, 12, 8}, {12, 12, 8}, {12, 12, 8}, {12, 12, 8}, {10, 8, 8}, {10, 8, 8}, {12, 10, 8}, {12, 10, 8}, {12, 12, 10}, {14, 14, 10}, {14, 14, 10}, {14, 14, 10}, {14, 14, 10}, {14, 12, 10}, {14, 12, 10}, {14, 12, 10}, {14, 12, 10}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {10, 8, 8}, {10, 8, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {14, 14, 10}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {10, 10, 8}, {10, 10, 8}};
uint32_t g_accentEntries = 0; // guest copy of the table: {name, length (s16), code (u16)} each

// TextSymbolTagFind (str): the table entry of a tag at the start of str, or null.
void textSymbolTagFind(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t textAddress = getRegU32(ctx, 4);
    if (g_active && textAddress != 0)
    {
        const char *text = reinterpret_cast<const char *>(rdram + (textAddress & 0x1FFFFFFFu));
        if (text[0] == '<')
        {
            if (!g_accentEntries)
            {
                g_accentEntries = g_runtime->guestMalloc(8u * kAccentCount, 16u);
                for (int i = 0; g_accentEntries && i < kAccentCount; ++i)
                {
                    const uint32_t length = static_cast<uint32_t>(std::strlen(kAccentTags[i]));
                    const uint32_t entry = g_accentEntries + 8u * static_cast<uint32_t>(i);
                    writeWord(rdram, entry, 0);
                    writeWord(rdram, entry + 4u, ((kAccentFirstCode + static_cast<uint32_t>(i)) << 16) | length);
                }
            }
            for (int i = 0; g_accentEntries && i < kAccentCount; ++i)
            {
                if (std::strncmp(text, kAccentTags[i], std::strlen(kAccentTags[i])) == 0)
                {
                    setReturnU32(ctx, g_accentEntries + 8u * static_cast<uint32_t>(i));
                    ctx->pc = getRegU32(ctx, 31);
                    return;
                }
            }
        }
    }
    g_originalSymbolFind(rdram, ctx, runtime);
}

// The menus draw their own font (UiGlyphTableBuild from menu_font_setting, UiGlyphFind),
// one byte or one Shift-JIS pair a glyph, with no tags. The European menu_font_setting
// lists the accents as tags; setup.py writes each as one byte, 0xA0 + its index in
// kAccentTags (Shift-JIS's one-byte range, which opens no pair: 64 codes, one a tag).
// The menu strings get the same bytes here, in a copy, before they are laid out.
constexpr uint32_t kUiLayoutStringGlyphs = 0x0028A800u; // (out, str, center, x, spacing)
constexpr uint32_t kAccentFirstByte = 0xA0u;
constexpr uint32_t kLayoutSlots = 16;
constexpr uint32_t kLayoutSlotSize = 1024;
uint32_t g_layoutBuffers = 0;
std::atomic<uint32_t> g_layoutNext{0};

void uiLayoutStringGlyphs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    centrePartyLabel(ctx);
    const uint32_t textAddress = getRegU32(ctx, 5);
    if (g_active && textAddress != 0)
    {
        const char *text = reinterpret_cast<const char *>(rdram + (textAddress & 0x1FFFFFFFu));
        const void *end = std::memchr(text, 0, kLayoutSlotSize);
        if (end && std::memchr(text, '<', static_cast<const char *>(end) - text))
        {
            if (!g_layoutBuffers)
            {
                g_layoutBuffers = g_runtime->guestMalloc(kLayoutSlots * kLayoutSlotSize, 16u);
            }
            if (g_layoutBuffers)
            {
                const uint32_t slot = g_layoutBuffers + (g_layoutNext++ % kLayoutSlots) * kLayoutSlotSize;
                char *out = reinterpret_cast<char *>(rdram + (slot & 0x1FFFFFFFu));
                size_t n = 0;
                for (const char *p = text; *p;)
                {
                    int tag = -1;
                    if (*p == '<')
                    {
                        for (int i = 0; i < kAccentCount; ++i)
                        {
                            const size_t length = std::strlen(kAccentTags[i]);
                            if (std::strncmp(p, kAccentTags[i], length) == 0)
                            {
                                tag = i;
                                p += length;
                                break;
                            }
                        }
                    }
                    out[n++] = tag >= 0 ? static_cast<char>(kAccentFirstByte + static_cast<uint32_t>(tag)) : *p++;
                }
                out[n] = '\0';
                ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(slot));
            }
        }
    }
    g_originalUiLayout(rdram, ctx, runtime);
}

// A text style's font: 0 the names, 1 the messages, 2 the system font (its byte at +1,
// TextStyleGetFontType).
int fontType(const uint8_t *rdram, uint32_t style)
{
    return static_cast<int8_t>(rdram[(style + 1u) & 0x1FFFFFFFu]);
}

bool accentCode(uint32_t code)
{
    return code >= kAccentFirstCode && code < kAccentFirstCode + kAccentCount;
}

// What FontGetCharWidth would read for an accent in a table long enough to hold it.
void returnAccentWidth(uint8_t *rdram, R5900Context *ctx, uint32_t code)
{
    const int type = fontType(rdram, getRegU32(ctx, 4));
    int width = type >= 0 && type < 3 ? kAccentWidths[code - kAccentFirstCode][type] : 0;
    if (type == 0)
    {
        width -= 1;
    }
    setReturnU32(ctx, static_cast<uint32_t>(width) & 0xFFu);
    ctx->pc = getRegU32(ctx, 31);
}

// FontGetCharWidth (font, code).
void fontGetCharWidth(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t code = getRegU32(ctx, 5) & 0xFFFFu;
    if (g_active && accentCode(code))
    {
        returnAccentWidth(rdram, ctx, code);
        return;
    }
    g_originalCharWidth(rdram, ctx, runtime);
}

// FontGetGlyphWidth (font, code): the accent codes sit in the part of the code space the US
// executable takes for full-width characters; give them their own width instead.
void fontGetGlyphWidth(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t code = getRegU32(ctx, 5) & 0xFFFFu;
    if (g_active && accentCode(code))
    {
        returnAccentWidth(rdram, ctx, code);
        return;
    }
    g_originalGlyphWidth(rdram, ctx, runtime);
}

// The names' font (floating names over people and enemies): its European page puts the
// letters on rows 22 pixels apart, where the US one has 18 (room above the capitals for
// their accents). The PAL executable's FontGetLineHeight and FontGetGlyphRect say 22 for it.
constexpr int kNameCellWidth = 16;
constexpr int kNameCellHeight = 22;
constexpr int kNameColumns = 32;
constexpr uint32_t kNameBankCodes = 476u; // below it, FontGetGlyphBankType1 says bank 0

void fontGetLineHeight(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_active && fontType(rdram, getRegU32(ctx, 4)) == 0)
    {
        setReturnU32(ctx, static_cast<uint32_t>(kNameCellHeight));
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    g_originalLineHeight(rdram, ctx, runtime);
}

// FontGetGlyphRect (out, font, code) writes the glyph's x, y, w, h on its page to out.
void fontGetGlyphRect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t code = getRegU32(ctx, 6) & 0xFFFFu;
    if (g_active && fontType(rdram, getRegU32(ctx, 5)) == 0 && (code < kNameBankCodes || code == 0xFF02u))
    {
        const uint32_t out = getRegU32(ctx, 4);
        const int32_t rect[4] = {static_cast<int32_t>(code % kNameColumns) * kNameCellWidth,
                                 static_cast<int32_t>(code / kNameColumns) * kNameCellHeight, kNameCellWidth,
                                 kNameCellHeight};
        for (int i = 0; i < 4; ++i)
        {
            writeWord(rdram, out + 4u * static_cast<uint32_t>(i), static_cast<uint32_t>(rect[i]));
        }
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    g_originalGlyphRect(rdram, ctx, runtime);
}

// The hint on a scene's pause screen (PauseReasonDrawMaybe, 0x2F5C40) is drawn from a table
// of eight lists of pieces {name, x, y, u, v, w, h} at 0x451CD0, cut from the textures
// b_m_base and btl_st01 of battle_menu_fix_b_<LANG>.img. The European pictures are cut
// otherwise (a button icon where the US one has the words "START button"), so the PAL
// executable's lists (its table at 0x45F8A0), copied here, take the US ones' place.
constexpr uint32_t kPauseReasonDraw = 0x002F5C40u;
constexpr uint32_t kPauseLists = 0x00451CD0u;
constexpr uint32_t kPauseNameBase = 0x00451940u;   // the US list 0's first piece: b_m_base
constexpr uint32_t kPauseNameStatus = 0x0045195Cu; // its second: btl_st01
struct PausePiece
{
    uint8_t texture; // 0 b_m_base, 1 btl_st01
    int16_t x, y, u, v, w, h;
};
constexpr PausePiece kPauseList0[] = {{0, 222, 196, 188, 236, 68, 20}, {1, 298, 235, 196, 0, 32, 26},
                                      {0, 330, 240, 122, 76, 134, 20}, {1, 303, 268, 108, 44, 22, 22},
                                      {0, 330, 270, 122, 96, 134, 20}};
constexpr PausePiece kPauseList1[] = {{0, 222, 196, 188, 236, 68, 20}, {1, 298, 235, 196, 0, 32, 26},
                                      {0, 330, 240, 122, 76, 134, 20}, {1, 303, 268, 130, 44, 22, 22},
                                      {1, 330, 270, 134, 136, 86, 20}, {1, 303, 298, 108, 44, 22, 22},
                                      {0, 330, 300, 122, 96, 134, 20}};
constexpr PausePiece kPauseList2[] = {{0, 222, 164, 188, 236, 68, 20}, {1, 298, 203, 196, 0, 32, 26},
                                      {0, 330, 208, 122, 76, 134, 20}};
constexpr PausePiece kPauseList3[] = {{0, 222, 164, 188, 236, 68, 20}, {1, 298, 203, 196, 0, 32, 26},
                                      {0, 330, 208, 122, 76, 134, 20}, {1, 303, 236, 130, 44, 22, 22},
                                      {1, 330, 238, 134, 136, 86, 20}};
constexpr PausePiece kPauseList7[] = {{1, 30, 398, 130, 44, 22, 22}, {1, 54, 400, 134, 136, 86, 20}};
struct PauseList
{
    int index;
    const PausePiece *pieces;
    size_t count;
};
constexpr PauseList kPauseListsPal[] = {{0, kPauseList0, std::size(kPauseList0)},
                                        {1, kPauseList1, std::size(kPauseList1)},
                                        {2, kPauseList2, std::size(kPauseList2)},
                                        {3, kPauseList3, std::size(kPauseList3)},
                                        {7, kPauseList7, std::size(kPauseList7)}};
PS2Runtime::RecompiledFunction g_originalPauseDraw = nullptr;
bool g_pausePatched = false;

void patchPauseLists(uint8_t *rdram)
{
    g_pausePatched = true;
    const uint32_t names[2] = {readWord(rdram, kPauseNameBase), readWord(rdram, kPauseNameStatus)};
    const char *expected[2] = {"b_m_base", "btl_st01"};
    for (int i = 0; i < 2; ++i)
    {
        if (!names[i] || std::strcmp(reinterpret_cast<const char *>(rdram + (names[i] & 0x1FFFFFFFu)), expected[i]) != 0)
        {
            std::fprintf(stderr, "[rg] the pause screen keeps the US pieces (no %s)\n", expected[i]);
            return;
        }
    }
    size_t pieces = 0;
    for (const PauseList &list : kPauseListsPal)
    {
        pieces += list.count + 1; // and the end, a piece with no name
    }
    const uint32_t memory = g_runtime->guestMalloc(static_cast<uint32_t>(pieces * 28u), 16u);
    if (!memory)
    {
        return;
    }
    uint32_t at = memory;
    for (const PauseList &list : kPauseListsPal)
    {
        writeWord(rdram, kPauseLists + 4u * static_cast<uint32_t>(list.index), at);
        for (size_t i = 0; i <= list.count; ++i, at += 28u)
        {
            const bool end = i == list.count;
            const PausePiece piece = end ? PausePiece{} : list.pieces[i];
            const int32_t fields[7] = {end ? 0 : static_cast<int32_t>(names[piece.texture]), piece.x, piece.y,
                                       piece.u, piece.v, piece.w, piece.h};
            for (int k = 0; k < 7; ++k)
            {
                writeWord(rdram, at + 4u * static_cast<uint32_t>(k), static_cast<uint32_t>(fields[k]));
            }
        }
    }
}

void pauseReasonDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_active && !g_pausePatched)
    {
        patchPauseLists(rdram);
    }
    g_originalPauseDraw(rdram, ctx, runtime);
}

// The main menu's System submenu (Settings and Help, Opciones and Ayuda): its
// highlight (widget 0x1AE) and frame (0x1AF's sprites 2 to 10) come from the
// layout, wider in a European text (european.py keeps the PAL groups' room:
// 78 for Spanish against 70), but MenuMainSubSelectLayoutCb (0x294550, the
// list's callback, after which nothing else sizes them) sets the unselected
// row's background (0x1AF's sprite 0 or 1) 70 wide, the US figure, which left
// a strip of the backdrop at its right. The PAL executable sets it to the
// highlight's width, as this does after the callback (the same 70 in
// English). The UI's widgets are a list from 0x4DB1B0; a widget's sprites
// (+0x20) are 0x20 bytes each, the width an s16 at +4; the callback's a0 has
// the selected row, an s16 at +0x12.
constexpr uint32_t kMainSubSelectLayout = 0x00294550u;
constexpr uint32_t kMainMenuUi = 0x004DB1B0u;
constexpr uint16_t kSubHighlight = 0x1AEu;
constexpr uint16_t kSubRows = 0x1AFu;
constexpr uint32_t kSelectedRow = 0x12u;
constexpr uint32_t kWidgetSprites = 0x20u;
constexpr uint32_t kSpriteBytes = 0x20u;
constexpr uint32_t kSpriteWidth = 0x4u;
PS2Runtime::RecompiledFunction g_originalSubLayout = nullptr;

// The widget `id` of the UI at `ui`, or 0.
uint32_t findWidget(const uint8_t *rdram, uint32_t ui, uint16_t id)
{
    uint32_t node = readWord(rdram, ui);
    for (int guard = 0; node != 0u && guard < 4096; ++guard)
    {
        const uint32_t widget = readWord(rdram, node);
        if (widget == 0u)
        {
            return 0u;
        }
        uint16_t widgetId = 0u;
        std::memcpy(&widgetId, rdram + (widget & 0x1FFFFFFFu), sizeof(widgetId));
        if (widgetId == id)
        {
            return widget;
        }
        node = readWord(rdram, node + 8u);
    }
    return 0u;
}

int europeanColumn();

void afterSubLayout(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t list)
{
    if (europeanColumn() < 0)
    {
        return;
    }
    const uint32_t highlight = findWidget(rdram, kMainMenuUi, kSubHighlight);
    const uint32_t rows = findWidget(rdram, kMainMenuUi, kSubRows);
    const uint32_t highlightSprites = highlight ? readWord(rdram, highlight + kWidgetSprites) : 0u;
    const uint32_t rowSprites = rows ? readWord(rdram, rows + kWidgetSprites) : 0u;
    if (list == 0u || highlightSprites == 0u || rowSprites == 0u)
    {
        return;
    }
    int16_t width = 0, selected = 0;
    std::memcpy(&width, rdram + ((highlightSprites + kSpriteWidth) & 0x1FFFFFFFu), sizeof(width));
    std::memcpy(&selected, rdram + ((list + kSelectedRow) & 0x1FFFFFFFu), sizeof(selected));
    if (width <= 0)
    {
        return;
    }
    for (int16_t row = 0; row < 2; ++row)
    {
        const uint32_t at = (rowSprites + static_cast<uint32_t>(row) * kSpriteBytes + kSpriteWidth) & 0x1FFFFFFFu;
        int16_t rowWidth = 0;
        std::memcpy(&rowWidth, rdram + at, sizeof(rowWidth));
        if (row != selected && rowWidth != 0)
        {
            std::memcpy(rdram + at, &width, sizeof(width));
        }
    }
}

// These fix-ups run once the hooked original has returned to its caller
// (rg::hooks::callThen): a scheduler checkpoint may stop it at any of its
// calls, and the run loop then finishes it past the hook. In English the
// hooks only call the original.
void mainSubSelectLayout(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (europeanColumn() < 0)
    {
        g_originalSubLayout(rdram, ctx, runtime);
        return;
    }
    rg::hooks::callThen(g_originalSubLayout, rdram, ctx, runtime, &afterSubLayout, getRegU32(ctx, 4));
}

// Lists the US code sizes once, for English, and the PAL executable per
// language from tables of its own (read from SCES-54552; its columns are
// English, Spanish, French, Italian, German). In a European text they take
// the PAL widths, as that executable sets them; in English they stay.
// Indexed by our languages' order (French, German, Spanish, Italian).
int16_t readHalf(const uint8_t *rdram, uint32_t address)
{
    int16_t value = 0;
    std::memcpy(&value, rdram + (address & 0x1FFFFFFFu), sizeof(value));
    return value;
}

void writeHalf(uint8_t *rdram, uint32_t address, int16_t value)
{
    std::memcpy(rdram + (address & 0x1FFFFFFFu), &value, sizeof(value));
}

// French 0 to Italian 3, or -1 in English (and while the language's files
// are not known usable, when the game reads the English ones).
int europeanColumn()
{
    const int language = g_language.load(std::memory_order_relaxed);
    return g_active && g_text.loaded && g_text.ok && language >= 1 && language <= 4 ? language - 1 : -1;
}

// The item commands' lists (Use, Equip, Discard...), six of them: their
// frame (MenuItemCmdFrameResize, 0x2A5750), rows and highlight
// (MenuItemCmdSetRowHighlight, 0x2A1800), labels (0x2A6060, 0x2A5BD0), the
// count panel under them (0x2A53F0) and their place (0x2A4620) all read a
// list's width from {s16 rows, s16 width} x6 at 0x450DD0, nothing else
// does, and the table is never loaded again; the PAL one (0x46E9D0) has a
// width per language. Written once, before any menu opens.
constexpr uint32_t kItemCmdTable = 0x00450DD0u;
constexpr int16_t kItemCmdUs[6] = {80, 74, 64, 78, 94, 86};
constexpr int16_t kItemCmdWidths[6][4] = {
    {82, 84, 82, 78}, {72, 88, 78, 82}, {58, 74, 78, 68}, {74, 72, 78, 70}, {100, 98, 98, 82}, {82, 90, 94, 76},
};
bool g_itemCommandsPatched = false;

void patchItemCommands(uint8_t *rdram)
{
    const int column = europeanColumn();
    if (g_itemCommandsPatched || column < 0)
    {
        return;
    }
    g_itemCommandsPatched = true;
    for (uint32_t list = 0; list < 6u; ++list)
    {
        if (readHalf(rdram, kItemCmdTable + 4u * list + 2u) != kItemCmdUs[list])
        {
            std::fprintf(stderr, "[rg] the item commands keep the US widths (not the table expected)\n");
            return;
        }
    }
    for (uint32_t list = 0; list < 6u; ++list)
    {
        writeHalf(rdram, kItemCmdTable + 4u * list + 2u, kItemCmdWidths[list][column]);
    }
}

// A party member's commands: a list of four rows (widget 0x11A) or of three
// (0x119), 124 and 84 wide in the US code, PAL's 0x48D530 and 0x48D540 per
// language. MenuPartyMemberCmdOpenState (0x2B99F0) writes the list's frame
// into its sprites (offsets from the first: the rows' width W+14, the
// middle's width W, W+12 for the right-hand pieces (the x of some, the width
// of others), the right edge's x W+17) and centres the
// labels at W/2+11 (its UiLayoutStringGlyphs calls); MenuPartyCmdListHighlight
// (0x29C440) sets the unselected rows and the highlight (widget 0x118's first
// sprite) W+14 wide. The open state's UI is the one at 0x482360, the
// highlight's its menu's (a0) at +0x4C4, as each reads it. After each, the
// language's W where the US one is.
constexpr uint32_t kMenuUi = 0x00482360u;
constexpr uint32_t kPartyCmdOpen = 0x002B99F0u;
constexpr uint32_t kPartyCmdHighlight = 0x0029C440u;
constexpr uint32_t kPartyCmdMenuUi = 0x4C4u;
constexpr uint16_t kPartyCmdHighlightWidget = 0x118u;
struct PartyList
{
    uint16_t id;
    int16_t us;
    int16_t widths[4];
    uint32_t rows[4];
    uint32_t middles[2];
    uint32_t corners[5];
    uint32_t edge;
};
constexpr PartyList kPartyLists[] = {
    {0x11Au, 124, {102, 140, 88, 100}, {0x4u, 0x24u, 0x44u, 0x64u}, {0xA4u, 0x144u},
     {0xC0u, 0x160u, 0x184u, 0x1A4u, 0x1C4u}, 0x100u},
    {0x119u, 84, {104, 140, 88, 94}, {0x4u, 0x24u, 0x44u, 0u}, {0x84u, 0x124u}, {0xA0u, 0x140u, 0x164u, 0x184u, 0u},
     0xE0u},
};
// The open state's label calls (UiLayoutStringGlyphs), by where they return.
constexpr uint32_t kPartyLabelReturns[] = {0x002B9C88u, 0x002B9CC4u, 0x002B9D00u, 0x002B9D3Cu};
PS2Runtime::RecompiledFunction g_originalPartyOpen = nullptr;
PS2Runtime::RecompiledFunction g_originalPartyHighlight = nullptr;

uint32_t widgetSprites(const uint8_t *rdram, uint32_t ui, uint16_t id)
{
    const uint32_t widget = ui ? findWidget(rdram, ui, id) : 0u;
    return widget ? readWord(rdram, widget + kWidgetSprites) : 0u;
}

// A widget's sub-items (numbers and the like), 0x20 bytes each, x an s16 at +0.
constexpr uint32_t kWidgetSubItems = 0x24u;

uint32_t widgetSubItems(const uint8_t *rdram, uint32_t ui, uint16_t id)
{
    const uint32_t widget = ui ? findWidget(rdram, ui, id) : 0u;
    return widget ? readWord(rdram, widget + kWidgetSubItems) : 0u;
}

void afterPartyOpen(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t)
{
    const int column = europeanColumn();
    if (column < 0)
    {
        return;
    }
    const uint32_t ui = readWord(rdram, kMenuUi);
    for (const PartyList &list : kPartyLists)
    {
        const uint32_t sprites = widgetSprites(rdram, ui, list.id);
        // The frame as the US code has just written it.
        if (sprites == 0u || readHalf(rdram, sprites + list.edge) != list.us + 17)
        {
            continue;
        }
        const int16_t width = list.widths[column];
        for (const uint32_t row : list.rows)
        {
            if (row != 0u && readHalf(rdram, sprites + row) == list.us + 14)
            {
                writeHalf(rdram, sprites + row, static_cast<int16_t>(width + 14));
            }
        }
        for (const uint32_t middle : list.middles)
        {
            writeHalf(rdram, sprites + middle, width);
        }
        for (const uint32_t corner : list.corners)
        {
            if (corner != 0u)
            {
                writeHalf(rdram, sprites + corner, static_cast<int16_t>(width + 12));
            }
        }
        writeHalf(rdram, sprites + list.edge, static_cast<int16_t>(width + 17));
    }
}

void partyCmdOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (europeanColumn() < 0)
    {
        g_originalPartyOpen(rdram, ctx, runtime);
        return;
    }
    rg::hooks::callThen(g_originalPartyOpen, rdram, ctx, runtime, &afterPartyOpen, 0u);
}

// `data`: the highlight's menu, with its low bit set for the four-row list
// (the game's ids are 0x11A and 0x119, from its table at 0x4614C0).
void afterPartyHighlight(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t data)
{
    const int column = europeanColumn();
    if (column < 0)
    {
        return;
    }
    const PartyList &list = (data & 1u) ? kPartyLists[0] : kPartyLists[1];
    const uint16_t id = list.id;
    const uint32_t ui = readWord(rdram, (data & ~1u) + kPartyCmdMenuUi);
    const int16_t width = static_cast<int16_t>(list.widths[column] + 14);
    if (const uint32_t sprites = widgetSprites(rdram, ui, id))
    {
        for (uint32_t row = 0; row < 4u; ++row)
        {
            const uint32_t at = sprites + row * kSpriteBytes + kSpriteWidth;
            if (readHalf(rdram, at) == list.us + 14)
            {
                writeHalf(rdram, at, width);
            }
        }
    }
    if (const uint32_t sprites = widgetSprites(rdram, ui, kPartyCmdHighlightWidget);
        sprites != 0u && readHalf(rdram, sprites + kSpriteWidth) == list.us + 14)
    {
        writeHalf(rdram, sprites + kSpriteWidth, width);
    }
}

void partyCmdHighlight(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t menu = getRegU32(ctx, 4);
    const uint16_t id = static_cast<uint16_t>(getRegU32(ctx, 5));
    if (europeanColumn() < 0 || menu == 0u || (menu & 1u) != 0u ||
        (id != kPartyLists[0].id && id != kPartyLists[1].id))
    {
        g_originalPartyHighlight(rdram, ctx, runtime);
        return;
    }
    rg::hooks::callThen(g_originalPartyHighlight, rdram, ctx, runtime, &afterPartyHighlight,
                        menu | (id == kPartyLists[0].id ? 1u : 0u));
}

// An item's description (MenuItemInfoPopupLayout, 0x289F30) shows a weapon's
// kind as a 22x22 cell of gaiji2, picked from {s16 kind, u, v} x13 at
// 0x461060, read only there: the main weapon's "M", the sub weapon's "S".
// The PAL executable has a table per language (0x46E580): "P" in French,
// Spanish and Italian, "H" and "Z" in German. The European font pack
// carries PAL's gaiji2, which has those cells (the US one is blank there):
// only with that pack in the text. Written once, like the item commands;
// the caller holds g_mutex, the text's index loaded.
constexpr uint32_t kItemKindIcons = 0x00461060u;
struct KindIcon
{
    int16_t kind, u, v;
};
constexpr KindIcon kKindIconsUs[2] = {{0, 66, 126}, {1, 88, 126}};
constexpr KindIcon kKindIcons[2][4] = {
    {{0, 110, 82}, {0, 110, 104}, {0, 110, 82}, {0, 110, 82}},
    {{1, 88, 126}, {1, 110, 126}, {1, 88, 126}, {1, 88, 126}},
};
bool g_kindIconsPatched = false;

void patchItemKindIcons(uint8_t *rdram)
{
    const int column = europeanColumn();
    if (g_kindIconsPatched || column < 0)
    {
        return;
    }
    g_kindIconsPatched = true;
    std::string font = "meswin\\fontset_main_" + upperCode() + ".pak";
    std::transform(font.begin(), font.end(), font.begin(), [](unsigned char c) { return std::tolower(c); });
    if (g_text.files.count(font) == 0u)
    {
        std::fprintf(stderr, "[rg] the weapons' kind icons stay M and S (no European font in the text)\n");
        return;
    }
    for (uint32_t entry = 0; entry < 2u; ++entry)
    {
        const uint32_t at = kItemKindIcons + 6u * entry;
        const KindIcon &us = kKindIconsUs[entry];
        if (readHalf(rdram, at) != us.kind || readHalf(rdram, at + 2u) != us.u || readHalf(rdram, at + 4u) != us.v)
        {
            std::fprintf(stderr, "[rg] the weapons' kind icons stay M and S (not the table expected)\n");
            return;
        }
    }
    for (uint32_t entry = 0; entry < 2u; ++entry)
    {
        const uint32_t at = kItemKindIcons + 6u * entry;
        writeHalf(rdram, at + 2u, kKindIcons[entry][column].u);
        writeHalf(rdram, at + 4u, kKindIcons[entry][column].v);
    }
}

// The item menu's Rare Items tab: once the story is far enough (the flag
// MenuItemTabsAvailUpdate, 0x2B0070, reads), the "Ancient Key Pieces"
// heading shows, a picture 256 wide (widget 0x6D's sprite 0x11, in the UI at
// 0x482360) with the word at its left. The layout puts it at x 210, which
// centres the English word (132 wide) on 276; the PAL executable sets x to
// 276 - half the language's word (0x46EAA0: 160, 160, 116, 138 for Spanish,
// French, Italian, German). As it, after the original.
constexpr uint32_t kItemTabsUpdate = 0x002B0070u;
constexpr uint16_t kItemTabsWidget = 0x6Du;
constexpr uint32_t kKeyPiecesSprite = 0x11u;
constexpr int16_t kKeyPiecesUsX = 210;
constexpr int16_t kKeyPiecesWidth = 0x100;
constexpr int16_t kKeyPiecesX[4] = {196, 207, 196, 218};
PS2Runtime::RecompiledFunction g_originalItemTabs = nullptr;

void afterItemTabs(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t)
{
    const int column = europeanColumn();
    const uint32_t sprites = column < 0 ? 0u : widgetSprites(rdram, readWord(rdram, kMenuUi), kItemTabsWidget);
    if (sprites == 0u)
    {
        return;
    }
    const uint32_t sprite = sprites + kKeyPiecesSprite * kSpriteBytes;
    if (readHalf(rdram, sprite + kSpriteWidth) == kKeyPiecesWidth && readHalf(rdram, sprite) == kKeyPiecesUsX)
    {
        writeHalf(rdram, sprite, kKeyPiecesX[column]);
    }
}

void itemTabsUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (europeanColumn() < 0)
    {
        g_originalItemTabs(rdram, ctx, runtime);
        return;
    }
    rg::hooks::callThen(g_originalItemTabs, rdram, ctx, runtime, &afterItemTabs, 0u);
}

// The "Open Revelation Flow spaces" list (Items > an item > Revelation, and
// the transporter's): a title window and a list window, both as wide as the
// title plus 40 (UiWindowFitToText, 0x28D0C0, centred in a box), then a row
// per member with its name, "Available" or "None", an icon and a count
// (MenuPartyItemSkillListLayoutMaybe, 0x2A2EE0). The US code leaves the
// title bar (238), the highlight bar (200 between its caps) and the icons
// (row + 160) where a window 246 wide has them. The PAL executable gives
// the windows a minimum width per language (0x46EAD8), sizes the title bar
// W - 8 and the highlight W - 46, and puts the icons at its own offset
// (0x46EA28), the counts 24 after. As it, after each original: a window
// narrower than the minimum is laid out again at that width about the same
// centre, then the list's pieces follow its width.
constexpr uint32_t kWindowFit = 0x0028D0C0u;
constexpr uint32_t kRevelationFitReturns[] = {0x002A6C40u, 0x002A6C54u, 0x0037C6D4u, 0x0037C6E8u};
constexpr uint32_t kFitSprite = 0x0u; // the window's first sprite, as the fit is called
constexpr int kRevelationMinWidth[4] = {265, 260, 270, 265};
constexpr uint32_t kSkillLayout = 0x002A2EE0u;
constexpr uint32_t kSkillLayoutMain = 0x002A6DA8u;
constexpr uint32_t kSkillLayoutTransporter = 0x0037C83Cu;
struct RevelationIds
{
    uint16_t list, highlight, icons, counts, titleBar;
};
constexpr RevelationIds kRevelationMain = {0x9Cu, 0x9Fu, 0x9Du, 0x9Eu, 0x9Au};
constexpr RevelationIds kRevelationTransporter = {0x761Au, 0x761Du, 0x761Bu, 0x761Cu, 0x7618u};
constexpr int kRevelationIconOffset[4] = {178, 170, 190, 175};
constexpr int kRevelationUsIcon = 0xC0;  // from the window's left: row 32 + 160
constexpr int kRevelationUsCount = 0xD8; // row 32 + 184
constexpr int kRevelationIconWidth = 0x1C;
constexpr int16_t kRevelationUsHighlight = 200;
constexpr int16_t kRevelationUsTitleBar = 238;
PS2Runtime::RecompiledFunction g_originalWindowFit = nullptr;
PS2Runtime::RecompiledFunction g_originalSkillLayout = nullptr;

// `first`: the window's first sprite (the fit's thirteen follow it). Both
// windows are centred in a box (the fit's mode 4), so the new width keeps
// the centre. The text the fit also places is the list's, which its caller
// empties right after.
void afterRevelationFit(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t first)
{
    const int column = europeanColumn();
    if (column < 0 || first == 0u)
    {
        return;
    }
    auto sprite = [first](uint32_t index) { return first + index * kSpriteBytes; };
    const int16_t middle = readHalf(rdram, sprite(8) + kSpriteWidth);
    const int width = middle + 32;
    const int minimum = kRevelationMinWidth[column];
    if (middle <= 0 || width >= minimum)
    {
        return;
    }
    // The fit's x is the box's centre less half the width.
    const int x = readHalf(rdram, sprite(5)) + width / 2 - minimum / 2;
    const int inner = minimum - 32;
    const int16_t left = static_cast<int16_t>(x);
    const int16_t right = static_cast<int16_t>(x + 16 + inner);
    for (const uint32_t index : {0u, 1u, 2u, 10u, 11u, 12u})
    {
        writeHalf(rdram, sprite(index), right);
    }
    for (const uint32_t index : {3u, 5u, 6u, 7u})
    {
        writeHalf(rdram, sprite(index), left);
    }
    writeHalf(rdram, sprite(4), static_cast<int16_t>(x + 6));
    writeHalf(rdram, sprite(4) + kSpriteWidth, static_cast<int16_t>(inner + 0x15));
    for (const uint32_t index : {8u, 9u})
    {
        writeHalf(rdram, sprite(index), static_cast<int16_t>(x + 16));
        writeHalf(rdram, sprite(index) + kSpriteWidth, static_cast<int16_t>(inner));
    }
}

void windowFit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // Every window of the game fits through here: the four calls first.
    const uint32_t ra = getRegU32(ctx, 31);
    const bool revelation = europeanColumn() >= 0 &&
                            std::find(std::begin(kRevelationFitReturns), std::end(kRevelationFitReturns), ra) !=
                                std::end(kRevelationFitReturns);
    const uint32_t desc = revelation ? getRegU32(ctx, 4) : 0u;
    const uint32_t first = desc != 0u ? readWord(rdram, desc + kFitSprite) : 0u;
    if (first == 0u)
    {
        g_originalWindowFit(rdram, ctx, runtime);
        return;
    }
    rg::hooks::callThen(g_originalWindowFit, rdram, ctx, runtime, &afterRevelationFit, first);
}

// `data`: the layout's a0 (where the UI's address is), its low bit set in
// the transporter.
void afterRevelationList(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t data)
{
    const int column = europeanColumn();
    if (column < 0)
    {
        return;
    }
    const RevelationIds &ids = (data & 1u) ? kRevelationTransporter : kRevelationMain;
    const uint32_t ui = readWord(rdram, data & ~1u);
    const uint32_t list = widgetSprites(rdram, ui, ids.list);
    if (list == 0u)
    {
        return;
    }
    const int16_t middle = readHalf(rdram, list + 8u * kSpriteBytes + kSpriteWidth);
    const int width = middle + 32;
    const int left = readHalf(rdram, list) - middle - 16;
    const int icon = left + 32 + kRevelationIconOffset[column];
    if (const uint32_t icons = widgetSprites(rdram, ui, ids.icons))
    {
        for (uint32_t row = 0; row < 8u; ++row)
        {
            const uint32_t at = icons + row * kSpriteBytes;
            if (readHalf(rdram, at + kSpriteWidth) == kRevelationIconWidth &&
                readHalf(rdram, at) == left + kRevelationUsIcon)
            {
                writeHalf(rdram, at, static_cast<int16_t>(icon));
            }
        }
    }
    if (const uint32_t counts = widgetSubItems(rdram, ui, ids.counts))
    {
        for (uint32_t row = 0; row < 8u; ++row)
        {
            const uint32_t at = counts + row * kSpriteBytes;
            if (readHalf(rdram, at) == left + kRevelationUsCount)
            {
                writeHalf(rdram, at, static_cast<int16_t>(icon + 24));
            }
        }
    }
    if (const uint32_t bar = widgetSprites(rdram, ui, ids.highlight);
        bar != 0u && readHalf(rdram, bar + kSpriteBytes + kSpriteWidth) == kRevelationUsHighlight)
    {
        const int16_t barMiddle = static_cast<int16_t>(width - 46);
        writeHalf(rdram, bar + kSpriteBytes + kSpriteWidth, barMiddle);
        writeHalf(rdram, bar + 2u * kSpriteBytes,
                  static_cast<int16_t>(readHalf(rdram, bar + kSpriteBytes) + barMiddle));
    }
    if (const uint32_t title = widgetSprites(rdram, ui, ids.titleBar);
        title != 0u && readHalf(rdram, title + kSpriteWidth) == kRevelationUsTitleBar)
    {
        writeHalf(rdram, title + kSpriteWidth, static_cast<int16_t>(width - 8));
    }
}

void skillLayout(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t where = getRegU32(ctx, 4);
    const uint16_t list = static_cast<uint16_t>(getRegU32(ctx, 5));
    const bool main = ra == kSkillLayoutMain && list == kRevelationMain.list;
    const bool transporter = ra == kSkillLayoutTransporter && list == kRevelationTransporter.list;
    if (europeanColumn() < 0 || where == 0u || (where & 1u) != 0u || (!main && !transporter))
    {
        g_originalSkillLayout(rdram, ctx, runtime);
        return;
    }
    rg::hooks::callThen(g_originalSkillLayout, rdram, ctx, runtime, &afterRevelationList,
                        where | (transporter ? 1u : 0u));
}

// The main menu's weapons page heads "JASTER's Weapons" with two pictures
// (widget 0x24B: sprite 0 the name, sprite 1 the label), and tags the two
// slots "M-W" and "S-W" (widget 0x248, a letter a sprite: 0 to 2 and 3 to
// 5). MenuModelPageCharaLabelLayoutMaybe (0x359120, a0 the page, its UI at
// +0x134) puts the label after the name, at a place per character, and never
// touches the tags. The European pictures say "Armas de", "Waffen von"...,
// which the PAL executable puts first, the name after it, each sprite as wide
// as its word, and it reverses the tags in Spanish, French and Italian
// ("A-P", "A-S"). As it, after the original.
constexpr uint32_t kWeaponsHeader = 0x00359120u;
constexpr uint32_t kWeaponsPageUi = 0x134u;
constexpr uint32_t kWeaponsPageChara = 0xDAu;    // s16 ids, indexed by the byte at +0x175
constexpr uint32_t kWeaponsPageCharaAt = 0x175u;
constexpr int kWeaponsFirstChara = 0x2AF9;
constexpr uint16_t kWeaponsLabelWidget = 0x24Bu;
constexpr uint16_t kWeaponsTagWidget = 0x248u;
constexpr uint32_t kSpriteUvWidth = 0xCu;
constexpr int16_t kWeaponsLabelUsX[8] = {0x78, 0x74, 0x72, 0x75, 0x70, 0x70, 0x6D, 0x80};
constexpr int16_t kWeaponsNameWidth[8] = {64, 60, 58, 60, 58, 56, 54, 74};
constexpr int16_t kWeaponsLabelWidth[4] = {72, 84, 71, 58};
constexpr int16_t kWeaponsLabelX = 57;
constexpr int16_t kWeaponsTagLayoutX[6] = {246, 260, 268, 300, 312, 320};
constexpr int16_t kWeaponsTagLayoutWidth[6] = {14, 8, 14, 14, 8, 14};
constexpr int16_t kWeaponsTagX[2][6] = {{268, 260, 246, 322, 314, 300}, {246, 260, 268, 300, 312, 320}};
constexpr int16_t kWeaponsTagWidth[2][6] = {{13, 8, 14, 12, 8, 14}, {14, 8, 14, 12, 8, 14}};
PS2Runtime::RecompiledFunction g_originalWeaponsHeader = nullptr;

void setSpriteWidth(uint8_t *rdram, uint32_t sprite, int16_t width)
{
    writeHalf(rdram, sprite + kSpriteWidth, width);
    writeHalf(rdram, sprite + kSpriteUvWidth, width);
}

void afterWeaponsHeader(uint8_t *rdram, R5900Context *, PS2Runtime *, uint32_t page)
{
    const int column = europeanColumn();
    if (column < 0)
    {
        return;
    }
    const uint8_t slot = rdram[(page + kWeaponsPageCharaAt) & 0x1FFFFFFFu];
    const int chara = readHalf(rdram, page + kWeaponsPageChara + 2u * slot) - kWeaponsFirstChara;
    const uint32_t ui = readWord(rdram, page + kWeaponsPageUi);
    const uint32_t label = widgetSprites(rdram, ui, kWeaponsLabelWidget);
    if (chara < 0 || chara >= 8 || label == 0u)
    {
        return;
    }
    // The label where the US code has just put it (a place per character).
    const uint32_t name = label, word = label + kSpriteBytes;
    if (std::find(std::begin(kWeaponsLabelUsX), std::end(kWeaponsLabelUsX), readHalf(rdram, word)) !=
        std::end(kWeaponsLabelUsX))
    {
        const int16_t labelWidth = kWeaponsLabelWidth[column];
        writeHalf(rdram, word, kWeaponsLabelX);
        setSpriteWidth(rdram, word, labelWidth);
        writeHalf(rdram, name, static_cast<int16_t>(kWeaponsLabelX + labelWidth + 1));
        setSpriteWidth(rdram, name, kWeaponsNameWidth[chara]);
    }
    // The tags as the layout has them (nothing else moves them), else as
    // they were left here.
    const uint32_t tags = widgetSprites(rdram, ui, kWeaponsTagWidget);
    if (tags == 0u)
    {
        return;
    }
    for (uint32_t letter = 0; letter < 6u; ++letter)
    {
        const uint32_t sprite = tags + letter * kSpriteBytes;
        if (readHalf(rdram, sprite) != kWeaponsTagLayoutX[letter] ||
            readHalf(rdram, sprite + kSpriteWidth) != kWeaponsTagLayoutWidth[letter])
        {
            return;
        }
    }
    const int order = column == 1 ? 1 : 0; // German keeps "H-W" and "Z-W"
    for (uint32_t letter = 0; letter < 6u; ++letter)
    {
        const uint32_t sprite = tags + letter * kSpriteBytes;
        writeHalf(rdram, sprite, kWeaponsTagX[order][letter]);
        setSpriteWidth(rdram, sprite, kWeaponsTagWidth[order][letter]);
    }
}

// The Attack Chain (the button presses some abilities ask for) shows "PUSH!!",
// the hits with a "Hits" label at the bottom right and the damage at the
// bottom left, pictures of chn\common.pac. A European text carries the PAL
// pack's picture of its language (european.py), longer words ("Golpes",
// "Treffer"), and the PAL executable draws the label and the count further
// left by an offset per language (0x471970). As it, on the x (f12) of those
// draws in AttackChainDraw (0x380660), told by where they return. In a wide
// frame (IsWidescreen, 0x1C6AB0: the game's flag at 0x481638 or our wide
// mode) the game draws the chain 0.75 as wide and places it so; the offset
// shrinks with it.
constexpr uint32_t kSpriteAnchorBL = 0x00383860u;
constexpr uint32_t kGameWidescreen = 0x00481638u;
constexpr float kWideScale = 0.75f;
constexpr uint32_t kSpriteAnchorBR = 0x003836A0u;
constexpr uint32_t kChainLabelReturn = 0x00381A18u;
constexpr uint32_t kChainCountReturns[] = {0x00381C84u, 0x00381CB8u, 0x00381CF8u, 0x00381D94u};
constexpr float kChainOffset[4] = {-8.0f, -14.0f, -10.0f, -4.0f};
PS2Runtime::RecompiledFunction g_originalAnchorBL = nullptr;
PS2Runtime::RecompiledFunction g_originalAnchorBR = nullptr;

void moveChainPiece(const uint8_t *rdram, R5900Context *ctx, bool count)
{
    const int column = europeanColumn();
    if (column < 0 || !g_chainPictures.load(std::memory_order_relaxed))
    {
        return;
    }
    const uint32_t ra = getRegU32(ctx, 31);
    const bool chain = count ? std::find(std::begin(kChainCountReturns), std::end(kChainCountReturns), ra) !=
                                   std::end(kChainCountReturns)
                             : ra == kChainLabelReturn;
    if (chain)
    {
        const bool wide = rg::widescreen::wide() || readWord(rdram, kGameWidescreen) == 1u;
        ctx->f[12] += kChainOffset[column] * (wide ? kWideScale : 1.0f);
    }
}

void spriteAnchorBL(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    moveChainPiece(rdram, ctx, false);
    g_originalAnchorBL(rdram, ctx, runtime);
}

void spriteAnchorBR(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    moveChainPiece(rdram, ctx, true);
    g_originalAnchorBR(rdram, ctx, runtime);
}

void weaponsHeader(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t page = getRegU32(ctx, 4);
    if (europeanColumn() < 0 || page == 0u)
    {
        g_originalWeaponsHeader(rdram, ctx, runtime);
        return;
    }
    rg::hooks::callThen(g_originalWeaponsHeader, rdram, ctx, runtime, &afterWeaponsHeader, page);
}

// The save screens' Yes/No box (SaveMsgBoxShow, 0x376BF0, a1 the UI): the US
// code puts its two labels at fixed places in their buttons (the box's
// sprites 9 and 10, its widget id at 0x47FB04), 12 and 18 in, where "Yes"
// and "No" sit in the middle; the PAL code centres them on the buttons
// (UiSetTextCenteredX, 0x28F300, at a button's x + 26), so "Sí" or "Oui" do
// not sit to one side. As it, after the original, in a European text.
constexpr uint32_t kSaveMsgBoxShow = 0x00376BF0u;
constexpr uint32_t kUiSetTextCenteredX = 0x0028F300u;
constexpr uint32_t kSaveMsgBoxId = 0x0047FB04u;
constexpr uint32_t kButtonsFirst = 9u;
// A widget's texts (+0x28), 0x10 bytes each, x an s16 at +0.
constexpr uint32_t kWidgetTexts = 0x28u;
constexpr uint32_t kTextStride = 0x10u;
constexpr int16_t kButtonMiddle = 0x1A;
PS2Runtime::RecompiledFunction g_originalSaveMsgBox = nullptr;
PS2Runtime::RecompiledFunction g_originalCentredX = nullptr;

// UiSetTextCenteredX(ui, id, x, text), the game's own (past the hook), to
// its end.
void centreText(uint8_t *rdram, const R5900Context *ctx, PS2Runtime *runtime, uint32_t ui, uint32_t id, int32_t x,
                uint32_t text)
{
    if (!g_originalCentredX)
    {
        return;
    }
    R5900Context local = *ctx;
    local.r[4] = _mm_set_epi64x(0, static_cast<int64_t>(ui));
    local.r[5] = _mm_set_epi64x(0, static_cast<int64_t>(id));
    local.r[6] = _mm_set_epi64x(0, static_cast<int64_t>(x));
    local.r[7] = _mm_set_epi64x(0, static_cast<int64_t>(text));
    if (!rg::hooks::runToEnd(g_originalCentredX, kUiSetTextCenteredX, rdram, local, runtime))
    {
        std::fprintf(stderr, "[rg] centring a label stopped at 0x%08X\n", local.pc);
    }
}

void afterSaveMsgBox(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t ui)
{
    if (europeanColumn() < 0 || ui == 0u)
    {
        return;
    }
    const uint16_t id = static_cast<uint16_t>(readHalf(rdram, kSaveMsgBoxId));
    const uint32_t widget = findWidget(rdram, ui, id);
    const uint32_t sprites = widget ? readWord(rdram, widget + kWidgetSprites) : 0u;
    const uint32_t texts = widget ? readWord(rdram, widget + kWidgetTexts) : 0u;
    // A question: the US code has just put "Yes" 12 into its button (the
    // buttons are sized later, so their width says nothing yet).
    if (sprites == 0u || texts == 0u ||
        readHalf(rdram, texts + kTextStride) != readHalf(rdram, sprites + kButtonsFirst * kSpriteBytes) + 12)
    {
        return;
    }
    for (uint32_t label = 1u; label <= 2u; ++label)
    {
        const int16_t x = readHalf(rdram, sprites + (kButtonsFirst + label - 1u) * kSpriteBytes);
        centreText(rdram, ctx, runtime, ui, id, x + kButtonMiddle, label);
    }
}

void saveMsgBoxShow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (europeanColumn() < 0)
    {
        g_originalSaveMsgBox(rdram, ctx, runtime);
        return;
    }
    rg::hooks::callThen(g_originalSaveMsgBox, rdram, ctx, runtime, &afterSaveMsgBox, getRegU32(ctx, 5));
}

// The battle's warning with the gauge empty (widget 0x83 of the normal
// overlay's FieldHudWidget83Open, 0x4F95B0: "You can't fight until [atg]
// recharges!" over "Guard against attacks with [r1] to recharge it!"): the US
// code centres the second line (UiSetTextCenteredX at 0xEC) and leaves the
// first where the layout puts it, made for the English line, so a longer
// European one ran out of the window's right side. After that call, the
// first line is centred at the same place. Told by where the call returns
// (0x4F962C, the overlay's) and its arguments, as other overlays use those
// addresses.
constexpr uint32_t kWarningCentreReturn = 0x004F962Cu;
constexpr uint32_t kWarningWidget = 0x83u;
constexpr uint32_t kWarningMiddle = 0xECu;

void afterWarningCentre(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t ui)
{
    if (europeanColumn() >= 0)
    {
        centreText(rdram, ctx, runtime, ui, kWarningWidget, static_cast<int32_t>(kWarningMiddle), 0u);
    }
}

void uiSetTextCenteredX(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (europeanColumn() >= 0 && getRegU32(ctx, 31) == kWarningCentreReturn &&
        (getRegU32(ctx, 5) & 0xFFFFu) == kWarningWidget && getRegU32(ctx, 6) == kWarningMiddle &&
        getRegU32(ctx, 7) == 1u)
    {
        rg::hooks::callThen(g_originalCentredX, rdram, ctx, runtime, &afterWarningCentre, getRegU32(ctx, 4));
        return;
    }
    g_originalCentredX(rdram, ctx, runtime);
}

// The open state's labels, centred on the language's list (in the label
// call's a3: the US W/2+11, 73 or 53).
void centrePartyLabel(R5900Context *ctx)
{
    const int column = europeanColumn();
    const uint32_t from = getRegU32(ctx, 31);
    if (column < 0 || std::find(std::begin(kPartyLabelReturns), std::end(kPartyLabelReturns), from) ==
                          std::end(kPartyLabelReturns))
    {
        return;
    }
    const int32_t x = static_cast<int16_t>(getRegU32(ctx, 7));
    for (const PartyList &list : kPartyLists)
    {
        if (x == list.us / 2 + 11)
        {
            ctx->r[7] = _mm_set_epi64x(_mm_extract_epi64(ctx->r[7], 1),
                                       static_cast<int64_t>(list.widths[column] / 2 + 11));
            return;
        }
    }
}

void returnSize(R5900Context *ctx, uint32_t size, const char *what)
{
    std::fprintf(stderr, "[rg] %s: 0x%X blocks (the PAL size)\n", what, size);
    setReturnU32(ctx, size);
    ctx->pc = getRegU32(ctx, 31);
}

void menuResStackSize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!g_active)
    {
        g_originalMenuResSize(rdram, ctx, runtime);
        return;
    }
    returnSize(ctx, 0x1900u, "menu resource stack");
}

void gameDataStackSize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!g_active)
    {
        g_originalGameDataSize(rdram, ctx, runtime);
        return;
    }
    returnSize(ctx, 0x12A00u, "game data stack");
}

// ResGroupStackInit (group, parent, index, name, size): size is the fifth argument, t0.
void resGroupStackInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_active)
    {
        const uint32_t size = getRegU32(ctx, 8);
        for (const Resize &resize : kGroupSizes)
        {
            if (size == resize.us)
            {
                ctx->r[8] = _mm_set_epi64x(0, static_cast<int64_t>(resize.pal));
                break;
            }
        }
    }
    g_originalGroupInit(rdram, ctx, runtime);
}

} // namespace

void setPreferred(int choice)
{
    g_preferred.store(choice, std::memory_order_relaxed);
}

int language()
{
    return g_language.load(std::memory_order_relaxed);
}

void chooseAtBoot(uint8_t *rdram, PS2Runtime &runtime)
{
    // RG_TEXT_LANG (a developer's switch, any two letters of the game's table) over
    // the menu's choice.
    std::string wanted;
    if (const char *env = std::getenv("RG_TEXT_LANG"); env && *env)
    {
        wanted = env;
    }
    else
    {
        const int choice = g_preferred.load(std::memory_order_relaxed);
        if (choice > 0 && choice < static_cast<int>(sizeof(kCodes) / sizeof(kCodes[0])))
        {
            wanted = kCodes[choice];
        }
    }
    if (wanted.empty())
    {
        return;
    }
    for (char &c : wanted)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    std::error_code ec;
    if (wanted != "EN" && !std::filesystem::exists(languageIndex(wanted), ec))
    {
        std::fprintf(stderr,
                     "[rg] the text stays English: disc/EU/%s.idx is missing (Install-European.bat makes it "
                     "from your European disc)\n",
                     wanted.c_str());
        return;
    }
    for (int i = 0; i < kLanguageTableSize; ++i)
    {
        const uint32_t text = readWord(rdram, kLanguageTable + 4u * static_cast<uint32_t>(i));
        if (text == 0)
        {
            continue;
        }
        const char *name = reinterpret_cast<const char *>(rdram + (text & 0x1FFFFFFFu));
        if (std::strncmp(name, wanted.c_str(), 2) == 0 && name[2] == '\0')
        {
            writeWord(rdram, kTextLanguage, text);
            g_code.clear();
            for (const char *p = name; *p; ++p)
            {
                g_code += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
            }
            g_runtime = &runtime;
            g_active = g_code != "en";
            for (int choice = 1; choice < static_cast<int>(sizeof(kCodes) / sizeof(kCodes[0])); ++choice)
            {
                if (wanted == kCodes[choice])
                {
                    g_language.store(choice, std::memory_order_relaxed);
                }
            }
            std::fprintf(stderr, "[rg] text in %s\n", name);
            return;
        }
    }
    std::fprintf(stderr, "[rg] %s is not in the game's language table; the text stays English\n", wanted.c_str());
}

struct Hook
{
    uint32_t address;
    PS2Runtime::RecompiledFunction replacement;
    PS2Runtime::RecompiledFunction &original;
};

// Two hooks that only work together: both in, or neither (the first is put
// back when the second cannot go in).
bool hookBoth(PS2Runtime &runtime, Hook first, Hook second)
{
    if (!rg::timing::hookFunction(runtime, first.address, first.replacement, first.original))
    {
        return false;
    }
    if (rg::timing::hookFunction(runtime, second.address, second.replacement, second.original))
    {
        return true;
    }
    runtime.replaceFunction(first.address, first.original);
    return false;
}

void install(PS2Runtime &runtime)
{
    g_runtime = &runtime;
    if (!rg::timing::hookFunction(runtime, kDirTableFindFile, &dirTableFindFile, g_original))
    {
        std::fprintf(stderr, "[rg] the text stays in English\n");
    }
    if (!rg::timing::hookFunction(runtime, kArchiveFindEntryByName, &archiveFindEntryByName, g_originalFindEntry))
    {
        std::fprintf(stderr, "[rg] the font entries keep the text language's names\n");
    }
    if (!rg::timing::hookFunction(runtime, kTextSymbolTagFind, &textSymbolTagFind, g_originalSymbolFind))
    {
        std::fprintf(stderr, "[rg] the accent tags stay as they are\n");
    }
    if (!rg::timing::hookFunction(runtime, kFontGetCharWidth, &fontGetCharWidth, g_originalCharWidth) ||
        !rg::timing::hookFunction(runtime, kFontGetGlyphWidth, &fontGetGlyphWidth, g_originalGlyphWidth))
    {
        std::fprintf(stderr, "[rg] the accents keep the width of a full-width letter\n");
    }
    if (!rg::timing::hookFunction(runtime, kFontGetLineHeight, &fontGetLineHeight, g_originalLineHeight) ||
        !rg::timing::hookFunction(runtime, kFontGetGlyphRect, &fontGetGlyphRect, g_originalGlyphRect))
    {
        std::fprintf(stderr, "[rg] the names over people keep the US letter rows\n");
    }
    if (!rg::timing::hookFunction(runtime, kMenuResStackSize, &menuResStackSize, g_originalMenuResSize) ||
        !rg::timing::hookFunction(runtime, kGameDataStackSize, &gameDataStackSize, g_originalGameDataSize) ||
        !rg::timing::hookFunction(runtime, kResGroupStackInit, &resGroupStackInit, g_originalGroupInit))
    {
        std::fprintf(stderr, "[rg] the menus keep the US sizes (European layouts may not fit)\n");
    }
    if (!rg::timing::hookFunction(runtime, kUiLayoutStringGlyphs, &uiLayoutStringGlyphs, g_originalUiLayout))
    {
        std::fprintf(stderr, "[rg] the menus' accent tags stay as they are\n");
    }
    if (!rg::timing::hookFunction(runtime, kPauseReasonDraw, &pauseReasonDraw, g_originalPauseDraw))
    {
        std::fprintf(stderr, "[rg] the pause screen keeps the US pieces\n");
    }
    if (!rg::timing::hookFunction(runtime, kMainSubSelectLayout, &mainSubSelectLayout, g_originalSubLayout))
    {
        std::fprintf(stderr, "[rg] the main menu's System list keeps the US rows' width\n");
    }
    if (!rg::timing::hookFunction(runtime, kPartyCmdOpen, &partyCmdOpen, g_originalPartyOpen) ||
        !rg::timing::hookFunction(runtime, kPartyCmdHighlight, &partyCmdHighlight, g_originalPartyHighlight))
    {
        std::fprintf(stderr, "[rg] a party member's commands keep the US widths\n");
    }
    if (!rg::timing::hookFunction(runtime, kItemTabsUpdate, &itemTabsUpdate, g_originalItemTabs))
    {
        std::fprintf(stderr, "[rg] the Rare Items tab's heading keeps the English place\n");
    }
    if (!hookBoth(runtime, {kWindowFit, &windowFit, g_originalWindowFit},
                  {kSkillLayout, &skillLayout, g_originalSkillLayout}))
    {
        std::fprintf(stderr, "[rg] the Revelation Flow list keeps the US layout\n");
    }
    if (!rg::timing::hookFunction(runtime, kWeaponsHeader, &weaponsHeader, g_originalWeaponsHeader))
    {
        std::fprintf(stderr, "[rg] the weapons page keeps the US heading order\n");
    }
    if (!hookBoth(runtime, {kSpriteAnchorBL, &spriteAnchorBL, g_originalAnchorBL},
                  {kSpriteAnchorBR, &spriteAnchorBR, g_originalAnchorBR}))
    {
        std::fprintf(stderr, "[rg] the Attack Chain's count keeps the English place\n");
    }
    if (!rg::timing::hookFunction(runtime, kSaveMsgBoxShow, &saveMsgBoxShow, g_originalSaveMsgBox))
    {
        std::fprintf(stderr, "[rg] the save screens' Yes and No keep the US places\n");
    }
    if (!rg::timing::hookFunction(runtime, kUiSetTextCenteredX, &uiSetTextCenteredX, g_originalCentredX))
    {
        std::fprintf(stderr, "[rg] the battle's empty-gauge warning keeps its first line where English has it\n");
        // The save screens' Yes/No centre with the game's own all the same.
        if (runtime.hasFunction(kUiSetTextCenteredX))
        {
            g_originalCentredX = runtime.lookupFunction(kUiSetTextCenteredX);
        }
    }
}

} // namespace rg::textlanguage
