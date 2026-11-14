#include "Installer.h"
#include "Archive.h"
#include "Fs.h"
#include "GameFiles.h"
#include "Merge.h"
#include "Paths.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <new>
#include <set>
#include <nlohmann/json.hpp>

using nlohmann::json;

static const char *kOldPrefix = ".patchbay-old-";
static const char *kStagingPrefix = ".patchbay-staging-";
// Shared files whose copies add up to more than this are not merged (they
// would not fit in memory); the highest-priority copy is used.
static const int64_t kMaxMergeBytes = 96ll * 1024 * 1024;

static std::string lower(const std::string &s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return out;
}

void Installer::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    mods_.clear();
    combos_.clear();
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

    if (j.contains("combos") && j["combos"].is_object()) {
        for (auto it = j["combos"].begin(); it != j["combos"].end(); ++it) {
            const json &v = it.value();
            if (!v.is_object()) continue;
            Combo c;
            c.id = it.key();
            c.titleId = v.value("titleId", "");
            c.gameName = v.value("game", "");
            c.enabled = v.value("enabled", true);
            c.built = v.value("built", false);
            c.builtAt = v.value("builtAt", (int64_t) 0);
            c.sizeBytes = v.value("sizeBytes", (int64_t) 0);
            c.merged = v.value("merged", 0);
            c.conflicts = v.value("conflicts", 0);
            c.usedBase = v.value("usedBase", false);
            if (v.contains("members") && v["members"].is_array())
                for (auto &m : v["members"])
                    if (m.is_string()) c.members.push_back(m.get<std::string>());
            if (v.contains("conflictSamples") && v["conflictSamples"].is_array())
                for (auto &m : v["conflictSamples"])
                    if (m.is_string()) c.conflictSamples.push_back(m.get<std::string>());
            if (v.contains("builtVersions") && v["builtVersions"].is_object())
                for (auto b = v["builtVersions"].begin(); b != v["builtVersions"].end(); ++b)
                    if (b.value().is_string()) c.builtVersions[b.key()] = b.value().get<std::string>();
            if (v.contains("restoreEnabled") && v["restoreEnabled"].is_object())
                for (auto b = v["restoreEnabled"].begin(); b != v["restoreEnabled"].end(); ++b)
                    if (b.value().is_boolean()) c.restoreEnabled[b.key()] = b.value().get<bool>();
            // Only ids PatchBay generates: "PatchBayCombo-<digits>".
            static const std::string prefix = "PatchBayCombo-";
            bool validId = c.id.size() > prefix.size() && c.id.compare(0, prefix.size(), prefix) == 0 &&
                           c.id.find_first_not_of("0123456789", prefix.size()) == std::string::npos;
            if (validId && c.titleId.size() == 16) combos_[c.id] = c;
            else fs::log("installed.json: ignoring combo with bad id '%s'", c.id.c_str());
        }
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
    j["combos"] = json::object();
    for (const auto &kv : combos_) {
        const Combo &c = kv.second;
        j["combos"][c.id] = {
            {"titleId", c.titleId},
            {"game", c.gameName},
            {"members", c.members},
            {"enabled", c.enabled},
            {"built", c.built},
            {"builtAt", c.builtAt},
            {"sizeBytes", c.sizeBytes},
            {"merged", c.merged},
            {"conflicts", c.conflicts},
            {"usedBase", c.usedBase},
            {"conflictSamples", c.conflictSamples},
            {"builtVersions", c.builtVersions},
            {"restoreEnabled", c.restoreEnabled},
        };
    }
    if (!fs::writeFileAtomic(paths::kInstalledDb, j.dump(2) + "\n")) fs::log("failed to write installed.json");
}

std::string Installer::currentDirLocked(const InstalledMod &m) const {
    return m.enabled ? paths::modDir(m.titleId, m.id) : paths::disabledModDir(m.titleId, m.id);
}

std::string Installer::comboDirLocked(const Combo &c) const {
    return c.enabled ? paths::modDir(c.titleId, c.id) : paths::disabledModDir(c.titleId, c.id);
}

