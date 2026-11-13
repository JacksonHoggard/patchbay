#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace zip {

// Extracts every entry of `zipPath` into `destDir`, keeping the archive's
// folder structure.
//
// The archive is read strictly front-to-back through its local headers. The
// Wii U's newlib has a 32-bit off_t, so seeking past 2 GiB (needed to reach
// the central directory of a big modpack) is not reliable; streaming avoids
// seeking entirely.
//
// Supports stored and deflated entries, data descriptors on deflated
// entries, and ZIP64 size fields. Entries with absolute paths or ".." are
// rejected (zip-slip). CRC-32 of every file is checked.
bool extract(const std::string &zipPath, const std::string &destDir, int64_t zipSize,
             const std::function<void(int64_t, int64_t)> &progress, const std::atomic<bool> *cancel,
             std::string &error);

// Returns a safe relative path for an archive entry name, or "" if the name
// must be rejected.
std::string sanitizeEntryName(const std::string &name);

} // namespace zip
