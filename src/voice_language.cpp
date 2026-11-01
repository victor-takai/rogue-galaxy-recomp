#include "voice_language.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "ps2_runtime.h"
#include "scene_timing.h"

namespace rg::voicelanguage
{
namespace
{

constexpr uint32_t kStaticInitLangCodeTables = 0x004698B0u;
constexpr uint32_t kVoiceLanguage = 0x0048152Cu; // the string `!` becomes
constexpr uint32_t kLanguageTable = 0x0044AF38u; // pointers to "EN", "ES", ...
constexpr int kLanguageTableSize = 12;
constexpr uint32_t kCdDataSetNamesInit = 0x001C31A0u; // fills the two archive names of a layer
constexpr uint32_t kDiscLayer = 0x00481560u;
constexpr uint32_t kVoiceNameSlot = 0x20u;            // the second name, DATA%d_1
constexpr uint32_t kNameSlotSize = 0x20u;
constexpr const char *kJapaneseFolder = "JA";
constexpr uint32_t kSoundStreamQueueOpen = 0x001C0360u; // (stream, path), a movie's file is opened here
constexpr uint32_t kStreamMgrStartChan = 0x001D66F0u;   // (manager, channel, path, ...), a voice starts here
constexpr uint32_t kEventVoiceMakePath = 0x0024EDB0u;   // (list, id, buffer, size), a scene's voice path
constexpr uint32_t kPathBufferSize = 0x104u;             // MovieStreamOpen's `full`

PS2Runtime::RecompiledFunction g_original = nullptr;
PS2Runtime::RecompiledFunction g_originalNames = nullptr;
PS2Runtime::RecompiledFunction g_originalStreamOpen = nullptr;
PS2Runtime::RecompiledFunction g_originalStartChan = nullptr;
PS2Runtime::RecompiledFunction g_originalMakePath = nullptr;
bool g_japaneseVoices = false;
std::atomic<bool> g_requested{false}; // the menu's switch, read at boot

bool startsWithNoCase(const std::string &text, const std::string &prefix)
{
    if (text.size() < prefix.size())
    {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(text[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
        {
            return false;
        }
    }
    return true;
}

uint32_t readWord(const uint8_t *rdram, uint32_t address)
{
    uint32_t value = 0;
    std::memcpy(&value, rdram + (address & 0x1FFFFFFFu), sizeof(value));
    return value;
}

void staticInitLangCodeTables(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_original(rdram, ctx, runtime);

    // RG_VOICE_LANG names any language of the game's table (a developer's
    // switch); the menu's switch means Japanese.
    const char *wanted = std::getenv("RG_VOICE_LANG");
    if (!wanted || !*wanted)
    {
        wanted = g_requested.load() ? "JA" : nullptr;
    }
    if (!wanted)
    {
        return;
    }
    if (std::strcmp(wanted, "JA") == 0)
    {
        std::error_code ec;
        if (!std::filesystem::exists(PS2Runtime::getIoPaths().cdRoot / kJapaneseFolder / "DATA0_1.HD6", ec))
        {
            std::fprintf(stderr,
                         "[rg] Japanese voices need the Japanese disc's files in disc/JA: "
                         "python setup.py extract-japanese --iso <your Japanese Director's Cut>\n");
            return;
        }
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
            std::memcpy(rdram + kVoiceLanguage, &text, sizeof(text));
            g_japaneseVoices = std::strcmp(name, "JA") == 0;
            std::fprintf(stderr, "[rg] voices in %s (string at 0x%08X)\n", name, text);
            return;
        }
    }
    std::fprintf(stderr, "[rg] RG_VOICE_LANG=%s is not in the game's language table\n", wanted);
}

// The voice archives of a layer are DATA<layer>_1 (.HD6 index, .DAT data). With
// Japanese voices their names become JA/DATA<layer>_1, which the disc folder
// serves from disc/JA/ (the player's own Japanese DATA0_1 and DATA1_1).
void cdDataSetNamesInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t names = getRegU32(ctx, 4);
    g_originalNames(rdram, ctx, runtime);
    if (!g_japaneseVoices)
    {
        return;
    }
    const uint32_t layer = readWord(rdram, kDiscLayer);
    const std::string file = "DATA" + std::to_string(layer) + "_1";
    const std::filesystem::path index =
        PS2Runtime::getIoPaths().cdRoot / kJapaneseFolder / (file + ".HD6");
    std::error_code ec;
    if (!std::filesystem::exists(index, ec))
    {
        static bool told = false;
        if (!told)
        {
            told = true;
            std::fprintf(stderr, "[rg] no Japanese voice archive at %s: the voices stay in English\n",
                         index.string().c_str());
        }
        return;
    }
    const std::string name = std::string(kJapaneseFolder) + "/" + file;
    if (name.size() + 1 > kNameSlotSize)
    {
        return;
    }
    std::memcpy(rdram + ((names + kVoiceNameSlot) & 0x1FFFFFFFu), name.c_str(), name.size() + 1);
    std::fprintf(stderr, "[rg] layer %u voices read from %s\n", layer, index.string().c_str());
}

// MovieStreamOpen builds cdrom0:\MOVIE\<name>;1 and queues it here. With Japanese
// voices a movie that exists as disc/JA/MOVIE/<name> plays from there: the
// sound is part of the file. The rest (silent movies, those the two discs share)
// stay on the US disc.
void soundStreamQueueOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t pathAddress = getRegU32(ctx, 5);
    if (g_japaneseVoices && pathAddress != 0)
    {
        char *path = reinterpret_cast<char *>(rdram + (pathAddress & 0x1FFFFFFFu));
        const std::string prefix = "cdrom0:\\MOVIE\\";
        const std::string text = path;
        if (text.size() > prefix.size() && startsWithNoCase(text, prefix))
        {
            std::string rest = text.substr(prefix.size());
            std::string relative = rest;
            const size_t version = relative.rfind(';');
            if (version != std::string::npos)
            {
                relative.erase(version);
            }
            for (char &c : relative)
            {
                if (c == '\\')
                {
                    c = '/';
                }
            }
            std::error_code ec;
            const std::filesystem::path japanese =
                PS2Runtime::getIoPaths().cdRoot / kJapaneseFolder / "MOVIE" / relative;
            const std::string replaced = std::string("cdrom0:\\") + kJapaneseFolder + "\\MOVIE\\" + rest;
            if (std::filesystem::exists(japanese, ec) && replaced.size() + 1 <= kPathBufferSize)
            {
                std::memcpy(path, replaced.c_str(), replaced.size() + 1);
                std::fprintf(stderr, "[rg] movie from %s\n", japanese.string().c_str());
            }
        }
    }
    g_originalStreamOpen(rdram, ctx, runtime);
}

// Voices start here with the localized path. With Japanese voices each one is
// written to the log, to tell which lines have no Japanese file.
void streamMgrStartChan(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t pathAddress = getRegU32(ctx, 6);
    if (g_japaneseVoices && pathAddress != 0)
    {
        const char *path = reinterpret_cast<const char *>(rdram + (pathAddress & 0x1FFFFFFFu));
        if (std::strncmp(path, "wav/", 4) == 0)
        {
            std::fprintf(stderr, "[rg] voice %s\n", path);
        }
    }
    g_originalStartChan(rdram, ctx, runtime);
}

// A scene's voice path is built here; with Japanese voices it is logged.
void eventVoiceMakePath(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t bufferAddress = getRegU32(ctx, 6);
    g_originalMakePath(rdram, ctx, runtime);
    if (g_japaneseVoices && bufferAddress != 0)
    {
        const char *path = reinterpret_cast<const char *>(rdram + (bufferAddress & 0x1FFFFFFFu));
        if (std::strncmp(path, "wav/", 4) == 0)
        {
            std::fprintf(stderr, "[rg] voice %s\n", path);
        }
    }
}

} // namespace

