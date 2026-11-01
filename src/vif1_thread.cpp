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
#include "vu1_native.h"
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
const rg::vu1native::Mode g_nativeMode = rg::vu1native::modeFromEnvironment();
// RG_VU1_NATIVE_CHECK_EVERY=N: check mode compares one chunk in N (each costs
// the VU1's run, the native build and two decodes, on this thread).
const uint32_t g_checkEvery = [] {
    const char *value = std::getenv("RG_VU1_NATIVE_CHECK_EVERY");
    const unsigned long every = value ? std::strtoul(value, nullptr, 10) : 1ul;
    return static_cast<uint32_t>(every == 0ul ? 1ul : every);
}();
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
// A native strip likewise, in order with the packets.
void deliverMesh(GS &gs, const GSNativeMesh &mesh);
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
        if (g_nativeMode != rg::vu1native::Mode::Off)
        {
            if (!resume)
            {
                if (memory.getVU1CodeGeneration() != m_nativeGeneration || startPC != m_nativePC)
                {
                    m_nativeGeneration = memory.getVU1CodeGeneration();
                    m_nativePC = startPC;
                    m_nativeProgram = rg::vu1native::identify(memory.getVU1Code(), startPC);
                }
            }
            else if (memory.getVU1CodeGeneration() != m_nativeGeneration)
            {
                // An MPG between the MSCAL and its MSCNTs: the code the
                // program was identified in is gone.
                m_nativeProgram = nullptr;
            }
            if (m_nativeProgram && resume != rg::vu1native::drawsAtCall(*m_nativeProgram))
            {
                const bool gpu = g_nativeMode == rg::vu1native::Mode::Gpu;
                // The chunks that clip go to the VU1 in every mode: the GPU's
                // clipping of triangles across the eye plane does not match
                // the VU1's on every driver.
                const char *reason = rg::vu1native::declineChunk(*m_nativeProgram, memory.getVU1Data(), top, false);
                if (reason)
                {
                    m_nativeMonitor.declined(*m_nativeProgram, reason);
                }
                else if (g_nativeMode == rg::vu1native::Mode::Check && m_checkCount++ % g_checkEvery != 0u)
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
        if (!native || (g_nativeMode != rg::vu1native::Mode::Skip && g_nativeMode != rg::vu1native::Mode::Gpu))
        {
            if (native && g_nativeMode == rg::vu1native::Mode::Check)
            {
                m_vu1Packets.clear();
                t_vu1Packets = &m_vu1Packets;
                // Its inputs, to save it if it differs.
                m_checkData.assign(memory.getVU1Data(), memory.getVU1Data() + PS2_VU1_DATA_SIZE);
                m_checkState = vu1.state();
            }
            t_dropVu1Packets = native && g_nativeMode == rg::vu1native::Mode::Replace;
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
            if (g_nativeMode == rg::vu1native::Mode::Check)
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
                if (g_nativeMode == rg::vu1native::Mode::Gpu && rg::vu1native::hasMesh(*native))
                {
                    deliverMesh(m_runtime.gs(), m_nativeMesh);
                }
                // What the VU1 would have left in its memory, where it did not run.
                if (g_nativeMode != rg::vu1native::Mode::Replace)
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
                if (!m_current.data.empty())
                {
                    memory.processVIF1Data(m_current.data.data(), static_cast<uint32_t>(m_current.data.size()));
                }
            }
            catch (const std::exception &error)
            {
                std::fprintf(stderr, "[vif1] list failed: %s\n", error.what());
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
    uint32_t m_checkCount = 0u;
    rg::vu1native::Monitor m_nativeMonitor{g_nativeMode};
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

void deliverPacket(GS &gs, const uint8_t *data, uint32_t size)
{
    if (g_stage && t_onVif1Thread)
    {
        g_stage->packet(data, size);
        return;
    }
    if (g_stage)
    {
        // The lists in flight too, not only what they handed the stage:
        // every DMA start drains them already, this keeps it so.
        g_thread->drain();
    }
    ps2_gs_detail::t_sourceTag = t_onVif1Thread ? t_vu1Tag : 0u;
    ps2_gs_detail::t_sourceCamera = t_onVif1Thread ? t_vu1Camera : 0u;
    gs.processGIFPacket(data, size);
}

void deliverMesh(GS &gs, const GSNativeMesh &mesh)
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
                    g_capture ? &list.origins : nullptr);
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
    g_gsWaiters[Index].second(rdram, ctx, runtime);
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
            char more[96];
            std::snprintf(more, sizeof(more), " | vu1 interpreted %llu pairs (%.0f%%)",
                          static_cast<unsigned long long>(newInterpreted),
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
