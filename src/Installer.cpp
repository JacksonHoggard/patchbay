#include "Installer.h"
#include "Fs.h"
#include "Paths.h"
#include "Zip.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <set>
#include <nlohmann/json.hpp>

using nlohmann::json;

static const char *kOldPrefix = ".patchbay-old-";
static const char *kStagingPrefix = ".patchbay-staging-";

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
            c.conflicts = v.value("conflicts", 0);
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
            {"conflicts", c.conflicts},
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
                               const std::atomic<bool> *cancel) const {
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

    // FAT is case-insensitive, so conflicts are detected on lower-case paths.
    std::map<std::string, std::string> winner; // lower rel -> winning mod name
    for (const auto &s : srcs) {
        if (cancel && cancel->load()) break;
        for (const auto &f : fs::walkFiles(s.dir)) {
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
            auto w = winner.find(key);
            if (w != winner.end()) {
                plan.conflicts++;
                if (plan.conflictSamples.size() < 200)
                    plan.conflictSamples.push_back(rel + ": " + w->second + " over " + s.name);
                continue;
            }
            winner[key] = s.name;
            plan.copies.push_back({s.dir + "/" + f.rel, rel, f.size});
            plan.totalBytes += f.size;
        }
    }
    return plan;
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
    if (c.members.empty() || plan.copies.empty()) {
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
                                   if (progress) progress(done, plan.totalBytes);
                               },
                               cancel);
        if (!ok) {
            fs::removeAll(staging);
            error = (cancel && cancel->load()) ? "Cancelled" : "Copy failed for " + cp.rel + " (SD card full?)";
            return false;
        }
    }

    // Human-readable manifest next to content/ (SDCafiine ignores it).
    {
        std::string manifest = "Built by PatchBay for " + c.gameName + " (" + c.titleId + ")\n\nPriority order:\n";
        for (size_t i = 0; i < c.members.size(); ++i)
            manifest += "  " + std::to_string(i + 1) + ". " + c.members[i] + "\n";
        manifest += "\n" + std::to_string(plan.conflicts) + " overridden file(s)\n";
        for (const auto &s : plan.conflictSamples) manifest += "  " + s + "\n";
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
    live.sizeBytes = plan.totalBytes;
    live.conflicts = plan.conflicts;
    live.conflictSamples = plan.conflictSamples;
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
    fs::log("built combo %s: %d members, %d conflicts, %lld bytes", id.c_str(), (int) c.members.size(),
            plan.conflicts, (long long) plan.totalBytes);
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
