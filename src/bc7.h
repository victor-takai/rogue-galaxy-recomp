#pragma once

#include <cstdint>
#include <cstring>

#include "bc7_tables.h"

// BC7 (DDS's DXGI_FORMAT_BC7_UNORM) decoded on the CPU, one 4x4 block of 16
// bytes into 16 RGBA pixels. Checked against an independent decoder on random
// blocks of every mode (tools/texture_pack/check_dds.py).
namespace rg::bc7
{

namespace detail
{

struct Mode
{
    int subsets, partitionBits, rotationBits, indexModeBits, colorBits, alphaBits;
    int pbits; // 0 none, 1 one per endpoint, 2 one per subset
    int indexBits, secondIndexBits;
};

constexpr Mode kModes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 3, 0}, {2, 6, 0, 0, 6, 0, 2, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 2, 0},
};

constexpr uint8_t kWeights2[4] = {0, 21, 43, 64};
constexpr uint8_t kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr uint8_t kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

inline int weight(int bits, int index)
{
    return bits == 2 ? kWeights2[index] : bits == 3 ? kWeights3[index] : kWeights4[index];
}

inline int interpolate(int a, int b, int w)
{
    return (a * (64 - w) + b * w + 32) >> 6;
}

class BitReader
{
public:
    explicit BitReader(const uint8_t *data) : m_data(data) {}

    int read(int count)
    {
        int value = 0;
        for (int i = 0; i < count; ++i, ++m_at)
        {
            value |= ((m_data[m_at >> 3] >> (m_at & 7)) & 1) << i;
        }
        return value;
    }

private:
    const uint8_t *m_data;
    int m_at = 0;
};

// A value of `bits` bits widened to 8 by repeating its top bits.
inline int widen(int value, int bits)
{
    value <<= 8 - bits;
    return value | (value >> bits);
}

} // namespace detail

// A block of mode 8 or above (the first byte zero) is transparent black, as
// the format says.
inline void decodeBlock(const uint8_t *block, uint8_t (*out)[4])
{
    using namespace detail;
    int mode = 0;
    while (mode < 8 && !((block[0] >> mode) & 1))
    {
        ++mode;
    }
    if (mode >= 8)
    {
        std::memset(out, 0, 16 * 4);
        return;
    }
    const Mode &m = kModes[mode];
    BitReader bits(block);
    bits.read(mode + 1);
    const int partition = bits.read(m.partitionBits);
    const int rotation = bits.read(m.rotationBits);
    const int indexMode = bits.read(m.indexModeBits);

    const int endpoints = m.subsets * 2;
    int color[6][4];
    for (int channel = 0; channel < 3; ++channel)
    {
        for (int e = 0; e < endpoints; ++e)
        {
            color[e][channel] = bits.read(m.colorBits);
        }
    }
    for (int e = 0; e < endpoints; ++e)
    {
        color[e][3] = m.alphaBits ? bits.read(m.alphaBits) : 255;
    }
    int pbit[6] = {0, 0, 0, 0, 0, 0};
    if (m.pbits == 1)
    {
        for (int e = 0; e < endpoints; ++e)
        {
            pbit[e] = bits.read(1);
        }
    }
    else if (m.pbits == 2)
    {
        for (int s = 0; s < m.subsets; ++s)
        {
            pbit[s * 2] = pbit[s * 2 + 1] = bits.read(1);
        }
    }
    for (int e = 0; e < endpoints; ++e)
    {
        for (int channel = 0; channel < 4; ++channel)
        {
            const int precision = channel < 3 ? m.colorBits : m.alphaBits;
            if (channel == 3 && !m.alphaBits)
            {
                continue;
            }
            if (m.pbits)
            {
                color[e][channel] = widen((color[e][channel] << 1) | pbit[e], precision + 1);
            }
            else
            {
                color[e][channel] = widen(color[e][channel], precision);
            }
        }
    }

    auto subsetOf = [&](int pixel) {
        return m.subsets == 1 ? 0 : m.subsets == 2 ? kPartition2[partition][pixel] : kPartition3[partition][pixel];
    };
    auto isAnchor = [&](int pixel) {
        if (pixel == 0)
        {
            return true;
        }
        if (m.subsets == 2)
        {
            return pixel == kAnchor2[partition];
        }
        return m.subsets == 3 && (pixel == kAnchor3a[partition] || pixel == kAnchor3b[partition]);
    };

    int index[16], secondIndex[16];
    for (int pixel = 0; pixel < 16; ++pixel)
    {
        index[pixel] = bits.read(m.indexBits - (isAnchor(pixel) ? 1 : 0));
    }
    if (m.secondIndexBits)
    {
        for (int pixel = 0; pixel < 16; ++pixel)
        {
            secondIndex[pixel] = bits.read(m.secondIndexBits - (pixel == 0 ? 1 : 0));
        }
    }

    for (int pixel = 0; pixel < 16; ++pixel)
    {
        const int subset = subsetOf(pixel);
        const int *a = color[subset * 2];
        const int *b = color[subset * 2 + 1];
        int rgba[4];
        if (m.secondIndexBits)
        {
            // Mode 4 may swap which index set does the colour and which the alpha.
            const bool swap = mode == 4 && indexMode == 1;
            const int colorIndex = swap ? secondIndex[pixel] : index[pixel];
            const int colorBits = swap ? m.secondIndexBits : m.indexBits;
            const int alphaIndex = swap ? index[pixel] : secondIndex[pixel];
            const int alphaBits = swap ? m.indexBits : m.secondIndexBits;
            for (int channel = 0; channel < 3; ++channel)
            {
                rgba[channel] = interpolate(a[channel], b[channel], weight(colorBits, colorIndex));
            }
            rgba[3] = interpolate(a[3], b[3], weight(alphaBits, alphaIndex));
        }
        else
        {
            const int w = weight(m.indexBits, index[pixel]);
            for (int channel = 0; channel < 4; ++channel)
            {
                rgba[channel] = interpolate(a[channel], b[channel], w);
            }
        }
        // Rotation: the alpha channel traded with red, green or blue.
        if (rotation)
        {
            const int other = rotation - 1;
            const int kept = rgba[3];
            rgba[3] = rgba[other];
            rgba[other] = kept;
        }
        for (int channel = 0; channel < 4; ++channel)
        {
            out[pixel][channel] = static_cast<uint8_t>(rgba[channel]);
        }
    }
}

} // namespace rg::bc7
