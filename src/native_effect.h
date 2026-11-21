#pragma once

#include <cstdint>

// A post effect as a chain of sprites (docs/NATIVE-PIPELINE.md, step 5): what
// src/native_effects.h makes of the packets a game function wrote for it, and
// what rg::nativeproducer::effect turns into the renderer's mesh draws. Each
// op is one sprite as the GS would draw it, its strips joined; `external`
// buffers are the GS's own (the frame, read and drawn into), the rest belong
// to the effect: written before they are read, and read by nothing after it.
// Trivially copyable.
struct GSNativeEffect
{
    struct Buffer
    {
        uint32_t base = 0u; // 256-byte block address (FBP << 5)
        uint32_t fbw = 0u;  // in units of 64 pixels
        uint32_t psm = 0u;
        uint32_t width = 0u; // GS pixels the effect uses
        uint32_t height = 0u;
        uint32_t external = 0u;
        // Read only, by a kObjectTail: the object's layer stands for this GS
        // buffer, which the object routed there never drew into.
        uint32_t layer = 0u;
    };
    struct Op
    {
        uint32_t target = 0u; // index into buffers
        int32_t source = -1;  // likewise, or -1 for an untextured sprite
        float rect[4] = {};   // x0, y0, x1, y1: window coordinates less XYOFFSET
        float uv[4] = {};     // u0, v0, u1, v1: texels at those corners
        uint32_t scissor[4] = {}; // x0, y0, x1, y1, inclusive
        uint8_t rgba[4] = {};
        uint64_t alpha = 0u; // ALPHA, when kBlend
        uint64_t test = 0u;  // TEST (its alpha test)
        uint32_t flags = 0u;
        uint32_t texa = 0u;  // TEXA's TA0 (bits 0-7) and AEM (bit 15), for kCt24
        // When TEST tests depth (ZTE, ZTST GEQUAL or GREATER): the depth
        // buffer (ZBUF, not written) and the Z at x0 and at x1, linear
        // between (a sprite's one Z; the depth of field's quads ramp), into
        // a GS target.
        uint64_t zbuf = 0u;
        uint32_t z[2] = {};
    };
    static constexpr uint32_t kBlend = 1u;     // ABE
    static constexpr uint32_t kLinear = 2u;    // bilinear (TEX1)
    static constexpr uint32_t kDecal = 4u;     // TFX DECAL rather than MODULATE
    static constexpr uint32_t kColClamp = 8u;  // COLCLAMP
    static constexpr uint32_t kTextureAlpha = 16u; // TCC
    static constexpr uint32_t kCt24 = 32u;         // the texture read as CT24: its alpha from TEXA
    static constexpr uint32_t kHighlight = 64u;    // TFX HIGHLIGHT
    static constexpr uint32_t kHighlight2 = 128u;  // TFX HIGHLIGHT2
    // The target's alpha bits 2-7 from its green ones, the rest kept: the
    // GS's CT16 view of a CT32 buffer copying each RG half onto its BA half
    // with R and G masked (the source is the target).
    static constexpr uint32_t kGreenToAlpha = 1024u;
    static constexpr uint32_t kKeepAlpha = 2048u;  // FBMSK 0xFF000000: the target's alpha not written
    // kCt24's TEXA as the draw registers hold it when the effect is drawn
    // (the producer reads src/draw_state_mirror.h): the effect's own packets
    // did not set it.
    static constexpr uint32_t kTexaFromGs = 4096u;
    // FBMSK 0x00FFFFFF: the alpha alone written, the colour kept (Rogue
    // Galaxy's CBlurDraw writes 02a40's alpha from 03f80 so).
    static constexpr uint32_t kKeepColor = 32768u;
    // Buffer::external for a GS buffer drawn as the effect's own (sprites
    // into it the pipeline cannot blend): from what it holds, and what it
    // ends with put back.
    static constexpr uint32_t kTakenOver = 2u;
    static constexpr uint32_t kMaxBuffers = 12u;
    static constexpr uint32_t kMaxOps = 96u;
    // What else it is, instead of sprites: Rogue Galaxy's per-object
    // composites, each object drawn apart from the others into an object
    // layer of the renderer's own and laid over the frame later from it.
    // kRouteLayer: the draws into buffers[0] go to a layer, for the object
    // whose bounds are `box` (x0, y0, x1, y1, in its pixels); kRouteEnd: to
    // the GS buffer again. kObjectTail: the sprites that lay that object
    // over the frame, their source (`layer` set) its layer. kComposite: the
    // end of a run of objects.
    static constexpr uint32_t kSprites = 0u;
    static constexpr uint32_t kRouteLayer = 1u;
    static constexpr uint32_t kRouteEnd = 2u;
    static constexpr uint32_t kObjectTail = 3u;
    static constexpr uint32_t kComposite = 4u;
    uint32_t command = kSprites;
    int32_t box[4] = {};
    uint32_t bufferCount = 0u;
    uint32_t opCount = 0u;
    Buffer buffers[kMaxBuffers];
    Op ops[kMaxOps];
};
