#include "Repo.h"
#include "Fs.h"
#include "Http.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <nlohmann/json.hpp>

using nlohmann::json;

const Game *Catalog::findGameByTitle(const std::string &titleId) const {
    for (const auto &g : games)
        for (const auto &t : g.titleIds)
            if (t == titleId) return &g;
    return nullptr;
}

const Mod *Catalog::findMod(const std::string &modId, const Game **outGame) const {
    for (const auto &g : games)
        for (const auto &m : g.mods)
            if (m.id == modId) {
                if (outGame) *outGame = &g;
                return &m;
            }
    return nullptr;
}

namespace repo {

std::string resolveUrl(const std::string &repoUrl, const std::string &path) {
    if (path.rfind("https://", 0) == 0 || path.rfind("http://", 0) == 0) return path;

    if (!path.empty() && path[0] == '/') {
        // Absolute path: keep scheme://host from the repo URL.
        auto schemeEnd = repoUrl.find("://");
        auto hostEnd = schemeEnd == std::string::npos ? std::string::npos : repoUrl.find('/', schemeEnd + 3);
        return (hostEnd == std::string::npos ? repoUrl : repoUrl.substr(0, hostEnd)) + path;
    }

    std::string base = repoUrl;
    auto q = base.find_first_of("?#");
    if (q != std::string::npos) base.resize(q);
    auto slash = base.find_last_of('/');
    if (slash != std::string::npos) base.resize(slash + 1);

    std::string rel = path;
    while (rel.rfind("./", 0) == 0) rel.erase(0, 2);
    return base + rel;
}

std::string normalizeTitleId(const std::string &tid) {
    std::string out;
    for (char c : tid) {
        if (std::isxdigit((unsigned char) c)) out += (char) std::toupper((unsigned char) c);
    }
    return out;
}

bool isValidModId(const std::string &id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

static std::string str(const json &j, const char *key) {
    auto it = j.find(key);
    if (it == j.end()) return "";
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<int64_t>());
    if (it->is_number()) return it->dump();
    return "";
}

static std::vector<std::string> strList(const json &j, const char *key) {
    std::vector<std::string> out;
    auto it = j.find(key);
    if (it == j.end()) return out;
    if (it->is_array()) {
        for (const auto &v : *it)
            if (v.is_string()) out.push_back(v.get<std::string>());
    } else if (it->is_string() && !it->get<std::string>().empty()) {
        out.push_back(it->get<std::string>());
    }
    return out;
}

bool parseRepo(const std::string &text, RepoInfo &info, std::vector<std::string> &gamePaths, std::string &error) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "repo.json is not valid JSON";
        return false;
    }
    info.name = str(j, "name");
    info.description = str(j, "description");
    info.author = str(j, "author");
    info.version = str(j, "version");
    auto fv = j.find("formatVersion");
    info.formatVersion = (fv != j.end() && fv->is_number_integer()) ? fv->get<int>() : 1;
    if (info.formatVersion > 1) {
        error = "Unsupported formatVersion " + std::to_string(info.formatVersion);
        return false;
    }

    auto games = j.find("games");
    if (games == j.end() || !games->is_array()) {
        error = "repo.json has no \"games\" array";
        return false;
    }
    for (const auto &g : *games) {
        if (g.is_object() && g.contains("path") && g["path"].is_string())
            gamePaths.push_back(g["path"].get<std::string>());
        else if (g.is_string())
            gamePaths.push_back(g.get<std::string>());
    }
    if (info.name.empty()) info.name = "Unnamed repo";
    return true;
}

