#include "vu1_replay.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <xmmintrin.h>

#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"
#include "vu1_native.h"

namespace rg::vu1replay
{
namespace
{

const char *const g_path = std::getenv("RG_VU1_REPLAY");
const char *const g_harvest = std::getenv("RG_VU1_HARVEST");

uint32_t envNumber(const char *name, uint32_t fallback)
{
    const char *value = std::getenv(name);
    return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : fallback;
}

uint64_t fnv(const uint8_t *bytes, uint32_t size)
{
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t i = 0; i < size; ++i)
    {
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    return hash;
}

// Only the thread that runs VU1 calls in, one at a time.
struct Recorder
{
    std::FILE *file = nullptr;
    uint32_t from = envNumber("RG_VU1_REPLAY_FROM", 1u);
    uint32_t lists = envNumber("RG_VU1_REPLAY_LISTS", 30u);
    bool after = envNumber("RG_VU1_REPLAY_AFTER", 0u) != 0u;
    uint32_t firstList = 0u;
    uint32_t lastList = 0u;
    bool finished = false;
    // A run being recorded, between begin and end.
    bool recording = false;
    uint32_t runs = 0u;
    uint64_t kicks = 0u;
    uint64_t generation = ~0ull;
    uint64_t codeHash = 0u;
    std::unordered_set<uint64_t> written;

    // A run that ends inside the window keeps what it wrote.
    ~Recorder() { close(); }

    void put(const void *bytes, size_t size) { std::fwrite(bytes, 1, size, file); }
    void put32(uint32_t value) { put(&value, sizeof(value)); }

    bool open()
    {
        file = std::fopen(g_path, "wb");
        if (!file)
        {
            std::fprintf(stderr, "[vu1replay] cannot write %s\n", g_path);
            finished = true;
            return false;
        }
        std::setvbuf(file, nullptr, _IOFBF, 16u << 20);
        put32(kMagic);
        put32(kVersion);
        put32(sizeof(VU1State));
        put32(PS2_VU1_CODE_SIZE);
        put32(PS2_VU1_DATA_SIZE);
        return true;
    }

    void close()
    {
        finished = true;
        recording = false;
        if (file)
        {
            std::fclose(file);
            file = nullptr;
            std::fprintf(stderr, "[vu1replay] %u runs, %llu kicks, %zu code images from lists %u-%u in %s\n", runs,
                         static_cast<unsigned long long>(kicks), written.size(), firstList, lastList, g_path);
        }
    }
};

Recorder &recorder()
{
    static Recorder instance;
    return instance;
}

void putRun(std::FILE *file, bool resume, uint32_t startPC, uint32_t top, uint32_t itop, uint32_t list,
            uint32_t index, uint64_t codeHash, const uint8_t *data, const VU1State &state)
{
    RunHeader header{};
    header.flags = (resume ? kRunResume : 0u) | (state.dBitEnabled ? kRunDBit : 0u) |
                   (state.tBitEnabled ? kRunTBit : 0u);
    header.startPC = startPC;
    header.top = top;
    header.itop = itop;
    header.list = list;
    header.index = index;
    header.mxcsr = _mm_getcsr();
    header.codeHash = codeHash;
    const uint32_t kind = kRun;
    std::fwrite(&kind, sizeof(kind), 1, file);
    std::fwrite(&header, sizeof(header), 1, file);
    std::fwrite(&state, sizeof(state), 1, file);
    std::fwrite(data, 1, PS2_VU1_DATA_SIZE, file);
}

// RG_VU1_HARVEST: samples of the programs DIR has no file for. Only the
// thread that runs VU1 calls in.
struct Harvester
{
    struct Program
    {
        std::string path;
        bool known = false; // DIR had its file before this run
        uint32_t samples = 0u;
        uint32_t lastList = 0u;
        std::vector<uint64_t> images; // code images written to its file
    };

