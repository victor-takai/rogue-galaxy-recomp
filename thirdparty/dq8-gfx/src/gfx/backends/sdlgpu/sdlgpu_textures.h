// Texture cache: GS texture regions expanded to RGBA8 GPU textures.
//
// Everything the GS can put in front of the texture unit ends up as one
// R8G8B8A8 texture here -- indexed formats resolved through their CLUT, 16-bit
// formats expanded, TEXA applied. The fragment shader then only has to deal
// with wrapping and filtering, not with PSM variety.
//
// Entries are keyed on everything that changes the texels, and invalidated by
// local-memory page, so transfers refresh only affected texels on next use.
#pragma once

#include "gfx/backends/sdlgpu/sdlgpu_regions.h"
#include "gfx/backends/sdlgpu/sdlgpu_targets.h"
#include "gfx/gs/gs_vram.h"

#include "runtime/gs/gs_types.h"

#include <SDL3/SDL_gpu.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dq8::gfx {

class SdlGpuDevice;
class GsTargetCache;

struct GsTextureKey {
    uint32_t tbp0 = 0u;
    uint32_t tbw = 1u;
    uint32_t psm = 0u;
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t cbp = 0u;
    uint32_t cpsm = 0u;
    uint32_t csm = 0u;
    uint32_t csa = 0u;
    // TA0 | AEM << 8 | TA1 << 16: part of the key because TEXA decides the
    // alpha of every texel in a 16- or 24-bit format.
    uint32_t texa = 0u;
    // CBW | COU << 8 | COV << 16, which addresses a CSM2 palette.
    uint32_t texclut = 0u;

    bool operator==(const GsTextureKey &other) const = default;
};

struct GsTextureKeyHash {
    size_t operator()(const GsTextureKey &key) const;
};

// Content identity independent of the cache address. Separate texel and CLUT
// hashes let replacements match base art across palette animation.
struct GsTextureIdentity {
    uint64_t texels = 0u;
    uint64_t clut = 0u;  // zero when the format is not indexed

    bool operator==(const GsTextureIdentity &other) const = default;
};

struct GsTextureCacheStats {
    uint64_t lookups = 0u;
    uint64_t hits = 0u;
    uint64_t builds = 0u;
    uint64_t invalidations = 0u;
    uint64_t invalidationsFromHostWrite = 0u;
    uint64_t invalidationsFromDraw = 0u;
    // Entries kept and patched in place rather than discarded, and the texels
    // that cost. The difference between this and texelsExpanded is the whole
    // point: a movie writing tiles into a texture should not re-expand the
    // texture.
    uint64_t partialUpdates = 0u;
    uint64_t texelsUpdated = 0u;
    // Whole-texture updates whose raw texels hashed to what the texture
    // already held, so nothing was expanded or uploaded (Rogue Galaxy:
    // added).
    uint64_t unchangedUpdates = 0u;
    uint64_t evictions = 0u;
    uint64_t texelsExpanded = 0u;
    // Textures sourced from a region a render target owns. Correct, but the
    // resolve costs the extra detail a scaled target holds.
    uint64_t renderTargetSources = 0u;
    // Of those, 8-bit indexed ones built on the GPU from the target itself.
    uint64_t expandedFromTargets = 0u;

    double hitRate() const {
        return lookups == 0u ? 0.0
                             : static_cast<double>(hits) / static_cast<double>(lookups);
    }
};

class GsTextureCache {
public:
    GsTextureCache(SdlGpuDevice &device, GsVram &vram, GsTargetCache &targets);
    ~GsTextureCache();

    GsTextureCache(const GsTextureCache &) = delete;
    GsTextureCache &operator=(const GsTextureCache &) = delete;

    void reset();

    // Returns the GPU texture for this draw state's TEX0/TEXA/TEXCLUT, building
    // it from local memory on a miss. Null on failure, with `error` filled.
    // `sampled`: the texels this draw can reach, when the caller can bound
    // them (Rogue Galaxy: added). A texture over pages a render target still
    // owns is then read back only there; the rest waits for a draw that
    // reaches it. Rogue Galaxy declares a 512x512 texture over the 512x416 it
    // uploads, and the lines below lie in a live target.
    SDL_GPUTexture *acquire(const GSDrawState &state, std::string &error, const GsRegion *sampled = nullptr);

