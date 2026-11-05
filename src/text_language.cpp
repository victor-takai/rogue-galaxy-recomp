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

#include "scene_timing.h"

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
    std::fprintf(stderr, "[rg] %s.idx: %zu files%s\n", upper.c_str(), g_text.files.size(),
                 g_text.ok ? "" : " (not usable)");
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
            std::fprintf(stderr, "[rg] text in %s\n", name);
            return;
        }
    }
    std::fprintf(stderr, "[rg] %s is not in the game's language table; the text stays English\n", wanted.c_str());
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
}

} // namespace rg::textlanguage
