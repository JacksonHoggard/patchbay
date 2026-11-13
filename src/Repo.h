#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Data model for the CoffeeShop repo template (formatVersion 1):
//
//   repo.json : { formatVersion, name, description, author, version,
//                 games: [ { "id"?, "path": "games/<x>/game.json" } ] }
//   game.json : { name, icon?, titleIds: [..], mods: [ Mod... ] }
//
// Repos written for CoffeeShop are consumed unchanged.

struct Mod {
    // Required by the template.
    std::string id;
    std::string name;
    std::string author;
    std::string version;
    std::string description;
    std::string download;
    std::string type; // "mod" | "modpack"

    // Optional.
    std::string thumbnail;
    std::vector<std::string> screenshots;
    std::vector<std::string> includes;
    std::string releaseDate;
    std::string license;
    std::vector<std::string> tags;
    int64_t fileSize = -1;
    std::string sha256; // lowercase hex, empty if not provided
    std::vector<std::string> requirements;
    std::string changelog;

    // Where it came from.
    int repoIndex = -1;
    std::string repoName;
    std::string repoUrl;

    // GameBanana-sourced mods.
    std::string source = "repo"; // "repo" or "gamebanana"
    int64_t gbModId = 0;
    std::string md5;        // lowercase hex, verified after download when sha256 is empty
    std::string profileUrl; // web page (GameBanana)

    bool isModpack() const { return type == "modpack"; }
};

struct Game {
    std::string name;
    std::string icon;
    std::vector<std::string> titleIds; // upper-case 16-digit hex
    std::vector<Mod> mods;
    int gbGameId = 0; // GameBanana game id, 0 if unknown
};

struct RepoInfo {
    std::string url;
    std::string name;
    std::string description;
    std::string author;
    std::string version;
    int formatVersion = 0;
    int gameCount = 0;
    int modCount = 0;
    std::string error; // non-empty if the repo failed to load
};

struct Catalog {
    std::vector<RepoInfo> repos;
    std::vector<Game> games;
    std::vector<std::string> warnings;

    const Game *findGameByTitle(const std::string &titleId) const;
    const Mod *findMod(const std::string &modId, const Game **outGame = nullptr) const;
};

namespace repo {

// Resolves a game.json "path" against the URL of its repo.json.
std::string resolveUrl(const std::string &repoUrl, const std::string &path);

std::string normalizeTitleId(const std::string &tid);
bool isValidModId(const std::string &id);

// Parsers are exposed separately so they can be reused/tested without
// network access. Return false and fill `error` on fatal problems.
bool parseRepo(const std::string &text, RepoInfo &info, std::vector<std::string> &gamePaths, std::string &error);
bool parseGame(const std::string &text, Game &game, std::vector<std::string> &warnings, std::string &error);

// Downloads every configured repo and merges them. Games from different
// repos that share a title ID are merged into one entry. `status` receives
// short progress strings for the loading screen.
Catalog loadAll(const std::vector<std::string> &repoUrls, void (*status)(const std::string &, void *), void *ud);

} // namespace repo
