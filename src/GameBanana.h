#pragma once

#include "Repo.h"

#include <cstdint>
#include <string>
#include <vector>

// GameBanana (apiv11) as a second mod source next to the JSON repos.
// Everything that touches the network blocks: call it off the render thread.
namespace gb {

struct GameInfo {
    std::string name;
    int gbGameId;
    std::vector<std::string> titleIds; // upper-case 16-digit hex
};

// Built-in table of Wii U games on GameBanana.
const std::vector<GameInfo> &wiiuGames();

// Adds a Game for each table entry not already in the catalog (matched by any
// shared title ID) and sets gbGameId on matching existing games. Re-sorts the
// games by name (case-insensitive) like repo::loadAll does.
void mergeInto(Catalog &catalog);

enum class Sort { MostDownloaded, Newest, MostLiked };
const char *sortName(Sort s); // "Most downloaded", "Newest", "Most liked"

struct Page {
    std::vector<Mod> mods;
    int total = 0;         // records GameBanana reports for the game
    bool complete = false; // true on the last page
    int rawCount = 0;      // records on this page before filtering (0 = past the end)
    std::string error;
};

// `page` is 1-based. Mods come back without a download URL: fetch the details
// and applyFile() one of its files before queueing.
Page fetchMods(int gbGameId, int page, Sort sort, int perPage = 20);

struct FileInfo {
    int64_t id = 0;
    std::string name;
    int64_t size = -1;
    std::string url;
    std::string md5; // lowercase hex, may be empty
    std::string avResult;
    std::string analysisResult;
    bool supported = false; // .zip/.rar/.7z, not archived, not flagged by AV
};

struct Details {
    std::string description; // plain text converted from the HTML body
    std::vector<std::string> screenshots;
    std::vector<FileInfo> files;
    std::string version;
    std::string error;
};

Details fetchDetails(int64_t gbModId);

// Sets download = f.url, fileSize = f.size, md5 = f.md5.
void applyFile(Mod &mod, const FileInfo &f);

// Stable install id: "gb<id>-<slug of name>", at most 48 chars, valid for
// repo::isValidModId. Used as the SDCafiine folder name.
std::string modIdFor(int64_t gbModId, const std::string &name);

} // namespace gb
