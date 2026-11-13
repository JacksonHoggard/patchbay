#include "Http.h"
#include "Fs.h"

#include <cstdio>
#include <curl/curl.h>
#include <vector>

extern const char kCaCertPem[];
extern const std::size_t kCaCertPemSize;

namespace http {

static const char *kUserAgent = "PatchBay/" PATCHBAY_VERSION " (Wii U)";

static std::atomic<bool> gAbort{false};

void abortAll() {
    gAbort = true;
}

bool aborted() {
    return gAbort.load();
}

static int abortCheck(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return gAbort.load() ? 1 : 0;
}

bool globalInit() {
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

void globalCleanup() {
    curl_global_cleanup();
}

bool isHttps(const std::string &url) {
    return url.size() > 8 && (url.compare(0, 8, "https://") == 0 || url.compare(0, 8, "HTTPS://") == 0);
}

static CURL *makeHandle(const std::string &url) {
    CURL *c = curl_easy_init();
    if (!c) return nullptr;

    curl_blob ca;
    ca.data  = const_cast<char *>(kCaCertPem);
    ca.len   = kCaCertPemSize - 1; // exclude the terminating NUL
    ca.flags = CURL_BLOB_NOCOPY;

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    // Redirects must stay on HTTPS (GitHub releases redirect to a CDN).
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &ca);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    // Abort if the transfer stalls below 1 byte/s for 60 s.
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, abortCheck);
    return c;
}

struct MemSink {
    std::string *body;
    size_t max;
    bool overflow = false;
};

static size_t memWrite(char *ptr, size_t size, size_t nmemb, void *ud) {
    auto *s = static_cast<MemSink *>(ud);
    size_t n = size * nmemb;
    if (s->body->size() + n > s->max) {
        s->overflow = true;
        return 0;
    }
    s->body->append(ptr, n);
    return n;
}

Result get(const std::string &url, std::string &body, size_t maxBytes) {
    Result r;
    body.clear();
    if (gAbort) {
        r.error = "Aborted";
        return r;
    }
    if (!isHttps(url)) {
        r.error = "Only HTTPS URLs are allowed";
        return r;
    }
    CURL *c = makeHandle(url);
    if (!c) {
        r.error = "curl_easy_init failed";
        return r;
    }
    // Compressed transfer is fine for small JSON/image fetches. Downloads
    // must not use it: Range offsets would refer to encoded bytes.
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    MemSink sink{&body, maxBytes};
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, memWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);

    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);

    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_easy_cleanup(c);

    if (sink.overflow) {
        r.error = "Response too large";
    } else if (rc != CURLE_OK) {
        r.error = errbuf[0] ? errbuf : curl_easy_strerror(rc);
    } else if (r.status < 200 || r.status >= 300) {
        r.error = "HTTP " + std::to_string(r.status);
    } else {
        r.ok = true;
    }
    return r;
}

struct FileSink {
    FILE *f;
    bool writeError = false;
};

static size_t fileWrite(char *ptr, size_t size, size_t nmemb, void *ud) {
    auto *s = static_cast<FileSink *>(ud);
    size_t n = size * nmemb;
    if (std::fwrite(ptr, 1, n, s->f) != n) {
        s->writeError = true;
        return 0;
    }
    return n;
}

struct ProgressCtx {
    const Progress *cb;
    const std::atomic<bool> *cancel;
    int64_t offset;
};

static int xferInfo(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto *p = static_cast<ProgressCtx *>(ud);
    if ((p->cancel && p->cancel->load()) || gAbort.load()) return 1;
    if (p->cb && *p->cb) {
        int64_t total = dltotal > 0 ? (int64_t) dltotal + p->offset : -1;
        (*p->cb)((int64_t) dlnow + p->offset, total);
    }
    return 0;
}

Result download(const std::string &url, const std::string &path, const Progress &progress,
                const std::atomic<bool> *cancel) {
    Result r;
    if (!isHttps(url)) {
        r.error = "Only HTTPS downloads are allowed";
        return r;
    }

    fs::mkdirs(fs::parent(path));
    int64_t existing = fs::fileSize(path);
    if (existing < 0) existing = 0;

    for (int attempt = 0; attempt < 2; ++attempt) {
        FILE *f = std::fopen(path.c_str(), existing > 0 ? "ab" : "wb");
        if (!f) {
            r.error = "Cannot open " + path + " for writing";
            return r;
        }
        // Large buffer: SD writes are slow with small chunks. Must outlive
        // the FILE, so it is declared before use and freed after fclose.
        std::vector<char> ioBuf(256 * 1024);
        std::setvbuf(f, ioBuf.data(), _IOFBF, ioBuf.size());

        CURL *c = makeHandle(url);
        if (!c) {
            std::fclose(f);
            r.error = "curl_easy_init failed";
            return r;
        }

        FileSink sink{f};
        ProgressCtx pctx{&progress, cancel, existing};
        char errbuf[CURL_ERROR_SIZE] = {0};
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, fileWrite);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xferInfo);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &pctx);
        curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
        curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 128L * 1024L);
        if (existing > 0) curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t) existing);

        CURLcode rc = curl_easy_perform(c);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
        curl_easy_cleanup(c);
        std::fclose(f);

        if (cancel && cancel->load()) {
            r.error = "Cancelled";
            return r;
        }
        // 416: the partial file is already complete (or bigger than the
        // remote). RANGE_ERROR: the server ignored the Range header.
        // Either way restart from scratch once.
        bool restart = existing > 0 &&
                       (r.status == 416 || rc == CURLE_RANGE_ERROR || rc == CURLE_BAD_DOWNLOAD_RESUME);
        if (restart && attempt == 0) {
            std::remove(path.c_str());
            existing = 0;
            continue;
        }

        if (sink.writeError) {
            r.error = "SD card write failed (card full?)";
        } else if (rc != CURLE_OK) {
            r.error = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        } else if (r.status < 200 || r.status >= 300) {
            r.error = "HTTP " + std::to_string(r.status);
        } else {
            r.ok = true;
        }
        return r;
    }
    r.error = "Download failed";
    return r;
}

} // namespace http
