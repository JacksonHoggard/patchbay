# PatchBay

PatchBay is a Wii U homebrew app that downloads and installs game mods for
[SDCafiine](https://github.com/wiiu-env/SDCafiine) on Aroma. You point it at one
or more mod repositories, browse games and mods on the TV or GamePad, and it
downloads, verifies and extracts each mod into the folder SDCafiine reads from.
It can also disable, re-enable, update and uninstall mods.

It reads repositories written for CoffeeShop's
[mod repo template](https://github.com/timkicker/coffeeshop-repo-template)
without any changes, and can take over CoffeeShop's config and installed mods.

PatchBay is written in C++17 with SDL2 and built with devkitPro/wut.

## Requirements

- A Wii U running a current CFW environment:
  - [Aroma](https://aroma.foryour.cafe/) (recommended), or
  - Tiramisu (legacy; use the `.rpx` from the Homebrew Launcher).
- The SDCafiine plugin (`sdcafiine.wps`) for your environment.
- An SD card (FAT32) in the console.
- An internet connection configured in System Settings.

PatchBay only needs the SD card and the network. If Mocha is running (it is
part of Aroma), combos also read the game's original files from internal
storage or USB to merge mods properly; without it, everything else works the
same and combos fall back to file priority (see Combining mods).

## Install

| Environment | File | Copy to | Launch from |
|---|---|---|---|
| Aroma | `patchbay.wuhb` | `SD:/wiiu/apps/patchbay/patchbay.wuhb` | Wii U Menu or Homebrew Launcher |
| Tiramisu | `patchbay.rpx` | `SD:/wiiu/apps/patchbay/patchbay.rpx` | Homebrew Launcher |

When launched from the Homebrew Launcher, exiting (+) returns to the Homebrew
Launcher; otherwise it returns to the Wii U Menu. While PatchBay is open, the
console's screen dimming and auto power-down are turned off so long downloads
are not interrupted. They are restored on exit.

The first launch creates `SD:/wiiu/apps/patchbay/` and an empty `config.json`
(unless it finds a CoffeeShop config, see below). Add at least one repository
URL and restart PatchBay or press Y on the Repos tab to reload.

## Configuration

PatchBay reads `SD:/wiiu/apps/patchbay/config.json`:

```json
{
  "repos": [
    "https://raw.githubusercontent.com/your-name/your-repo/main/repo.json"
  ]
}
```

- `repos` is a list of URLs, each pointing at a repository's `repo.json`.
- Entries may also be objects of the form `{ "url": "https://..." }`.
- Comments (`//` and `/* */`) are allowed in this file.
- All URLs must be `https://`.

See `config.example.json` in this repository for a starting point.

Load order:

1. If `SD:/wiiu/apps/patchbay/config.json` exists, it is used.
2. Otherwise, if `SD:/wiiu/apps/coffeeshop/config.json` exists, it is used, and
   (if it parses) an identical copy is written to
   `SD:/wiiu/apps/patchbay/config.json`. From then on PatchBay uses its own copy;
   later edits to CoffeeShop's file are not picked up.
3. Otherwise, an empty template (`{"repos": []}`) is written to
   `SD:/wiiu/apps/patchbay/config.json`.

If no repositories are configured, or none of them yields any games, PatchBay
opens on the Repos tab, which shows the config file in use, per-repo load
errors and any warnings produced while parsing.

## Repository format

PatchBay consumes the CoffeeShop repo template (formatVersion 1) as is:
<https://github.com/timkicker/coffeeshop-repo-template>. A short summary of
what PatchBay actually reads follows; the template repo is the reference.

### repo.json

```json
{
  "formatVersion": 1,
  "name": "My Mods",
  "description": "Mods for some games",
  "author": "your-name",
  "version": "1.0.0",
  "games": [
    { "path": "games/example/game.json" }
  ]
}
```

- `formatVersion` defaults to 1 if missing. A value greater than 1 is rejected.
- `games[].path` is resolved relative to the URL of `repo.json`. Absolute paths
  (`/x/game.json`) keep the repo's host, and full `https://` URLs are used as is.
  A plain string instead of `{ "path": ... }` is also accepted.

### game.json

```json
{
  "name": "Example Game",
  "icon": "https://example.com/icon.png",
  "titleIds": ["0005000010101C00", "0005000010101D00"],
  "mods": [
    {
      "id": "example-mod",
      "name": "Example Mod",
      "author": "your-name",
      "version": "1.2.0",
      "description": "What the mod does.",
      "download": "https://github.com/your-name/your-repo/releases/download/v1.2.0/example-mod.zip",
      "type": "mod",
      "thumbnail": "https://example.com/thumb.png",
      "screenshots": ["https://example.com/shot1.png"],
      "releaseDate": "2026-01-01",
      "license": "MIT",
      "tags": ["graphics"],
      "fileSize": 1048576,
      "sha256": "<64 hex characters>",
      "requirements": ["Game update v1.2"],
      "changelog": "Fixed things."
    }
  ]
}
```

- `titleIds` (or `title_ids`) is required. Each entry must be a 16-digit hex
  title ID; separators are stripped and case is ignored. Invalid ones are
  skipped with a warning. A game with no valid title IDs is skipped.
- Mod fields from the template: `id`, `name`, `author`, `version`,
  `description`, `download`, `type` (`mod` or `modpack`). Optional: `thumbnail`,
  `screenshots`, `includes` (mod ids contained in a modpack), `releaseDate`,
  `license`, `tags`, `fileSize`, `sha256`, `requirements`, `changelog`.
- `id` must be 1-64 characters of `a-z`, `0-9`, `-`, `_`. Mods with an invalid
  id or no `download` URL are skipped with a warning.
- Mod ids must be unique across all configured repos, because the install
  folder is named after the id. Later duplicates are skipped with a warning.
- Games from different repos that share any title ID are merged into one entry.
- `fileSize` (bytes) is shown in the UI and used to detect an already complete
  download. `sha256` enables verification (see Security). A malformed
  `sha256` is ignored with a warning.
- Updates are detected by comparing `version` with the installed version,
  number by number (`1.10.0` is newer than `1.9.2`).

### Mod ZIP layout

Each `download` must be a ZIP laid out the way SDCafiine expects:

```
example-mod.zip
  content/...     replacement game files (required unless aoc/ is present)
  aoc/...         DLC files (optional)
  CREDITS.txt     other files and folders are fine and get copied as is
```

ZIP, 7-Zip and RAR (v4 and v5) archives are accepted; the format is detected
from the file contents, not its name. The mod root is the shallowest folder
(up to 4 levels deep, for example `ExampleMod/Wii U/content/...`) that
directly contains `content/` or `aoc/`; it becomes the top level and
anything outside it is dropped. Archives with several such folders at the
same depth (variant packs such as `Option A/content` and `Option B/content`),
Switch mods (`romfs/`, `exefs/`), Cemu graphic packs without game files and
archives without `content/` or `aoc/` are rejected. Entries with absolute
paths, drive letters or `..`, and links or devices, are rejected.

## Files on the SD card

| Path | Purpose |
| --- | --- |
| `SD:/wiiu/apps/patchbay/patchbay.wuhb` | The app. |
| `SD:/wiiu/apps/patchbay/config.json` | Repository list. |
| `SD:/wiiu/sdcafiine/<TITLEID>/<modId>/` | Installed, enabled mods. This is where SDCafiine looks. |
| `SD:/wiiu/sdcafiine/<TITLEID>/PatchBayCombo-N/` | Enabled combos (merged copies of several mods, see Combining mods). |
| `SD:/wiiu/apps/patchbay/disabled/<TITLEID>/<modId>/` | Disabled mods and combos. SDCafiine does not scan this folder. |
| `SD:/wiiu/apps/patchbay/installed.json` | Record of mods PatchBay manages. |
| `SD:/wiiu/apps/patchbay/tmp/<modId>.zip.part` | Downloads in progress. |
| `SD:/wiiu/apps/patchbay/patchbay.log` | Log file (appended to on every run). |

Install details:

- A mod is extracted into a staging folder
  (`SD:/wiiu/sdcafiine/<TITLEID>/.patchbay-staging-<modId>`) and then swapped in
  for the previous version. A failed or cancelled extract does not touch the
  existing install. If the console loses power mid-install, leftover staging
  folders are deleted and an interrupted swap is rolled back on the next launch.
- Disabling a mod moves its folder to the `disabled/` tree; enabling moves it
  back. Uninstalling deletes the folder.
- Updating or reinstalling a disabled mod keeps it disabled.
- `installed.json` stores, per mod id: `titleId`, `game`, `name`, `version`,
  `repo`, `sha256`, `enabled`, `installedAt`, plus combo definitions under
  `combos` (see Combining mods). If the file is unreadable it is
  renamed to `installed.json.bad` and PatchBay starts with an empty record. If a
  recorded mod's folder has been deleted by hand, the record is dropped.

Downloads:

- Downloads go to `tmp/<modId>.zip.part` and are processed one at a time from
  the Downloads tab.
- On cancel or a network error the partial file is kept. Queuing the same mod
  again (Retry on the Downloads tab, or install again later, including after a
  restart) resumes it with an HTTP Range request. If the server does not
  support ranges, the download restarts from zero.
- If the partial file already matches `fileSize`, the download step is skipped.
- The file is deleted after a successful install, on a SHA-256 mismatch, on an
  HTTP error, or when the archive fails to extract.

## Adopting mods installed by CoffeeShop

After the repositories load, PatchBay looks for every mod in the catalog under
`SD:/wiiu/sdcafiine/<TITLEID>/<modId>/` (and under its own `disabled/` tree).
Any it finds that it does not already track are added to `installed.json`, so
they show up on the Installed tab and can be disabled, uninstalled or
reinstalled. This covers mods installed by CoffeeShop, which uses the same
folder layout.

Adopted mods have no recorded version ("version unknown"), so PatchBay cannot
tell whether they are out of date and will not offer an update. Use Reinstall
on the mod's detail page to replace one with the current version. Folders whose
name is not a mod id in a configured repo are left alone and not shown.

## Combining mods (combos)

SDCafiine loads one modpack folder per game. A combo merges several installed
mods for the same title into a single folder,
`SD:/wiiu/sdcafiine/<TITLEID>/PatchBayCombo-N/`, so they can be used together.

- Files are copied, because FAT32 has no links. A combo needs extra SD space
  equal to its merged size. The member mods stay installed and are not
  modified.
- When two or more members ship the same file (same path, compared
  case-insensitively), the copies are merged against the game's original file
  so each mod keeps what it changed:
  - A copy identical to the original changed nothing and is ignored. Mods
    often ship a whole archive to change one entry; this keeps such a mod
    from overwriting another mod's edits to the same archive.
  - Yaz0-compressed files (`.szs` and others) are decompressed, merged and
    recompressed.
  - SARC archives (`.sarc`, `.pack`, `.bars`, the payload of most `.szs`) are
    merged entry by entry, including archives nested inside archives.
    Entries a mod adds are kept, and entries a mod removes are removed.
  - RARC archives (`.arc`, used by Twilight Princess HD) with the same
    folder layout are merged file by file.
  - BMG message files (game text) are merged message by message.
  - When two mods changed the same file, archive entry or message
    differently, the member higher in the list wins. That is a conflict.
- Twilight Princess HD's `content/FileSizeList.txt` and
  `content/DecompressedSizeList.txt` list the size of every game file, and
  every mod ships its own copy. A combo rewrites both from the files it
  actually contains, so no mod's files are listed with the wrong size.
- The original files are read from the installed game update, then the base
  game (or the DLC for `aoc/`), on internal storage or USB. This needs Mocha
  (included in Aroma). Without it, or for a disc-only game with no installed
  update, every copy counts as changed. SARC archives are still combined, but
  only entries a single mod ships survive and the rest is decided by
  priority. The combo editor shows whether the original files were found.
- Members are ordered by priority. The shared files are listed before
  building. Conflicts, and whether the original files were used, are written
  to `PATCHBAY_COMBO.txt` in the combo folder, together with the priority
  order.
- Shared files whose copies add up to more than 96 MiB are not merged (they
  would not fit in memory); the highest-priority copy is used.
- Other formats inside archives (BYML, MSBT, BFRES, and so on) are compared
  as whole files, not merged field by field. RARC archives whose folder
  layouts differ, and UTF-16 BMG files, are not merged.
- Only `content/` and `aoc/` are merged. Other top-level files and folders
  (credits, readmes) are kept per mod under `_credits/<modId>/`.
- Building a combo disables its enabled members so SDCafiine does not offer
  them separately. Enabling a combo disables its members again. Disabling a
  combo moves it to the `disabled/` tree and leaves members as they are.
- Deleting a combo re-enables the members that were enabled before the combo
  disabled them, unless another enabled combo still includes them.
- A combo shows "Needs rebuild" when a member is updated or uninstalled, or the
  member list was changed. Rebuild it from the combo editor. Members that are
  no longer installed are dropped on rebuild.
- Builds run in the Downloads queue and can be cancelled. A build is copied
  into a staging folder and swapped in at the end, so a failed or cancelled
  build leaves the previous combo folder intact.
- Combos are stored in `installed.json` under `combos`.

## Region selection

A game lists one title ID per region. When you install a mod:

- If the mod is already installed (an update or reinstall), it is installed
  for the same title ID as before.
- If exactly one of the game's title IDs is installed on the console, that one
  is used without asking.
- If more than one is installed, you pick from the installed ones.
- If none is installed, you pick from all of the game's title IDs.

Games not found on the console are shown dimmed in the Games list.

## Security

- Only `https://` URLs are fetched, for repos, game files, images and
  downloads. Redirects must also stay on HTTPS.
- Server certificates are verified against a Mozilla CA bundle embedded in the
  app at build time.
- If a mod has a `sha256`, the downloaded ZIP is hashed before extraction and
  rejected (and deleted) on mismatch. Mods without `sha256` are installed
  without this check; repository maintainers should provide it.

## Controls

Navigation works with the D-pad or left stick; holding a direction repeats.
On every screen, + exits to the Wii U Menu (with a confirmation if downloads
are running; unfinished downloads resume when queued again). In dialogs, use
Up/Down to choose, A to confirm and B to cancel.

L/R switch between the five tabs: Games, Installed, Combos, Downloads and
Repos.

| Screen | Buttons |
| --- | --- |
| Games | Up/Down select, Left/Right page, A open game, Y reload repos |
| Game (mod list) | Up/Down select, Left/Right page, A details, Y install or update, X uninstall, B back |
| Mod details | A install / update / reinstall (or view downloads while busy), X uninstall (or cancel the running download), Y enable/disable, Up/Down scroll, L/R previous/next mod (when opened from a game's mod list), B back |
| Installed | Up/Down select, Left/Right page, A details, Y enable/disable, X uninstall |
| Combos | Up/Down select, A edit, Y enable/disable, X delete, - new combo |
| Combo editor | Up/Down select, A add/remove mod, X move up, Y move down, - save and build, B back |
| Downloads | Up/Down select, A retry a failed or cancelled job, X cancel, Y clear finished |
| Repos | Up/Down select, Y reload |

Notes:

- On the mod list, Y does nothing for a mod that is installed and up to date;
  use Reinstall on the details page instead.
- A on the Installed tab only opens details for mods that are in a configured
  repo.
- SDCafiine loads one modpack per game. If more than one mod is enabled for the
  same title, SDCafiine asks which one to load when the game starts. PatchBay
  warns about this when you queue an install; disable the ones you do not want
  from the Installed tab, or merge them into a combo.

## Limitations

- ZIP support covers stored and deflated entries only. Stored (uncompressed)
  entries that use a data descriptor are not supported; re-create the archive
  with compression or without streaming mode.
- Encrypted (password-protected) archives are not supported.
- 7-Zip and RAR archives must be under 2 GiB (ZIPs are streamed and have no
  such limit). Multi-volume (split) archives are not supported.
- The SD card is FAT32, so no single file (including the downloaded ZIP) can be
  4 GiB or larger.
- SDCafiine loads one modpack at a time per game. To use several mods
  together, build a combo; it is a full copy and uses extra SD space. Combos
  merge Yaz0, SARC, RARC and BMG files; other formats are compared as whole files and
  decided by priority when two mods change the same one.
- Updates are only detected for mods installed by PatchBay (adopted mods have
  no known version).

## Building

### Requirements

devkitPro with the Wii U toolchain. On Windows, install it through MSYS2 so it
lives in `C:\msys64\opt\devkitpro`. The following packages are needed:

```
wut wiiu-sdl2 wiiu-sdl2_ttf wiiu-sdl2_image wiiu-curl wiiu-mbedtls ppc-zlib ppc-liblzma ppc-bzip2
wiiu-pkg-config wiiu-cmake
```

Install them from an MSYS2 shell with:

```sh
pacman -S --needed wut wiiu-sdl2 wiiu-sdl2_ttf wiiu-sdl2_image wiiu-curl \
    wiiu-mbedtls ppc-zlib ppc-liblzma ppc-bzip2 wiiu-pkg-config wiiu-cmake
```

(on Linux/macOS use `dkp-pacman` instead of `pacman`).

### Build

From an MSYS2 shell with `DEVKITPRO=/opt/devkitpro` set, in the project root:

```sh
/opt/devkitpro/portlibs/wiiu/bin/powerpc-eabi-cmake -B build -S .
cmake --build build -j
```

The output is `build/patchbay.wuhb` (plus `build/patchbay.rpx`). Configuring
with plain `cmake` fails on purpose; use `powerpc-eabi-cmake`.

### Bundled files

- `data/cacert.pem`: the Mozilla CA bundle from <https://curl.se/ca/cacert.pem>.
  It is embedded into the binary at build time and used to verify HTTPS
  servers. Replace it with a newer copy from the same URL to update it.
- `libs/nlohmann/json.hpp`: vendored [nlohmann/json](https://github.com/nlohmann/json)
  v3.11.3. If the file is missing, CMake downloads it at configure time and
  checks its SHA-256.
- `meta/icon.png`, `meta/tv-splash.png`, `meta/drc-splash.png`: the icon and
  splash screens packed into the `.wuhb`. They must exist for the build to
  succeed.

### CI

`.github/workflows/build.yml` builds the project on every push and pull request
in the `devkitpro/devkitppc` container and uploads `patchbay.wuhb` and
`patchbay.rpx` as the `patchbay` artifact.

## Releasing

1. Bump `project(PatchBay VERSION x.y.z)` in `CMakeLists.txt` and commit.
2. Tag and push: `git tag vx.y.z && git push origin vx.y.z`.
3. CI checks that the tag matches the CMake version, builds, and publishes a
   GitHub release with `patchbay.wuhb` and `patchbay.rpx` attached. The
   Homebrew App Store picks new releases up for review automatically.

Store submission images (icon 256x150, banner 848x208) are in `meta/hbas/`,
generated by `tools/make_meta.py`.

## License

MIT, see `LICENSE`. Bundled third-party components are listed in
`THIRD_PARTY_NOTICES.md`.
