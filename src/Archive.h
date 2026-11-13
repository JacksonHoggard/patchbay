#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace archive {

// Extracts every file of the archive at `path` into `destDir`, keeping its
// folder structure. The format is detected from the first bytes, not from
// the file name:
//   - ZIP goes through zip::extract (streaming, handles > 2 GiB).
//   - 7-Zip, RAR 4 and RAR 5 go through libarchive. These need seeking and
//     the Wii U's off_t is 32-bit, so they are limited to 2 GiB.
// Every entry name goes through zip::sanitizeEntryName (zip-slip), and
// links, devices and encrypted entries are rejected. `progress` gets
// (archive bytes consumed, `size`).
bool extract(const std::string &path, const std::string &destDir, int64_t size,
             const std::function<void(int64_t, int64_t)> &progress, const std::atomic<bool> *cancel,
             std::string &error);

} // namespace archive
