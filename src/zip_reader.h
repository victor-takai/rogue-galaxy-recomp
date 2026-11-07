#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// A zip file read for its entries by name: stored or deflated (the two kinds
// every zip tool writes), no encryption, no zip64. The central directory is
// read at open; an entry's bytes are read and inflated on demand, one at a
// time, from any thread. Entry names are matched as the archive spells them,
// with / for folders (a \ is turned into one).
namespace rg::zip
{

namespace detail
{

// Raw DEFLATE (RFC 1951) in the manner of zlib's puff.c: canonical Huffman
// decoding bit by bit. Slow beside zlib, plenty for pictures read once on a
// thread of their own. Writes exactly `expected` bytes or fails.
class Inflater
{
public:
    Inflater(const uint8_t *in, size_t inSize, std::vector<uint8_t> &out, size_t expected)
        : m_in(in), m_inSize(inSize), m_out(out), m_expected(expected)
    {
        m_out.clear();
        m_out.reserve(expected);
    }

    bool run()
    {
        int last = 0;
        do
        {
            last = bits(1);
            const int type = bits(2);
            if (m_error)
            {
                return false;
            }
            bool ok = false;
            if (type == 0)
            {
                ok = stored();
            }
            else if (type == 1)
            {
                ok = fixedBlock();
            }
            else if (type == 2)
            {
                ok = dynamicBlock();
            }
            if (!ok || m_error)
            {
                return false;
            }
        } while (!last);
        return m_out.size() == m_expected;
    }

private:
    static constexpr int kMaxBits = 15;
    struct Huffman
    {
        int count[kMaxBits + 1] = {};
        int symbol[288] = {};
    };

    const uint8_t *m_in;
    size_t m_inSize, m_inAt = 0u;
    std::vector<uint8_t> &m_out;
    size_t m_expected;
    uint32_t m_bitBuffer = 0u;
    int m_bitCount = 0;
    bool m_error = false;

    int bits(int need)
    {
        uint32_t value = m_bitBuffer;
        while (m_bitCount < need)
        {
            if (m_inAt >= m_inSize)
            {
                m_error = true;
                return 0;
            }
            value |= static_cast<uint32_t>(m_in[m_inAt++]) << m_bitCount;
            m_bitCount += 8;
        }
        m_bitBuffer = value >> need;
        m_bitCount -= need;
        return static_cast<int>(value & ((1u << need) - 1u));
    }

    bool stored()
    {
        m_bitBuffer = 0u;
        m_bitCount = 0;
        if (m_inAt + 4u > m_inSize)
        {
            return false;
        }
        const unsigned length = m_in[m_inAt] | m_in[m_inAt + 1] << 8;
        const unsigned inverse = m_in[m_inAt + 2] | m_in[m_inAt + 3] << 8;
        m_inAt += 4u;
        if (length != (~inverse & 0xFFFFu) || m_inAt + length > m_inSize || m_out.size() + length > m_expected)
        {
            return false;
        }
        m_out.insert(m_out.end(), m_in + m_inAt, m_in + m_inAt + length);
        m_inAt += length;
        return true;
    }

