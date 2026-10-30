// Rogue Galaxy Recompiled: boots the recompiled SCUS_974.90 on the PS2Recomp
// runtime.
//
//   roguegalaxy [path/to/SCUS_974.90]
//
// The ELF must sit in an extracted disc tree (python setup.py extract): the
// runtime serves cdrom0: from the ELF's folder. Without an argument the ELF is
// looked up as disc/SCUS_974.90 under the current directory.

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>

#include "debug/call_trace.h"
#include "debug/crash_report.h"
#include "debug/hw_watch.h"
#include "debug/input_script.h"
#include "debug/pos_trace.h"
#include "debug/encounter.h"
#include "debug/jump_trace.h"
#include "debug/sampler.h"
#include "debug/selftest.h"
#include "debug/stall_watch.h"
#include "overlay/overlay_manager.h"
#include "flip_pacing.h"
#include "graphics.h"
#include "movie_audio.h"
#include "libc_rand.h"
#include "loading_screen.h"
#include "battle_rates.h"
#include "button_glyphs.h"
#include "camera_view.h"
#include "dev_tools.h"
#include "draw_distance.h"
#include "free_camera.h"
#include "instant_text.h"
#include "sharp_letters.h"
#include "map_chests.h"
#include "hud_size.h"
#include "inventory.h"
#include "save_anywhere.h"
#include "opening.h"
#include "widescreen.h"
#include "quick_scenes.h"
#include "scene_timing.h"
#include "settings_pages.h"
#include "shop_prices.h"
#include "step_fixes.h"
#include "vif1_thread.h"
#include "vu0_flags.h"
#include "ps2_runtime.h"
#include "runtime/ps2_native_iop.h"

