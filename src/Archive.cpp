#include "Archive.h"
#include "ArchiveLibarchive.h"
#include "Zip.h"

#include <cstdio>
#include <cstring>

namespace archive {

namespace {

enum class Format { Unknown, Zip, Rar, SevenZip };

Format detect(const std::string &path) {
    unsigned char m[8] = {};
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return Format::Unknown;
    size_t n = std::fread(m, 1, sizeof(m), f);
    std::fclose(f);

    static const unsigned char kZip[] = {'P', 'K', 0x03, 0x04};
    static const unsigned char kRar4[] = {'R', 'a', 'r', '!', 0x1A, 0x07, 0x00};
    static const unsigned char kRar5[] = {'R', 'a', 'r', '!', 0x1A, 0x07, 0x01, 0x00};
    static const unsigned char k7z[] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};

    if (n >= sizeof(kZip) && std::memcmp(m, kZip, sizeof(kZip)) == 0) return Format::Zip;
    if (n >= sizeof(kRar4) && std::memcmp(m, kRar4, sizeof(kRar4)) == 0) return Format::Rar;
    if (n >= sizeof(kRar5) && std::memcmp(m, kRar5, sizeof(kRar5)) == 0) return Format::Rar;
    if (n >= sizeof(k7z) && std::memcmp(m, k7z, sizeof(k7z)) == 0) return Format::SevenZip;
    return Format::Unknown;
}

} // namespace

bool extract(const std::string &path, const std::string &destDir, int64_t size,
             const std::function<void(int64_t, int64_t)> &progress, const std::atomic<bool> *cancel,
             std::string &error) {
    switch (detect(path)) {
    case Format::Zip:
        return zip::extract(path, destDir, size, progress, cancel, error);
    case Format::Rar:
    case Format::SevenZip:
        // libarchive seeks with a 32-bit off_t on the Wii U.
        if (size > INT32_MAX) {
            error = "RAR/7z archives over 2 GB are not supported (ask for a ZIP)";
            return false;
        }
        return archive_libarchive::extract(path, destDir, size, progress, cancel, error);
    case Format::Unknown:
        break;
    }
    error = "Unsupported archive format";
    return false;
}

} // namespace archive
