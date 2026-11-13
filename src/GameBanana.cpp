#include "GameBanana.h"
#include "Fs.h"
#include "Http.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace gb {

static const char *kApi = "https://gamebanana.com/apiv11";

// GameBanana game id -> Wii U base game title IDs (JPN/USA/EUR). Title IDs of
// games in the reference repo are copied from its game.json files, the rest
// come from the wiiubrew.org title database.
const std::vector<GameInfo> &wiiuGames() {
    static const std::vector<GameInfo> games = {
        {"Mario Kart 8", 5709, {"000500001010EB00", "000500001010EC00", "000500001010ED00"}},
        {"Breath of the Wild", 5866, {"00050000101C9300", "00050000101C9400", "00050000101C9500"}},
        {"Splatoon", 6170, {"0005000010162B00", "0005000010176900", "0005000010176A00"}},
        {"Super Mario 3D World", 5872, {"0005000010106100", "0005000010145C00", "0005000010145D00"}},
        {"New Super Mario Bros. U", 6391,
         {"0005000010101C00", "0005000010101D00", "0005000010101E00", "000500001014B700", "000500001014B800"}},
        {"Super Smash Bros. for Wii U", 5547, {"0005000010110E00", "0005000010144F00", "0005000010145000"}},
        {"The Wind Waker HD", 8216, {"0005000010143400", "0005000010143500", "0005000010143600"}},
        {"Twilight Princess HD", 18167, {"000500001019C800", "000500001019E500", "000500001019E600"}},
        {"Super Mario Maker", 5867, {"000500001018DB00", "000500001018DC00", "000500001018DD00"}},
        {"Pikmin 3", 6467, {"000500001012BC00", "000500001012BD00", "000500001012BE00"}},
        {"Hyrule Warriors", 6171, {"000500001017CD00", "000500001017D800", "000500001017D900"}},
        {"Captain Toad: Treasure Tracker", 7020, {"0005000010180500", "0005000010180600", "0005000010180700"}},
        {"Paper Mario: Color Splash", 5723, {"000500001F600900", "000500001F600A00", "000500001F600B00"}},
        {"Mario Tennis: Ultra Smash", 6980, {"0005000010199000", "00050000101A3500", "00050000101A3600"}},
        {"Nintendo Land", 7788, {"0005000010101F00", "0005000010102000", "0005000010102100"}},
        {"Wii Party U", 13792, {"000500001011A800", "0005000010137D00", "0005000010137E00"}},
        {"Minecraft: Wii U Edition", 5911, {"00050000101DBE00", "00050000101D9D00", "00050000101D7500"}},
        {"Xenoblade Chronicles X", 6630, {"0005000010116100", "00050000101C4D00", "00050000101C4C00"}},
        {"Bayonetta 2", 6745, {"000500001011B900", "0005000010172600", "0005000010172700"}},
        {"Yoshi's Woolly World", 7654, {"0005000010131F00", "0005000010184D00", "0005000010184E00"}},
        {"Kirby and the Rainbow Curse", 7132, {"0005000010188B00", "00050000101ABC00", "00050000101B5100"}},
        {"Star Fox Zero", 23170, {"00050000101AFF00", "00050000101B0400", "00050000101B0500"}},
        {"Mario Party 10", 6983, {"0005000010161F00", "0005000010162D00", "0005000010162E00"}},
        {"Wii Sports Club", 7038, {"000500001012F100", "0005000010144D00", "0005000010144E00"}},
        {"Pokken Tournament", 6693, {"00050000101C5800", "00050000101DF400", "00050000101DF500"}},
    };
    return games;
}

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

void mergeInto(Catalog &catalog) {
    for (const auto &info : wiiuGames()) {
        Game *match = nullptr;
        for (auto &g : catalog.games) {
            for (const auto &t : g.titleIds)
                if (std::find(info.titleIds.begin(), info.titleIds.end(), t) != info.titleIds.end()) {
                    match = &g;
                    break;
                }
            if (match) break;
        }
        if (match) {
            match->gbGameId = info.gbGameId;
            continue;
        }
        Game g;
        g.name = info.name;
        g.titleIds = info.titleIds;
        g.gbGameId = info.gbGameId;
        catalog.games.push_back(std::move(g));
    }
    std::sort(catalog.games.begin(), catalog.games.end(),
              [](const Game &a, const Game &b) { return lower(a.name) < lower(b.name); });
}

const char *sortName(Sort s) {
    switch (s) {
    case Sort::MostDownloaded: return "Most downloaded";
    case Sort::Newest: return "Newest";
    case Sort::MostLiked: return "Most liked";
    }
    return "";
}

