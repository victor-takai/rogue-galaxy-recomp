// The native programs of src/vu1_native.h as an oracle candidate: every run of
// a program it recognizes, MSCAL (which kicks nothing) and the MSCNTs it does
// not decline, compared with the recording.
#include <chrono>
#include <cstdio>

#include "oracle.h"
#include "vu1_native.h"

namespace rg::oracle
{
namespace
{

class Native final : public Candidate
{
public:
    const char *name() const override { return "native (src/vu1_native.cpp)"; }

    bool accepts(const Run &run) const override
    {
        const rg::vu1native::Program *program = programOf(run);
        if (!program)
        {
            return false;
        }
        // A strip program's MSCAL only ends (its MSCNTs draw); the overlay
        // program's draws, and no MSCNT follows it.
        const bool resume = (run.header.flags & rg::vu1replay::kRunResume) != 0u;
        if (!resume && !rg::vu1native::drawsAtCall(*program))
        {
            return true;
        }
        if (resume && rg::vu1native::drawsAtCall(*program))
        {
            ++m_declined[std::string("left to the VU1, ") + program->name + ": an MSCNT after it"];
            return false;
        }
        const char *reason = rg::vu1native::declineChunk(*program, run.data.data(), run.header.top);
        if (reason)
        {
            ++m_declined[std::string("left to the VU1, ") + program->name + ": " + reason];
            return false;
        }
        ++m_taken[program->name];
        return true;
    }

    std::map<std::string, uint32_t> declined() const override
    {
        std::map<std::string, uint32_t> lines = m_declined;
        for (const auto &[name, count] : m_taken)
        {
            lines[std::string("done natively, ") + name] = count;
        }
        for (const auto &[name, count] : m_memoryDiffers)
        {
            lines[name] = count;
        }
        return lines;
    }

    void run(const Recording &recording, const Run &run, std::vector<GifEvent> &events) override
    {
        events.clear();
        const auto key = std::make_pair(run.programHash, run.programPC);
        if (!m_programs.count(key) && run.programHash == run.header.codeHash)
        {
            const auto code = recording.codes.find(run.programHash);
            m_programs[key] =
                code == recording.codes.end() ? nullptr : rg::vu1native::identify(code->second.data(), run.programPC);
        }
        const rg::vu1native::Program *program = programOf(run);
        const bool resume = program && (run.header.flags & rg::vu1replay::kRunResume) != 0u;
        if (!program || resume == rg::vu1native::drawsAtCall(*program) ||
            rg::vu1native::declineChunk(*program, run.data.data(), run.header.top))
        {
            return;
        }
        // Timed apart: the packets (the CPU path) and the mesh (the GPU path,
        // whose transform is then the GPU's).
        auto started = std::chrono::steady_clock::now();
        rg::vu1native::buildChunk(*program, run.data.data(), run.header.top, m_packets);
        m_lastNanos = elapsed(started);
        started = std::chrono::steady_clock::now();
        rg::vu1native::buildMesh(*program, run.data.data(), run.header.top, m_meshPackets, m_mesh);
        m_meshNanos += elapsed(started);
        ++m_meshChunks;
        GifState state;
        for (const auto &packet : m_packets)
        {
            decodeGifPacket(packet.data(), static_cast<uint32_t>(packet.size()), state, events);
        }
        // What it leaves in VU1 memory, where the recording has the memory
        // after the run: counted on a line of its own.
        if (!run.endData.empty() &&
            !rg::vu1native::memoryEffectsMatch(*program, run.data.data(), run.header.top, run.endData.data()))
        {
            ++m_memoryDiffers[std::string("VU1 memory differs, ") + program->name];
        }
    }

    uint64_t lastNanos() const override { return m_lastNanos; }

    std::string timing() const override
    {
        char line[160];
        std::snprintf(line, sizeof(line), "the mesh instead of the packets: %.2f us a chunk over %llu",
                      m_meshChunks ? m_meshNanos / 1000.0 / m_meshChunks : 0.0,
                      static_cast<unsigned long long>(m_meshChunks));
        return line;
    }

private:
    static uint64_t elapsed(std::chrono::steady_clock::time_point started)
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    }

    const rg::vu1native::Program *programOf(const Run &run) const
    {
        const auto found = m_programs.find(std::make_pair(run.programHash, run.programPC));
        return found == m_programs.end() ? nullptr : found->second;
    }

    // Per code image and entry (the MSCAL's), what it is, if anything.
    std::map<std::pair<uint64_t, uint32_t>, const rg::vu1native::Program *> m_programs;
    std::vector<std::vector<uint8_t>> m_packets;
    std::vector<std::vector<uint8_t>> m_meshPackets;
    GSNativeMesh m_mesh;
    uint64_t m_lastNanos = 0u;
    uint64_t m_meshNanos = 0u;
    uint64_t m_meshChunks = 0u;
    mutable std::map<std::string, uint32_t> m_declined;
    mutable std::map<std::string, uint32_t> m_taken;
    std::map<std::string, uint32_t> m_memoryDiffers;
};

} // namespace

std::unique_ptr<Candidate> makeNative()
{
    return std::make_unique<Native>();
}

} // namespace rg::oracle