void Installer::recover() {
    // Swaps happen in both the live SDCafiine tree and the disabled tree.
    std::vector<std::string> tidDirs;
    for (const std::string &root : {paths::kSdCafiine, paths::kDisabledRoot})
        for (const auto &t : fs::listDir(root))
            if (t.isDir) tidDirs.push_back(root + "/" + t.name);
    for (const auto &base : tidDirs) {
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

    for (auto it = combos_.begin(); it != combos_.end();) {
        if (it->second.built && !fs::isDir(comboDirLocked(it->second))) {
            fs::log("combo %s is gone from SD; forgetting it", it->first.c_str());
            Combo gone = it->second;
            it = combos_.erase(it);
            for (const auto &kv : gone.restoreEnabled) restoreMemberLocked(kv.first, kv.second, gone.id);
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
    for (const auto &kv : combos_) {
        const Combo &c = kv.second;
        if (!c.built || !c.enabled || c.titleId != titleId || c.id == exceptId) continue;
        InstalledMod im;
        im.id = c.id;
        im.titleId = c.titleId;
        im.gameName = c.gameName;
        im.name = "Combo " + c.id;
        out.push_back(im);
    }
    return out;
}

static std::string lowerName(const std::string &s) {
    std::string out;
    for (char c : s) out += (char) std::tolower((unsigned char) c);
    return out;
}

static bool containsModRoot(const std::string &dir) {
    for (const auto &e : fs::listDir(dir)) {
        if (!e.isDir) continue;
        std::string lower = lowerName(e.name);
        if (lower == "content" || lower == "aoc") return true;
    }
    return false;
}

// Mods are packaged in all sorts of ways ("MyMod/content/...",
// "MyMod v2/Wii U/content/...", plus readmes and screenshots). Finds the
// shallowest folder that directly holds content/ or aoc/ and makes it the
// mod root, dropping everything outside it. Fails with a helpful message for
// variant packs and for archives that are not SDCafiine mods.
static bool normalizeLayout(const std::string &staging, std::string &error) {
    constexpr int kMaxDepth = 4;
    fs::removeAll(staging + "/__MACOSX");

    std::vector<std::string> level{""}; // paths relative to staging
    std::vector<std::string> found;
    for (int depth = 0; depth <= kMaxDepth && found.empty() && !level.empty(); depth++) {
        std::vector<std::string> next;
        for (const auto &rel : level) {
            std::string abs = rel.empty() ? staging : staging + "/" + rel;
            if (containsModRoot(abs)) {
                found.push_back(rel);
                continue;
            }
            for (const auto &e : fs::listDir(abs))
                if (e.isDir) next.push_back(rel.empty() ? e.name : rel + "/" + e.name);
        }
        level.swap(next);
    }

    if (found.size() > 1) {
        // Name each variant by the first folder where the paths differ
        // ("Pack/Option A/Wii U" and "Pack/Option B/Wii U" -> "Option A").
        auto split = [](const std::string &p) {
            std::vector<std::string> parts;
            size_t i = 0;
            while (i <= p.size()) {
                size_t j = p.find('/', i);
                if (j == std::string::npos) j = p.size();
                parts.push_back(p.substr(i, j - i));
                i = j + 1;
            }
            return parts;
        };
        std::vector<std::vector<std::string>> paths;
        for (const auto &f : found) paths.push_back(split(f));
        size_t common = 0;
        while (true) {
            bool same = true;
            for (const auto &p : paths)
                if (common + 1 >= p.size() || p[common] != paths[0][common]) same = false;
            if (!same) break;
            common++;
        }
        std::string names;
        for (size_t i = 0; i < paths.size() && i < 4; i++) {
            if (i) names += ", ";
            names += paths[i][std::min(common, paths[i].size() - 1)];
        }
        if (found.size() > 4) names += ", ...";
        error = "Archive contains several variants (" + names +
                "); install one manually or ask the repo maintainer to package it";
        return false;
    }

    if (found.empty()) {
        // Look a little deeper to explain what the archive is instead.
        bool switchMod = false, graphicPack = false;
        std::vector<std::string> dirs{staging};
        for (int depth = 0; depth <= kMaxDepth + 2 && !dirs.empty(); depth++) {
            std::vector<std::string> next;
            for (const auto &d : dirs) {
                for (const auto &e : fs::listDir(d)) {
                    std::string lower = lowerName(e.name);
                    if (e.isDir && (lower == "romfs" || lower == "exefs")) switchMod = true;
                    if (!e.isDir && lower == "rules.txt") graphicPack = true;
                    if (e.isDir) next.push_back(d + "/" + e.name);
                }
            }
            dirs.swap(next);
        }
        if (switchMod)
            error = "This is a Nintendo Switch mod, not a Wii U mod";
        else if (graphicPack)
            error = "This is a Cemu graphic pack without game files";
        else
            error = "Archive has no content/ or aoc/ folder (not an SDCafiine mod)";
        return false;
    }

    const std::string &root = found[0];
    if (root.empty()) return true;

    // Move the mod root out of the way first so a child named like a
    // top-level entry ("MyMod/MyMod/...") cannot collide.
    std::string tmp = staging + "/.patchbay-wrap";
    fs::removeAll(tmp);
    if (!fs::rename(staging + "/" + root, tmp)) {
        error = "Cannot reorganize extracted files";
        return false;
    }
    // SDCafiine also uses code/ and meta/. If the archive keeps them in an
    // ancestor folder ("Mod/meta" beside "Mod/Wii U/content"), carry them
    // into the mod root instead of deleting them.
    {
        std::vector<std::string> ancestors{staging};
        size_t pos = 0;
        while ((pos = root.find('/', pos)) != std::string::npos) {
            ancestors.push_back(staging + "/" + root.substr(0, pos));
            pos++;
        }
        for (const auto &anc : ancestors) {
            for (const auto &e : fs::listDir(anc)) {
                std::string ln = lowerName(e.name);
                if (!e.isDir || (ln != "code" && ln != "meta")) continue;
                bool present = false;
                for (const auto &t : fs::listDir(tmp))
                    if (lowerName(t.name) == ln) present = true;
                if (!present) fs::rename(anc + "/" + e.name, tmp + "/" + e.name);
            }
        }
    }
    for (const auto &e : fs::listDir(staging))
        if (e.name != ".patchbay-wrap") fs::removeAll(staging + "/" + e.name);
    for (const auto &e : fs::listDir(tmp)) {
        if (!fs::rename(tmp + "/" + e.name, staging + "/" + e.name)) {
            error = "Cannot reorganize extracted files";
            return false;
        }
    }
    fs::removeAll(tmp);
    return true;
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

    if (!archive::extract(zipPath, staging, zipSize, progress, cancel, error)) {
        fs::removeAll(staging);
        return false;
    }
    if (!normalizeLayout(staging, error)) {
        fs::removeAll(staging);
        return false;
    }

    fs::removeAll(old);

    std::unique_lock<std::mutex> lock(mutex_);

    // Updating a disabled mod (for example a combo member) keeps it
    // disabled: the new copy goes straight into the disabled tree.
    auto prev = mods_.find(mod.id);
    bool keepDisabled = prev != mods_.end() && !prev->second.enabled;
    std::string target = keepDisabled ? paths::disabledModDir(titleId, mod.id) : final_;
    std::string oldDir = fs::parent(target) + "/" + kOldPrefix + mod.id;

    // A previous install somewhere else (other region) is only deleted once
    // the new copy is in place.
    std::string prevDir;
    if (prev != mods_.end()) {
        prevDir = currentDirLocked(prev->second);
        if (prevDir == target) prevDir.clear();
    }

    if (!swapIn(staging, target, oldDir, error)) {
        lock.unlock();
        fs::removeAll(staging);
        return false;
    }
    // A stray copy in the other tree would make SDCafiine see it twice.
    std::string other = keepDisabled ? final_ : paths::disabledModDir(titleId, mod.id);

    InstalledMod im;
    im.id = mod.id;
    im.titleId = titleId;
    im.gameName = gameName;
    im.name = mod.name;
    im.version = mod.version;
    im.sha256 = mod.sha256;
    im.repoUrl = mod.repoUrl;
    im.enabled = !keepDisabled;
    im.installedAt = (int64_t) std::time(nullptr);
    mods_[mod.id] = im;
    saveLocked();
    lock.unlock();

    // Slow deletes happen without the lock so the UI keeps drawing.
    fs::removeAll(oldDir);
    if (!prevDir.empty()) fs::removeAll(prevDir);
    if (other != prevDir) fs::removeAll(other);
    fs::log("installed %s %s to %s", mod.id.c_str(), mod.version.c_str(), target.c_str());
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
    if (!setEnabledLocked(it->second, enabled, error)) return false;
    saveLocked();
    return true;
}

bool Installer::setEnabledLocked(InstalledMod &m, bool enabled, std::string &error) {
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

// ================================================================ combos

static const char *kComboPrefix = "PatchBayCombo-";

std::vector<Combo> Installer::combos() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Combo> out;
    for (const auto &kv : combos_) out.push_back(kv.second);
    return out;
}

bool Installer::getCombo(const std::string &id, Combo &out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = combos_.find(id);
    if (it == combos_.end()) return false;
    out = it->second;
    return true;
}

std::string Installer::nextComboId(const std::string &titleId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int n = 1;; ++n) {
        std::string id = kComboPrefix + std::to_string(n);
        if (combos_.count(id)) continue;
        if (fs::exists(paths::modDir(titleId, id)) || fs::exists(paths::disabledModDir(titleId, id))) continue;
        return id;
    }
}

void Installer::saveComboDefinition(const Combo &combo) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = combos_.find(combo.id);
    if (it == combos_.end()) {
        combos_[combo.id] = combo;
    } else {
        it->second.members = combo.members;
        it->second.gameName = combo.gameName;
    }
    saveLocked();
}

bool Installer::comboStale(const Combo &c) const {
    if (!c.built) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    if (c.builtVersions.size() != c.members.size()) return true;
    for (const auto &id : c.members) {
        auto m = mods_.find(id);
        auto b = c.builtVersions.find(id);
        if (m == mods_.end() || b == c.builtVersions.end() || b->second != m->second.version) return true;
    }
    return false;
}

std::vector<std::string> Installer::combosContaining(const std::string &modId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto &kv : combos_)
        if (std::find(kv.second.members.begin(), kv.second.members.end(), modId) != kv.second.members.end())
            out.push_back(kv.first);
    return out;
}