// ---- Tolerant JSON accessors. GameBanana sometimes sends numbers as strings
// and empty objects as [].

static const json *field(const json &j, const char *key) {
    if (!j.is_object()) return nullptr;
    auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
}

static std::string str(const json &j, const char *key) {
    const json *v = field(j, key);
    if (!v) return "";
    if (v->is_string()) return v->get<std::string>();
    if (v->is_number_integer()) return std::to_string(v->get<int64_t>());
    if (v->is_number()) return v->dump();
    return "";
}

static int64_t num(const json &j, const char *key, int64_t def = 0) {
    const json *v = field(j, key);
    if (!v) return def;
    if (v->is_number_integer() || v->is_number_unsigned()) return v->get<int64_t>();
    if (v->is_number_float()) return (int64_t) v->get<double>();
    if (v->is_string()) {
        const std::string &s = v->get_ref<const std::string &>();
        char *end = nullptr;
        long long n = std::strtoll(s.c_str(), &end, 10);
        if (end && end != s.c_str()) return (int64_t) n;
    }
    return def;
}

static bool flag(const json &j, const char *key, bool def = false) {
    const json *v = field(j, key);
    if (!v) return def;
    if (v->is_boolean()) return v->get<bool>();
    if (v->is_number()) return v->get<double>() != 0;
    if (v->is_string()) {
        std::string s = lower(v->get<std::string>());
        return s == "true" || s == "1";
    }
    return def;
}

static const json &obj(const json &j, const char *key) {
    static const json empty = json::object();
    const json *v = field(j, key);
    return v && v->is_object() ? *v : empty;
}

// Iterates an array, or the values of an object used as an array.
template <typename F> static void each(const json &j, const char *key, F f) {
    const json *v = field(j, key);
    if (!v) return;
    if (v->is_array() || v->is_object())
        for (const auto &e : *v) f(e);
}

// Unix timestamp -> "YYYY-MM-DD" (UTC).
static std::string date(int64_t ts) {
    if (ts <= 0) return "";
    int64_t z = ts / 86400 + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", (int) y, (int) m, (int) d);
    return buf;
}

struct Image {
    std::string thumb;
    std::string full;
};

static std::vector<Image> images(const json &record) {
    std::vector<Image> out;
    each(obj(record, "_aPreviewMedia"), "_aImages", [&](const json &img) {
        std::string base = str(img, "_sBaseUrl");
        std::string file = str(img, "_sFile");
        if (base.empty() || file.empty()) return;
        std::string small = str(img, "_sFile530");
        if (small.empty()) small = str(img, "_sFile220");
        if (small.empty()) small = file;
        out.push_back({base + "/" + small, base + "/" + file});
    });
    return out;
}

static bool fetchJson(const std::string &url, json &out, std::string &error) {
    std::string body;
    auto res = http::get(url, body, 8 * 1024 * 1024);
    if (!res.ok) {
        error = !res.error.empty() ? res.error : "HTTP " + std::to_string(res.status);
        return false;
    }
    out = json::parse(body, nullptr, false);
    if (out.is_discarded() || !out.is_object()) {
        error = "GameBanana returned invalid data";
        return false;
    }
    std::string apiErr = str(out, "_sErrorMessage");
    if (apiErr.empty()) apiErr = str(out, "_sErrorCode");
    if (!apiErr.empty()) {
        error = "GameBanana: " + apiErr;
        return false;
    }
    return true;
}

std::string modIdFor(int64_t gbModId, const std::string &name) {
    std::string prefix = "gb" + std::to_string(gbModId);
    std::string slug;
    bool dash = false;
    for (unsigned char c : name) {
        if (std::isalnum(c) && c < 0x80) {
            if (dash && !slug.empty()) slug += '-';
            dash = false;
            slug += (char) std::tolower(c);
        } else {
            dash = true;
        }
    }
    const size_t kMax = 48;
    if (slug.empty() || prefix.size() + 1 >= kMax) return prefix.substr(0, kMax);
    size_t room = kMax - prefix.size() - 1;
    if (slug.size() > room) slug.resize(room);
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    return slug.empty() ? prefix : prefix + "-" + slug;
}

