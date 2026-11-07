// rg_vu1_oracle: replays an RG_VU1_REPLAY recording (src/debug/vu1_replay.h)
// through VU1 without the game, and checks what each candidate draws against
// what the game's own runs kicked.
//
//   rg_vu1_oracle FILE... [--candidate compiled|interpreter|all|native] [--runs N]
//                      [--program HASH:PC] [--show RUN|diff] [--tol-xy N] [--tol-z N]
//                      [--tol-rgba N] [--tol-uv N] [--tol-stq F]
//                      [--program HASH:PC --coverage FILE] [--time] [--inventory]
//
// --time adds the time each candidate's own work took per run (the VU1
// program's execution; the native code's packets or mesh), averaged over the
// runs it took. --inventory lists every program in the recording instead, by
// its share of the vertices, with the pieces of src/vu1_native.cpp it has.
//
// --coverage single-steps the interpreter through the program's runs (with
// --program) and writes how often each instruction pair ran, `pc hits` in
// bytes, for tools/vu1/vudis.py; it prints how many distinct paths they took.
//
// --show prints one run's events beside the recorded ones, from its first
// difference; `--show diff` does so for the first differing run of each
// program.
//
// Several files (RG_VU1_HARVEST's, one a program) are read one after another.
//
// A candidate turns a run into GS events (gif_events.h): the VU1 engines run
// the recorded program from the registers and data memory the game had and
// decode what it kicks; a native replacement of a program family (phase 1 of
// docs/NATIVE-RENDERER.md) computes the events itself: `native` is
// src/vu1_native.h (native.cpp), which lists the runs it did, and those it
// left to the VU1 and why. Runs are replayed in
// the file's order on one VU1, since an MSCNT continues the run before it.
//
// A run's program is its code image and the entry of the MSCAL that began it;
// continuations count toward it. The table lists, per program, the runs, the
// events, how many differ beyond the tolerances and the largest differences,
// and whether the registers the run ended with match. Exit status 1 when any
// candidate differs.
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <xmmintrin.h>

#include "gif_events.h"
#include "oracle.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"
#include "vu1_native.h"
#include "vu1_replay.h"

