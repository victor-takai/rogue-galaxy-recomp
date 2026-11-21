#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

class GSRasterBackend;
class PS2Runtime;

// Presents the frames the game flips, one each, in order.
//
// The runtime's presenter samples the display at host vblank ticks and waits
// for the GS worker to reach each sample. With the worker busy, those samples
// land at uneven times against the game's 30 Hz flips, so some frames show
// twice and others never, and characters visibly skip frames. Here the
// presentation is instead prepared on the game's thread right after each
// GfxFlipBuffers (0x00143650), at that point in the GS command stream, and
// the presenter shows those. Without flips for a while (movies, loading
// screens) it presents as before.
//
// RG_PRESENT=vblank turns it off.
namespace rg::graphics
{

// Wraps the backend the GS frontend talks to (the worker queue, when there
// is one). Returns it unchanged when disabled.
std::unique_ptr<GSRasterBackend> paceOnFlips(std::unique_ptr<GSRasterBackend> backend);

// After loadELF(): hooks the game's flip function.
void installFlipHook(PS2Runtime &runtime);

// Flips the game has done so far, the frames it finished drawing: the menu's
// frame-rate counter. From any thread.
uint64_t guestFlips();

// When the game's last flip returned (any thread): the native pipeline's
// note for DQ8_GFX_PRESENT_LOG.
std::chrono::steady_clock::time_point lastFlipTime();

// The presenter thread, on each pass of its loop (graphics.cpp's host present
// hook, so only with the game's own window, not RG_GS_NO_WINDOW). A flip
// returns from its vsync wait at the same tick the presenter sees, and often
// lands a moment after the presenter has looked: it then waited for the next
// tick, so at 30 fps frames reached the screen 1 and 3 ticks apart in turn
// instead of 2, and at 60 fps the next flip replaced it there, unseen.
// Between ticks this shows such a flip at once, through the GS frontend's
// latch, as the native pipeline does (RG_NATIVE_PRESENT_TICK). True when it
// showed one. RG_PRESENT_TICK=1 keeps the flips to the ticks.
bool showLateFlip(bool newTick);

} // namespace rg::graphics