namespace
{

// Guest RAM layout. The entry code calls SetupThread with the main stack at
// 0x01FD0000..0x02000000 and SetupHeap(0x00528200, -1); on hardware the heap
// then runs up to that stack, and the game sizes its main memory stack (one
// 24.7 MB memalign, mgCMemory at 0x4A6FA0) to nearly all of it. The runtime's
// default heap end, 0x01F00000, is too low: that memalign fails, the game
// carries on with a null base and its first clear wipes RAM from address 0.
//
// The runtime also carves 16 KB stacks for guest callbacks down from the top
// of RAM, which is the main stack, and keeps a small heap of its own for HLE
// buffers. So:
//   0x00080000..0x00100000  runtime heap (EE kernel space, unused here)
//   0x00528200..0x01F80000  game heap (EndOfHeap returns the end)
//   0x01F80000..0x01FD0000  runtime callback stacks (20 of them)
//   0x01FD0000..0x02000000  main thread stack
// The heap end cannot go lower: at 0x01F50000 the game's big memalign
// failed again and the boot died with every file path empty (2026-10-19).
// The pool once reached 0x01F74000 (23 stacks, a return to the title); the
// top of the heap is unused by the game in practice, and it only warns.
constexpr uint32_t kMainStackBottom = 0x01FD0000u;
constexpr uint32_t kGuestHeapEnd = 0x01F80000u;
constexpr uint32_t kRuntimeHeapBase = 0x00080000u;
constexpr uint32_t kRuntimeHeapEnd = 0x00100000u;

bool configureMemory(PS2Runtime &runtime)
{
    // Moves the callback stacks below the main stack: they are bumped down
    // from the top, so taking the top first leaves them under it.
    if (runtime.reserveAsyncCallbackStack(PS2_RAM_SIZE - kMainStackBottom) == 0u)
    {
        std::fprintf(stderr, "[rg] could not reserve the main stack area\n");
        return false;
    }
    runtime.setGuestHeapCeiling(kGuestHeapEnd);
    runtime.configureGuestHeap(kRuntimeHeapBase, kRuntimeHeapEnd);
    return true;
}

std::filesystem::path elfPath(int argc, char **argv)
{
    if (argc >= 2 && argv[1][0] != '\0')
    {
        return argv[1];
    }
    return std::filesystem::current_path() / "disc" / "SCUS_974.90";
}

int run(int argc, char **argv)
{
    const std::filesystem::path elf = elfPath(argc, argv);
    if (!std::filesystem::is_regular_file(elf))
    {
        std::fprintf(stderr,
                     "[rg] %s not found. Pass the ELF of your extracted disc "
                     "(python setup.py extract), e.g. roguegalaxy disc/SCUS_974.90\n",
                     elf.string().c_str());
        return 1;
    }

    PS2Runtime runtime;
    rg::graphics::Setup graphics(runtime);
    if (!runtime.initialize("Rogue Galaxy Recompiled " RG_APP_VERSION))
    {
        std::fprintf(stderr, "[rg] PS2Runtime::initialize() failed\n");
        return 1;
    }
    // The disc's sound drivers (SCE libsd/sdrdrv and the Sound Kit, the same
    // set DQ8-Recompiled runs) load onto the runtime's emulated IOP with an
    // SPU2, so the game's libsdr and sound RPCs reach their real code. Audio
    // starts when the game's sceSifLoadModule loads the first of them.
    // RG_MUTE=1 silences the output; the IOP still runs, the game waits on it.
    ps2_native_iop::setModules(runtime, {"LIBSD.IRX", "SDRDRV.IRX", "MODHSYN.IRX", "MODMIDI.IRX",
                                         "MODMSIN.IRX", "SKSOUND.IRX", "SKHSYNTH.IRX", "SKMIDI.IRX",
                                         "SKMSIN.IRX", "PCMPLAY.IRX"});
    if (const char *mute = std::getenv("RG_MUTE"); mute && *mute && *mute != '0')
    {
        ps2_native_iop::setVolume(0.0f);
    }
    graphics.install(runtime);
    if (!runtime.loadELF(elf.string()))
    {
        std::fprintf(stderr, "[rg] PS2Runtime::loadELF(%s) failed\n", elf.string().c_str());
        return 1;
    }
    // RG_MC0_DIR=path puts the memory card in another folder (mc1 is its
    // sibling, as with disc/mc0). The benchmarks run on a copy holding only
    // the save they load: a second save of the player's changes what the
    // scripted load menu picks, and with it every reference frame.
    if (const char *mc0 = std::getenv("RG_MC0_DIR"); mc0 && *mc0)
    {
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        paths.mcRoot = std::filesystem::absolute(mc0).lexically_normal();
        PS2Runtime::setIoPaths(paths);
    }
    // After loadELF(): it sets the defaults these override, and fills the
    // function table the hooks replace entries of.
    if (!configureMemory(runtime) || !rg::overlay::install(runtime))
    {
        return 1;
    }
    rg::graphics::installFlipHook(runtime);
    rg::vif1::install(runtime);
    rg::timing::install(runtime);
    rg::steps::install(runtime);
    rg::libc::install(runtime);
    rg::vu0::install(runtime);
    rg::movie::install(runtime);
    rg::loading::install(runtime);
    rg::widescreen::install(runtime);
    rg::hudsize::install(runtime);
    rg::saveanywhere::install(runtime);
    rg::inventory::install(runtime);
    rg::buttons::install(runtime);
    rg::opening::install(runtime);
    rg::devtools::install(runtime);
    rg::cameraview::install(runtime);
    rg::drawdistance::install(runtime);
    rg::battlerates::install(runtime);
    rg::shopprices::install(runtime);
    rg::instanttext::install(runtime);
    rg::letters::install(runtime);
    rg::mapchests::install(runtime);
    rg::freecamera::install(runtime);
    rg::settingspages::install(runtime);
    rg::quickscenes::install(runtime);
    rg::debug::installCallTrace(runtime);
    rg::debug::installHardwareWatch(runtime);
    rg::debug::installPosTrace(runtime);
    rg::debug::installJumpTrace(runtime);
    rg::debug::installForcedEncounter(runtime);
    rg::debug::startInputScript();
    rg::debug::startSampler();
    rg::debug::startStallWatch(runtime);
    std::printf("[rg] booting %s\n", elf.string().c_str());
    std::fflush(stdout);

    runtime.run();
    rg::debug::stopSampler();
    rg::debug::stopStallWatch();
    rg::vif1::shutdown();

    std::printf("[rg] runtime stopped after %u overlay load(s), resident: %s\n",
                rg::overlay::loadCount(), rg::overlay::residentName());
    if (runtime.asyncCallbackStackTop() < kGuestHeapEnd)
    {
        std::fprintf(stderr, "[rg] runtime callback stacks grew into the game heap (down to 0x%08X)\n",
                     runtime.asyncCallbackStackTop());
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    int status = 1;
    // Redirected to a file (the launchers), stdout is fully buffered, and
    // a crash or a killed process loses its last lines.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    rg::debug::installCrashReport();
    try
    {
        if (!rg::debug::runSelfTestIfRequested(status))
        {
            status = run(argc, argv);
        }
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "[rg] fatal: %s\n", e.what());
    }
    std::fflush(stdout);
    std::fflush(stderr);
    // Guest threads may still be parked inside the runtime; skip their
    // destructors the way the framework's own runner does.
    std::_Exit(status);
}