ComboPlan Installer::planCombo(const std::string &titleId, const std::vector<std::string> &members,
                               const std::atomic<bool> *cancel, ComboPlanProgress *progress) const {
    ComboPlan plan;
    struct Src {
        std::string id, name, dir;
    };
    std::vector<Src> srcs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &id : members) {
            auto it = mods_.find(id);
            if (it == mods_.end() || it->second.titleId != titleId) {
                plan.missing.push_back(id);
                continue;
            }
            srcs.push_back({id, it->second.name, currentDirLocked(it->second)});
            plan.versions[id] = it->second.version;
        }
    }

    // FAT is case-insensitive, so shared files are detected on lower-case paths.
    struct Entry {
        std::string rel;
        std::vector<std::string> srcs, names;
        std::vector<int64_t> sizes;
    };
    std::vector<Entry> entries;
    std::map<std::string, size_t> index; // lower rel -> entries[]
    time_t started = std::time(nullptr);
    if (progress) progress->members = (int) srcs.size();
    std::function<void()> onFile;
    if (progress) onFile = [progress] { progress->files++; };
    for (size_t si = 0; si < srcs.size(); ++si) {
        const Src &s = srcs[si];
        if (cancel && cancel->load()) break;
        if (progress) progress->member = (int) si + 1;
        for (const auto &f : fs::walkFiles(s.dir, onFile, cancel)) {
            std::string rel = f.rel;
            auto slash = rel.find('/');
            std::string top = lower(slash == std::string::npos ? rel : rel.substr(0, slash));
            // Only content/ and aoc/ are read by SDCafiine. Keep everything
            // else (credits, readmes) per mod so nothing collides.
            if (slash == std::string::npos || (top != "content" && top != "aoc"))
                rel = "_credits/" + s.id + "/" + rel;
            else
                rel = top + rel.substr(slash);

            std::string key = lower(rel);
            auto it = index.find(key);
            if (it == index.end()) {
                it = index.emplace(key, entries.size()).first;
                entries.push_back({rel, {}, {}, {}});
            }
            Entry &e = entries[it->second];
            e.srcs.push_back(s.dir + "/" + f.rel);
            e.names.push_back(s.name);
            e.sizes.push_back(f.size);
        }
    }

    for (auto &e : entries) {
        if (e.srcs.size() == 1) {
            plan.copies.push_back({e.srcs[0], e.rel, e.sizes[0]});
            plan.totalBytes += e.sizes[0];
            plan.workBytes += e.sizes[0];
            continue;
        }
        ComboPlan::Merge m;
        m.srcs = std::move(e.srcs);
        m.rel = e.rel;
        m.srcBytes = 0;
        for (auto sz : e.sizes) m.srcBytes += sz;
        plan.totalBytes += e.sizes[0]; // estimate: a merge is about as big as one copy
        plan.workBytes += m.srcBytes;
        if (plan.sharedSamples.size() < 200) {
            std::string who;
            for (size_t i = 0; i < e.names.size(); ++i) who += (i ? " + " : "") + e.names[i];
            plan.sharedSamples.push_back(e.rel + ": " + who);
        }
        plan.merges.push_back(std::move(m));
    }
    plan.shared = (int) plan.merges.size();
    fs::log("plan %s: %d members, %d files, %d shared, scanned in %lds", titleId.c_str(), (int) srcs.size(),
            (int) entries.size(), plan.shared, (long) (std::time(nullptr) - started));
    // A shared file can be one the mods add, so look at a few.
    if (progress) progress->checkingBase = true;
    for (size_t i = 0; i < plan.merges.size() && i < 8 && !plan.baseAvailable; ++i) {
        if (cancel && cancel->load()) break;
        plan.baseAvailable = gamefiles::exists(titleId, plan.merges[i].rel);
    }
    if (!plan.merges.empty())
        fs::log("plan %s: original game files %s (%lds total)", titleId.c_str(),
                plan.baseAvailable ? "found" : "not found", (long) (std::time(nullptr) - started));
    return plan;
}

