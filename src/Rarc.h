#pragma once

#include <cstdint>
#include <string>
#include <vector>

// RARC is the archive format of GameCube/Wii-era Nintendo engines (JKRArchive),
// used for .arc files in Twilight Princess HD and others. Big-endian.
//
// Only file contents can be replaced: the directory tree, names and flags are
// kept as they are. That is enough to merge mods that edit files inside the
// same archive, which is by far the common case.
namespace rarc {

struct Entry {
    std::string name;
    uint8_t flags = 0; // 0x01 file, 0x02 directory, 0x10 preload to MRAM, 0x20 ARAM
    uint32_t dataOffset = 0; // files: offset in the data section; dirs: node index
    uint32_t size = 0;
    bool isFile() const { return (flags & 0x01) && !(flags & 0x02); }
};

struct Archive {
    std::string raw;          // the archive as parsed
    std::vector<Entry> entries;
    uint32_t entryTable = 0;  // absolute offset of the entry table
    uint32_t dataStart = 0;   // absolute offset of the data section

    std::string fileData(size_t entry) const;
};

bool isRarc(const std::string &data);
bool parse(const std::string &data, Archive &out);

// True if both archives have the same tree (names, flags, directories), so
// their files can be matched by entry index.
bool sameLayout(const Archive &a, const Archive &b);

// Rebuilds `layout` with new contents for its files. `data` has one element
// per entry; only file entries are used.
std::string rebuild(const Archive &layout, const std::vector<std::string> &data);

} // namespace rarc