    uint32_t maxSamples = envNumber("RG_VU1_HARVEST_SAMPLES", 4u);
    uint32_t maxChunks = envNumber("RG_VU1_HARVEST_CHUNKS", 64u);
    uint32_t spacing = envNumber("RG_VU1_HARVEST_SPACING", 300u);
    std::unordered_map<uint64_t, Program> programs;
    std::FILE *file = nullptr; // the sample being written
    uint64_t image = 0u;
    uint32_t chunks = 0u;
    uint32_t runs = 0u;
    // A sample is open, and a run of it is being recorded (the packets
    // between runs are the game's own, not the program's).
    bool recording = false;
    bool inRun = false;
    bool started = false;

    ~Harvester() { closeSample(); }

    void closeSample()
    {
        recording = false;
        inRun = false;
        if (file)
        {
            std::fclose(file);
            file = nullptr;
        }
    }

    // The program at an MSCAL: its entry and 64 bytes at each code slot's
    // start, which tell its pieces apart without hashing the whole image. The
    // base program (entry 0) is in its own slot whatever the others hold.
    static uint64_t key(const uint8_t *code, uint32_t startPC)
    {
        static constexpr uint32_t kSlots[] = {0x0000u, 0x0320u, 0x0960u, 0x10E0u, 0x1900u, 0x2620u, 0x39D0u};
        uint64_t hash = fnv(reinterpret_cast<const uint8_t *>(&startPC), sizeof(startPC));
        for (const uint32_t slot : kSlots)
        {
            if (startPC == 0u && slot != 0u)
            {
                break;
            }
            for (uint32_t i = 0; i < 64u; ++i)
            {
                hash = (hash ^ code[slot + i]) * 1099511628211ull;
            }
        }
        return hash;
    }

    void begin(bool resume, uint32_t startPC, uint32_t top, uint32_t itop, uint32_t list, const uint8_t *code,
               const uint8_t *data, const VU1State &state)
    {
        if (resume)
        {
            if (recording && chunks < maxChunks)
            {
                putRun(file, true, startPC, top, itop, list, runs++, image, data, state);
                ++chunks;
                inRun = true;
            }
            else
            {
                recording = false;
            }
            return;
        }
        closeSample();
        if (!started)
        {
            started = true;
            std::error_code error;
            std::filesystem::create_directories(g_harvest, error);
        }
        const auto [entry, inserted] = programs.try_emplace(key(code, startPC));
        Program &program = entry->second;
        if (inserted)
        {
            char name[64];
            std::snprintf(name, sizeof(name), "/%016llx_%x.vu1r",
                          static_cast<unsigned long long>(fnv(code, PS2_VU1_CODE_SIZE)), startPC);
            program.path = std::string(g_harvest) + name;
            std::error_code error;
            program.known = std::filesystem::exists(program.path, error);
            if (!program.known)
            {
                std::fprintf(stderr, "[harvest] new program %s, entry 0x%x: %s\n", name + 1, startPC,
                             rg::vu1native::describePieces(code, startPC).c_str());
            }
        }
        if (program.known || program.samples >= maxSamples ||
            (program.samples != 0u && list - program.lastList < spacing))
        {
            return;
        }
        file = std::fopen(program.path.c_str(), "ab");
        if (!file)
        {
            program.known = true;
            return;
        }
        if (program.samples == 0u)
        {
            const uint32_t header[5] = {kMagic, kVersion, sizeof(VU1State), PS2_VU1_CODE_SIZE, PS2_VU1_DATA_SIZE};
            std::fwrite(header, sizeof(header), 1, file);
        }
        image = fnv(code, PS2_VU1_CODE_SIZE);
        if (std::find(program.images.begin(), program.images.end(), image) == program.images.end())
        {
            program.images.push_back(image);
            const uint32_t kind = kCode;
            std::fwrite(&kind, sizeof(kind), 1, file);
            std::fwrite(&image, sizeof(image), 1, file);
            std::fwrite(code, 1, PS2_VU1_CODE_SIZE, file);
        }
        putRun(file, false, startPC, top, itop, list, runs++, image, data, state);
        ++program.samples;
        program.lastList = list;
        chunks = 0u;
        recording = true;
        inRun = true;
    }