void setEnabled(bool on)
{
    g_requested.store(on);
}

void install(PS2Runtime &runtime)
{
    if (!rg::timing::hookFunction(runtime, kStaticInitLangCodeTables, &staticInitLangCodeTables, g_original))
    {
        std::fprintf(stderr, "[rg] the voice language stays as the game sets it\n");
    }
    if (!rg::timing::hookFunction(runtime, kCdDataSetNamesInit, &cdDataSetNamesInit, g_originalNames))
    {
        std::fprintf(stderr, "[rg] the voice archives stay where the game puts them\n");
    }
    if (!rg::timing::hookFunction(runtime, kSoundStreamQueueOpen, &soundStreamQueueOpen, g_originalStreamOpen))
    {
        std::fprintf(stderr, "[rg] the movies stay on the US disc\n");
    }
    if (!rg::timing::hookFunction(runtime, kStreamMgrStartChan, &streamMgrStartChan, g_originalStartChan))
    {
        std::fprintf(stderr, "[rg] voice lines are not logged\n");
    }
    if (!rg::timing::hookFunction(runtime, kEventVoiceMakePath, &eventVoiceMakePath, g_originalMakePath))
    {
        std::fprintf(stderr, "[rg] scene voice lines are not logged\n");
    }
}

} // namespace rg::voicelanguage
