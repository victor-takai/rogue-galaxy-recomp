#include "native_producer.h"

#if defined(RG_HAS_SDLGPU)

#include <algorithm>
#include <array>
#include <deque>
#include <atomic>
#include <map>
#include <set>
#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "gfx/native/native_pipeline.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/gs/gs_types.h"
#include "flip_pacing.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace rg::nativeproducer
{
namespace
{
namespace native = dq8::gfx::native;

constexpr uint32_t kCT32 = 0x00u, kCT16 = 0x02u, kCT16S = 0x0Au, kT8 = 0x13u, kT4 = 0x14u;

struct Counts
{
    uint64_t levels = 0, texturesNew = 0, texturesKnown = 0;
    uint64_t refusedFormat = 0, refusedBytes = 0, refusedLayout = 0;
    uint64_t palettesNew = 0, palettesKnown = 0, palettesRefused = 0;
    uint64_t targetsNew = 0, targetsAuto = 0, targetsGrown = 0, texturesOverTargets = 0;
    uint64_t bytesHashed = 0;
    uint64_t withheldTextures = 0, withheldPalettes = 0, withheldBytes = 0;
    uint64_t passedNotProduced = 0, split = 0, continuedInPacket = 0;
    // Step 4a: VU1 meshes made mesh draws.
    uint64_t meshes = 0, meshesNoState = 0, meshesSplit = 0, meshesWhole = 0, camerasNew = 0;
    uint64_t materialKnown = 0, textureMiss = 0, textureMissPsm = 0, textureMissTarget = 0, paletteMiss = 0, targetMiss = 0;
    uint64_t unknownBlend = 0, unknownClamp = 0, unknownWriteMask = 0, unknownOther = 0;
    uint64_t triangles = 0, trianglesSkipped = 0;
    // Step 5b: 2D pieces made window-space mesh draws, and why not.
    uint64_t pieces = 0, piecesMade = 0, piecesNoState = 0, piecesNoTarget = 0, piecesNoSource = 0;
    uint64_t piecesFogged = 0, piecesFromTarget = 0, piecesSubTarget = 0, piecesCt24 = 0;
    uint64_t opsGreenToAlpha = 0;
    // Step 5c: draws into an object's own layer.
    uint64_t drawsRouted = 0;
    // Step 6: host uploads nobody took, kept as images (hostUpload).
    uint64_t hostUploads = 0, hostRefused = 0, hostBytes = 0, hostTexturesNew = 0, hostTexturesKnown = 0;
    uint64_t hostPalettes = 0, hostReadsRefused = 0;
    // Narrow views: a buffer drawn or read narrower than its target.
    uint64_t viewsNew = 0, viewWrites = 0, viewReads = 0;
    // Stale records made again from what was sent (replaceUpload).
    uint64_t palettesReplaced = 0, texturesReplaced = 0;
    uint64_t copiesMirrored = 0, copiesOtherFormat = 0, copiesNoSource = 0, copiesNoDestination = 0;
};

struct State
{
    std::mutex mutex;
    // Content (hash and shape) to id, and back, for what the renderer holds.
    std::unordered_map<uint64_t, native::Id> textureIds, paletteIds;
    std::unordered_map<native::Id, uint64_t> textureKeys;
    native::Id nextTexture = 1u, nextPalette = 1u, nextTarget = kSceneTarget + 1u;
    // The game's addresses to ids (step 4's lookups).
    std::unordered_map<uint32_t, std::pair<uint32_t, native::Id>> textureAt; // tbp -> (psm, id)
    std::unordered_map<uint32_t, native::Id> paletteAt;                       // cbp -> id
    std::unordered_map<uint32_t, native::Id> targetAt;                        // base -> id
    std::map<uint32_t, native::Target> targets;                               // base -> as declared
    std::unordered_map<uint32_t, native::Id> depthAt;                         // ZBUF base -> id
    std::unordered_map<uint64_t, native::Id> cameraIds;                       // matrices' hash -> id
    native::Id nextCamera = 1u;
    native::Id lastCamera = 0u; // the newest 3D draw's, for 2D draws that test depth
    // Step 5c: while an object is routed, the draws into `routeBase` go to
    // the layer of its own (`layer`, one target reused: each object's tail
    // is drawn from it, in order, before the next is routed).
    bool routing = false;
    uint32_t routeBase = 0u;
    native::Id layer = 0u;
    uint16_t layerWidth = 0u, layerHeight = 0u;
    int32_t layerBox[4] = {}; // the routed object's, x1 and y1 exclusive
    // Its viewport's depth row (z' = v22 z + v32 w), for depth read back
    // (step 7b).
    float lastDepthScale = 0.0f, lastDepthOffset = 0.0f;
    uint64_t depthReads = 0u, depthAnswered = 0u, depthNoTarget = 0u;
    uint32_t sceneBase = 0u;
    bool sceneBaseKnown = false;
    // Uploads made into records, in the game's order, for takeProduced.
    struct Produced
    {
        uint32_t dbp, dbw, dpsm, width, height;
        bool palette;
        uint32_t key, psm; // the texture's TBP and format, or the palette's CBP
        native::Id id;
        uint64_t hash; // imageHash of the guest memory it was made from
        uint32_t bytes;
    };
    // What GS memory holds as the VIF1 thread sends it on, in the stream's
    // order (takeProduced): the game thread's tables above run a frame of
    // uploads ahead, and the game reuses its texture addresses within one.
    std::unordered_map<uint32_t, std::pair<uint32_t, native::Id>> residentTexture; // tbp -> (psm, id)
    std::unordered_map<uint32_t, native::Id> residentPalette;                       // cbp -> id
    // When each address was last written, in the stream's order: uploaded
    // (a texture's TBP) or drawn into (a target's base).
    uint64_t sequence = 0u;
    std::unordered_map<uint32_t, uint64_t> uploadedSeq, drawnSeq;
    std::deque<Produced> produced;
    // When the VIF1 thread last took a produced texture (by TBP) or palette
    // (by CBP), against a host image's (step 6).
    std::unordered_map<uint32_t, uint64_t> takenSeq, takenPaletteSeq;
    // Step 6: what host uploads nobody took left in GS memory, one linear
    // image per base and format (the upload's pixels, rows of the buffer
    // width; a byte a texel for the 8- and 4-bit formats), read back by
    // draws of that format and buffer width: as the GS reads what it was
    // written with, no address swizzle is needed.
    struct HostImage
    {
        uint32_t dbw = 1u, psm = 0u, height = 0u;
        std::vector<uint8_t> pixels;
        uint64_t seq = 0u, version = 0u;
    };
    std::map<std::pair<uint32_t, uint32_t>, HostImage> hostImages; // (base, psm)
    // The record last made of an image, by its read (TBP, PSM, TW, TH; or
    // CBP, CPSM, entries), and the image's version then.
    struct HostRecord
    {
        uint64_t version = 0u;
        native::Id id = 0u;
    };
    std::unordered_map<uint64_t, HostRecord> hostTextures, hostPalettes;
    std::set<uint32_t> copiedTo; // local copies' destinations (logged reads)
    // A buffer the game also draws into at a narrower buffer width than its
    // target was declared at (an effect building a 64x64 texture in the
    // corner of a full-screen buffer, then sampling it at that width): a
    // target of its own, by base and width (viewKey), so the full-screen
    // draws do not cover it. Not modelled: a read at one width of what was
    // drawn at another, which the GS's pages fold.
    std::unordered_map<uint32_t, native::Target> views;          // viewKey -> as declared
    std::unordered_map<uint32_t, std::vector<uint32_t>> viewWidths; // base -> widths with a view
    // RG_NATIVE_TEXTURE_DUMP: the records' contents kept, to write out.
    struct Kept
    {
        std::shared_ptr<std::vector<uint8_t>> data;
        uint32_t width = 0u, height = 0u;
        bool indexed = false;
    };
    std::unordered_map<native::Id, Kept> keptTextures, keptPalettes;
    // Every live texture record's texels (a byte a texel, or RGBA8), for
    // local copies (mirrorCopy); forgotten with the renderer's evictions.
    std::unordered_map<native::Id, Kept> texels;
    std::set<uint64_t> dumped;
    Counts counts, reported;
    bool statsSet = false;
};
// RG_NATIVE_TEXTURE_DUMP=tbp,tbp (hex): the textures 2D pieces read there,
// written to build/run/texdump as PPM (colour, and alpha beside it).
const std::vector<uint32_t> &dumpBases()
{
    static const std::vector<uint32_t> bases = [] {
        std::vector<uint32_t> out;
        const char *v = std::getenv("RG_NATIVE_TEXTURE_DUMP");
        while (v && *v)
        {
            char *end = nullptr;
            out.push_back(static_cast<uint32_t>(std::strtoul(v, &end, 16)));
            v = (end && *end == ',') ? end + 1 : nullptr;
        }
        return out;
    }();
    return bases;
}
bool dumpingTextures()
{
    return !dumpBases().empty();
}
// The flip after `touch build/run/dump-now` (0: none yet), whose 2D pieces
// and meshes are logged; it also asks the renderer for its side of that
// frame (build/run/dump-gpu). Checked once a flip.
State &state();
std::string holdingAt(State &s, uint32_t base);
uint64_t triggeredDumpFlip()
{
    static uint64_t checked = 0u, triggered = 0u;
    const uint64_t flip = rg::graphics::guestFlips();
    if (flip != checked)
    {
        checked = flip;
        if (std::remove("build/run/dump-now") == 0)
        {
            triggered = flip + 1u;
            if (FILE *f = std::fopen("build/run/dump-gpu", "w"))
                std::fclose(f);
            std::fprintf(stderr, "[piece-dump] dumping flip %llu\n", (unsigned long long)triggered);
            for (const auto &[base, t] : state().targets)
                std::fprintf(stderr, "[target-map] base %04x -> target %u %ux%u role %d; holds:%s\n", base, t.id,
                             t.width, t.height, int(t.role), holdingAt(state(), base).c_str());
        }
    }
    return triggered;
}
State &state()
{
    static State instance;
    return instance;
}

// The identity of an image's bytes, never 0 (as the GS path's named
// textures hashed them; one pass of four lanes, 32 bytes a step).
uint64_t imageHash(const uint8_t *at, size_t size)
{
    constexpr uint64_t k = 0x9E3779B97F4A7C15ull;
    uint64_t lane[4] = {k, k ^ 1u, k ^ 2u, k ^ 3u};
    size_t left = size;
    while (left >= 32u)
    {
        for (int i = 0; i < 4; ++i)
        {
            uint64_t word;
            std::memcpy(&word, at + i * 8, 8);
            lane[i] = (lane[i] ^ word) * k;
            lane[i] ^= lane[i] >> 29;
        }
        at += 32;
        left -= 32u;
    }
    uint64_t tail = 0u;
    if (left)
        std::memcpy(&tail, at, left);
    uint64_t h = size;
    for (uint64_t v : {lane[0], lane[1], lane[2], lane[3], tail})
    {
        h = (h ^ v) * k;
        h ^= h >> 31;
    }
    return h | 1u;
}

uint64_t keyOf(uint64_t hash, uint32_t a, uint32_t b, uint32_t c)
{
    uint64_t k = hash ^ (uint64_t(a) * 0x9E3779B97F4A7C15ull);
    k ^= (uint64_t(b) << 21) * 0xC2B2AE3D27D4EB4Full;
    k ^= (uint64_t(c) << 42) * 0x165667B19E3779F9ull;
    return k ? k : 1u;
}

std::string statsLine()
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    const Counts &n = s.counts, &r = s.reported;
    const auto d = [](uint64_t a, uint64_t b) { return std::to_string(a - b); };
    std::string line = "[native-producer] texture levels " + d(n.levels, r.levels) + ": new " +
                       d(n.texturesNew, r.texturesNew) + ", known " + d(n.texturesKnown, r.texturesKnown) +
                       ", refused: format " + d(n.refusedFormat, r.refusedFormat) + ", bytes " +
                       d(n.refusedBytes, r.refusedBytes) + ", layout " + d(n.refusedLayout, r.refusedLayout) +
                       " | palettes new " + d(n.palettesNew, r.palettesNew) + ", known " +
                       d(n.palettesKnown, r.palettesKnown) + ", refused " + d(n.palettesRefused, r.palettesRefused) +
                       " | targets new " + d(n.targetsNew, r.targetsNew) + " (undeclared, from their draws " +
                       d(n.targetsAuto, r.targetsAuto) + "; grown to their draws " +
                       d(n.targetsGrown, r.targetsGrown) + ") | hashed " +
                       std::to_string((n.bytesHashed - r.bytesHashed) >> 10) + " KiB" +
                       " | meshes " + d(n.meshes, r.meshes) + ": no draw state " +
                       d(n.meshesNoState, r.meshesNoState) + ", camera and model " + d(n.meshesSplit, r.meshesSplit) +
                       ", whole matrix (model check failed) " + d(n.meshesWhole, r.meshesWhole) + ", cameras new " +
                       d(n.camerasNew, r.camerasNew) + ", material known " + d(n.materialKnown, r.materialKnown) +
                       " (misses: texture " + d(n.textureMiss, r.textureMiss) + " [another format there " +
                       d(n.textureMissPsm, r.textureMissPsm) + ", a render target there " +
                       d(n.textureMissTarget, r.textureMissTarget) + "], palette " +
                       d(n.paletteMiss, r.paletteMiss) + ", target " + d(n.targetMiss, r.targetMiss) +
                       "; unknown: blend " + d(n.unknownBlend, r.unknownBlend) + ", clamp " +
                       d(n.unknownClamp, r.unknownClamp) + ", write mask " + d(n.unknownWriteMask, r.unknownWriteMask) +
                       ", other " + d(n.unknownOther, r.unknownOther) + "), triangles " +
                       d(n.triangles, r.triangles) + " (skipped " + d(n.trianglesSkipped, r.trianglesSkipped) + ")" +
                       " | sampled a texture uploaded over a target " +
                       d(n.texturesOverTargets, r.texturesOverTargets) + " | 2D pieces " + d(n.pieces, r.pieces) + ": made " + d(n.piecesMade, r.piecesMade) +
                       " (from a target " + d(n.piecesFromTarget, r.piecesFromTarget) + ", as CT24 " +
                       d(n.piecesCt24, r.piecesCt24) + ", fogged " +
                       d(n.piecesFogged, r.piecesFogged) + "), to the GS: no state " +
                       d(n.piecesNoState, r.piecesNoState) + ", target not declared " +
                       d(n.piecesNoTarget, r.piecesNoTarget) + ", source unknown " +
                       d(n.piecesNoSource, r.piecesNoSource) + " (inside a target " +
                       d(n.piecesSubTarget, r.piecesSubTarget) + ") | made green to alpha " +
                       d(n.opsGreenToAlpha, r.opsGreenToAlpha) +
                       " | withheld from the GS: textures " + d(n.withheldTextures, r.withheldTextures) +
                       ", palettes " + d(n.withheldPalettes, r.withheldPalettes) + " (" +
                       std::to_string((n.withheldBytes - r.withheldBytes) >> 10) + " KiB); uploads sent on (no record) " +
                       d(n.passedNotProduced, r.passedNotProduced) + "; A+D blocks split (other registers kept) " +
                       d(n.split, r.split) + "; image continued in a packet " +
                       d(n.continuedInPacket, r.continuedInPacket) +
                       " | host uploads nobody took " + d(n.hostUploads, r.hostUploads) + " (" +
                       std::to_string((n.hostBytes - r.hostBytes) >> 10) + " KiB, refused " +
                       d(n.hostRefused, r.hostRefused) + "): textures made " +
                       d(n.hostTexturesNew, r.hostTexturesNew) + ", known " +
                       d(n.hostTexturesKnown, r.hostTexturesKnown) + ", palettes " +
                       d(n.hostPalettes, r.hostPalettes) + ", reads refused " +
                       d(n.hostReadsRefused, r.hostReadsRefused) +
                       " | stale records made again: palettes " + d(n.palettesReplaced, r.palettesReplaced) +
                       ", textures " + d(n.texturesReplaced, r.texturesReplaced) + " | local copies mirrored " +
                       d(n.copiesMirrored, r.copiesMirrored) + ", other format " + d(n.copiesOtherFormat, r.copiesOtherFormat) +
                       ", no source " + d(n.copiesNoSource, r.copiesNoSource) + ", no destination " +
                       d(n.copiesNoDestination, r.copiesNoDestination) + " | narrow views new " + d(n.viewsNew, r.viewsNew) + ", draws into them " +
                       d(n.viewWrites, r.viewWrites) + ", reads of them " + d(n.viewReads, r.viewReads) +
                       " | depth reads (all so far) " +
                       std::to_string(s.depthReads) + ": answered " + std::to_string(s.depthAnswered) +
                       ", no depth target there " + std::to_string(s.depthNoTarget);
    s.reported = s.counts;
    return line;
}

// Locked. An upload made into a record (or known as one), for takeProduced.
void noteProduced(State &s, uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t width, uint32_t height, bool palette,
                  uint32_t key, uint32_t psm, native::Id id, uint64_t hash, uint32_t bytes)
{
    // Never sent ones age out; a bound in case the VIF1 thread takes none.
    if (s.produced.size() >= 8192u)
        s.produced.pop_front();
    s.produced.push_back(
        {dbp & 0x3FFFu, std::max(dbw, 1u), dpsm & 0x3Fu, width, height, palette, key, psm, id, hash, bytes});
}

// Locked. The renderer's evictions, forgotten here so they are sent again.
void forgetEvicted(State &s)
{
    for (const native::Id id : native::takeEvicted())
    {
        const auto key = s.textureKeys.find(id);
        if (key == s.textureKeys.end())
            continue;
        s.textureIds.erase(key->second);
        s.textureKeys.erase(key);
        s.texels.erase(id);
    }
    if (!s.statsSet)
    {
        native::setProducerStats(&statsLine);
        s.statsSet = true;
    }
}

uint8_t expand5(uint32_t v)
{
    return static_cast<uint8_t>((v & 31u) << 3);
}

// A CT16 texel as RGBA8 with the game's alpha: A set reads as 0x80. (The GS
// takes TEXA's TA1 and TA0 for set and clear; the game's are 0x80 and 0 where
// it uses CT16, not yet read from its TEXA.)
void ct16(uint16_t v, uint8_t *out)
{
    out[0] = expand5(v);
    out[1] = expand5(v >> 5);
    out[2] = expand5(v >> 10);
    out[3] = (v & 0x8000u) ? 0x80u : 0u;
}

// The byte of a pre-swizzled upload each T8 texel is, as the GS's memory
// lays a CT32 rectangle and reads it back as T8 (relative to the base; a
// page is a page in both). Empty: not that layout.
const std::vector<uint32_t> &unswizzle(const TextureLevel &l)
{
    const std::array<uint32_t, 6> shape = {l.transferWidth, l.transferHeight, std::max(l.transferBufferWidth, 1u),
                                           l.width, l.height, std::max(l.bufferWidth, 1u)};
    static std::map<std::array<uint32_t, 6>, std::vector<uint32_t>> maps; // game thread
    auto known = maps.find(shape);
    if (known != maps.end())
        return known->second;
    if (maps.size() >= 256u)
        maps.clear();
    std::vector<uint32_t> &byteOf = maps[shape];
    uint32_t maxWord = 0u;
    for (uint32_t y = 0u; y < shape[1]; ++y)
        for (uint32_t x = 0u; x < shape[0]; ++x)
            maxWord = std::max(maxWord, GSPSMCT32::addrPSMCT32(0u, shape[2], x, y) >> 2u);
    std::vector<uint32_t> pixelOfWord(size_t(maxWord) + 1u, ~0u);
    for (uint32_t y = 0u; y < shape[1]; ++y)
        for (uint32_t x = 0u; x < shape[0]; ++x)
            pixelOfWord[GSPSMCT32::addrPSMCT32(0u, shape[2], x, y) >> 2u] = y * shape[0] + x;
    byteOf.resize(size_t(shape[3]) * shape[4]);
    for (uint32_t y = 0u; y < shape[4]; ++y)
        for (uint32_t x = 0u; x < shape[3]; ++x)
        {
            const uint32_t address = GSPSMT8::addrPSMT8(0u, shape[5], x, y);
            const uint32_t word = address >> 2u;
            if (word >= pixelOfWord.size() || pixelOfWord[word] == ~0u)
            {
                byteOf.clear();
                return byteOf;
            }
            byteOf[size_t(y) * shape[3] + x] = pixelOfWord[word] * 4u + (address & 3u);
        }
    return byteOf;
}
} // namespace

