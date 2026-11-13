#include "Http.h"
#include "Fs.h"

#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <cstdio>
#include <curl/curl.h>
#include <mutex>
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

// One DNS cache shared by every transfer. The console's resolver is slow
// and fails intermittently, especially when several threads resolve at once,
// so asking it as rarely as possible avoids most "Could not resolve host".
static CURLSH *gShare = nullptr;
static std::mutex gShareLocks[CURL_LOCK_DATA_LAST];

static void shareLock(CURL *, curl_lock_data data, curl_lock_access, void *) {
    gShareLocks[data].lock();
}

static void shareUnlock(CURL *, curl_lock_data data, void *) {
    gShareLocks[data].unlock();
}

bool globalInit() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return false;
    gShare = curl_share_init();
    if (gShare) {
        curl_share_setopt(gShare, CURLSHOPT_LOCKFUNC, shareLock);
        curl_share_setopt(gShare, CURLSHOPT_UNLOCKFUNC, shareUnlock);
        curl_share_setopt(gShare, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    }
    return true;
}

void globalCleanup() {
    if (gShare) curl_share_cleanup(gShare);
    gShare = nullptr;
    curl_global_cleanup();
}

// Errors worth retrying: DNS hiccups, dropped/reset connections and the
// Wii U's occasional "Socket operation on non-socket" mid-transfer.
static bool isTransient(CURLcode rc) {
    switch (rc) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_PARTIAL_FILE:
    case CURLE_GOT_NOTHING:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM:
        return true;
    default:
        return false;
    }
}

static bool isTransientStatus(long status) {
    return status == 408 || status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

// Sleeps in short slices so cancel/exit stay responsive. Returns false if
// the wait was interrupted.
static bool backoff(int attempt, const std::atomic<bool> *cancel) {
    int ms = 1000 << (attempt < 4 ? attempt : 4); // 1, 2, 4, 8, 16 s
    for (int waited = 0; waited < ms; waited += 100) {
        if (gAbort.load() || (cancel && cancel->load())) return false;
        OSSleepTicks(OSMillisecondsToTicks(100));
    }
    return true;
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
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    if (gShare) curl_easy_setopt(c, CURLOPT_SHARE, gShare);
    curl_easy_setopt(c, CURLOPT_DNS_CACHE_TIMEOUT, 3600L);
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

static Result getOnce(const std::string &url, std::string &body, size_t maxBytes, CURLcode &rc);

Result get(const std::string &url, std::string &body, size_t maxBytes) {
    Result r;
    body.clear();
    if (!isHttps(url)) {
        r.error = "Only HTTPS URLs are allowed";
        return r;
    }
    for (int attempt = 0;; ++attempt) {
        if (gAbort) {
            r.error = "Aborted";
            return r;
        }
        CURLcode rc = CURLE_OK;
        r = getOnce(url, body, maxBytes, rc);
        bool retry = !r.ok && (isTransient(rc) || isTransientStatus(r.status));
        if (!retry || attempt >= 3) return r;
        fs::log("GET %s: %s (retry %d)", url.c_str(), r.error.c_str(), attempt + 1);
        if (!backoff(attempt, nullptr)) return r;
    }
}

static Result getOnce(const std::string &url, std::string &body, size_t maxBytes, CURLcode &rc) {
    Result r;
    body.clear();
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

    rc = curl_easy_perform(c);
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

    // Failures in a row without new data. A long download over a flaky
    // connection keeps going as long as each attempt makes progress.
    const int kMaxStalls = 6;
    int stalls = 0;
    bool restarted = false;

    while (true) {
        if (gAbort.load()) {
            r.error = "Aborted";
            return r;
        }
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
        r.status = 0;
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
        bool rangeProblem = existing > 0 &&
                            (r.status == 416 || rc == CURLE_RANGE_ERROR || rc == CURLE_BAD_DOWNLOAD_RESUME);
        if (rangeProblem && !restarted) {
            restarted = true;
            std::remove(path.c_str());
            existing = 0;
            continue;
        }

        if (sink.writeError) {
            r.error = "SD card write failed (card full?)";
            return r;
        }
        if (rc == CURLE_OK && r.status >= 200 && r.status < 300) {
            r.ok = true;
            r.error.clear();
            return r;
        }
        r.error = rc != CURLE_OK ? (errbuf[0] ? errbuf : curl_easy_strerror(rc)) : "HTTP " + std::to_string(r.status);
        if (!(isTransient(rc) || (rc == CURLE_OK && isTransientStatus(r.status)))) return r;

        // Transient: keep what we have and resume.
        int64_t now = fs::fileSize(path);
        if (now < 0) now = 0;
        stalls = now > existing ? 0 : stalls + 1;
        fs::log("download %s: %s at %lld bytes (stall %d/%d)", url.c_str(), r.error.c_str(), (long long) now, stalls,
                kMaxStalls);
        existing = now;
        if (stalls >= kMaxStalls) return r;
        if (!backoff(stalls, cancel)) {
            r.error = (cancel && cancel->load()) ? "Cancelled" : "Aborted";
            return r;
        }
    }
}

} // namespace http
