#pragma once

#include "Repo.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct InstalledMod {
    std::string id;
    std::string titleId;
    std::string gameName;
    std::string name;
    std::string version;  // empty = unknown (found on SD, not installed by PatchBay)
    std::string repoUrl;
    std::string sha256;
    bool enabled = true;
    int64_t installedAt = 0;
};

// Owns installed.json and everything under sd:/wiiu/sdcafiine that PatchBay
// manages. Thread-safe: the download worker installs while the UI reads.
class Installer {
public:
    void load();

    // Cleans up after a crash or power loss mid-install: deletes leftover
    // staging folders and restores a previous version if a swap was cut off.
    void recover();

    // Registers mods that already exist in the SDCafiine folder (for example
    // installed by CoffeeShop) so they can be managed from PatchBay.
    void adoptExisting(const Catalog &catalog);

    std::vector<InstalledMod> list() const;
    bool get(const std::string &modId, InstalledMod &out) const;
    bool isInstalled(const std::string &modId) const;

    // Extracts `zipPath` into a staging folder, then swaps it in for any
    // previous version. A failed extract never touches the existing install.
    bool install(const Mod &mod, const std::string &gameName, const std::string &titleId,
                 const std::string &zipPath, int64_t zipSize,
                 const std::function<void(int64_t, int64_t)> &progress, const std::atomic<bool> *cancel,
                 std::string &error);

    bool uninstall(const std::string &modId, std::string &error);
    bool setEnabled(const std::string &modId, bool enabled, std::string &error);

    // Other enabled mods for the same title. SDCafiine asks which modpack to
    // load at game boot when there is more than one.
    std::vector<InstalledMod> enabledForTitle(const std::string &titleId, const std::string &exceptId) const;

    static int compareVersions(const std::string &a, const std::string &b);

private:
    void saveLocked();
    std::string currentDirLocked(const InstalledMod &m) const;

    mutable std::mutex mutex_;
    std::map<std::string, InstalledMod> mods_;
};
