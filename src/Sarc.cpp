#include "Sarc.h"

#include <algorithm>
#include <cstring>

namespace sarc {

namespace {

const uint32_t kMaxAlignment = 0x2000;

struct Reader {
    const unsigned char *p;
    size_t n;
    bool be;
    bool ok = true;

    uint16_t u16(size_t off) {
        if (off + 2 > n) {
            ok = false;
            return 0;
        }
        return be ? (uint16_t) (p[off] << 8 | p[off + 1]) : (uint16_t) (p[off + 1] << 8 | p[off]);
    }
    uint32_t u32(size_t off) {
        if (off + 4 > n) {
            ok = false;
            return 0;
        }
        const unsigned char *q = p + off;
        return be ? (uint32_t) q[0] << 24 | (uint32_t) q[1] << 16 | (uint32_t) q[2] << 8 | q[3]
                  : (uint32_t) q[3] << 24 | (uint32_t) q[2] << 16 | (uint32_t) q[1] << 8 | q[0];
    }
};

struct Writer {
    std::string s;
    bool be;

    void u16(uint16_t v) {
        if (be) {
            s.push_back((char) (v >> 8));
            s.push_back((char) v);
        } else {
            s.push_back((char) v);
            s.push_back((char) (v >> 8));
        }
    }
    void u32(uint32_t v) {
        if (be) {
            u16((uint16_t) (v >> 16));
            u16((uint16_t) v);
        } else {
            u16((uint16_t) v);
            u16((uint16_t) (v >> 16));
        }
    }
    void put32(size_t off, uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            int shift = be ? 24 - 8 * i : 8 * i;
            s[off + (size_t) i] = (char) (v >> shift);
        }
    }
    void pad(size_t align) {
        while (s.size() % align) s.push_back('\0');
    }
};

uint32_t nameHash(const std::string &name, uint32_t key) {
    uint32_t h = 0;
    for (char c : name) h = h * key + (uint32_t) (int32_t) (signed char) c;
    return h;
}

// Largest power of two (4..kMaxAlignment) that divides `offset`.
uint32_t inferAlignment(uint32_t offset) {
    uint32_t a = 4;
    while (a < kMaxAlignment && offset % (a * 2) == 0) a *= 2;
    return a;
}

} // namespace

const File *Archive::find(const std::string &name) const {
    for (const auto &f : files)
        if (f.name == name) return &f;
    return nullptr;
}

bool isSarc(const std::string &data) {
    return data.size() >= 0x20 && std::memcmp(data.data(), "SARC", 4) == 0;
}

bool parse(const std::string &data, Archive &out) {
    if (!isSarc(data)) return false;
    const unsigned char *p = (const unsigned char *) data.data();
    bool be;
    if (p[6] == 0xFE && p[7] == 0xFF) be = true;
    else if (p[6] == 0xFF && p[7] == 0xFE) be = false;
    else return false;

    Reader r{p, data.size(), be};
    uint16_t headerSize = r.u16(4);
    uint32_t dataOffset = r.u32(0x0C);
    size_t sfat = headerSize;
    if (sfat + 0x0C > data.size() || std::memcmp(p + sfat, "SFAT", 4) != 0) return false;
    uint16_t sfatHeader = r.u16(sfat + 4);
    uint16_t count = r.u16(sfat + 6);
    uint32_t hashKey = r.u32(sfat + 8);
    size_t nodes = sfat + sfatHeader;
    size_t sfnt = nodes + (size_t) count * 0x10;
    if (sfnt + 8 > data.size() || std::memcmp(p + sfnt, "SFNT", 4) != 0) return false;
    size_t names = sfnt + r.u16(sfnt + 4);
    if (dataOffset > data.size() || names > dataOffset) return false;

    out = Archive();
    out.bigEndian = be;
    out.hashKey = hashKey;
    out.files.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        size_t node = nodes + (size_t) i * 0x10;
        uint32_t attrs = r.u32(node + 4);
        uint32_t start = r.u32(node + 8);
        uint32_t end = r.u32(node + 12);
        if (!r.ok) return false;
        if ((attrs >> 24) == 0) return false; // nameless entry
        size_t nameOff = names + (size_t) (attrs & 0xFFFFFF) * 4;
        if (nameOff >= dataOffset) return false;
        const char *nameP = (const char *) p + nameOff;
        const void *nul = std::memchr(nameP, 0, dataOffset - nameOff);
        if (!nul) return false;
        size_t nameLen = (size_t) ((const char *) nul - nameP);
        if (end < start || (uint64_t) dataOffset + end > data.size()) return false;

        File f;
        f.name.assign(nameP, nameLen);
        f.data.assign(data, dataOffset + start, end - start);
        f.alignment = inferAlignment(dataOffset + start);
        out.files.push_back(std::move(f));
    }
    return r.ok;
}

std::string write(const Archive &archive) {
    std::vector<const File *> sorted;
    sorted.reserve(archive.files.size());
    for (const auto &f : archive.files) sorted.push_back(&f);
    std::stable_sort(sorted.begin(), sorted.end(), [&](const File *a, const File *b) {
        return nameHash(a->name, archive.hashKey) < nameHash(b->name, archive.hashKey);
    });

    Writer w{std::string(), archive.bigEndian};
    // SARC header; file size and data offset are patched in at the end.
    w.s.append("SARC", 4);
    w.u16(0x14);
    w.u16(0xFEFF);
    w.u32(0);
    w.u32(0);
    w.u16(0x0100);
    w.u16(0);

    w.s.append("SFAT", 4);
    w.u16(0x0C);
    w.u16((uint16_t) sorted.size());
    w.u32(archive.hashKey);

    // Name table offsets (in 4-byte units) and data ranges.
    uint32_t nameOff = 0;
    uint32_t dataPos = 0;
    uint32_t maxAlign = 4;
    std::vector<uint32_t> starts;
    starts.reserve(sorted.size());
    for (const File *f : sorted) {
        uint32_t a = std::max<uint32_t>(4, std::min(f->alignment, kMaxAlignment));
        maxAlign = std::max(maxAlign, a);
        dataPos = (dataPos + a - 1) / a * a;
        starts.push_back(dataPos);
        w.u32(nameHash(f->name, archive.hashKey));
        w.u32(0x01000000u | (nameOff / 4));
        w.u32(dataPos);
        w.u32(dataPos + (uint32_t) f->data.size());
        dataPos += (uint32_t) f->data.size();
        nameOff += (uint32_t) ((f->name.size() + 1 + 3) / 4 * 4);
    }

    w.s.append("SFNT", 4);
    w.u16(0x08);
    w.u16(0);
    for (const File *f : sorted) {
        w.s.append(f->name);
        w.s.push_back('\0');
        w.pad(4);
    }

    // Data offsets are relative to the data section, which is aligned to the
    // strictest file so every file's absolute offset keeps its alignment.
    w.pad(maxAlign);
    uint32_t dataOffset = (uint32_t) w.s.size();
    for (size_t i = 0; i < sorted.size(); ++i) {
        w.s.resize(dataOffset + starts[i], '\0');
        w.s.append(sorted[i]->data);
    }
    w.put32(0x08, (uint32_t) w.s.size());
    w.put32(0x0C, dataOffset);
    return w.s;
}

} // namespace sarc
