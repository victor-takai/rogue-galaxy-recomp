#include "vif1_thread.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <string>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <iterator>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <xmmintrin.h>

#include "ThreadNaming.h"
#include "debug/vu1_replay.h"
#include "native_renderer.h"
#include "vu1_native.h"
#include "draw_state_mirror.h"
#include "native_2d.h"
#include "native_effects.h"
#include "native_producer.h"
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"

namespace rg::vif1
{
namespace
{
// Nanoseconds the worker spent inside processVIF1Data, for busyReport().
std::atomic<uint64_t> g_busyNanos{0u};
// And the GIF stage in the GS, for the same line.
std::atomic<uint64_t> g_gifBusyNanos{0u};
// Set on the VIF1 thread: its GS packets go to the GIF stage.
thread_local bool t_onVif1Thread = false;
// The VU1 program the VIF1 thread is running, as DQ8_GFX_PROFILE_CYCLE names
// it for the draw tables (0 outside one, and unless the cycle is armed or
// RG_JITTER is set):
// 0x80000000 | its entry (a pair index) << 16 | a hash of the microcode there.
thread_local uint32_t t_vu1Tag = 0;
// And the camera it ran under (ps2_gs_detail::CameraRegistry), 0 for none.
thread_local uint32_t t_vu1Camera = 0;
// RG_VU1_NATIVE (src/vu1_native.h): while a chunk done natively runs on the
// VU1, its packets are collected (check) or dropped (replace).
thread_local std::vector<std::vector<uint8_t>> *t_vu1Packets = nullptr;
thread_local bool t_dropVu1Packets = false;
// The native pipeline's effects (src/native_effects.h); read at first use,
// after the menu's setting.
bool nativeEffects()
{
    static const bool on = rg::nativefx::enabled();
    return on;
}
// RG_NATIVE_COMPOSITE: stage E's objects in object layers (deliverPacket).
bool composites()
{
    static const bool on = rg::nativefx::compositesBatched();
    return on;
}
// For the VIF code a VU1 program's packets came from (vif1Cursor, patch 0032).
PS2Memory *g_vif1Memory = nullptr;
// The VIF1 thread's list: its bytes and where each piece came from, for the
// EE address of a packet in it (listAddress).
thread_local const uint8_t *t_listData = nullptr;
thread_local size_t t_listSize = 0u;
thread_local const std::vector<std::pair<uint32_t, uint32_t>> *t_listOrigins = nullptr;
// Read when the thread starts, after the menu's native renderer setting
// (static initializers run before the menu hands it over).
rg::vu1native::Mode nativeMode()
{
    static const rg::vu1native::Mode mode = rg::vu1native::modeFromEnvironment();
    return mode;
}
// RG_VU1_NATIVE_CHECK_EVERY=N: check mode compares one chunk in N (each costs
// the VU1's run, the native build and two decodes, on this thread).
const uint32_t g_checkEvery = [] {
    const char *value = std::getenv("RG_VU1_NATIVE_CHECK_EVERY");
    const unsigned long every = value ? std::strtoul(value, nullptr, 10) : 1ul;
    return static_cast<uint32_t>(every == 0ul ? 1ul : every);
}();
// Whether the GPU path takes the chunks slot E clips: only with the native
// renderer, and there under the native pipeline or with
// RG_VU1_CLIP_ON_GPU=1 (=0 keeps them on the VU1 even under the pipeline).
// Otherwise off: the field walk of October 29 lost the near wall's texture
// and part of the ledge with them on the GPU (the clipper's new vertices
// take a depth made of each corner's capped, clamped one, which is not
// linear outside E's planes). The pipeline has no GS to draw the VU1's
// packets: they would reach it as 2D pieces, their texture interpolated
// across the screen without perspective (textures twist in front of the
// camera). Read at first use, after the menu's setting.
struct GpuClips
{
    operator bool() const
    {
        static const bool on = [] {
            if (!rg::nativerenderer::enabled())
                return false;
            const char *gpu = std::getenv("RG_VU1_CLIP_ON_GPU");
            if (gpu && *gpu)
                return std::strcmp(gpu, "0") != 0;
            return rg::nativeproducer::pipelineOn();
        }();
        return on;
    }
} g_gpuClips;
const bool g_tagPrograms = std::getenv("DQ8_GFX_PROFILE_CYCLE") != nullptr || std::getenv("RG_JITTER") != nullptr;
// RG_VU1_CAPTURE=<file> records the VU1 data memory each program starts with
// (16 KiB a start), for tools/vu1/capture_report.py; RG_VU1_CAPTURE_MAX caps
// the starts kept.
FILE *const g_capture = [] {
    const char *path = std::getenv("RG_VU1_CAPTURE");
    return path ? std::fopen(path, "wb") : nullptr;
}();
// After those, starts keep only the matrices (16 quads) for this many more
// DMA lists, to follow objects from one list to the next.
uint32_t g_captureLightLists = [] {
    const char *lists = std::getenv("RG_VU1_CAPTURE_LISTS");
    return lists ? static_cast<uint32_t>(std::strtoul(lists, nullptr, 10)) : 1500u;
}();
uint32_t g_captureLightFrom = 0u;
uint32_t g_captureLeft = [] {
    const char *max = std::getenv("RG_VU1_CAPTURE_MAX");
    return max ? static_cast<uint32_t>(std::strtoul(max, nullptr, 10)) : 4000u;
}();
} // namespace
namespace
{

constexpr uint32_t kVif1Channel = 0x10009000u;
constexpr uint32_t kGifChannel = 0x1000A000u;
constexpr uint32_t kChcrTte = 1u << 6;
constexpr uint32_t kChcrTie = 1u << 7;

// libdma, bound to runtime stubs (config/SCUS_974.90/recomp/main.toml).
constexpr uint32_t kSceDmaSend = 0x0010A040u;
constexpr uint32_t kSceDmaSendN = 0x0010A0A8u;
constexpr uint32_t kSceDmaSendI = 0x0010A120u;
constexpr uint32_t kSceDmaSync = 0x0010A2E0u;
// libgraph stubs that reach the GS paths or wait on them.
constexpr uint32_t kGsWaiters[] = {
    0x00108110u, // sceGsResetGraph
    0x001082B0u, // sceGsResetPath
    0x00108BB8u, // sceGsPutDrawEnv
    0x00108CA0u, // sceGsSetDefDBuff
    0x00108F38u, // sceGsSwapDBuff
    0x00109030u, // sceGsSyncPath
    0x00109488u, // sceGsExecStoreImage
};

// The runtime's address forms for a DMA start (Helpers/Support.h toDmaPhys).
uint32_t toDmaPhys(uint32_t addr)
{
    if ((addr & 0x80000000u) != 0u)
    {
        const uint32_t lower = addr & 0x7FFFFFFFu;
        if (lower >= PS2_SCRATCHPAD_BASE && lower < PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE)
        {
            return lower;
        }
    }
    return addr & 0x1FFFFFFFu;
}

// Bytes from local memory or scratchpad, wrapping as the runtime's DMA does.
void appendBytes(PS2Memory &memory, std::vector<uint8_t> &out, uint32_t address, uint32_t bytes)
{
    const bool scratch = memory.isScratchpad(address);
    uint32_t source = memory.translateAddress(address);
    const uint8_t *base = scratch ? memory.getScratchpad() : memory.getRDRAM();
    const uint32_t size = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
    while (bytes > 0u)
    {
        if (source >= size)
        {
            source = 0u;
        }
        const uint32_t chunk = std::min(bytes, size - source);
        out.insert(out.end(), base + source, base + source + chunk);
        bytes -= chunk;
        source += chunk;
    }
}

// The tags of a VIF1 source chain, the way PS2Memory::writeIORegister
// follows them: refe/cnt/next/ref/refs/call/ret/end, stopping at end, at an
// empty return stack or at a TIE interrupt. The visitor gets each tag's
// address and its payload's address and size, and ends the walk with false.
template <typename Visitor>
void walkChain(PS2Memory &memory, uint32_t tadr, bool tie, Visitor &&visit)
{
    uint32_t asr0 = 0u, asr1 = 0u, asp = 0u;
    for (uint32_t tags = 0u; tags < (1u << 20); ++tags)
    {
        const uint32_t tagAddress = tadr;
        uint64_t tag = 0u;
        try
        {
            const bool scratch = memory.isScratchpad(tagAddress);
            const uint32_t phys = memory.translateAddress(tagAddress);
            const uint8_t *base = scratch ? memory.getScratchpad() : memory.getRDRAM();
            const uint32_t size = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (phys + 16u > size)
            {
                break;
            }
            std::memcpy(&tag, base + phys, sizeof(tag));
        }
        catch (const std::exception &)
        {
            break;
        }
        const uint32_t qwc = static_cast<uint32_t>(tag & 0xFFFFu);
        const uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7u);
        const bool irq = ((tag >> 31) & 0x1u) != 0u;
        const uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFFFu);
        uint32_t data = 0u;
        bool end = false;
        switch (id)
        {
        case 0: data = addr; tadr += 16u; end = true; break;
        case 1: data = tadr + 16u; tadr = data + qwc * 16u; break;
        case 2: data = tadr + 16u; tadr = addr; break;
        case 3:
        case 4: data = addr; tadr += 16u; break;
        case 5:
            data = tadr + 16u;
            if (asp == 0u) { asr0 = data + qwc * 16u; asp = 1u; }
            else if (asp == 1u) { asr1 = data + qwc * 16u; asp = 2u; }
            tadr = addr;
            break;
        case 6:
            data = tadr + 16u;
            if (asp == 2u) { tadr = asr1; asp = 1u; }
            else if (asp == 1u) { tadr = asr0; asp = 0u; }
            else { end = true; }
            break;
        default: data = tadr + 16u; end = true; break;
        }
        if (!visit(tagAddress, data, qwc))
        {
            break;
        }
        if (end || (irq && tie))
        {
            break;
        }
    }
}

// The bytes the chain hands the VIF, into `out`: the tag's upper half first
// when TTE is set, then its payload. The tags are walked once, their spans
// kept aside, and the copy happens once into a buffer reserved for the
// total; the game's lists run to several megabytes a frame, and growing a
// vector through them copied most of that twice, while a second walk for
// the size alone read every tag again (thousands a frame, 2% of the game
// thread in a battle).
void gatherChain(PS2Memory &memory, uint32_t tadr, bool tte, bool tie, std::vector<uint8_t> &out,
                 std::vector<std::pair<uint32_t, uint32_t>> *origins = nullptr)
{
    out.clear();
    // (address, bytes) of each piece, in order. send() runs on the game
    // thread alone.
    thread_local std::vector<std::pair<uint32_t, uint32_t>> spans;
    spans.clear();
    size_t total = 0u;
    walkChain(memory, tadr, tie, [&](uint32_t tagAddress, uint32_t data, uint32_t qwc) {
        if (tte)
        {
            // With TTE every tag supplies two VIFcodes before its payload.
            spans.emplace_back(tagAddress + 8u, 8u);
            total += 8u;
        }
        if (qwc != 0u)
        {
            spans.emplace_back(data, qwc * 16u);
            total += static_cast<size_t>(qwc) * 16u;
        }
        return true;
    });
    try
    {
        // A garbage chain can sum to more than a buffer can hold; the list
        // then ends where the copy stops, like one that cannot be read.
        out.reserve(total);
        for (const auto &[address, bytes] : spans)
        {
            if (origins)
            {
                origins->emplace_back(static_cast<uint32_t>(out.size()), address);
            }
            appendBytes(memory, out, address, bytes);
        }
    }
    catch (const std::exception &)
    {
        // As before: the list ends at the piece that could not be read.
    }
}

// The GS half of the lists. What the VIF1 thread hands the GS -- the
// packets its VU1 programs XGKICK, DIRECT packets, and the textures that
// continue across DIRECTs -- is copied here and run on a thread of its own,
// in the order the VIF1 thread made it, as the PS2's GS draws beside VU1.
// At a boss gate (40 fps, 2026-10-20) the VIF1 thread was 94% busy, a third
// of it in the GS front end and in queueing each of its 42,000 primitives a
// frame for the GS worker, and it waited for that worker at every FINISH;
// copying the packets costs a few percent. RG_GIF_THREAD=0 keeps the GS on
// the VIF1 thread, as before.
//
// Only the GS state lock is shared: the front end calls nothing back on the
// game's side (SIGNAL and FINISH set CSR bits, which are atomic), so waiting
// for this stage cannot deadlock a thread that does not hold that lock.
class GifStage
{
public:
    explicit GifStage(GS &gs) : m_gs(gs)
    {
        m_thread = std::thread([this] { run(); });
        m_id = m_thread.get_id();
    }