namespace
{

using namespace rg::oracle;
namespace replay = rg::vu1replay;

// VU1 micro memory as the EE addresses it: a write here bumps the code
// generation the VU1 caches its decoding and compiled routines by.
constexpr uint32_t kVu1CodeAddress = 0x11008000u;

bool readRecording(const char *path, uint32_t maxRuns, Recording &out)
{
    std::FILE *file = std::fopen(path, "rb");
    if (!file)
    {
        std::fprintf(stderr, "cannot read %s\n", path);
        return false;
    }
    std::setvbuf(file, nullptr, _IOFBF, 16u << 20);
    const auto get = [&](void *bytes, size_t size) { return std::fread(bytes, 1, size, file) == size; };
    uint32_t header[5];
    if (!get(header, sizeof(header)) || header[0] != replay::kMagic || header[1] != replay::kVersion)
    {
        std::fprintf(stderr, "%s: not an RG_VU1_REPLAY recording (version %u)\n", path, replay::kVersion);
        std::fclose(file);
        return false;
    }
    if (header[2] != sizeof(VU1State) || header[3] != PS2_VU1_CODE_SIZE || header[4] != PS2_VU1_DATA_SIZE)
    {
        std::fprintf(stderr, "%s: recorded by a build with another VU1State (%u bytes, this one %zu)\n", path,
                     header[2], sizeof(VU1State));
        std::fclose(file);
        return false;
    }
    uint64_t programHash = 0u;
    uint32_t programPC = 0u;
    bool truncated = false;
    uint32_t kind;
    while (get(&kind, sizeof(kind)))
    {
        if (kind == replay::kCode)
        {
            uint64_t hash;
            std::vector<uint8_t> code(PS2_VU1_CODE_SIZE);
            truncated = !get(&hash, sizeof(hash)) || !get(code.data(), code.size());
            out.codes.emplace(hash, std::move(code));
        }
        else if (kind == replay::kRun)
        {
            if (out.runs.size() >= maxRuns)
            {
                break;
            }
            Run run;
            run.data.resize(PS2_VU1_DATA_SIZE);
            truncated = !get(&run.header, sizeof(run.header)) || !get(&run.state, sizeof(run.state)) ||
                        !get(run.data.data(), run.data.size());
            if (!(run.header.flags & replay::kRunResume))
            {
                programHash = run.header.codeHash;
                programPC = run.header.startPC;
            }
            run.programHash = programHash;
            run.programPC = programPC;
            out.runs.push_back(std::move(run));
        }
        else if (kind == replay::kKick && !out.runs.empty())
        {
            uint32_t size;
            truncated = !get(&size, sizeof(size));
            std::vector<uint8_t> packet(truncated ? 0u : size);
            truncated = truncated || !get(packet.data(), packet.size());
            out.runs.back().kicks.push_back(std::move(packet));
        }
        else if (kind == replay::kEnd && !out.runs.empty())
        {
            uint32_t flags;
            Run &run = out.runs.back();
            truncated = !get(&flags, sizeof(flags)) || !get(&run.endState, sizeof(run.endState));
            if (!truncated && (flags & replay::kEndData))
            {
                run.endData.resize(PS2_VU1_DATA_SIZE);
                truncated = !get(run.endData.data(), run.endData.size());
            }
            run.ended = !truncated;
        }
        else
        {
            std::fprintf(stderr, "%s: unknown record %08x after run %zu\n", path, kind, out.runs.size());
            truncated = true;
        }
        if (truncated)
        {
            break;
        }
    }
    std::fclose(file);
    if (truncated)
    {
        std::fprintf(stderr, "%s: ends inside a record; the last run is left out\n", path);
        if (!out.runs.empty())
        {
            out.runs.pop_back();
        }
    }
    return true;
}

std::vector<GifEvent> eventsOf(const std::vector<std::vector<uint8_t>> &packets)
{
    // The vertex state before the run is the same for every candidate, so
    // each starts from a blank one.
    std::vector<GifEvent> events;
    GifState state;
    for (const auto &packet : packets)
    {
        decodeGifPacket(packet.data(), static_cast<uint32_t>(packet.size()), state, events);
    }
    return events;
}

// The recorded program on PS2Recomp's VU1: its compiled routines, or its
// interpreter alone.
class Vu1Engine final : public Candidate
{
public:
    explicit Vu1Engine(bool compiled) : m_compiled(compiled)
    {
        if (!m_memory.initialize())
        {
            std::fprintf(stderr, "PS2Memory::initialize failed\n");
            std::exit(2);
        }
        m_vram.resize(PS2_GS_VRAM_SIZE);
        m_gs.init(m_vram.data(), static_cast<uint32_t>(m_vram.size()));
        m_memory.setGifArbiter(nullptr);
        m_memory.setGifPacketCallback([this](const uint8_t *data, uint32_t size) {
            if (m_packets)
            {
                m_packets->emplace_back(data, data + size);
            }
        });
        m_vu.setCompiledExecutionEnabled(compiled);
    }

    const char *name() const override { return m_compiled ? "compiled" : "interpreter"; }
    bool accepts(const Run &) const override { return true; }
    const VU1State *endState() const override { return &m_vu.state(); }
    uint64_t lastNanos() const override { return m_lastNanos; }

    // Single-steps the runs while set, counting each pair that ran, and
    // hashing the run's path into `path`.
    void setCoverage(std::map<uint32_t, uint64_t> *hits, uint64_t *path)
    {
        m_hits = hits;
        m_path = path;
    }

