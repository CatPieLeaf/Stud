cmake_minimum_required(VERSION 3.20)
project(Stud VERSION 2.0.8 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

option(STUD_BUILD_TESTS "Build Stud tests" ON)
option(STUD_BUILD_UI "Build the Qt6 launcher" ON)
# Off when Rosemary comes from elsewhere (STUD_ROSEMARY, or rosemary on PATH).
option(STUD_BUILD_ROSEMARY "Build Rosemary from github.com/CatPieLeaf/rosemary-stud" ON)
# Upstream (microlift/rosemary) with Stud's changes, rebased onto its newest.
set(STUD_ROSEMARY_REPOSITORY "https://github.com/CatPieLeaf/rosemary-stud.git" CACHE STRING
    "Git repository Rosemary is built from")
set(STUD_ROSEMARY_BRANCH "stud" CACHE STRING "Branch of STUD_ROSEMARY_REPOSITORY to build")
option(STUD_SANITIZE "Build with -fsanitize=address,undefined" OFF)
# Hardening: PIE and full RELRO, which distro build flags normally supply but
# prebuilt releases don't get (Arch's analyser flags their absence).

# CMP0083 makes CMake add -pie at link time; -fPIE alone still yields ET_EXEC.
cmake_policy(SET CMP0083 NEW)
include(CheckPIESupported)
check_pie_supported(LANGUAGES CXX)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_link_options($<$<LINK_LANGUAGE:CXX>:-Wl,-z,relro,-z,now>)

# The desktop entry name, AppStream id and Wayland app_id must all match so the
# compositor ties windows to the launcher.
set(STUD_APP_ID "io.github.catpieleaf.Stud" CACHE STRING
    "Reverse-DNS application id: desktop entry, AppStream component and Wayland app_id")
set(STUD_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Stud version, used for packages and AppStream")
# A cached version survives a bump and makes the build report the old one.
# An explicit -DSTUD_VERSION from a packager is left alone.
if(NOT STUD_VERSION STREQUAL PROJECT_VERSION)
  message(WARNING
    "STUD_VERSION is ${STUD_VERSION} but this source tree is ${PROJECT_VERSION}. "
    "This build directory was configured before the bump; re-run cmake with "
    "-DSTUD_VERSION=${PROJECT_VERSION} to build what the tree says it is.")
endif()

# Before any add_subdirectory(), which would otherwise see these empty.
include(GNUInstallDirs)
set(STUD_LIBEXEC_DIR "${CMAKE_INSTALL_LIBEXECDIR}/stud")

if(STUD_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_compile_options(-Wextra)

include(FetchContent)
FetchContent_Declare(
    miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG        3.1.2
)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
# The amalgamated miniz.h repeats its own declarations at this tag.
set(AMALGAMATE_SOURCES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(miniz)

add_subdirectory(stud-config)

# Rosemary (GPL-3.0, compatible with Stud's AGPL-3.0), built with cargo into the
# build tree where stud-ui finds it, and into libexec/stud on install.
if(STUD_BUILD_ROSEMARY)
    find_program(STUD_CARGO cargo REQUIRED)
    find_package(Git REQUIRED)
    include(ExternalProject)
    # Defaults to $STUD_ROSEMARY_TARGET_DIR so build-appimage.sh can share one.
    if(NOT "$ENV{STUD_ROSEMARY_TARGET_DIR}" STREQUAL "")
        set(stud_rosemary_target_default "$ENV{STUD_ROSEMARY_TARGET_DIR}")
    else()
        set(stud_rosemary_target_default "${CMAKE_CURRENT_BINARY_DIR}/rosemary-target")
    endif()
    set(STUD_ROSEMARY_TARGET_DIR "${stud_rosemary_target_default}" CACHE PATH
        "Cargo target directory Rosemary is built in")
    ExternalProject_Add(rosemary
        GIT_REPOSITORY "${STUD_ROSEMARY_REPOSITORY}"
        GIT_TAG        "${STUD_ROSEMARY_BRANCH}"
        GIT_SHALLOW    ON
        # The branch's newest commit every build, reset and cleaned so no leftover
        # change is ever built.
        UPDATE_COMMAND "${GIT_EXECUTABLE}" fetch --depth 1 origin "${STUD_ROSEMARY_BRANCH}"
               COMMAND "${GIT_EXECUTABLE}" reset --hard -q FETCH_HEAD
               COMMAND "${GIT_EXECUTABLE}" clean -fdq
        PATCH_COMMAND ""
        CONFIGURE_COMMAND ""
        BUILD_COMMAND "${STUD_CARGO}" build --release --bin rosemary
                      --manifest-path <SOURCE_DIR>/Cargo.toml
                      --target-dir "${STUD_ROSEMARY_TARGET_DIR}"
        BUILD_ALWAYS ON
        # Renamed over the old file, so a running session keeps its binary.
        INSTALL_COMMAND "${CMAKE_COMMAND}" -E copy
                        "${STUD_ROSEMARY_TARGET_DIR}/release/rosemary"
                        "${CMAKE_CURRENT_BINARY_DIR}/rosemary/rosemary.new"
                COMMAND "${CMAKE_COMMAND}" -E rename
                        "${CMAKE_CURRENT_BINARY_DIR}/rosemary/rosemary.new"
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
# Installing. The deb and rpm (CPack) and the AppImage all package this install
# tree. It is relocatable: the launcher finds Rosemary relative to itself.
#
#   <prefix>/bin/stud                      the launcher
#   <prefix>/libexec/stud/rosemary         Rosemary
#   <prefix>/share/...                     desktop entry, icon, AppStream
# Hidden .player and .studio entries give the Roblox windows their own taskbar icons. Lower case,
# so a package's file list has Stud's own entry first: Discover launches the first one it finds.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/${STUD_APP_ID}.desktop"
              "${CMAKE_CURRENT_SOURCE_DIR}/packaging/${STUD_APP_ID}.player.desktop"
              "${CMAKE_CURRENT_SOURCE_DIR}/packaging/${STUD_APP_ID}.studio.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/${STUD_APP_ID}.metainfo.xml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/stud-mime.xml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/mime/packages"
        RENAME "${STUD_APP_ID}.xml")
# Pre-rendered sizes: some things that draw small icons, title bars among them,
# don't scale the 512 one down when their size is missing.
foreach(icon_size 16 24 32 48 64 128 256)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons/${icon_size}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${icon_size}x${icon_size}/apps"
            RENAME "${STUD_APP_ID}.png")
endforeach()
# The red and blue studs of the Player and Studio actions.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/ui/resources/studred.png"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps"
        RENAME "${STUD_APP_ID}-player.png")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/ui/resources/studblu.png"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps"
        RENAME "${STUD_APP_ID}-studio.png")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/stud.png"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps"
        RENAME "${STUD_APP_ID}.png")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
              "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.exception"
              "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/stud")
# The package is lowercase `stud`; GNUInstallDirs would use the capitalised project name.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/stud")

# Debian requires a DEP-5 copyright file; installed everywhere to keep formats alike.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian-copyright"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/stud"
        RENAME "copyright")

# Compressed here so every format ships the same .gz.
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
install(FILES "${miniz_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/stud/miniz"
        RENAME "LICENSE-miniz.txt")

include(packaging/Packaging.cmake)
