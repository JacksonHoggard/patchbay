#include "Yaz0.h"

#include <cstring>
#include <vector>

namespace yaz0 {

static uint32_t be32(const unsigned char *p) {
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

static void putBe32(std::string &s, uint32_t v) {
    s.push_back((char) (v >> 24));
    s.push_back((char) (v >> 16));
    s.push_back((char) (v >> 8));
    s.push_back((char) v);
}

bool isYaz0(const std::string &data) {
    return data.size() >= 16 && std::memcmp(data.data(), "Yaz0", 4) == 0;
}

uint32_t alignmentHint(const std::string &data) {
    return isYaz0(data) ? be32((const unsigned char *) data.data() + 8) : 0;
}

bool decompress(const std::string &in, std::string &out) {
    if (!isYaz0(in)) return false;
    const unsigned char *src = (const unsigned char *) in.data();
    const size_t srcLen = in.size();
    const uint32_t size = be32(src + 4);
    out.assign(size, '\0');
    unsigned char *dst = (unsigned char *) &out[0];

    size_t s = 16, d = 0;
    while (d < size) {
        if (s >= srcLen) return false;
        uint8_t group = src[s++];
        for (int bit = 0; bit < 8 && d < size; ++bit, group <<= 1) {
            if (group & 0x80) {
                if (s >= srcLen) return false;
                dst[d++] = src[s++];
                continue;
            }
            if (s + 2 > srcLen) return false;
            uint8_t b1 = src[s], b2 = src[s + 1];
            s += 2;
            size_t dist = (size_t) ((b1 & 0x0F) << 8 | b2) + 1;
            size_t len = b1 >> 4;
            if (len == 0) {
                if (s >= srcLen) return false;
                len = (size_t) src[s++] + 0x12;
            } else {
                len += 2;
            }
            if (dist > d) return false;
            if (len > size - d) len = size - d;
            // Byte by byte: the source may overlap the destination (RLE).
            for (size_t i = 0; i < len; ++i, ++d) dst[d] = dst[d - dist];
        }
    }
    return true;
}

std::string compress(const std::string &in, uint32_t alignment) {
    const unsigned char *src = (const unsigned char *) in.data();
    const size_t n = in.size();
    const size_t kWindow = 0x1000, kMaxLen = 0x111, kMinLen = 3;
    // Bounds the search on the console's slow CPU; longer chains gain little.
    const int kMaxChain = 48;
    const size_t kHashSize = 1 << 15;

    std::string out;
    out.reserve(16 + n + n / 8 + 16);
    out.append("Yaz0", 4);
    putBe32(out, (uint32_t) n);
    putBe32(out, alignment);
    putBe32(out, 0);

    // head[h] = last position with hash h; prev[pos % window] = previous one.
    std::vector<int32_t> head(kHashSize, -1);
    std::vector<int32_t> prev(kWindow, -1);
    auto hash3 = [&](size_t p) {
        return ((uint32_t) src[p] << 10 ^ (uint32_t) src[p + 1] << 5 ^ src[p + 2]) & (kHashSize - 1);
    };
    auto insert = [&](size_t p) {
        if (p + kMinLen > n) return;
        uint32_t h = hash3(p);
        prev[p % kWindow] = head[h];
        head[h] = (int32_t) p;
    };
    auto findMatch = [&](size_t p, size_t &bestDist) -> size_t {
        if (p + kMinLen > n) return 0;
        size_t best = 0;
        size_t maxLen = n - p < kMaxLen ? n - p : kMaxLen;
        int32_t cand = head[hash3(p)];
        for (int chain = 0; cand >= 0 && chain < kMaxChain; ++chain) {
            size_t c = (size_t) cand;
            if (p - c > kWindow) break;
            if (src[c + best] == src[p + best]) {
                size_t len = 0;
                while (len < maxLen && src[c + len] == src[p + len]) ++len;
                if (len > best) {
                    best = len;
                    bestDist = p - c;
                    if (len == maxLen) break;
                }
            }
            int32_t next = prev[c % kWindow];
            if (next >= cand) break; // slot was overwritten by a newer position
            cand = next;
        }
        return best >= kMinLen ? best : 0;
    };

    size_t p = 0;
    while (p < n) {
        size_t groupPos = out.size();
        out.push_back(0);
        uint8_t group = 0;
        for (int bit = 0; bit < 8 && p < n; ++bit) {
            size_t dist = 0;
            size_t len = findMatch(p, dist);
            // Lazy matching: emit a literal if the next byte starts a longer match.
            if (len && len < kMaxLen && p + 1 < n) {
                insert(p);
                size_t dist2 = 0;
                size_t len2 = findMatch(p + 1, dist2);
                if (len2 > len + 1) {
                    group |= (uint8_t) (0x80 >> bit);
                    out.push_back((char) src[p]);
                    ++p;
                    continue;
                }
                // p is already in the chain; insert the rest of the match below.
                size_t d = dist - 1;
                if (len >= 0x12) {
                    out.push_back((char) (d >> 8));
                    out.push_back((char) (d & 0xFF));
                    out.push_back((char) (len - 0x12));
                } else {
                    out.push_back((char) ((len - 2) << 4 | d >> 8));
                    out.push_back((char) (d & 0xFF));
                }
                for (size_t i = 1; i < len; ++i) insert(p + i);
                p += len;
                continue;
            }
            if (len) {
                size_t d = dist - 1;
                if (len >= 0x12) {
                    out.push_back((char) (d >> 8));
                    out.push_back((char) (d & 0xFF));
                    out.push_back((char) (len - 0x12));
                } else {
                    out.push_back((char) ((len - 2) << 4 | d >> 8));
                    out.push_back((char) (d & 0xFF));
                }
                for (size_t i = 0; i < len; ++i) insert(p + i);
                p += len;
            } else {
                group |= (uint8_t) (0x80 >> bit);
                out.push_back((char) src[p]);
                insert(p);
                ++p;
            }
        }
        out[groupPos] = (char) group;
    }
    return out;
}

} // namespace yaz0