    void run(const Recording &recording, const Run &run, std::vector<GifEvent> &events) override
    {
        if (!m_loaded || m_codeHash != run.header.codeHash)
        {
            const auto found = recording.codes.find(run.header.codeHash);
            if (found == recording.codes.end())
            {
                std::fprintf(stderr, "run %u: code image %016" PRIx64 " missing from the file\n", run.header.index,
                             run.header.codeHash);
                std::exit(2);
            }
            std::memcpy(m_memory.getVU1Code(), found->second.data(), PS2_VU1_CODE_SIZE);
            const uint64_t before = m_memory.getVU1CodeGeneration();
            uint32_t first;
            std::memcpy(&first, found->second.data(), sizeof(first));
            m_memory.write32(kVu1CodeAddress, first);
            if (m_memory.getVU1CodeGeneration() == before)
            {
                std::fprintf(stderr, "a write to 0x%08x did not reach VU1 micro memory\n", kVu1CodeAddress);
                std::exit(2);
            }
            m_codeHash = run.header.codeHash;
            m_loaded = true;
        }
        std::memcpy(m_memory.getVU1Data(), run.data.data(), PS2_VU1_DATA_SIZE);
        m_vu.state() = run.state;
        std::vector<std::vector<uint8_t>> packets;
        m_packets = &packets;
        const uint32_t mxcsr = _mm_getcsr();
        _mm_setcsr(run.header.mxcsr);
        const auto started = std::chrono::steady_clock::now();
        if (m_hits)
        {
            step(run);
        }
        else if (run.header.flags & replay::kRunResume)
        {
            m_vu.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE, m_memory.getVU1Data(), PS2_VU1_DATA_SIZE, m_gs,
                        &m_memory, run.header.top, run.header.itop, 65536);
        }
        else
        {
            m_vu.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE, m_memory.getVU1Data(), PS2_VU1_DATA_SIZE, m_gs,
                         &m_memory, run.header.startPC, run.header.top, run.header.itop, 65536);
        }
        m_lastNanos = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
        _mm_setcsr(mxcsr);
        m_packets = nullptr;
        events = eventsOf(packets);
    }

private:
    // One cycle at a time: a pair ran when the PC moved on, or the run ended.
    void step(const Run &run)
    {
        bool first = !(run.header.flags & replay::kRunResume);
        for (uint32_t cycles = 0; cycles < 65536u; ++cycles)
        {
            const uint32_t pc = first ? run.header.startPC : m_vu.state().pc;
            if (first)
            {
                m_vu.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE, m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                             m_gs, &m_memory, run.header.startPC, run.header.top, run.header.itop, 1);
                first = false;
            }
            else
            {
                m_vu.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE, m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                            m_gs, &m_memory, run.header.top, run.header.itop, 1);
            }
            if (m_vu.state().pc != pc || m_vu.lastRunEnded())
            {
                ++(*m_hits)[pc];
                *m_path = (*m_path ^ pc) * 1099511628211ull;
            }
            if (m_vu.lastRunEnded())
            {
                return;
            }
        }
    }

    uint64_t m_lastNanos = 0u;
    std::map<uint32_t, uint64_t> *m_hits = nullptr;
    uint64_t *m_path = nullptr;
    bool m_compiled;
    PS2Memory m_memory;
    std::vector<uint8_t> m_vram;
    GS m_gs;
    VU1Interpreter m_vu{VU1Interpreter::Unit::VU1};
    std::vector<std::vector<uint8_t>> *m_packets = nullptr;
    uint64_t m_codeHash = 0u;
    bool m_loaded = false;
};

// Whether the registers a run ended with are the recorded ones: the vector
// and integer registers, ACC, Q, P and I, bit for bit.
bool sameRegisters(const VU1State &a, const VU1State &b)
{
    return std::memcmp(a.vf, b.vf, sizeof(a.vf)) == 0 && std::memcmp(a.vi, b.vi, sizeof(a.vi)) == 0 &&
           std::memcmp(a.acc, b.acc, sizeof(a.acc)) == 0 && std::memcmp(&a.q, &b.q, sizeof(a.q)) == 0 &&
           std::memcmp(&a.p, &b.p, sizeof(a.p)) == 0 && std::memcmp(&a.i, &b.i, sizeof(a.i)) == 0;
}

struct ProgramResult
{
    uint32_t runs = 0;
    uint32_t resumes = 0;
    uint32_t identical = 0; // runs whose events all match within the tolerance
    uint32_t registers = 0; // runs whose end registers differ
    uint64_t kicks = 0;
    uint64_t nanos = 0;
    Comparison comparison;
};

constexpr int64_t kShowFirstDifferences = -2;

