#include "GameFiles.h"
#include "Fs.h"

#include <coreinit/filesystem_fsa.h>
#include <coreinit/ios.h>
#include <coreinit/mcp.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <map>
#include <mutex>
#include <vector>

namespace gamefiles {

namespace {

// Files bigger than this are not read; merging them would not fit in memory.
const uint32_t kMaxFileSize = 192u * 1024 * 1024;

std::mutex gMutex;
bool gTried = false;
bool gReady = false;
// An FSA client (raw IOSU filesystem handle), unlocked by Mocha so it can
// read other titles' files. This is what libmocha's Mocha_MountFS does.
FSAClientHandle gClient = -1;
// Title id (upper-case hex) -> install path, e.g. /vol/storage_mlc01/usr/title/00050000/101c9500
std::map<std::string, std::string> gTitlePaths;

// Mocha answers a custom ioctl on /dev/mcp with its API version. Plain
// firmware rejects it.
bool mochaApiAvailable() {
    IOSHandle fd = IOS_Open("/dev/mcp", IOS_OPEN_READ);
    if (fd < 0) return false;
    alignas(0x40) uint32_t buf[0x40 / 4] = {};
    buf[0] = 0xF8; // IPC_CUSTOM_GET_MOCHA_API_VERSION
    bool ok = IOS_Ioctl(fd, 100, buf, 4, buf, 8) == IOS_ERROR_OK && buf[0] == 0xCAFEBABE && buf[1] >= 1;
    IOS_Close(fd);
    return ok;
}

// Same request as libmocha's Mocha_UnlockFSClientEx: gives the client full
// filesystem permissions.
bool unlockClient(FSAClientHandle client) {
    alignas(0x40) int32_t dummy[0x40 / 4] = {};
    return IOS_Ioctl(client, 0x28, dummy, sizeof(dummy), dummy, sizeof(dummy)) == IOS_ERROR_OK;
}

void loadTitlePaths() {
    int32_t handle = MCP_Open();
    if (handle < 0) return;
    int32_t count = MCP_TitleCount(handle);
    if (count > 0) {
        std::vector<MCPTitleListType> list((size_t) count);
        uint32_t got = 0;
        if (MCP_TitleList(handle, &got, list.data(), (uint32_t) (list.size() * sizeof(MCPTitleListType))) >= 0) {
            for (uint32_t i = 0; i < got && i < list.size(); ++i) {
                char id[17];
                std::snprintf(id, sizeof(id), "%016llX", (unsigned long long) list[i].titleId);
                const char *raw = list[i].path;
                const void *nul = std::memchr(raw, 0, sizeof(list[i].path));
                std::string path(raw, nul ? (size_t) ((const char *) nul - raw) : sizeof(list[i].path));
                if (!path.empty()) gTitlePaths[id] = path;
            }
        }
    }
    MCP_Close(handle);
}

bool initLocked() {
    if (gTried) return gReady;
    gTried = true;
    if (!mochaApiAvailable()) {
        fs::log("gamefiles: Mocha not available; combos merge without original game files");
        return false;
    }
    FSAInit();
    gClient = FSAAddClient(nullptr);
    if (gClient < 0) {
        fs::log("gamefiles: FSAAddClient failed: %s", FSAGetStatusStr((FSError) gClient));
        gClient = -1;
        return false;
    }
    if (!unlockClient(gClient)) {
        fs::log("gamefiles: Mocha refused to unlock the FS client");
        FSADelClient(gClient);
        gClient = -1;
        return false;
    }
    loadTitlePaths();
    fs::log("gamefiles: ready, %u titles", (unsigned) gTitlePaths.size());
    gReady = true;
    return true;
}

bool isDirLocked(const std::string &path) {
    FSAStat st;
    return FSAGetStat(gClient, path.c_str(), &st) == FS_ERROR_OK && (st.flags & FS_STAT_DIRECTORY);
}

std::string lower(std::string s) {
    for (auto &c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}

// Mods do not always match the game's capitalization (SDCafiine matches
// case-insensitively), so resolve each component of `rel` under `root`.
bool resolveLocked(const std::string &root, const std::string &rel, std::string &out) {
    std::string cur = root;
    size_t pos = 0;
    while (pos <= rel.size()) {
        size_t slash = rel.find('/', pos);
        std::string part = rel.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        pos = slash == std::string::npos ? rel.size() + 1 : slash + 1;
        if (part.empty()) continue;

        FSAStat st;
        std::string direct = cur + "/" + part;
        if (FSAGetStat(gClient, direct.c_str(), &st) == FS_ERROR_OK) {
            cur = direct;
            continue;
        }
        FSADirectoryHandle dh;
        if (FSAOpenDir(gClient, cur.c_str(), &dh) != FS_ERROR_OK) return false;
        std::string want = lower(part), found;
        FSADirectoryEntry e;
        while (FSAReadDir(gClient, dh, &e) == FS_ERROR_OK) {
            if (lower(e.name) == want) {
                found = e.name;
                break;
            }
        }
        FSACloseDir(gClient, dh);
        if (found.empty()) return false;
        cur += "/" + found;
    }
    out = cur;
    return true;
}

bool readLocked(const std::string &path, std::string &out) {
    FSAFileHandle fh;
    if (FSAOpenFileEx(gClient, path.c_str(), "r", (FSMode) 0, FS_OPEN_FLAG_NONE, 0, &fh) != FS_ERROR_OK)
        return false;
    bool ok = false;
    FSAStat st;
    if (FSAGetStatFile(gClient, fh, &st) == FS_ERROR_OK && st.size <= kMaxFileSize) {
        // FSA reads need a 64-byte aligned buffer.
        const uint32_t kChunk = 512 * 1024;
        uint8_t *buf = (uint8_t *) memalign(0x40, kChunk);
        if (buf) {
            out.clear();
            out.reserve(st.size);
            ok = true;
            while (out.size() < st.size) {
                uint32_t want = std::min<uint32_t>(kChunk, st.size - (uint32_t) out.size());
                int32_t n = (int32_t) FSAReadFile(gClient, buf, 1, want, fh, 0);
                if (n <= 0) {
                    ok = false;
                    break;
                }
                out.append((const char *) buf, (size_t) n);
            }
            std::free(buf);
        }
    }
    FSACloseFile(gClient, fh);
    return ok;
}

// Finds the original of `rel` and reads it into `out` (if not null).
bool findLocked(const std::string &titleId, const std::string &rel, std::string *out) {
    if (!initLocked() || titleId.size() != 16) return false;
    size_t slash = rel.find('/');
    if (slash == std::string::npos) return false;
    std::string top = lower(rel.substr(0, slash));
    std::string rest = rel.substr(slash + 1);
    std::string low = titleId.substr(8);
    for (auto &c : low) c = (char) std::toupper((unsigned char) c);

    // content/ comes from the update if it has the file, else the base game.
    // aoc/ comes from the DLC title.
    std::vector<std::string> high;
    if (top == "content") high = {"0005000E", "00050000"};
    else if (top == "aoc") high = {"0005000C"};
    else return false;

    for (const auto &h : high) {
        auto it = gTitlePaths.find(h + low);
        if (it == gTitlePaths.end()) continue;
        std::string root = it->second + "/content";
        if (!isDirLocked(root)) {
            fs::log("gamefiles: cannot open %s", root.c_str());
            continue;
        }
        std::string path;
        if (resolveLocked(root, rest, path) && (!out || readLocked(path, *out))) return true;
    }
    return false;
}

} // namespace

bool available() {
    std::lock_guard<std::mutex> lock(gMutex);
    return initLocked();
}

bool read(const std::string &titleId, const std::string &rel, std::string &out) {
    std::lock_guard<std::mutex> lock(gMutex);
    return findLocked(titleId, rel, &out);
}

bool exists(const std::string &titleId, const std::string &rel) {
    std::lock_guard<std::mutex> lock(gMutex);
    return findLocked(titleId, rel, nullptr);
}

void shutdown() {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gClient >= 0) FSADelClient(gClient);
    gClient = -1;
    gReady = false;
}

} // namespace gamefiles
