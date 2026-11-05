#include "draw_state_mirror.h"

#include <algorithm>
#include <mutex>
#include <vector>

#include "gif_events.h"
#include "runtime/gs/gs_frontend.h"

#if defined(RG_HAS_SDLGPU)
#include "gfx/native/native_pipeline.h"
#endif

namespace rg::drawstate
{
namespace
{
// The front end's draw registers (patch 0031's GS::DrawRegisters) and the
// effective PRIM (PRMODECONT applied), as GS::writeRegisterUnlocked decodes
// them.
struct Mirror
{
    std::mutex mutex;
    GS::DrawRegisters r;
    GSPrimReg prim{};
};
Mirror g_mirror;

GSPrimReg decodePrim(uint64_t value)
{
    GSPrimReg prim{};
    prim.type = static_cast<GSPrimType>(value & 0x7u);
    prim.iip = ((value >> 3) & 1u) != 0u;
    prim.tme = ((value >> 4) & 1u) != 0u;
    prim.fge = ((value >> 5) & 1u) != 0u;
    prim.abe = ((value >> 6) & 1u) != 0u;
    prim.aa1 = ((value >> 7) & 1u) != 0u;
    prim.fst = ((value >> 8) & 1u) != 0u;
    prim.ctxt = ((value >> 9) & 1u) != 0u;
    prim.fix = ((value >> 10) & 1u) != 0u;
    return prim;
}

void decodeTex0(GSTex0Reg &t, uint64_t value)
{
    t.tbp0 = static_cast<uint32_t>(value & 0x3FFF);
    t.tbw = static_cast<uint8_t>((value >> 14) & 0x3F);
    t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
    t.tw = static_cast<uint8_t>((value >> 26) & 0xF);
    t.th = static_cast<uint8_t>((value >> 30) & 0xF);
    t.tcc = static_cast<uint8_t>((value >> 34) & 0x1);
    t.tfx = static_cast<uint8_t>((value >> 35) & 0x3);
    t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
    t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
    t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
    t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
    t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
}

// One register write, as the front end takes it. Caller holds the mutex.
void write(Mirror &m, uint8_t reg, uint64_t value)
{
    GS::DrawRegisters &r = m.r;
    const int ci = (reg & 1u) ? 1 : 0;
    switch (reg)
    {
    case 0x00:
        r.primRegister = decodePrim(value);
        if (r.prmodecont)
            m.prim = r.primRegister;
        else
            m.prim.type = r.primRegister.type;
        break;
    case 0x06: case 0x07:
        decodeTex0(r.ctx[ci].tex0, value);
        break;
    case 0x08: case 0x09: r.ctx[ci].clamp = value; break;
    case 0x14: case 0x15: r.ctx[ci].tex1 = value; break;
    case 0x16: case 0x17: {
        // TEX2: part of TEX0.
        auto &t = r.ctx[ci].tex0;
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        break;
    }
    case 0x18: case 0x19:
        r.ctx[ci].xyoffset.ofx = static_cast<uint16_t>(value & 0xFFFF);
        r.ctx[ci].xyoffset.ofy = static_cast<uint16_t>((value >> 32) & 0xFFFF);
        break;
    case 0x1A: {
        r.prmodecont = (value & 1) != 0;
        const GSPrimType type = r.primRegister.type;
        m.prim = r.prmodecont ? r.primRegister : r.prmodeRegister;
        m.prim.type = type;
        break;
    }
    case 0x1B:
        r.prmodeRegister = decodePrim(value);
        if (!r.prmodecont)
        {
            const GSPrimType type = r.primRegister.type;
            m.prim = r.prmodeRegister;
            m.prim.type = type;
        }
        break;
    case 0x1C:
        r.texclut.cbw = static_cast<uint8_t>(value & 0x3Fu);
        r.texclut.cou = static_cast<uint8_t>((value >> 6) & 0x3Fu);
        r.texclut.cov = static_cast<uint16_t>((value >> 12) & 0x3FFu);
        break;
    case 0x22: r.scanmsk = value; break;
    case 0x34: case 0x35: r.ctx[ci].miptbp1 = value; break;
    case 0x36: case 0x37: r.ctx[ci].miptbp2 = value; break;
    case 0x3B:
        r.texa.ta0 = static_cast<uint8_t>(value & 0xFFu);
        r.texa.aem = ((value >> 15) & 0x1u) != 0u;
        r.texa.ta1 = static_cast<uint8_t>((value >> 32) & 0xFFu);
        break;
    case 0x3D:
        r.fogR = static_cast<uint8_t>(value & 0xFFu);
        r.fogG = static_cast<uint8_t>((value >> 8) & 0xFFu);
        r.fogB = static_cast<uint8_t>((value >> 16) & 0xFFu);
        break;
    case 0x40: case 0x41:
        r.ctx[ci].scissor.x0 = static_cast<uint16_t>(value & 0x7FF);
        r.ctx[ci].scissor.x1 = static_cast<uint16_t>((value >> 16) & 0x7FF);
        r.ctx[ci].scissor.y0 = static_cast<uint16_t>((value >> 32) & 0x7FF);
        r.ctx[ci].scissor.y1 = static_cast<uint16_t>((value >> 48) & 0x7FF);
        break;
    case 0x42: case 0x43: r.ctx[ci].alpha = value; break;
    case 0x44: r.dimx = value; break;
    case 0x45: r.dthe = value; break;
    case 0x46: r.colclamp = value; break;
    case 0x47: r.ctx[0].test = value; break;
    case 0x48: r.ctx[1].test = value; break;
    case 0x49: r.pabe = (value & 1u) != 0u; break;
    case 0x4A: case 0x4B: r.ctx[ci].fba = value; break;
    case 0x4C: case 0x4D:
        r.ctx[ci].frame.fbp = static_cast<uint32_t>(value & 0x1FF);
        r.ctx[ci].frame.fbw = static_cast<uint32_t>((value >> 16) & 0x3F);
        r.ctx[ci].frame.psm = static_cast<uint8_t>((value >> 24) & 0x3F);
        r.ctx[ci].frame.fbmsk = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        break;
    case 0x4E: case 0x4F:
        r.ctx[ci].zbuf.zbp = value & 0x1FF;
        r.ctx[ci].zbuf.psm = ((value >> 24) & 0xF) | 0x30;
        r.ctx[ci].zbuf.zmask = (value >> 32) & 1;
        break;
    default:
        break;
    }
}
} // namespace

bool enabled()
{
#if defined(RG_HAS_SDLGPU)
    return dq8::gfx::native::enabled();
#else
    return false;
#endif
}

void observe(const uint8_t *data, uint32_t size)
{
    if (!enabled() || !data || size < 16u)
        return;
    // The vertex state a packet leaves for the next, per sending thread.
    thread_local rg::gif::GifState carried;
    thread_local std::vector<rg::gif::GifEvent> events;
    events.clear();
    rg::gif::decodeGifPacket(data, size, carried, events);
    std::lock_guard lock(g_mirror.mutex);
    for (const rg::gif::GifEvent &e : events)
        if (e.kind == rg::gif::GifEvent::Kind::Register)
            write(g_mirror, e.reg, e.value);
}

void readBack(const GS &gs)
{
    if (!enabled())
        return;
    std::lock_guard lock(g_mirror.mutex);
    g_mirror.r = gs.drawRegisters();
    GSPrimReg prim = g_mirror.r.prmodecont ? g_mirror.r.primRegister : g_mirror.r.prmodeRegister;
    prim.type = g_mirror.r.primRegister.type;
    g_mirror.prim = prim;
}

GSDrawState current()
{
    std::lock_guard lock(g_mirror.mutex);
    const GS::DrawRegisters &r = g_mirror.r;
    const GSPrimReg &prim = g_mirror.prim;
    GSDrawState state{};
    state.context = r.ctx[prim.ctxt ? 1 : 0];
    state.prim = prim;
    state.texa = r.texa;
    state.texclut = r.texclut;
    state.pabe = r.pabe;
    state.scanmsk = r.scanmsk;
    state.dimx = r.dimx;
    state.dthe = r.dthe;
    state.colclamp = r.colclamp;
    state.fogR = r.fogR;
    state.fogG = r.fogG;
    state.fogB = r.fogB;
    state.textureWidth = static_cast<uint16_t>(1u << std::min<uint32_t>(state.context.tex0.tw, 10u));
    state.textureHeight = static_cast<uint16_t>(1u << std::min<uint32_t>(state.context.tex0.th, 10u));
    const uint64_t tex1 = state.context.tex1;
    const uint8_t mmag = static_cast<uint8_t>((tex1 >> 5u) & 0x1u);
    const uint8_t mmin = static_cast<uint8_t>((tex1 >> 6u) & 0x7u);
    state.linearFilter = mmag != 0u || mmin == 1u || (mmin & 0x4u) != 0u;
    return state;
}

} // namespace rg::drawstate