static bool writeWholeFile(const std::string &path, const std::string &data) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

// ---- Size tables (Twilight Princess HD)
//
// TPHD lists the size of every file in content/FileSizeList.txt, and the
// compressed and decompressed size of every .pack.gz in
// content/DecompressedSizeList.txt. The game trusts these, so a file whose
// listed size is wrong breaks the area that loads it. Every mod ships its own
// full copy of both lists, so a combo rewrites them from the files it
// actually contains.

static bool isSizeTable(const std::string &rel) {
    std::string l = lower(rel);
    return l == "content/filesizelist.txt" || l == "content/decompressedsizelist.txt";
}

// "a\0b\0" -> {"a", "b", ""}; joinNul reverses it exactly.
static std::vector<std::string> splitNul(const std::string &s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '\0') {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

static std::string joinNul(const std::vector<std::string> &parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out.push_back('\0');
        out += parts[i];
    }
    return out;
}

// Uncompressed size from a gzip file's trailer (ISIZE, little-endian).
static bool gzipSizes(const std::string &path, int64_t &comp, uint32_t &decomp) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char t[4];
    bool ok = std::fseek(f, 0, SEEK_END) == 0;
    long size = ok ? std::ftell(f) : -1;
    ok = ok && size >= 18 && std::fseek(f, size - 4, SEEK_SET) == 0 && std::fread(t, 1, 4, f) == 4;
    std::fclose(f);
    if (!ok) return false;
    comp = size;
    decomp = (uint32_t) t[0] | (uint32_t) t[1] << 8 | (uint32_t) t[2] << 16 | (uint32_t) t[3] << 24;
    return true;
}