void texture(const TextureLevel &l)
{
    if (!native::enabled())
        return;
    State &s = state();
    std::lock_guard lock(s.mutex);
    forgetEvicted(s);
    ++s.counts.levels;
    const uint32_t psm = l.psm & 0x3Fu;
    const size_t texels = size_t(l.width) * l.height;
    // What it occupies in EE memory, as sent.
    size_t bytes = 0u;
    if (l.preSwizzled)
        bytes = size_t(l.transferWidth) * l.transferHeight * 4u;
    else if (psm == kCT32)
        bytes = texels * 4u;
    else if (psm == kCT16 || psm == kCT16S)
        bytes = texels * 2u;
    else if (psm == kT8)
        bytes = texels;
    else if (psm == kT4)
        bytes = (texels + 1u) / 2u;
    else
    {
        ++s.counts.refusedFormat;
        return;
    }
    if (texels == 0u || bytes == 0u || !l.ram || l.ramBytes < bytes || l.width > 4096u || l.height > 4096u)
    {
        ++s.counts.refusedBytes;
        return;
    }
    const uint64_t hash = imageHash(l.ram, bytes);
    s.counts.bytesHashed += bytes;
    const uint64_t key = keyOf(hash, psm | (l.preSwizzled ? 0x100u : 0u), l.width, l.height);
    const native::Id palette = (psm == kT8 || psm == kT4) ? (s.paletteAt.count(l.cbp) ? s.paletteAt[l.cbp] : 0u) : 0u;
    if (const auto known = s.textureIds.find(key); known != s.textureIds.end())
    {
        ++s.counts.texturesKnown;
        s.textureAt[l.tbp] = {psm, known->second};
        noteProduced(s, l.tbp, l.transferBufferWidth, l.transferPsm, l.transferWidth, l.transferHeight, false, l.tbp,
                     psm, known->second, hash, static_cast<uint32_t>(bytes));
        return;
    }
    auto out = std::make_shared<std::vector<uint8_t>>();
    native::Texture record;
    record.width = static_cast<uint16_t>(l.width);
    record.height = static_cast<uint16_t>(l.height);
    record.palette = palette;
    if (psm == kCT32 && !l.preSwizzled)
    {
        record.format = native::PixelFormat::Rgba8;
        out->assign(l.ram, l.ram + bytes);
    }
    else if (psm == kCT16 || psm == kCT16S)
    {
        record.format = native::PixelFormat::Rgba8;
        out->resize(texels * 4u);
        for (size_t i = 0; i < texels; ++i)
            ct16(uint16_t(l.ram[i * 2u] | (l.ram[i * 2u + 1u] << 8)), out->data() + i * 4u);
    }
    else if (l.preSwizzled)
    {
        const std::vector<uint32_t> &byteOf = unswizzle(l);
        if (byteOf.empty())
        {
            ++s.counts.refusedLayout;
            return;
        }
        record.format = native::PixelFormat::Index8;
        out->resize(texels);
        for (size_t i = 0; i < texels; ++i)
            (*out)[i] = byteOf[i] < bytes ? l.ram[byteOf[i]] : 0u;
    }
    else if (psm == kT8)
    {
        record.format = native::PixelFormat::Index8;
        out->assign(l.ram, l.ram + bytes);
    }
    else // T4: the low nibble first, widened to a byte each
    {
        record.format = native::PixelFormat::Index8;
        out->resize(texels);
        for (size_t i = 0; i < texels; ++i)
            (*out)[i] = (l.ram[i / 2u] >> ((i & 1u) * 4u)) & 0x0Fu;
    }
    record.id = s.nextTexture++;
    if (dumpingTextures())
        s.keptTextures[record.id] = {out, record.width, record.height,
                                     record.format == native::PixelFormat::Index8};
    s.texels[record.id] = {out, record.width, record.height, record.format == native::PixelFormat::Index8};
    record.texels = std::move(out);
    s.textureIds[key] = record.id;
    s.textureKeys[record.id] = key;
    s.textureAt[l.tbp] = {psm, record.id};
    noteProduced(s, l.tbp, l.transferBufferWidth, l.transferPsm, l.transferWidth, l.transferHeight, false, l.tbp, psm,
                 record.id, hash, static_cast<uint32_t>(bytes));
    ++s.counts.texturesNew;
    native::push(std::move(record));
}

