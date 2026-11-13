#include "Installer.h"
#include "Fs.h"
#include "Paths.h"
#include "Zip.h"

#include <cctype>
#include <ctime>
#include <nlohmann/json.hpp>

using nlohmann::json;

static const char *kOldPrefix = ".patchbay-old-";
static const char *kStagingPrefix = ".patchbay-staging-";

void Installer::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    mods_.clear();
    std::string text;
    if (!fs::readFileAtomic(paths::kInstalledDb, text)) return;
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("mods") || !j["mods"].is_object()) {
        fs::log("installed.json is corrupt; starting fresh");
        fs::rename(paths::kInstalledDb, paths::kInstalledDb + ".bad");
        return;
    }
    for (auto it = j["mods"].begin(); it != j["mods"].end(); ++it) {
        const json &v = it.value();
        if (!v.is_object()) continue;
        InstalledMod m;
        m.id = it.key();
        m.titleId = v.value("titleId", "");
        m.gameName = v.value("game", "");
        m.name = v.value("name", m.id);
        m.version = v.value("version", "");
        m.repoUrl = v.value("repo", "");
        m.sha256 = v.value("sha256", "");
        m.enabled = v.value("enabled", true);
        m.installedAt = v.value("installedAt", (int64_t) 0);
        if (!m.titleId.empty() && repo::isValidModId(m.id)) mods_[m.id] = m;
    }
}

void Installer::saveLocked() {
    json j;
    j["version"] = 1;
    j["mods"] = json::object();
    for (const auto &kv : mods_) {
        const auto &m = kv.second;
        j["mods"][m.id] = {
            {"titleId", m.titleId},     {"game", m.gameName},  {"name", m.name},
            {"version", m.version},     {"repo", m.repoUrl},   {"sha256", m.sha256},
            {"enabled", m.enabled},     {"installedAt", m.installedAt},
        };
    }
    if (!fs::writeFileAtomic(paths::kInstalledDb, j.dump(2) + "\n")) fs::log("failed to write installed.json");
}

std::string Installer::currentDirLocked(const InstalledMod &m) const {
    return m.enabled ? paths::modDir(m.titleId, m.id) : paths::disabledModDir(m.titleId, m.id);
}

void Installer::recover() {
    for (const auto &tidDir : fs::listDir(paths::kSdCafiine)) {
        if (!tidDir.isDir) continue;
        std::string base = paths::kSdCafiine + "/" + tidDir.name;
        for (const auto &e : fs::listDir(base)) {
            if (e.name.rfind(kStagingPrefix, 0) == 0) {
                fs::log("recover: removing %s/%s", base.c_str(), e.name.c_str());
                fs::removeAll(base + "/" + e.name);
            } else if (e.name.rfind(kOldPrefix, 0) == 0) {
                std::string id = e.name.substr(std::string(kOldPrefix).size());
                std::string final_ = base + "/" + id;
                if (!fs::exists(final_)) {
                    fs::log("recover: restoring previous %s", final_.c_str());
                    fs::rename(base + "/" + e.name, final_);
                } else {
                    fs::removeAll(base + "/" + e.name);
                }
            }
        }
    }
    for (const auto &e : fs::listDir(paths::kTmpDir))
        if (e.isDir && e.name.rfind("trash-", 0) == 0) fs::removeAll(paths::kTmpDir + "/" + e.name);
}

void Installer::adoptExisting(const Catalog &catalog) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool changed = false;

    // Forget records whose folder was deleted by hand.
    for (auto it = mods_.begin(); it != mods_.end();) {
        if (!fs::isDir(currentDirLocked(it->second))) {
            fs::log("installed mod %s is gone from SD; forgetting it", it->first.c_str());
            it = mods_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }

    for (const auto &g : catalog.games) {
        for (const auto &m : g.mods) {
            if (mods_.count(m.id)) continue;
            for (const auto &tid : g.titleIds) {
                bool on = fs::isDir(paths::modDir(tid, m.id));
                bool off = !on && fs::isDir(paths::disabledModDir(tid, m.id));
                if (!on && !off) continue;
                InstalledMod im;
                im.id = m.id;
                im.titleId = tid;
                im.gameName = g.name;
                im.name = m.name;
                im.enabled = on;
                mods_[m.id] = im;
                changed = true;
                break;
            }
        }
    }
    if (changed) saveLocked();
}

std::vector<InstalledMod> Installer::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<InstalledMod> out;
    for (const auto &kv : mods_) out.push_back(kv.second);
    return out;
}

bool Installer::get(const std::string &modId, InstalledMod &out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = mods_.find(modId);
    if (it == mods_.end()) return false;
    out = it->second;
    return true;
}

bool Installer::isInstalled(const std::string &modId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mods_.count(modId) != 0;
}

std::vector<InstalledMod> Installer::enabledForTitle(const std::string &titleId, const std::string &exceptId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<InstalledMod> out;
    for (const auto &kv : mods_)
        if (kv.second.enabled && kv.second.titleId == titleId && kv.first != exceptId) out.push_back(kv.second);
    return out;
}

// Many mod ZIPs wrap everything in one extra folder ("MyMod/content/...").
// Flatten that so the result is "<modId>/content/...".
static void unwrapSingleFolder(const std::string &dir) {
    auto entries = fs::listDir(dir);
    if (entries.size() != 1 || !entries[0].isDir) return;
    std::string name = entries[0].name;
    std::string lower;
    for (char c : name) lower += (char) std::tolower((unsigned char) c);
    if (lower == "content" || lower == "aoc" || lower == "code" || lower == "meta") return;

    std::string tmp = dir + "/.patchbay-wrap";
    if (!fs::rename(dir + "/" + name, tmp)) return;
    for (const auto &e : fs::listDir(tmp)) fs::rename(tmp + "/" + e.name, dir + "/" + e.name);
    fs::removeAll(tmp);
}

