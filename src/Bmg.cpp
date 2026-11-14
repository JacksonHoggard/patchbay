#include "Bmg.h"

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <map>

namespace bmg {

namespace {

const size_t kHeader = 0x20;

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

void append32(std::string &s, uint32_t v) {
    s.push_back((char) (v >> 24));
    s.push_back((char) (v >> 16));
    s.push_back((char) (v >> 8));
    s.push_back((char) v);
}

// A message ends at a 0 byte outside a control code. Control codes are
// 0x1A, <total length>, ... and may contain zeros.
bool messageEnd(const std::string &dat, size_t start, size_t &end) {
    size_t i = start;
    while (i < dat.size()) {
        unsigned char c = (unsigned char) dat[i];
        if (c == 0x1A) {
            if (i + 1 >= dat.size()) return false;
            unsigned char len = (unsigned char) dat[i + 1];
            if (len < 2) return false;
            i += len;
            continue;
        }
        if (c == 0) {
            end = i + 1;
            return true;
        }
        ++i;
    }
    return false;
}

struct Section {
    std::string magic;
    size_t offset, size;
};

bool sectionsOf(const std::string &d, std::vector<Section> &out) {
    uint32_t count = be32(d, 0x0C);
    size_t off = kHeader;
    for (uint32_t i = 0; i < count; ++i) {
        if (off + 8 > d.size()) return false;
        uint32_t size = be32(d, off + 4);
        if (size < 8 || off + size > d.size()) return false;
        out.push_back({d.substr(off, 4), off, size});
        off += size;
    }
    return true;
}

} // namespace

bool isBmg(const std::string &data) {
    return data.size() >= kHeader && std::memcmp(data.data(), "MESGbmg1", 8) == 0;
}

bool parse(const std::string &data, File &out) {
    if (!isBmg(data)) return false;
    // 0 = legacy, 1 = CP1252, 3 = Shift-JIS: all byte-terminated. 2 is
    // UTF-16, which this parser does not handle.
    unsigned char enc = (unsigned char) data[0x10];
    if (enc == 2 || enc > 3) return false;

    std::vector<Section> secs;
    if (!sectionsOf(data, secs)) return false;
    const Section *inf = nullptr, *dat = nullptr;
    for (const auto &s : secs) {
        if (s.magic == "INF1") inf = &s;
        else if (s.magic == "DAT1") dat = &s;
    }
    if (!inf || !dat || inf->size < 16) return false;

    out = File();
    out.raw = data;
    for (const auto &s : secs) {
        out.sectionMagic.push_back(s.magic);
        bool msgSection = &s == inf || &s == dat;
        out.sections.push_back(msgSection ? std::string() : data.substr(s.offset, s.size));
    }

    uint16_t count = be16(data, inf->offset + 8);
    uint16_t entrySize = be16(data, inf->offset + 10);
    if (entrySize < 4 || 16 + (size_t) count * entrySize > inf->size) return false;
    std::string &datBytes = out.dat;
    datBytes = data.substr(dat->offset + 8, dat->size - 8);
    out.messages.reserve(count);
    out.textOffsets.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        size_t e = inf->offset + 16 + (size_t) i * entrySize;
        uint32_t textOff = be32(data, e);
        size_t end;
        if (textOff >= datBytes.size() || !messageEnd(datBytes, textOff, end)) return false;
        Message m;
        m.attrs = data.substr(e + 4, entrySize - 4);
        m.text = datBytes.substr(textOff, end - textOff);
        out.messages.push_back(std::move(m));
        out.textOffsets.push_back(textOff);
    }
    return true;
}

bool compatible(const File &a, const File &b) {
    if (a.messages.size() != b.messages.size() || a.sectionMagic != b.sectionMagic) return false;
    for (size_t i = 0; i < a.messages.size(); ++i)
        if (a.messages[i].attrs.size() != b.messages[i].attrs.size()) return false;
    return true;
}

std::string rebuild(const File &layout, const std::vector<Message> &messages,
                    const std::vector<std::string> &sections) {
    std::vector<Section> secs;
    sectionsOf(layout.raw, secs);

    // Text: unchanged messages keep their offset into the original data.
    // Changed text is appended once per distinct string. DAT1 data may end
    // with padding, so trim trailing zeros past the last message first.
    const std::string &raw = layout.raw;
    std::string dat = layout.dat;
    size_t used = 0;
    for (size_t i = 0; i < layout.messages.size(); ++i)
        used = std::max(used, (size_t) layout.textOffsets[i] + layout.messages[i].text.size());
    std::map<std::string, uint32_t> added;
    std::vector<uint32_t> offsets;
    offsets.reserve(messages.size());
    for (size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].text == layout.messages[i].text) {
            offsets.push_back(layout.textOffsets[i]);
            continue;
        }
        auto it = added.find(messages[i].text);
        if (it == added.end()) {
            if (added.empty()) dat.resize(used);
            it = added.emplace(messages[i].text, (uint32_t) dat.size()).first;
            dat.append(messages[i].text);
        }
        offsets.push_back(it->second);
    }

    std::string out = raw.substr(0, kHeader);
    for (size_t si = 0; si < secs.size(); ++si) {
        const Section &s = secs[si];
        if (s.magic == "INF1") {
            std::string inf = raw.substr(s.offset, 16);
            size_t entrySize = be16(raw, s.offset + 10);
            for (size_t i = 0; i < messages.size(); ++i) {
                append32(inf, offsets[i]);
                inf.append(messages[i].attrs);
                inf.resize(16 + (i + 1) * entrySize, '\0');
            }
            // Keep whatever padding followed the entries.
            if (inf.size() < s.size) inf.append(raw, s.offset + inf.size(), s.size - inf.size());
            while (inf.size() % 32) inf.push_back('\0');
            put32(inf, 4, (uint32_t) inf.size());
            out.append(inf);
        } else if (s.magic == "DAT1") {
            std::string sec = "DAT1";
            append32(sec, 0);
            sec.append(dat);
            while (sec.size() % 32) sec.push_back('\0');
            put32(sec, 4, (uint32_t) sec.size());
            out.append(sec);
        } else {
            out.append(sections[si]);
        }
    }
    // The header size field is the file length in some tools and something
    // else in others; shift it by the change in length either way.
    int64_t delta = (int64_t) out.size() - (int64_t) raw.size();
    put32(out, 0x08, (uint32_t) ((int64_t) be32(raw, 0x08) + delta));
    return out;
}

} // namespace bmg
