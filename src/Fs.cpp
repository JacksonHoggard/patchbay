#include "Fs.h"
#include "Paths.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs {

bool exists(const std::string &path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool isDir(const std::string &path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string parent(const std::string &path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) return "";
    return path.substr(0, pos);
}

bool mkdirs(const std::string &path) {
    if (path.empty() || isDir(path)) return true;
    // Never try to create the device root ("fs:" / "fs:/vol").
    std::string p = parent(path);
    if (!p.empty() && p.find('/') != std::string::npos && !isDir(p)) {
        if (!mkdirs(p)) return false;
    }
    if (::mkdir(path.c_str(), 0777) == 0) return true;
    return errno == EEXIST && isDir(path);
}

std::vector<Entry> listDir(const std::string &path) {
    std::vector<Entry> out;
    DIR *dir = ::opendir(path.c_str());
    if (!dir) return out;
    struct dirent *ent;
    while ((ent = ::readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        out.push_back({name, isDir(path + "/" + name)});
    }
    ::closedir(dir);
    return out;
}

static void walkInto(const std::string &root, const std::string &rel, std::vector<FileInfo> &out,
                     const std::function<void()> &onFile, const std::atomic<bool> *cancel) {
    std::string dir = rel.empty() ? root : root + "/" + rel;
    DIR *d = ::opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> subdirs;
    struct dirent *ent;
    while ((ent = ::readdir(d)) != nullptr) {
        if (cancel && cancel->load()) break;
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string r = rel.empty() ? name : rel + "/" + name;
#ifdef _DIRENT_HAVE_D_STAT
        // devkitPro's readdir fills in the entry's stat, which saves two
        // stat() calls per file (slow on the SD card).
        bool dirEntry = S_ISDIR(ent->d_stat.st_mode);
        int64_t size = (int64_t) ent->d_stat.st_size;
#else
        bool dirEntry = isDir(dir + "/" + name);
        int64_t size = dirEntry ? 0 : fileSize(dir + "/" + name);
#endif
        if (dirEntry) {
            subdirs.push_back(r);
        } else {
            out.push_back({r, size});
            if (onFile) onFile();
        }
    }
    ::closedir(d);
    // Recurse after closing, so only one directory handle is open at a time.
    for (const auto &s : subdirs) {
        if (cancel && cancel->load()) return;
        walkInto(root, s, out, onFile, cancel);
    }
}

std::vector<FileInfo> walkFiles(const std::string &root, const std::function<void()> &onFile,
                                const std::atomic<bool> *cancel) {
    std::vector<FileInfo> out;
    walkInto(root, "", out, onFile, cancel);
    return out;
}

bool copyFile(const std::string &from, const std::string &to, const std::function<void(int64_t)> &onBytes,
              const std::atomic<bool> *cancel) {
    FILE *in = ::fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE *out = ::fopen(to.c_str(), "wb");
    if (!out) {
        ::fclose(in);
        return false;
    }
    std::vector<char> buf(512 * 1024);
    bool ok = true;
    size_t n;
    while ((n = ::fread(buf.data(), 1, buf.size(), in)) > 0) {
        if ((cancel && cancel->load()) || ::fwrite(buf.data(), 1, n, out) != n) {
            ok = false;
            break;
        }
        if (onBytes) onBytes((int64_t) n);
    }
    if (::ferror(in)) ok = false;
    ::fclose(in);
    ok = (::fclose(out) == 0) && ok;
    if (!ok) ::remove(to.c_str());
    return ok;
}

bool removeAll(const std::string &path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return true;
    if (!S_ISDIR(st.st_mode)) return ::remove(path.c_str()) == 0;

    DIR *dir = ::opendir(path.c_str());
    if (dir) {
        struct dirent *ent;
        while ((ent = ::readdir(dir)) != nullptr) {
            std::string name = ent->d_name;
            if (name == "." || name == "..") continue;
            removeAll(path + "/" + name);
        }
        ::closedir(dir);
    }
    return ::rmdir(path.c_str()) == 0 || !exists(path);
}

bool rename(const std::string &from, const std::string &to) {
    return ::rename(from.c_str(), to.c_str()) == 0;
}

int64_t fileSize(const std::string &path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return -1;
    // off_t is 32-bit signed on the Wii U. FAT32 files can be up to 4 GiB,
    // so reinterpret the size as unsigned.
    return (int64_t) (uint32_t) st.st_size;
}

bool readFile(const std::string &path, std::string &out) {
    FILE *f = ::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[16 * 1024];
    size_t n;
    while ((n = ::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    bool ok = !::ferror(f);
    ::fclose(f);
    return ok;
}

bool writeFileAtomic(const std::string &path, const std::string &data) {
    mkdirs(parent(path));
    std::string tmp = path + ".tmp";
    FILE *f = ::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = ::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (::fclose(f) == 0) && ok;
    if (!ok) {
        ::remove(tmp.c_str());
        return false;
    }
    // FAT does not allow renaming over an existing file. Keep the old copy
    // as .bak until the new one is in place.
    std::string bak = path + ".bak";
    ::remove(bak.c_str());
    bool hadOld = ::rename(path.c_str(), bak.c_str()) == 0;
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        if (hadOld) ::rename(bak.c_str(), path.c_str());
        return false;
    }
    ::remove(bak.c_str());
    return true;
}

bool readFileAtomic(const std::string &path, std::string &out) {
    // .tmp is only known complete once .bak exists (the swap had started).
    return readFile(path, out) || (exists(path + ".bak") && readFile(path + ".tmp", out)) ||
           readFile(path + ".bak", out);
}

static std::mutex gLogMutex;

void log(const char *fmt, ...) {
    std::lock_guard<std::mutex> lock(gLogMutex);
    FILE *f = ::fopen(paths::kLogFile.c_str(), "ab");
    if (!f) return;
    std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(f, "[%s] ", stamp);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    ::fclose(f);
}

} // namespace fs
