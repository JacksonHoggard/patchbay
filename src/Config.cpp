#include "Config.h"
#include "Fs.h"
#include "Paths.h"

#include <nlohmann/json.hpp>

using nlohmann::json;

static bool parseConfig(const std::string &text, Config &cfg) {
    json j = json::parse(text, nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) {
        cfg.error = "config.json is not valid JSON";
        return false;
    }
    auto it = j.find("repos");
    if (it == j.end() || !it->is_array()) {
        cfg.error = "config.json has no \"repos\" array";
        return false;
    }
    for (const auto &r : *it) {
        if (r.is_string() && !r.get<std::string>().empty()) cfg.repos.push_back(r.get<std::string>());
        // Tolerate { "url": "..." } entries as well.
        else if (r.is_object() && r.contains("url") && r["url"].is_string())
            cfg.repos.push_back(r["url"].get<std::string>());
    }
    return true;
}

Config Config::load() {
    Config cfg;
    std::string text;

    if (fs::readFile(paths::kConfigFile, text)) {
        cfg.source = paths::kConfigFile;
        parseConfig(text, cfg);
        return cfg;
    }

    if (fs::readFile(paths::kCoffeeShopConfig, text)) {
        cfg.source = paths::kCoffeeShopConfig;
        if (parseConfig(text, cfg)) {
            // Migrate: keep an identical copy in PatchBay's folder.
            fs::writeFileAtomic(paths::kConfigFile, text);
        }
        return cfg;
    }

    // First run: write an empty template the user can fill in.
    json tmpl = {{"repos", json::array()}};
    fs::writeFileAtomic(paths::kConfigFile, tmpl.dump(2) + "\n");
    cfg.source = paths::kConfigFile;
    return cfg;
}