struct Options
{
    std::vector<const char *> paths;
    std::string candidate = "all";
    uint32_t maxRuns = UINT32_MAX;
    bool filtered = false;
    uint64_t programHash = 0u;
    uint32_t programPC = 0u;
    int64_t show = -1;
    const char *coverage = nullptr;
    bool time = false;
    bool inventory = false;
    // Synthetic checks for pieces no recording holds (--synth): the code
    // patched with pieces from raw images, data quads set, and the reference
    // the interpreter's run of the result instead of the recorded kicks.
    bool synth = false;
    // --synth-floats: each chunk's int16 positions, normals and UVs turned into the floats B would have made.
    bool floats = false;
    bool floatColors = false; // --synth-float-colors: the colors section as floats (int16 / 32768 * 256)
    // --synth-bones ENTRY: each chunk enters the skinning B at ENTRY pairs
    // (76.z set, TOP+1.w), its weights section widened to four bones (the
    // bones it had, repeated, weighted 100, 70, 55, 30 / 255).
    int32_t bonesEntry = -1;
    // --synth-list: each chunk drawn as a triangle list (TOP.x's PRIM type
    // 3), its count (TOP.y, and NLOOP in TOP.z) cut to whole triangles.
    bool list = false;
    struct CodePatch
    {
        std::string file;
        uint32_t start, end;
    };
    std::vector<CodePatch> patches;
    struct QuadPoke
    {
        uint32_t quad;
        float value[4];
    };
    std::vector<QuadPoke> pokes;
    Tolerance tolerance;
};

void usage()
{
    std::fprintf(stderr,
                 "usage: rg_vu1_oracle FILE... [--candidate compiled|interpreter|all|native] [--runs N]\n"
                 "                      [--program HASH:PC] [--show RUN|diff] [--tol-xy N] [--tol-z N]\n"
                 "                      [--tol-rgba N] [--tol-uv N] [--tol-stq F] [--time] [--inventory]\n");
}

bool parse(int argc, char **argv, Options &options)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--candidate" && hasValue)
            options.candidate = argv[++i];
        else if (arg == "--runs" && hasValue)
            options.maxRuns = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (arg == "--program" && hasValue)
        {
            char *end = nullptr;
            options.programHash = std::strtoull(argv[++i], &end, 16);
            if (!end || *end != ':')
                return false;
            options.programPC = static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 16));
            options.filtered = true;
        }
        else if (arg == "--show" && hasValue)
        {
            const std::string value = argv[++i];
            options.show = value == "diff" ? kShowFirstDifferences : std::strtoll(value.c_str(), nullptr, 0);
        }
        else if (arg == "--synth")
            options.synth = true;
        else if (arg == "--synth-code" && hasValue)
        {
            // FILE:START:END, bytes in hex, FILE a 16 KiB image (tools/vu1/piece.py).
            const std::string value = argv[++i];
            const size_t b = value.rfind(':'), a = b == std::string::npos ? b : value.rfind(':', b - 1);
            if (a == std::string::npos || b == std::string::npos)
                return false;
            options.patches.push_back({value.substr(0, a),
                                       static_cast<uint32_t>(std::strtoul(value.c_str() + a + 1, nullptr, 16)),
                                       static_cast<uint32_t>(std::strtoul(value.c_str() + b + 1, nullptr, 16))});
            options.synth = true;
        }
        else if (arg == "--synth-float-colors")
        {
            options.floatColors = true;
            options.floats = true;
            options.synth = true;
        }
        else if (arg == "--synth-bones" && hasValue)
        {
            options.bonesEntry = static_cast<int32_t>(std::strtol(argv[++i], nullptr, 0));
            options.synth = true;
        }
        else if (arg == "--synth-list")
        {
            options.list = true;
            options.synth = true;
        }
        else if (arg == "--synth-floats")
        {
            options.floats = true;
            options.synth = true;
        }
        else if (arg == "--synth-quad" && hasValue)
        {
            // N:x,y,z,w (floats): quad N of every run's data memory.
            const std::string value = argv[++i];
            Options::QuadPoke poke{static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 0)), {0, 0, 0, 0}};
            const char *at = std::strchr(value.c_str(), ':');
            for (int k = 0; at && k < 4; ++k)
            {
                poke.value[k] = std::strtof(at + 1, nullptr);
                at = std::strchr(at + 1, ',');
                if (!at && k < 3)
                    break;
            }
            options.pokes.push_back(poke);
            options.synth = true;
        }
        else if (arg == "--time")
            options.time = true;
        else if (arg == "--inventory")
            options.inventory = true;
        else if (arg == "--coverage" && hasValue)
            options.coverage = argv[++i];
        else if (arg == "--tol-xy" && hasValue)
            options.tolerance.xy = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (arg == "--tol-z" && hasValue)
            options.tolerance.z = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (arg == "--tol-rgba" && hasValue)
            options.tolerance.rgba = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (arg == "--tol-uv" && hasValue)
            options.tolerance.uv = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (arg == "--tol-stq" && hasValue)
            options.tolerance.stq = std::strtof(argv[++i], nullptr);
        else if (arg[0] != '-')
            options.paths.push_back(argv[i]);
        else
            return false;
    }
    return !options.paths.empty() && (!options.coverage || options.filtered);
}