    void kick(const uint8_t *data, uint32_t size)
    {
        if (!inRun)
        {
            return;
        }
        const uint32_t header[2] = {kKick, size};
        std::fwrite(header, sizeof(header), 1, file);
        std::fwrite(data, 1, size, file);
    }

    void end(const VU1State &state)
    {
        if (!inRun)
        {
            return;
        }
        inRun = false;
        const uint32_t header[2] = {kEnd, 0u};
        std::fwrite(header, sizeof(header), 1, file);
        std::fwrite(&state, sizeof(state), 1, file);
    }
};

Harvester &harvester()
{
    static Harvester instance;
    return instance;
}

void recordBegin(bool resume, uint32_t startPC, uint32_t top, uint32_t itop, uint32_t list, const uint8_t *code,
                 uint64_t codeGeneration, const uint8_t *data, const VU1State &state);
void recordKick(const uint8_t *data, uint32_t size);
void recordEnd(const VU1State &state, const uint8_t *data);

} // namespace

bool enabled()
{
    return g_path != nullptr || g_harvest != nullptr;
}

void begin(bool resume, uint32_t startPC, uint32_t top, uint32_t itop, uint32_t list, const uint8_t *code,
           uint64_t codeGeneration, const uint8_t *data, const VU1State &state)
{
    if (g_path)
    {
        recordBegin(resume, startPC, top, itop, list, code, codeGeneration, data, state);
    }
    if (g_harvest)
    {
        harvester().begin(resume, startPC, top, itop, list, code, data, state);
    }
}

void kick(const uint8_t *data, uint32_t size)
{
    if (g_path)
    {
        recordKick(data, size);
    }
    if (g_harvest)
    {
        harvester().kick(data, size);
    }
}

void end(const VU1State &state, const uint8_t *data)
{
    if (g_path)
    {
        recordEnd(state, data);
    }
    if (g_harvest)
    {
        harvester().end(state);
    }
}

namespace
{

void recordBegin(bool resume, uint32_t startPC, uint32_t top, uint32_t itop, uint32_t list, const uint8_t *code,
                 uint64_t codeGeneration, const uint8_t *data, const VU1State &state)
{
    Recorder &r = recorder();
    r.recording = false;
    if (r.finished)
    {
        return;
    }
    // Lists sent in normal mode carry no number: they belong to the one
    // around them.
    if (list != 0u)
    {
        if (!r.file)
        {
            // Whole programs only: the file starts with an MSCAL.
            if (list < r.from || resume || !r.open())
            {
                return;
            }
            r.firstList = list;
        }
        if (list - r.firstList >= r.lists)
        {
            r.close();
            return;
        }
        if (list != r.lastList)
        {
            r.lastList = list;
            std::fflush(r.file);
        }
    }
    else if (!r.file)
    {
        return;
    }
    if (codeGeneration != r.generation)
    {
        r.generation = codeGeneration;
        r.codeHash = fnv(code, PS2_VU1_CODE_SIZE);
    }
    if (r.written.insert(r.codeHash).second)
    {
        r.put32(kCode);
        r.put(&r.codeHash, sizeof(r.codeHash));
        r.put(code, PS2_VU1_CODE_SIZE);
    }
    RunHeader header{};
    header.flags = (resume ? kRunResume : 0u) | (state.dBitEnabled ? kRunDBit : 0u) |
                   (state.tBitEnabled ? kRunTBit : 0u);
    header.startPC = startPC;
    header.top = top;
    header.itop = itop;
    header.list = list;
    header.index = r.runs++;
    header.mxcsr = _mm_getcsr();
    header.codeHash = r.codeHash;
    r.put32(kRun);
    r.put(&header, sizeof(header));
    r.put(&state, sizeof(state));
    r.put(data, PS2_VU1_DATA_SIZE);
    r.recording = true;
}

void recordKick(const uint8_t *data, uint32_t size)
{
    Recorder &r = recorder();
    if (!r.recording)
    {
        return;
    }
    r.put32(kKick);
    r.put32(size);
    r.put(data, size);
    ++r.kicks;
}

} // namespace

void saveChunk(const char *program, const char *kind, const uint8_t *code, uint32_t programPC, uint32_t top,
               uint32_t itop, uint32_t list, const uint8_t *data, const VU1State &state,
               const std::vector<std::vector<uint8_t>> &packets, const VU1State &endState, bool atCall,
               const uint8_t *after)
{
    if (!g_harvest)
    {
        return;
    }
    static std::unordered_map<std::string, uint32_t> saved;
    static uint32_t files = 0u;
    uint32_t &count = saved[std::string(program) + ": " + kind];
    if (count >= 8u)
    {
        return;
    }
    ++count;
    std::error_code error;
    std::filesystem::create_directories(g_harvest, error);
    const uint64_t image = fnv(code, PS2_VU1_CODE_SIZE);
    char name[96];
    std::snprintf(name, sizeof(name), "/mismatch_%016llx_%u_%u.vu1r", static_cast<unsigned long long>(image),
                  list, files++);
    std::FILE *file = std::fopen((std::string(g_harvest) + name).c_str(), "wb");
    if (!file)
    {
        return;
    }
    const uint32_t header[5] = {kMagic, kVersion, sizeof(VU1State), PS2_VU1_CODE_SIZE, PS2_VU1_DATA_SIZE};
    std::fwrite(header, sizeof(header), 1, file);
    const uint32_t codeKind = kCode;
    std::fwrite(&codeKind, sizeof(codeKind), 1, file);
    std::fwrite(&image, sizeof(image), 1, file);
    std::fwrite(code, 1, PS2_VU1_CODE_SIZE, file);
    // The stand-in MSCAL: the strip programs' only ends, and names the
    // program for what follows.
    const uint32_t endHeader[2] = {kEnd, 0u};
    if (atCall)
    {
        putRun(file, false, programPC, top, itop, list, 0u, image, data, state);
    }
    else
    {
        putRun(file, false, programPC, top, itop, list, 0u, image, data, state);
        std::fwrite(endHeader, sizeof(endHeader), 1, file);
        std::fwrite(&state, sizeof(state), 1, file);
        putRun(file, true, 0u, top, itop, list, 1u, image, data, state);
    }
    for (const auto &packet : packets)
    {
        const uint32_t kick[2] = {kKick, static_cast<uint32_t>(packet.size())};
        std::fwrite(kick, sizeof(kick), 1, file);
        std::fwrite(packet.data(), 1, packet.size(), file);
    }
    const uint32_t endWithData[2] = {kEnd, kEndData};
    std::fwrite(endWithData, sizeof(endWithData), 1, file);
    std::fwrite(&endState, sizeof(endState), 1, file);
    std::fwrite(after, 1, PS2_VU1_DATA_SIZE, file);
    std::fclose(file);
    std::fprintf(stderr, "[harvest] %s differs (%s): saved %s\n", program, kind, name + 1);
}

namespace
{

void recordEnd(const VU1State &state, const uint8_t *data)
{
    Recorder &r = recorder();
    if (!r.recording)
    {
        return;
    }
    r.recording = false;
    r.put32(kEnd);
    r.put32(r.after ? kEndData : 0u);
    r.put(&state, sizeof(state));
    if (r.after)
    {
        r.put(data, PS2_VU1_DATA_SIZE);
    }
}

} // namespace

} // namespace rg::vu1replay
