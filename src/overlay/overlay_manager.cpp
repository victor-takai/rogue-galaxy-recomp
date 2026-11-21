#include "overlay/overlay_manager.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>

#include "overlay/overlay_image.h"
#include "ps2_runtime.h"

namespace rg::overlay
{
namespace
{

constexpr uint32_t kMagic = 0x336F574Du; // "MWo3"

// MWo3 ids 1 (VIEWER) and 4 (LAUNCHER) are 0xC0 bytes of zeros: loading one
// leaves no code in the arena.
constexpr uint32_t kEmptyIds[] = {1u, 4u};

const Image &(*const kImages[])() = {
    &overlayImage_title, &overlayImage_insmenu, &overlayImage_normal,
    &overlayImage_factory, &overlayImage_insect,
};

std::atomic<PS2Runtime::FunctionRegion *> g_region{nullptr};
std::atomic<const Image *> g_resident{nullptr};
std::atomic<uint32_t> g_loads{0u};

struct RamHeader
{
    uint32_t magic;
    Header fields;
    char name[32];
};
static_assert(sizeof(RamHeader) == kHeaderSize, "MWo3 header is 0x40 bytes");

PS2Runtime::FunctionRegion *resolve(uint32_t address, void *)
{
    if (address < kArenaStart || address >= kArenaEnd)
    {
        return nullptr;
    }
    return g_region.load(std::memory_order_acquire);
}

bool sameHeader(const Header &a, const Header &b)
{
    return a.id == b.id && a.loadAddress == b.loadAddress && a.textSize == b.textSize &&
           a.dataSize == b.dataSize && a.bssSize == b.bssSize && a.ctorBegin == b.ctorBegin &&
           a.ctorEnd == b.ctorEnd;
}

void deactivate()
{
    g_region.store(nullptr, std::memory_order_release);
    g_resident.store(nullptr, std::memory_order_release);
}

// Replaces the `jr ra` stub at kImageLoadedHook. Runs after the read and
// before the guest clears .bss and runs the constructors, which then
// dispatch into the overlay just switched to.
void onImageLoaded(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    ctx->pc = getRegU32(ctx, 31);
    const uint32_t loadAddress = getRegU32(ctx, 4);
    const uint32_t load = g_loads.fetch_add(1u, std::memory_order_acq_rel) + 1u;

    RamHeader header{};
    if (loadAddress != kArenaStart)
    {
        std::fprintf(stderr, "[overlay] load #%u: header at 0x%08X, not the arena 0x%08X; ignored\n",
                     load, loadAddress, kArenaStart);
        deactivate();
        return;
    }
    std::memcpy(&header, rdram + (loadAddress & PS2_RAM_MASK), sizeof(header));
    if (header.magic != kMagic || header.fields.loadAddress != kArenaStart)
    {
        std::fprintf(stderr, "[overlay] load #%u: no MWo3 image at 0x%08X (magic 0x%08X)\n",
                     load, loadAddress, header.magic);
        deactivate();
        return;
    }

    char name[sizeof(header.name) + 1] = {};
    std::memcpy(name, header.name, sizeof(header.name));

    for (uint32_t id : kEmptyIds)
    {
        if (header.fields.id == id)
        {
            std::printf("[overlay] load #%u: %s (id %u), no code\n", load, name, id);
            deactivate();
            return;
        }
    }

    for (const auto image : kImages)
    {
        const Image &candidate = image();
        if (candidate.header.id != header.fields.id)
        {
            continue;
        }
        if (!sameHeader(candidate.header, header.fields))
        {
            // Another release or a modified file: this tree's code is not what
            // was loaded, so running it would execute the wrong instructions.
            std::fprintf(stderr,
                         "[overlay] load #%u: %s (id %u) does not match the image %s was generated "
                         "from (text 0x%X/0x%X, data 0x%X/0x%X); arena calls will fail\n",
                         load, name, header.fields.id, candidate.name, header.fields.textSize,
                         candidate.header.textSize, header.fields.dataSize, candidate.header.dataSize);
            deactivate();
            return;
        }
        g_region.store(candidate.region, std::memory_order_release);
        g_resident.store(&candidate, std::memory_order_release);
        std::printf("[overlay] load #%u: %s (id %u) resident, dispatch 0x%08X..0x%08X\n", load,
                    candidate.name, header.fields.id, candidate.region->base, candidate.region->end);
        std::fflush(stdout);
        return;
    }

    std::fprintf(stderr, "[overlay] load #%u: unknown MWo3 id %u (%s)\n", load, header.fields.id, name);
    deactivate();
}

} // namespace

bool install(PS2Runtime &runtime)
{
    for (const auto image : kImages)
    {
        const Image &i = image();
        if (i.region->base < kArenaStart + kHeaderSize || i.region->end > kArenaEnd || i.region->slotCount == 0u)
        {
            std::fprintf(stderr, "[overlay] %s: table 0x%08X..0x%08X is outside the arena\n", i.name,
                         i.region->base, i.region->end);
            return false;
        }
    }

    PS2Runtime::setFunctionRegionResolver(&resolve, nullptr);
    if (!runtime.replaceFunction(kImageLoadedHook, &onImageLoaded))
    {
        std::fprintf(stderr, "[overlay] cannot hook 0x%08X; overlays would never become resident\n",
                     kImageLoadedHook);
        PS2Runtime::setFunctionRegionResolver(nullptr, nullptr);
        return false;
    }
    return true;
}

const char *residentName()
{
    const Image *image = g_resident.load(std::memory_order_acquire);
    return image ? image->name : "none";
}

uint32_t loadCount()
{
    return g_loads.load(std::memory_order_acquire);
}

static_assert(std::is_same_v<Function, PS2Runtime::RecompiledFunction>,
              "rg::overlay::Function must stay PS2Runtime::RecompiledFunction");

namespace
{
// What hookFunction found at a slot before putting a hook there, for the
// resume points' check below.
struct Replaced
{
    const PS2Runtime::FunctionRegion *region;
    uint32_t slot;
    Function original;
};
std::vector<Replaced> g_replaced;

// The function a slot stood for before any hook went in.
Function generatedAt(const PS2Runtime::FunctionRegion *region, uint32_t slot)
{
    for (const Replaced &r : g_replaced)
    {
        if (r.region == region && r.slot == slot)
        {
            return r.original;
        }
    }
    return region->slots[slot];
}
} // namespace

bool hookFunction(const char *name, uint32_t address, Function replacement, Function &original)
{
    original = nullptr;
    for (const auto image : kImages)
    {
        const Image &candidate = image();
        if (std::strcmp(candidate.name, name) != 0)
        {
            continue;
        }
        PS2Runtime::FunctionRegion *region = candidate.region;
        const uint32_t slot = (address - region->base) >> 2;
        if (address < region->base || address >= region->end || (address & 3u) != 0u ||
            slot >= region->slotCount || region->slots[slot] == nullptr)
        {
            std::fprintf(stderr, "[overlay] %s has no function at 0x%08X to hook\n", name, address);
            return false;
        }
        // A function's resume points (the places a call unwound at a safe
        // point resumes) have its entry too: the same function at the
        // nearest entry below, with only empty slots between, means this is
        // not where it starts (compared as generated, a hook at its start
        // notwithstanding).
        const Function self = generatedAt(region, slot);
        for (uint32_t before = slot; before-- > 0u;)
        {
            if (region->slots[before] == nullptr)
            {
                continue;
            }
            if (generatedAt(region, before) == self)
            {
                std::fprintf(stderr, "[overlay] 0x%08X is inside a function of %s, not its start\n", address,
                             name);
                return false;
            }
            break;
        }
        g_replaced.push_back({region, slot, region->slots[slot]});
        original = region->slots[slot];
        region->slots[slot] = replacement;
        return true;
    }
    std::fprintf(stderr, "[overlay] no overlay called %s to hook 0x%08X in\n", name, address);
    return false;
}

} // namespace rg::overlay
