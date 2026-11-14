#include "Rarc.h"

#include <algorithm>
#include <cstring>
#include <numeric>

namespace rarc {

namespace {

const uint32_t kAlign = 0x20;

uint32_t be32(const std::string &s, size_t off) {
    const unsigned char *p = (const unsigned char *) s.data() + off;
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

uint16_t be16(const std::string &s, size_t off) {
    const unsigned char *p = (const unsigned char *) s.data() + off;
    return (uint16_t) (p[0] << 8 | p[1]);
}

void put32(std::string &s, size_t off, uint32_t v) {
    s[off] = (char) (v >> 24);
    s[off + 1] = (char) (v >> 16);
    s[off + 2] = (char) (v >> 8);
    s[off + 3] = (char) v;
}

uint32_t alignUp(uint32_t v) { return (v + kAlign - 1) / kAlign * kAlign; }

} // namespace

std::string Archive::fileData(size_t i) const {
    const Entry &e = entries[i];
    return raw.substr(dataStart + e.dataOffset, e.size);
}

bool isRarc(const std::string &data) {
    return data.size() >= 0x40 && std::memcmp(data.data(), "RARC", 4) == 0;
}

bool parse(const std::string &data, Archive &out) {
    if (!isRarc(data)) return false;
    const size_t n = data.size();
    uint32_t headerSize = be32(data, 0x08);
    uint32_t dataOff = be32(data, 0x0C);
    if (headerSize < 0x20 || headerSize + 0x20 > n) return false;
    size_t info = headerSize;
    uint32_t entryCount = be32(data, info + 0x08);
    uint32_t entryOff = be32(data, info + 0x0C) + headerSize;
    uint32_t strOff = be32(data, info + 0x14) + headerSize;
    uint32_t dataStart = dataOff + headerSize;
    if ((uint64_t) entryOff + (uint64_t) entryCount * 20 > n || strOff >= n || dataStart > n) return false;

    out = Archive();
    out.raw = data;
    out.entryTable = entryOff;
    out.dataStart = dataStart;
    out.entries.reserve(entryCount);
    for (uint32_t i = 0; i < entryCount; ++i) {
        size_t e = entryOff + (size_t) i * 20;
        Entry en;
        en.flags = (uint8_t) data[e + 4];
        uint16_t nameOff = be16(data, e + 6);
        en.dataOffset = be32(data, e + 8);
        en.size = be32(data, e + 12);
        size_t np = strOff + nameOff;
        if (np >= n) return false;
        const void *nul = std::memchr(data.data() + np, 0, n - np);
        if (!nul) return false;
        en.name.assign(data.data() + np, (const char *) nul - (data.data() + np));
        if (en.isFile() && (uint64_t) dataStart + en.dataOffset + en.size > n) return false;
        out.entries.push_back(std::move(en));
    }
    return true;
}

bool sameLayout(const Archive &a, const Archive &b) {
    if (a.entries.size() != b.entries.size()) return false;
    for (size_t i = 0; i < a.entries.size(); ++i) {
        const Entry &x = a.entries[i], &y = b.entries[i];
        if (x.name != y.name || x.flags != y.flags) return false;
        if (!x.isFile() && x.dataOffset != y.dataOffset) return false;
    }
    return true;
}

std::string rebuild(const Archive &layout, const std::vector<std::string> &data) {
    // Everything before the data section (header, nodes, entries, names)
    // stays; only the entry table's offsets and sizes change.
    std::string out = layout.raw.substr(0, layout.dataStart);

    // Keep the original data order (preloaded files usually come first).
    std::vector<size_t> files;
    for (size_t i = 0; i < layout.entries.size(); ++i)
        if (layout.entries[i].isFile()) files.push_back(i);
    std::stable_sort(files.begin(), files.end(), [&](size_t a, size_t b) {
        return layout.entries[a].dataOffset < layout.entries[b].dataOffset;
    });

    int64_t mramDelta = 0, aramDelta = 0;
    uint32_t pos = 0;
    for (size_t i : files) {
        const Entry &e = layout.entries[i];
        const std::string &d = data[i];
        size_t ent = layout.entryTable + i * 20;
        put32(out, ent + 8, pos);
        put32(out, ent + 12, (uint32_t) d.size());
        out.append(d);
        uint32_t padded = alignUp((uint32_t) d.size());
        out.resize(layout.dataStart + pos + padded, '\0');
        pos += padded;
        int64_t delta = (int64_t) padded - (int64_t) alignUp(e.size);
        if (e.flags & 0x10) mramDelta += delta;
        if (e.flags & 0x20) aramDelta += delta;
    }

    const std::string &raw = layout.raw;
    put32(out, 0x04, (uint32_t) out.size());
    put32(out, 0x10, pos);
    // Preload sizes: shift by how much the preloaded files grew or shrank, so
    // whatever convention the original tool used is kept.
    if (be32(raw, 0x14)) put32(out, 0x14, (uint32_t) ((int64_t) be32(raw, 0x14) + mramDelta));
    if (be32(raw, 0x18)) put32(out, 0x18, (uint32_t) ((int64_t) be32(raw, 0x18) + aramDelta));
    return out;
}

} // namespace rarc
