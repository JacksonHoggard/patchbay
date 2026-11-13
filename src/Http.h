#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace http {

bool globalInit();
void globalCleanup();

// Makes every running and future transfer fail fast. Used on exit so worker
// threads can be joined without waiting for timeouts.
void abortAll();
bool aborted();

bool isHttps(const std::string &url);

struct Result {
    bool ok = false;
    long status = 0;
    std::string error;
};

// Fetches a (small) resource into memory. `maxBytes` guards against a
// misconfigured URL pointing at a huge file.
Result get(const std::string &url, std::string &body, size_t maxBytes = 32 * 1024 * 1024);

// (downloaded, total) in bytes. total is -1 when unknown.
using Progress = std::function<void(int64_t, int64_t)>;

// Downloads `url` into `path`. If `path` already holds a partial download it
// is resumed with a Range request. Setting `*cancel` aborts the transfer and
// keeps the partial file for a later resume.
Result download(const std::string &url, const std::string &path, const Progress &progress,
                const std::atomic<bool> *cancel);

} // namespace http
