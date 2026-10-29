// check_zip.py's other half: reads every entry of the zip it is given with the
// game's reader (src/zip_reader.h) and writes each into the folder given, named
// by its place in the archive with / turned into __.
//   clang++ -std=c++17 -I src tools/texture_pack/check_zip.cpp -o check_zip
#include <cstdio>
#include <fstream>
#include <string>

#include "zip_reader.h"

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        return 2;
    }
    rg::zip::Archive archive;
    if (const char *error = archive.open(argv[1]))
    {
        std::fprintf(stderr, "%s: %s\n", argv[1], error);
        return 1;
    }
    int failures = 0;
    for (const auto &entry : archive.entries())
    {
        std::vector<uint8_t> bytes;
        if (!archive.read(entry.first, bytes))
        {
            std::fprintf(stderr, "%s: cannot read\n", entry.first.c_str());
            ++failures;
            continue;
        }
        std::string name = entry.first;
        for (size_t at; (at = name.find('/')) != std::string::npos;)
        {
            name.replace(at, 1, "__");
        }
        std::ofstream out(std::string(argv[2]) + "/" + name, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return failures ? 1 : 0;
}