// `files` holds the lower-case paths ("content/...") of every file in the combo.
static void fixSizeTables(const std::string &staging, const std::set<std::string> &files, const std::string &id) {
    const std::string content = staging + "/content";
    int fixed = 0;

    // DecompressedSizeList first: FileSizeList lists its size.
    std::string text;
    if (files.count("content/decompressedsizelist.txt") &&
        fs::readFile(content + "/DecompressedSizeList.txt", text)) {
        // Records: compressed size, decompressed size, ratio, "./DVDRoot/<path>".
        auto parts = splitNul(text);
        bool changed = false;
        for (size_t i = 0; i + 3 < parts.size(); i += 4) {
            const std::string &p = parts[i + 3];
            static const std::string prefix = "./DVDRoot/";
            if (p.compare(0, prefix.size(), prefix) != 0) continue;
            std::string rel = p.substr(prefix.size()) + ".gz";
            if (!files.count("content/" + lower(rel))) continue;
            int64_t comp;
            uint32_t decomp;
            if (!gzipSizes(content + "/" + rel, comp, decomp) || decomp == 0) continue;
            std::string c = std::to_string(comp), d = std::to_string(decomp);
            if (parts[i] == c && parts[i + 1] == d) continue;
            double v = std::round((1.0 - (double) comp / decomp) * 1000.0) / 10.0;
            char pct[32];
            if (v == std::floor(v)) std::snprintf(pct, sizeof(pct), "%d%%", (int) v);
            else std::snprintf(pct, sizeof(pct), "%.1f%%", v);
            parts[i] = c;
            parts[i + 1] = d;
            parts[i + 2] = pct;
            changed = true;
            fixed++;
        }
        if (changed && !writeWholeFile(content + "/DecompressedSizeList.txt", joinNul(parts)))
            fs::log("combo %s: cannot rewrite DecompressedSizeList.txt", id.c_str());
    }

    if (files.count("content/filesizelist.txt") && fs::readFile(content + "/FileSizeList.txt", text)) {
        // Records: "<path>", "<size>".
        auto parts = splitNul(text);
        bool changed = false;
        for (size_t i = 0; i + 1 < parts.size(); i += 2) {
            std::string key = "content/" + lower(parts[i]);
            if (key == "content/filesizelist.txt" || !files.count(key)) continue;
            int64_t size = fs::fileSize(content + "/" + parts[i]);
            if (size < 0) continue;
            std::string s = std::to_string(size);
            if (parts[i + 1] == s) continue;
            parts[i + 1] = s;
            changed = true;
            fixed++;
        }
        if (changed && !writeWholeFile(content + "/FileSizeList.txt", joinNul(parts)))
            fs::log("combo %s: cannot rewrite FileSizeList.txt", id.c_str());
    }
    if (fixed) fs::log("combo %s: corrected %d size table entries", id.c_str(), fixed);
}