void showRun(const Run &run, const std::vector<GifEvent> &reference, const std::vector<GifEvent> &candidate,
             const char *name, int64_t from)
{
    std::printf("run %u (%s of %016" PRIx64 ":%x, list %u, %zu packets): recorded, then %s\n", run.header.index,
                (run.header.flags & replay::kRunResume) ? "MSCNT" : "MSCAL", run.programHash, run.programPC,
                run.header.list, run.kicks.size(), name);
    const size_t first = from > 4 ? static_cast<size_t>(from - 4) : 0u;
    const size_t last = std::min(std::max(reference.size(), candidate.size()), first + 24u);
    for (size_t i = first; i < last; ++i)
    {
        const std::string a = i < reference.size() ? describe(reference[i]) : "-";
        const std::string b = i < candidate.size() ? describe(candidate[i]) : "-";
        std::printf("%6zu %s %s\n%6s %s\n", i, a == b ? " " : "*", a.c_str(), "", b.c_str());
    }
}

// Every program of the recording by its share of the vertices kicked, with
// the pieces src/vu1_native.cpp knows in it: the work list for the next
// native programs (docs/VU1-NATIVE.md).
int inventory(const Recording &recording)
{
    struct Entry
    {
        uint32_t runs = 0u, chunks = 0u;
        uint64_t packets = 0u, vertices = 0u;
        std::string pieces;
    };
    std::map<std::pair<uint64_t, uint32_t>, Entry> programs;
    uint64_t total = 0u;
    for (const Run &run : recording.runs)
    {
        Entry &entry = programs[{run.programHash, run.programPC}];
        if (entry.runs++ == 0u)
        {
            const auto code = recording.codes.find(run.programHash);
            entry.pieces = code == recording.codes.end()
                ? "?" : rg::vu1native::describePieces(code->second.data(), run.programPC);
        }
        entry.chunks += (run.header.flags & replay::kRunResume) ? 1u : 0u;
        entry.packets += run.kicks.size();
        for (const GifEvent &event : eventsOf(run.kicks))
        {
            entry.vertices += event.kind == GifEvent::Kind::Vertex ? 1u : 0u;
        }
    }
    for (const auto &[key, entry] : programs)
    {
        total += entry.vertices;
    }
    std::vector<std::pair<std::pair<uint64_t, uint32_t>, Entry>> sorted(programs.begin(), programs.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto &a, const auto &b) { return a.second.vertices > b.second.vertices; });
    std::printf("%-24s %7s %7s %8s %9s %6s  %s\n", "program", "runs", "mscnt", "packets", "vertices", "share",
                "pieces known to src/vu1_native.cpp, by slot");
    for (const auto &[key, entry] : sorted)
    {
        std::printf("%016" PRIx64 ":%-6x %7u %7u %8" PRIu64 " %9" PRIu64 " %5.1f%%  %s\n", key.first, key.second,
                    entry.runs, entry.chunks, entry.packets, entry.vertices,
                    total ? 100.0 * entry.vertices / total : 0.0, entry.pieces.c_str());
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!parse(argc, argv, options))
    {
        usage();
        return 2;
    }
    Recording recording;
    for (const char *path : options.paths)
    {
        if (!readRecording(path, options.maxRuns, recording))
        {
            return 2;
        }
    }
    for (const Options::CodePatch &patch : options.patches)
    {
        std::vector<uint8_t> image(PS2_VU1_CODE_SIZE);
        std::FILE *f = std::fopen(patch.file.c_str(), "rb");
        if (!f || std::fread(image.data(), 1, image.size(), f) != image.size())
        {
            std::fprintf(stderr, "cannot read the image %s\n", patch.file.c_str());
            return 2;
        }
        std::fclose(f);
        for (auto &[hash, code] : recording.codes)
            if (!options.filtered || hash == options.programHash)
                std::memcpy(code.data() + patch.start, image.data() + patch.start, patch.end - patch.start);
    }
    for (Run &run : recording.runs)
        for (const Options::QuadPoke &poke : options.pokes)
            if (!options.filtered || run.programHash == options.programHash)
                std::memcpy(run.data.data() + poke.quad * 16u, poke.value, 16u);
    if (options.bonesEntry >= 0)
    {
        for (Run &run : recording.runs)
        {
            if (options.filtered && run.programHash != options.programHash)
                continue;
            auto word = [&](uint32_t q, uint32_t lane) -> uint32_t {
                uint32_t v;
                std::memcpy(&v, run.data.data() + (q & 1023u) * 16u + lane * 4u, 4u);
                return v;
            };
            auto set = [&](uint32_t q, uint32_t lane, uint32_t v) {
                std::memcpy(run.data.data() + (q & 1023u) * 16u + lane * 4u, &v, 4u);
            };
            const uint32_t top = run.header.top;
            const uint32_t had = (word(76u, 2) & 0xFFFFu) != 0u ? word(top + 1u, 3) & 0xFFFFu : 0u;
            set(76u, 2, 1u);
            if (!(run.header.flags & replay::kRunResume))
                continue;
            set(top + 1u, 3, static_cast<uint32_t>(options.bonesEntry));
            const uint32_t count = word(top, 1) & 0xFFFFu;
            const uint32_t weights = top + 2u + (word(top + 1u, 2) & 0xFFFFu);
            static constexpr uint32_t kWeights[4] = {100u, 70u, 55u, 30u};
            for (uint32_t k = 0; k < count; ++k)
            {
                const uint32_t q = weights + 2u * k;
                const uint32_t a = word(q, 0) & 0xFFFFu, b = had != 0u ? word(q, 1) & 0xFFFFu : a;
                const uint32_t index[4] = {a, b, b, a};
                for (uint32_t i = 0; i < 4u; ++i)
                {
                    set(q, i, (word(q, i) & 0xFFFF0000u) | index[i]);
                    set(q + 1u, i, kWeights[i]);
                }
            }
        }
    }
    if (options.list)
    {
        for (Run &run : recording.runs)
        {
            if (!(run.header.flags & replay::kRunResume) || (options.filtered && run.programHash != options.programHash))
                continue;
            uint8_t *top = run.data.data() + (run.header.top & 1023u) * 16u;
            uint32_t prim, count;
            std::memcpy(&prim, top, 4u);
            std::memcpy(&count, top + 4u, 4u);
            prim = (prim & ~7u) | 3u;
            count = (count & 0xFFFF0000u) | ((count & 0xFFFFu) / 3u * 3u);
            // TOP.z is the GIF tag's word: NLOOP the count, EOP above it.
            uint32_t tag;
            std::memcpy(&tag, top + 8u, 4u);
            tag = (tag & ~0x7FFFu) | (count & 0xFFFFu);
            std::memcpy(top, &prim, 4u);
            std::memcpy(top + 4u, &count, 4u);
            std::memcpy(top + 8u, &tag, 4u);
        }
    }
    if (options.floats)
    {
        for (Run &run : recording.runs)
        {
            if (!(run.header.flags & replay::kRunResume) || (options.filtered && run.programHash != options.programHash))
                continue;
            auto word = [&](uint32_t q, uint32_t lane) -> int32_t {
                int32_t v;
                std::memcpy(&v, run.data.data() + (q & 1023u) * 16u + lane * 4u, 4u);
                return v;
            };
            auto put = [&](uint32_t q, uint32_t lane, float f) {
                std::memcpy(run.data.data() + (q & 1023u) * 16u + lane * 4u, &f, 4u);
            };
            const uint32_t top = run.header.top;
            const uint32_t count = static_cast<uint32_t>(word(top, 1)) & 0xFFFFu;
            const uint32_t positions = top + 2u;
            const uint32_t normals = positions + (static_cast<uint32_t>(word(top, 3)) & 0xFFFFu);
            const uint32_t uvs = positions + (static_cast<uint32_t>(word(top + 1u, 0)) & 0xFFFFu);
            const uint32_t colors = positions + (static_cast<uint32_t>(word(top + 1u, 1)) & 0xFFFFu);
            float scale[3][4];
            for (int q = 0; q < 3; ++q)
                std::memcpy(scale[q], run.data.data() + (77u + q) * 16u, 16u);
            for (uint32_t k = 0; k < count; ++k)
            {
                for (uint32_t i = 0; i < 3u; ++i)
                {
                    put(positions + k, i, static_cast<float>(word(positions + k, i)) / 32768.0f * scale[0][i]);
                    put(normals + k, i, static_cast<float>(word(normals + k, i)) / 32768.0f * scale[1][i]);
                }
                if (options.floatColors)
                    for (uint32_t i = 0; i < 4u; ++i)
                        put(colors + k, i, static_cast<float>(word(colors + k, i)) / 32768.0f * 256.0f);
                for (uint32_t i = 0; i < 2u; ++i)
                    put(uvs + k, i, static_cast<float>(word(uvs + k, i)) / 32768.0f * scale[2][i]);
            }
        }
    }
    std::unique_ptr<Vu1Engine> synthReference;
    if (options.synth)
        synthReference = std::make_unique<Vu1Engine>(false);
    uint32_t resumes = 0u;
    uint64_t kicks = 0u;
    for (const Run &run : recording.runs)
    {
        resumes += (run.header.flags & replay::kRunResume) ? 1u : 0u;
        kicks += run.kicks.size();
    }
    std::printf("%s%s: %zu runs (%u continuations), %" PRIu64 " packets, %zu code images, lists %u-%u\n",
                options.paths[0], options.paths.size() > 1 ? " and the rest" : "", recording.runs.size(), resumes, kicks, recording.codes.size(),
                recording.runs.empty() ? 0u : recording.runs.front().header.list,
                recording.runs.empty() ? 0u : recording.runs.back().header.list);

    if (options.inventory)
    {
        return inventory(recording);
    }

    std::vector<std::unique_ptr<Candidate>> candidates;
    std::map<uint32_t, uint64_t> hits;
    std::map<uint64_t, uint32_t> paths;
    uint64_t path = 0u;
    Vu1Engine *stepper = nullptr;
    if (options.coverage)
    {
        options.candidate = "interpreter";
    }
    if (options.candidate == "compiled" || options.candidate == "all")
        candidates.push_back(std::make_unique<Vu1Engine>(true));
    if (options.candidate == "interpreter" || options.candidate == "all")
    {
        candidates.push_back(std::make_unique<Vu1Engine>(false));
        stepper = options.coverage ? static_cast<Vu1Engine *>(candidates.back().get()) : nullptr;
    }
    if (options.candidate == "native")
        candidates.push_back(makeNative());
    if (candidates.empty())
    {
        usage();
        return 2;
    }

    bool allMatch = true;
    for (const auto &candidate : candidates)
    {
        std::map<std::pair<uint64_t, uint32_t>, ProgramResult> programs;
        for (const Run &run : recording.runs)
        {
            std::vector<GifEvent> events;
            const bool wanted = !options.filtered ||
                                (run.programHash == options.programHash && run.programPC == options.programPC);
            if (stepper)
            {
                if (!(run.header.flags & replay::kRunResume) && path != 0u)
                {
                    ++paths[path];
                    path = 0u;
                }
                stepper->setCoverage(wanted ? &hits : nullptr, &path);
            }
            std::vector<GifEvent> synthEvents;
            if (synthReference)
                synthReference->run(recording, run, synthEvents);
            candidate->run(recording, run, events);
            if (!wanted || !candidate->accepts(run))
            {
                continue;
            }
            const std::vector<GifEvent> reference = synthReference ? synthEvents : eventsOf(run.kicks);
            const Comparison comparison = compare(reference, events, options.tolerance);
            ProgramResult &result = programs[{run.programHash, run.programPC}];
            ++result.runs;
            result.resumes += (run.header.flags & replay::kRunResume) ? 1u : 0u;
            result.kicks += run.kicks.size();
            result.nanos += candidate->lastNanos();
            result.comparison.add(comparison);
            const bool same = comparison.mismatched == 0u && comparison.countDifference == 0;
            result.identical += same ? 1u : 0u;
            const VU1State *end = candidate->endState();
            const bool registersDiffer = end && run.ended && !sameRegisters(*end, run.endState);
            result.registers += registersDiffer ? 1u : 0u;
            if (options.show == static_cast<int64_t>(run.header.index) ||
                (options.show == kShowFirstDifferences && !same && result.runs - result.identical == 1u))
            {
                showRun(run, reference, events, candidate->name(), comparison.firstMismatch);
            }
        }

        ProgramResult total;
        std::printf("\n%s\n%-24s %7s %6s %8s %9s %9s %9s %6s %8s %6s %5s %9s %8s%s\n", candidate->name(), "program",
                    "runs", "mscnt", "packets", "vertices", "same", "differ", "regs", "max xy", "max z", "rgba",
                    "uv/16", "stq", options.time ? "   us/run" : "");
        for (const auto &[key, result] : programs)
        {
            const Comparison &c = result.comparison;
            std::printf("%016" PRIx64 ":%-6x %7u %6u %8" PRIu64 " %9" PRIu64 " %9u %9u %6u %8.4f %6u %5u %9u %8.2g",
                        key.first, key.second, result.runs, result.resumes, result.kicks, c.vertices,
                        result.identical, result.runs - result.identical, result.registers, c.maxXy / 16.0, c.maxZ,
                        c.maxRgba, c.maxUv, c.maxStq);
            if (options.time)
                std::printf(" %9.2f", result.runs ? result.nanos / 1000.0 / result.runs : 0.0);
            std::printf("\n");
            total.runs += result.runs;
            total.resumes += result.resumes;
            total.identical += result.identical;
            total.registers += result.registers;
            total.kicks += result.kicks;
            total.nanos += result.nanos;
            total.comparison.add(result.comparison);
        }
        std::printf("%-24s %7u %6u %8" PRIu64 " %9" PRIu64 " %9u %9u %6u %8.4f %6u %5u %9u %8.2g\n", "all",
                    total.runs, total.resumes, total.kicks, total.comparison.vertices, total.identical,
                    total.runs - total.identical, total.registers, total.comparison.maxXy / 16.0,
                    total.comparison.maxZ, total.comparison.maxRgba, total.comparison.maxUv,
                    total.comparison.maxStq);
        if (options.time)
        {
            std::printf("time: %.1f ms for all %u runs taken, %.2f us a run%s%s\n", total.nanos / 1e6, total.runs,
                        total.runs ? total.nanos / 1000.0 / total.runs : 0.0,
                        candidate->timing().empty() ? "" : "; ", candidate->timing().c_str());
        }
        for (const auto &[reason, count] : candidate->declined())
        {
            std::printf("%9u runs  %s\n", count, reason.c_str());
        }
        allMatch = allMatch && total.identical == total.runs;
    }
    if (stepper)
    {
        if (path != 0u)
        {
            ++paths[path];
        }
        std::FILE *out = std::fopen(options.coverage, "w");
        if (!out)
        {
            std::fprintf(stderr, "cannot write %s\n", options.coverage);
            return 2;
        }
        for (const auto &[pc, count] : hits)
        {
            std::fprintf(out, "%x %" PRIu64 "\n", pc, count);
        }
        std::fclose(out);
        std::vector<uint32_t> counts;
        for (const auto &[hash, count] : paths)
        {
            counts.push_back(count);
        }
        std::sort(counts.rbegin(), counts.rend());
        std::printf("\ncoverage: %zu pairs ran, into %s; %zu distinct paths (an MSCAL and its MSCNTs), the "
                    "commonest taken by", hits.size(), options.coverage, paths.size());
        for (size_t i = 0; i < std::min<size_t>(counts.size(), 8u); ++i)
        {
            std::printf(" %u", counts[i]);
        }
        std::printf("\n");
    }
    return allMatch ? 0 : 1;
}
