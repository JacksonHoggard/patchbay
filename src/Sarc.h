#pragma once

#include <cstdint>
#include <string>
#include <vector>

// SARC is Nintendo's flat archive format (.sarc, .pack, .bars, and the
// payload of most .szs files). Wii U archives are big-endian; little-endian
// ones (Switch ports) are read and written too.
namespace sarc {

struct File {
    std::string name;
    std::string data;
    // Alignment the data needs inside the archive. Not stored in the format,
    // so it is inferred from where the file sat in the archive it came from.
    uint32_t alignment = 4;
};

struct Archive {
    bool bigEndian = true;
    uint32_t hashKey = 0x65;
    std::vector<File> files;

    const File *find(const std::string &name) const;
};

bool isSarc(const std::string &data);

// Fails on a malformed archive or one with nameless entries (those cannot be
// matched across archives, so they cannot be merged).
bool parse(const std::string &data, Archive &out);

std::string write(const Archive &archive);

} // namespace sarc
