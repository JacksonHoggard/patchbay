#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace sha256 {

// Hashes a file and returns lowercase hex, or "" on read error/cancel.
std::string ofFile(const std::string &path, const std::function<void(int64_t, int64_t)> &progress,
                   const std::atomic<bool> *cancel);

} // namespace sha256
