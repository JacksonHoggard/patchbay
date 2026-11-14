cmake_minimum_required(VERSION 3.13)

project(PatchBay VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIIU)
    message(FATAL_ERROR "PatchBay must be configured with the devkitPro Wii U toolchain (powerpc-eabi-cmake).")
endif()

# nlohmann/json (single header) - fetched once at configure time.
set(JSON_HPP "${CMAKE_CURRENT_SOURCE_DIR}/libs/nlohmann/json.hpp")
if(NOT EXISTS "${JSON_HPP}")
    message(STATUS "Downloading nlohmann/json 3.11.3")
    file(DOWNLOAD
        "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
        "${JSON_HPP}"
        EXPECTED_HASH SHA256=9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
        TLS_VERIFY ON)
endif()

# libarchive (read side only: 7-Zip, RAR, RAR5) - fetched once at configure
# time, pinned by SHA-256. Only the libarchive/ source folder and the license
# are kept under libs/. It is compiled here with a hand-written config.h
# (libs/libarchive-config) instead of libarchive's own CMake, whose platform
# probes do not fit wut/newlib.
set(LIBARCHIVE_VERSION 3.8.11)
set(LIBARCHIVE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/libarchive-${LIBARCHIVE_VERSION}")
if(NOT EXISTS "${LIBARCHIVE_DIR}/libarchive/archive.h")
    set(LIBARCHIVE_TGZ "${CMAKE_CURRENT_BINARY_DIR}/libarchive-${LIBARCHIVE_VERSION}.tar.gz")
    message(STATUS "Downloading libarchive ${LIBARCHIVE_VERSION}")
    file(DOWNLOAD
        "https://github.com/libarchive/libarchive/releases/download/v${LIBARCHIVE_VERSION}/libarchive-${LIBARCHIVE_VERSION}.tar.gz"
        "${LIBARCHIVE_TGZ}"
        EXPECTED_HASH SHA256=900fe9fc7237c2b04f090f3c3648de8fe597525771f5035016537ce2d667d12b
        TLS_VERIFY ON)
    file(ARCHIVE_EXTRACT INPUT "${LIBARCHIVE_TGZ}"
        DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/libs"
        PATTERNS "libarchive-${LIBARCHIVE_VERSION}/libarchive/*.[ch]"
                 "libarchive-${LIBARCHIVE_VERSION}/COPYING")
    file(REMOVE "${LIBARCHIVE_TGZ}")
endif()

set(LIBARCHIVE_SRC "${LIBARCHIVE_DIR}/libarchive")
add_library(archive_read STATIC
    ${LIBARCHIVE_SRC}/archive_acl.c
    ${LIBARCHIVE_SRC}/archive_blake2s_ref.c
    ${LIBARCHIVE_SRC}/archive_blake2sp_ref.c
    ${LIBARCHIVE_SRC}/archive_check_magic.c
    ${LIBARCHIVE_SRC}/archive_crc32.c
    ${LIBARCHIVE_SRC}/archive_cryptor.c
    ${LIBARCHIVE_SRC}/archive_entry.c
    ${LIBARCHIVE_SRC}/archive_entry_sparse.c
    ${LIBARCHIVE_SRC}/archive_entry_xattr.c
    ${LIBARCHIVE_SRC}/archive_hmac.c
    ${LIBARCHIVE_SRC}/archive_options.c
    ${LIBARCHIVE_SRC}/archive_ppmd7.c
    ${LIBARCHIVE_SRC}/archive_rb.c
    ${LIBARCHIVE_SRC}/archive_read.c
    ${LIBARCHIVE_SRC}/archive_read_set_options.c
    ${LIBARCHIVE_SRC}/archive_read_support_format_7zip.c
    ${LIBARCHIVE_SRC}/archive_read_support_format_rar.c
    ${LIBARCHIVE_SRC}/archive_read_support_format_rar5.c
    ${LIBARCHIVE_SRC}/archive_string.c
    ${LIBARCHIVE_SRC}/archive_string_sprintf.c
    ${LIBARCHIVE_SRC}/archive_time.c
    ${LIBARCHIVE_SRC}/archive_util.c
    ${LIBARCHIVE_SRC}/archive_virtual.c
)
target_compile_definitions(archive_read
    PRIVATE PLATFORM_CONFIG_H="${CMAKE_CURRENT_SOURCE_DIR}/libs/libarchive-config/config.h"
    PUBLIC LIBARCHIVE_STATIC)
target_include_directories(archive_read
    PUBLIC "${LIBARCHIVE_DIR}"
    PRIVATE "${DEVKITPRO}/portlibs/ppc/include")
# Third-party code: keep the build output about our own sources.
target_compile_options(archive_read PRIVATE -w -O2)

# Embed the Mozilla CA bundle so libcurl/mbedTLS can verify HTTPS servers.
set(CACERT_PEM "${CMAKE_CURRENT_SOURCE_DIR}/data/cacert.pem")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CACERT_PEM}")
file(READ "${CACERT_PEM}" CACERT_CONTENT)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/cacert.cpp"
    "// Generated from data/cacert.pem - do not edit.\n"
    "#include <cstddef>\n"
    "extern const char kCaCertPem[];\n"
    "extern const std::size_t kCaCertPemSize;\n"
    "const char kCaCertPem[] = R\"CACERT(${CACERT_CONTENT})CACERT\";\n"
    "const std::size_t kCaCertPemSize = sizeof(kCaCertPem);\n")

add_executable(patchbay
    src/main.cpp
    src/App.cpp
    src/Config.cpp
    src/Repo.cpp
    src/GameBanana.cpp
    src/Http.cpp
    src/Fs.cpp
    src/Zip.cpp
    src/Archive.cpp
    src/ArchiveLibarchive.cpp
    src/Sha256.cpp
    src/Installer.cpp
    src/GameFiles.cpp
    src/Merge.cpp
    src/Bmg.cpp
    src/Rarc.cpp
    src/Sarc.cpp
    src/Yaz0.cpp
    src/Titles.cpp
    src/Worker.cpp
    src/JobQueue.cpp
    src/ImageCache.cpp
    src/Gfx.cpp
    src/Platform.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/cacert.cpp"
)

target_include_directories(patchbay PRIVATE src libs)
target_compile_definitions(patchbay PRIVATE
    PATCHBAY_VERSION="${PROJECT_VERSION}")
target_compile_options(patchbay PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2_LIBS REQUIRED IMPORTED_TARGET SDL2_ttf SDL2_image sdl2)
pkg_check_modules(CURL_LIBS REQUIRED IMPORTED_TARGET libcurl)

target_link_libraries(patchbay PRIVATE
    PkgConfig::SDL2_LIBS
    PkgConfig::CURL_LIBS
    archive_read
    lzma
    bz2
    z)

wut_create_rpx(patchbay)

wut_create_wuhb(patchbay
    NAME "PatchBay"
    SHORTNAME "PatchBay"
    AUTHOR "JacksonHoggard"
    ICON "${CMAKE_CURRENT_SOURCE_DIR}/meta/icon.png"
    TVSPLASH "${CMAKE_CURRENT_SOURCE_DIR}/meta/tv-splash.png"
    DRCSPLASH "${CMAKE_CURRENT_SOURCE_DIR}/meta/drc-splash.png"
)