    std::thread::id id() const { return m_id; }

    // From the VIF1 thread: a packet or an image continuation, copied (the
    // VU reuses its XGKICK buffer, the list its bytes).
    void packet(const uint8_t *data, uint32_t size) { append(Kind::Packet, data, size); }
    void image(const uint8_t *data, uint32_t size) { append(Kind::Image, data, size); }
    // A strip of a VU1 program done natively (RG_VU1_NATIVE=gpu): its header,
    // then its vertices, for GS::submitNativeMesh.
    void mesh(const GSNativeMesh &mesh)
    {
        m_meshBytes.resize(sizeof(GSNativeMesh::Header) + mesh.vertices.size() * sizeof(GSNativeMesh::Vertex));
        std::memcpy(m_meshBytes.data(), &mesh.header, sizeof(mesh.header));
        if (!mesh.vertices.empty())
        {
            std::memcpy(m_meshBytes.data() + sizeof(mesh.header), mesh.vertices.data(),
                        mesh.vertices.size() * sizeof(GSNativeMesh::Vertex));
        }
        append(Kind::Mesh, m_meshBytes.data(), static_cast<uint32_t>(m_meshBytes.size()));
    }

    // From the VIF1 thread, at the end of each list and as a list fills a
    // buffer: hands over what it staged.
    void publish()
    {
        if (m_staged.empty())
        {
            return;
        }
        const size_t bytes = m_staged.size();
        {
            std::unique_lock lock(m_mutex);
            // A GS that falls behind holds the VIF1 thread back instead of
            // letting the copies pile up.
            m_space.wait(lock, [&] { return m_outstanding == 0u || m_outstanding + bytes <= kCapacity; });
            m_buffers.push_back(std::move(m_staged));
            m_outstanding += bytes;
            ++m_published;
            m_staged.clear();
            if (!m_pool.empty())
            {
                m_staged = std::move(m_pool.back());
                m_pool.pop_back();
            }
        }
        m_work.notify_one();
    }

    // Waits until everything handed over has run; a no-op on this stage's
    // own thread.
    void drain()
    {
        if (std::this_thread::get_id() == m_id)
        {
            return;
        }
        std::unique_lock lock(m_mutex);
        m_idle.wait(lock, [&] { return m_done == m_published; });
    }

    bool busy()
    {
        std::lock_guard lock(m_mutex);
        return m_done != m_published;
    }

    // Runs what is queued, then ends the thread.
    void stop()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_work.notify_all();
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

private:
    enum class Kind : uint32_t
    {
        Packet,
        Image,
        Mesh,
    };
    // A multiple of sixteen bytes, so the data after it stays quadword aligned.
    struct Header
    {
        Kind kind;
        uint32_t size;
        // The VIF1 thread's MXCSR when it handed the packet over, without
        // the sticky exception flags. The GS front end's float work (Q, S
        // and T among them) ran under it: VU1 programs select rounding
        // toward zero, and under this thread's default the logged frames
        // differed in the last bit of those values.
        uint32_t mxcsr;
        // t_vu1Tag when the VIF1 thread handed the packet over.
        uint32_t tag;
        // t_vu1Camera likewise.
        uint32_t camera;
        uint32_t reserved[3];
    };
    static_assert(sizeof(Header) == 32u);

    void append(Kind kind, const uint8_t *data, uint32_t size)
    {
        if (!data || size == 0u)
        {
            return;
        }
        const Header header{kind, size, _mm_getcsr() & ~kMxcsrFlags, t_vu1Tag, t_vu1Camera, {}};
        const auto *raw = reinterpret_cast<const uint8_t *>(&header);
        m_staged.insert(m_staged.end(), raw, raw + sizeof(header));
        m_staged.insert(m_staged.end(), data, data + size);
        m_staged.resize((m_staged.size() + 15u) & ~size_t{15u});
        if (m_staged.size() >= kPublishBytes)
        {
            publish();
        }
    }

    void run()
    {
        ThreadNaming::SetCurrentThreadName("GIF");
        uint32_t mxcsr = _mm_getcsr() & ~kMxcsrFlags;
        for (;;)
        {
            std::vector<uint8_t> buffer;
            {
                std::unique_lock lock(m_mutex);
                m_work.wait(lock, [&] { return !m_buffers.empty() || m_stop; });
                if (m_buffers.empty())
                {
                    return;
                }
                buffer = std::move(m_buffers.front());
                m_buffers.pop_front();
            }
            const auto started = std::chrono::steady_clock::now();
            size_t at = 0u;
            while (at + sizeof(Header) <= buffer.size())
            {
                Header header;
                std::memcpy(&header, buffer.data() + at, sizeof(header));
                const uint8_t *data = buffer.data() + at + sizeof(Header);
                if (header.mxcsr != mxcsr)
                {
                    _mm_setcsr(header.mxcsr);
                    mxcsr = header.mxcsr;
                }
                ps2_gs_detail::t_sourceTag = header.tag;
                ps2_gs_detail::t_sourceCamera = header.camera;
                try
                {
                    if (header.kind == Kind::Packet)
                    {
                        m_gs.processGIFPacket(data, header.size);
                    }
                    else if (header.kind == Kind::Mesh)
                    {
                        std::memcpy(&m_mesh.header, data, sizeof(m_mesh.header));
                        m_mesh.vertices.resize((header.size - sizeof(m_mesh.header)) / sizeof(GSNativeMesh::Vertex));
                        if (!m_mesh.vertices.empty())
                        {
                            std::memcpy(m_mesh.vertices.data(), data + sizeof(m_mesh.header),
                                        m_mesh.vertices.size() * sizeof(GSNativeMesh::Vertex));
                        }
                        m_gs.submitNativeMesh(m_mesh);
                    }
                    else
                    {
                        m_gs.processImageContinuation(data, header.size);
                    }
                }
                catch (const std::exception &error)
                {
                    std::fprintf(stderr, "[gif] packet failed: %s\n", error.what());
                }
                at += sizeof(Header) + ((static_cast<size_t>(header.size) + 15u) & ~size_t{15u});
            }
            g_gifBusyNanos.fetch_add(
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                        .count()),
                std::memory_order_relaxed);
            const size_t bytes = buffer.size();
            {
                std::lock_guard lock(m_mutex);
                m_outstanding -= bytes;
                ++m_done;
                if (m_pool.size() < kPoolSize && buffer.capacity() <= 4u * kPublishBytes)
                {
                    buffer.clear();
                    m_pool.push_back(std::move(buffer));
                }
            }
            m_space.notify_one();
            m_idle.notify_all();
        }
    }

    // A buffer goes over once it holds this much: a frame is several
    // megabytes, so the stage works beside the list, not after it.
    static constexpr size_t kPublishBytes = 128u * 1024u;
    static constexpr size_t kCapacity = 64u * 1024u * 1024u;
    static constexpr size_t kPoolSize = 16u;
    static constexpr uint32_t kMxcsrFlags = 0x3Fu;

    GS &m_gs;
    std::thread m_thread;
    std::thread::id m_id;
    std::mutex m_mutex;
    std::condition_variable m_work, m_space, m_idle;
    std::deque<std::vector<uint8_t>> m_buffers; // guarded by m_mutex
    std::vector<std::vector<uint8_t>> m_pool;   // guarded by m_mutex
    std::vector<uint8_t> m_staged;              // the VIF1 thread's
    std::vector<uint8_t> m_meshBytes;           // the VIF1 thread's
    GSNativeMesh m_mesh;                        // the stage's
    size_t m_outstanding = 0u;
    uint64_t m_published = 0u;
    uint64_t m_done = 0u;
    bool m_stop = false;
};

GifStage *g_stage = nullptr;
// A GS packet on its way, as the memory's GIF callback sends it (below).
void deliverPacket(GS &gs, const uint8_t *data, uint32_t size);
// A native strip likewise, in order with the packets. `vu1Data`: the VU1
// data memory a program's mesh was built from, for the native pipeline's
// mesh draw (rg::nativeproducer::mesh); null for others.
void deliverMesh(GS &gs, const GSNativeMesh &mesh, const uint8_t *vu1Data = nullptr);
// An effect's packets held on the VIF1 thread go on, as the renderer's
// effect or as they are (below).
void flushEffect();
// Stage E's batch, at a list's end or before what is no composite's.
void flushComposites();
// Both dropped, after a flush that threw.
void discardHeld();
// For busyReport's VU1 pair counts.
PS2Runtime *g_runtime = nullptr;

// One handed-over list, with the VU1 D/T-bit enables (VU0 FBRST bits 10 and
// 11) the game thread had when it started it.
struct List
{
    std::vector<uint8_t> data;
    bool dBit = false;
    bool tBit = false;
    // With RG_VU1_CAPTURE: where each piece of `data` came from, as
    // (offset in data, EE address), and the list's number.
    std::vector<std::pair<uint32_t, uint32_t>> origins;
    uint32_t number = 0u;
};

class Vif1Thread
{
public:
    explicit Vif1Thread(PS2Runtime &runtime) : m_runtime(runtime)
    {
        m_thread = std::thread([this] { run(); });
        m_workerId = m_thread.get_id();
    }

    // Runs what is queued, then ends the thread. Before the runtime goes: a
    // list finishing after it posted its interrupt to a freed scheduler.
    void stop()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_work.notify_all();
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

    void submit(List list)
    {
        {
            std::lock_guard lock(m_mutex);
            m_lists.push_back(std::move(list));
            ++m_submitted;
        }
        m_work.notify_one();
    }

    // A buffer for the next list, with the capacity of one that has run:
    // allocating a list's megabytes afresh every time, and freeing them on
    // the other thread, was a measurable part of the game thread's send.
    std::vector<uint8_t> takeBuffer()
    {
        std::lock_guard lock(m_mutex);
        if (m_pool.empty())
        {
            return {};
        }
        std::vector<uint8_t> buffer = std::move(m_pool.back());
        m_pool.pop_back();
        return buffer;
    }

