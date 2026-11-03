#include "texture_pack.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dds.h"
#include "gfx/backends/sdlgpu/sdlgpu_backend.h"
#include "mini_json.h"
#include "zip_reader.h"

// raylib's stb_image and stb_image_write, which the runtime links in.
extern "C" int stbi_write_png(const char *filename, int w, int h, int comp, const void *data, int stride_in_bytes);
extern "C" unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y,
                                                int *channels_in_file, int desired_channels);
extern "C" void stbi_image_free(void *retval_from_stbi_load);

namespace rg::texturePack
{
namespace
{

dq8::gfx::SdlGpuBackend *g_backend = nullptr;

// A region as the collected files name it.
uint64_t regionKey(uint64_t texels, uint64_t clut, uint32_t width, uint32_t height)
{
    uint64_t key = 1469598103934665603ull;
    for (const uint64_t value : {texels, clut, uint64_t(width) << 32u | height})
    {
        key ^= value;
        key *= 1099511628211ull;
    }
    return key;
}

std::string collectedName(const dq8::gfx::RegionRequest &request)
{
    char name[64];
    std::snprintf(name, sizeof name, "%016llX_%016llX_%ux%u.png", static_cast<unsigned long long>(request.texels),
                  static_cast<unsigned long long>(request.clut), request.width, request.height);
    return name;
}

std::string lowerCase(std::string text)
{
    for (char &c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// The packs (docs/TEXTURE_PACKS.md): each region's larger version, by the
// collected file's name, TEXELS_CLUT_WxH.png (or .dds). CLUT may be "any",
// which answers for every palette the content shows with (a palette's own
// file wins). A pack is a folder or a zip. Read on a thread of its own, so a
// picture seen for the first time costs the GS thread nothing; it draws the
// region as it is until the picture is ready (the item menu reloads one icon
// slot icon after icon, and reading each from disk there held its frames up).
constexpr int kFormat = 1;
constexpr uint32_t kLargestPicture = 8192u;

struct Entry
{
    // A file's path, or the entry's name inside `zip`.
    std::string path;
    std::shared_ptr<zip::Archive> zip;
    // The file's alpha is the GS's already (0x80 opaque), not a PNG's 0 to 255.
    bool rawAlpha = false;
    // Drawn from its own texels, not filtered across them (pack.json's "filter").
    bool nearest = false;
};

struct Pack
{
    std::string id;   // the folder's or zip's name
    std::string key;  // id as a settings key: letters, digits, _ and -
    std::string name; // pack.json's, or the id
    bool enabled = true;
    std::unordered_map<uint64_t, Entry> exact;    // by regionKey
    std::unordered_map<uint64_t, Entry> wildcard; // by regionKey, clut 0
    std::unordered_set<uint64_t> ignore;          // regionKeys left as the game draws them
    std::unordered_set<uint64_t> ignoreWildcard;
    size_t regions = 0u;
};

// What the enabled packs make together: a later pack over an earlier one, its
// ignored regions taking the earlier ones' away.
struct Merged
{
    std::unordered_map<uint64_t, Entry> exact, wildcard;
    std::unordered_set<uint64_t> ignored;
};

struct Image
{
    uint32_t width = 0u, height = 0u;
    // RGBA8, or a DDS's blocks as they are (`blocks` is their kRegionBc*).
    std::vector<uint8_t> rgba;
    int blocks = 0;
    bool nearest = false;
};
struct Pending
{
    uint64_t key;
    uint64_t generation;
    Entry entry;
    // The block formats the GPU samples itself, for a whole texture.
    int blockFormats = 0;
};

// RG_TEXTURE_PACK_DECODE=1: every picture goes to the GPU decoded, as RGBA8
// (to compare with its blocks).
bool g_decodeAll = false;

std::vector<Pack> g_packs; // after scan(), read-only except `enabled`, under g_mutex
bool g_scanned = false;
std::mutex g_mutex;
std::condition_variable g_wake;
std::shared_ptr<const Merged> g_merged = std::make_shared<Merged>();
std::atomic<bool> g_anyPicture{false};
uint64_t g_generation = 0u;
std::deque<Pending> g_queue;
std::unordered_set<uint64_t> g_asked;
std::unordered_map<uint64_t, Image> g_ready;

// "TEXELS_CLUT_WxH", CLUT being 16 hex digits or "any"; no extension.
bool parseRegionName(const std::string &name, uint64_t &key, bool &wildcard)
{
    unsigned long long texels = 0u, clut = 0u;
    unsigned width = 0u, height = 0u;
    char tail = 0;
    wildcard = name.size() > 20u && name.compare(17u, 3u, "any") == 0;
    if (wildcard)
    {
        if (std::sscanf(name.c_str(), "%16llx_any_%ux%u%c", &texels, &width, &height, &tail) != 3)
        {
            return false;
        }
    }
    else if (std::sscanf(name.c_str(), "%16llx_%16llx_%ux%u%c", &texels, &clut, &width, &height, &tail) != 4)
    {
        return false;
    }
    key = regionKey(texels, clut, width, height);
    return true;
}

// A whole texture as PCSX2 names it, "TEX0HASH-CLUTHASH-BITS" (BITS: the
// format in its low 6 bits, then log2 of the width and of the height in 4 bits
// each), no extension. The picture of the palette's own hash wins; else any one
// of the texels' (a wildcard).
bool parseWholeName(const std::string &name, uint64_t &key, uint64_t &anyKey)
{
    unsigned long long texels = 0u, clut = 0u;
    unsigned bits = 0u;
    char tail = 0;
    // PCSX2 writes the hashes without leading zeros.
    if (std::count(name.begin(), name.end(), '-') != 2 ||
        name.find_first_not_of("0123456789abcdefABCDEF-") != std::string::npos ||
        std::sscanf(name.c_str(), "%16llx-%16llx-%8x%c", &texels, &clut, &bits, &tail) != 3)
    {
        return false;
    }
    const uint32_t width = 1u << ((bits >> 6u) & 15u), height = 1u << ((bits >> 10u) & 15u);
    key = regionKey(texels, clut, width, height);
    anyKey = regionKey(texels, 0u, width, height);
    return true;
}

bool hasExtension(const std::string &name, const char *extension)
{
    const std::string lower = lowerCase(name);
    const size_t length = std::char_traits<char>::length(extension);
    return lower.size() > length && lower.compare(lower.size() - length, length, extension) == 0;
}

bool isPicture(const std::string &name)
{
    return hasExtension(name, ".png") || hasExtension(name, ".dds");
}

// Where a pack's files are read from: a folder, or a zip (its files at the
// root, or all under one folder, as a zipped folder is).
struct Source
{
    std::filesystem::path directory;
    std::shared_ptr<zip::Archive> archive;
    std::string prefix;

    bool read(const std::string &relative, std::vector<uint8_t> &bytes) const
    {
        if (archive)
        {
            return archive->read(prefix + relative, bytes);
        }
        std::ifstream file(directory / relative, std::ios::binary);
        if (!file)
        {
            return false;
        }
        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return true;
    }

    Entry entry(const std::string &relative, bool rawAlpha) const
    {
        Entry entry;
        entry.path = archive ? prefix + relative : (directory / relative).string();
        entry.zip = archive;
        entry.rawAlpha = rawAlpha;
        return entry;
    }

    bool has(const std::string &relative) const
    {
        return archive ? archive->has(prefix + relative) : std::filesystem::is_regular_file(directory / relative);
    }

    // The pack's own files, flat, by name.
    std::vector<std::string> pictures() const
    {
        std::vector<std::string> names;
        if (archive)
        {
            for (const auto &item : archive->entries())
            {
                if (item.first.size() > prefix.size() && item.first.compare(0, prefix.size(), prefix) == 0)
                {
                    const std::string rest = item.first.substr(prefix.size());
                    if (rest.find('/') == std::string::npos && isPicture(rest))
                    {
                        names.push_back(rest);
                    }
                }
            }
        }
        else
        {
            std::error_code error;
            for (const auto &file : std::filesystem::directory_iterator(directory, error))
            {
                if (file.is_regular_file() && isPicture(file.path().filename().string()))
                {
                    names.push_back(file.path().filename().string());
                }
            }
        }
        return names;
    }
};

std::string settingsKey(const std::string &id)
{
    std::string key;
    for (const char c : id)
    {
        key.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '-' ? c : '_');
    }
    return key;
}

// One pack: its files, then its aliases (a region's name for any file inside
// the pack), then what it ignores (a region left as the game draws it,
// whatever the packs before gave). Returns false for a pack left out.
bool loadPack(const Source &source, const std::string &id, Pack &pack)
{
    pack.id = id;
    pack.name = id;
    bool rawAlpha = false;
    bool nearest = false;
    rg::json::Value index;
    if (std::vector<uint8_t> text; source.read("pack.json", text))
    {
        const std::string json(text.begin(), text.end());
        if (!rg::json::Reader(json).parse(index) || index.type != rg::json::Value::Type::Object)
        {
            std::fprintf(stderr, "[rg] texture pack %s: pack.json is not valid, the pack is left out\n",
                         id.c_str());
            return false;
        }
        if (const auto *format = index.find("format"); format && format->number > kFormat)
        {
            std::fprintf(stderr, "[rg] texture pack %s: format %g is newer than this game's %d, left out\n",
                         id.c_str(), format->number, kFormat);
            return false;
        }
        if (const auto *enabled = index.find("enabled");
            enabled && enabled->type == rg::json::Value::Type::Bool && !enabled->boolean)
        {
            pack.enabled = false;
        }
        if (const auto *title = index.find("name");
            title && title->type == rg::json::Value::Type::String && !title->string.empty())
        {
            pack.name = title->string;
        }
        if (const auto *alpha = index.find("alpha"); alpha && alpha->string == "ps2")
        {
            rawAlpha = true;
        }
        if (const auto *filter = index.find("filter");
            filter && filter->type == rg::json::Value::Type::String)
        {
            if (filter->string == "nearest")
            {
                nearest = true;
            }
            else if (filter->string != "linear")
            {
                std::fprintf(stderr, "[rg] texture pack %s: filter \"%s\" is neither \"linear\" nor \"nearest\"\n",
                             id.c_str(), filter->string.c_str());
            }
        }
    }
    auto add = [&](const std::string &region, const std::string &relative) {
        uint64_t key = 0u;
        bool wildcard = false;
        if (!parseRegionName(region, key, wildcard))
        {
            std::fprintf(stderr, "[rg] texture pack %s: %s is no region's name\n", id.c_str(), region.c_str());
            return;
        }
        Entry made = source.entry(relative, rawAlpha);
        made.nearest = nearest;
        (wildcard ? pack.wildcard : pack.exact)[key] = std::move(made);
        ++pack.regions;
    };
    for (const std::string &file : source.pictures())
    {
        const std::string stem = file.substr(0, file.rfind('.'));
        uint64_t key = 0u;
        bool wildcard = false;
        if (parseRegionName(stem, key, wildcard))
        {
            add(stem, file);
        }
        else if (uint64_t anyKey = 0u; parseWholeName(stem, key, anyKey))
        {
            // A PCSX2 pack keeps the console's alpha (0x80 opaque).
            Entry made = source.entry(file, true);
            made.nearest = nearest;
            pack.wildcard[anyKey] = made;
            pack.exact[key] = std::move(made);
            ++pack.regions;
        }
    }
    if (const auto *aliases = index.find("aliases"); aliases && aliases->type == rg::json::Value::Type::Object)
    {
        for (const auto &alias : aliases->members)
        {
            // A file inside the pack only: no "..", no drive or root.
            const std::filesystem::path relative(alias.second.string);
            if (alias.second.type != rg::json::Value::Type::String || relative.is_absolute() ||
                relative.has_root_name() || alias.second.string.find("..") != std::string::npos ||
                !isPicture(alias.second.string) || !source.has(alias.second.string))
            {
                std::fprintf(stderr, "[rg] texture pack %s: alias %s left out\n", id.c_str(), alias.first.c_str());
                continue;
            }
            add(alias.first, alias.second.string);
        }
    }
    if (const auto *ignore = index.find("ignore"); ignore && ignore->type == rg::json::Value::Type::Array)
    {
        for (const auto &item : ignore->items)
        {
            uint64_t key = 0u;
            bool wildcard = false;
            if (item.type == rg::json::Value::Type::String && parseRegionName(item.string, key, wildcard))
            {
                (wildcard ? pack.ignoreWildcard : pack.ignore).insert(key);
            }
        }
    }
    return true;
}

void loadFolder(const std::filesystem::path &directory)
{
    Pack pack;
    Source source;
    source.directory = directory;
    if (loadPack(source, directory.filename().string(), pack))
    {
        g_packs.push_back(std::move(pack));
    }
}

void loadZip(const std::filesystem::path &file)
{
    auto archive = std::make_shared<zip::Archive>();
    if (const char *error = archive->open(file.string()))
    {
        std::fprintf(stderr, "[rg] texture pack %s: %s, left out\n", file.filename().string().c_str(), error);
        return;
    }
    Source source;
    source.archive = archive;
    // Every file under one folder (a zipped folder) is that folder's pack.
    std::string first;
    bool nested = !archive->entries().empty();
    for (const auto &item : archive->entries())
    {
        const size_t slash = item.first.find('/');
        const std::string top = slash == std::string::npos ? std::string() : item.first.substr(0, slash + 1u);
        if (top.empty() || (!first.empty() && top != first))
        {
            nested = false;
            break;
        }
        first = top;
    }
    if (nested)
    {
        source.prefix = first;
    }
    Pack pack;
    if (loadPack(source, file.stem().string(), pack))
    {
        g_packs.push_back(std::move(pack));
    }
}

// The packs, once: RG_TEXTURE_PACK=DIR (or a .zip) is that one pack; else
// textures/hd (what make-pack.bat writes) first, then the folders and zips of
// textures/packs by name, a later one over an earlier.
void scan()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_scanned)
    {
        return;
    }
    g_scanned = true;
    std::error_code error;
    auto load = [](const std::filesystem::path &path) {
        if (std::filesystem::is_directory(path))
        {
            loadFolder(path);
        }
        else if (lowerCase(path.extension().string()) == ".zip")
        {
            loadZip(path);
        }
    };
    if (const char *one = std::getenv("RG_TEXTURE_PACK"); one && *one)
    {
        load(one);
    }
    else
    {
        if (std::filesystem::is_directory("textures/hd", error))
        {
            loadFolder("textures/hd");
        }
        std::vector<std::filesystem::path> items;
        for (const auto &item : std::filesystem::directory_iterator("textures/packs", error))
        {
            items.push_back(item.path());
        }
        std::sort(items.begin(), items.end());
        for (const auto &item : items)
        {
            load(item);
        }
    }
    // A key of its own for each, for the settings file.
    std::unordered_set<std::string> used;
    for (Pack &pack : g_packs)
    {
        const std::string base = settingsKey(pack.id);
        pack.key = base;
        for (int n = 2; !used.insert(pack.key).second; ++n)
        {
            pack.key = base + "_" + std::to_string(n);
        }
    }
}

// With g_mutex held. Changes what the lookups answer, so what was read is
// let go (the backend's kept answers are, through refresh()).
void rebuild()
{
    auto merged = std::make_shared<Merged>();
    for (const Pack &pack : g_packs)
    {
        if (!pack.enabled)
        {
            continue;
        }
        for (const auto &item : pack.exact)
        {
            merged->exact[item.first] = item.second;
            merged->ignored.erase(item.first);
        }
        for (const auto &item : pack.wildcard)
        {
            merged->wildcard[item.first] = item.second;
        }
        for (const uint64_t key : pack.ignore)
        {
            merged->exact.erase(key);
            merged->ignored.insert(key);
        }
        for (const uint64_t key : pack.ignoreWildcard)
        {
            merged->wildcard.erase(key);
        }
    }
    g_anyPicture.store(!merged->exact.empty() || !merged->wildcard.empty(), std::memory_order_relaxed);
    g_merged = std::move(merged);
    ++g_generation;
    g_queue.clear();
    g_asked.clear();
    g_ready.clear();
}

const Entry *findEntry(const Merged &merged, const dq8::gfx::RegionRequest &request, uint64_t key)
{
    if (merged.ignored.count(key))
    {
        return nullptr;
    }
    if (const auto exact = merged.exact.find(key); exact != merged.exact.end())
    {
        return &exact->second;
    }
    const auto any = merged.wildcard.find(regionKey(request.texels, 0u, request.width, request.height));
    return any != merged.wildcard.end() ? &any->second : nullptr;
}

// A BC file's blocks as they are, when the GPU samples that format, the
// alpha is the GS's already and the size is whole blocks.
bool keepBlocks(const std::vector<uint8_t> &bytes, int blockFormats, Image &image)
{
    dds::Header header;
    if (dds::parse(bytes.data(), bytes.size(), header))
    {
        return false;
    }
    int flag = 0;
    switch (header.kind)
    {
    case dds::Kind::Bc1: flag = dq8::gfx::kRegionBc1; break;
    case dds::Kind::Bc2: flag = dq8::gfx::kRegionBc2; break;
    case dds::Kind::Bc3: flag = dq8::gfx::kRegionBc3; break;
    case dds::Kind::Bc7: flag = dq8::gfx::kRegionBc7; break;
    default: return false;
    }
    if (!(blockFormats & flag) || header.width % 4u || header.height % 4u || header.width > kLargestPicture ||
        header.height > kLargestPicture || dds::blocks(bytes.data(), bytes.size(), header, image.rgba))
    {
        return false;
    }
    image.width = header.width;
    image.height = header.height;
    image.blocks = flag;
    return true;
}

bool decodePicture(const Entry &entry, Image &image, int blockFormats)
{
    std::vector<uint8_t> bytes;
    if (entry.zip)
    {
        if (!entry.zip->read(entry.path, bytes))
        {
            return false;
        }
    }
    else
    {
        std::ifstream file(entry.path, std::ios::binary);
        if (!file)
        {
            return false;
        }
        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    if (hasExtension(entry.path, ".dds"))
    {
        if (entry.rawAlpha && blockFormats && !g_decodeAll && keepBlocks(bytes, blockFormats, image))
        {
            return true;
        }
        if (const char *error = dds::decode(bytes.data(), bytes.size(), image.width, image.height, image.rgba))
        {
            std::fprintf(stderr, "[rg] texture pack: %s: %s\n", entry.path.c_str(), error);
            return false;
        }
    }
    else
    {
        int w = 0, h = 0, channels = 0;
        unsigned char *pixels =
            stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
        if (!pixels)
        {
            return false;
        }
        image.width = static_cast<uint32_t>(w);
        image.height = static_cast<uint32_t>(h);
        image.rgba.assign(pixels, pixels + size_t(w) * h * 4u);
        stbi_image_free(pixels);
    }
    if (image.width > kLargestPicture || image.height > kLargestPicture)
    {
        return false;
    }
    // A picture's alpha is 0 to 255, the GS's opaque is 0x80 (the collected
    // files were written doubled, so a pack made from them comes back right);
    // a pack that says "alpha": "ps2" has the GS's already.
    if (!entry.rawAlpha)
    {
        for (size_t index = 3u; index < image.rgba.size(); index += 4u)
        {
            image.rgba[index] = static_cast<uint8_t>((image.rgba[index] + 1u) / 2u);
        }
    }
    return true;
}

void reader()
{
    for (;;)
    {
        Pending job;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_wake.wait(lock, [] { return !g_queue.empty(); });
            job = std::move(g_queue.front());
            g_queue.pop_front();
        }
        Image image;
        if (!decodePicture(job.entry, image, job.blockFormats))
        {
            std::fprintf(stderr, "[rg] larger 2D art: cannot read %s\n", job.entry.path.c_str());
            image = Image{};
        }
        image.nearest = job.entry.nearest;
        std::lock_guard<std::mutex> lock(g_mutex);
        // Not for a pack that has been switched since.
        if (job.generation == g_generation)
        {
            g_ready[job.key] = std::move(image);
        }
    }
}

// Collecting: each new region is written once, on a thread of its own.
std::string g_dumpDirectory;
struct Dump
{
    std::string name;
    uint32_t width, height;
    std::vector<uint8_t> rgba;
};
std::mutex g_dumpMutex;
std::condition_variable g_dumpWake;
std::deque<Dump> g_dumpQueue;
std::unordered_set<std::string> g_dumped;

void writer()
{
    for (;;)
    {
        Dump dump;
        {
            std::unique_lock<std::mutex> lock(g_dumpMutex);
            g_dumpWake.wait(lock, [] { return !g_dumpQueue.empty(); });
            dump = std::move(g_dumpQueue.front());
            g_dumpQueue.pop_front();
        }
        // Alpha doubled, so the GS's 0x80 is opaque.
        for (size_t index = 3u; index < dump.rgba.size(); index += 4u)
        {
            dump.rgba[index] = static_cast<uint8_t>(std::min(255u, dump.rgba[index] * 2u));
        }
        const std::string path = g_dumpDirectory + "/" + dump.name;
        stbi_write_png(path.c_str(), static_cast<int>(dump.width), static_cast<int>(dump.height), 4,
                       dump.rgba.data(), static_cast<int>(dump.width * 4u));
    }
}

void collect(const dq8::gfx::RegionRequest &request)
{
    std::string name = collectedName(request);
    std::lock_guard<std::mutex> lock(g_dumpMutex);
    if (!g_dumped.insert(name).second)
    {
        return;
    }
    Dump dump{std::move(name), request.width, request.height, {}};
    // The whole region: another draw of it may show the texels this one does not.
    const uint8_t *whole = request.full ? request.full : request.rgba;
    dump.rgba.assign(whole, whole + size_t(request.width) * request.height * 4u);
    g_dumpQueue.push_back(std::move(dump));
    g_dumpWake.notify_one();
}

// Whether the backend should hash the regions drawn with a palette at all.
bool wanted(uint64_t)
{
    return !g_dumpDirectory.empty() || g_anyPicture.load(std::memory_order_relaxed);
}

int lookup(const dq8::gfx::RegionRequest &request, uint32_t &width, uint32_t &height, std::vector<uint8_t> &rgba)
{
    if (request.whole)
    {
        // The texture's own palette first, else any one of its texels' pictures.
        const uint64_t key = regionKey(request.texels, request.clut, request.width, request.height);
        std::lock_guard<std::mutex> lock(g_mutex);
        const Entry *found = findEntry(*g_merged, request, key);
        if (!found)
        {
            return dq8::gfx::kRegionNone;
        }
        if (const auto ready = g_ready.find(key); ready != g_ready.end())
        {
            Image image = std::move(ready->second);
            g_ready.erase(ready);
            g_asked.erase(key);
            if (image.rgba.empty())
            {
                return dq8::gfx::kRegionNone;
            }
            width = image.width;
            height = image.height;
            rgba = std::move(image.rgba);
            return dq8::gfx::kRegionReady | (image.nearest ? dq8::gfx::kRegionNearest : 0) | image.blocks;
        }
        if (g_asked.insert(key).second)
        {
            g_queue.push_back(Pending{key, g_generation, *found, request.blockFormats});
            g_wake.notify_one();
        }
        return dq8::gfx::kRegionPending;
    }
    if (!g_dumpDirectory.empty())
    {
        collect(request);
    }
    if (request.border || request.shadow)
    {
        return dq8::gfx::kRegionNone;
    }
    const uint64_t key = regionKey(request.texels, request.clut, request.width, request.height);
    std::lock_guard<std::mutex> lock(g_mutex);
    const Entry *entry = findEntry(*g_merged, request, key);
    if (!entry)
    {
        return dq8::gfx::kRegionNone;
    }
    if (const auto ready = g_ready.find(key); ready != g_ready.end())
    {
        Image image = std::move(ready->second);
        g_ready.erase(ready);
        // Asked again if the GPU's copy is ever let go.
        g_asked.erase(key);
        if (image.rgba.empty())
        {
            return dq8::gfx::kRegionNone;
        }
        width = image.width;
        height = image.height;
        rgba = std::move(image.rgba);
        return image.nearest ? dq8::gfx::kRegionReady | dq8::gfx::kRegionNearest : dq8::gfx::kRegionReady;
    }
    if (g_asked.insert(key).second)
    {
        g_queue.push_back(Pending{key, g_generation, *entry});
        g_wake.notify_one();
    }
    return dq8::gfx::kRegionPending;
}

} // namespace

