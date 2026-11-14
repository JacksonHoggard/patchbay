#pragma once

// Console integration that differs between launch environments.
//
// PatchBay runs on the current Wii U CFWs:
//   - Aroma: as a .wuhb from the Wii U Menu, or as .rpx/.wuhb from the
//     Homebrew Launcher.
//   - Tiramisu: as a .rpx from the Homebrew Launcher.
// It only writes to the SD card, through wut's "fs:" devoptab. Mocha is
// optional: combos use it to read original game files (see GameFiles.h).
namespace platform {

void init();
void shutdown();

// Brings up the network connection configured in System Settings. Blocks for
// a few seconds if there is none; call from a worker thread.
bool connectNetwork();

// True when running inside the Homebrew Launcher (which hijacks Mii Maker).
bool runningFromHbl();

// Leaves the app the way the current environment expects: back to the
// Homebrew Launcher when launched from it, otherwise to the Wii U Menu.
void requestExit();

} // namespace platform
