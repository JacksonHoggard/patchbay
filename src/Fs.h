#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fs {

struct Entry {
    std::string name;
    bool isDir;
};

// Lists a directory (without "." and ".."). Empty if it does not exist.
std::vector<Entry> listDir(const std::string &path);

struct FileInfo {
    std::string rel; // path relative to the walked root, '/'-separated
    int64_t size;
};

// Recursively lists every regular file under `root`.
std::vector<FileInfo> walkFiles(const std::string &root);

// Copies one file. `onBytes` is called with the number of bytes just written.
bool copyFile(const std::string &from, const std::string &to, const std::function<void(int64_t)> &onBytes,
              const std::atomic<bool> *cancel);

bool exists(const std::string &path);
bool isDir(const std::string &path);
// Creates every missing directory in `path`.
bool mkdirs(const std::string &path);
// Removes a file or a whole directory tree. Returns true if the path is gone.
bool removeAll(const std::string &path);
bool rename(const std::string &from, const std::string &to);
int64_t fileSize(const std::string &path);

bool readFile(const std::string &path, std::string &out);
// Reads a file written by writeFileAtomic, falling back to the copies it
// keeps while swapping if a crash interrupted the swap.
bool readFileAtomic(const std::string &path, std::string &out);
// Writes to `<path>.tmp` then renames over `path`, so a crash never leaves a
// truncated file behind.
bool writeFileAtomic(const std::string &path, const std::string &data);

std::string parent(const std::string &path);

void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace fs
