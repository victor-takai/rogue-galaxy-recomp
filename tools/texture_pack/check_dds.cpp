// check_dds.py's other half: decodes the DDS files it is given with the
// game's reader (src/dds.h) and writes each one's RGBA beside it as NAME.rgba.
//   clang++ -std=c++17 -I src tools/texture_pack/check_dds.cpp -o check_dds
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "dds.h"

int main(int argc, char **argv)
{
    int failures = 0;
    for (int i = 1; i < argc; ++i)
    {
        std::ifstream file(argv[i], std::ios::binary);
        const std::vector<char> text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgba;
        const char *error = rg::dds::decode(reinterpret_cast<const uint8_t *>(text.data()), text.size(), width,
                                            height, rgba);
        if (error)
        {
            std::fprintf(stderr, "%s: %s\n", argv[i], error);
            ++failures;
            continue;
        }
        std::ofstream out(std::string(argv[i]) + ".rgba", std::ios::binary);
        out.write(reinterpret_cast<const char *>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
    }
    return failures ? 1 : 0;
}