    // Returns the code's symbol or -1.
    int decode(const Huffman &h)
    {
        int code = 0, first = 0, index = 0;
        for (int length = 1; length <= kMaxBits; ++length)
        {
            code |= bits(1);
            if (m_error)
            {
                return -1;
            }
            const int count = h.count[length];
            if (code - count < first)
            {
                return h.symbol[index + (code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

    // Builds the tables from code lengths; returns 0 for a complete code,
    // positive for an incomplete one, negative for an over-subscribed one.
    static int construct(Huffman &h, const short *length, int n)
    {
        for (int len = 0; len <= kMaxBits; ++len)
        {
            h.count[len] = 0;
        }
        for (int symbol = 0; symbol < n; ++symbol)
        {
            ++h.count[length[symbol]];
        }
        if (h.count[0] == n)
        {
            return 0;
        }
        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len)
        {
            left <<= 1;
            left -= h.count[len];
            if (left < 0)
            {
                return left;
            }
        }
        short offsets[kMaxBits + 1];
        offsets[1] = 0;
        for (int len = 1; len < kMaxBits; ++len)
        {
            offsets[len + 1] = static_cast<short>(offsets[len] + h.count[len]);
        }
        for (int symbol = 0; symbol < n; ++symbol)
        {
            if (length[symbol] != 0)
            {
                h.symbol[offsets[length[symbol]]++] = symbol;
            }
        }
        return left;
    }

    bool codes(const Huffman &lengthCodes, const Huffman &distanceCodes)
    {
        static const short lengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                             31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short lengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                              2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short distanceBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                               33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                               1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short distanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;)
        {
            int symbol = decode(lengthCodes);
            if (symbol < 0)
            {
                return false;
            }
            if (symbol < 256)
            {
                if (m_out.size() >= m_expected)
                {
                    return false;
                }
                m_out.push_back(static_cast<uint8_t>(symbol));
            }
            else if (symbol == 256)
            {
                return true;
            }
            else
            {
                symbol -= 257;
                if (symbol >= 29)
                {
                    return false;
                }
                const int length = lengthBase[symbol] + bits(lengthExtra[symbol]);
                const int distanceSymbol = decode(distanceCodes);
                if (distanceSymbol < 0 || distanceSymbol >= 30)
                {
                    return false;
                }
                const size_t distance = static_cast<size_t>(distanceBase[distanceSymbol]) +
                                        static_cast<size_t>(bits(distanceExtra[distanceSymbol]));
                if (m_error || distance > m_out.size() || m_out.size() + length > m_expected)
                {
                    return false;
                }
                for (int i = 0; i < length; ++i)
                {
                    m_out.push_back(m_out[m_out.size() - distance]);
                }
            }
        }
    }

    bool fixedBlock()
    {
        Huffman lengthCodes, distanceCodes;
        short lengths[288];
        int symbol = 0;
        for (; symbol < 144; ++symbol) lengths[symbol] = 8;
        for (; symbol < 256; ++symbol) lengths[symbol] = 9;
        for (; symbol < 280; ++symbol) lengths[symbol] = 7;
        for (; symbol < 288; ++symbol) lengths[symbol] = 8;
        construct(lengthCodes, lengths, 288);
        for (symbol = 0; symbol < 30; ++symbol) lengths[symbol] = 5;
        construct(distanceCodes, lengths, 30);
        return codes(lengthCodes, distanceCodes);
    }

    bool dynamicBlock()
    {
        static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (m_error || nlen > 286 || ndist > 30)
        {
            return false;
        }
        short lengths[320] = {};
        for (int index = 0; index < ncode; ++index)
        {
            lengths[order[index]] = static_cast<short>(bits(3));
        }
        Huffman lengthCodes, distanceCodes, codeCodes;
        if (m_error || construct(codeCodes, lengths, 19) != 0)
        {
            return false;
        }
        int index = 0;
        short written[320] = {};
        while (index < nlen + ndist)
        {
            int symbol = decode(codeCodes);
            if (symbol < 0)
            {
                return false;
            }
            if (symbol < 16)
            {
                written[index++] = static_cast<short>(symbol);
                continue;
            }
            short previous = 0;
            int repeat = 0;
            if (symbol == 16)
            {
                if (index == 0)
                {
                    return false;
                }
                previous = written[index - 1];
                repeat = 3 + bits(2);
            }
            else if (symbol == 17)
            {
                repeat = 3 + bits(3);
            }
            else
            {
                repeat = 11 + bits(7);
            }
            if (m_error || index + repeat > nlen + ndist)
            {
                return false;
            }
            while (repeat--)
            {
                written[index++] = previous;
            }
        }
        if (written[256] == 0)
        {
            return false;
        }
        const int lengthResult = construct(lengthCodes, written, nlen);
        if (lengthResult < 0 || (lengthResult > 0 && nlen - lengthCodes.count[0] != 1))
        {
            return false;
        }
        const int distanceResult = construct(distanceCodes, written + nlen, ndist);
        if (distanceResult < 0 || (distanceResult > 0 && ndist - distanceCodes.count[0] != 1))
        {
            return false;
        }
        return codes(lengthCodes, distanceCodes);
    }
};

inline uint32_t u16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

inline uint32_t u32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24;
}

} // namespace detail

class Archive
{
public:
    struct Entry
    {
        uint32_t method = 0u, compressed = 0u, size = 0u, header = 0u;
    };

