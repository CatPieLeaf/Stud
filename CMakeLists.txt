cmake_minimum_required(VERSION 3.20)
project(Stud VERSION 1.2.14 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

option(STUD_BUILD_TESTS "Build Stud tests" ON)
option(STUD_BUILD_UI "Build the Qt6 launcher/auth UI" ON)
# Rosemary, which runs the macOS client, built from upstream with cargo.
# Off for a package that takes Rosemary from elsewhere (STUD_ROSEMARY, or
# `rosemary` on PATH).
option(STUD_BUILD_ROSEMARY "Build Rosemary from github.com/microlift/rosemary" ON)
option(STUD_SANITIZE "Build with -fsanitize=address,undefined" OFF)
# Hardening.
#
# Position-independent executables and immediate binding with a read-only
# GOT are what a distribution's own build flags supply by default, and an
# Arch packaging analyser reports their absence (`lacks PIE`, `lacks FULL
# RELRO`) on binaries built without them, which a prebuilt release is,
# since it never passes through makepkg's flags.

# POSITION_INDEPENDENT_CODE alone only reaches the COMPILER (-fPIE); the
# link needs -pie as well, which CMP0083 is what makes CMake add. Without
# the policy the binaries still come out ET_EXEC and the analyser is
# right to say so, checked, before this line existed.
cmake_policy(SET CMP0083 NEW)
include(CheckPIESupported)
check_pie_supported(LANGUAGES CXX)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_link_options($<$<LINK_LANGUAGE:CXX>:-Wl,-z,relro,-z,now>)

# The application's identity, in one place.
#
# This is what a desktop entry is named, what AppStream keys its
# component on, and what Stud's windows and the client's report as their
# Wayland app_id, a compositor ties a window back to its launcher by matching
# those, and a store lists an application by it. They have to agree, so
# they all come from here.
set(STUD_APP_ID "io.github.catpieleaf.Stud" CACHE STRING
    "Reverse-DNS application id: desktop entry, AppStream component and Wayland app_id")
set(STUD_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Stud version, used for packages and AppStream")
# A cache entry is written once and then kept, so bumping the version above
# leaves an existing build directory building and REPORTING the old one --
# which is exactly how a freshly built Stud still called itself 1.1.4. A
# packager passing -DSTUD_VERSION is doing it deliberately and is left
# alone; anyone else is told what to run.
if(NOT STUD_VERSION STREQUAL PROJECT_VERSION)
  message(WARNING
    "STUD_VERSION is ${STUD_VERSION} but this source tree is ${PROJECT_VERSION}. "
    "This build directory was configured before the bump; re-run cmake with "
    "-DSTUD_VERSION=${PROJECT_VERSION} to build what the tree says it is.")
endif()

# Where an install puts things. Defined before any add_subdirectory()
# below, because a subdirectory is processed where it is added and would
# otherwise see these empty.
include(GNUInstallDirs)
set(STUD_LIBEXEC_DIR "${CMAKE_INSTALL_LIBEXECDIR}/stud")
set(STUD_PRIVATE_LIB_DIR "lib/stud")

if(STUD_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# The warning floor for Stud's own targets.
#
# Added here rather than per target, so every directory below inherits it.
add_compile_options(-Wextra)

# nlohmann_json for stud-config, and miniz for the client download and the
# log export.
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)
FetchContent_Declare(
    miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG        3.1.2
)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
# The amalgamated miniz.h repeats its own declarations at this tag; the
# plain headers (miniz.h, miniz_zip.h) do not.
set(AMALGAMATE_SOURCES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(miniz)

add_subdirectory(stud-config)

# Rosemary, from its own repository's default branch, with cargo. Into
# rosemary/rosemary in the build tree, where a stud-ui run from the build
# finds it, and libexec/stud/ on install. Its licence is GPL-3.0, which
# Stud's AGPL-3.0 combines with; its source is upstream, unmodified.
if(STUD_BUILD_ROSEMARY)
    find_program(STUD_CARGO cargo REQUIRED)
    include(ExternalProject)
    set(STUD_ROSEMARY_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/rosemary-target")
    ExternalProject_Add(rosemary
        GIT_REPOSITORY https://github.com/microlift/rosemary.git
        GIT_TAG        main
        GIT_SHALLOW    ON
        CONFIGURE_COMMAND ""
        BUILD_COMMAND "${STUD_CARGO}" build --release --bin rosemary
                      --manifest-path <SOURCE_DIR>/Cargo.toml
                      --target-dir "${STUD_ROSEMARY_TARGET_DIR}"
        BUILD_ALWAYS ON
        INSTALL_COMMAND "${CMAKE_COMMAND}" -E copy
                        "${STUD_ROSEMARY_TARGET_DIR}/release/rosemary"
                        "${CMAKE_CURRENT_BINARY_DIR}/rosemary/rosemary"
                COMMAND "${CMAKE_COMMAND}" -E copy <SOURCE_DIR>/LICENSE
                        "${CMAKE_CURRENT_BINARY_DIR}/rosemary/LICENSE"
        USES_TERMINAL_BUILD ON
    )
    install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/rosemary/rosemary"
            DESTINATION "${STUD_LIBEXEC_DIR}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/rosemary/LICENSE"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/stud/rosemary"
            RENAME "LICENSE-Rosemary.txt")
endif()

if(STUD_BUILD_UI)
    add_subdirectory(ui)
endif()

if(STUD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()


# ---------------------------------------------------------------------
# Installing, and the packages built from it
#
# One install tree feeds every format: the deb and the rpm are CPack
# packaging exactly what `cmake --install` lays down, and the AppImage is
# quick-sharun bundling the same layout (packaging/build-appimage.sh).
#
# The layout is relocatable: the launcher finds Rosemary relative to its
# own path, because an AppImage is mounted somewhere different on every
# run, so nothing may be compiled in as absolute.
#
#   <prefix>/bin/stud                      the launcher
#   <prefix>/libexec/stud/rosemary         Rosemary
#   <prefix>/share/...                     desktop entry, icon, AppStream
# The desktop entry and AppStream component, named by the application id.
# An installed entry runs the plain command, since the launcher is the one
# binary on PATH; the build-tree script substitutes an absolute path into
# the same template instead.
set(STUD_EXEC "stud")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/stud.desktop.in"
               "${CMAKE_CURRENT_BINARY_DIR}/packaging/${STUD_APP_ID}.desktop" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/stud.metainfo.xml.in"
               "${CMAKE_CURRENT_BINARY_DIR}/packaging/${STUD_APP_ID}.metainfo.xml" @ONLY)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/packaging/${STUD_APP_ID}.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/packaging/${STUD_APP_ID}.metainfo.xml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
# Every size, not just the master.
#
# A single 512x512 icon is what a desktop scales down when it has nothing
# better, and several things that draw small icons, a window's titlebar
# among them, do not look for one at all if the theme has no entry at
# their size. The smaller ones are pre-rendered and committed rather than
# generated here, so building Stud needs no image tooling.
foreach(icon_size 16 24 32 48 64 128 256)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons/${icon_size}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${icon_size}x${icon_size}/apps"
            RENAME "${STUD_APP_ID}.png")
endforeach()
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/stud.png"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps"
        RENAME "${STUD_APP_ID}.png")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
              "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.exception"
              "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/stud")
