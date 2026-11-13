# Third-party notices

PatchBay is MIT licensed (see `LICENSE`). The release binaries also contain
the following third-party code and data, each under its own license.

| Component | Version | License | Source |
|---|---|---|---|
| nlohmann/json | 3.11.3 | MIT | `libs/nlohmann/json.hpp`, https://github.com/nlohmann/json |
| libarchive (read support only) | 3.8.11 | BSD 2-Clause (see its `COPYING`) | downloaded at configure time, https://github.com/libarchive/libarchive |
| Mozilla CA certificate bundle | see header of `data/cacert.pem` | MPL-2.0 | https://curl.se/docs/caextract.html |
| libcurl | devkitPro portlib | curl license (MIT/X derivative) | https://curl.se |
| Mbed TLS | devkitPro portlib | Apache-2.0 | https://github.com/Mbed-TLS/mbedtls |
| SDL2, SDL2_ttf, SDL2_image | devkitPro portlibs | zlib | https://libsdl.org |
| zlib, liblzma, bzip2, libpng, libjpeg-turbo, libwebp, FreeType, HarfBuzz | devkitPro portlibs | zlib / public domain / bzip2 / libpng / IJG+BSD / BSD / FTL / MIT | upstream projects |
| wut | devkitPro | zlib | https://github.com/devkitPro/wut |

The text font is the Wii U's built-in system font, read from the console at
runtime; it is not distributed with PatchBay.
