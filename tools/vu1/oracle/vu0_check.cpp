// The native VU0 matrix program (src/vu0_native.cpp) against PS2Recomp's
// VU0 on synthetic inputs: N cases of random scale, angles (each zero, or
// minus zero, a quarter of the time, so every branch runs) and translation,
// and random other registers; every vf register, ACC, I and vi1-vi4 are
// compared bit for bit. The code is read from the player's disc, as the
// executable uploads it (three VIF0 MPGs), so no game code is kept here.
//   rg_vu0_check disc/SCUS_974.90 [CASES=100000]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"
#include "vu0_native.h"

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: rg_vu0_check SCUS_974.90 [CASES]\n");
        return 2;
    }
    const long cases = argc > 2 ? std::atol(argv[2]) : 100000;
    FILE *file = std::fopen(argv[1], "rb");
    if (!file)
        return 2;
    std::vector<uint8_t> elf;
    for (int c; (c = std::fgetc(file)) != EOF;)
        elf.push_back(static_cast<uint8_t>(c));
    std::fclose(file);
    std::vector<uint8_t> code(PS2_VU0_CODE_SIZE, 0u), data(PS2_VU0_DATA_SIZE, 0u);
    // The VIF0 MPG tags (file offset, pairs, first pair) of the upload.
    const struct { uint32_t tag, pairs, at; } uploads[] = {{0x34599c, 112, 0}, {0x345d24, 20, 200}, {0x345dec, 59, 300}};
    for (const auto &u : uploads)
    {
        uint32_t word;
        std::memcpy(&word, elf.data() + u.tag, 4);
        if (((word >> 24) & 0x7f) != 0x4a || ((word >> 16) & 0xff) != u.pairs || (word & 0xffff) != u.at)
        {
            std::fprintf(stderr, "no VIF0 MPG at 0x%x: another executable?\n", u.tag);
            return 2;
        }
        std::memcpy(code.data() + u.at * 8u, elf.data() + u.tag + 4u, u.pairs * 8u);
    }
    PS2Memory memory;
    if (!memory.initialize())
        return 2;
    std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE);
    GS gs;
    gs.init(vram.data(), static_cast<uint32_t>(vram.size()));
    VU1Interpreter vu{VU1Interpreter::Unit::VU0};
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> angle(-12.0f, 12.0f), wide(-1000.0f, 1000.0f), unit(0.01f, 8.0f);
    long differ = 0, branches[4] = {};
    for (long n = 0; n < cases; ++n)
    {
        float vf[32][4];
        for (auto &reg : vf)
            for (float &lane : reg)
                lane = wide(rng);
        vf[0][0] = vf[0][1] = vf[0][2] = 0.0f;
        vf[0][3] = 1.0f;
        for (int c = 0; c < 4; ++c)
        {
            vf[1][c] = unit(rng);
            const unsigned pick = rng() & 7u;
            vf[2][c] = pick == 0u ? 0.0f : pick == 1u ? -0.0f : angle(rng);
        }
        float acc[4] = {wide(rng), wide(rng), wide(rng), wide(rng)};
        float i = wide(rng);
        uint16_t vi[16];
        for (auto &r : vi)
            r = static_cast<uint16_t>(rng());
        vi[0] = 0;
        ++branches[(vf[2][0] == 0.0f && vf[2][1] == 0.0f ? 1 : 0) + (vf[2][2] == 0.0f ? 2 : 0)];

        VU1State &state = vu.state();
        vu.reset();
        std::memset(&state, 0, sizeof(state));
        std::memcpy(state.vf, vf, sizeof(vf));
        std::memcpy(state.acc, acc, sizeof(acc));
        state.i = i;
        for (int r = 0; r < 16; ++r)
            state.vi[r] = static_cast<int16_t>(vi[r]);
        vu.execute(code.data(), PS2_VU0_CODE_SIZE, data.data(), PS2_VU0_DATA_SIZE, gs, &memory, 0u, 0u, 0u, 4096u);

        float nvf[32][4];
        std::memcpy(nvf, vf, sizeof(vf));
        float nacc[4];
        std::memcpy(nacc, acc, sizeof(acc));
        float ni = i;
        uint16_t nvi[16];
        std::memcpy(nvi, vi, sizeof(vi));
        rg::vu0native::runProgram(nvf, nacc, ni, nvi);

        const char *what = nullptr;
        int reg = -1;
        for (int r = 1; r < 32 && !what; ++r)
            if (std::memcmp(nvf[r], state.vf[r], 16) != 0)
                what = "vf", reg = r;
        if (!what && std::memcmp(nacc, state.acc, 16) != 0)
            what = "ACC";
        if (!what && std::memcmp(&ni, &state.i, 4) != 0)
            what = "I";
        for (int r = 1; r < 16 && !what; ++r)
            if (nvi[r] != static_cast<uint16_t>(state.vi[r]))
                what = "vi", reg = r;
        if (what && ++differ <= 8)
        {
            std::printf("case %ld: %s%d differs; angles %g %g %g\n", n, what, reg, vf[2][0], vf[2][1], vf[2][2]);
            if (reg >= 0 && what[1] == 'f')
                std::printf("  VU0    %.9g %.9g %.9g %.9g\n  native %.9g %.9g %.9g %.9g\n", state.vf[reg][0],
                            state.vf[reg][1], state.vf[reg][2], state.vf[reg][3], nvf[reg][0], nvf[reg][1],
                            nvf[reg][2], nvf[reg][3]);
        }
    }
    std::printf("%ld cases (all rotations %ld, z only %ld, x and y only %ld, none %ld): %ld differ\n", cases,
                branches[0], branches[1], branches[2], branches[3], differ);
    return differ == 0 ? 0 : 1;
}