    // MSCAL/MSCNT from the list being run. The runtime's own callbacks read
    // the D/T enables from, and write VPU_STAT into, the context of whatever
    // guest thread the scheduler is running, which from this thread can be
    // mid-change or already freed; these use the list's enables and keep the
    // stop bits for the game thread (takeVpuStat).
    void runVu1(bool resume, uint32_t startPC, uint32_t top, uint32_t itop)
    {
        PS2Memory &memory = m_runtime.memory();
        VU1Interpreter &vu1 = m_runtime.vu1();
        vu1.state().dBitEnabled = m_current.dBit;
        vu1.state().tBitEnabled = m_current.tBit;
        if (g_capture && !resume && (g_captureLeft || g_captureLightLists))
        {
            const bool full = g_captureLeft != 0u;
            if (full)
            {
                --g_captureLeft;
            }
            else if (!g_captureLightFrom)
            {
                g_captureLightFrom = m_current.number;
            }
            else if (m_current.number - g_captureLightFrom >= g_captureLightLists)
            {
                g_captureLightLists = 0u;
                std::fflush(g_capture);
                goto captured;
            }
            const uint32_t tag = programTag(memory, startPC);
            const uint8_t *data = memory.getVU1Data();
            // Where the model matrix (quads 60-63) came from: found in the
            // list's bytes, its piece names the EE address. 0 if not found.
            uint32_t source = 0u;
            const auto &bytes = m_current.data;
            // (The search reads the whole list: full records only.)
            const uint8_t *found = full ? std::search(bytes.data(), bytes.data() + bytes.size(),
                                                      data + 60u * 16u, data + 64u * 16u)
                                        : bytes.data() + bytes.size();
            if (found != bytes.data() + bytes.size())
            {
                const uint32_t offset = static_cast<uint32_t>(found - bytes.data());
                for (const auto &[start, address] : m_current.origins)
                {
                    if (start > offset)
                    {
                        break;
                    }
                    source = address + (offset - start);
                }
            }
            const uint32_t header[7] = {full ? 0x44315556u : 0x4C315556u, tag, startPC, top, itop,
                                        m_current.number, source};
            std::fwrite(header, sizeof(header), 1, g_capture);
            if (full)
            {
                std::fwrite(data, 1, PS2_VU1_DATA_SIZE, g_capture);
            }
            else
            {
                // A hash of the mesh's shape, which does not move with the
                // buffer or the pose: the vertex count (quad 104 x) and the
                // distances between the four section pointers (quad 103). And
                // one of the first 64 quads at TOP.
                auto fnv = [&](uint64_t hash, const uint8_t *at, size_t count) {
                    for (size_t i = 0; i < count; ++i)
                    {
                        hash = (hash ^ at[i]) * 1099511628211ull;
                    }
                    return hash;
                };
                const bool inside = (top + 64u) * 16u <= PS2_VU1_DATA_SIZE;
                uint32_t section[4];
                uint32_t count;
                std::memcpy(section, data + 103u * 16u, sizeof(section));
                std::memcpy(&count, data + 104u * 16u, sizeof(count));
                const uint32_t relative[4] = {count, section[1] - section[0], section[2] - section[1],
                                              section[3] - section[2]};
                const uint64_t shape = fnv(1469598103934665603ull, reinterpret_cast<const uint8_t *>(relative),
                                           sizeof(relative));
                uint64_t whole = 1469598103934665603ull;
                if (inside)
                {
                    whole = fnv(whole, data + top * 16u, 64u * 16u);
                }
                const uint32_t hashes[2] = {static_cast<uint32_t>(shape ^ (shape >> 32)),
                                            static_cast<uint32_t>(whole ^ (whole >> 32))};
                std::fwrite(hashes, sizeof(hashes), 1, g_capture);
                // Projection, view, model, and the clip-space product.
                for (const uint32_t quad : {8u, 24u, 60u, 72u})
                {
                    std::fwrite(data + quad * 16u, 16u, 4u, g_capture);
                }
            }
        }
    captured:
        if (g_tagPrograms)
        {
            // A continuation (MSCNT) is the same program; a DIRECT packet
            // after the program ends carries no tag.
            if (!resume)
            {
                m_programTag = programTag(memory, startPC);
            }
            t_vu1Tag = m_programTag;
            // The matrices this program transforms with: viewport (4-7),
            // projection (8-11) and view (24-27). Quads are 16-byte rows.
            ps2_gs_detail::SceneCamera camera;
            const uint8_t *data = memory.getVU1Data();
            std::memcpy(camera.viewport, data + 4u * 16u, sizeof(camera.viewport));
            std::memcpy(camera.projection, data + 8u * 16u, sizeof(camera.projection));
            std::memcpy(camera.view, data + 24u * 16u, sizeof(camera.view));
            t_vu1Camera = ps2_gs_detail::CameraRegistry::add(camera);
        }
        // RG_VU1_NATIVE: the chunk of a program done natively, built from the
        // data memory before the VU1 changes it. The MSCAL names the program
        // (once per code image and entry), its MSCNTs run the chunks; or the
        // MSCAL is the chunk (drawsAtCall).
        const rg::vu1native::Program *native = nullptr;
        if (nativeMode() != rg::vu1native::Mode::Off)
        {
            if (!resume)
            {
                // identify hashes every candidate piece byte by byte (a
                // quarter of this thread at the boss, 2026-11-01). The game
                // composes its programs from pieces per object, so the code
                // changes often but is one of a few images: each image is
                // hashed once as it changes (a word at a time) and what each
                // entry of it is stays known for the run.
                if (memory.getVU1CodeGeneration() != m_nativeGeneration)
                {
                    m_nativeGeneration = memory.getVU1CodeGeneration();
                    m_codeHash = imageHash(memory.getVU1Code());
                }
                m_nativePC = startPC;
                const uint64_t key = m_codeHash ^ (static_cast<uint64_t>(startPC) * 0x9E3779B97F4A7C15ull);
                auto found = m_identified.find(key);
                if (found == m_identified.end())
                {
                    if (m_identified.size() >= 4096u)
                        m_identified.clear();
                    found = m_identified.emplace(key, rg::vu1native::identify(memory.getVU1Code(), startPC)).first;
                }
                m_nativeProgram = found->second;
            }
            else if (memory.getVU1CodeGeneration() != m_nativeGeneration)
            {
                // An MPG between the MSCAL and its MSCNTs: the code the
                // program was identified in is gone.
                m_nativeProgram = nullptr;
            }
            if (m_nativeProgram && resume != rg::vu1native::drawsAtCall(*m_nativeProgram))
            {
                const bool gpu = nativeMode() == rg::vu1native::Mode::Gpu;
                // The chunks that clip (slot E) go to the GPU with the native
                // renderer, under the pipeline or with RG_VU1_CLIP_ON_GPU=1: the mesh carries E's near
                // and far planes and the shader clips there (gl_ClipDistance),
                // so no driver is left to cut at the eye on its own.
                // Otherwise, and in the packet modes, they go to the VU1.
                const char *reason =
                    rg::vu1native::declineChunk(*m_nativeProgram, memory.getVU1Data(), top, gpu && g_gpuClips);
                if (reason)
                {
                    m_nativeMonitor.declined(*m_nativeProgram, reason);
                }
                else if (nativeMode() == rg::vu1native::Mode::Check && m_checkCount++ % g_checkEvery != 0u)
                {
                    // Not this one: the VU1 runs it, unchecked.
                }
                else
                {
                    native = m_nativeProgram;
                    if (gpu)
                    {
                        rg::vu1native::buildMesh(*native, memory.getVU1Data(), top, m_nativePackets, m_nativeMesh);
                    }
                    else
                    {
                        rg::vu1native::buildChunk(*native, memory.getVU1Data(), top, m_nativePackets);
                    }
                    m_nativeMonitor.native(*native);
                }
            }
            m_nativeMonitor.tick();
        }
        if (!native || (nativeMode() != rg::vu1native::Mode::Skip && nativeMode() != rg::vu1native::Mode::Gpu))
        {
            if (native && nativeMode() == rg::vu1native::Mode::Check)
            {
                m_vu1Packets.clear();
                t_vu1Packets = &m_vu1Packets;
                // Its inputs, to save it if it differs.
                m_checkData.assign(memory.getVU1Data(), memory.getVU1Data() + PS2_VU1_DATA_SIZE);
                m_checkState = vu1.state();
            }
            t_dropVu1Packets = native && nativeMode() == rg::vu1native::Mode::Replace;
            static const bool replay = rg::vu1replay::enabled();
            if (replay)
            {
                rg::vu1replay::begin(resume, startPC, top, itop, m_current.number, memory.getVU1Code(),
                                     memory.getVU1CodeGeneration(), memory.getVU1Data(), vu1.state());
            }
            if (resume)
            {
                vu1.resume(memory.getVU1Code(), PS2_VU1_CODE_SIZE, memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                           m_runtime.gs(), &memory, top, itop, 65536);
            }
            else
            {
                vu1.execute(memory.getVU1Code(), PS2_VU1_CODE_SIZE, memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                            m_runtime.gs(), &memory, startPC, top, itop, 65536);
            }
            if (replay)
            {
                rg::vu1replay::end(vu1.state(), memory.getVU1Data());
            }
            t_vu1Packets = nullptr;
            t_dropVu1Packets = false;
        }
        if (native)
        {
            if (nativeMode() == rg::vu1native::Mode::Check)
            {
                const char *kind = m_nativeMonitor.check(*native, m_vu1Packets, m_nativePackets);
                if (!kind && !rg::vu1native::memoryEffectsMatch(*native, m_checkData.data(), top,
                                                                memory.getVU1Data()))
                {
                    kind = "VU1 memory";
                    m_nativeMonitor.noteKind(*native, kind);
                }
                if (kind)
                {
                    rg::vu1replay::saveChunk(native->name, kind, memory.getVU1Code(), m_nativePC, top, itop,
                                             m_current.number, m_checkData.data(), m_checkState, m_vu1Packets,
                                             vu1.state(), rg::vu1native::drawsAtCall(*native),
                                             memory.getVU1Data());
                }
            }
            else
            {
                for (const auto &packet : m_nativePackets)
                {
                    deliverPacket(m_runtime.gs(), packet.data(), static_cast<uint32_t>(packet.size()));
                }
                if (nativeMode() == rg::vu1native::Mode::Gpu && rg::vu1native::hasMesh(*native))
                {
                    deliverMesh(m_runtime.gs(), m_nativeMesh, memory.getVU1Data());
                }
                // What the VU1 would have left in its memory, where it did not run.
                if (nativeMode() != rg::vu1native::Mode::Replace)
                {
                    rg::vu1native::applyMemoryEffects(*native, memory.getVU1Data(), top);
                }
            }
        }
        t_vu1Tag = 0u;
        t_vu1Camera = 0u;
        m_vpuStat.store((vu1.state().stoppedByD ? 0x0200u : 0u) | (vu1.state().stoppedByT ? 0x0400u : 0u),
                        std::memory_order_release);
    }

    // The tag of the program starting at pair `startPC`: the entry and a hash
    // of the 1 KiB of microcode from it, which names the program whatever else
    // the code memory holds. Kept until the code memory is next written.
    uint32_t programTag(PS2Memory &memory, uint32_t startPC)
    {
        const uint64_t generation = memory.getVU1CodeGeneration();
        if (generation != m_tagGeneration)
        {
            m_tagCache.clear();
            m_tagGeneration = generation;
        }
        const auto [entry, inserted] = m_tagCache.try_emplace(startPC, 0u);
        if (inserted)
        {
            const uint32_t first = std::min<uint32_t>(startPC * 8u, PS2_VU1_CODE_SIZE);
            const uint32_t last = std::min<uint32_t>(first + 1024u, PS2_VU1_CODE_SIZE);
            const uint8_t *code = memory.getVU1Code();
            uint64_t hash = 1469598103934665603ull;
            for (uint32_t at = first; at < last; ++at)
            {
                hash = (hash ^ code[at]) * 1099511628211ull;
            }
            const uint32_t folded = static_cast<uint16_t>(hash ^ (hash >> 16) ^ (hash >> 32) ^ (hash >> 48));
            entry->second = 0x80000000u | ((startPC & 0x7FFu) << 16) | folded;
            static std::atomic<uint32_t> logged{0u};
            if (logged.fetch_add(1u, std::memory_order_relaxed) < 6u)
            {
                std::fprintf(stderr, "[profile] VU1 program: pc 0x%03x, tag %08x\n", startPC & 0x7FFu, entry->second);
            }
        }
        return entry->second;
    }

    // VPU_STAT's VU1 D/T stop bits after the last MSCAL/MSCNT.
    uint32_t takeVpuStat() const { return m_vpuStat.load(std::memory_order_acquire); }

    // Waits until every list handed over has run, and the GS packets they
    // made with them. A no-op on the VIF1 thread itself, which never starts
    // DMA but could reach a hook, and on the GIF stage's.
    void drain()
    {
        if (std::this_thread::get_id() == m_workerId || (g_stage && std::this_thread::get_id() == g_stage->id()))
        {
            return;
        }
        {
            std::unique_lock lock(m_mutex);
            m_idle.wait(lock, [&] { return m_completed == m_submitted; });
        }
        if (g_stage)
        {
            g_stage->drain();
        }
    }

    bool busy()
    {
        {
            std::lock_guard lock(m_mutex);
            if (m_completed != m_submitted)
            {
                return true;
            }
        }
        return g_stage && g_stage->busy();
    }