# GNUInstallDirs derives the doc directory from the CMake project name,
# which is capitalised; the package is `stud`, and the two have to agree
# or the documentation lands somewhere nothing looks.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/stud")

# Debian policy requires /usr/share/doc/<package>/copyright, in the
# machine-readable DEP-5 format, naming every component and its licence
# (lintian: no-copyright-file). It is installed everywhere rather than
# only in the deb, an rpm or an Arch package carrying it too costs a
# few kilobytes and one fewer thing that differs between formats.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian-copyright"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/stud"
        RENAME "copyright")

# The man page. @STUD_VERSION@ in its header is the only substitution.
#
# Compressed here rather than left to the packaging tools, so every format
# gets the same file: man reads a .gz directly, and dpkg and makepkg both
# expect one.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/man/stud.1"
               "${CMAKE_CURRENT_BINARY_DIR}/packaging/stud.1" @ONLY)
find_program(STUD_GZIP gzip)
if(STUD_GZIP)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/packaging/stud.1.gz"
        COMMAND "${STUD_GZIP}" -9nc "${CMAKE_CURRENT_BINARY_DIR}/packaging/stud.1"
                > "${CMAKE_CURRENT_BINARY_DIR}/packaging/stud.1.gz"
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/packaging/stud.1"
        COMMENT "Compressing stud.1"
        VERBATIM)
    add_custom_target(stud_manpage ALL
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/packaging/stud.1.gz")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/packaging/stud.1.gz"
            DESTINATION "${CMAKE_INSTALL_MANDIR}/man1")
else()
    message(WARNING "no gzip found, the man page will ship uncompressed")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/packaging/stud.1"
            DESTINATION "${CMAKE_INSTALL_MANDIR}/man1")
endif()
# volk and miniz are compiled into the launcher; their notices travel with it.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/volk/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/stud/volk"
        RENAME "LICENSE-volk.txt")
install(FILES "${miniz_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/stud/miniz"
        RENAME "LICENSE-miniz.txt")

include(packaging/Packaging.cmake)
