#pragma once

#include <string>

// All SD card paths used by PatchBay. On Wii U (wut) the SD card is reachable
// through the "fs:" devoptab at /vol/external01.
namespace paths {

inline const std::string kSdRoot      = "fs:/vol/external01";
inline const std::string kAppDir      = kSdRoot + "/wiiu/apps/patchbay";
inline const std::string kConfigFile  = kAppDir + "/config.json";
inline const std::string kInstalledDb = kAppDir + "/installed.json";
inline const std::string kTmpDir      = kAppDir + "/tmp";
inline const std::string kLogFile     = kAppDir + "/patchbay.log";

// CoffeeShop's config location. Read as a fallback so existing users can
// switch over without re-entering their repo URLs.
inline const std::string kCoffeeShopConfig = kSdRoot + "/wiiu/apps/coffeeshop/config.json";

// SDCafiine layout: <sdcafiine>/<TITLEID>/<modId>/content/...
inline const std::string kSdCafiine = kSdRoot + "/wiiu/sdcafiine";

// Disabled mods are moved here (same filesystem, so a rename is atomic).
// SDCafiine never scans this folder.
inline const std::string kDisabledRoot = kAppDir + "/disabled";

inline std::string modDir(const std::string &titleId, const std::string &modId) {
    return kSdCafiine + "/" + titleId + "/" + modId;
}

inline std::string disabledModDir(const std::string &titleId, const std::string &modId) {
    return kDisabledRoot + "/" + titleId + "/" + modId;
}

inline std::string stagingDir(const std::string &titleId, const std::string &modId) {
    return kSdCafiine + "/" + titleId + "/.patchbay-staging-" + modId;
}

inline std::string downloadFile(const std::string &modId) {
    return kTmpDir + "/" + modId + ".zip.part";
}

} // namespace paths