    // Lists that finished since the last call, for their DMAC completion.
    uint64_t takeFinished()
    {
        std::lock_guard lock(m_mutex);
        const uint64_t finished = m_completed - m_reported;
        m_reported = m_completed;
        return finished;
    }

private:
    void run()
    {
        ThreadNaming::SetCurrentThreadName("VIF1");
        t_onVif1Thread = true;
        PS2Memory &memory = m_runtime.memory();
        for (;;)
        {
            {
                std::unique_lock lock(m_mutex);
                m_work.wait(lock, [&] { return !m_lists.empty() || m_stop; });
                if (m_lists.empty())
                {
                    return;
                }
                m_current = std::move(m_lists.front());
                m_lists.pop_front();
            }
            const auto started = std::chrono::steady_clock::now();
            try
            {
                t_listData = m_current.data.data();
                t_listSize = m_current.data.size();
                t_listOrigins = &m_current.origins;
                if (!m_current.data.empty())
                {
                    memory.processVIF1Data(m_current.data.data(), static_cast<uint32_t>(m_current.data.size()));
                }
            }
            catch (const std::exception &error)
            {
                std::fprintf(stderr, "[vif1] list failed: %s\n", error.what());
            }
            // What the list held goes on before the next list's packets, also
            // when it threw. If that throws too, it is dropped.
            try
            {
                flushEffect();
                flushComposites();
            }
            catch (const std::exception &error)
            {
                std::fprintf(stderr, "[vif1] the list's held packets failed: %s\n", error.what());
                discardHeld();
            }
            // The list's GS packets go over before it counts as done, so a
            // drain that sees it done waits for them too.
            if (g_stage)
            {
                g_stage->publish();
            }
            g_busyNanos.fetch_add(
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                        .count()),
                std::memory_order_relaxed);
            {
                std::lock_guard lock(m_mutex);
                ++m_completed;
                if (m_pool.size() < kPoolSize)
                {
                    m_current.data.clear();
                    m_pool.push_back(std::move(m_current.data));
                }
            }
            m_idle.notify_all();
            // Wakes the EE so it delivers the list's VIFcode interrupt now,
            // not at its next vblank.
            m_runtime.eeScheduler().postEvent(EeEvent{EeEventType::Dmac, 1u, 0u});
        }
    }

    PS2Runtime &m_runtime;
    std::thread m_thread;
    std::thread::id m_workerId;
    std::mutex m_mutex;
    std::condition_variable m_work;
    std::condition_variable m_idle;
    std::deque<List> m_lists;
    List m_current; // the VIF1 thread's
    static constexpr size_t kPoolSize = 8u;
    std::vector<std::vector<uint8_t>> m_pool; // guarded by m_mutex
    std::atomic<uint32_t> m_vpuStat{0u};
    // The VIF1 thread's: the running program's tag, and the tags by entry for
    // the current contents of the code memory (programTag).
    uint32_t m_programTag = 0u;
    // RG_VU1_NATIVE: the program the last MSCAL started, for its MSCNTs, the
    // code image it was found in, and the packets of a chunk.
    const rg::vu1native::Program *m_nativeProgram = nullptr;
    uint64_t m_nativeGeneration = ~0ull;
    uint32_t m_nativePC = 0u;
    // The programs identified, by code image (its hash) and entry.
    std::unordered_map<uint64_t, const rg::vu1native::Program *> m_identified;
    uint64_t m_codeHash = 0u;

    // The 16 KiB of micro memory, a word at a time.
    static uint64_t imageHash(const uint8_t *code)
    {
        uint64_t hash = 0x84222325CBF29CE4ull;
        for (uint32_t i = 0; i < PS2_VU1_CODE_SIZE; i += 8u)
        {
            uint64_t word;
            std::memcpy(&word, code + i, sizeof(word));
            hash = (hash ^ word) * 0x100000001B3ull;
            hash ^= hash >> 29;
        }
        return hash;
    }
    uint32_t m_checkCount = 0u;
    rg::vu1native::Monitor m_nativeMonitor{nativeMode()};
    std::vector<std::vector<uint8_t>> m_nativePackets;
    GSNativeMesh m_nativeMesh;
    std::vector<std::vector<uint8_t>> m_vu1Packets;
    std::vector<uint8_t> m_checkData;
    VU1State m_checkState{};
    uint64_t m_tagGeneration = 0u;
    std::unordered_map<uint32_t, uint32_t> m_tagCache;
    uint64_t m_submitted = 0u;
    uint64_t m_completed = 0u;
    uint64_t m_reported = 0u;
    bool m_stop = false;
};

Vif1Thread *g_thread = nullptr;

// Steps 3b and 6 of docs/NATIVE-PIPELINE.md, under the native pipeline:
// uploads the producer made records of are withheld from the GS, a record
// whose memory the game changed after the texture hook is made again from
// what was sent, and host uploads nobody took and local copies are handed
// to the producer (which keeps them as images) on their way to the GS.
bool uploadsTaken()
{
    static const bool on = rg::nativeproducer::pipelineOn();
    return on;
}

// The transfer registers as the packets set them (this thread's).
thread_local uint64_t t_bitbltbuf = 0u, t_trxpos = 0u, t_trxreg = 0u;

// Step 3b.
struct UploadWithhold
{
    uint32_t transferBytes = 0; // the withheld transfer's data still to come in IMAGE tags
    uint32_t imageBytes = 0;    // an IMAGE tag's data still to come outside its packet
    bool palette = false;
    // The withheld data as sent, against the record's hash of the guest
    // memory it was made from (a record made before the game finished
    // writing that memory is stale): RG_NATIVE_UPLOAD_CHECK.
    std::vector<uint8_t> seen;
    uint64_t expectHash = 0u;
    uint32_t expectBytes = 0u, dbp = 0u, dpsm = 0u, width = 0u, height = 0u;
};
std::atomic<uint64_t> g_uploadsChecked{0}, g_uploadsStale{0};

// On under the pipeline (RG_NATIVE_UPLOAD_CHECK=0 off): a stale record is
// made again from what was sent (rg::nativeproducer::replaceUpload).
bool checkingUploads()
{
    static const bool on = [] {
        const char *v = std::getenv("RG_NATIVE_UPLOAD_CHECK");
        return !(v && *v == '0');
    }();
    return on;
}

// The largest upload checked (RG_NATIVE_UPLOAD_CHECK=all: every one). The
// stale records seen were all palettes (1 KiB) and 64x64 CT32 textures
// (16 KiB); at the boss, checking every upload was about a twentieth of
// this thread and found none.
uint32_t checkedUploadLimit()
{
    static const uint32_t limit = [] {
        const char *v = std::getenv("RG_NATIVE_UPLOAD_CHECK");
        return v && std::strcmp(v, "all") == 0 ? ~0u : 16u * 1024u;
    }();
    return limit;
}
thread_local UploadWithhold t_upload;

// Withheld bytes, for the check; compared when all have come.
void seeWithheld(const uint8_t *data, uint32_t size)
{
    UploadWithhold &u = t_upload;
    if (!checkingUploads() || u.expectBytes == 0u || u.seen.size() >= u.expectBytes)
        return;
    const uint32_t n = std::min<uint32_t>(size, u.expectBytes - static_cast<uint32_t>(u.seen.size()));
    u.seen.insert(u.seen.end(), data, data + n);
    if (u.seen.size() < u.expectBytes)
        return;
    g_uploadsChecked.fetch_add(1u, std::memory_order_relaxed);
    if (rg::nativeproducer::uploadHash(u.seen.data(), u.expectBytes) != u.expectHash)
    {
        const uint64_t stale = g_uploadsStale.fetch_add(1u, std::memory_order_relaxed);
        if (stale < 8u)
            std::fprintf(stderr,
                         "[native-upload] stale record: DBP %04x DPSM %02x %ux%u%s, %u bytes (sent differs from the "
                         "record's guest memory): made again from what was sent\n",
                         u.dbp, u.dpsm, u.width, u.height, u.palette ? " (palette)" : "", u.expectBytes);
        rg::nativeproducer::replaceUpload(u.palette, u.dbp, u.dpsm, u.width, u.height, u.seen.data(),
                                          u.expectBytes);
    }
    u.expectBytes = 0u;
}

uint32_t transferBits(uint32_t psm)
{
    switch (psm & 0x3Fu)
    {
    case 0x00u: return 32u; // CT32
    case 0x01u: return 24u; // CT24
    case 0x02u: case 0x0Au: return 16u; // CT16, CT16S
    case 0x13u: return 8u;  // T8
    case 0x14u: return 4u;  // T4
    default: return 0u;
    }
}

// What of the packet goes to the GS, into `kept`; false: all of it, as it is.
bool withholdUploads(const uint8_t *data, uint32_t size, std::vector<uint8_t> &kept)
{
    namespace producer = rg::nativeproducer;
    if (!uploadsTaken())
        return false;
    kept.clear();
    bool changed = false;
    uint32_t pos = 0u;
    if (t_upload.imageBytes > 0u)
    {
        // A withheld image's data at the packet's start, not behind a tag.
        const uint32_t n = std::min(t_upload.imageBytes, size);
        t_upload.imageBytes -= n;
        seeWithheld(data, n);
        pos = n;
        changed = true;
        producer::noteContinuedInPacket();
    }
    while (pos + 16u <= size)
    {
        uint64_t lo = 0u, hi = 0u;
        std::memcpy(&lo, data + pos, 8u);
        std::memcpy(&hi, data + pos + 8u, 8u);
        const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
        const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
        uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
        nreg = nreg ? nreg : 16u;
        const uint64_t body = flg == 0u   ? uint64_t(nloop) * nreg * 16u
                              : flg == 1u ? (uint64_t(nloop) * nreg + 1u) / 2u * 16u
                                          : uint64_t(nloop) * 16u;
        const uint32_t end = static_cast<uint32_t>(std::min<uint64_t>(size, pos + 16u + body));
        bool drop = false;
        if (flg == 0u && nreg == 1u && (hi & 0xFu) == 0xEu)
        {
            // A+D: a transfer's registers alone, its TRXDIR host to local?
            uint64_t bitbltbuf = 0u, trxreg = 0u, trxdir = ~0ull;
            uint32_t others = 0u;
            for (uint32_t at = pos + 16u; at + 16u <= end; at += 16u)
            {
                uint64_t value = 0u;
                std::memcpy(&value, data + at, 8u);
                const uint8_t reg = data[at + 8u];
                if (reg == 0x50u)
                    bitbltbuf = value;
                else if (reg == 0x52u)
                    trxreg = value;
                else if (reg == 0x53u)
                    trxdir = value;
                else if (reg != 0x51u)
                    ++others;
            }
            if (trxdir != ~0ull)
            {
                t_upload.transferBytes = 0u;
                const uint32_t dbp = static_cast<uint32_t>((bitbltbuf >> 32) & 0x3FFFu);
                const uint32_t dbw = std::max<uint32_t>(static_cast<uint32_t>((bitbltbuf >> 48) & 0x3Fu), 1u);
                const uint32_t dpsm = static_cast<uint32_t>((bitbltbuf >> 56) & 0x3Fu);
                const uint32_t rrw = static_cast<uint32_t>(trxreg & 0xFFFu);
                const uint32_t rrh = static_cast<uint32_t>((trxreg >> 32) & 0xFFFu);
                const uint32_t bits = transferBits(dpsm);
                bool palette = false;
                uint64_t hash = 0u;
                uint32_t hashedBytes = 0u;
                if ((trxdir & 3u) != 0u || bits == 0u)
                    ; // not an upload this takes
                else if (!producer::takeProduced(dbp, dbw, dpsm, rrw, rrh, palette, hash, hashedBytes))
                    producer::notePassed(producer::Passed::NotProduced);
                else
                {
                    t_upload.transferBytes = (rrw * rrh * bits / 8u + 15u) & ~15u;
                    t_upload.palette = palette;
                    t_upload.seen.clear();
                    t_upload.expectHash = hash;
                    t_upload.expectBytes = hashedBytes <= checkedUploadLimit() ? hashedBytes : 0u;
                    t_upload.dbp = dbp;
                    t_upload.dpsm = dpsm;
                    t_upload.width = rrw;
                    t_upload.height = rrh;
                    drop = true;
                    if (others != 0u)
                    {
                        // The block's other registers, as a block of their own.
                        const uint64_t tag = (lo & ~uint64_t(0x7FFFu)) | others;
                        const size_t at = kept.size();
                        kept.resize(at + 16u);
                        std::memcpy(kept.data() + at, &tag, 8u);
                        std::memcpy(kept.data() + at + 8u, &hi, 8u);
                        for (uint32_t q = pos + 16u; q + 16u <= end; q += 16u)
                            if (data[q + 8u] < 0x50u || data[q + 8u] > 0x53u)
                                kept.insert(kept.end(), data + q, data + q + 16u);
                        producer::noteSplit();
                    }
                }
            }
        }
        else if (flg >= 2u && t_upload.transferBytes > 0u)
        {
            // The withheld transfer's data, as many IMAGE tags as it takes.
            drop = true;
            const uint32_t taken = static_cast<uint32_t>(std::min<uint64_t>(body, t_upload.transferBytes));
            t_upload.transferBytes -= taken;
            seeWithheld(data + pos + 16u, std::min<uint32_t>(taken, end - (pos + 16u)));
            t_upload.imageBytes = static_cast<uint32_t>(pos + 16u + body - end);
            if (t_upload.transferBytes == 0u)
                producer::noteWithheld(t_upload.palette, taken);
        }
        if (drop)
            changed = true;
        else
            kept.insert(kept.end(), data + pos, data + end);
        pos = end;
    }
    if (changed && pos < size)
        kept.insert(kept.end(), data + pos, data + size);
    return changed;
}

