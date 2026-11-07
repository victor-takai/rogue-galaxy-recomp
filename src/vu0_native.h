#pragma once

#include <cstdint>

class PS2Runtime;

// The game's VU0 micro program (the matrix from scale, rotation and
// translation that MatFromSRTVu0 and MatRotateVu0 start with `vcallms 0`),
// run natively instead of on the VU0 (src/vu0_native.cpp). On with the
// native renderer; RG_VU0_NATIVE=0 off, 1 on without it.
namespace rg::vu0native
{
void install(PS2Runtime &runtime);
// Programs run natively so far.
uint64_t runs();
// The program on these registers, as the hook runs it (for
// tools/vu1/oracle/vu0_check.cpp).
void runProgram(float vf[32][4], float acc[4], float &i, uint16_t vi[16]);
} // namespace rg::vu0native