bool Installer::swapIn(const std::string &staging, const std::string &final_, const std::string &old,
                       std::string &error) {
    fs::removeAll(old);
    bool hadOld = fs::exists(final_);
    if (hadOld && !fs::rename(final_, old)) {
        error = "Cannot replace existing folder (in use?)";
        return false;
    }
    fs::mkdirs(fs::parent(final_));
    if (!fs::rename(staging, final_)) {
        if (hadOld) fs::rename(old, final_);
        error = "Cannot move folder into place";
        return false;
    }
    return true;
}

bool Installer::buildCombo(const std::string &id, const std::function<void(int64_t, int64_t)> &progress,
                           const std::atomic<bool> *cancel, std::string &error) {
    Combo c;
    if (!getCombo(id, c)) {
        error = "Combo no longer exists";
        return false;
    }

    ComboPlan plan = planCombo(c.titleId, c.members, cancel);
    if (cancel && cancel->load()) {
        error = "Cancelled";
        return false;
    }
    for (const auto &m : plan.missing) {
        fs::log("combo %s: dropping missing member %s", id.c_str(), m.c_str());
        c.members.erase(std::remove(c.members.begin(), c.members.end(), m), c.members.end());
    }
    if (c.members.empty() || (plan.copies.empty() && plan.merges.empty())) {
        error = "None of the combo's mods are installed";
        return false;
    }

    std::string staging = paths::stagingDir(c.titleId, c.id);
    fs::removeAll(staging);
    if (!fs::mkdirs(staging)) {
        error = "Cannot create " + staging;
        return false;
    }

    int64_t done = 0;
    std::set<std::string> madeDirs;
    for (const auto &cp : plan.copies) {
        if (cancel && cancel->load()) {
            fs::removeAll(staging);
            error = "Cancelled";
            return false;
        }
        std::string dst = staging + "/" + cp.rel;
        std::string dir = fs::parent(dst);
        if (!madeDirs.count(dir)) {
            fs::mkdirs(dir);
            madeDirs.insert(dir);
        }
        bool ok = fs::copyFile(cp.src, dst,
                               [&](int64_t n) {
                                   done += n;
                                   if (progress) progress(done, plan.workBytes);
                               },
                               cancel);
        if (!ok) {
            fs::removeAll(staging);
            error = (cancel && cancel->load()) ? "Cancelled" : "Copy failed for " + cp.rel + " (SD card full?)";
            return false;
        }
    }

    // Files several members ship: merge them against the game's original.
    int merged = 0, conflicts = 0;
    bool usedBase = false;
    int64_t sizeBytes = 0;
    for (const auto &cp : plan.copies) sizeBytes += cp.size;
    std::vector<std::string> conflictSamples;
    for (const auto &mg : plan.merges) {
        if (cancel && cancel->load()) {
            fs::removeAll(staging);
            error = "Cancelled";
            return false;
        }
        std::string dst = staging + "/" + mg.rel;
        fs::mkdirs(fs::parent(dst));
        if (isSizeTable(mg.rel)) {
            // Regenerated from the combo's files below; any copy will do.
            if (!fs::copyFile(mg.srcs[0], dst, nullptr, cancel)) {
                fs::removeAll(staging);
                error = (cancel && cancel->load()) ? "Cancelled" : "Copy failed for " + mg.rel + " (SD card full?)";
                return false;
            }
            merged++;
            sizeBytes += fs::fileSize(dst);
            done += mg.srcBytes;
            if (progress) progress(done, plan.workBytes);
            continue;
        }
        if (mg.srcBytes > kMaxMergeBytes) {
            // Too big to hold in memory: take the highest-priority copy.
            if (!fs::copyFile(mg.srcs[0], dst, nullptr, cancel)) {
                fs::removeAll(staging);
                error = (cancel && cancel->load()) ? "Cancelled" : "Copy failed for " + mg.rel + " (SD card full?)";
                return false;
            }
            conflicts++;
            if (conflictSamples.size() < 200) conflictSamples.push_back(mg.rel + " (too large to merge)");
            fs::log("combo %s: %s too large to merge", id.c_str(), mg.rel.c_str());
            sizeBytes += fs::fileSize(dst);
            done += mg.srcBytes;
            if (progress) progress(done, plan.workBytes);
            continue;
        }

        merge::Result res;
        {
            std::vector<std::string> copies(mg.srcs.size());
            std::vector<const std::string *> ptrs;
            for (size_t i = 0; i < mg.srcs.size(); ++i) {
                int64_t sz = fs::fileSize(mg.srcs[i]);
                if (sz > 0) copies[i].reserve((size_t) sz);
                if (!fs::readFile(mg.srcs[i], copies[i])) {
                    fs::removeAll(staging);
                    error = "Cannot read " + mg.srcs[i];
                    return false;
                }
                ptrs.push_back(&copies[i]);
            }
            std::string base;
            bool haveBase = gamefiles::read(c.titleId, mg.rel, base);
            usedBase |= haveBase;
            try {
                res = merge::mergeFiles(haveBase ? &base : nullptr, ptrs, mg.rel);
            } catch (const std::bad_alloc &) {
                res = merge::Result();
                res.data = std::move(copies[0]);
                res.conflicts = 1;
                res.conflictPaths.push_back(mg.rel + " (out of memory while merging)");
            }
        }
        if (res.conflicts == 0) merged++;
        conflicts += res.conflicts;
        for (auto &p : res.conflictPaths)
            if (conflictSamples.size() < 200) conflictSamples.push_back(p);
        if (res.conflicts) fs::log("combo %s: %d conflict(s) in %s", id.c_str(), res.conflicts, mg.rel.c_str());

        if (!writeWholeFile(dst, res.data)) {
            fs::removeAll(staging);
            error = "Write failed for " + mg.rel + " (SD card full?)";
            return false;
        }
        sizeBytes += (int64_t) res.data.size();
        done += mg.srcBytes;
        if (progress) progress(done, plan.workBytes);
    }

    {
        std::set<std::string> files;
        for (const auto &cp : plan.copies) files.insert(lower(cp.rel));
        for (const auto &mg : plan.merges) files.insert(lower(mg.rel));
        fixSizeTables(staging, files, id);
    }

    // Human-readable manifest next to content/ (SDCafiine ignores it).
    {
        std::string manifest = "Built by PatchBay for " + c.gameName + " (" + c.titleId + ")\n\nPriority order:\n";
        for (size_t i = 0; i < c.members.size(); ++i)
            manifest += "  " + std::to_string(i + 1) + ". " + c.members[i] + "\n";
        manifest += "\n" + std::to_string(plan.shared) + " file(s) shipped by more than one mod, " +
                    std::to_string(merged) + " merged without conflicts.\n";
        if (plan.shared > 0)
            manifest += usedBase ? "Merged against the game's original files.\n"
                                 : "The game's original files were not available (needs Mocha and an installed\n"
                                   "game), so only archive entries a single mod ships could be combined.\n";
        manifest += "\n" + std::to_string(conflicts) +
                    " conflict(s) where mods changed the same thing differently (higher priority wins):\n";
        for (const auto &s : conflictSamples) manifest += "  " + s + "\n";
        FILE *f = std::fopen((staging + "/PATCHBAY_COMBO.txt").c_str(), "wb");
        if (f) {
            std::fwrite(manifest.data(), 1, manifest.size(), f);
            std::fclose(f);
        }
    }

    std::unique_lock<std::mutex> lock(mutex_);
    auto it = combos_.find(id);
    if (it == combos_.end()) {
        lock.unlock();
        fs::removeAll(staging);
        error = "Combo was deleted during the build";
        return false;
    }
    Combo &live = it->second;
    std::string final_ = comboDirLocked(live);
    std::string old = fs::parent(final_) + "/" + kOldPrefix + live.id;
    if (!swapIn(staging, final_, old, error)) {
        lock.unlock();
        fs::removeAll(staging);
        return false;
    }

    live.members = c.members;
    live.built = true;
    live.builtAt = (int64_t) std::time(nullptr);
    live.sizeBytes = sizeBytes;
    live.merged = merged;
    live.conflicts = conflicts;
    live.usedBase = usedBase;
    live.conflictSamples = conflictSamples;
    // Record the versions that were actually copied, so a member updated
    // mid-build still shows the combo as needing a rebuild.
    live.builtVersions.clear();
    for (const auto &m : live.members) {
        auto v = plan.versions.find(m);
        if (v != plan.versions.end()) live.builtVersions[m] = v->second;
    }
    // Mods removed from the combo get their previous state back.
    for (auto r = live.restoreEnabled.begin(); r != live.restoreEnabled.end();) {
        if (std::find(live.members.begin(), live.members.end(), r->first) == live.members.end()) {
            restoreMemberLocked(r->first, r->second, live.id);
            r = live.restoreEnabled.erase(r);
        } else {
            ++r;
        }
    }
    if (live.enabled) disableMembersLocked(live);
    saveLocked();
    lock.unlock();

    fs::removeAll(old);
    fs::log("built combo %s: %d members, %d shared files, %d merged, %d conflicts, base %s, %lld bytes", id.c_str(),
            (int) c.members.size(), plan.shared, merged, conflicts, usedBase ? "yes" : "no", (long long) sizeBytes);
    return true;
}