void palette(const uint8_t *ram, uint32_t ramBytes, uint32_t cbp, uint32_t cpsm, uint32_t entries)
{
    if (!native::enabled())
        return;
    State &s = state();
    std::lock_guard lock(s.mutex);
    forgetEvicted(s);
    cpsm &= 0x3Fu;
    const bool is16 = cpsm == kCT16 || cpsm == kCT16S;
    const uint32_t bytes = entries * (is16 ? 2u : 4u);
    if (!ram || (entries != 16u && entries != 256u) || (cpsm != kCT32 && !is16) || ramBytes < bytes)
    {
        ++s.counts.palettesRefused;
        return;
    }
    const uint64_t hash = imageHash(ram, bytes);
    s.counts.bytesHashed += bytes;
    const uint64_t key = keyOf(hash, cpsm, entries, 0u);
    if (const auto known = s.paletteIds.find(key); known != s.paletteIds.end())
    {
        ++s.counts.palettesKnown;
        s.paletteAt[cbp] = known->second;
        noteProduced(s, cbp, 1u, cpsm, entries == 256u ? 16u : 8u, entries == 256u ? 16u : 2u, true, cbp, cpsm,
                     known->second, hash, bytes);
        return;
    }
    auto colors = std::make_shared<std::vector<uint8_t>>(entries * 4u);
    for (uint32_t i = 0; i < entries; ++i)
    {
        // CSM1: a 256-entry palette's entries 8-15 and 16-23 of each 32 are
        // stored swapped.
        const uint32_t at = entries == 256u ? ((i & 0xE7u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1)) : i;
        if (is16)
            ct16(uint16_t(ram[at * 2u] | (ram[at * 2u + 1u] << 8)), colors->data() + i * 4u);
        else
            std::copy_n(ram + at * 4u, 4u, colors->data() + i * 4u);
    }
    native::Palette record;
    record.id = s.nextPalette++;
    record.entries = static_cast<uint16_t>(entries);
    if (dumpingTextures())
        s.keptPalettes[record.id] = {colors, record.entries, 1u, false};
    record.colors = std::move(colors);
    s.paletteIds[key] = record.id;
    s.paletteAt[cbp] = record.id;
    noteProduced(s, cbp, 1u, cpsm, entries == 256u ? 16u : 8u, entries == 256u ? 16u : 2u, true, cbp, cpsm,
                 record.id, hash, bytes);
    ++s.counts.palettesNew;
    native::push(std::move(record));
}

namespace
{
// Locked. target()'s declaration; the id.
native::Id declare(State &s, uint32_t base, uint32_t width, uint32_t height, uint32_t psm, int kind)
{
    native::Target record;
    record.role = kind == 1 ? native::TargetRole::SceneColor
                  : kind == 2 ? native::TargetRole::Display
                              : native::TargetRole::Offscreen;
    // The scene is one buffer: the first the game names as its frame.
    // Another named so (CBlur's 02a40 is) is a buffer of its own; sharing
    // the scene's id, a copy into it overwrote the scene.
    if (record.role == native::TargetRole::SceneColor)
    {
        if (!s.sceneBaseKnown)
        {
            s.sceneBase = base;
            s.sceneBaseKnown = true;
        }
        else if (base != s.sceneBase)
            record.role = native::TargetRole::Offscreen;
    }
    record.width = static_cast<uint16_t>(std::min<uint32_t>(width, 4096u));
    record.height = static_cast<uint16_t>(std::min<uint32_t>(height, 4096u));
    record.format = (psm & 0x30u) == 0x30u ? native::PixelFormat::Depth32 : native::PixelFormat::Rgba8;
    auto at = s.targetAt.find(base);
    // A buffer other than the scene is used past the size it is named with
    // (draws below a layer's height): it keeps the largest it was named or
    // drawn at, so it never shrinks.
    if (const auto before = s.targets.find(base);
        before != s.targets.end() && record.role != native::TargetRole::SceneColor &&
        before->second.role != native::TargetRole::SceneColor)
    {
        record.width = std::max(record.width, before->second.width);
        record.height = std::max(record.height, before->second.height);
    }
    if (record.role == native::TargetRole::SceneColor)
        record.id = kSceneTarget;
    else if (at != s.targetAt.end() && at->second != kSceneTarget)
        record.id = at->second;
    else
        record.id = s.nextTarget++;
    const auto before = s.targets.find(base);
    const bool same = before != s.targets.end() && before->second.id == record.id &&
                      before->second.role == record.role && before->second.width == record.width &&
                      before->second.height == record.height && before->second.format == record.format;
    s.targetAt[base] = record.id;
    if (same)
        return record.id;
    s.targets[base] = record;
    ++s.counts.targetsNew;
    native::push(native::Target(record));
    return record.id;
}

// Locked. The target at `base`; one the game never named is declared from
// what draws into it (counted), and one drawn past its size grows to it.
native::Id targetForUnrouted(State &s, uint32_t base, uint32_t width, uint32_t height, uint32_t psm);

// Locked. The layer target, declared at least as large as the routed buffer.
native::Id layerTarget(State &s, uint32_t width, uint32_t height)
{
    if (s.layer == 0u)
        s.layer = s.nextTarget++;
    if (width > s.layerWidth || height > s.layerHeight)
    {
        s.layerWidth = static_cast<uint16_t>(std::min<uint32_t>(std::max<uint32_t>(width, s.layerWidth), 4096u));
        s.layerHeight = static_cast<uint16_t>(std::min<uint32_t>(std::max<uint32_t>(height, s.layerHeight), 4096u));
        native::Target record;
        record.id = s.layer;
        record.role = native::TargetRole::Offscreen;
        record.width = s.layerWidth;
        record.height = s.layerHeight;
        native::push(native::Target(record));
        ++s.counts.targetsNew;
    }
    return s.layer;
}

// Locked. A draw into the buffer at `base`: its target, or the object's
// layer while one is routed there.
native::Id targetFor(State &s, uint32_t base, uint32_t width, uint32_t height, uint32_t psm)
{
    const native::Id id = targetForUnrouted(s, base, width, height, psm);
    if (!s.routing || base != s.routeBase || id == 0u)
        return id;
    ++s.counts.drawsRouted;
    const auto declared = s.targets.find(base);
    return layerTarget(s, declared != s.targets.end() ? declared->second.width : width,
                       declared != s.targets.end() ? declared->second.height : height);
}

native::Id targetForUnrouted(State &s, uint32_t base, uint32_t width, uint32_t height, uint32_t psm)
{
    if (const auto at = s.targetAt.find(base); at != s.targetAt.end())
    {
        const auto declared = s.targets.find(base);
        if (declared != s.targets.end() && declared->second.role != native::TargetRole::SceneColor &&
            (width > declared->second.width || height > declared->second.height))
        {
            ++s.counts.targetsGrown;
            const int kind = declared->second.role == native::TargetRole::Display ? 2 : 0;
            return declare(s, base, std::max<uint32_t>(width, declared->second.width),
                           std::max<uint32_t>(height, declared->second.height), psm, kind);
        }
        return at->second;
    }
    if (width == 0u || height == 0u)
        return 0u;
    ++s.counts.targetsAuto;
    return declare(s, base, width, height, psm, 0);
}
uint32_t viewKey(uint32_t base, uint32_t fbw)
{
    return 0x80000000u | (std::min(std::max(fbw, 1u), 63u) << 16) | (base & 0xFFFFu);
}

// Locked. A draw or read at `base` with buffer width `fbw` (64-pixel
// units) narrower than the base's target: one for a view (State::views).
bool narrow(State &s, uint32_t base, uint32_t fbw)
{
    const auto t = s.targets.find(base);
    return t != s.targets.end() && t->second.role != native::TargetRole::SceneColor &&
           std::max(fbw, 1u) * 64u < t->second.width;
}

// Locked. The view at (base, fbw), declared, or grown to `width` x `height`.
native::Id viewTarget(State &s, uint32_t base, uint32_t fbw, uint32_t width, uint32_t height)
{
    fbw = std::max(fbw, 1u);
    width = std::min(std::min(width, fbw * 64u), 4096u);
    height = std::min(std::max(height, 1u), 4096u);
    native::Target &v = s.views[viewKey(base, fbw)];
    if (v.id != 0u && width <= v.width && height <= v.height)
        return v.id;
    if (v.id == 0u)
    {
        v.id = s.nextTarget++;
        s.viewWidths[base].push_back(fbw);
        ++s.counts.viewsNew;
    }
    v.role = native::TargetRole::Offscreen;
    v.format = native::PixelFormat::Rgba8;
    v.width = static_cast<uint16_t>(std::max<uint32_t>(width, v.width));
    v.height = static_cast<uint16_t>(std::max<uint32_t>(height, v.height));
    native::push(native::Target(v));
    return v.id;
}
} // namespace

void target(uint32_t base, uint32_t bufferWidth, uint32_t width, uint32_t height, uint32_t psm, int kind)
{
    if (!native::enabled() || width == 0u || height == 0u)
        return;
    State &s = state();
    std::lock_guard lock(s.mutex);
    (void)bufferWidth;
    declare(s, base, width, height, psm, kind);
}

