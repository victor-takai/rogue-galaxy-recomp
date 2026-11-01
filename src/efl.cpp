#include "efl.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ps2_runtime.h"
#include "scene_timing.h"

namespace rg::efl
{
namespace
{

constexpr uint32_t kFileMakeDevicePath = 0x00181980u;
constexpr uint32_t kFileSlotOpen       = 0x00180710u;
constexpr uint32_t kFileSlotClose      = 0x00180540u;

PS2Runtime::RecompiledFunction g_fileMakeDevicePath = nullptr;

// Set a 32-bit guest argument register.
void setGuestArg(R5900Context *ctx, int reg, uint32_t value)
{
    ctx->r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
}

// Invoke a recompiled guest function using an isolated CPU context.
bool invokeGuestFunction(
    uint8_t *rdram,
    R5900Context *originalCtx,
    PS2Runtime *runtime,
    uint32_t address,
    uint32_t arg0,
    uint32_t arg1,
    int32_t &result)
{
    PS2Runtime::RecompiledFunction function =
        runtime->lookupFunction(address);

    if (!function)
    {
        return false;
    }

    // Preserve the game's CPU state.
    R5900Context callCtx = *originalCtx;

    callCtx.pc = address;

    setGuestArg(&callCtx, 4, arg0); // a0
    setGuestArg(&callCtx, 5, arg1); // a1

    // Execute the recompiled guest function.
    function(rdram, &callCtx, runtime);

    // Guest function return value is in v0.
    result = static_cast<int32_t>(getRegU32(&callCtx, 2));

    return true;
}

void fileMakeDevicePath(
    uint8_t *rdram,
    R5900Context *ctx,
    PS2Runtime *runtime)
{
    // Preserve the original guest address before masking it for host access.
    const uint32_t outputGuestAddress = getRegU32(ctx, 5);
    const uint32_t outputAddress = outputGuestAddress & 0x1FFFFFFFu;

    // Let the original game function construct the path.
    g_fileMakeDevicePath(rdram, ctx, runtime);

    if (outputAddress == 0u ||
        outputAddress > PS2_RAM_SIZE - 7u)
    {
        return;
    }

    uint8_t *path = rdram + outputAddress;

    static constexpr char kOriginalPrefix[] = "cddat:";
    static constexpr char kOverridePrefix[] = "cdrom:";

    // Only process paths using the CD/DVD device.
    if (std::memcmp(path, kOriginalPrefix, 6) != 0)
    {
        return;
    }

    // Temporarily redirect the path to the host filesystem.
    std::memcpy(path, kOverridePrefix, 6);

    int32_t slot = -1;

    // Check whether the loose file exists using the game's own file API.
    if (!invokeGuestFunction(
            rdram,
            ctx,
            runtime,
            kFileSlotOpen,
            outputGuestAddress,
            0,
            slot))
    {
        // Could not invoke FileSlotOpen.
        std::memcpy(path, kOriginalPrefix, 6);
        return;
    }

    if (slot >= 0)
    {
        // The host file exists. Close the probe slot.
        int32_t closeResult = -1;

        invokeGuestFunction(
            rdram,
            ctx,
            runtime,
            kFileSlotClose,
            static_cast<uint32_t>(slot),
            0,
            closeResult);

        // Keep the cdrom: path.
        return;
    }

    // The host file does not exist. Restore the original archive path.
    std::memcpy(path, kOriginalPrefix, 6);
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