bool parseGame(const std::string &text, Game &game, std::vector<std::string> &warnings, std::string &error) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "game.json is not valid JSON";
        return false;
    }
    game.name = str(j, "name");
    game.icon = str(j, "icon");

    // The validator accepts both spellings.
    std::vector<std::string> tids = strList(j, "titleIds");
    if (tids.empty()) tids = strList(j, "title_ids");
    for (auto &t : tids) {
        std::string n = normalizeTitleId(t);
        if (n.size() == 16) game.titleIds.push_back(n);
        else warnings.push_back(game.name + ": ignoring bad title ID '" + t + "'");
    }
    if (game.titleIds.empty()) {
        error = "game '" + game.name + "' has no valid titleIds";
        return false;
    }

    auto mods = j.find("mods");
    if (mods == j.end() || !mods->is_array()) return true;

    for (const auto &mj : *mods) {
        if (!mj.is_object()) continue;
        Mod m;
        m.id = str(mj, "id");
        m.name = str(mj, "name");
        m.author = str(mj, "author");
        m.version = str(mj, "version");
        m.description = str(mj, "description");
        m.download = str(mj, "download");
        m.type = str(mj, "type");
        m.thumbnail = str(mj, "thumbnail");
        m.screenshots = strList(mj, "screenshots");
        m.includes = strList(mj, "includes");
        m.releaseDate = str(mj, "releaseDate");
        m.license = str(mj, "license");
        m.tags = strList(mj, "tags");
        m.requirements = strList(mj, "requirements");
        m.changelog = str(mj, "changelog");
        m.sha256 = str(mj, "sha256");
        std::transform(m.sha256.begin(), m.sha256.end(), m.sha256.begin(),
                       [](unsigned char c) { return (char) std::tolower(c); });

        auto fsz = mj.find("fileSize");
        if (fsz != mj.end()) {
            if (fsz->is_number_integer()) m.fileSize = fsz->get<int64_t>();
            else if (fsz->is_string()) m.fileSize = std::strtoll(fsz->get<std::string>().c_str(), nullptr, 10);
        }

        if (!isValidModId(m.id)) {
            warnings.push_back(game.name + ": skipping mod with invalid id '" + m.id + "'");
            continue;
        }
        if (m.download.empty()) {
            warnings.push_back(game.name + "/" + m.id + ": no download URL");
            continue;
        }
        if (m.sha256.size() != 0 && m.sha256.size() != 64) {
            warnings.push_back(game.name + "/" + m.id + ": ignoring malformed sha256");
            m.sha256.clear();
        }
        if (m.name.empty()) m.name = m.id;
        if (m.type.empty()) m.type = "mod";
        game.mods.push_back(std::move(m));
    }
    return true;
}

Catalog loadAll(const std::vector<std::string> &repoUrls, void (*status)(const std::string &, void *), void *ud) {
    Catalog cat;
    std::set<std::string> seenModIds;

    for (size_t ri = 0; ri < repoUrls.size(); ++ri) {
        if (http::aborted()) break;
        RepoInfo info;
        info.url = repoUrls[ri];
        if (status) status("Loading repo " + std::to_string(ri + 1) + "/" + std::to_string(repoUrls.size()), ud);

        std::string body;
        auto res = http::get(info.url, body);
        std::vector<std::string> gamePaths;
        std::string err;
        if (!res.ok) {
            info.error = res.error;
        } else if (!parseRepo(body, info, gamePaths, err)) {
            info.error = err;
        }
        if (!info.error.empty()) {
            fs::log("repo %s: %s", info.url.c_str(), info.error.c_str());
            cat.repos.push_back(info);
            continue;
        }

        for (size_t gi = 0; gi < gamePaths.size(); ++gi) {
            if (http::aborted()) break;
            std::string url = resolveUrl(info.url, gamePaths[gi]);
            if (status)
                status(info.name + ": game " + std::to_string(gi + 1) + "/" + std::to_string(gamePaths.size()), ud);

            std::string gbody;
            auto gres = http::get(url, gbody);
            if (!gres.ok) {
                cat.warnings.push_back(info.name + ": " + gamePaths[gi] + ": " + gres.error);
                continue;
            }
            Game game;
            std::string gerr;
            if (!parseGame(gbody, game, cat.warnings, gerr)) {
                cat.warnings.push_back(info.name + ": " + gamePaths[gi] + ": " + gerr);
                continue;
            }
            info.gameCount++;

            for (auto &m : game.mods) {
                m.repoIndex = (int) ri;
                m.repoName = info.name;
                m.repoUrl = info.url;
            }

            // Merge with an existing game that shares any title ID.
            Game *target = nullptr;
            for (auto &existing : cat.games) {
                for (const auto &t : game.titleIds) {
                    if (std::find(existing.titleIds.begin(), existing.titleIds.end(), t) != existing.titleIds.end()) {
                        target = &existing;
                        break;
                    }
                }
                if (target) break;
            }
            if (!target) {
                cat.games.push_back(Game{game.name, game.icon, game.titleIds, {}});
                target = &cat.games.back();
            } else {
                for (const auto &t : game.titleIds)
                    if (std::find(target->titleIds.begin(), target->titleIds.end(), t) == target->titleIds.end())
                        target->titleIds.push_back(t);
                if (target->icon.empty()) target->icon = game.icon;
            }

            for (auto &m : game.mods) {
                // Install folders are keyed by mod id, so ids must be unique.
                if (!seenModIds.insert(m.id).second) {
                    cat.warnings.push_back(info.name + ": duplicate mod id '" + m.id + "' skipped");
                    continue;
                }
                info.modCount++;
                target->mods.push_back(std::move(m));
            }
        }
        cat.repos.push_back(info);
    }

    std::sort(cat.games.begin(), cat.games.end(), [](const Game &a, const Game &b) {
        std::string x = a.name, y = b.name;
        std::transform(x.begin(), x.end(), x.begin(), ::tolower);
        std::transform(y.begin(), y.end(), y.begin(), ::tolower);
        return x < y;
    });
    for (const auto &w : cat.warnings) fs::log("warning: %s", w.c_str());
    return cat;
}

} // namespace repo
