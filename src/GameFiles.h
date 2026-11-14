#pragma once

#include <string>

// Reads a game's original (unmodded) files from the console's internal
// storage or USB drive, so combos can tell what each mod changed.
//
// This needs Mocha (built into Aroma, and available on Tiramisu): PatchBay
// asks Mocha to lift the filesystem permissions of one FS client. Without
// Mocha, available() is false and combos fall back to file priority.
// Disc-only games have no installed base files; their installed update, if
// any, is still used.
namespace gamefiles {

// True if title files can be read. Cheap after the first call.
bool available();

// Reads the original version of `rel` ("content/..." or "aoc/...", as laid
// out in an SDCafiine modpack) for `titleId` (16 hex digits). The installed
// update is checked first, then the base game (or the DLC for aoc/).
// Thread-safe. Returns false if the file is not found or too large.
bool read(const std::string &titleId, const std::string &rel, std::string &out);

// Like read(), without reading the file.
bool exists(const std::string &titleId, const std::string &rel);

// Releases the FS client. Call once on exit.
void shutdown();

} // namespace gamefiles