// An image continuation's bytes that belong to a withheld upload; the rest,
// if any, goes on.
uint32_t withheldContinuation(const uint8_t *data, uint32_t size)
{
    if (t_upload.imageBytes == 0u)
        return 0u;
    const uint32_t n = std::min(t_upload.imageBytes, size);
    t_upload.imageBytes -= n;
    seeWithheld(data, n);
    return n;
}

// A host upload nobody took (no record withheld it), its pixels gathered
// from the IMAGE data that follows (in the packet or continuing past it)
// for the producer, which keeps what it leaves in GS memory
// (rg::nativeproducer::hostUpload). The upload still goes to the GS.
struct HostCapture
{
    bool active = false;
    uint64_t bitbltbuf = 0u, trxpos = 0u, trxreg = 0u;
    uint32_t need = 0u;
    std::vector<uint8_t> data;
};
thread_local HostCapture t_capture;

uint32_t captureBits(uint32_t psm)
{
    switch (psm & 0x3Fu)
    {
    case 0x00u: return 32u;                     // CT32
    case 0x02u: case 0x0Au: return 16u;         // CT16, CT16S
    case 0x13u: case 0x1Bu: return 8u;          // T8, T8H
    case 0x14u: case 0x24u: case 0x2Cu: return 4u; // T4, T4HL, T4HH
    default: return 0u;
    }
}

void startCapture()
{
    HostCapture &c = t_capture;
    c.active = false;
    c.data.clear();
    const uint32_t bits = captureBits(static_cast<uint32_t>(t_bitbltbuf >> 56));
    const uint64_t need = (uint64_t(t_trxreg & 0xFFFu) * ((t_trxreg >> 32) & 0xFFFu) * bits + 7u) / 8u;
    if (bits == 0u || need == 0u || need > (16u << 20))
        return;
    c.active = true;
    c.bitbltbuf = t_bitbltbuf;
    c.trxpos = t_trxpos;
    c.trxreg = t_trxreg;
    c.need = static_cast<uint32_t>(need);
    c.data.reserve(c.need);
}

// IMAGE bytes of the upload being captured; whatever is past it is not.
void feedCapture(const uint8_t *data, uint32_t size)
{
    HostCapture &c = t_capture;
    if (!c.active)
        return;
    const uint32_t n = std::min<uint32_t>(size, c.need - static_cast<uint32_t>(c.data.size()));
    c.data.insert(c.data.end(), data, data + n);
    if (c.data.size() < c.need)
        return;
    c.active = false;
    const uint64_t b = c.bitbltbuf;
    rg::nativeproducer::hostUpload(
        static_cast<uint32_t>((b >> 32) & 0x3FFFu), static_cast<uint32_t>((b >> 48) & 0x3Fu),
        static_cast<uint32_t>((b >> 56) & 0x3Fu), static_cast<uint32_t>((c.trxpos >> 32) & 0x7FFu),
        static_cast<uint32_t>((c.trxpos >> 48) & 0x7FFu), static_cast<uint32_t>(c.trxreg & 0xFFFu),
        static_cast<uint32_t>((c.trxreg >> 32) & 0xFFFu), c.data.data(), static_cast<uint32_t>(c.data.size()));
    c.data.clear();
}

void transferRegister(uint8_t reg, uint64_t value)
{
    switch (reg)
    {
    case 0x50u: t_bitbltbuf = value; break;
    case 0x51u: t_trxpos = value; break;
    case 0x52u: t_trxreg = value; break;
    case 0x53u: // TRXDIR
        if ((value & 3u) == 0u)
            startCapture();
        else
            t_capture.active = false;
        if ((value & 3u) == 2u)
        {
            const uint64_t b = t_bitbltbuf;
            rg::nativeproducer::noteLocalCopy(
                unsigned(b & 0x3FFFu), unsigned((b >> 16) & 0x3Fu), unsigned((b >> 24) & 0x3Fu),
                unsigned(t_trxpos & 0x7FFu), unsigned((t_trxpos >> 16) & 0x7FFu), unsigned((b >> 32) & 0x3FFFu),
                unsigned((b >> 48) & 0x3Fu), unsigned((b >> 56) & 0x3Fu), unsigned((t_trxpos >> 32) & 0x7FFu),
                unsigned((t_trxpos >> 48) & 0x7FFu), unsigned(t_trxreg & 0xFFFu), unsigned((t_trxreg >> 32) & 0xFFFu));
        }
        break;
    default:
        break;
    }
}

// A packet on its way to the GS: its transfers, for the producer.
void watchTransfers(const uint8_t *data, uint32_t size)
{
    uint32_t pos = 0u;
    while (pos + 16u <= size)
    {
        uint64_t lo = 0u, hi = 0u;
        std::memcpy(&lo, data + pos, 8u);
        std::memcpy(&hi, data + pos + 8u, 8u);
        const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
        const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
        uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
        nreg = nreg ? nreg : 16u;
        pos += 16u;
        if (flg == 0u)
        {
            // PACKED: a qword a register; only A+D reaches the transfer ones.
            for (uint32_t i = 0; i < nloop * nreg && pos + 16u <= size; ++i, pos += 16u)
                if (((hi >> ((i % nreg) * 4u)) & 0xFu) == 0xEu)
                {
                    uint64_t value = 0u;
                    std::memcpy(&value, data + pos, 8u);
                    transferRegister(data[pos + 8u], value);
                }
        }
        else if (flg == 1u)
        {
            pos += ((nloop * nreg * 8u) + 15u) & ~15u; // REGLIST: no A+D
        }
        else
        {
            const uint32_t bytes = std::min<uint32_t>(nloop * 16u, size - std::min(size, pos));
            feedCapture(data + pos, bytes);
            pos += bytes;
        }
    }
}

// (native2D, below.)
bool native2D();

// Step 6 of docs/NATIVE-PIPELINE.md, finished: under the pipeline (with its
// 2D on), what is left of a packet after the converter and the upload
// withholding no longer goes to the GS. The renderer draws from records, the
// draw-state mirror and the converter follow the packets, and the GS draws
// nothing. What the game still sees of the GS is SIGNAL, FINISH and LABEL in
// the privileged registers (CSR, SIGLBLID), set here as the GS would.
// RG_NATIVE_ABSORB=0 sends it to the GS as before.
bool absorbing()
{
    static const bool on = [] {
        const char *value = std::getenv("RG_NATIVE_ABSORB");
        return uploadsTaken() && native2D() && !(value && std::strcmp(value, "0") == 0);
    }();
    return on;
}
std::atomic<uint64_t> g_absorbedPackets{0u};

void absorbRegister(uint8_t reg, uint64_t value)
{
    GSRegisters *regs = g_runtime ? &g_runtime->memory().gs() : nullptr;
    if (!regs)
        return;
    switch (reg)
    {
    case 0x60u: // SIGNAL
    {
        const uint32_t id = static_cast<uint32_t>(value), mask = static_cast<uint32_t>(value >> 32);
        uint32_t lo = static_cast<uint32_t>(regs->siglblid);
        lo = (lo & ~mask) | (id & mask);
        regs->siglblid = (regs->siglblid & 0xFFFFFFFF00000000ull) | lo;
        regs->csr.fetch_or(0x1u);
        break;
    }
    case 0x61u: // FINISH
        regs->csr.fetch_or(0x2u);
        break;
    case 0x62u: // LABEL
    {
        const uint32_t id = static_cast<uint32_t>(value), mask = static_cast<uint32_t>(value >> 32);
        uint32_t hi = static_cast<uint32_t>(regs->siglblid >> 32);
        hi = (hi & ~mask) | (id & mask);
        regs->siglblid = (uint64_t(hi) << 32) | (regs->siglblid & 0xFFFFFFFFull);
        break;
    }
    default:
        break;
    }
}

// The packet's SIGNAL, FINISH and LABEL (A+D writes; the GIF has no other
// way to them), as the GS would take them.
void absorb(const uint8_t *data, uint32_t size)
{
    g_absorbedPackets.fetch_add(1u, std::memory_order_relaxed);
    uint32_t pos = 0u;
    while (pos + 16u <= size)
    {
        uint64_t lo = 0u, hi = 0u;
        std::memcpy(&lo, data + pos, 8u);
        std::memcpy(&hi, data + pos + 8u, 8u);
        const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
        const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
        uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
        nreg = nreg ? nreg : 16u;
        pos += 16u;
        if (flg == 0u)
        {
            for (uint32_t i = 0; i < nloop * nreg && pos + 16u <= size; ++i, pos += 16u)
                if (((hi >> ((i % nreg) * 4u)) & 0xFu) == 0xEu)
                {
                    uint64_t value = 0u;
                    std::memcpy(&value, data + pos, 8u);
                    absorbRegister(data[pos + 8u], value);
                }
        }
        else if (flg == 1u)
            pos += ((nloop * nreg * 8u) + 15u) & ~15u;
        else
            pos += std::min<uint32_t>(nloop * 16u, size - std::min(size, pos));
    }
}

// A packet to the GS as it is.
void sendRaw(GS &gs, const uint8_t *data, uint32_t size)
{
    if (uploadsTaken())
    {
        thread_local std::vector<uint8_t> kept;
        if (withholdUploads(data, size, kept))
        {
            if (kept.empty())
                return;
            data = kept.data();
            size = static_cast<uint32_t>(kept.size());
        }
        watchTransfers(data, size);
        if (absorbing())
        {
            rg::drawstate::observe(data, size);
            absorb(data, size);
            return;
        }
    }
    // The native pipeline's draw-state mirror follows every packet, in the
    // order the GS gets them.
    if (g_stage && t_onVif1Thread)
    {
        rg::drawstate::observe(data, size);
        g_stage->packet(data, size);
        return;
    }
    if (g_stage)
    {
        // The lists in flight too, not only what they handed the stage:
        // every DMA start drains them already, this keeps it so.
        g_thread->drain();
    }
    rg::drawstate::observe(data, size);
    ps2_gs_detail::t_sourceTag = t_onVif1Thread ? t_vu1Tag : 0u;
    ps2_gs_detail::t_sourceCamera = t_onVif1Thread ? t_vu1Camera : 0u;
    gs.processGIFPacket(data, size);
}

// A native strip to the GS, in order with the packets.
void meshToGs(GS &gs, const GSNativeMesh &mesh)
{
    if (g_stage && t_onVif1Thread)
    {
        g_stage->mesh(mesh);
        return;
    }
    if (g_stage)
    {
        g_thread->drain();
    }
    gs.submitNativeMesh(mesh);
}

// The native pipeline's 2D (src/native_2d.h); read at first use, after the
// menu's setting.
bool native2D()
{
    static const bool on = rg::native2d::enabled();
    return on;
}
std::atomic<uint64_t> g_packetsConverted{0u}, g_packetsRefused{0u}, g_piecesMade{0u}, g_piecesToGs{0u};

