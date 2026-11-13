#include "Sha256.h"
#include "Fs.h"

#include <cstdio>
#include <mbedtls/sha256.h>
#include <vector>

namespace sha256 {

std::string ofFile(const std::string &path, const std::function<void(int64_t, int64_t)> &progress,
                   const std::atomic<bool> *cancel) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return "";

    int64_t total = fs::fileSize(path);
    int64_t done = 0;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts_ret(&ctx, 0);

    std::vector<unsigned char> buf(256 * 1024);
    bool ok = true;
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
        if (cancel && cancel->load()) {
            ok = false;
            break;
        }
        mbedtls_sha256_update_ret(&ctx, buf.data(), n);
        done += (int64_t) n;
        if (progress) progress(done, total);
    }
    if (std::ferror(f)) ok = false;
    std::fclose(f);

    unsigned char digest[32];
    mbedtls_sha256_finish_ret(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    if (!ok) return "";

    static const char *hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned char b : digest) {
        out += hex[b >> 4];
        out += hex[b & 0xF];
    }
    return out;
}

} // namespace sha256
