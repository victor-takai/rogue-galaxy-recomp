#include "text_language.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "scene_timing.h"

namespace rg::textlanguage
{
namespace
{

constexpr uint32_t kDirTableFindFile = 0x00182F70u; // (table, entry, path), one archive's index
constexpr uint32_t kArchiveFindEntryByName = 0x00181E20u; // (archive, name), an entry of a pack
constexpr uint32_t kTextSymbolTagFind = 0x0026EFE0u; // (str), the symbol tag at its start
constexpr uint32_t kFontGetGlyphWidth = 0x0026F420u; // (font, code)
constexpr uint32_t kFontGetCharWidth = 0x0026F4C0u;  // (font, code), from the width table
constexpr uint32_t kSceCdSearchFile = 0x00129AE0u;  // (file, path), the runtime's stub
constexpr uint32_t kTextLanguage = 0x00481528u;     // the string `?` becomes
constexpr uint32_t kLanguageTable = 0x0044AF38u;    // pointers to "EN", "ES", ...
constexpr int kLanguageTableSize = 12;
constexpr uint32_t kDiscLayer = 0x004811E0u;        // set by CdDatArchivesOpenOnLayer
constexpr const char *kPalFolder = "EU";
constexpr const char *kFontPack = "fontset_main_eur.pak"; // built from the two discs' packs

PS2Runtime::RecompiledFunction g_original = nullptr;
PS2Runtime::RecompiledFunction g_originalFindEntry = nullptr;
PS2Runtime::RecompiledFunction g_originalSymbolFind = nullptr;
PS2Runtime::RecompiledFunction g_originalGlyphWidth = nullptr;
PS2Runtime::RecompiledFunction g_charWidth = nullptr; // FontGetCharWidth, called as it is
PS2Runtime *g_runtime = nullptr;
bool g_active = false;
std::string g_code; // lower case, "fr"

struct Entry
{
    uint32_t sector = 0; // from the start of the DAT
    uint32_t size = 0;
};

// One DATA<layer>_0 index, its names lower case with backslashes.
struct Archive
{
    bool loaded = false;
    bool ok = false;
    uint32_t baseLbn = 0;
    std::unordered_map<std::string, Entry> files;
};

Archive g_us[2];
Archive g_pal[2];
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

// The HD6 index: header, token pool, names as LEB128 token ids, one u64 a file
// (18 bit name offset, 22 bit sector in the DAT, 24 bit size in `unit` bytes),
// as the game reads it in DirTableFindFile.
bool parseIndex(const std::filesystem::path &file, Archive &archive)
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
        archive.files[normalize(name.c_str())] = entry;
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

Archive &load(Archive *set, bool pal, uint32_t layer, uint8_t *rdram, R5900Context *ctx)
{
    Archive &archive = set[layer & 1];
    if (archive.loaded)
    {
        return archive;
    }
    archive.loaded = true;
    const std::string file = "DATA" + std::to_string(layer & 1) + "_0";
    const std::filesystem::path root = PS2Runtime::getIoPaths().cdRoot / (pal ? kPalFolder : "");
    archive.ok = parseIndex(root / (file + ".HD6"), archive);
    if (archive.ok && pal)
    {
        archive.baseLbn =
            discSector(rdram, ctx, std::string("cdrom0:\\") + kPalFolder + "\\" + file + ".DAT;1");
        archive.ok = archive.baseLbn != 0;
    }
    std::fprintf(stderr, "[rg] %s %s: %zu files%s\n", pal ? "PAL" : "US", file.c_str(), archive.files.size(),
                 archive.ok ? "" : " (not usable)");
    return archive;
}

// The text language's code in a name, as `_fr.` or `\fr\`; npos if none.
size_t languageToken(const std::string &name)
{
    for (const std::string &pattern : {"_" + g_code + ".", "\\" + g_code + "\\"})
    {
        const size_t at = name.rfind(pattern);
        if (at != std::string::npos)
        {
            return at + 1;
        }
    }
    return std::string::npos;
}

bool isEventPack(const std::string &name)
{
    return name.rfind("event\\", 0) == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, ".pac") == 0;
}

void dirTableFindFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t entryAddress = getRegU32(ctx, 5);
    const uint32_t pathAddress = getRegU32(ctx, 6);
    if (g_active && pathAddress != 0)
    {
        char *path = reinterpret_cast<char *>(rdram + (pathAddress & 0x1FFFFFFFu));
        const std::string name = normalize(path);
        const size_t token = languageToken(name);
        const bool pack = isEventPack(name);
        // Developer switches for the study: RG_TEXT_ONLY=<part of a name> serves
        // only those names from the PAL disc; RG_TEXT_FORCE=<part> serves also
        // those that carry no language (a file both discs hold, to check the lookup).
        static const char *only = std::getenv("RG_TEXT_ONLY");
        static const char *force = std::getenv("RG_TEXT_FORCE");
        const bool forced = force && *force && name.find(force) != std::string::npos;
        const bool allowed = !only || !*only || name.find(only) != std::string::npos;
        static const bool trace = std::getenv("RG_TEXT_TRACE") != nullptr;
        if (trace && g_logged < 600 && name.compare(0, 4, "wav\\") != 0)
        {
            ++g_logged;
            std::lock_guard<std::mutex> lock(g_mutex);
            Archive &us = load(g_us, false, readWord(rdram, kDiscLayer) & 1u, rdram, ctx);
            std::fprintf(stderr, "[rg] lookup%s%s: %s\n", (token != std::string::npos || pack) ? " (text)" : "",
                         us.files.find(name) == us.files.end() ? " (not in the US index)" : "", name.c_str());
        }
        if (forced || (allowed && (token != std::string::npos || pack)))
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const uint32_t layer = readWord(rdram, kDiscLayer) & 1u;
            Archive &pal = load(g_pal, true, layer, rdram, ctx);
            // The font: PAL's pack with the US pack's fifth page (the US executable
            // loads five normal pages, PAL's has four), built into disc/EU.
            if (name.rfind("meswin\\fontset_main_", 0) == 0)
            {
                static uint32_t fontSector = 0;
                static uint32_t fontSize = 0;
                if (!fontSector)
                {
                    std::error_code ec;
                    const std::filesystem::path file = PS2Runtime::getIoPaths().cdRoot / kPalFolder / kFontPack;
                    fontSize = static_cast<uint32_t>(std::filesystem::exists(file, ec) ? std::filesystem::file_size(file, ec) : 0);
                    if (fontSize)
                    {
                        fontSector = discSector(rdram, ctx, std::string("cdrom0:\\") + kPalFolder + "\\" + kFontPack + ";1");
                    }
                }
                if (fontSector && fontSize)
                {
                    writeWord(rdram, entryAddress + 0x0, 0);
                    writeWord(rdram, entryAddress + 0x4, fontSector);
                    writeWord(rdram, entryAddress + 0x8, fontSize);
                    writeWord(rdram, entryAddress + 0xC, 0);
                    writeWord(rdram, entryAddress + 0x10, 0);
                    std::fprintf(stderr, "[rg] font from %s\n", kFontPack);
                    setReturnU32(ctx, 1);
                    ctx->pc = getRegU32(ctx, 31);
                    return;
                }
            }
            auto found = pal.ok ? pal.files.find(name) : pal.files.end();
            if (pal.ok && found == pal.files.end() && token != std::string::npos &&
                name.find("fontset_") != std::string::npos)
            {
                // The PAL disc has one font for its five languages, named EUR.
                std::string european = name;
                european.replace(token, g_code.size(), "eur");
                found = pal.files.find(european);
            }
            if (found != pal.files.end())
            {
                writeWord(rdram, entryAddress + 0x0, 0);
                writeWord(rdram, entryAddress + 0x4, pal.baseLbn + found->second.sector);
                // The game sizes some buffers to the US file (menu_static_res: 50000
                // bytes there, 52176 in PAL), and the longer PAL file overruns them.
                // A language file is cut to its English counterpart's size as a
                // stopgap, until the buffers are found.
                uint32_t size = found->second.size;
                static const char *cut = std::getenv("RG_TEXT_CUT"); // part of a name; default menu_static_res
                const std::string cutList = std::string(",") + (cut && *cut ? cut : "menu_static_res,cmd_static") + ",";
                bool cutThis = false;
                for (size_t from = 1; from < cutList.size(); )
                {
                    const size_t to = cutList.find(',', from);
                    cutThis = cutThis || name.find(cutList.substr(from, to - from)) != std::string::npos;
                    from = to + 1;
                }
                if (token != std::string::npos && cutThis)
                {
                    std::string english = name;
                    english.replace(token, g_code.size(), "en");
                    Archive &us = load(g_us, false, layer, rdram, ctx);
                    const auto counterpart = us.files.find(english);
                    if (counterpart != us.files.end() && counterpart->second.size < size)
                    {
                        std::fprintf(stderr, "[rg] %s cut from %u to %u bytes (the US buffer)\n", name.c_str(), size,
                                     counterpart->second.size);
                        size = counterpart->second.size;
                    }
                }
                writeWord(rdram, entryAddress + 0x8, size);
                writeWord(rdram, entryAddress + 0xC, 0);
                writeWord(rdram, entryAddress + 0x10, 0);
                if (g_logged < 60)
                {
                    ++g_logged;
                    std::fprintf(stderr, "[rg] text from the PAL disc: %s\n", name.c_str());
                }
                setReturnU32(ctx, 1);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            if (token != std::string::npos)
            {
                // Neither disc has it in this language: the English name.
                Archive &us = load(g_us, false, layer, rdram, ctx);
                if (us.files.find(name) == us.files.end())
                {
                    std::string english = name;
                    english.replace(token, g_code.size(), "en");
                    if (us.files.find(english) != us.files.end())
                    {
                        size_t lead = 0;
                        while (path[lead] == '\\' || path[lead] == '/')
                        {
                            ++lead;
                        }
                        std::memcpy(path + lead + token, "en", 2);
                        if (g_logged < 60)
                        {
                            ++g_logged;
                            std::fprintf(stderr, "[rg] text in English instead: %s\n", name.c_str());
                        }
                    }
                }
            }
        }
    }
    g_original(rdram, ctx, runtime);
}