    // Why a range of local memory stopped being trustworthy. Kept apart
    // because the two have very different fixes: a host write really did
    // change the texels, whereas a draw only wrote pixels that may share pages
    // with a texture and often leaves its content identical.
    enum class InvalidationSource { HostWrite, Draw };

    // Marks entries stale; repeated tile writes coalesce until the next acquire.
    void invalidate(const GsPageSet &pages, InvalidationSource source);

    const GsTextureCacheStats &stats() const { return m_stats; }
    void resetStats() { m_stats = {}; }
    size_t size() const { return m_entries.size(); }

    // True when the next miss would clear the cache. Callers holding texture
    // pointers -- a queued draw does -- must retire them before that happens.
    bool evictionImminent() const;

    // Called before a cached texture is rewritten in place or released (null:
    // every texture, before the cache is cleared). Host writes no longer
    // flush the queued draws, so a draw queued with the old texels may still
    // be waiting; the backend lands it first. (Rogue Galaxy: added.)
    void setBeforeRewrite(std::function<void(SDL_GPUTexture *)> hook) { m_beforeRewrite = std::move(hook); }

    // Pages a draw state's texture would be built from, so a caller can tell
    // whether queued work still owes it content.
    GsPageSet sourcePagesFor(const GSDrawState &state) const;

    // A region of a draw state's texture by content, as a whole texture's
    // identity is hashed: its texels (indices for an indexed format) and the
    // palette window (Rogue Galaxy: added; sdlgpu_regions.h).
    GsTextureIdentity regionIdentity(const GSDrawState &state, const GsRegion &region);
    // The host's larger versions of regions (sdlgpu_regions.h).
    void setRegionLookup(RegionLookup lookup, RegionPalettes palettes) {
        m_regionLookup = std::move(lookup);
        m_regionPalettes = std::move(palettes);
    }
    bool hasRegionLookup() const { return static_cast<bool>(m_regionLookup); }
    // Any thread: the host's answers changed (a pack switched on or off), so
    // what was kept of them -- the larger pictures made, and the regions
    // asked about and answered "none" -- is let go at the next lookup, and
    // every region is asked about again (Rogue Galaxy: added).
    void invalidateRegions() { m_regionGeneration.fetch_add(1u, std::memory_order_relaxed); }
    // The host's version of what `region` of an indexed texture holds now,
    // for a target at `wantedScale` (its border version with `border`, its
    // shadow version with `shadow`, its version for a draw past 0x80 alpha
    // with `bold`): a
    // texture of its own at `scale` texels per GS texel, or null (none, or
    // not made yet). Kept by content, so a region whose content alternates
    // costs a hash, and the least recently drawn go past a budget. The
    // region is hashed at every call: a memo kept until the texture's next
    // update went stale in the item menu, whose tabs' digits then came out
    // as another picture (Rogue Galaxy: added).
    // `sampled`: the texels the draw shows (within `region`); the rest are
    // handed over transparent: a glyph's sprite spans a texel more than its
    // pixels, and that last row and column, never shown, can hold a
    // descender's end, which traced and bordered stuck out below.
    SDL_GPUTexture *replacement(const GSDrawState &state, const GsRegion &region, const GsRegion &sampled,
                                uint32_t wantedScale, bool border, bool shadow, bool bold, uint32_t &scale,
                                bool &nearest, std::string &error);
    // A larger version of the whole texture `acquire` last gave for `state`,
    // by the hash PCSX2 names it with (a pack made for PCSX2), drawn from at
    // `scale` times the texels. Null while the host is still reading it, or
    // when it has none (Rogue Galaxy: added).
    SDL_GPUTexture *wholeReplacement(const GSDrawState &state, uint32_t &scale, bool &nearest, std::string &error);

private:
    struct Entry {
        SDL_GPUTexture *texture = nullptr;
        // Texture pages and palette pages together, for callers that ask
        // "which pages does this entry read".
        GsPageSet pages;
        // The same two kept apart, because what a write to each one means is
        // different: a texel write changes what the texture shows, a palette
        // write only might. regionForPages maps texel pages; it has no answer
        // for a palette page, and treating "no answer" as "untrustworthy" was
        // dropping textures on every palette rewrite.
        GsPageSet texelPages;
        GsPageSet clutPages;
        GsTextureIdentity identity;
        // Texels a host write has invalidated but which have not been
        // re-expanded yet. Empty means the texture is current.
        GsRegion dirty;
        GsPageSet dirtyPages;
        // Texels over pages a target owned that no draw has reached yet, so
        // they were expanded from local memory without a readback; they join
        // `dirty` when one does (Rogue Galaxy: added).
        GsRegion unresolved;
        // A palette page was written. On next use the palette window is
        // re-hashed; when it is unchanged -- the common case, a game
        // re-uploading the same CLUT every frame -- nothing else happens.
        bool clutDirty = false;
        // Set when a refresh saw the palette actually change and cleared by
        // the next whole-texture upload. The texel hash cannot see a palette
        // change (it hashes indices), so this is what stops an unchanged-
        // indices texture from skipping an upload it needs.
        bool clutUploadPending = false;
        // Created as a colour target too, so it can be built on the GPU.
        bool renderable = false;
        // The invalidate() call that last visited this entry, so an entry
        // over several of a write's pages is visited once (Rogue Galaxy:
        // added).
        uint64_t invalidated = 0u;
        // wholeReplacement(): PCSX2's hash of the texture, valid while
        // `identity.texels` is what it was computed for.
        uint64_t pcsx2 = 0u;
        uint64_t pcsx2For = 0u;
        // And of its palette, valid while `identity.clut` is what it was
        // computed for.
        uint64_t pcsx2Clut = 0u;
        uint64_t pcsx2ClutFor = 0u;
        bool pcsx2ClutValid = false;
    };

