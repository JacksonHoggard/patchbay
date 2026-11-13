#include "ArchiveLibarchive.h"
#include "Fs.h"
#include "Zip.h"

#include <libarchive/archive.h>
#include <libarchive/archive_entry.h>

#include <cstdio>
#include <vector>
#include <algorithm>
#include <cstring>

namespace archive_libarchive {

namespace {

constexpr size_t kReadBufSize = 256 * 1024;
constexpr size_t kWriteBufSize = 256 * 1024;

struct Source {
    FILE *f = nullptr;
    std::vector<unsigned char> buf = std::vector<unsigned char>(kReadBufSize);
    int64_t pos = 0; // bytes of the archive consumed so far
};

la_ssize_t readCb(struct archive *a, void *client, const void **out) {
    Source *s = static_cast<Source *>(client);
    size_t n = std::fread(s->buf.data(), 1, s->buf.size(), s->f);
    if (n == 0 && std::ferror(s->f)) {
        archive_set_error(a, -1, "Read error");
        return -1;
    }
    s->pos += (int64_t) n;
    *out = s->buf.data();
    return (la_ssize_t) n;
}

la_int64_t seekCb(struct archive *a, void *client, la_int64_t offset, int whence) {
    Source *s = static_cast<Source *>(client);
    // Archives are capped at 2 GiB by the caller, so `long` is enough.
    if (offset > INT32_MAX || offset < INT32_MIN || std::fseek(s->f, (long) offset, whence) != 0) {
        archive_set_error(a, -1, "Seek error");
        return ARCHIVE_FATAL;
    }
    long p = std::ftell(s->f);
    if (p < 0) return ARCHIVE_FATAL;
    s->pos = p;
    return p;
}

int closeCb(struct archive *, void *client) {
    Source *s = static_cast<Source *>(client);
    if (s->f) std::fclose(s->f);
    s->f = nullptr;
    return ARCHIVE_OK;
}

class OutFile {
public:
    // The stdio buffer is shared across entries: allocating and zeroing a
    // fresh 256 KB per file is slow for archives with thousands of files.
    explicit OutFile(std::vector<char> &buf) : buf_(buf) {}

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
    std::vector<char> &buf_;
};

bool mentionsEncryption(const char *msg) {
    if (!msg) return false;
    std::string m = msg;
    return m.find("ncrypt") != std::string::npos || m.find("assword") != std::string::npos;
}

} // namespace

bool extract(const std::string &path, const std::string &destDir, int64_t size,
             const std::function<void(int64_t, int64_t)> &progress, const std::atomic<bool> *cancel,
             std::string &error) {
    Source src;
    src.f = std::fopen(path.c_str(), "rb");
    if (!src.f) {
        error = "Cannot open archive";
        return false;
    }
    // Reads already go through our own large buffer.
    std::setvbuf(src.f, nullptr, _IONBF, 0);

    struct archive *a = archive_read_new();
    if (!a) {
        std::fclose(src.f);
        error = "Out of memory";
        return false;
    }
    struct Freer {
        struct archive *a;
        ~Freer() { archive_read_free(a); }
    } freer{a};

    archive_read_support_format_7zip(a);
    archive_read_support_format_rar(a);
    archive_read_support_format_rar5(a);
    archive_read_set_callback_data(a, &src);
    archive_read_set_read_callback(a, readCb);
    archive_read_set_seek_callback(a, seekCb);
    archive_read_set_close_callback(a, closeCb);

    auto failArchive = [&](const char *what) {
        const char *msg = archive_error_string(a);
        if (mentionsEncryption(msg))
            error = "Encrypted archives are not supported";
        else
            error = std::string(what) + (msg ? std::string(": ") + msg : std::string());
        return false;
    };
    auto fail = [&](const std::string &msg) {
        error = msg;
        return false;
    };

    if (archive_read_open1(a) != ARCHIVE_OK) {
        // open1 calls the close callback on failure itself.
        return failArchive("Cannot read archive");
    }

    std::vector<char> outBuf(kWriteBufSize);

    std::vector<char> writeBuf(kWriteBufSize);
    int files = 0;

    while (true) {
        if (cancel && cancel->load()) return fail("Cancelled");

        struct archive_entry *entry = nullptr;
        int r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) break;
        if ((r != ARCHIVE_OK && r != ARCHIVE_WARN) || !entry) return failArchive("Corrupt archive");

        if (archive_entry_is_encrypted(entry)) return fail("Encrypted archives are not supported");

        const char *rawName = archive_entry_pathname_utf8(entry);
        if (!rawName) rawName = archive_entry_pathname(entry);
        if (!rawName) return fail("Archive has an entry with an unreadable name");

        // A bare root entry carries nothing to extract.
        if (std::strcmp(rawName, ".") == 0 || std::strcmp(rawName, "./") == 0) continue;

        if (archive_entry_hardlink(entry) || archive_entry_symlink(entry))
            return fail(std::string("Links are not supported in archives: ") + rawName);

        mode_t type = archive_entry_filetype(entry);
        bool isDir = type == AE_IFDIR;
        if (!isDir && type != AE_IFREG)
            return fail(std::string("Unsupported entry type in archive: ") + rawName);

        std::string rel = zip::sanitizeEntryName(rawName);
        if (rel.empty()) return fail(std::string("Unsafe path in archive: ") + rawName);
        while (!rel.empty() && rel.back() == '/') {
            isDir = true;
            rel.pop_back();
        }
        if (rel.empty()) continue; // "./" style root entry
        std::string outPath = destDir + "/" + rel;

        if (isDir) {
            if (!fs::mkdirs(outPath)) return fail("Cannot create folder " + rel);
            continue;
        }

        if (!fs::mkdirs(fs::parent(outPath))) return fail("Cannot create folder for " + rel);
        OutFile out(writeBuf);
        if (!out.open(outPath)) return fail("Cannot write " + rel);

        int64_t written = 0;
        while (true) {
            la_ssize_t n = archive_read_data(a, outBuf.data(), outBuf.size());
            if (n == 0) break;
            if (n < 0) return failArchive(("Corrupt data in " + rel).c_str());
            if (!out.write(outBuf.data(), (size_t) n)) return fail("SD card write failed (card full?)");
            written += n;
            if (progress) progress(src.pos < size ? src.pos : size, size);
            if (cancel && cancel->load()) return fail("Cancelled");
        }
        // Sparse entries may end in a hole that is never emitted; pad it.
        if (archive_entry_size_is_set(entry) && written < (int64_t) archive_entry_size(entry)) {
            std::fill(outBuf.begin(), outBuf.end(), 0);
            int64_t left = (int64_t) archive_entry_size(entry) - written;
            while (left > 0) {
                size_t k = (size_t) std::min<int64_t>(left, (int64_t) outBuf.size());
                if (!out.write(outBuf.data(), k)) return fail("SD card write failed (card full?)");
                left -= (int64_t) k;
            }
        }
        if (!out.close()) return fail("SD card write failed (card full?)");
        files++;
        if (progress) progress(src.pos < size ? src.pos : size, size);
    }

    if (files == 0) return fail("Archive contains no files");
    if (progress) progress(size, size);
    return true;
}

} // namespace archive_libarchive
