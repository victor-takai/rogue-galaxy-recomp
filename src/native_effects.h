#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "native_effect.h"

class PS2Runtime;

// The game's post effects as the native pipeline's own draws
// (docs/NATIVE-PIPELINE.md, step 5), instead of the GS sprites the game
// builds for them.
//
// The game's function still runs, so all it does besides drawing stays as
// it was; a hook notes which of the frame's packet memory it wrote. The VIF1
// thread holds the packets it reads from there, turns their sprites into a
// GSNativeEffect (each sprite as the GS would draw it, its strips joined)
// for rg::nativeproducer::effect, and sends the packets on with their kicks
// withheld (XYZ2 as XYZ3: the GS registers end as they would have). Packets
// that are not the shape expected, or an effect the producer cannot make,
// go on as they are.
//
// Only with the native pipeline on (RG_NATIVE_PIPELINE=1). Each effect is on
// unless its variable is 0: RG_NATIVE_GLARE (GlareFilterDrawMaybe: the
// frame's bright parts down to 192x192, 20 shifted adds of that buffer onto
// itself, back onto the frame), RG_NATIVE_SHADOW (DrawListEndPlaneShadowPass:
// the shadow volumes' mask, blurred, onto the frame), RG_NATIVE_CBLUR
// (CBlurTexBlurMaybe and CBlurDrawMaybe), RG_NATIVE_BLOOM
// (BloomExtractBlurMaybe, as the glare), RG_NATIVE_DOF (DofFilterDrawMaybe),
// RG_NATIVE_REFRACTION (RefractionCaptureFrame), RG_NATIVE_FLIP (the flip's
// copy), RG_NATIVE_COLOR_FILTER (ScreenColorFilterMaybe), RG_NATIVE_FOCUS
// (FocusFilterDrawMaybe), RG_NATIVE_SHADOW_MAP (ShadowMapBlur),
// RG_NATIVE_COMPOSITE (the per-object composites, as object layers).
// RG_NATIVE_EFFECTS_LOG=1 prints the first of each effect built;
// RG_NATIVE_EFFECTS_TRACE=1 traces the sprites of a few and dumps refused
// effects' packets.
namespace rg::nativefx
{

enum class Effect : uint8_t
{
    None,
    Glare,
    Shadow,
    // CBlurTexBlurMaybe: the 64x64 buffer at 03f80 decayed, added to and
    // its alpha taken from its green, a frame at a time.
    CBlur,
    // BloomExtractBlurMaybe: the frame's bright parts blurred in 02a00 and
    // 02c40 and added back, as the glare.
    Bloom,
    // DofFilterDrawMaybe: the frame to 02a00 and laid back over itself.
    Dof,
    // CBlurDrawMaybe: the displayed frame copied to 02a40, its alpha from
    // 03f80, laid over the frame a pixel or two off.
    CBlurDraw,
    // RefractionCaptureFrame (CharaDrawTranslucentPass's): the frame halved
    // into 03b80, which the refraction material samples.
    Refraction,
    // DrawListBeginPlaneShadowPass: its packets go on as they are; the
    // registers they set are where the shadow's sprites start from.
    ShadowSetup,
    // ModelDrawOffscreenComposite (stage E), an object at a time: the head
    // draws it into the offscreen buffer (its clear, its model), the tail
    // lays that over the frame. The VIF1 thread draws each object into an
    // object layer of the renderer's own and its tail from there
    // (src/vif1_thread.cpp batches them). Place::unit: the object.
    CompositeHead,
    CompositeTail,
    // GfxFlipBuffersMaybe's copy of the frame into the display buffer: sent
    // from the game thread, not the VIF1 lists, so taken by flipPacket
    // rather than by packet memory.
    Flip,
    // ScreenColorFilterMaybe: the scene's tint, built from its arguments
    // (Place::unit: the colour), its packets sent on with their kicks
    // withheld.
    ColorFilter,
    // FocusFilterDrawMaybe and ShadowMapBlur: built from their sprites, as
    // the bloom.
    Focus,
    ShadowMapBlur,
};

// What an EE address of the frame's packet memory holds: an effect's
// packets, and for the colour filter its colour, for a composite its
// object's number.
struct Place
{
    Effect effect = Effect::None;
    uint32_t unit = 0u;
};

// Whether any effect is on (the native pipeline is, and not every effect's
// variable is 0).
bool enabled();
const char *name(Effect effect);
// RG_NATIVE_COMPOSITE (on unless 0): stage E's objects as object layers.
bool compositesBatched();

// After loadELF(): hooks the effects' functions.
void install(PS2Runtime &runtime);

// GfxFlipBuffersMaybe (RGDecomp's C) sends, from the game thread: a draw
// environment whose FRAME is the display buffer (CT24), then a packet it
// built once and sends every frame: ALPHA 0x8000000064, TEST 0x20002, PRMODE
// 0x110 and DECAL sprites of the frame, 16 pixels wide (nearest; one
// bilinear sprite when the display's height is not the frame's); then the
// environment back. The flip's hook (src/flip_pacing.cpp) marks it; the
// game thread's packets go through flipPacket, which turns the sprites into
// a Flip effect: one copy instead of 32 draws.
bool flipNative();
void beginFlip();
void endFlip();
// The game thread's own GIF packet: true when it was the flip's copy, with
// the effect and what to send in its place (the sprites' packet, its kicks
// withheld: the GS registers end as they would have).
bool flipPacket(const uint8_t *data, uint32_t size, GSNativeEffect &out,
                std::vector<std::vector<uint8_t>> &withheld);

// The effect whose packets the EE address holds (any of RDRAM's windows).
Effect effectAt(uint32_t address);
Place placeAt(uint32_t address);

// A ShadowSetup packet: the registers it writes, for the next shadow.
void observe(Effect effect, const uint8_t *data, uint32_t size);

// A composite's head: the box its clear sprites cover in the offscreen
// buffer (CT32 at `base`, `fbw`), in pixels, x1 and y1 exclusive.
struct Box
{
    uint32_t base = 0u;
    uint32_t fbw = 0u;
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
bool clearBox(const std::vector<std::vector<uint8_t>> &packets, Box &box);
// When a packet starts a transfer into GS memory: the pages it may write.
bool uploadPages(const uint8_t *data, uint32_t size, uint32_t &first, uint32_t &last);

// Debugging: the registers and primitives of `packets`, one line each.
void dumpPackets(const char *what, const std::vector<std::vector<uint8_t>> &packets);

// From the packets an effect's function wrote, in order: the effect, and
// the packets with their kicks withheld. False, with `why`, when they are
// not the sprites expected. `unit`: the colour filter's colour.
bool build(Effect effect, const std::vector<std::vector<uint8_t>> &packets, GSNativeEffect &out,
           std::vector<std::vector<uint8_t>> &withheld, std::string &why, uint32_t unit = 0u);

} // namespace rg::nativefx