// Step 5b: a converter mesh as the producer's window-space draw, under the
// draw state the packets before it left (the PRIM write just before it
// among them); one it refuses goes to the GS.
void deliverPiece(GS &gs, const rg::native2d::Piece &piece)
{
    if (!t_onVif1Thread)
    {
        g_thread->drain(); // the lists in flight first, as for the GS
    }
    const GSDrawState state = rg::drawstate::current();
    if (rg::nativeproducer::piece(piece.mesh, &state, piece.texelUvs))
    {
        ++g_piecesMade;
        return;
    }
    ++g_piecesToGs;
    meshToGs(gs, piece.mesh);
}

// A packet on to the GS. Under the native pipeline's 2D its primitives
// become the producer's draws and only the rest goes on (src/native_2d.h).
void sendPacket(GS &gs, const uint8_t *data, uint32_t size)
{
    if (native2D() && data && size >= 16u)
    {
        thread_local std::vector<rg::native2d::Piece> pieces;
        const rg::native2d::Span one{data, size};
        std::string why;
        if (rg::native2d::build(&one, 1u, pieces, why))
        {
            const bool anyMesh = std::any_of(pieces.begin(), pieces.end(),
                                             [](const rg::native2d::Piece &p) { return !p.isPacket(); });
            if (!anyMesh)
            {
                sendRaw(gs, data, size); // the converter has seen it
                return;
            }
            for (const rg::native2d::Piece &piece : pieces)
            {
                if (piece.isPacket())
                    sendRaw(gs, piece.bytes(), piece.size());
                else
                    deliverPiece(gs, piece);
            }
            const uint64_t count = ++g_packetsConverted;
            if (count == 1u || count % 100000u == 0u)
                std::fprintf(stderr, "[native2d] packets converted: %llu (pieces made %llu, to the GS %llu), %llu refused\n",
                             static_cast<unsigned long long>(count),
                             static_cast<unsigned long long>(g_piecesMade.load()),
                             static_cast<unsigned long long>(g_piecesToGs.load()),
                             static_cast<unsigned long long>(g_packetsRefused.load()));
            return;
        }
        ++g_packetsRefused;
        static std::mutex toldMutex;
        static std::vector<std::string> told;
        std::lock_guard lock(toldMutex);
        if (told.size() < 8u && std::find(told.begin(), told.end(), why) == told.end())
        {
            told.push_back(why);
            std::fprintf(stderr, "[native2d] a packet stays a packet: %s\n", why.c_str());
        }
        rg::native2d::observe(data, size);
    }
    sendRaw(gs, data, size);
}

// The packets of an effect the producer may draw (src/native_effects.h),
// held on the VIF1 thread until something else goes to the GS (an effect's
// packets come in one run) or the list ends.
struct HeldEffect
{
    rg::nativefx::Effect effect = rg::nativefx::Effect::None;
    uint32_t unit = 0u; // the colour filter's colour
    GS *gs = nullptr;
    std::vector<std::vector<uint8_t>> packets;
};
thread_local HeldEffect t_held;
std::atomic<uint64_t> g_effectsNative{0u};
std::atomic<uint64_t> g_effectsAsPackets{0u};

// The EE address of a packet in the VIF1 thread's list, or ~0u.
uint32_t listAddress(const uint8_t *data)
{
    if (!t_listData || !t_listOrigins || data < t_listData || data >= t_listData + t_listSize)
    {
        return ~0u;
    }
    const uint32_t offset = static_cast<uint32_t>(data - t_listData);
    auto it = std::upper_bound(t_listOrigins->begin(), t_listOrigins->end(), offset,
                               [](uint32_t value, const auto &piece) { return value < piece.first; });
    if (it == t_listOrigins->begin())
    {
        return ~0u;
    }
    --it;
    return it->second + (offset - it->first);
}

// An effect built from its packets, as the producer's draws: then its
// packets go on with their kicks withheld (the registers they set still
// reach the GS and the draw-state mirror). False, with `why`, leaves the
// packets to go on as they are.
bool drawEffect(rg::nativefx::Effect kind, const std::vector<std::vector<uint8_t>> &packets, uint32_t unit,
                GS &gs, std::string &why)
{
    thread_local GSNativeEffect effect;
    thread_local std::vector<std::vector<uint8_t>> withheld;
    if (!rg::nativefx::build(kind, packets, effect, withheld, why, unit))
        return false;
    rg::nativeproducer::noteEffect(effect);
    if (!rg::nativeproducer::effect(effect))
    {
        why = "the producer cannot draw it (its stats line says why)";
        return false;
    }
    for (const std::vector<uint8_t> &packet : withheld)
    {
        sendPacket(gs, packet.data(), static_cast<uint32_t>(packet.size()));
    }
    return true;
}

void countEffect(bool drawn, rg::nativefx::Effect kind, const std::string &why,
                 const std::vector<std::vector<uint8_t>> &packets)
{
    if (drawn)
    {
        const uint64_t count = ++g_effectsNative;
        if (count == 1u || count % 3600u == 0u)
        {
            std::fprintf(stderr, "[nativefx] %llu drawn by the renderer, %llu as packets\n",
                         static_cast<unsigned long long>(count),
                         static_cast<unsigned long long>(g_effectsAsPackets.load()));
        }
        return;
    }
    ++g_effectsAsPackets;
    static std::mutex toldMutex;
    static std::vector<std::string> told;
    const std::string line = std::string(rg::nativefx::name(kind)) + ": " + why;
    std::lock_guard lock(toldMutex);
    if (told.size() < 16u && std::find(told.begin(), told.end(), line) == told.end())
    {
        told.push_back(line);
        std::fprintf(stderr, "[nativefx] an effect stays packets: %s\n", line.c_str());
        static const bool dump = std::getenv("RG_NATIVE_EFFECTS_TRACE") != nullptr;
        if (dump)
            rg::nativefx::dumpPackets("nativefx refused", packets);
    }
}

void flushEffect()
{
    if (t_held.effect == rg::nativefx::Effect::None)
    {
        return;
    }
    GS &gs = *t_held.gs;
    std::string why;
    const bool drawn = drawEffect(t_held.effect, t_held.packets, t_held.unit, gs, why);
    if (!drawn)
    {
        for (const std::vector<uint8_t> &packet : t_held.packets)
        {
            sendPacket(gs, packet.data(), static_cast<uint32_t>(packet.size()));
        }
    }
    countEffect(drawn, t_held.effect, why, t_held.packets);
    t_held.effect = rg::nativefx::Effect::None;
    t_held.unit = 0u;
    t_held.packets.clear();
}

// RG_NATIVE_COMPOSITE: stage E's objects, each drawn into the offscreen
// buffer (its head: a clear of its box, its model) and then laid over the
// frame (its tail), drawn by the renderer into object layers of its own
// (GSNativeEffect::kRouteLayer): the head's draws into the buffer go to the
// object's layer, the tail's sprites become a kObjectTail from that layer,
// their kicks withheld, and the waiting tails are drawn together
// (kComposite) before anything else reaches the GS. Uploads and TEXFLUSHes
// between objects go on: the layers are not GS memory. So the GS's buffer is
// never drawn, and the renderer switches target twice a run of objects
// instead of twice an object.
struct CompositeBatch
{
    enum class State : uint8_t
    {
        Outside,
        Head,
        Tail,
    };
    State state = State::Outside;
    uint32_t unit = 0u;
    bool decided = false;   // the head's box known and its packets sent
    bool routed = false;    // its draws go to a layer, its tail held
    bool waiting = false;   // tails sent that no kComposite has drawn yet
    bool uploading = false; // an upload's data goes on too
    GS *gs = nullptr;
    rg::nativefx::Box box;
    std::vector<std::vector<uint8_t>> head; // the head's packets until its box is known
    std::vector<std::vector<uint8_t>> tail;
};
thread_local CompositeBatch t_batch;
std::atomic<uint64_t> g_compositeUnits{0u}, g_compositeRouted{0u}, g_compositeRuns{0u}, g_compositeRefused{0u};

// A layer command or a tail to the producer; false when it refused it.
bool sendEffect(GS &, const GSNativeEffect &effect)
{
    rg::nativeproducer::noteEffect(effect);
    return rg::nativeproducer::effect(effect);
}

void sendCommand(GS &gs, uint32_t command)
{
    thread_local GSNativeEffect effect;
    effect = GSNativeEffect{};
    effect.command = command;
    sendEffect(gs, effect);
}

// The waiting tails drawn, before what comes next.
void compositeRun()
{
    CompositeBatch &batch = t_batch;
    if (!batch.waiting)
        return;
    batch.waiting = false;
    ++g_compositeRuns;
    sendCommand(*batch.gs, GSNativeEffect::kComposite);
}

// The head's box is known (or will not be): routed to a layer with it, or,
// without one, drawn as the game draws it, after the waiting tails.
void decideHead()
{
    CompositeBatch &batch = t_batch;
    batch.routed = rg::nativefx::clearBox(batch.head, batch.box);
    if (batch.routed)
    {
        thread_local GSNativeEffect route;
        route = GSNativeEffect{};
        route.command = GSNativeEffect::kRouteLayer;
        route.bufferCount = 1u;
        route.buffers[0].base = batch.box.base;
        route.buffers[0].fbw = batch.box.fbw;
        route.box[0] = batch.box.x0;
        route.box[1] = batch.box.y0;
        route.box[2] = batch.box.x1;
        route.box[3] = batch.box.y1;
        sendEffect(*batch.gs, route);
        ++g_compositeRouted;
    }
    else
    {
        compositeRun();
    }
    for (const std::vector<uint8_t> &packet : batch.head)
        sendPacket(*batch.gs, packet.data(), static_cast<uint32_t>(packet.size()));
    batch.head.clear();
    batch.decided = true;
    const uint64_t units = ++g_compositeUnits;
    if (units == 1u || units % 5000u == 0u)
        std::fprintf(stderr, "[composite] %llu objects, %llu in layers, %llu runs drawn, %llu tails refused\n",
                     static_cast<unsigned long long>(units), static_cast<unsigned long long>(g_compositeRouted.load()),
                     static_cast<unsigned long long>(g_compositeRuns.load()),
                     static_cast<unsigned long long>(g_compositeRefused.load()));
}

// A routed object's tail, whole: its sprites to the renderer, from the
// layer, and its packets to the GS with their kicks withheld. Refused, the
// object's layer goes back into the GS's buffer first (kObjectTail without
// sprites) and the packets as they are.
void finishTail()
{
    CompositeBatch &batch = t_batch;
    if (batch.tail.empty())
        return;
    GS &gs = *batch.gs;
    thread_local GSNativeEffect effect;
    thread_local std::vector<std::vector<uint8_t>> withheld;
    std::string why;
    bool ok = rg::nativefx::build(rg::nativefx::Effect::CompositeTail, batch.tail, effect, withheld, why);
    if (ok)
    {
        bool fromLayer = false;
        for (uint32_t i = 0; i < effect.bufferCount; ++i)
        {
            GSNativeEffect::Buffer &buffer = effect.buffers[i];
            if (buffer.base == batch.box.base && buffer.fbw == batch.box.fbw)
            {
                buffer.layer = 1u;
                fromLayer = true;
            }
        }
        ok = fromLayer;
        why = ok ? why : "no sprite from the object's buffer";
    }
    if (ok)
    {
        effect.command = GSNativeEffect::kObjectTail;
        ok = sendEffect(gs, effect);
        why = ok ? why : "the producer cannot draw it (its stats line says why)";
    }
    if (ok)
    {
        for (const std::vector<uint8_t> &packet : withheld)
            sendPacket(gs, packet.data(), static_cast<uint32_t>(packet.size()));
        batch.waiting = true;
    }
    else
    {
        const bool nothing = why == "no sprites";
        static std::vector<std::string> told;
        if (!nothing && told.size() < 8u && std::find(told.begin(), told.end(), why) == told.end())
        {
            told.push_back(why);
            std::fprintf(stderr, "[composite] a tail stays packets: %s\n", why.c_str());
        }
        if (!nothing)
            ++g_compositeRefused;
        static const bool trace = std::getenv("RG_COMPOSITE_TRACE") != nullptr;
        static int dumped = 0;
        if (trace && !nothing && dumped++ < 3)
        {
            std::fprintf(stderr, "[composite] refused (%s), box %d,%d-%d,%d:\n", why.c_str(), batch.box.x0, batch.box.y0,
                         batch.box.x1, batch.box.y1);
            rg::nativefx::dumpPackets("composite", batch.tail);
        }
        effect = GSNativeEffect{};
        effect.command = GSNativeEffect::kObjectTail;
        effect.bufferCount = nothing ? 0u : 1u;
        effect.buffers[0].base = batch.box.base;
        effect.buffers[0].fbw = batch.box.fbw;
        sendEffect(gs, effect);
        batch.waiting = false;
        for (const std::vector<uint8_t> &packet : batch.tail)
            sendPacket(gs, packet.data(), static_cast<uint32_t>(packet.size()));
    }
    batch.tail.clear();
}

