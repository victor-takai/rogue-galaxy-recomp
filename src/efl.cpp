#include "efl.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "ps2_runtime.h"
#include "return_hook.h"
#include "scene_timing.h"

namespace rg::efl
{
namespace
{

constexpr uint32_t kFileMakeDevicePath = 0x00181980u;

PS2Runtime::RecompiledFunction g_fileMakeDevicePath = nullptr;

void afterMakePath(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t outputGuestAddress)
{
    const uint32_t outputAddress = outputGuestAddress & 0x1FFFFFFFu;

    if (outputAddress == 0u ||
        outputAddress > PS2_RAM_SIZE - 7u)
    {
        return;
    }

    char *fileDevicePath = reinterpret_cast<char *>(rdram + outputAddress);

    static constexpr char kOriginalPrefix[] = "cddat:";
    static constexpr char kOverridePrefix[] = "cdrom:";

    // Only process paths using the CDDAT device.
    if (std::memcmp(fileDevicePath, kOriginalPrefix, 6) != 0)
    {
        return;
    }

    // Extract the relative path by skipping the 6-character "cddat:" prefix
    std::string relativePath(fileDevicePath + 6);

    // Bail out on any non-ASCII Shift-JIS bytes to prevent std::filesystem crashes
    const bool hasNonAsciiByte = std::any_of(
        relativePath.begin(),
        relativePath.end(),
        [](unsigned char byte) { return byte > 0x7Fu; });

    if (hasNonAsciiByte)
    {
        return;
    }

    // Normalize backslashes to forward slashes for Steam Deck/Linux compatibility
    std::replace(relativePath.begin(), relativePath.end(), '\\', '/');

    // Strip any leading slash so std::filesystem doesn't treat it as an absolute root path
    if (!relativePath.empty() && relativePath.front() == '/')
    {
        relativePath.erase(0, 1);
    }

    // Target the "mods/" folder at the game root
    std::filesystem::path modFilePath = PS2Runtime::getIoPaths().cdRoot / "mods" / relativePath;

    // Check if the loose file exists on the host OS
    if (!std::filesystem::is_regular_file(modFilePath))
    {
        return;
    }

    // The host file exists. Redirect the path to the host filesystem.
    std::memcpy(fileDevicePath, kOverridePrefix, 6);
}

void fileMakeDevicePath(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_fileMakeDevicePath, rdram, ctx, runtime, &afterMakePath, getRegU32(ctx, 5));
}

} // namespace

void install(PS2Runtime &runtime)
{
    const bool complete =
        rg::timing::hookFunction(
            runtime,
            kFileMakeDevicePath,
            &fileMakeDevicePath,
            g_fileMakeDevicePath);

    if (!complete)
    {
        std::fprintf(
            stderr,
            "[rg] file override: FileMakeDevicePath hook failed\n");
    }
}

} // namespace rg::efl
