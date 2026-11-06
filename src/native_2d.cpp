#include "native_2d.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "gif_events.h"
#include "native_producer.h"

namespace rg::native2d
{
namespace
{
std::vector<uint8_t> adPacket(const std::vector<std::pair<uint8_t, uint64_t>> &writes)
{
    std::vector<uint8_t> packet(16u + writes.size() * 16u);
    // NLOOP writes, EOP, PACKED, one register: A+D.
    const uint64_t tag[2] = {static_cast<uint64_t>(writes.size()) | (1ull << 15) | (1ull << 60), 0xEull};
    std::memcpy(packet.data(), tag, sizeof(tag));
    for (size_t i = 0; i < writes.size(); ++i)
    {
        const uint64_t pair[2] = {writes[i].second, writes[i].first};
        std::memcpy(packet.data() + 16u + i * 16u, pair, sizeof(pair));
    }
    return packet;
}

GSNativeMesh::Header screenHeader()
{
    GSNativeMesh::Header header{};
    header.matrix[0][0] = header.matrix[1][1] = header.matrix[2][2] = header.matrix[3][3] = 1.0f;
    header.fog[0] = header.fog[1] = 255.0f; // F = 255: no fog
    header.colorMax[0] = header.colorMax[1] = header.colorMax[2] = header.colorMax[3] = 255.0f;
    header.triangleList = 1u;
    return header;
}
// What the GS would carry from one packet to the next, mirrored per thread:
// the GIF vertex state, a strip or fan under way, PRMODECONT and PRMODE.
// build keeps it for what it converts, observe for what goes to the GS
// as it is.
struct Carried
{
    rg::gif::GifState state;
    std::vector<rg::gif::GifEvent> queue;
    uint16_t queuePrim = 0xFFFFu;
    bool attributesFromPrim = true; // PRMODECONT AC
    uint16_t prmode = 0u;
};
thread_local Carried t_carried;
} // namespace

bool enabled()
{
    static const bool on = [] {
        const char *value = std::getenv("RG_NATIVE_2D");
        return rg::nativeproducer::pipelineOn() && !(value && std::strcmp(value, "0") == 0);
    }();
    return on;
}

void observe(const uint8_t *data, uint32_t size)
{
    Carried &carried = t_carried;
    thread_local std::vector<rg::gif::GifEvent> events;
    rg::gif::eachPacket(data, size, [&](uint32_t at, uint32_t piece) {
        events.clear();
        rg::gif::decodeGifPacket(data + at, piece, carried.state, events);
        for (const rg::gif::GifEvent &e : events)
        {
            if (e.kind == rg::gif::GifEvent::Kind::Register)
            {
                if (e.reg == 0x1Au)
                    carried.attributesFromPrim = (e.value & 1u) != 0u;
                else if (e.reg == 0x1Bu)
                    carried.prmode = static_cast<uint16_t>(e.value & 0x7F8u);
                else if (e.reg != 0x00u)
                    continue;
            }
            // A PRIM write, or a vertex the GS queued itself: whatever strip
            // the converter had under way is not the GS's any more.
            carried.queue.clear();
            carried.queuePrim = 0xFFFFu;
        }
    });
}

bool build(const Span *packets, size_t count, std::vector<Piece> &out, std::string &why)
{
    // The last call's meshes' vertex storage, for this call's meshes: a
    // packet's meshes would otherwise allocate and free it each time.
    thread_local std::vector<std::vector<GSNativeMesh::Vertex>> spare;
    for (Piece &old : out)
        if (old.mesh.vertices.capacity() != 0u && spare.size() < 64u)
        {
            old.mesh.vertices.clear();
            spare.push_back(std::move(old.mesh.vertices));
        }
    out.clear();
    Carried &carried = t_carried;
    rg::gif::GifState &state = carried.state;
    std::vector<rg::gif::GifEvent> &queue = carried.queue;
    uint16_t &queuePrim = carried.queuePrim;
    thread_local std::vector<rg::gif::GifEvent> events;
    std::vector<std::pair<uint8_t, uint64_t>> writes;
    Piece mesh;
    uint16_t meshPrim = 0xFFFFu;

    const auto closeMesh = [&] {
        if (!mesh.mesh.vertices.empty())
        {
            if (meshPrim != 0xFFFFu && (meshPrim & 0x20u))
            {
                // Fogged: clip = (x w, y w, 0, w) with w = 1 + F/255, and
                // F = -255 + 255 w gives each vertex its own F back (the
                // shader's varyings are noperspective; Z is the vertex's own).
                GSNativeMesh::Header &h = mesh.mesh.header;
                h.matrix[2][2] = 0.0f;
                h.matrix[2][3] = 1.0f;
                h.matrix[3][3] = 0.0f;
                h.fog[0] = -255.0f;
                h.fog[1] = 255.0f;
                h.fog[2] = 0.0f;
                h.fog[3] = 255.0f;
                // Texel UVs too: an FST draw reads gs_native.vert's
                // coordinate, (u, v) / w, as texels, without dividing by Q.
                const bool texels = (meshPrim & 0x110u) == 0x110u;
                for (GSNativeMesh::Vertex &v : mesh.mesh.vertices)
                {
                    v.x *= v.z;
                    v.y *= v.z;
                    if (texels)
                    {
                        v.u *= v.z;
                        v.v *= v.z;
                    }
                }
            }
            mesh.mesh.header.vertexCount = static_cast<uint32_t>(mesh.mesh.vertices.size());
            out.push_back(std::move(mesh));
        }
        mesh = Piece{};
        if (!spare.empty())
        {
            mesh.mesh.vertices = std::move(spare.back());
            spare.pop_back();
        }
        meshPrim = 0xFFFFu;
    };
    const auto flushWrites = [&] {
        if (!writes.empty())
        {
            closeMesh();
            Piece piece;
            piece.packet = adPacket(writes);
            out.push_back(std::move(piece));
            writes.clear();
        }
    };
    const auto vertexOf = [&](const rg::gif::GifEvent &e, const rg::gif::GifEvent &provoking, uint16_t prim, bool &ok) {
        GSNativeMesh::Vertex v;
        v.x = static_cast<float>(e.x) / 16.0f;
        v.y = static_cast<float>(e.y) / 16.0f;
        v.z = static_cast<float>(e.z);
        v.depth = v.z; // the Z as the packet has it
        if (prim & 0x20u) // FGE: w carries F (closeMesh scales x and y by it)
            v.z = 1.0f + (static_cast<float>(e.f) + 0.25f) / 255.0f;
        const rg::gif::GifEvent &c = (prim & 0x08u) ? e : provoking; // IIP
        v.r = c.rgba[0];
        v.g = c.rgba[1];
        v.b = c.rgba[2];
        v.a = c.rgba[3];
        if (prim & 0x10u) // TME
        {
            if (prim & 0x100u) // FST: texel UVs, 10.4 (Piece::texelUvs)
            {
                v.u = static_cast<float>(e.u) / 16.0f;
                v.v = static_cast<float>(e.v) / 16.0f;
            }
            else
            {
                const float q = e.q != 0.0f ? e.q : 1.0f;
                v.u = e.s / q;
                v.v = e.t / q;
            }
        }
        return v;
    };
    const auto openMesh = [&](uint16_t prim) {
        if (meshPrim == prim)
            return;
        closeMesh();
        // The draw state the mesh takes is the GS's PRIM: its attributes,
        // as a triangle, written just before it.
        writes.emplace_back(0x00u, static_cast<uint64_t>((prim & ~7u) | 3u));
        flushWrites();
        mesh.mesh.header = screenHeader();
        mesh.texelUvs = (prim & 0x110u) == 0x110u;
        meshPrim = prim;
    };
    const auto triangle = [&](const rg::gif::GifEvent &a, const rg::gif::GifEvent &b, const rg::gif::GifEvent &c,
                              uint16_t prim, bool &ok) {
        openMesh(prim);
        mesh.mesh.vertices.push_back(vertexOf(a, c, prim, ok));
        mesh.mesh.vertices.push_back(vertexOf(b, c, prim, ok));
        mesh.mesh.vertices.push_back(vertexOf(c, c, prim, ok));
    };
    const auto sprite = [&](const rg::gif::GifEvent &a, const rg::gif::GifEvent &b, uint16_t prim, bool &ok) {
        openMesh(prim);
        // Corners from the two vertices; Z and colour are the second's.
        rg::gif::GifEvent corner[4] = {a, b, a, b};
        corner[0].z = corner[2].z = b.z;
        corner[1].x = b.x; corner[1].y = a.y; corner[1].u = b.u; corner[1].v = a.v; corner[1].s = b.s; corner[1].t = a.t;
        corner[2].x = a.x; corner[2].y = b.y; corner[2].u = a.u; corner[2].v = b.v; corner[2].s = a.s; corner[2].t = b.t;
        corner[1].q = corner[2].q = b.q;
        corner[0].q = a.q;
        const GSNativeMesh::Vertex v0 = vertexOf(corner[0], b, prim & ~0x08u, ok);
        const GSNativeMesh::Vertex v1 = vertexOf(corner[1], b, prim & ~0x08u, ok);
        const GSNativeMesh::Vertex v2 = vertexOf(corner[2], b, prim & ~0x08u, ok);
        const GSNativeMesh::Vertex v3 = vertexOf(corner[3], b, prim & ~0x08u, ok);
        mesh.mesh.vertices.insert(mesh.mesh.vertices.end(), {v0, v1, v3, v0, v3, v2});
    };

    bool ok = true;
    bool anyVertex = false;
    // One DIRECT's bytes hold as many GIF packets as the game put there, each
    // up to its EOP, and the GS reads on past an EOP (eachPacket): every one
    // of them, not only the first.
    thread_local std::vector<std::pair<uint32_t, uint32_t>> pieces;
    for (size_t index = 0; index < count; ++index)
    {
        const Span &packet = packets[index];
        pieces.clear();
        rg::gif::eachPacket(packet.data, packet.size, [&](uint32_t at, uint32_t size) { pieces.emplace_back(at, size); });
        for (const auto &[at, size] : pieces)
        {
            events.clear();
            const rg::gif::GifDecodeStats stats = rg::gif::decodeGifPacket(packet.data + at, size, state, events);
            // A tag cut at the packet's end (an IMAGE tag whose data follows
            // through the image callback, a PACKED tag a DIRECT cuts short) is
            // what GS::processGIFPacket takes as far as it goes and then drops:
            // the events decoded up to the cut are what the GS would draw.
            if (stats.imageQwords != 0u)
            {
                // An upload: on as it is, in its place.
                flushWrites();
                closeMesh();
                queue.clear();
                Piece piece;
                piece.view = packet.data + at;
                piece.viewSize = size;
                out.push_back(std::move(piece));
                continue;
            }
            for (const rg::gif::GifEvent &e : events)
            {
                if (e.kind == rg::gif::GifEvent::Kind::Register)
                {
                    if (e.reg == 0x1Au)
                        carried.attributesFromPrim = (e.value & 1u) != 0u;
                    if (e.reg == 0x1Bu)
                        carried.prmode = static_cast<uint16_t>(e.value & 0x7F8u);
                    if (e.reg == 0x00u)
                    {
                        queue.clear();
                        queuePrim = 0xFFFFu;
                    }
                    closeMesh();
                    writes.emplace_back(e.reg, e.value);
                    continue;
                }
                anyVertex = true;
                // With PRMODECONT's AC clear the attributes are PRMODE's.
                const uint16_t prim = carried.attributesFromPrim
                                          ? e.prim
                                          : static_cast<uint16_t>((e.prim & 7u) | carried.prmode);
                const uint32_t type = prim & 7u;
                if (type == 7u)
                {
                    why = "a reserved primitive type";
                    return false;
                }
                if (prim != queuePrim)
                {
                    queue.clear();
                    queuePrim = prim;
                }
                switch (type)
                {
                case 0u: // point: a pixel's square
                    if (e.drawing)
                    {
                        rg::gif::GifEvent a = e, b = e;
                        a.x = static_cast<uint16_t>(e.x & ~15u);
                        a.y = static_cast<uint16_t>(e.y & ~15u);
                        b.x = static_cast<uint16_t>(a.x + 16u);
                        b.y = static_cast<uint16_t>(a.y + 16u);
                        sprite(a, b, static_cast<uint16_t>(prim & ~0x08u), ok);
                    }
                    break;
                case 1u: // line
                case 2u: // line strip
                    queue.push_back(e);
                    if (queue.size() > 2u)
                        queue.erase(queue.begin());
                    if (queue.size() == 2u && e.drawing)
                    {
                        // Half a pixel either side of the centre line, as the
                        // backend widens the GS's lines.
                        const rg::gif::GifEvent &p0 = queue[0], &p1 = queue[1];
                        const float dx = (static_cast<float>(p1.x) - p0.x) / 16.0f;
                        const float dy = (static_cast<float>(p1.y) - p0.y) / 16.0f;
                        const float length = std::sqrt(dx * dx + dy * dy);
                        const float nx = length > 1e-6f ? -dy / length * 0.5f : 0.5f;
                        const float ny = length > 1e-6f ? dx / length * 0.5f : 0.0f;
                        openMesh(prim);
                        GSNativeMesh::Vertex v[4] = {vertexOf(p0, p1, prim, ok), vertexOf(p0, p1, prim, ok),
                                                     vertexOf(p1, p1, prim, ok), vertexOf(p1, p1, prim, ok)};
                        v[0].x += nx; v[0].y += ny;
                        v[1].x -= nx; v[1].y -= ny;
                        v[2].x -= nx; v[2].y -= ny;
                        v[3].x += nx; v[3].y += ny;
                        mesh.mesh.vertices.insert(mesh.mesh.vertices.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
                        if (type == 1u)
                            queue.clear();
                    }
                    else if (type == 1u && queue.size() == 2u)
                    {
                        queue.clear();
                    }
                    break;
                case 6u: // sprite
                    queue.push_back(e);
                    if (queue.size() == 2u)
                    {
                        if (e.drawing)
                            sprite(queue[0], queue[1], prim, ok);
                        queue.clear();
                    }
                    break;
                case 3u: // triangle
                    queue.push_back(e);
                    if (queue.size() == 3u)
                    {
                        if (e.drawing)
                            triangle(queue[0], queue[1], queue[2], prim, ok);
                        queue.clear();
                    }
                    break;
                case 4u: // strip
                    queue.push_back(e);
                    if (queue.size() > 3u)
                        queue.erase(queue.begin());
                    if (queue.size() == 3u && e.drawing)
                        triangle(queue[0], queue[1], queue[2], prim, ok);
                    break;
                case 5u: // fan
                    if (queue.size() == 3u)
                    {
                        queue[1] = queue[2];
                        queue.pop_back();
                    }
                    queue.push_back(e);
                    if (queue.size() == 3u && e.drawing)
                        triangle(queue[0], queue[1], queue[2], prim, ok);
                    break;
                default:
                    break;
                }

            }
        }
    }
    // The vertex registers the meshes took in (RGBAQ, ST, UV, FOG) and PRIM
    // as the packets left them: the GS keeps them for what comes next, and a
    // packet after the block may draw with them (an ST and XYZ with the Q of
    // an earlier RGBAQ).
    if (anyVertex)
    {
        uint32_t q;
        std::memcpy(&q, &state.q, sizeof(q));
        uint32_t sBits, tBits;
        std::memcpy(&sBits, &state.s, sizeof(sBits));
        std::memcpy(&tBits, &state.t, sizeof(tBits));
        closeMesh();
        const auto put = [&](uint8_t reg, uint64_t value) { writes.emplace_back(reg, value); };
        put(0x00u, static_cast<uint64_t>(state.prim));
        put(0x01u, static_cast<uint64_t>(state.rgba[0]) | (static_cast<uint64_t>(state.rgba[1]) << 8) |
                                       (static_cast<uint64_t>(state.rgba[2]) << 16) |
                                       (static_cast<uint64_t>(state.rgba[3]) << 24) | (static_cast<uint64_t>(q) << 32));
        put(0x02u, static_cast<uint64_t>(sBits) | (static_cast<uint64_t>(tBits) << 32));
        put(0x03u, static_cast<uint64_t>(state.u & 0x3FFFu) | (static_cast<uint64_t>(state.v & 0x3FFFu) << 16));
        put(0x0Au, static_cast<uint64_t>(state.f) << 56);
    }
    flushWrites();
    closeMesh();
    return true;
}

} // namespace rg::native2d