// An entry of a font pack is asked for by the text language (fonttbl_normal_FR.bin,
// FontTex_name_FR_0.tm2); PAL's pack, the one served in place of the missing
// fontset_main_fr.pak, names them EUR.
void archiveFindEntryByName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t nameAddress = getRegU32(ctx, 5);
    if (g_active && nameAddress != 0)
    {
        char *name = reinterpret_cast<char *>(rdram + (nameAddress & 0x1FFFFFFFu));
        const char *base = name;
        for (const char *p = name; *p; ++p)
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
                    std::memcpy(const_cast<char *>(base), european.c_str(), european.size() + 1);
                }
            }
        }
    }
    g_originalFindEntry(rdram, ctx, runtime);
}

// The European texts spell accents and a few marks as tags (<'e>, <degree>). The PAL
// executable matches them in a table of 64 and gives the nth the glyph code 107 + n,
// the cell where its font page draws that mark; the US one has no such table, so the
// tag would print as it is. The names are in the PAL table's order.
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
// table (a byte per font at 0x44E851 + 4 * code) holds the widths of the cells' old glyphs.
constexpr uint8_t kAccentWidths[kAccentCount][3] = {{6, 6, 4}, {6, 6, 4}, {6, 6, 6}, {6, 6, 6}, {6, 6, 6}, {10, 10, 8}, {14, 12, 12}, {16, 16, 14}, {6, 4, 4}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {6, 6, 6}, {16, 16, 12}, {16, 16, 12}, {14, 16, 12}, {14, 16, 12}, {14, 16, 12}, {14, 16, 12}, {14, 14, 10}, {12, 12, 8}, {12, 12, 8}, {12, 12, 8}, {12, 12, 8}, {10, 8, 8}, {10, 8, 8}, {12, 10, 8}, {12, 10, 8}, {12, 12, 10}, {14, 14, 10}, {14, 14, 10}, {14, 14, 10}, {14, 14, 10}, {14, 12, 10}, {14, 12, 10}, {14, 12, 10}, {14, 12, 10}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {10, 8, 8}, {10, 8, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {14, 14, 10}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {12, 10, 8}, {10, 10, 8}, {10, 10, 8}};
constexpr uint32_t kWidthTable = 0x0044E851u;
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
                    static int shown = 0;
                    if (shown < 3 && std::getenv("RG_TEXT_TRACE"))
                    {
                        ++shown;
                        const uint8_t *w = rdram + ((kWidthTable + 4u * (kAccentFirstCode + static_cast<uint32_t>(i))) & 0x1FFFFFFFu);
                        std::fprintf(stderr, "[rg] accent %s code %u widths %u %u %u %u\n", kAccentTags[i],
                                     kAccentFirstCode + static_cast<uint32_t>(i), w[0], w[1], w[2], w[3]);
                    }
                    setReturnU32(ctx, g_accentEntries + 8u * static_cast<uint32_t>(i));
                    ctx->pc = getRegU32(ctx, 31);
                    return;
                }
            }
        }
    }
    g_originalSymbolFind(rdram, ctx, runtime);
}