bool takeProduced(uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t width, uint32_t height, bool &palette,
                  uint64_t &hash, uint32_t &bytes)
{
    if (!native::enabled())
        return false;
    State &s = state();
    std::lock_guard lock(s.mutex);
    constexpr size_t kLookahead = 64u;
    const size_t limit = std::min(s.produced.size(), kLookahead);
    for (size_t i = 0; i < limit; ++i)
    {
        const State::Produced &p = s.produced[i];
        if (p.dbp == (dbp & 0x3FFFu) && p.dbw == std::max(dbw, 1u) && p.dpsm == (dpsm & 0x3Fu) && p.width == width &&
            p.height == height)
        {
            palette = p.palette;
            hash = p.hash;
            bytes = p.bytes;
            if (p.palette)
            {
                s.residentPalette[p.key] = p.id;
                s.takenPaletteSeq[p.key] = ++s.sequence;
            }
            else
            {
                s.residentTexture[p.key] = {p.psm, p.id};
                s.uploadedSeq[p.key] = s.takenSeq[p.key] = ++s.sequence;
            }
            // The ones before it were never sent (docs/NATIVE-PIPELINE.md
            // step 6, "produced levels never taken"): the first few, by shape.
            static uint32_t told = 0u;
            for (size_t j = 0; j < i && told < 8u; ++j, ++told)
            {
                const State::Produced &q = s.produced[j];
                std::fprintf(stderr,
                             "[native-producer] produced, never taken: DBP %04x DBW %u DPSM %02x %ux%u%s; the next "
                             "taken: DBP %04x DBW %u DPSM %02x %ux%u\n",
                             q.dbp, q.dbw, q.dpsm, q.width, q.height, q.palette ? " (palette)" : "", p.dbp, p.dbw,
                             p.dpsm, p.width, p.height);
            }
            s.produced.erase(s.produced.begin(), s.produced.begin() + static_cast<std::ptrdiff_t>(i) + 1);
            return true;
        }
    }
    return false;
}

void replaceUpload(bool palette, uint32_t dbp, uint32_t dpsm, uint32_t width, uint32_t height, const uint8_t *data,
                   uint32_t bytes)
{
    if (!native::enabled() || !data)
        return;
    State &s = state();
    std::lock_guard lock(s.mutex);
    forgetEvicted(s);
    dpsm &= 0x3Fu;
    const uint64_t hash = imageHash(data, bytes);
    if (palette)
    {
        const uint32_t entries = width * height;
        const bool is16 = dpsm == kCT16 || dpsm == kCT16S;
        if ((entries != 16u && entries != 256u) || (dpsm != kCT32 && !is16) || bytes < entries * (is16 ? 2u : 4u))
            return;
        const uint64_t key = keyOf(hash, dpsm, entries, 0u);
        native::Id id = 0u;
        if (const auto known = s.paletteIds.find(key); known != s.paletteIds.end())
            id = known->second;
        else
        {
            auto colors = std::make_shared<std::vector<uint8_t>>(entries * 4u);
            for (uint32_t i = 0; i < entries; ++i)
            {
                const uint32_t at = entries == 256u ? ((i & 0xE7u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1)) : i;
                if (is16)
                    ct16(uint16_t(data[at * 2u] | (data[at * 2u + 1u] << 8)), colors->data() + i * 4u);
                else
                    std::copy_n(data + at * 4u, 4u, colors->data() + i * 4u);
            }
            native::Palette record;
            record.id = id = s.nextPalette++;
            record.entries = static_cast<uint16_t>(entries);
            if (dumpingTextures())
                s.keptPalettes[record.id] = {colors, record.entries, 1u, false};
            record.colors = std::move(colors);
            s.paletteIds[key] = record.id;
            native::push(std::move(record));
        }
        s.residentPalette[dbp] = id;
        s.takenPaletteSeq[dbp] = ++s.sequence;
        ++s.counts.palettesReplaced;
        return;
    }
    // A texture: only one sent as the format it is read in (not pre-swizzled).
    const auto resident = s.residentTexture.find(dbp);
    if (resident == s.residentTexture.end() || resident->second.first != dpsm)
        return;
    const size_t texels = size_t(width) * height;
    native::Texture record;
    auto out = std::make_shared<std::vector<uint8_t>>();
    if (dpsm == kCT32 && bytes >= texels * 4u)
    {
        record.format = native::PixelFormat::Rgba8;
        out->assign(data, data + texels * 4u);
    }
    else if ((dpsm == kCT16 || dpsm == kCT16S) && bytes >= texels * 2u)
    {
        record.format = native::PixelFormat::Rgba8;
        out->resize(texels * 4u);
        for (size_t i = 0; i < texels; ++i)
            ct16(uint16_t(data[i * 2u] | (data[i * 2u + 1u] << 8)), out->data() + i * 4u);
    }
    else if (dpsm == kT8 && bytes >= texels)
    {
        record.format = native::PixelFormat::Index8;
        out->assign(data, data + texels);
    }
    else if (dpsm == kT4 && bytes >= (texels + 1u) / 2u)
    {
        record.format = native::PixelFormat::Index8;
        out->resize(texels);
        for (size_t i = 0; i < texels; ++i)
            (*out)[i] = (data[i / 2u] >> ((i & 1u) * 4u)) & 0x0Fu;
    }
    else
        return;
    const uint64_t key = keyOf(hash, dpsm, width, height);
    native::Id id = 0u;
    if (const auto known = s.textureIds.find(key); known != s.textureIds.end())
        id = known->second;
    else
    {
        record.width = static_cast<uint16_t>(width);
        record.height = static_cast<uint16_t>(height);
        record.id = id = s.nextTexture++;
        if (dumpingTextures())
            s.keptTextures[record.id] = {out, record.width, record.height,
                                         record.format == native::PixelFormat::Index8};
        s.texels[record.id] = {out, record.width, record.height, record.format == native::PixelFormat::Index8};
        record.texels = std::move(out);
        s.textureIds[key] = record.id;
        s.textureKeys[record.id] = key;
        native::push(std::move(record));
    }
    resident->second.second = id;
    s.uploadedSeq[dbp] = s.takenSeq[dbp] = ++s.sequence;
    ++s.counts.texturesReplaced;
}

void noteWithheld(bool palette, uint32_t imageBytes)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    ++(palette ? s.counts.withheldPalettes : s.counts.withheldTextures);
    s.counts.withheldBytes += imageBytes;
}

void notePassed(Passed why)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    (void)why;
    ++s.counts.passedNotProduced;
}

void noteSplit()
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    ++s.counts.split;
}

void noteContinuedInPacket()
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    ++s.counts.continuedInPacket;
}

namespace
{
// A 4x4 of row vectors from VU1 data memory, quads `first` to `first + 3`.
void quadsMatrix(const uint8_t *vu1, uint32_t first, float out[16])
{
    std::memcpy(out, vu1 + first * 16u, 16u * sizeof(float));
}

void multiply(const float a[16], const float b[16], float out[16])
{
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] +
                           a[i * 4 + 3] * b[3 * 4 + j];
    std::memcpy(out, r, sizeof(r));
}

// Gauss-Jordan with partial pivoting; false when `m` is singular.
bool invert(const float m[16], float out[16])
{
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j)
            a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c)
    {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c]))
                pivot = r;
        if (std::fabs(a[pivot][c]) < 1e-12)
            return false;
        if (pivot != c)
            for (int j = 0; j < 8; ++j)
                std::swap(a[c][j], a[pivot][j]);
        const double d = a[c][c];
        for (int j = 0; j < 8; ++j)
            a[c][j] /= d;
        for (int r = 0; r < 4; ++r)
            if (r != c && a[r][c] != 0.0)
            {
                const double f = a[r][c];
                for (int j = 0; j < 8; ++j)
                    a[r][j] -= f * a[c][j];
            }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[i * 4 + j] = static_cast<float>(a[i][j + 4]);
    return true;
}

// `m` followed by the window's offset: x - ofx and y - ofy after the divide.
void foldOffset(float m[16], float ofx, float ofy)
{
    for (int i = 0; i < 4; ++i)
    {
        m[i * 4 + 0] -= m[i * 4 + 3] * ofx;
        m[i * 4 + 1] -= m[i * 4 + 3] * ofy;
    }
}

// Whether model x view x projection x viewport is the program's own matrix
// (68-71), column by column within 2e-3 of the column's largest entry.
bool sameMatrix(const float a[16], const float (&b)[4][4])
{
    for (int j = 0; j < 4; ++j)
    {
        float scale = 0.0f, error = 0.0f;
        for (int i = 0; i < 4; ++i)
        {
            scale = std::max(scale, std::fabs(b[i][j]));
            error = std::max(error, std::fabs(a[i * 4 + j] - b[i][j]));
        }
        if (!(error <= 2e-3f * scale + 1e-6f))
            return false;
    }
    return true;
}

native::Compare alphaCompare(uint32_t atst)
{
    static constexpr native::Compare table[8] = {
        native::Compare::Never,        native::Compare::Always,  native::Compare::Less,
        native::Compare::LessEqual,    native::Compare::Equal,   native::Compare::GreaterEqual,
        native::Compare::Greater,      native::Compare::NotEqual};
    return table[atst & 7u];
}

bool indexedPsm(uint32_t psm)
{
    return psm == kT8 || psm == kT4 || psm == 0x1Bu || psm == 0x24u || psm == 0x2Cu;
}

// The blend ALPHA means, (A - B) * C + D; A, B, D: 0 Cs, 1 Cd, 2 zero; C:
// 0 As, 1 Ad, 2 FIX. Every such equation is Cs * Fs + Cd * Fd, each factor
// 0, 1, C, 1 - C or -C: a GPU blend, a negative factor its op's subtract.
// False for C 3 (reserved) and a factor of 1 + C (A and D both Cs or Cd),
// which a GPU blend has no factor for.
bool blendOf(uint64_t alpha, native::Material &m)
{
    const uint32_t a = alpha & 3u, b = (alpha >> 2) & 3u, c = (alpha >> 4) & 3u, d = (alpha >> 6) & 3u;
    m.blending = true;
    m.blendFactor = static_cast<uint8_t>((alpha >> 32) & 0xFFu);
    // The coefficient of a color (0 Cs, 1 Cd): k1 * C + k0.
    const auto coefficient = [&](uint32_t color, int &k1, int &k0) {
        k1 = (a == color ? 1 : 0) - (b == color ? 1 : 0);
        k0 = d == color ? 1 : 0;
    };
    const native::BlendFactor withC = c == 0u ? native::BlendFactor::SrcAlpha
                                      : c == 1u ? native::BlendFactor::DstAlpha
                                                : native::BlendFactor::Fixed;
    const native::BlendFactor withoutC = c == 0u ? native::BlendFactor::InvSrcAlpha
                                         : c == 1u ? native::BlendFactor::InvDstAlpha
                                                   : native::BlendFactor::InvFixed;
    bool known = c != 3u;
    bool negative[2] = {};
    native::BlendFactor factors[2];
    for (uint32_t color = 0; color < 2u; ++color)
    {
        int k1, k0;
        coefficient(color, k1, k0);
        native::BlendFactor &f = factors[color];
        if (k1 == 0)
            f = k0 ? native::BlendFactor::One : native::BlendFactor::Zero;
        else if (k1 == 1 && k0 == 0)
            f = withC;
        else if (k1 == -1 && k0 == 1)
            f = withoutC;
        else if (k1 == -1 && k0 == 0)
        {
            f = withC;
            negative[color] = true;
        }
        else
        {
            f = native::BlendFactor::One; // 1 + C
            known = false;
        }
    }
    m.srcFactor = factors[0];
    m.dstFactor = factors[1];
    m.blendOp = negative[0] ? native::BlendOp::ReverseSubtract
                : negative[1] ? native::BlendOp::Subtract
                              : native::BlendOp::Add;
    return known;
}