    std::function<void(SDL_GPUTexture *)> m_beforeRewrite;
    void beforeRewrite(SDL_GPUTexture *texture) {
        if (m_beforeRewrite)
            m_beforeRewrite(texture);
    }

    bool build(const GsTextureKey &key, Entry &entry, std::string &error, const GsRegion *sampled);
    // `exclude`: texel pages left to the target (restrictToSampled).
    bool resolveSource(const GsTextureKey &key, const GsRegion &region, std::string &error,
                       const GsPageSet *exclude = nullptr);
    // Pages a palette read needs current in local memory.
    void markClutReads(const GsTextureKey &key, GsPageSet &pages) const;
    // An 8-bit indexed texture whose texels a native CT32 target holds is
    // built on the GPU from that target rather than read back; `handled`
    // stays false when the texture is not one of those.
    bool expandFromTarget(const GsTextureKey &key, Entry &entry, GsRegion region, bool &handled,
                          std::string &error);
    // Fills m_clutRgba with the texture's palette as final RGBA.
    void buildPalette(const GsTextureKey &key);
    void checkAgainstCpu(const GsTextureKey &key, Entry &entry, const GsRegion &region);
    // Re-expands and re-uploads only `entry.dirty`.
    bool update(const GsTextureKey &key, Entry &entry, std::string &error, const GsRegion *sampled);
    // The texel pages of a region whose words a target holds and local
    // memory does not (Rogue Galaxy: added).
    GsPageSet staleTexelPages(const GsTextureKey &key, const GsRegion &region) const;
    // When the draw reaches none of the texels those pages hold, `exclude`
    // names them so resolveSource leaves them, and their texels (a bounding
    // box) join `unresolved` for a draw that does reach them (Rogue Galaxy:
    // added).
    void restrictToSampled(const GsTextureKey &key, const GsRegion &region, const GsRegion *sampled,
                           GsPageSet &exclude, GsRegion &unresolved) const;
    // Re-hashes the palette window after a write to a palette page, and
    // schedules a whole-texture re-expand when it really changed.
    void refreshClut(const GsTextureKey &key, Entry &entry);
    // Hash of the palette window this texture's indices resolve into.
    uint64_t hashClutWindow(const GsTextureKey &key) const;
    // Fills m_staging with RGBA8 for one texel rectangle. Returns the hash of
    // the raw texels it read, which is the whole texture's identity only when
    // the region is the whole texture. `hashOnly` reads and hashes them the
    // same way and expands nothing (Rogue Galaxy: added).
    uint64_t expand(const GsTextureKey &key, const GsRegion &region, bool hashOnly = false);
    bool uploadRegion(SDL_GPUTexture *texture, const GsRegion &region, std::string &error);
    // Transfer buffers come from a pool guarded by the device's submission
    // serials: the movie path uploads dozens of regions a frame, and creating
    // a fresh buffer per region spent a quarter of the EE thread inside the
    // driver's allocator.
    SDL_GPUTransferBuffer *takeUploadBuffer(size_t bytes);
    // The buffer was recorded into the device's pending command buffer.
    void recycleUploadBuffer(SDL_GPUTransferBuffer *buffer);
    void releaseUploadPool();
    void markSourcePages(const GsTextureKey &key, GsPageSet &pages) const;
    void markTexelPages(const GsTextureKey &key, GsPageSet &pages) const;
    void markClutPages(const GsTextureKey &key, GsPageSet &pages) const;
    // Which texels of a texture a set of local-memory pages covers.
    GsRegion regionForPages(const GsTextureKey &key, const GsPageSet &pages) const;
    static GsTextureKey keyFor(const GSDrawState &state);