// FontGetGlyphWidth (font, code): the accent codes sit in the part of the code space the US
// executable takes for full-width characters; give them the width of the table instead.
void fontGetGlyphWidth(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t code = getRegU32(ctx, 5) & 0xFFFFu;
    if (g_active && code >= kAccentFirstCode && code < kAccentFirstCode + kAccentCount && g_charWidth)
    {
        g_charWidth(rdram, ctx, runtime);
        return;
    }
    g_originalGlyphWidth(rdram, ctx, runtime);
}

} // namespace

void chooseAtBoot(uint8_t *rdram, PS2Runtime &runtime)
{
    const char *wanted = std::getenv("RG_TEXT_LANG");
    if (!wanted || !*wanted)
    {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(PS2Runtime::getIoPaths().cdRoot / kPalFolder / "DATA0_0.HD6", ec))
    {
        std::fprintf(stderr, "[rg] a European text language needs the PAL disc's files in disc/EU\n");
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
        if (std::strncmp(name, wanted, 2) == 0 && name[2] == '\0')
        {
            writeWord(rdram, kTextLanguage, text);
            g_code.clear();
            for (const char *p = name; *p; ++p)
            {
                g_code += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
            }
            g_runtime = &runtime;
            g_active = g_code != "en";
            for (int i = 0; g_active && i < kAccentCount; ++i)
            {
                for (uint32_t font = 0; font < 3; ++font)
                {
                    rdram[(kWidthTable + 4u * (kAccentFirstCode + static_cast<uint32_t>(i)) + font) & 0x1FFFFFFFu] =
                        kAccentWidths[i][font];
                }
            }
            std::fprintf(stderr, "[rg] text in %s (string at 0x%08X)\n", name, text);
            return;
        }
    }
    std::fprintf(stderr, "[rg] RG_TEXT_LANG=%s is not in the game's language table\n", wanted);
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
    g_charWidth = runtime.lookupFunction(kFontGetCharWidth);
    if (!rg::timing::hookFunction(runtime, kFontGetGlyphWidth, &fontGetGlyphWidth, g_originalGlyphWidth))
    {
        std::fprintf(stderr, "[rg] the accents keep the width of a full-width letter\n");
    }
}

} // namespace rg::textlanguage