// The alpha, destination alpha and depth tests TEST means, with ZBUF's mask.
bool testOf(uint64_t test, bool zmask, native::Material &m)
{
    if (test & 1u)
    {
        m.alphaTest = alphaCompare(static_cast<uint32_t>(test >> 1));
        m.alphaRef = static_cast<uint8_t>((test >> 4) & 0xFFu);
        static constexpr native::AlphaFail fails[4] = {native::AlphaFail::Nothing, native::AlphaFail::ColorOnly,
                                                      native::AlphaFail::DepthOnly, native::AlphaFail::RgbOnly};
        m.alphaFail = fails[(test >> 12) & 3u];
    }
    const bool zte = ((test >> 16) & 1u) != 0u;
    static constexpr native::Compare depths[4] = {native::Compare::Never, native::Compare::Always,
                                                  native::Compare::GreaterEqual, native::Compare::Greater};
    m.depthTest = zte ? depths[(test >> 17) & 3u] : native::Compare::Always;
    m.depthWrite = !zmask && m.depthTest != native::Compare::Never;
    // DATE, by DATM: the pixels whose alpha bit 7 is DATM's.
    m.destinationAlphaTest = ((test >> 14) & 1u) ? static_cast<int8_t>((test >> 15) & 1u) : int8_t(-1);
    return true;
}

// Locked. The material the draw state means; false where some part of it
// has no exact meaning here (counted by reason).
} // namespace
bool sourceOf(State &s, const GSTex0Reg &t, native::Material &m);
namespace
{
bool materialOf(State &s, const GSDrawState &state, native::Material &m)
{
    const GSContext &c = state.context;
    bool known = true;
    if (state.prim.tme)
    {
        const GSTex0Reg &t = c.tex0;
        // As a 2D piece's: a target drawn there, a produced texture, or a
        // host upload's image, whichever was written last (sourceOf).
        if (!sourceOf(s, t, m))
        {
            ++s.counts.textureMiss;
            const auto at = s.residentTexture.find(t.tbp0);
            if (at != s.residentTexture.end() && at->second.first != (t.psm & 0x3Fu))
            {
                ++s.counts.textureMissPsm;
                static uint64_t seen[64] = {};
                const uint32_t pair = at->second.first & 0x3Fu;
                if (std::getenv("RG_NATIVE_MESH_DUMP") && !(seen[pair] & (1ull << (t.psm & 0x3Fu))))
                {
                    seen[pair] |= 1ull << (t.psm & 0x3Fu);
                    std::fprintf(stderr, "[mesh-miss] tbp %x recorded psm %x, sampled psm %x tw %u th %u\n", t.tbp0,
                                 pair, t.psm, t.tw, t.th);
                }
            }
            else if (indexedPsm(t.psm) && at != s.residentTexture.end())
                ++s.counts.paletteMiss;
            known = false;
        }
        else if ((t.psm & 0x3Fu) == 0x01u)
        {
            // CT24, a target or a CT32 texture: alpha from TEXA (as piece()).
            m.fixedTextureAlpha = state.texa.ta0;
            m.alphaZeroOnBlack = state.texa.aem;
        }
        else if (m.sourceTarget && (t.psm & 0x3Fu) != 0x00u)
            known = false; // a target read in another format: not modelled
        m.textureWidth = static_cast<uint16_t>(1u << std::min<uint32_t>(t.tw, 10u));
        m.textureHeight = static_cast<uint16_t>(1u << std::min<uint32_t>(t.th, 10u));
        m.linear = state.linearFilter;
        const uint32_t wms = static_cast<uint32_t>(c.clamp & 3u), wmt = static_cast<uint32_t>((c.clamp >> 2) & 3u);
        m.clampU = wms != 0u;
        m.clampV = wmt != 0u;
        // Region clamp and repeat (a tile of a sheet): the renderer's.
        m.wrapU = static_cast<uint8_t>(wms);
        m.wrapV = static_cast<uint8_t>(wmt);
        m.regionU[0] = static_cast<uint16_t>((c.clamp >> 4) & 0x3FFu);
        m.regionU[1] = static_cast<uint16_t>((c.clamp >> 14) & 0x3FFu);
        m.regionV[0] = static_cast<uint16_t>((c.clamp >> 24) & 0x3FFu);
        m.regionV[1] = static_cast<uint16_t>((c.clamp >> 34) & 0x3FFu);
        if ((wms > 1u || wmt > 1u) && m.sourceTarget)
        {
            ++s.counts.unknownClamp; // a target's region: not fetched so
            known = false;
        }
        static constexpr native::TextureFunction functions[4] = {
            native::TextureFunction::Modulate, native::TextureFunction::Decal, native::TextureFunction::Highlight,
            native::TextureFunction::Highlight2};
        m.function = functions[t.tfx & 3u];
        m.textureAlpha = t.tcc != 0u;
    }

    if (state.prim.abe && !blendOf(c.alpha, m))
    {
        ++s.counts.unknownBlend;
        known = false;
    }

    if (!testOf(c.test, c.zbuf.zmask, m) || (c.fba & 1u))
    {
        ++s.counts.unknownOther;
        known = false;
    }

    // FBMSK: a channel is written when none of its bits are masked.
    const uint32_t mask = c.frame.fbmsk;
    m.colorWrite = 0u;
    bool partial = false;
    for (uint32_t ch = 0; ch < 4u; ++ch)
    {
        const uint32_t bits = (mask >> (ch * 8u)) & 0xFFu;
        if (bits == 0u)
            m.colorWrite |= static_cast<uint8_t>(1u << ch);
        else if (bits != 0xFFu)
            partial = true;
    }
    if (partial)
    {
        ++s.counts.unknownWriteMask;
        known = false;
    }
    m.fog = state.prim.fge;
    m.cullBack = false; // the producer's indices already leave out what the program culls
    return known;
}

// Locked. The id of the depth buffer at `base`, declared like the color
// target it is drawn with.
native::Id depthTarget(State &s, uint32_t base, uint32_t colorBase, native::Id color)
{
    const auto at = s.depthAt.find(base);
    if (at != s.depthAt.end())
        return at->second;
    native::Target record;
    record.id = s.nextTarget++;
    record.role = color == kSceneTarget ? native::TargetRole::SceneDepth : native::TargetRole::Offscreen;
    record.format = native::PixelFormat::Depth32;
    record.width = 640u;
    record.height = 448u;
    if (const auto declared = s.targets.find(colorBase); declared != s.targets.end())
    {
        record.width = declared->second.width;
        record.height = declared->second.height;
    }
    s.depthAt.emplace(base, record.id);
    ++s.counts.targetsNew;
    native::push(native::Target(record));
    return record.id;
}

// Locked. The camera's id, a record sent when it is new.
native::Id cameraOf(State &s, const float view[16], const float projection[16], const float viewport[16])
{
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&](const float *m) {
        for (int i = 0; i < 16; ++i)
        {
            uint32_t bits;
            std::memcpy(&bits, &m[i], 4u);
            hash = (hash ^ bits) * 1099511628211ull;
        }
    };
    mix(view);
    mix(projection);
    mix(viewport);
    const auto at = s.cameraIds.find(hash);
    if (at != s.cameraIds.end())
        return at->second;
    if (s.cameraIds.size() >= native::kCameraWindow) // the renderer keeps no more (native_records.h)
        s.cameraIds.clear();
    native::Camera record;
    record.id = s.nextCamera++;
    std::memcpy(record.view, view, sizeof(record.view));
    std::memcpy(record.projection, projection, sizeof(record.projection));
    std::memcpy(record.viewport, viewport, sizeof(record.viewport));
    s.cameraIds.emplace(hash, record.id);
    ++s.counts.camerasNew;
    native::push(native::Camera(record));
    return record.id;
}
} // namespace