bool Installer::deleteCombo(const std::string &id, std::string &error) {
    std::string trash;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = combos_.find(id);
        if (it == combos_.end()) {
            error = "Combo not found";
            return false;
        }
        Combo c = it->second;
        std::string dir = comboDirLocked(c);
        trash = paths::kTmpDir + "/trash-" + id;
        fs::mkdirs(paths::kTmpDir);
        fs::removeAll(trash);
        if (fs::exists(dir) && !fs::rename(dir, trash)) {
            error = "Could not remove " + dir;
            return false;
        }
        combos_.erase(it);

        // Give members back the state they had before the combo, unless
        // another enabled combo still covers them.
        for (const auto &kv : c.restoreEnabled) restoreMemberLocked(kv.first, kv.second, c.id);
        saveLocked();
    }
    fs::removeAll(trash);
    fs::log("deleted combo %s", id.c_str());
    return true;
}

bool Installer::setComboEnabled(const std::string &id, bool enabled, std::string &error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = combos_.find(id);
    if (it == combos_.end()) {
        error = "Combo not found";
        return false;
    }
    Combo &c = it->second;
    if (c.enabled != enabled && c.built) {
        std::string from = comboDirLocked(c);
        std::string to = enabled ? paths::modDir(c.titleId, c.id) : paths::disabledModDir(c.titleId, c.id);
        fs::mkdirs(fs::parent(to));
        fs::removeAll(to);
        if (!fs::rename(from, to)) {
            error = "Could not move " + from;
            return false;
        }
    }
    c.enabled = enabled;
    // The combo replaces its members; keep SDCafiine from offering both.
    if (enabled && c.built) disableMembersLocked(c);
    saveLocked();
    return true;
}

bool Installer::coveredByComboLocked(const std::string &modId, const std::string &exceptComboId) const {
    for (const auto &o : combos_) {
        if (o.first == exceptComboId || !o.second.built || !o.second.enabled) continue;
        if (std::find(o.second.members.begin(), o.second.members.end(), modId) != o.second.members.end()) return true;
    }
    return false;
}

void Installer::restoreMemberLocked(const std::string &modId, bool wasEnabled, const std::string &exceptComboId) {
    if (!wasEnabled) return;
    auto mi = mods_.find(modId);
    if (mi == mods_.end() || coveredByComboLocked(modId, exceptComboId)) return;
    std::string err;
    if (!setEnabledLocked(mi->second, true, err)) fs::log("could not re-enable %s: %s", modId.c_str(), err.c_str());
}

void Installer::disableMembersLocked(Combo &c) {
    for (const auto &m : c.members) {
        auto mi = mods_.find(m);
        if (mi == mods_.end()) continue;
        if (!c.restoreEnabled.count(m)) c.restoreEnabled[m] = mi->second.enabled;
        std::string err;
        if (!setEnabledLocked(mi->second, false, err))
            fs::log("combo %s: could not disable member %s: %s", c.id.c_str(), m.c_str(), err.c_str());
    }
}
