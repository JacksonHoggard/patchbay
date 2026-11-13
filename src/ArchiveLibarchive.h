#pragma once

// Internal to Archive.cpp. Kept in its own translation unit because
// libarchive's global `struct archive` cannot coexist with PatchBay's
// `namespace archive` in the same file.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace archive_libarchive {

// Extracts a 7-Zip / RAR / RAR5 archive (format already checked by the
// caller) with libarchive's readers.
bool extract(const std::string &path, const std::string &destDir, int64_t size,
             const std::function<void(int64_t, int64_t)> &progress, const std::atomic<bool> *cancel,
             std::string &error);

} // namespace archive_libarchive