// The object being read is over (what comes next is not its).
void endObject()
{
    CompositeBatch &batch = t_batch;
    if (batch.state == CompositeBatch::State::Head && !batch.decided)
        decideHead();
    if (batch.state == CompositeBatch::State::Head && batch.routed)
        sendCommand(*batch.gs, GSNativeEffect::kRouteEnd);
    finishTail();
    batch.state = CompositeBatch::State::Outside;
    batch.routed = false;
}

// At a list's end, or before anything that is no composite's.
void flushComposites()
{
    if (!t_batch.gs)
        return;
    endObject();
    compositeRun();
    t_batch.uploading = false;
}

void discardHeld()
{
    t_held.effect = rg::nativefx::Effect::None;
    t_held.unit = 0u;
    t_held.packets.clear();
    t_batch = CompositeBatch{};
}

void compositeOther(const char *what = "?", uint32_t address = ~0u, uint32_t cursor = ~0u)
{
    static const bool trace = std::getenv("RG_COMPOSITE_TRACE") != nullptr;
    static int traced = 0;
    if (trace && traced < 40 && (t_batch.waiting || t_batch.state != CompositeBatch::State::Outside))
    {
        ++traced;
        std::fprintf(stderr, "[composite]   run ended by %s @%08x cursor %08x (state %d)\n", what, address, cursor,
                     int(t_batch.state));
    }
    flushComposites();
}

bool uploadOrFlush(const uint8_t *data, uint32_t size)
{
    uint32_t first, last;
    if (rg::nativefx::uploadPages(data, size, first, last))
        return true;
    // A packet that only flushes the texture cache (TEXFLUSH: no state, no
    // drawing).
    if (size < 32u)
        return false;
    uint64_t tag, regs;
    std::memcpy(&tag, data, 8);
    std::memcpy(&regs, data + 8, 8);
    const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
    bool flushOnly = (tag >> 58) == 0x4u && (regs & 0xFu) == 0xEu && nloop != 0u && 16u + nloop * 16u <= size;
    for (uint32_t i = 0; flushOnly && i < nloop; ++i)
        flushOnly = data[16u + i * 16u + 8u] == 0x3Fu;
    return flushOnly;
}

// A packet of a composite (its place in the list's EE memory, or for what a
// VU1 program kicked, the part being run): true when it was taken.
bool compositePacket(GS &gs, const uint8_t *data, uint32_t size, uint32_t address)
{
    CompositeBatch &batch = t_batch;
    batch.gs = &gs;
    rg::nativefx::Place place;
    // What a VU1 program kicked: the part whose list holds the MSCAL.
    uint32_t cursor = ~0u;
    if (address == ~0u && g_vif1Memory)
        cursor = listAddress(g_vif1Memory->vif1Cursor());
    if (address != ~0u)
        place = rg::nativefx::placeAt(address);
    else if (cursor != ~0u)
        place = rg::nativefx::placeAt(cursor);
    // Until its tail, a head goes on through what it refers to: the model's
    // packets from the model's memory, and what its VU1 programs kick.
    // (Every object marked has a tail.)
    if (batch.state == CompositeBatch::State::Head && place.effect == rg::nativefx::Effect::None)
        place = {rg::nativefx::Effect::CompositeHead, batch.unit};
    {
        static const bool trace = std::getenv("RG_COMPOSITE_TRACE") != nullptr;
        static int traced = 0;
        if (trace && traced < 400 && (place.effect != rg::nativefx::Effect::None || batch.state != CompositeBatch::State::Outside))
        {
            ++traced;
            std::fprintf(stderr, "[composite]   %s unit %u (state %d unit %u decided %d) @%08x cursor %08x %u bytes\n",
                         rg::nativefx::name(place.effect), place.unit, int(batch.state), batch.unit, int(batch.decided),
                         address, cursor, size);
        }
    }
    if (place.effect == rg::nativefx::Effect::CompositeHead)
    {
        if (batch.state != CompositeBatch::State::Head || batch.unit != place.unit)
        {
            endObject();
            batch.state = CompositeBatch::State::Head;
            batch.unit = place.unit;
            batch.decided = false;
        }
        batch.uploading = false;
        if (!batch.decided)
        {
            if (address != ~0u && rg::nativefx::placeAt(address).effect == rg::nativefx::Effect::CompositeHead)
            {
                batch.head.emplace_back(data, data + size);
                return true;
            }
            decideHead();
        }
        sendPacket(gs, data, size);
        return true;
    }
    // (Unit 0: the fixed chain every tail calls.)
    if (place.effect == rg::nativefx::Effect::CompositeTail && (place.unit == batch.unit || place.unit == 0u) &&
        (batch.state == CompositeBatch::State::Head || batch.state == CompositeBatch::State::Tail))
    {
        if (batch.state == CompositeBatch::State::Head)
        {
            if (!batch.decided)
                decideHead();
            if (batch.routed)
                sendCommand(gs, GSNativeEffect::kRouteEnd);
            batch.state = CompositeBatch::State::Tail;
        }
        if (batch.routed)
            batch.tail.emplace_back(data, data + size);
        else
            sendPacket(gs, data, size);
        return true;
    }
    // The upload's data, as an IMAGE packet.
    if (batch.uploading && size >= 16u)
    {
        uint64_t tag;
        std::memcpy(&tag, data, 8);
        if (((tag >> 58) & 3u) == 2u)
        {
            sendPacket(gs, data, size);
            return true;
        }
    }
    batch.uploading = false;
    // Between objects the caller uploads textures (CLUTs) and flushes the
    // texture cache: on ahead of the waiting tails, which read only their
    // layers (each tail's packets set every register it draws with).
    if (batch.state != CompositeBatch::State::Head && (batch.waiting || batch.state == CompositeBatch::State::Tail) &&
        uploadOrFlush(data, size))
    {
        endObject();
        uint32_t first, last;
        batch.uploading = rg::nativefx::uploadPages(data, size, first, last);
        sendPacket(gs, data, size);
        return true;
    }
    compositeOther(rg::nativefx::name(place.effect), address, cursor);
    return false;
}

void deliverPacket(GS &gs, const uint8_t *data, uint32_t size)
{
    // Step 5: an effect's packets, held until the effect's run ends. A
    // packet outside the held run ends it first, before a composite can
    // take the packet: the effect goes on ahead of what follows it.
    rg::nativefx::Effect effect = rg::nativefx::Effect::None;
    uint32_t unit = 0u;
    const bool effects = nativeEffects() && t_onVif1Thread;
    if (effects)
    {
        const uint32_t address = listAddress(data);
        const rg::nativefx::Place place = address == ~0u ? rg::nativefx::Place{} : rg::nativefx::placeAt(address);
        effect = place.effect;
        unit = effect == rg::nativefx::Effect::ColorFilter ? place.unit : 0u;
        if (effect != t_held.effect || unit != t_held.unit)
        {
            flushEffect();
        }
    }
    if (composites() && t_onVif1Thread && compositePacket(gs, data, size, listAddress(data)))
    {
        return;
    }
    if (effects)
    {
        if (effect == rg::nativefx::Effect::ShadowSetup)
        {
            rg::nativefx::observe(effect, data, size);
        }
        else if (effect != rg::nativefx::Effect::None)
        {
            t_held.effect = effect;
            t_held.unit = unit;
            t_held.gs = &gs;
            t_held.packets.emplace_back(data, data + size);
            return;
        }
    }
    // The flip's copy, from the game thread: after the lists in flight, as
    // the GS would get it.
    if (!t_onVif1Thread && rg::nativefx::flipNative())
    {
        thread_local GSNativeEffect effect;
        thread_local std::vector<std::vector<uint8_t>> withheld;
        if (rg::nativefx::flipPacket(data, size, effect, withheld))
        {
            g_thread->drain();
            rg::nativeproducer::noteEffect(effect);
            const bool drawn = rg::nativeproducer::effect(effect);
            if (drawn)
            {
                for (const std::vector<uint8_t> &packet : withheld)
                {
                    sendPacket(gs, packet.data(), static_cast<uint32_t>(packet.size()));
                }
            }
            else
            {
                sendPacket(gs, data, size);
            }
            countEffect(drawn, rg::nativefx::Effect::Flip, "the producer cannot draw it", {});
            return;
        }
    }
    sendPacket(gs, data, size);
}

void deliverMesh(GS &gs, const GSNativeMesh &mesh, const uint8_t *vu1Data)
{
    // A held effect goes on first: a composite's head decided here sends
    // its packets.
    if (nativeEffects() && t_onVif1Thread)
    {
        flushEffect();
    }
    if (composites() && t_onVif1Thread)
    {
        t_batch.gs = &gs;
        if (t_batch.state == CompositeBatch::State::Head)
        {
            if (!t_batch.decided)
                decideHead();
        }
        else
        {
            compositeOther("a mesh");
        }
    }
    // Steps 4a and 4c of docs/NATIVE-PIPELINE.md: a VU1 program's mesh as a
    // native mesh draw, under the draw state the GS would draw it with; made
    // one, it does not go to the GS at all.
    if (vu1Data && rg::drawstate::enabled())
    {
        if (g_stage && !t_onVif1Thread)
            g_thread->drain();
        const GSDrawState state = rg::drawstate::current();
        if (rg::nativeproducer::mesh(mesh, &state, vu1Data, 0u))
            return;
        // Refused here only for a buffer the game never named (counted as a
        // target miss): with no GS behind the pipeline nothing can draw it,
        // so it goes where the absorbed packets go, not to the tripwire.
        if (absorbing())
            return;
    }
    meshToGs(gs, mesh);
}
// The thread that runs guest code, as the first hooked DMA call sees it.
std::thread::id g_gameThread;
PS2Runtime::RecompiledFunction g_dmaSend = nullptr;
PS2Runtime::RecompiledFunction g_dmaSendN = nullptr;
PS2Runtime::RecompiledFunction g_dmaSendI = nullptr;
PS2Runtime::RecompiledFunction g_dmaSync = nullptr;
std::vector<std::pair<uint32_t, PS2Runtime::RecompiledFunction>> g_gsWaiters;

uint32_t channelOf(R5900Context *ctx)
{
    return getRegU32(ctx, 4) & 0x1FFFFF00u;
}

// What `jr $ra` leaves. A hook that returns without the original must do
// this itself: a call through dispatchGuestBranch forgives a pc left at the
// entry, but a thread the run loop resumes at the entry (a safe point
// unwound it at that call) would be dispatched here again, forever.
void returnToCaller(R5900Context *ctx, int32_t value)
{
    setReturnS32(ctx, value);
    ctx->pc = getRegU32(ctx, 31);
}

// What the stub does right after a transfer, for the lists that have
// finished since, on the game's thread: VU1's D/T stop bits into VPU_STAT,
// and the DMAC completion handlers.
void reportFinished(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint64_t finished = g_thread->takeFinished();
    if (finished == 0u)
    {
        return;
    }
    ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & ~0x0600u) | g_thread->takeVpuStat();
    for (uint64_t n = finished; n > 0u; --n)
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, runtime, 1u);
    }
}