Page fetchMods(int gbGameId, int page, Sort sort, int perPage) {
    Page out;
    if (gbGameId <= 0) {
        out.error = "Game is not on GameBanana";
        return out;
    }
    if (page < 1) page = 1;
    perPage = std::max(1, std::min(perPage, 50));

    std::string url = std::string(kApi) + "/Mod/Index?_nPage=" + std::to_string(page) +
                      "&_nPerpage=" + std::to_string(perPage) +
                      "&_aFilters%5BGeneric_Game%5D=" + std::to_string(gbGameId);
    // Without _sSort the index is newest first.
    if (sort == Sort::MostDownloaded) url += "&_sSort=Generic_MostDownloaded";
    else if (sort == Sort::MostLiked) url += "&_sSort=Generic_MostLiked";

    json j;
    if (!fetchJson(url, j, out.error)) return out;

    const json &meta = obj(j, "_aMetadata");
    out.total = (int) num(meta, "_nRecordCount");
    out.complete = flag(meta, "_bIsComplete", true);

    each(j, "_aRecords", [&](const json &r) {
        out.rawCount++;
        if (!r.is_object()) return;
        if (!flag(r, "_bHasFiles", true) || flag(r, "_bIsObsolete")) return;
        int64_t id = num(r, "_idRow");
        std::string name = str(r, "_sName");
        if (id <= 0 || name.empty()) return;

        Mod m;
        m.source = "gamebanana";
        m.gbModId = id;
        m.id = modIdFor(id, name);
        m.name = name;
        m.author = str(obj(r, "_aSubmitter"), "_sName");
        m.version = str(r, "_sVersion");
        if (m.version.empty())
            m.version = date(std::max(num(r, "_tsDateUpdated"), num(r, "_tsDateModified")));

        std::string root = str(obj(r, "_aRootCategory"), "_sName");
        std::string sub = str(obj(r, "_aSubCategory"), "_sName");
        if (!root.empty()) m.tags.push_back(root);
        if (!sub.empty() && sub != root) m.tags.push_back(sub);
        for (const auto &t : m.tags) m.description += (m.description.empty() ? "" : " / ") + t;

        m.type = "mod";
        auto imgs = images(r);
        if (!imgs.empty()) m.thumbnail = imgs.front().thumb;
        for (const auto &img : imgs) m.screenshots.push_back(img.full);
        m.releaseDate = date(num(r, "_tsDateAdded"));
        m.profileUrl = str(r, "_sProfileUrl");
        m.fileSize = -1;
        m.repoName = "GameBanana";
        out.mods.push_back(std::move(m));
    });
    return out;
}

// ---- HTML to plain text

static void appendUtf8(std::string &out, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return;
    if (cp < 0x80) {
        out += (char) cp;
    } else if (cp < 0x800) {
        out += (char) (0xC0 | (cp >> 6));
        out += (char) (0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char) (0xE0 | (cp >> 12));
        out += (char) (0x80 | ((cp >> 6) & 0x3F));
        out += (char) (0x80 | (cp & 0x3F));
    } else {
        out += (char) (0xF0 | (cp >> 18));
        out += (char) (0x80 | ((cp >> 12) & 0x3F));
        out += (char) (0x80 | ((cp >> 6) & 0x3F));
        out += (char) (0x80 | (cp & 0x3F));
    }
}

