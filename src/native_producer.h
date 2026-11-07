#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct GSNativeMesh;
struct GSDrawState;

// The game side of the native pipeline (docs/NATIVE-PIPELINE.md): what turns
// the game's GS-shaped data into the renderer's records. The renderer sees
// ids only; the tables from the game's GS addresses to those ids are kept
// here, for the producers of draws (step 4). A producer may know the GS
// (this file does: the layouts the game uploads in); the renderer may not.
//
// Every function is a no-op unless the native pipeline is on
// (RG_NATIVE_PIPELINE=1 under the native renderer flag).
namespace rg::nativeproducer
{
// Whether the native pipeline is on (for the game's other switches that
// only make sense with the GS).
bool pipelineOn();

// One mip level of an image node, as TexBuildUploadPkt sends it (game
// thread). `ram` is the level's bytes in EE memory, `ramBytes` how many
// are readable there.
struct TextureLevel
{
    const uint8_t *ram = nullptr;
    uint32_t ramBytes = 0;
    uint32_t tbp = 0;        // where the game puts it (a GS block), for the table
    uint32_t psm = 0;        // the texture's format (CT32, CT16, T8, T4)
    uint32_t width = 0;      // the texture's size in texels
    uint32_t height = 0;
    uint32_t bufferWidth = 0; // the texture's buffer width, in 64-pixel units
    // The transfer as the GS gets it: `transferWidth` by `transferHeight`
    // texels of `transferPsm` at `transferBufferWidth`. 8-bit art can go as
    // CT32 at half the width and height (`preSwizzled`), laid out so the
    // GS's memory reads it as T8.
    bool preSwizzled = false;
    uint32_t transferWidth = 0;
    uint32_t transferHeight = 0;
    uint32_t transferBufferWidth = 0;
    uint32_t transferPsm = 0;
    uint32_t cbp = 0; // the node's palette (TEX0's CBP), for an indexed texture
};
void texture(const TextureLevel &level);

// A palette as the node uploads it (game thread): 256 entries (a 16x16
// image, CSM1's order) or 16 (8x2), CT32 or CT16, to the GS block `cbp`.
void palette(const uint8_t *ram, uint32_t ramBytes, uint32_t cbp, uint32_t cpsm, uint32_t entries);

// A render target the game names (gs_named_targets' kinds: 0 auto, 1 the
// frame, 2 the display buffer). Any thread.
void target(uint32_t base, uint32_t bufferWidth, uint32_t width, uint32_t height, uint32_t psm, int kind);

// The tables, for draw producers (any thread): the id of the latest texture
// at this block in this format, the palette at this block, the target at
// this base. 0 when none.
uint32_t textureAt(uint32_t tbp, uint32_t psm);
uint32_t paletteAt(uint32_t cbp);
uint32_t targetAt(uint32_t base);

// Step 4a: a VU1 program's mesh as the renderer's mesh draw (the VIF1
// thread, as the mesh goes to the GS). `vu1Data` is the VU1 data memory the
// mesh was built from: the camera (quads 4-11, 24-27) and the model (60-63)
// are read from it. `state` is the draw state the mesh is drawn under
// (src/draw_state_mirror.h); null when it has none (the mesh is counted, not
// made). `source` names what drew it, for profiles. True when the record
// was made: the mesh is then not sent to the GS (step 4c).
bool mesh(const GSNativeMesh &mesh, const GSDrawState *state, const uint8_t *vu1Data, uint32_t source);

// Step 3b's counts (the VIF1 thread, which withholds named uploads the
// producer sent from the GS): one withheld (palette or texture), one sent on
// and why, an A+D block whose other registers went on without the
// transfer's, and image data that continued in a later packet.
// The VIF1 thread, for each upload on its way to the GS: whether this
// producer made a record of it (in the order the game built them; uploads
// not seen within a short lookahead were never sent and are dropped). Taken
// when it did: the upload is withheld, and `palette` says which it was.
// `hash` and `bytes`: the record's imageHash of the guest memory it was
// made from (what the upload sends, when the game has not changed it since).
bool takeProduced(uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t width, uint32_t height, bool &palette,
                  uint64_t &hash, uint32_t &bytes);

enum class Passed
{
    NotProduced, // no record of it here: not a named upload, or refused
};
// Step 6: a host upload nobody took (the absorber's, docs/NATIVE-PIPELINE.md
// "host uploads nobody took"), its pixels as sent (rows in order; 4-bit
// formats low nibble first). Kept as a linear image per base and format,
// which draws reading that format at that buffer width sample (sourceOf).
void hostUpload(uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                const uint8_t *data, uint32_t bytes);
// Step 6: a local copy (TRXDIR 2) the absorber took, logged with what the
// producer holds at its source and destination (the first few).
void noteLocalCopy(uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t sx, uint32_t sy, uint32_t dbp, uint32_t dbw,
                   uint32_t dpsm, uint32_t dx, uint32_t dy, uint32_t width, uint32_t height);
// A withheld upload whose data as sent differs from the guest memory its
// record was made from (the game wrote it after the texture hook): the
// record made again from `data` and resident in its place (VIF1 thread, in
// the stream's order). Palettes of 16 or 256 entries; textures uploaded as
// the format they are read in.
void replaceUpload(bool palette, uint32_t dbp, uint32_t dpsm, uint32_t width, uint32_t height, const uint8_t *data,
                   uint32_t bytes);
void noteWithheld(bool palette, uint32_t imageBytes);
void notePassed(Passed why);
void noteSplit();
void noteContinuedInPacket();

// Step 7b: a depth read the game makes (sceGsExecStoreImage of a Z buffer:
// the lens flare's 8x8 block at the sun), from the renderer's depth a frame
// or two late, in the read's format (Z32, Z24, Z16: little-endian, rows in
// order). `base` is the read's SBP, in blocks. Zeros (the clear's) where the
// renderer has no answer yet. False when the pipeline is off or the read is
// not of a depth format or too large: it then goes as before.
bool readDepth(uint32_t base, uint32_t psm, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
               std::vector<uint8_t> &out);

// Step 7c: the target a colour read of the GS buffer at `base` reads: the
// one drawn there, or the scene (for the display buffer, or a buffer never
// drawn).
uint32_t captureTarget(uint32_t base);

// The scene target's id (the frame's).
constexpr uint32_t kSceneTarget = 1u;
} // namespace rg::nativeproducer
