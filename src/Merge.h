#pragma once

#include <string>
#include <vector>

// Three-way merge of one game file that several mods replace.
//
// Each mod's copy is compared with the original game file (the base) to see
// what that mod actually changed. A mod whose copy equals the base changed
// nothing and is ignored. When two or more mods changed the file:
//   - Yaz0-compressed files are decompressed, merged, and recompressed.
//   - SARC archives are merged entry by entry, recursing into nested
//     archives, so mods that edit different files inside the same archive
//     both keep their changes. Entries added by a mod are kept; entries a mod
//     removed from the base are removed.
//   - Anything else that two mods changed differently is a conflict: the
//     highest-priority mod's copy wins.
// Without a base, every mod's copy counts as changed. SARC archives are then
// still combined (entries only one mod has are kept), but any entry two mods
// ship with different contents is a conflict.
namespace merge {

struct Result {
    std::string data;
    // True if an archive was merged entry by entry.
    bool combined = false;
    // Places where priority decided because two mods changed the same thing.
    int conflicts = 0;
    std::vector<std::string> conflictPaths; // "<label>" or "<label>/<entry>/..."
};

// `mods` is in priority order, highest first, and must not be empty. `base`
// is null when the original file is unavailable. `label` names the file in
// conflict paths.
Result mergeFiles(const std::string *base, const std::vector<const std::string *> &mods, const std::string &label);

} // namespace merge
