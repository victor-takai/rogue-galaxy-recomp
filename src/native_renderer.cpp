#include "native_renderer.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace rg::nativerenderer
{

namespace
{

std::atomic<bool> g_preferred{false};

} // namespace

void setPreferred(bool on)
{
    g_preferred.store(on, std::memory_order_relaxed);
}

bool enabled()
{
    static const bool on = [] {
        const char *value = std::getenv("RG_NATIVE_RENDERER");
        if (value && *value)
            return std::strcmp(value, "0") != 0;
        return g_preferred.load(std::memory_order_relaxed);
    }();
    return on;
}

} // namespace rg::nativerenderer