// Decodes the entity starting at html[i] ('&'). Returns chars consumed, 0 if
// it is not a known entity.
static size_t decodeEntity(const std::string &html, size_t i, std::string &out) {
    // Entities are short; only look a few characters ahead for ';' so a bare
    // '&' in a long description doesn't rescan the rest of the text.
    size_t semi = std::string::npos;
    for (size_t k = i + 1; k < html.size() && k <= i + 11; ++k)
        if (html[k] == ';') {
            semi = k;
            break;
        }
    if (semi == std::string::npos) return 0;
    std::string name = html.substr(i + 1, semi - i - 1);
    if (name.empty()) return 0;
    if (name[0] == '#') {
        bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
        std::string digits = name.substr(hex ? 2 : 1);
        if (digits.empty()) return 0;
        char *end = nullptr;
        unsigned long cp = std::strtoul(digits.c_str(), &end, hex ? 16 : 10);
        if (!end || *end) return 0;
        if (cp == 0xA0) cp = ' ';
        appendUtf8(out, (uint32_t) cp);
        return semi - i + 1;
    }
    static const struct {
        const char *name;
        const char *text;
    } kNamed[] = {{"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"#39", "'"},
                  {"nbsp", " "}, {"ndash", "-"}, {"mdash", "-"}, {"hellip", "..."}, {"rsquo", "'"},
                  {"lsquo", "'"}, {"rdquo", "\""}, {"ldquo", "\""}, {"bull", "*"}};
    for (const auto &e : kNamed)
        if (name == e.name) {
            out += e.text;
            return semi - i + 1;
        }
    return 0;
}

static std::string htmlToText(const std::string &html) {
    std::string out;
    out.reserve(html.size());
    size_t i = 0;
    while (i < html.size()) {
        char c = html[i];
        // Only treat '<' as a tag when it looks like one ("<b", "</p", "<!--")
        // and closes reasonably soon; otherwise it is plain text ("a < b").
        size_t end = std::string::npos;
        if (c == '<' && i + 1 < html.size() &&
            (std::isalpha((unsigned char) html[i + 1]) || html[i + 1] == '/' || html[i + 1] == '!')) {
            end = html.find('>', i);
            if (end != std::string::npos && end - i > 4096) end = std::string::npos;
        }
        if (c == '<' && end != std::string::npos) {
            std::string tag = lower(html.substr(i + 1, end - i - 1));
            i = end + 1;
            bool closing = !tag.empty() && tag[0] == '/';
            size_t s = closing ? 1 : 0, e = s;
            while (e < tag.size() && std::isalnum((unsigned char) tag[e])) e++;
            std::string name = tag.substr(s, e - s);

            if (!closing && (name == "script" || name == "style")) {
                size_t close = lower(html.substr(i)).find("</" + name);
                if (close == std::string::npos) break;
                i += close;
                continue;
            }
            if (name == "br") out += '\n';
            else if (name == "p" || name == "div" || name == "ul" || name == "ol" || name == "blockquote" ||
                     name == "tr" || name == "hr" ||
                     (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6'))
                out += '\n';
            else if (name == "li") out += closing ? "\n" : "\n- ";
            else if ((name == "td" || name == "th") && closing) out += ' ';
            continue;
        }
        if (c == '&') {
            size_t n = decodeEntity(html, i, out);
            if (n) {
                i += n;
                continue;
            }
        }
        // HTML collapses source whitespace; line breaks come from tags.
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        out += c;
        i++;
    }

    // Tidy: collapse spaces, trim each line, at most one blank line in a row.
    std::string text;
    int blank = 0;
    size_t pos = 0;
    while (pos <= out.size()) {
        size_t nl = out.find('\n', pos);
        if (nl == std::string::npos) nl = out.size();
        std::string line;
        for (size_t k = pos; k < nl; k++) {
            char ch = out[k];
            if (ch == ' ' && (line.empty() || line.back() == ' ')) continue;
            line += ch;
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (line.empty() || line == "-") {
            blank++;
        } else {
            if (!text.empty()) text += blank > 0 ? "\n\n" : "\n";
            text += line;
            blank = 0;
        }
        pos = nl + 1;
    }
    return text;
}

static bool supportedArchive(const std::string &name) {
    std::string n = lower(name);
    auto ends = [&](const char *ext) {
        size_t l = std::strlen(ext);
        return n.size() > l && n.compare(n.size() - l, l, ext) == 0;
    };
    return ends(".zip") || ends(".rar") || ends(".7z");
}

Details fetchDetails(int64_t gbModId) {
    Details out;
    json j;
    if (!fetchJson(std::string(kApi) + "/Mod/" + std::to_string(gbModId) + "/ProfilePage", j, out.error)) return out;

    out.description = htmlToText(str(j, "_sText"));
    if (out.description.empty()) out.description = str(j, "_sDescription");
    out.version = str(j, "_sVersion");
    for (const auto &img : images(j)) out.screenshots.push_back(img.full);

    each(j, "_aFiles", [&](const json &f) {
        if (!f.is_object()) return;
        FileInfo fi;
        fi.id = num(f, "_idRow");
        fi.name = str(f, "_sFile");
        fi.size = num(f, "_nFilesize", -1);
        if (fi.size <= 0) fi.size = -1;
        fi.url = str(f, "_sDownloadUrl");
        if (fi.url.empty() && fi.id > 0) fi.url = "https://gamebanana.com/dl/" + std::to_string(fi.id);
        fi.md5 = lower(str(f, "_sMd5Checksum"));
        if (fi.md5.size() != 32) fi.md5.clear();
        fi.avResult = str(f, "_sAvResult");
        fi.analysisResult = str(f, "_sAnalysisResult");
        std::string av = lower(fi.avResult);
        fi.supported = supportedArchive(fi.name) && !flag(f, "_bIsArchived") && av != "infected" &&
                       av != "suspicious" && http::isHttps(fi.url);
        out.files.push_back(std::move(fi));
    });
    return out;
}

void applyFile(Mod &mod, const FileInfo &f) {
    mod.download = f.url;
    mod.fileSize = f.size;
    mod.md5 = f.md5;
}

} // namespace gb
