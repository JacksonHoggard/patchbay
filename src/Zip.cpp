#include "Zip.h"
#include "Fs.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <zlib.h>

namespace zip {

namespace {

constexpr uint32_t kLocalSig     = 0x04034b50;
constexpr uint32_t kCentralSig   = 0x02014b50;
constexpr uint32_t kEndSig       = 0x06054b50;
constexpr uint32_t kEnd64Sig     = 0x06064b50;
constexpr uint32_t kDescriptorSig = 0x08074b50;
constexpr size_t kBufSize = 512 * 1024;

uint16_t le16(const unsigned char *p) { return (uint16_t) (p[0] | (p[1] << 8)); }
uint32_t le32(const unsigned char *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}
uint64_t le64(const unsigned char *p) { return (uint64_t) le32(p) | ((uint64_t) le32(p + 4) << 32); }

// Forward-only buffered reader with an exposed window so zlib can consume
// directly from it and leave the remainder for the next record.
class Reader {
public:
    explicit Reader(FILE *f) : f_(f), buf_(kBufSize) {}

    // Ensures at least `n` bytes are buffered (n <= kBufSize). Returns false
    // at EOF.
    bool want(size_t n) {
        if (end_ - start_ >= n) return true;
        if (start_ > 0) {
            std::memmove(buf_.data(), buf_.data() + start_, end_ - start_);
            end_ -= start_;
            start_ = 0;
        }
        while (end_ - start_ < n) {
            size_t got = std::fread(buf_.data() + end_, 1, buf_.size() - end_, f_);
            if (got == 0) return false;
            end_ += got;
        }
        return true;
    }

    // Buffers whatever is available (at least 1 byte unless EOF).
    size_t avail() {
        if (end_ == start_) want(1);
        return end_ - start_;
    }

    const unsigned char *data() const { return buf_.data() + start_; }
    void consume(size_t n) {
        start_ += n;
        pos_ += n;
    }
    bool read(void *dst, size_t n) {
        unsigned char *d = static_cast<unsigned char *>(dst);
        while (n > 0) {
            size_t a = avail();
            if (a == 0) return false;
            size_t take = a < n ? a : n;
            std::memcpy(d, data(), take);
            consume(take);
            d += take;
            n -= take;
        }
        return true;
    }
    bool skip(uint64_t n) {
        while (n > 0) {
            size_t a = avail();
            if (a == 0) return false;
            size_t take = (uint64_t) a < n ? a : (size_t) n;
            consume(take);
            n -= take;
        }
        return true;
    }
    int64_t pos() const { return pos_; }

private:
    FILE *f_;
    std::vector<unsigned char> buf_;
    size_t start_ = 0, end_ = 0;
    int64_t pos_ = 0;
};

class OutFile {
public:
    bool open(const std::string &path) {
        f_ = std::fopen(path.c_str(), "wb");
        if (f_) std::setvbuf(f_, buf_.data(), _IOFBF, buf_.size());
        return f_ != nullptr;
    }
    bool write(const void *p, size_t n) { return n == 0 || std::fwrite(p, 1, n, f_) == n; }
    bool close() {
        if (!f_) return true;
        bool ok = std::fclose(f_) == 0;
        f_ = nullptr;
        return ok;
    }
    ~OutFile() { close(); }

private:
    FILE *f_ = nullptr;
    std::vector<char> buf_ = std::vector<char>(256 * 1024);
};

} // namespace

std::string sanitizeEntryName(const std::string &raw) {
    std::string name = raw;
    for (auto &c : name)
        if (c == '\\') c = '/';
    if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos) return "";
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7F) return "";

    std::string out;
    size_t i = 0;
    while (i <= name.size()) {
        size_t j = name.find('/', i);
        if (j == std::string::npos) j = name.size();
        std::string part = name.substr(i, j - i);
        if (part == "..") return "";
        if (!part.empty() && part != ".") {
            if (!out.empty()) out += '/';
            out += part;
        }
        i = j + 1;
    }
    // Keep a trailing slash so callers can tell directories apart.
    if (!out.empty() && name.back() == '/') out += '/';
    return out;
}

