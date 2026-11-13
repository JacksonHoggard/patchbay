#pragma once

#include <string>
#include <vector>

// config.json uses the same shape as CoffeeShop's:
//   { "repos": [ "https://.../repo.json", ... ] }
struct Config {
    std::vector<std::string> repos;
    std::string source;   // file the repos were read from (for the UI)
    std::string error;    // non-empty if the file exists but is invalid

    // Loads PatchBay's config. If it does not exist, falls back to
    // CoffeeShop's config.json and copies it over so both stay usable.
    static Config load();
};