bool mesh(const GSNativeMesh &mesh, const GSDrawState *drawState, const uint8_t *vu1Data, uint32_t source)
{
    if (!native::enabled() || !vu1Data)
        return false;
    // A chunk the program clips (h.clips: its slot E clips at z = -w and
    // z = w) keeps the triangles crossing the guard band: the GS backend
    // draws them clipped by the planes, and here the GPU clips them in clip
    // space. Skipping them left holes in floors near the camera.
    // RG_NATIVE_GUARD_CLIPPED=1 skips them as before (for comparing).
    static const bool guardClipped = [] {
        const char *v = std::getenv("RG_NATIVE_GUARD_CLIPPED");
        return v && std::strcmp(v, "0") != 0;
    }();
    GSNativeMesh::Header h = mesh.header;
    if (h.clips && !guardClipped)
        h.guardBand = 0u;
    const uint32_t count = static_cast<uint32_t>(mesh.vertices.size());

    native::MeshDraw draw;
    draw.source = source;

    // The triangles the program draws: a strip's every vertex from the
    // third on, a list's every third, less those the program culls (the
    // GS front end's rule for this mesh, gsNativeMeshSkips).
    const bool culls = h.guardBand || h.backface;
    thread_local std::vector<GSNativeMeshPoint> points;
    if (culls)
    {
        // Only what the skip rule reads (px, py, outside), by the same
        // operations as gsNativeMeshPoint, so the same bits.
        points.resize(count);
        const auto &m = h.matrix;
        for (uint32_t k = 0; k < count; ++k)
        {
#if defined(__clang__)
#pragma clang fp contract(off) // rounded as the VU1's separate multiply and add
#endif
            const GSNativeMesh::Vertex &v = mesh.vertices[k];
            float clip[4];
            for (int i = 0; i < 4; ++i)
                clip[i] = v.x * m[0][i] + v.y * m[1][i] + v.z * m[2][i] + m[3][i];
            GSNativeMeshPoint &p = points[k];
            p.q = gsNativeReciprocal(clip[3]);
            p.px = clip[0] * p.q;
            p.py = clip[1] * p.q;
            p.outside = false;
            if (h.guardBand)
            {
                const float projected[3] = {p.px, p.py, clip[2] * p.q};
                for (int i = 0; i < 3; ++i)
                {
                    const float g = projected[i] * h.bandScale[i] + h.bandOffset[i] * h.bandScale[i];
                    p.outside = p.outside || g > 1.0f || g < -1.0f;
                }
            }
        }
    }
    const bool lists = h.triangleList != 0u;
    const bool listCull = lists && h.backface;
    float winding = listCull ? -h.winding : h.winding;
    uint64_t skipped = 0u;
    draw.indices.reserve(count >= 2u ? (count - 2u) * 3u : 0u);
    for (uint32_t k = 1; k < count; ++k)
    {
        const bool judged = !listCull || k % 3u == 2u;
        bool skip = culls && judged && gsNativeMeshSkips(h, points.data(), k, winding);
        // A clipping chunk's triangle with a corner behind the eye (w < 0):
        // its projected corner lands mirrored, so the projected facing has
        // the wrong sign and a floor in front of the camera was culled at
        // some angles. The VU1 judges such a triangle after its slot E has
        // clipped it; here the GPU clips after, so the facing is judged in
        // homogeneous terms: the projected one times the sign of the three
        // w's product (the same as before while every w is positive).
        if (h.clips && h.backface && judged && k >= 2u)
        {
            const float wSign = points[k - 2u].q * points[k - 1u].q * points[k].q;
            if (wSign < 0.0f)
            {
                GSNativeMesh::Header facingOnly = h;
                facingOnly.guardBand = 0u;
                skip = gsNativeMeshSkips(facingOnly, points.data(), k, -winding);
            }
        }
        if (culls && !listCull)
            winding = -winding;
        const bool drawn = k >= 2u && (lists ? k % 3u == 2u : true);
        if (!drawn)
            continue;
        if (skip)
        {
            ++skipped;
            continue;
        }
        draw.indices.push_back(k - 2u);
        draw.indices.push_back(k - 1u);
        draw.indices.push_back(k);
    }

    draw.vertices.resize(count);
    for (uint32_t k = 0; k < count; ++k)
    {
        const GSNativeMesh::Vertex &in = mesh.vertices[k];
        native::Vertex &out = draw.vertices[k];
        out.position[0] = in.x;
        out.position[1] = in.y;
        out.position[2] = in.z;
        out.uv[0] = in.u;
        out.uv[1] = in.v;
        out.color[0] = std::fmin(in.r, h.colorMax[0]);
        out.color[1] = std::fmin(in.g, h.colorMax[1]);
        out.color[2] = std::fmin(in.b, h.colorMax[2]);
        out.color[3] = std::fmin(in.a, h.colorMax[3]);
    }
    std::memcpy(draw.fog, h.fog, sizeof(draw.fog));

    // The camera and the model, as the game keeps them in VU1 data memory.
    float model[16], view[16], projection[16], viewport[16], whole[16];
    quadsMatrix(vu1Data, 60u, model);
    quadsMatrix(vu1Data, 24u, view);
    quadsMatrix(vu1Data, 8u, projection);
    quadsMatrix(vu1Data, 4u, viewport);
    // Quads 4-7 are the projection all the way to the window (8-11 times
    // the screen's scale and offset), not a viewport after 8-11: the
    // program's matrix is model x view x (4-7). The camera keeps 8-11 as
    // its clip space and the rest, inverse(8-11) x (4-7), as its viewport.
    float toWindow[16], inverse[16];
    std::memcpy(toWindow, viewport, sizeof(toWindow));
    multiply(model, view, whole);
    multiply(whole, toWindow, whole);
    const bool split = sameMatrix(whole, h.matrix) && invert(projection, inverse);
    if (split)
        multiply(inverse, toWindow, viewport);

    State &s = state();
    std::lock_guard lock(s.mutex);
    forgetEvicted(s);
    ++s.counts.meshes;
    s.counts.triangles += draw.indices.size() / 3u;
    s.counts.trianglesSkipped += skipped;
    if (!drawState)
    {
        ++s.counts.meshesNoState;
        return false;
    }
    const GSContext &c = drawState->context;
    const float ofx = static_cast<float>(c.xyoffset.ofx) / 16.0f;
    const float ofy = static_cast<float>(c.xyoffset.ofy) / 16.0f;
    if (split)
    {
        foldOffset(viewport, ofx, ofy);
        draw.camera = cameraOf(s, view, projection, viewport);
        s.lastCamera = draw.camera;
        s.lastDepthScale = viewport[10];
        s.lastDepthOffset = viewport[14];
        draw.hasModel = true;
        std::memcpy(draw.model, model, sizeof(draw.model));
        draw.floatDepth = true;
        ++s.counts.meshesSplit;
    }
    else
    {
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                draw.model[i * 4 + j] = h.matrix[i][j];
        foldOffset(draw.model, ofx, ofy);
        draw.hasModel = true;
        ++s.counts.meshesWhole;
    }

    bool known = materialOf(s, *drawState, draw.material);
    const uint32_t frameBase = c.frame.fbp * 32u;
    uint32_t seqKey = frameBase; // the buffer's, or its narrow view's
    if (narrow(s, frameBase, c.frame.fbw))
    {
        seqKey = viewKey(frameBase, c.frame.fbw);
        draw.target = viewTarget(s, frameBase, c.frame.fbw, c.scissor.x1 + 1u, c.scissor.y1 + 1u);
        draw.material.depthTest = native::Compare::Always; // a view has no depth of its own
        draw.material.depthWrite = false;
        ++s.counts.viewWrites;
    }
    else
        draw.target = s.targetAt.count(frameBase)
                          ? targetFor(s, frameBase, c.scissor.x1 + 1u, c.scissor.y1 + 1u, c.frame.psm)
                          : 0u;
    if (draw.target == 0u)
    {
        // A buffer the game never named: the renderer has nowhere to draw
        // it, so the GS keeps it (review of #41).
        ++s.counts.targetMiss;
        return false;
    }
    if (draw.material.depthTest != native::Compare::Always || draw.material.depthWrite)
        draw.depthTarget = depthTarget(s, c.zbuf.zbp * 32u, frameBase, draw.target);
    draw.scissor = {c.scissor.x0, c.scissor.y0, c.scissor.x1 + 1, c.scissor.y1 + 1};
    draw.fogColor[0] = drawState->fogR;
    draw.fogColor[1] = drawState->fogG;
    draw.fogColor[2] = drawState->fogB;
    draw.material.known = known;
    if (known)
        ++s.counts.materialKnown;
    if (draw.target != s.layer || s.layer == 0u) // a routed draw leaves the buffer as it was
        s.drawnSeq[seqKey] = ++s.sequence;
    if (const uint64_t dumpFlip = triggeredDumpFlip(); dumpFlip != 0u && rg::graphics::guestFlips() == dumpFlip)
    {
        const GSContext &c = drawState->context;
        const native::Material &m = draw.material;
        uint32_t declaredW = 0u, declaredH = 0u;
        if (m.sourceTarget)
            for (const auto &[base, t] : s.targets)
                if (t.id == m.sourceTarget)
                {
                    declaredW = t.width;
                    declaredH = t.height;
                }
        std::fprintf(stderr,
                     "[mesh-dump] source declared %ux%u | fbp %04x -> target %u depth %u cam %u | tme %d tbp %05x tbw %u psm %02x %ux%u cbp "
                     "%05x csa %u tfx %u tcc %u -> %s %u palette %u | alpha %llx test %llx clamp %llx | %zu vertices%s\n",
                     declaredW, declaredH, c.frame.fbp * 32u, draw.target, draw.depthTarget, draw.camera,
                     int(drawState->prim.tme),
                     c.tex0.tbp0, c.tex0.tbw, c.tex0.psm & 0x3Fu, 1u << c.tex0.tw, 1u << c.tex0.th, c.tex0.cbp,
                     c.tex0.csa, c.tex0.tfx, c.tex0.tcc, m.sourceTarget ? "target" : "texture",
                     m.sourceTarget ? m.sourceTarget : m.texture, m.palette, (unsigned long long)c.alpha,
                     (unsigned long long)c.test, (unsigned long long)c.clamp, draw.vertices.size(),
                     m.known ? "" : " (unknown)");
    }
    native::push(native::MeshDraw(std::move(draw)));
    return true;
}

namespace
{
// Bits a host upload sends a pixel of, by DPSM; 0: not one kept.
uint32_t hostBits(uint32_t psm)
{
    switch (psm)
    {
    case kCT32: return 32u;
    case kCT16: case kCT16S: return 16u;
    case kT8: case 0x1Bu: return 8u;               // T8, T8H
    case kT4: case 0x24u: case 0x2Cu: return 4u;   // T4, T4HL, T4HH
    default: return 0u;
    }
}

// The bits of each 32-bit word a format in a 32-bit word holds; the others
// (CT16, T8, T4) lay memory out their own way: all of it.
uint32_t wordBits(uint32_t psm)
{
    switch (psm)
    {
    case kCT32: return 0xFFFFFFFFu;
    case 0x1Bu: return 0xFF000000u;
    case 0x24u: return 0x0F000000u;
    case 0x2Cu: return 0xF0000000u;
    default: return 0u;
    }
}

// The bytes of GS memory an image covers, roughly (rows of whole pages
// aside), from its base.
uint64_t hostSpan(uint32_t psm, uint32_t dbw, uint32_t height)
{
    const uint64_t texels = uint64_t(dbw) * 64u * height;
    if (wordBits(psm))
        return texels * 4u;
    return texels * hostBits(psm) / 8u;
}

// Locked. The host image a texturing read names: its format, at its base,
// at the buffer width it was written with (a palette's: any). Null when
// none.
State::HostImage *hostImageFor(State &s, uint32_t base, uint32_t psm, uint32_t bufferWidth, bool anyWidth = false)
{
    const auto at = s.hostImages.find({base, psm});
    if (at == s.hostImages.end())
        return nullptr;
    if (!anyWidth && at->second.dbw != std::max(bufferWidth, 1u))
    {
        ++s.counts.hostReadsRefused;
        return nullptr;
    }
    return &at->second;
}

// Locked. A record of a host image as a texture of the read's size (texels
// past what was written are 0): the one made before while unchanged.
native::Id hostTexture(State &s, const State::HostImage &image, const GSTex0Reg &t)
{
    const uint32_t psm = t.psm & 0x3Fu;
    const uint32_t width = 1u << std::min<uint32_t>(t.tw, 10u), height = 1u << std::min<uint32_t>(t.th, 10u);
    const uint64_t read = (uint64_t(t.tbp0) << 24) | (uint64_t(psm) << 8) | (uint64_t(t.tw & 15u) << 4) | (t.th & 15u);
    State::HostRecord &made = s.hostTextures[read];
    if (made.id != 0u && made.version == image.version && s.textureKeys.count(made.id))
        return made.id;
    const uint32_t stride = image.dbw * 64u;
    const uint32_t bytesPer = psm == kCT32 ? 4u : (psm == kCT16 || psm == kCT16S) ? 2u : 1u;
    const bool rgba = bytesPer > 1u;
    auto out = std::make_shared<std::vector<uint8_t>>(size_t(width) * height * (rgba ? 4u : 1u), uint8_t(0));
    for (uint32_t y = 0; y < height && y < image.height; ++y)
    {
        const uint8_t *row = image.pixels.data() + size_t(y) * stride * bytesPer;
        uint8_t *dst = out->data() + size_t(y) * width * (rgba ? 4u : 1u);
        const uint32_t n = std::min(width, stride);
        if (bytesPer == 2u)
            for (uint32_t x = 0; x < n; ++x)
                ct16(uint16_t(row[x * 2u] | (row[x * 2u + 1u] << 8)), dst + x * 4u);
        else
            std::memcpy(dst, row, size_t(n) * bytesPer);
    }
    const uint64_t hash = imageHash(out->data(), out->size());
    s.counts.bytesHashed += out->size();
    made.version = image.version;
    const uint64_t key = keyOf(hash, psm | 0x200u, width, height);
    if (const auto known = s.textureIds.find(key); known != s.textureIds.end())
    {
        ++s.counts.hostTexturesKnown;
        made.id = known->second;
        return made.id;
    }
    native::Texture record;
    record.width = static_cast<uint16_t>(width);
    record.height = static_cast<uint16_t>(height);
    record.format = rgba ? native::PixelFormat::Rgba8 : native::PixelFormat::Index8;
    record.id = s.nextTexture++;
    if (dumpingTextures())
        s.keptTextures[record.id] = {out, record.width, record.height,
                                     record.format == native::PixelFormat::Index8};
    s.texels[record.id] = {out, record.width, record.height, record.format == native::PixelFormat::Index8};
    record.texels = std::move(out);
    s.textureIds[key] = record.id;
    s.textureKeys[record.id] = key;
    made.id = record.id;
    ++s.counts.hostTexturesNew;
    native::push(std::move(record));
    return made.id;
}

// Locked. A host image read as a palette (CSM1, CSA 0): 16 entries from
// an 8x2 rectangle, 256 from 16x16 (entries 8-15 and 16-23 of each 32
// swapped). 0 when it holds less.
native::Id hostPalette(State &s, const State::HostImage &image, uint32_t cbp, uint32_t entries)
{
    const uint32_t columns = entries == 256u ? 16u : 8u, rows = entries == 256u ? 16u : 2u;
    const uint32_t stride = image.dbw * 64u;
    const bool is16 = image.psm != kCT32;
    if (image.height < rows || stride < columns)
        return 0u;
    const uint64_t read = (uint64_t(cbp) << 24) | (uint64_t(image.psm) << 12) | entries;
    State::HostRecord &made = s.hostPalettes[read];
    if (made.id != 0u && made.version == image.version)
        return made.id;
    auto colors = std::make_shared<std::vector<uint8_t>>(entries * 4u);
    for (uint32_t i = 0; i < entries; ++i)
    {
        const uint32_t at = entries == 256u ? ((i & 0xE7u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1)) : i;
        const size_t texel = size_t(at / columns) * stride + at % columns;
        const uint8_t *src = image.pixels.data() + texel * (is16 ? 2u : 4u);
        if (is16)
            ct16(uint16_t(src[0] | (src[1] << 8)), colors->data() + i * 4u);
        else
            std::copy_n(src, 4u, colors->data() + i * 4u);
    }
    const uint64_t hash = imageHash(colors->data(), colors->size());
    made.version = image.version;
    const uint64_t key = keyOf(hash, image.psm | 0x200u, entries, 0u);
    if (const auto known = s.paletteIds.find(key); known != s.paletteIds.end())
    {
        made.id = known->second;
        return made.id;
    }
    native::Palette record;
    record.id = s.nextPalette++;
    record.entries = static_cast<uint16_t>(entries);
    if (dumpingTextures())
        s.keptPalettes[record.id] = {colors, record.entries, 1u, false};
    record.colors = std::move(colors);
    s.paletteIds[key] = record.id;
    made.id = record.id;
    ++s.counts.hostPalettes;
    native::push(std::move(record));
    return made.id;
}

} // namespace