static bool hasSdCafiineLayout(const std::string &dir) {
    for (const auto &e : fs::listDir(dir)) {
        if (!e.isDir) continue;
        std::string lower;
        for (char c : e.name) lower += (char) std::tolower((unsigned char) c);
        if (lower == "content" || lower == "aoc") return true;
    }
    return false;
}

bool Installer::install(const Mod &mod, const std::string &gameName, const std::string &titleId,
                        const std::string &zipPath, int64_t zipSize,
                        const std::function<void(int64_t, int64_t)> &progress, const std::atomic<bool> *cancel,
                        std::string &error) {
    std::string staging = paths::stagingDir(titleId, mod.id);
    std::string final_ = paths::modDir(titleId, mod.id);
    std::string old = paths::kSdCafiine + "/" + titleId + "/" + kOldPrefix + mod.id;

    fs::removeAll(staging);
    if (!fs::mkdirs(staging)) {
        error = "Cannot create " + staging;
        return false;
    }

    if (!zip::extract(zipPath, staging, zipSize, progress, cancel, error)) {
        fs::removeAll(staging);
        return false;
    }
    unwrapSingleFolder(staging);
    if (!hasSdCafiineLayout(staging)) {
        fs::removeAll(staging);
        error = "Archive has no content/ or aoc/ folder (not an SDCafiine mod)";
        return false;
    }

    fs::removeAll(old);

    std::unique_lock<std::mutex> lock(mutex_);

    // A previous install somewhere else (other region or disabled folder)
    // is only deleted once the new copy is in place.
    std::string prevDir;
    auto prev = mods_.find(mod.id);
    if (prev != mods_.end()) {
        prevDir = currentDirLocked(prev->second);
        if (prevDir == final_) prevDir.clear();
    }

    bool hadOld = fs::exists(final_);
    if (hadOld && !fs::rename(final_, old)) {
        fs::removeAll(staging);
        error = "Cannot replace existing install (folder in use?)";
        return false;
    }
    if (!fs::rename(staging, final_)) {
        if (hadOld) fs::rename(old, final_);
        fs::removeAll(staging);
        error = "Cannot move mod into place";
        return false;
    }
    std::string disabled = paths::disabledModDir(titleId, mod.id);

    InstalledMod im;
    im.id = mod.id;
    im.titleId = titleId;
    im.gameName = gameName;
    im.name = mod.name;
    im.version = mod.version;
    im.sha256 = mod.sha256;
    im.repoUrl = mod.repoUrl;
    im.enabled = true;
    im.installedAt = (int64_t) std::time(nullptr);
    mods_[mod.id] = im;
    saveLocked();
    lock.unlock();

    // Slow deletes happen without the lock so the UI keeps drawing.
    if (hadOld) fs::removeAll(old);
    if (!prevDir.empty()) fs::removeAll(prevDir);
    if (disabled != prevDir) fs::removeAll(disabled);
    fs::log("installed %s %s to %s", mod.id.c_str(), mod.version.c_str(), final_.c_str());
    return true;
}

bool Installer::uninstall(const std::string &modId, std::string &error) {
    std::string trash;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mods_.find(modId);
        if (it == mods_.end()) {
            error = "Not installed";
            return false;
        }
        // Move out of SDCafiine's view first (fast), delete after unlocking.
        std::string dir = currentDirLocked(it->second);
        trash = paths::kTmpDir + "/trash-" + modId;
        fs::mkdirs(paths::kTmpDir);
        fs::removeAll(trash);
        if (fs::exists(dir) && !fs::rename(dir, trash)) {
            error = "Could not remove " + dir;
            return false;
        }
        mods_.erase(it);
        saveLocked();
    }
    fs::removeAll(trash);
    fs::log("uninstalled %s", modId.c_str());
    return true;
}

bool Installer::setEnabled(const std::string &modId, bool enabled, std::string &error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = mods_.find(modId);
    if (it == mods_.end()) {
        error = "Not installed";
        return false;
    }
    InstalledMod &m = it->second;
    if (m.enabled == enabled) return true;
    std::string from = currentDirLocked(m);
    std::string to = enabled ? paths::modDir(m.titleId, m.id) : paths::disabledModDir(m.titleId, m.id);
    fs::mkdirs(fs::parent(to));
    fs::removeAll(to);
    if (!fs::rename(from, to)) {
        error = "Could not move " + from;
        return false;
    }
    m.enabled = enabled;
    saveLocked();
    return true;
}

int Installer::compareVersions(const std::string &a, const std::string &b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        while (i < a.size() && !std::isdigit((unsigned char) a[i])) i++;
        while (j < b.size() && !std::isdigit((unsigned char) b[j])) j++;
        unsigned long long x = 0, y = 0;
        bool hx = false, hy = false;
        while (i < a.size() && std::isdigit((unsigned char) a[i])) {
            x = x * 10 + (a[i++] - '0');
            hx = true;
        }
        while (j < b.size() && std::isdigit((unsigned char) b[j])) {
            y = y * 10 + (b[j++] - '0');
            hy = true;
        }
        if (!hx && !hy) break;
        if (x != y) return x < y ? -1 : 1;
    }
    if (a == b) return 0;
    return a < b ? -1 : 1;
}