    SdlGpuDevice &m_device;
    GsVram &m_vram;
    GsTargetCache &m_targets;
    std::unordered_map<GsTextureKey, Entry, GsTextureKeyHash> m_entries;
    // sourcePagesFor's last answer: consecutive draws nearly always share a
    // texture, and the page walk runs twice for each of them.
    mutable GsTextureKey m_lastSourceKey{};
    mutable GsPageSet m_lastSourcePages{};
    mutable bool m_lastSourceValid = false;
    // The source pages of every entry, so an invalidation of pages no
    // texture uses skips the walk over the entries (Rogue Galaxy: added).
    GsPageSet m_sourcePagesAll{};
    // The entries by each page they read, so an invalidation visits the
    // entries over the written pages alone: a texture upload touches a
    // handful of pages, and walking the whole map for each of the hundreds
    // a frame was a tenth of the worker in Rogue Galaxy's battles. The map
    // keeps its nodes in place, and entries only ever leave it through
    // reset() (Rogue Galaxy: added).
    std::array<std::vector<Entry *>, kGsPageCount> m_entriesByPage{};
    uint64_t m_invalidateGeneration = 0u;
    std::vector<uint8_t> m_staging;
    // replacement(): the host's lookup, the picture it last handed over, the
    // versions on the GPU by content and the contents it has none for (Rogue
    // Galaxy: added).
    RegionLookup m_regionLookup;
    RegionPalettes m_regionPalettes;
    std::vector<uint8_t> m_regionPixels;
    struct Larger {
        SDL_GPUTexture *texture = nullptr;
        uint32_t scale = 0u;
        uint64_t bytes = 0u;
        uint64_t used = 0u;
        bool nearest = false;
    };
    std::unordered_map<uint64_t, Larger> m_larger;
    std::unordered_set<uint64_t> m_largerMissing;
    std::vector<uint8_t> m_regionFull;
    std::atomic<uint64_t> m_regionGeneration{0u};
    uint64_t m_regionGenerationSeen = 0u;
    uint64_t m_largerBytes = 0u;
    uint64_t m_largerClock = 0u;
    // The palette last asked about, and the answer.
    uint64_t m_lastPalette = 0u;
    bool m_lastPaletteWanted = false;
    bool m_lastPaletteValid = false;
    void releaseLarger();
    // Lets go of the larger versions when the host changed what it answers.
    void syncRegionGeneration();
    // Uploads the host's picture (m_regionPixels, width x height) and keeps it
    // under `id`; null with `error` set if the GPU refuses it.
    SDL_GPUTexture *storeLarger(uint64_t id, uint32_t factor, bool nearest, uint32_t width, uint32_t height,
                                std::string &error);
    // A texture PCSX2 names by its indices (8-bit and 4-bit, in their own
    // words or in a 32-bit one's high bits), with a power-of-two size.
    static bool pcsx2Hashable(const GsTextureKey &key);
    // The hash PCSX2 names that texture by (XXH3-64 of its GS blocks, or of
    // its indices a byte each, row by row); m_wholeBlocks is its scratch.
    uint64_t pcsx2TexelHash(const GsTextureKey &key);
    std::vector<uint8_t> m_wholeBlocks;
    // Expand scratch: raw texels for one row (indices for indexed PSMs, whole
    // words when the indices ride inside a 32-bit layout), and the palette
    // resolved to final RGBA so the texel loop is one lookup per pixel.
    std::vector<uint8_t> m_rawRow;
    std::vector<uint8_t> m_idxRow;
    std::vector<uint32_t> m_clutRgba;

    SdlGpuUploadPool m_uploadPool;
    GsTextureCacheStats m_stats{};
};

} // namespace dq8::gfx