// sceDmaSend/SendI (chain) and SendN (normal) to VIF1 go to the thread;
// anything else to the GS path first waits for the lists in flight.
void send(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, PS2Runtime::RecompiledFunction original,
          bool normal)
{
    if (g_gameThread == std::thread::id())
    {
        g_gameThread = std::this_thread::get_id();
    }
    const uint32_t channel = channelOf(ctx);
    if (channel != kVif1Channel)
    {
        if (channel == kGifChannel)
        {
            g_thread->drain();
        }
        original(rdram, ctx, runtime);
        return;
    }
    PS2Memory &memory = runtime->memory();
    const uint32_t chcr = memory.readIORegister(kVif1Channel);
    const uint32_t address = toDmaPhys(getRegU32(ctx, 5));
    List list;
    list.data = g_thread->takeBuffer();
    list.dBit = (ctx->vu0_fbrst & (1u << 10)) != 0u;
    list.tBit = (ctx->vu0_fbrst & (1u << 11)) != 0u;
    if (normal)
    {
        const uint32_t qwc = getRegU32(ctx, 6) & 0xFFFFu;
        try
        {
            list.data.reserve(qwc * 16u);
            appendBytes(memory, list.data, address, qwc * 16u);
            if (nativeEffects())
            {
                list.origins.emplace_back(0u, address);
            }
        }
        catch (const std::exception &)
        {
            list.data.clear();
        }
    }
    else
    {
        static uint32_t listNumber = 0u;
        list.number = ++listNumber;
        gatherChain(memory, address, (chcr & kChcrTte) != 0u, (chcr & kChcrTie) != 0u, list.data,
                    (g_capture || nativeEffects()) ? &list.origins : nullptr);
    }
    reportFinished(rdram, ctx, runtime);
    g_thread->submit(std::move(list));
    returnToCaller(ctx, 0);
}

void dmaSend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { send(rdram, ctx, runtime, g_dmaSend, false); }
void dmaSendI(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { send(rdram, ctx, runtime, g_dmaSendI, false); }
void dmaSendN(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { send(rdram, ctx, runtime, g_dmaSendN, true); }

// sceDmaSync(chan, mode, timeout): mode 0 blocks until the channel is idle,
// any other mode only asks. For VIF1 the channel is busy while the thread
// still has lists; the stub's own model never saw them.
void dmaSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (channelOf(ctx) != kVif1Channel)
    {
        g_dmaSync(rdram, ctx, runtime);
        return;
    }
    if (getRegU32(ctx, 5) != 0u && g_thread->busy())
    {
        returnToCaller(ctx, 1);
        return;
    }
    g_thread->drain();
    reportFinished(rdram, ctx, runtime);
    returnToCaller(ctx, 0);
}

// The libgraph stubs: the lists in flight first, then the stub.
template <size_t Index>
void gsWaiter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_thread->drain();
    // Step 7a (docs/NATIVE-PIPELINE.md): under the native pipeline's 2D the
    // stub's register writes (its draw environments, and the clear sprite)
    // are taken instead of written to the GS (patch 0034), and go through
    // the converter as one A+D packet, in order with the lists just drained:
    // the mirror follows them, and the clear is a 2D piece like any other.
    // The GS's own clear shortcut is dropped.
    if (native2D())
    {
        thread_local GS::RegisterSink writes;
        writes.clear();
        GS::setRegisterSink(&writes);
        g_gsWaiters[Index].second(rdram, ctx, runtime);
        GS::setRegisterSink(nullptr);
        if (!writes.empty())
        {
            const std::vector<uint8_t> packet = rg::native2d::registerPacket(writes);
            sendPacket(runtime->gs(), packet.data(), static_cast<uint32_t>(packet.size()));
        }
        return;
    }
    g_gsWaiters[Index].second(rdram, ctx, runtime);
    // The stubs write the front end's registers themselves (sceGsPutDrawEnv,
    // sceGsSwapDBuff's environments and clear): the native pipeline's
    // draw-state mirror reads them back.
    rg::drawstate::readBack(runtime->gs());
}

template <size_t... I>
constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(I)> gsWaiterHooks(std::index_sequence<I...>)
{
    return {&gsWaiter<I>...};
}

bool hook(PS2Runtime &runtime, uint32_t address, PS2Runtime::RecompiledFunction replacement,
          PS2Runtime::RecompiledFunction &original)
{
    original = runtime.lookupFunction(address);
    if (!original || !runtime.replaceFunction(address, replacement))
    {
        std::fprintf(stderr, "[vif1] 0x%08X is not in the function table\n", address);
        return false;
    }
    return true;
}

} // namespace

void install(PS2Runtime &runtime)
{
    const char *mode = std::getenv("RG_VIF1_THREAD");
    if (mode && std::strcmp(mode, "0") == 0)
    {
        return;
    }
    if (!runtime.lookupFunction(kSceDmaSend) || !runtime.lookupFunction(kSceDmaSendN) ||
        !runtime.lookupFunction(kSceDmaSendI) || !runtime.lookupFunction(kSceDmaSync))
    {
        std::fprintf(stderr, "[vif1] libdma entry points missing; VIF1 stays on the game's thread\n");
        return;
    }
    g_thread = new Vif1Thread(runtime);
    g_runtime = &runtime;
    g_vif1Memory = &runtime.memory();

    // GS packets from the VIF1 thread and from the game's own GIF transfers
    // reach the GS through PS2Memory. Its GifArbiter queues them unlocked and
    // every DMA start drains it, so it cannot be shared between threads;
    // packets go straight to the GS instead, whose processGIFPacket holds the
    // GS state lock. The arbiter only ever ordered packets queued together,
    // and the VIF1 paths already drained it after each one.
    PS2Memory &memory = runtime.memory();
    memory.setGifArbiter(nullptr);
    const char *gifMode = std::getenv("RG_GIF_THREAD");
    if (!gifMode || std::strcmp(gifMode, "0") != 0)
    {
        g_stage = new GifStage(runtime.gs());
    }
    // From the VIF1 thread through the GIF stage. From any other thread (the
    // game's own GIF transfers, after the drain every DMA start does) straight
    // to the GS, behind whatever the stage still holds.
    memory.setGifPacketCallback([&gs = runtime.gs()](const uint8_t *data, uint32_t size) {
        // A program's XGKICK, while RG_VU1_REPLAY records the run.
        static const bool replay = rg::vu1replay::enabled();
        if (replay && t_onVif1Thread)
        {
            rg::vu1replay::kick(data, size);
        }
        // RG_VU1_NATIVE: a chunk done natively, whose own packets go instead.
        if (t_vu1Packets)
        {
            t_vu1Packets->emplace_back(data, data + size);
        }
        if (t_dropVu1Packets)
        {
            return;
        }
        deliverPacket(gs, data, size);
    });
    // A texture split over DIRECT packets continues in place, without the
    // copy behind a synthesized IMAGE tag.
    memory.setGifImageCallback([&gs = runtime.gs()](const uint8_t *data, uint32_t size) {
        // A held effect goes on first: a composite's head decided here sends
        // its packets.
        if (t_onVif1Thread)
        {
            flushEffect();
        }
        if (composites() && t_onVif1Thread)
        {
            t_batch.gs = &gs;
            if (t_batch.state == CompositeBatch::State::Head)
            {
                if (!t_batch.decided)
                    decideHead();
            }
            else if (!t_batch.uploading)
            {
                compositeOther("an image");
            }
        }
        if (uploadsTaken())
        {
            // A withheld upload's rest goes no further; another's is
            // gathered for the producer on its way.
            const uint32_t withheld = withheldContinuation(data, size);
            data += withheld;
            size -= withheld;
            if (size == 0u)
                return;
            feedCapture(data, size);
            if (absorbing())
                return;
        }
        if (g_stage && t_onVif1Thread)
        {
            g_stage->image(data, size);
            return;
        }
        if (g_stage)
        {
            g_thread->drain();
        }
        gs.processImageContinuation(data, size);
    });
    // DMA the game starts through the registers rather than sceDmaSend.
    // VU1 started by the lists, without the guest thread context the
    // runtime's callbacks reach into (see Vif1Thread::runVu1).
    memory.setVu1MscalCallback([](uint32_t startPC, uint32_t top, uint32_t itop) {
        g_thread->runVu1(false, startPC, top, itop);
    });
    memory.setVu1MscntCallback([](uint32_t top, uint32_t itop) { g_thread->runVu1(true, 0u, top, itop); });
    memory.setDmaStartObserver([](uint32_t channel) {
        if (channel == kGifChannel || channel == kVif1Channel)
        {
            g_thread->drain();
        }
    });

    hook(runtime, kSceDmaSend, &dmaSend, g_dmaSend);
    hook(runtime, kSceDmaSendN, &dmaSendN, g_dmaSendN);
    hook(runtime, kSceDmaSendI, &dmaSendI, g_dmaSendI);
    hook(runtime, kSceDmaSync, &dmaSync, g_dmaSync);
    constexpr size_t kWaiters = std::size(kGsWaiters);
    static constexpr auto hooks = gsWaiterHooks(std::make_index_sequence<kWaiters>{});
    g_gsWaiters.resize(kWaiters);
    for (size_t i = 0; i < kWaiters; ++i)
    {
        g_gsWaiters[i].first = kGsWaiters[i];
        hook(runtime, kGsWaiters[i], hooks[i], g_gsWaiters[i].second);
    }
    std::printf("[rg] VIF1 lists run on their own thread%s\n", g_stage ? ", their GS packets on another" : "");
}

void shutdown()
{
    if (g_thread)
    {
        g_thread->stop();
    }
    if (g_stage)
    {
        g_stage->stop();
    }
}

std::string busyReport()
{
    static auto lastTime = std::chrono::steady_clock::now();
    static uint64_t lastBusy = 0u, lastGif = 0u;
    const auto now = std::chrono::steady_clock::now();
    const uint64_t busy = g_busyNanos.load(std::memory_order_relaxed);
    const uint64_t gif = g_gifBusyNanos.load(std::memory_order_relaxed);
    const double wall = std::chrono::duration<double>(now - lastTime).count();
    auto share = [&](uint64_t nanos) { return wall > 0.0 ? (static_cast<double>(nanos) * 1e-9) / wall * 100.0 : 0.0; };
    char text[96];
    if (g_stage)
    {
        std::snprintf(text, sizeof(text), "vif1 busy=%.0f%% gif busy=%.0f%% of wall", share(busy - lastBusy),
                      share(gif - lastGif));
    }
    else
    {
        std::snprintf(text, sizeof(text), "vif1 busy=%.0f%% of wall", share(busy - lastBusy));
    }
    lastTime = now;
    lastBusy = busy;
    lastGif = gif;
    // VU1 pairs run on the interpreter since the last report: a program the
    // build has not compiled (several times slower), e.g. an effect's, shows
    // up here. Read unsynchronised from the VIF1 thread's counters; a stats
    // line can live with a torn count.
    if (g_runtime)
    {
        static uint64_t lastInterpreted = 0u, lastCompiled = 0u;
        const uint64_t interpreted = g_runtime->vu1().interpretedPairsExecuted();
        const uint64_t compiled = g_runtime->vu1().compiledPairsExecuted();
        const uint64_t newInterpreted = interpreted - std::min(interpreted, lastInterpreted);
        const uint64_t newCompiled = compiled - std::min(compiled, lastCompiled);
        lastInterpreted = interpreted;
        lastCompiled = compiled;
        if (newInterpreted != 0u)
        {
            // Per second, and as a share of the pairs the VU1 ran at all: the
            // chunks done natively never run it, so a high share of a small
            // count is no load (2026-11-01: 44% was 14k pairs a second).
            char more[128];
            std::snprintf(more, sizeof(more), " | vu1 interpreted %.1fk pairs/s (%.0f%% of what the VU1 ran)",
                          wall > 0.0 ? static_cast<double>(newInterpreted) / wall / 1000.0 : 0.0,
                          100.0 * static_cast<double>(newInterpreted) /
                              static_cast<double>(newInterpreted + newCompiled));
            return std::string(text) + more;
        }
    }
    return text;
}

void drainForSnapshot()
{
    // Only from the game's thread. The presenter reaches a frame boundary
    // too, through flip pacing's fallback while the game shows no flips (a
    // loading screen), and it does so holding the GS state lock, which the
    // VIF1 thread needs to finish its list: waiting for it there is a
    // deadlock (it was one, at the Story loading screen).
    if (g_thread && std::this_thread::get_id() == g_gameThread)
    {
        g_thread->drain();
    }
}

bool busy()
{
    return g_thread && g_thread->busy();
}

} // namespace rg::vif1
