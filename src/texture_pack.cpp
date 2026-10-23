#include "texture_pack.h"

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gfx/backends/sdlgpu/sdlgpu_backend.h"

// raylib's stb_image and stb_image_write, which the runtime links in.
extern "C" int stbi_write_png(const char *filename, int w, int h, int comp, const void *data, int stride_in_bytes);
extern "C" unsigned char *stbi_load(const char *filename, int *x, int *y, int *comp, int req_comp);
extern "C" void stbi_image_free(void *retval_from_stbi_load);

namespace rg::texturePack
{
namespace
{

bool writePng(const char *path, uint32_t width, uint32_t height, const uint8_t *rgba)
{
    return stbi_write_png(path, static_cast<int>(width), static_cast<int>(height), 4, rgba,
                          static_cast<int>(width * 4u)) != 0;
}

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

// The pack: each region's larger version, by the collected file's name.
// Read on a thread of its own, so a picture seen for the first time costs
// the GS thread nothing; it draws the region as it is until the picture is
// ready (the item menu reloads one icon slot icon after icon, and reading
// each from disk there held its frames up).
std::unordered_map<uint64_t, std::string> g_pack;
struct Image
{
    uint32_t width = 0u, height = 0u;
    std::vector<uint8_t> rgba;
};
std::mutex g_mutex;
std::condition_variable g_wake;
std::deque<uint64_t> g_queue;
std::unordered_set<uint64_t> g_asked;
std::unordered_map<uint64_t, Image> g_ready;

void reader()
{
    for (;;)
    {
        uint64_t key = 0u;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_wake.wait(lock, [] { return !g_queue.empty(); });
            key = g_queue.front();
            g_queue.pop_front();
        }
        Image image;
        int w = 0, h = 0, channels = 0;
        if (unsigned char *pixels = stbi_load(g_pack.at(key).c_str(), &w, &h, &channels, 4))
        {
            image.width = static_cast<uint32_t>(w);
            image.height = static_cast<uint32_t>(h);
            image.rgba.assign(pixels, pixels + size_t(w) * h * 4u);
            stbi_image_free(pixels);
            // Written with alpha doubled; the GS's opaque is 0x80.
            for (size_t index = 3u; index < image.rgba.size(); index += 4u)
            {
                image.rgba[index] = static_cast<uint8_t>((image.rgba[index] + 1u) / 2u);
            }
        }
        else
        {
            std::fprintf(stderr, "[rg] larger 2D art: cannot read %s\n", g_pack.at(key).c_str());
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_ready[key] = std::move(image);
    }
}

// dq8-gfx's GsTextureCache::kRegion* answers.
constexpr int kNone = 0, kPending = 1, kReady = 2;

int lookup(uint64_t texels, uint64_t clut, uint32_t width, uint32_t height, uint32_t &largerWidth,
           uint32_t &largerHeight, std::vector<uint8_t> &rgba)
{
    const uint64_t key = regionKey(texels, clut, width, height);
    if (!g_pack.count(key))
    {
        return kNone;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (const auto ready = g_ready.find(key); ready != g_ready.end())
    {
        Image image = std::move(ready->second);
        g_ready.erase(ready);
        // Asked again if the GPU's copy is ever let go.
        g_asked.erase(key);
        if (image.rgba.empty())
        {
            return kNone;
        }
        largerWidth = image.width;
        largerHeight = image.height;
        rgba = std::move(image.rgba);
        return kReady;
    }
    if (g_asked.insert(key).second)
    {
        g_queue.push_back(key);
        g_wake.notify_one();
    }
    return kPending;
}

} // namespace

void install(dq8::gfx::SdlGpuBackend &backend)
{
    if (const char *dump = std::getenv("RG_TEXTURE_DUMP"); dump && *dump)
    {
        backend.setRegionDump(dump, &writePng);
        std::printf("[rg] texture regions collected into %s\n", dump);
    }

    const char *pack = std::getenv("RG_TEXTURE_PACK");
    const std::string directory = pack && *pack ? pack : "textures/hd";
    std::error_code error;
    for (const auto &file : std::filesystem::directory_iterator(directory, error))
    {
        unsigned long long texels = 0u, clut = 0u;
        unsigned width = 0u, height = 0u;
        if (std::sscanf(file.path().filename().string().c_str(), "%16llx_%16llx_%ux%u.png", &texels, &clut,
                        &width, &height) == 4)
        {
            g_pack[regionKey(texels, clut, width, height)] = file.path().string();
        }
    }
    if (!g_pack.empty())
    {
        std::thread(reader).detach();
        backend.setRegionReplacements(&lookup);
        std::printf("[rg] larger 2D art: %zu regions in %s\n", g_pack.size(), directory.c_str());
    }
}

} // namespace rg::texturePack