// Locked. The source a texturing draw state names: a target (whose base
// TBP0 is), a texture the VIF1 thread last sent there (and its palette), or
// what a host upload nobody took left there (step 6). False when none.
bool sourceOf(State &s, const GSTex0Reg &t, native::Material &m)
{
    const uint32_t psm = t.psm & 0x3Fu;
    // The game uploads textures into the same memory some of its buffers
    // use (02a00, 02a40): whichever wrote there last, in the stream's
    // order, is what the GS would read.
    const auto tex = s.residentTexture.find(t.tbp0);
    // A CT24 read of a CT32 texture is the same texels, its alpha TEXA's
    // (the caller's): an effect's 64x64 upload read so.
    bool texture = tex != s.residentTexture.end() &&
                   (tex->second.first == psm || (psm == 0x01u && tex->second.first == kCT32));
    State::HostImage *host = hostImageFor(s, t.tbp0, psm, t.tbw);
    if (host && texture)
    {
        const auto taken = s.takenSeq.find(t.tbp0);
        if (taken != s.takenSeq.end() && taken->second > host->seq)
            host = nullptr;
        else
            texture = false;
    }
    // A narrow read of a buffer with narrow views: the view of its width,
    // else whichever of the views and the buffer was drawn last.
    if (const auto widths = s.viewWidths.find(t.tbp0); widths != s.viewWidths.end() && narrow(s, t.tbp0, t.tbw))
    {
        const auto seqOf = [&](uint32_t key) {
            const auto q = s.drawnSeq.find(key);
            return q == s.drawnSeq.end() ? uint64_t(0) : q->second;
        };
        native::Id id = 0u;
        uint64_t seq = 0u;
        for (const uint32_t w : widths->second)
        {
            const uint32_t key = viewKey(t.tbp0, w);
            const uint64_t q = seqOf(key);
            const auto v = s.views.find(key);
            if (v == s.views.end())
                continue;
            if (w == std::max<uint32_t>(t.tbw, 1u))
            {
                id = v->second.id;
                seq = q;
                break;
            }
            if (q > seq)
            {
                id = v->second.id;
                seq = q;
            }
        }
        const auto uploaded = s.uploadedSeq.find(t.tbp0);
        const bool uploadedLast = uploaded != s.uploadedSeq.end() && uploaded->second > seq;
        const auto own = s.views.find(viewKey(t.tbp0, t.tbw));
        const bool ownWidth = own != s.views.end() && own->second.id == id;
        if (id != 0u && (seq >= seqOf(t.tbp0) || ownWidth) &&
            !((texture || host) && uploadedLast))
        {
            m.sourceTarget = id;
            ++s.counts.viewReads;
            return true;
        }
    }
    const auto at = s.targetAt.find(t.tbp0);
    if (at != s.targetAt.end())
    {
        const auto drawn = s.drawnSeq.find(t.tbp0), uploaded = s.uploadedSeq.find(t.tbp0);
        const bool uploadedLast = uploaded != s.uploadedSeq.end() &&
                                  (drawn == s.drawnSeq.end() || uploaded->second > drawn->second);
        if (!(texture || host) || !uploadedLast)
        {
            m.sourceTarget = at->second;
            return true;
        }
        ++s.counts.texturesOverTargets;
    }
    if (host)
    {
        if (psm == 0x01u) // CT24: its alpha is TEXA's, not kept here
            return false;
        m.texture = hostTexture(s, *host, t);
    }
    else if (texture)
        m.texture = tex->second.second;
    else
        return false;
    if (indexedPsm(psm))
    {
        if (t.csa != 0u)
            return false;
        const uint32_t entries = (psm == kT8 || psm == 0x1Bu) ? 256u : 16u;
        const auto p = s.residentPalette.find(t.cbp);
        State::HostImage *hostClut = hostImageFor(s, t.cbp, t.cpsm & 0x3Fu, 0u, true);
        if (hostClut && p != s.residentPalette.end())
        {
            const auto taken = s.takenPaletteSeq.find(t.cbp);
            if (taken != s.takenPaletteSeq.end() && taken->second > hostClut->seq)
                hostClut = nullptr;
        }
        if (hostClut)
            m.palette = hostPalette(s, *hostClut, t.cbp, entries);
        else if (p != s.residentPalette.end())
            m.palette = p->second;
        if (m.palette == 0u)
            return false;
    }
    return m.texture != 0u;
}

namespace
{
// Locked. What the producer holds at a GS address, for the logs.
std::string holdingAt(State &s, uint32_t base)
{
    std::string out;
    char buf[96];
    if (const auto t = s.residentTexture.find(base); t != s.residentTexture.end())
    {
        std::snprintf(buf, sizeof(buf), " texture psm %02x id %u (taken seq %llu)", t->second.first, t->second.second,
                      (unsigned long long)(s.takenSeq.count(base) ? s.takenSeq[base] : 0u));
        out += buf;
    }
    if (const auto t = s.targetAt.find(base); t != s.targetAt.end())
    {
        std::snprintf(buf, sizeof(buf), " target %u (drawn seq %llu)", t->second,
                      (unsigned long long)(s.drawnSeq.count(base) ? s.drawnSeq[base] : 0u));
        out += buf;
    }
    for (const auto &[key, image] : s.hostImages)
        if (key.first == base)
        {
            std::snprintf(buf, sizeof(buf), " host image psm %02x dbw %u h %u", key.second, image.dbw, image.height);
            out += buf;
        }
    if (const auto t = s.textureAt.find(base); t != s.textureAt.end())
    {
        std::snprintf(buf, sizeof(buf), " (game thread: texture psm %02x id %u)", t->second.first, t->second.second);
        out += buf;
    }
    return out.empty() ? " nothing" : out;
}

// Locked. Texels of GS memory at a base as one format, in buffer rows:
// the host image there or the resident texture, whichever was written
// last (as sourceOf). Bytes a texel as the record keeps them (RGBA8 for
// the 16-bit formats' textures, raw for their host images).
struct Plane
{
    std::vector<uint8_t> *pixels = nullptr; // host image: written in place
    const State::Kept *kept = nullptr;      // texture record: remade
    State::HostImage *host = nullptr;
    uint32_t stride = 0u, height = 0u, bytesPer = 0u;
};
Plane planeAt(State &s, uint32_t base, uint32_t psm, uint32_t bufferWidth)
{
    Plane p;
    State::HostImage *host = nullptr;
    if (const auto at = s.hostImages.find({base, psm});
        at != s.hostImages.end() && at->second.dbw == std::max(bufferWidth, 1u))
        host = &at->second;
    const auto resident = s.residentTexture.find(base);
    const State::Kept *kept = nullptr;
    if (resident != s.residentTexture.end() && resident->second.first == psm)
        if (const auto k = s.texels.find(resident->second.second); k != s.texels.end() && k->second.data)
            kept = &k->second;
    if (host && kept)
    {
        const auto taken = s.takenSeq.find(base);
        if (taken != s.takenSeq.end() && taken->second > host->seq)
            host = nullptr;
    }
    if (host)
    {
        p.host = host;
        p.pixels = &host->pixels;
        p.stride = host->dbw * 64u;
        p.height = host->height;
        p.bytesPer = psm == kCT32 ? 4u : (psm == kCT16 || psm == kCT16S) ? 2u : 1u;
    }
    else if (kept)
    {
        p.kept = kept;
        p.stride = kept->width;
        p.height = kept->height;
        p.bytesPer = kept->indexed ? 1u : 4u;
    }
    return p;
}

// Locked. A local copy (TRXDIR 2) done on what the producer holds: between
// the same format, from a host image or texture into one, the
// destination's texture made again with the rectangle written (clipped to
// both). Copies between formats reinterpret GS memory's layout and are
// counted, not done (docs/NATIVE-PIPELINE.md step 6).
void mirrorCopy(State &s, uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t sx, uint32_t sy, uint32_t dbp,
                uint32_t dbw, uint32_t dpsm, uint32_t dx, uint32_t dy, uint32_t width, uint32_t height)
{
    spsm &= 0x3Fu;
    dpsm &= 0x3Fu;
    if (spsm != dpsm)
    {
        ++s.counts.copiesOtherFormat;
        return;
    }
    const Plane from = planeAt(s, sbp, spsm, sbw);
    if (!from.pixels && !from.kept)
    {
        ++s.counts.copiesNoSource;
        return;
    }
    Plane to = planeAt(s, dbp, dpsm, dbw);
    if ((!to.pixels && !to.kept) || to.bytesPer != from.bytesPer)
    {
        ++s.counts.copiesNoDestination;
        return;
    }
    const uint8_t *src = from.pixels ? from.pixels->data() : from.kept->data->data();
    const size_t srcSize = from.pixels ? from.pixels->size() : from.kept->data->size();
    // A copy of the source's rows first: source and destination may be one.
    const uint32_t w = std::min({width, sx < from.stride ? from.stride - sx : 0u, dx < to.stride ? to.stride - dx : 0u});
    const uint32_t h = std::min({height, sy < from.height ? from.height - sy : 0u,
                                 dy < (to.pixels ? 2048u : to.height) ? (to.pixels ? 2048u : to.height) - dy : 0u});
    if (w == 0u || h == 0u)
    {
        ++s.counts.copiesNoSource;
        return;
    }
    const uint32_t bpp = from.bytesPer;
    std::vector<uint8_t> rect(size_t(w) * h * bpp);
    for (uint32_t y = 0; y < h; ++y)
    {
        const size_t at = (size_t(sy + y) * from.stride + sx) * bpp;
        if (at + size_t(w) * bpp <= srcSize)
            std::memcpy(rect.data() + size_t(y) * w * bpp, src + at, size_t(w) * bpp);
    }
    if (to.pixels)
    {
        State::HostImage &image = *to.host;
        if (dy + h > image.height)
        {
            image.height = dy + h;
            image.pixels.resize(size_t(image.height) * to.stride * bpp, uint8_t(0));
        }
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(image.pixels.data() + (size_t(dy + y) * to.stride + dx) * bpp,
                        rect.data() + size_t(y) * w * bpp, size_t(w) * bpp);
        ++image.version;
        image.seq = s.uploadedSeq[dbp] = ++s.sequence;
        ++s.counts.copiesMirrored;
        return;
    }
    auto out = std::make_shared<std::vector<uint8_t>>(*to.kept->data);
    for (uint32_t y = 0; y < h; ++y)
    {
        const size_t at = (size_t(dy + y) * to.stride + dx) * bpp;
        if (at + size_t(w) * bpp <= out->size())
            std::memcpy(out->data() + at, rect.data() + size_t(y) * w * bpp, size_t(w) * bpp);
    }
    const uint32_t texW = to.kept->width, texH = to.kept->height;
    const bool indexed = to.kept->indexed;
    auto resident = s.residentTexture.find(dbp);
    const uint64_t hash = imageHash(out->data(), out->size());
    const uint64_t key = keyOf(hash, dpsm | 0x400u, texW, texH);
    native::Id id = 0u;
    if (const auto known = s.textureIds.find(key); known != s.textureIds.end())
        id = known->second;
    else
    {
        native::Texture record;
        record.width = static_cast<uint16_t>(texW);
        record.height = static_cast<uint16_t>(texH);
        record.format = indexed ? native::PixelFormat::Index8 : native::PixelFormat::Rgba8;
        record.id = id = s.nextTexture++;
        if (dumpingTextures())
            s.keptTextures[record.id] = {out, record.width, record.height, indexed};
        s.texels[record.id] = {out, record.width, record.height, indexed};
        record.texels = std::move(out);
        s.textureIds[key] = record.id;
        s.textureKeys[record.id] = key;
        native::push(std::move(record));
    }
    resident->second.second = id;
    s.uploadedSeq[dbp] = s.takenSeq[dbp] = ++s.sequence;
    ++s.counts.copiesMirrored;
}
} // namespace

