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

// A combo merges several installed mods for one game into a single SDCafiine
// modpack folder (sdcafiine/<TITLEID>/<id>/), because SDCafiine only loads
// one modpack per game. Files are copied, so member mods stay untouched.
// When two members ship the same file, the one earlier in `members` wins.
struct Combo {
    std::string id; // folder name, e.g. "PatchBayCombo-1"
    std::string titleId;
    std::string gameName;
    std::vector<std::string> members; // mod ids, highest priority first
    bool enabled = true;

    // Filled in by a build.
    bool built = false;
    int64_t builtAt = 0;
    int64_t sizeBytes = 0;
    int conflicts = 0;
    std::vector<std::string> conflictSamples;      // "path: winner over loser"
    std::map<std::string, std::string> builtVersions; // member id -> version used
    // Member enabled state before the combo disabled it, restored on delete.
    std::map<std::string, bool> restoreEnabled;
};

// Result of planning a merge without writing anything.
struct ComboPlan {
    struct Copy {
        std::string src; // absolute
        std::string rel; // destination relative to the combo folder
        int64_t size;
    };
    std::vector<Copy> copies;
    int64_t totalBytes = 0;
    int conflicts = 0;
    std::vector<std::string> conflictSamples;
    std::vector<std::string> missing; // members that are not installed
    std::map<std::string, std::string> versions; // member id -> version that was planned
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

    // ---- Combos
    std::vector<Combo> combos() const;
    bool getCombo(const std::string &id, Combo &out) const;
    std::string nextComboId(const std::string &titleId) const;
    // Creates or replaces a combo's definition (member list). Does not build.
    void saveComboDefinition(const Combo &combo);
    // True if the combo has never been built or a member changed since.
    bool comboStale(const Combo &combo) const;
    std::vector<std::string> combosContaining(const std::string &modId) const;

    // Works out which file wins for every path. Reads the SD card (walks
    // every member folder), so call it off the render thread for big mods.
    ComboPlan planCombo(const std::string &titleId, const std::vector<std::string> &members,
                        const std::atomic<bool> *cancel = nullptr) const;

    // Copies the members into a staging folder and swaps it in. Members that
    // are enabled get disabled so SDCafiine does not offer them separately.
    bool buildCombo(const std::string &id, const std::function<void(int64_t, int64_t)> &progress,
                    const std::atomic<bool> *cancel, std::string &error);
    bool deleteCombo(const std::string &id, std::string &error);
    bool setComboEnabled(const std::string &id, bool enabled, std::string &error);

private:
    void saveLocked();
    std::string currentDirLocked(const InstalledMod &m) const;
    std::string comboDirLocked(const Combo &c) const;
    bool setEnabledLocked(InstalledMod &m, bool enabled, std::string &error);
    bool coveredByComboLocked(const std::string &modId, const std::string &exceptComboId) const;
    // Re-enables a member a combo had disabled, unless another built+enabled
    // combo still covers it.
    void restoreMemberLocked(const std::string &modId, bool wasEnabled, const std::string &exceptComboId);
    void disableMembersLocked(Combo &c);
    bool swapIn(const std::string &staging, const std::string &final_, const std::string &old, std::string &error);

    mutable std::mutex mutex_;
    std::map<std::string, InstalledMod> mods_;
    std::map<std::string, Combo> combos_;
};
