#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "bc7.h"

// A DDS picture read to RGBA8, its first (largest) level only: BC1, BC2, BC3
// and BC7 (DXGI or the FourCC forms DXT1, DXT3, DXT5), and uncompressed
// 32-bit formats by their masks; or a BC picture's blocks as they are
// (blocks). Returns null on success or what is wrong.
namespace rg::dds
{

namespace detail
{

inline uint32_t load32(const uint8_t *data)
{
    return data[0] | data[1] << 8 | data[2] << 16 | static_cast<uint32_t>(data[3]) << 24;
}

inline int widen(int value, int bits)
{
    value <<= 8 - bits;
    return value | (value >> bits);
}

// The colour half of BC1, BC2 and BC3. `opaque` lets a BC1 block with
// c0 <= c1 make its fourth colour transparent.
inline void colorBlock(const uint8_t *block, bool bc1, uint8_t (*out)[4])
{
    const int c0 = block[0] | block[1] << 8;
    const int c1 = block[2] | block[3] << 8;
    int palette[4][4];
    const int raw[2] = {c0, c1};
    for (int i = 0; i < 2; ++i)
    {
        palette[i][0] = widen(raw[i] >> 11, 5);
        palette[i][1] = widen((raw[i] >> 5) & 63, 6);
        palette[i][2] = widen(raw[i] & 31, 5);
        palette[i][3] = 255;
    }
    for (int channel = 0; channel < 3; ++channel)
    {
        if (c0 > c1 || !bc1)
        {
            palette[2][channel] = (2 * palette[0][channel] + palette[1][channel]) / 3;
            palette[3][channel] = (palette[0][channel] + 2 * palette[1][channel]) / 3;
        }
        else
        {
            palette[2][channel] = (palette[0][channel] + palette[1][channel]) / 2;
            palette[3][channel] = 0;
        }
    }
    palette[2][3] = 255;
    palette[3][3] = c0 > c1 || !bc1 ? 255 : 0;
    const uint32_t indices = load32(block + 4);
    for (int pixel = 0; pixel < 16; ++pixel)
    {
        const int *color = palette[(indices >> (pixel * 2)) & 3];
        for (int channel = 0; channel < 4; ++channel)
        {
            out[pixel][channel] = static_cast<uint8_t>(color[channel]);
        }
    }
}

// BC3's alpha block.
inline void alphaBlock(const uint8_t *block, uint8_t (*out)[4])
{
    const int a0 = block[0], a1 = block[1];
    int palette[8] = {a0, a1};
    if (a0 > a1)
    {
        for (int i = 1; i < 7; ++i)
        {
            palette[i + 1] = ((7 - i) * a0 + i * a1) / 7;
        }
    }
    else
    {
        for (int i = 1; i < 5; ++i)
        {
            palette[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        }
        palette[6] = 0;
        palette[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i)
    {
        bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
    }
    for (int pixel = 0; pixel < 16; ++pixel)
    {
        out[pixel][3] = static_cast<uint8_t>(palette[(bits >> (pixel * 3)) & 7]);
    }
}

} // namespace detail

enum class Kind { Bc1, Bc2, Bc3, Bc7, Raw32, None };

// What a file's header says: its size, its format, where its first level
// starts and, for Raw32, where each channel sits.
struct Header
{
    uint32_t width = 0u, height = 0u;
    Kind kind = Kind::None;
    size_t offset = 128u;
    uint32_t masks[4] = {};
};

// The bytes of a BC kind's 4x4 block.
inline size_t blockBytes(Kind kind)
{
    return kind == Kind::Bc1 ? 8u : 16u;
}

// Reads the header, and checks the first level is all there.
inline const char *parse(const uint8_t *data, size_t size, Header &header)
{
    using namespace detail;
    if (size < 128 || std::memcmp(data, "DDS ", 4) != 0 || load32(data + 4) != 124u)
    {
        return "not a DDS file";
    }
    const uint32_t height = header.height = load32(data + 12);
    const uint32_t width = header.width = load32(data + 16);
    if (!width || !height || width > 16384u || height > 16384u)
    {
        return "bad size";
    }
    const uint32_t pixelFlags = load32(data + 80);
    const uint32_t fourCC = load32(data + 84);
    Kind &kind = header.kind;
    size_t &offset = header.offset;
    uint32_t *masks = header.masks;
    if (pixelFlags & 4u) // DDPF_FOURCC
    {
        if (fourCC == 0x31545844u) // DXT1
        {
            kind = Kind::Bc1;
        }
        else if (fourCC == 0x33545844u) // DXT3
        {
            kind = Kind::Bc2;
        }
        else if (fourCC == 0x35545844u) // DXT5
        {
            kind = Kind::Bc3;
        }
        else if (fourCC == 0x30315844u) // DX10
        {
            if (size < 148)
            {
                return "truncated";
            }
            offset = 148u;
            const uint32_t format = load32(data + 128);
            // An array's first picture is the one read.
            switch (format)
            {
            case 70: case 71: case 72: kind = Kind::Bc1; break;
            case 73: case 74: case 75: kind = Kind::Bc2; break;
            case 76: case 77: case 78: kind = Kind::Bc3; break;
            case 97: case 98: case 99: kind = Kind::Bc7; break;
            case 28: case 29: // R8G8B8A8
                kind = Kind::Raw32;
                masks[0] = 0x000000FFu, masks[1] = 0x0000FF00u, masks[2] = 0x00FF0000u, masks[3] = 0xFF000000u;
                break;
            case 87: // B8G8R8A8
                kind = Kind::Raw32;
                masks[0] = 0x00FF0000u, masks[1] = 0x0000FF00u, masks[2] = 0x000000FFu, masks[3] = 0xFF000000u;
                break;
            default:
                return "a DXGI format this game does not read (BC1, BC2, BC3, BC7 and 8-bit RGBA are)";
            }
        }
    }
    else if ((pixelFlags & 0x40u) && load32(data + 88) == 32u) // DDPF_RGB, 32 bits
    {
        kind = Kind::Raw32;
        masks[0] = load32(data + 92), masks[1] = load32(data + 96), masks[2] = load32(data + 100);
        masks[3] = (pixelFlags & 1u) ? load32(data + 104) : 0u;
    }
    if (kind == Kind::None)
    {
        return "a DDS format this game does not read (BC1, BC2, BC3, BC7 and 32-bit RGB are)";
    }
    const size_t needed = kind == Kind::Raw32
                              ? size_t(width) * height * 4u
                              : size_t((width + 3u) / 4u) * ((height + 3u) / 4u) * blockBytes(kind);
    if (size < offset + needed)
    {
        return "truncated";
    }
    return nullptr;
}

// A BC file's first level as it is, a row of 4x4 blocks after another, for a
// GPU that samples the format itself (header.kind says which). A Raw32 file
// is no BC: decode reads it.
inline const char *blocks(const uint8_t *data, size_t size, Header &header, std::vector<uint8_t> &out)
{
    if (const char *error = parse(data, size, header))
    {
        return error;
    }
    if (header.kind == Kind::Raw32)
    {
        return "not block-compressed";
    }
    const uint8_t *first = data + header.offset;
    out.assign(first, first + size_t((header.width + 3u) / 4u) * ((header.height + 3u) / 4u) *
                                  blockBytes(header.kind));
    return nullptr;
}

inline const char *decode(const uint8_t *data, size_t size, uint32_t &width, uint32_t &height,
                          std::vector<uint8_t> &rgba)
{
    using namespace detail;
    Header header;
    if (const char *error = parse(data, size, header))
    {
        return error;
    }
    width = header.width;
    height = header.height;
    const Kind kind = header.kind;
    const size_t offset = header.offset;
    const uint32_t *masks = header.masks;
    rgba.assign(size_t(width) * height * 4u, 0u);
    if (kind == Kind::Raw32)
    {
        for (size_t pixel = 0; pixel < size_t(width) * height; ++pixel)
        {
            const uint32_t value = load32(data + offset + pixel * 4u);
            for (int channel = 0; channel < 4; ++channel)
            {
                uint32_t mask = masks[channel];
                if (!mask)
                {
                    rgba[pixel * 4 + channel] = 255;
                    continue;
                }
                int shift = 0;
                while (!((mask >> shift) & 1u))
                {
                    ++shift;
                }
                int count = 0;
                while ((mask >> (shift + count)) & 1u)
                {
                    ++count;
                }
                const uint32_t field = (value & mask) >> shift;
                rgba[pixel * 4 + channel] = static_cast<uint8_t>(count >= 8 ? field >> (count - 8)
                                                                            : widen(static_cast<int>(field), count));
            }
        }
        return nullptr;
    }

    const size_t step = blockBytes(kind);
    const uint32_t blocksX = (width + 3u) / 4u, blocksY = (height + 3u) / 4u;
    const uint8_t *block = data + offset;
    for (uint32_t by = 0; by < blocksY; ++by)
    {
        for (uint32_t bx = 0; bx < blocksX; ++bx, block += step)
        {
            uint8_t pixels[16][4];
            switch (kind)
            {
            case Kind::Bc1:
                colorBlock(block, true, pixels);
                break;
            case Kind::Bc2:
                colorBlock(block + 8, false, pixels);
                for (int pixel = 0; pixel < 16; ++pixel)
                {
                    const int nibble = (block[pixel / 2] >> ((pixel & 1) * 4)) & 15;
                    pixels[pixel][3] = static_cast<uint8_t>(nibble * 17);
                }
                break;
            case Kind::Bc3:
                colorBlock(block + 8, false, pixels);
                alphaBlock(block, pixels);
                break;
            default:
                bc7::decodeBlock(block, pixels);
                break;
            }
            for (uint32_t py = 0; py < 4u && by * 4u + py < height; ++py)
            {
                for (uint32_t px = 0; px < 4u && bx * 4u + px < width; ++px)
                {
                    std::memcpy(&rgba[((size_t(by) * 4u + py) * width + bx * 4u + px) * 4u], pixels[py * 4u + px], 4u);
                }
            }
        }
    }
    return nullptr;
}

} // namespace rg::dds