void refresh()
{
    if (g_backend)
    {
        g_backend->invalidateRegionReplacements();
    }
}

std::vector<PackInfo> packs()
{
    scan();
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<PackInfo> list;
    for (const Pack &pack : g_packs)
    {
        list.push_back({pack.key, pack.name, pack.regions, pack.enabled});
    }
    return list;
}

void setPackEnabled(const std::string &key, bool on)
{
    scan();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (Pack &pack : g_packs)
        {
            if (pack.key == key && pack.enabled != on)
            {
                pack.enabled = on;
                rebuild();
                break;
            }
        }
    }
    refresh();
}

void install(dq8::gfx::SdlGpuBackend &backend)
{
    std::error_code error;
    if (const char *decode = std::getenv("RG_TEXTURE_PACK_DECODE"); decode && *decode == '1')
    {
        g_decodeAll = true;
    }
    if (const char *dump = std::getenv("RG_TEXTURE_DUMP"); dump && *dump)
    {
        g_dumpDirectory = dump;
        std::filesystem::create_directories(g_dumpDirectory, error);
        // Files already there count as written.
        for (const auto &file : std::filesystem::directory_iterator(g_dumpDirectory, error))
        {
            g_dumped.insert(file.path().filename().string());
        }
        std::thread(writer).detach();
        std::printf("[rg] texture regions collected into %s\n", dump);
    }

    scan();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        rebuild();
        for (const Pack &pack : g_packs)
        {
            std::printf("[rg] texture pack %s (%zu regions)%s\n", pack.name.c_str(), pack.regions,
                        pack.enabled ? "" : ", off");
        }
    }
    std::thread(reader).detach();
    g_backend = &backend;
    backend.setRegionReplacements(&lookup, &wanted);
    backend.setRegionReplacementsEnabled(true);
}

} // namespace rg::texturePack