void noteLocalCopy(uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t sx, uint32_t sy, uint32_t dbp, uint32_t dbw,
                   uint32_t dpsm, uint32_t dx, uint32_t dy, uint32_t width, uint32_t height)
{
    if (!native::enabled())
        return;
    State &s = state();
    std::lock_guard lock(s.mutex);
    static uint32_t told = 0u;
    if (told < 8u)
    {
        ++told;
        std::fprintf(stderr,
                 "[native-copy] SBP %04x SBW %u SPSM %02x (%u,%u) -> DBP %04x DBW %u DPSM %02x (%u,%u) %ux%u, seq "
                 "%llu\n  source:%s\n  destination:%s\n",
                 sbp, sbw, spsm, sx, sy, dbp, dbw, dpsm, dx, dy, width, height, (unsigned long long)s.sequence,
                 holdingAt(s, sbp).c_str(), holdingAt(s, dbp).c_str());
    }
    s.copiedTo.insert(dbp);
    mirrorCopy(s, sbp, sbw, spsm, sx, sy, dbp, dbw, dpsm, dx, dy, width, height);
}

void hostUpload(uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                const uint8_t *data, uint32_t bytes)
{
    if (!native::enabled())
        return;
    State &s = state();
    std::lock_guard lock(s.mutex);
    const uint32_t psm = dpsm & 0x3Fu, bits = hostBits(psm);
    dbw = std::max(dbw, 1u);
    const uint32_t stride = dbw * 64u;
    const uint64_t need = (uint64_t(width) * height * bits + 7u) / 8u;
    ++s.counts.hostUploads;
    if (bits == 0u || width == 0u || height == 0u || x + width > stride || y + height > 2048u || !data ||
        bytes < need)
    {
        ++s.counts.hostRefused;
        return;
    }
    s.counts.hostBytes += need;
    // What it overwrites: an image of another format whose memory and bits
    // it shares is gone (T4HL and T4HH share a word, not its bits).
    const uint64_t from = uint64_t(dbp) * 256u, to = from + hostSpan(psm, dbw, y + height);
    for (auto it = s.hostImages.begin(); it != s.hostImages.end();)
    {
        const State::HostImage &other = it->second;
        const uint64_t otherFrom = uint64_t(it->first.first) * 256u;
        const uint64_t otherTo = otherFrom + hostSpan(other.psm, other.dbw, other.height);
        const uint32_t a = wordBits(psm), b = wordBits(other.psm);
        const bool shares = otherFrom < to && from < otherTo && (a == 0u || b == 0u || (a & b) != 0u);
        if (shares && !(it->first.first == dbp && other.psm == psm))
            it = s.hostImages.erase(it);
        else
            ++it;
    }
    State::HostImage &image = s.hostImages[{dbp, psm}];
    if (image.dbw != dbw || image.psm != psm)
    {
        image = {};
        image.dbw = dbw;
        image.psm = psm;
    }
    const uint32_t bytesPer = bits >= 16u ? bits / 8u : 1u;
    if (y + height > image.height)
    {
        image.height = y + height;
        image.pixels.resize(size_t(stride) * image.height * bytesPer, 0u);
    }
    for (uint32_t row = 0; row < height; ++row)
    {
        uint8_t *dst = image.pixels.data() + (size_t(y + row) * stride + x) * bytesPer;
        if (bits >= 8u)
            std::memcpy(dst, data + size_t(row) * width * bytesPer, size_t(width) * bytesPer);
        else
            for (uint32_t col = 0; col < width; ++col)
            {
                const size_t i = size_t(row) * width + col; // low nibble first
                dst[col] = (data[i / 2u] >> ((i & 1u) * 4u)) & 0x0Fu;
            }
    }
    ++image.version;
    image.seq = s.uploadedSeq[dbp] = ++s.sequence;
}

bool pipelineOn()
{
    return native::enabled();
}

uint32_t textureAt(uint32_t tbp, uint32_t psm)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    const auto it = s.textureAt.find(tbp);
    return it != s.textureAt.end() && it->second.first == (psm & 0x3Fu) ? it->second.second : 0u;
}

uint32_t paletteAt(uint32_t cbp)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    const auto it = s.paletteAt.find(cbp);
    return it != s.paletteAt.end() ? it->second : 0u;
}

uint32_t targetAt(uint32_t base)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    const auto it = s.targetAt.find(base);
    return it != s.targetAt.end() ? it->second : 0u;
}

uint32_t captureTarget(uint32_t base)
{
    State &s = state();
    std::lock_guard lock(s.mutex);
    const auto at = s.targetAt.find(base);
    const auto declared = s.targets.find(base);
    // The display buffer is the frame shown: natively, the scene as last drawn.
    if (at == s.targetAt.end() ||
        (declared != s.targets.end() && declared->second.role == native::TargetRole::Display))
        return kSceneTarget;
    return at->second;
}

bool readDepth(uint32_t base, uint32_t psm, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
               std::vector<uint8_t> &out)
{
    if (!native::enabled())
        return false;
    psm &= 0x3Fu;
    const uint32_t bytesPerPixel = psm == 0x30u ? 4u : psm == 0x31u ? 3u : (psm == 0x32u || psm == 0x3Au) ? 2u : 0u;
    if (bytesPerPixel == 0u || width == 0u || height == 0u || width > native::kMaxDepthSide ||
        height > native::kMaxDepthSide)
        return false;
    native::DepthBlock block;
    float scale = 0.0f, offset = 0.0f;
    {
        State &s = state();
        std::lock_guard lock(s.mutex);
        ++s.depthReads;
        const auto at = s.depthAt.find(base);
        if (at != s.depthAt.end())
            block.target = at->second;
        else
            ++s.depthNoTarget;
        scale = s.lastDepthScale;
        offset = s.lastDepthOffset;
    }
    block.x = static_cast<int32_t>(x);
    block.y = static_cast<int32_t>(y);
    block.width = width;
    block.height = height;
    // Until there is an answer (or with no depth drawn there): the clear's
    // 0, as the GS's memory would hold.
    thread_local std::vector<float> depth;
    depth.assign(size_t(width) * height, 0.0f);
    if (block.target != 0u && native::readDepth(block, depth))
    {
        State &s = state();
        std::lock_guard lock(s.mutex);
        ++s.depthAnswered;
    }
    const double maximum = bytesPerPixel == 4u ? 4294967295.0 : bytesPerPixel == 3u ? 16777215.0 : 65535.0;
    out.resize(size_t(width) * height * bytesPerPixel);
    for (size_t i = 0; i < depth.size(); ++i)
    {
        // Reversed depth d is (1 - z/w) / 2 of the projection's z; the GS's
        // Z is v22 z/w + v32 through the camera's viewport.
        const float d = depth[i];
        double z = d > 0.0f ? double(scale) * (1.0 - 2.0 * double(d)) + double(offset) : 0.0;
        z = std::clamp(z, 0.0, maximum);
        const uint32_t value = static_cast<uint32_t>(z);
        for (uint32_t b = 0; b < bytesPerPixel; ++b)
            out[i * bytesPerPixel + b] = static_cast<uint8_t>(value >> (8u * b));
    }
    return true;
}
} // namespace rg::nativeproducer

#else

namespace rg::nativeproducer
{
void texture(const TextureLevel &) {}
void palette(const uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t) {}
void target(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, int) {}
void noteWithheld(bool, uint32_t) {}
void notePassed(Passed) {}
void noteSplit() {}
bool takeProduced(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool &, uint64_t &, uint32_t &) { return false; }
void noteContinuedInPacket() {}
bool mesh(const GSNativeMesh &, const GSDrawState *, const uint8_t *, uint32_t) { return false; }
bool pipelineOn() { return false; }
uint32_t captureTarget(uint32_t) { return 0u; }
bool readDepth(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, std::vector<uint8_t> &) { return false; }
void replaceUpload(bool, uint32_t, uint32_t, uint32_t, uint32_t, const uint8_t *, uint32_t) {}
void hostUpload(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, const uint8_t *, uint32_t) {}
void noteLocalCopy(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                   uint32_t, uint32_t) {}
uint32_t textureAt(uint32_t, uint32_t) { return 0u; }
uint32_t paletteAt(uint32_t) { return 0u; }
uint32_t targetAt(uint32_t) { return 0u; }
} // namespace rg::nativeproducer

#endif
