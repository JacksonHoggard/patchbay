cmake_minimum_required(VERSION 3.13)

project(PatchBay VERSION 1.0.0 LANGUAGES C CXX)

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
    src/Http.cpp
    src/Fs.cpp
    src/Zip.cpp
    src/Sha256.cpp
    src/Installer.cpp
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