    // Reads the central directory. Returns null on success or what is wrong.
    const char *open(const std::string &path)
    {
        m_file.open(path, std::ios::binary);
        if (!m_file)
        {
            return "cannot open the file";
        }
        m_file.seekg(0, std::ios::end);
        const uint64_t size = static_cast<uint64_t>(m_file.tellg());
        const uint64_t tailSize = std::min<uint64_t>(size, 22u + 65535u);
        std::vector<uint8_t> tail(tailSize);
        m_file.seekg(static_cast<std::streamoff>(size - tailSize));
        m_file.read(reinterpret_cast<char *>(tail.data()), static_cast<std::streamsize>(tailSize));
        size_t end = std::string::npos;
        for (size_t at = tail.size() >= 22u ? tail.size() - 22u + 1u : 0u; at-- > 0u;)
        {
            if (detail::u32(&tail[at]) == 0x06054B50u)
            {
                end = at;
                break;
            }
        }
        if (end == std::string::npos)
        {
            return "not a zip file";
        }
        const uint32_t count = detail::u16(&tail[end + 10]);
        const uint32_t directorySize = detail::u32(&tail[end + 12]);
        const uint32_t directoryOffset = detail::u32(&tail[end + 16]);
        if (count == 0xFFFFu || directorySize == 0xFFFFFFFFu || directoryOffset == 0xFFFFFFFFu)
        {
            return "a zip64 file, which this game does not read";
        }
        if (static_cast<uint64_t>(directoryOffset) + directorySize > size)
        {
            return "damaged: the directory is past the end";
        }
        std::vector<uint8_t> directory(directorySize);
        m_file.seekg(directoryOffset);
        m_file.read(reinterpret_cast<char *>(directory.data()), directorySize);
        size_t at = 0u;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (at + 46u > directory.size() || detail::u32(&directory[at]) != 0x02014B50u)
            {
                return "damaged: the directory is cut short";
            }
            const uint8_t *p = &directory[at];
            const uint32_t flags = detail::u16(p + 8), nameLength = detail::u16(p + 28),
                           extraLength = detail::u16(p + 30), commentLength = detail::u16(p + 32);
            if (at + 46u + nameLength + extraLength + commentLength > directory.size())
            {
                return "damaged: an entry's name is past the directory";
            }
            std::string name(reinterpret_cast<const char *>(p + 46), nameLength);
            std::replace(name.begin(), name.end(), '\\', '/');
            Entry entry;
            entry.method = detail::u16(p + 10);
            entry.compressed = detail::u32(p + 20);
            entry.size = detail::u32(p + 24);
            entry.header = detail::u32(p + 42);
            // Encrypted entries, and folders, are not entries to read.
            if (!(flags & 1u) && !name.empty() && name.back() != '/')
            {
                m_entries[name] = entry;
            }
            at += 46u + nameLength + extraLength + commentLength;
        }
        return nullptr;
    }

    bool has(const std::string &name) const
    {
        return m_entries.count(name) != 0u;
    }

    const std::map<std::string, Entry> &entries() const
    {
        return m_entries;
    }

    // An entry's bytes, up to `limit`; false for a missing, damaged or
    // unsupported one.
    bool read(const std::string &name, std::vector<uint8_t> &out, size_t limit = 256u << 20u)
    {
        const auto found = m_entries.find(name);
        if (found == m_entries.end() || found->second.size > limit)
        {
            return false;
        }
        const Entry &entry = found->second;
        std::vector<uint8_t> raw(entry.compressed);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            uint8_t local[30];
            m_file.clear();
            m_file.seekg(entry.header);
            m_file.read(reinterpret_cast<char *>(local), sizeof local);
            if (!m_file || detail::u32(local) != 0x04034B50u)
            {
                return false;
            }
            m_file.seekg(static_cast<std::streamoff>(entry.header) + 30 + detail::u16(local + 26) +
                         detail::u16(local + 28));
            m_file.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(raw.size()));
            if (!m_file)
            {
                return false;
            }
        }
        if (entry.method == 0u)
        {
            if (entry.compressed != entry.size)
            {
                return false;
            }
            out = std::move(raw);
            return true;
        }
        if (entry.method != 8u)
        {
            return false;
        }
        return detail::Inflater(raw.data(), raw.size(), out, entry.size).run();
    }

private:
    std::ifstream m_file;
    std::mutex m_mutex;
    std::map<std::string, Entry> m_entries;
};

} // namespace rg::zip