bool extract(const std::string &zipPath, const std::string &destDir, int64_t zipSize,
             const std::function<void(int64_t, int64_t)> &progress, const std::atomic<bool> *cancel,
             std::string &error) {
    FILE *f = std::fopen(zipPath.c_str(), "rb");
    if (!f) {
        error = "Cannot open archive";
        return false;
    }
    std::vector<char> fbuf(64 * 1024);
    std::setvbuf(f, fbuf.data(), _IOFBF, fbuf.size());

    struct Closer {
        FILE *f;
        ~Closer() { std::fclose(f); }
    } closer{f};

    Reader rd(f);
    std::vector<unsigned char> outBuf(256 * 1024);
    int files = 0;

    auto fail = [&](const std::string &msg) {
        error = msg;
        return false;
    };

    while (true) {
        if (cancel && cancel->load()) return fail("Cancelled");

        unsigned char sigb[4];
        // A complete archive always ends with a central directory. Running
        // out of data first means the download was cut short.
        if (!rd.read(sigb, 4)) return fail("Archive is truncated (incomplete download?)");
        uint32_t sig = le32(sigb);
        if (sig == kCentralSig || sig == kEndSig || sig == kEnd64Sig) break;
        if (sig != kLocalSig) return fail("Not a ZIP archive (bad header)");

        unsigned char h[26];
        if (!rd.read(h, sizeof(h))) return fail("Truncated local header");
        uint16_t flags = le16(h + 2);
        uint16_t method = le16(h + 4);
        uint32_t crcExpected = le32(h + 10);
        uint64_t csize = le32(h + 14);
        uint64_t usize = le32(h + 18);
        uint16_t nameLen = le16(h + 22);
        uint16_t extraLen = le16(h + 24);

        std::string rawName(nameLen, '\0');
        std::vector<unsigned char> extra(extraLen);
        if (!rd.read(&rawName[0], nameLen) || !rd.read(extra.data(), extraLen))
            return fail("Truncated local header");

        bool zip64 = false;
        for (size_t p = 0; p + 4 <= extra.size();) {
            uint16_t id = le16(&extra[p]);
            uint16_t len = le16(&extra[p + 2]);
            if (p + 4 + len > extra.size()) break;
            if (id == 0x0001) {
                zip64 = true;
                size_t q = p + 4;
                if (usize == 0xFFFFFFFFu && q + 8 <= p + 4 + len) {
                    usize = le64(&extra[q]);
                    q += 8;
                }
                if (csize == 0xFFFFFFFFu && q + 8 <= p + 4 + len) csize = le64(&extra[q]);
            }
            p += 4 + len;
        }

        if (flags & 0x0001) return fail("Encrypted archives are not supported");
        if (method != 0 && method != 8)
            return fail("Unsupported compression method " + std::to_string(method) + " in " + rawName);
        bool hasDescriptor = (flags & 0x0008) != 0;

        std::string rel = sanitizeEntryName(rawName);
        if (rel.empty()) return fail("Unsafe path in archive: " + rawName);
        bool isDir = rel.back() == '/';
        std::string outPath = destDir + "/" + (isDir ? rel.substr(0, rel.size() - 1) : rel);

        OutFile out;
        if (isDir) {
            if (!fs::mkdirs(outPath)) return fail("Cannot create folder " + rel);
        } else {
            if (!fs::mkdirs(fs::parent(outPath))) return fail("Cannot create folder for " + rel);
            if (!out.open(outPath)) return fail("Cannot write " + rel);
        }

        uLong crc = crc32(0L, Z_NULL, 0);

        if (method == 0) {
            if (hasDescriptor && csize == 0 && !isDir)
                return fail("Stored entry with data descriptor is not supported: " + rel);
            uint64_t left = csize;
            while (left > 0) {
                size_t a = rd.avail();
                if (a == 0) return fail("Truncated data for " + rel);
                size_t take = (uint64_t) a < left ? a : (size_t) left;
                if (!isDir) {
                    if (!out.write(rd.data(), take)) return fail("SD card write failed (card full?)");
                    crc = crc32(crc, rd.data(), (uInt) take);
                }
                rd.consume(take);
                left -= take;
                if (progress) progress(rd.pos(), zipSize);
                if (cancel && cancel->load()) return fail("Cancelled");
            }
        } else {
            z_stream zs;
            std::memset(&zs, 0, sizeof(zs));
            if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return fail("inflateInit failed");
            struct InflateEnd {
                z_stream *z;
                ~InflateEnd() { inflateEnd(z); }
            } ie{&zs};

            // Without a descriptor the compressed size is authoritative; with
            // one, the deflate stream's own end marker tells us where to stop.
            uint64_t left = hasDescriptor ? UINT64_MAX : csize;
            int zr = Z_OK;
            while (zr != Z_STREAM_END) {
                size_t a = rd.avail();
                if (a == 0 || left == 0) return fail("Truncated data for " + rel);
                size_t give = (uint64_t) a < left ? a : (size_t) left;
                zs.next_in = const_cast<Bytef *>(rd.data());
                zs.avail_in = (uInt) give;
                do {
                    zs.next_out = outBuf.data();
                    zs.avail_out = (uInt) outBuf.size();
                    zr = inflate(&zs, Z_NO_FLUSH);
                    if (zr != Z_OK && zr != Z_STREAM_END && zr != Z_BUF_ERROR)
                        return fail("Corrupt data in " + rel);
                    size_t produced = outBuf.size() - zs.avail_out;
                    if (produced && !isDir) {
                        if (!out.write(outBuf.data(), produced)) return fail("SD card write failed (card full?)");
                        crc = crc32(crc, outBuf.data(), (uInt) produced);
                    }
                    if (zr == Z_BUF_ERROR && produced == 0) break; // needs more input
                } while (zr != Z_STREAM_END && (zs.avail_in > 0 || zs.avail_out == 0));
                size_t used = give - zs.avail_in;
                rd.consume(used);
                if (left != UINT64_MAX) left -= used;
                if (progress) progress(rd.pos(), zipSize);
                if (cancel && cancel->load()) return fail("Cancelled");
            }
            // Skip any padding the writer left inside the declared size.
            if (!hasDescriptor && left > 0 && !rd.skip(left)) return fail("Truncated data for " + rel);
        }

        if (hasDescriptor) {
            unsigned char d[24];
            if (!rd.read(d, 4)) return fail("Truncated data descriptor");
            size_t sizeBytes = zip64 ? 16 : 8;
            uint32_t first = le32(d);
            if (first == kDescriptorSig) {
                if (!rd.read(d, 4)) return fail("Truncated data descriptor");
                crcExpected = le32(d);
            } else {
                crcExpected = first;
            }
            if (!rd.skip(sizeBytes)) return fail("Truncated data descriptor");
        }

        if (!out.close()) return fail("SD card write failed (card full?)");
        if (!isDir && (uint32_t) crc != crcExpected) return fail("CRC mismatch in " + rel + " (corrupt download?)");
        files++;
    }

    if (files == 0) {
        error = "Archive contains no files";
        return false;
    }
    return true;
}

} // namespace zip
