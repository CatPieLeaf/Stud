cmake_minimum_required(VERSION 3.20)
project(Stud VERSION 0.1.0 LANGUAGES CXX C ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# RelWithDebInfo, not Debug. Stud is a renderer: every GL and Vulkan call
# the engine makes crosses this codebase, hundreds of times a frame, so
# an unoptimised build is not merely slower to develop against -- it is
# the frame rate. Profiling the render thread in a real game with a Debug
# build put a third of the frame inside std::vector's own iterator
# machinery in Stud's Vulkan client, code that vanishes entirely at -O2.
#
# Debug info is kept because this project genuinely depends on it: the
# sandbox blocks perf, so profiles come from attaching and resolving
# addresses against these binaries (see tmp prof.sh in the frame-rate
# section of the engineering notes).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

option(STUD_BUILD_TESTS "Build Stud tests" ON)
option(STUD_BUILD_UI "Build the Qt6 launcher/auth UI" ON)
option(STUD_SANITIZE "Build with -fsanitize=address,undefined" OFF)
option(STUD_ENABLE_DEV_RENDER_TOGGLE "Enable the prototype-only dev render-backend config toggle" ON)

# The application's identity, in one place.
#
# This is what a desktop entry is named, what AppStream keys its
# component on, and what both of Stud's windows report as their Wayland
# app_id -- a compositor ties a window back to its launcher by matching
# those, and a store lists an application by it. They have to agree, so
# they all come from here.
set(STUD_APP_ID "io.github.catpieleaf.Stud" CACHE STRING
    "Reverse-DNS application id: desktop entry, AppStream component and Wayland app_id")
set(STUD_VERSION "0.1.0" CACHE STRING "Stud version, used for packages and AppStream")

# Where an install puts things. Defined before any add_subdirectory()
# below, because a subdirectory is processed where it is added and would
# otherwise see these empty.
include(GNUInstallDirs)
set(STUD_LIBEXEC_DIR "${CMAKE_INSTALL_LIBEXECDIR}/stud")
set(STUD_PRIVATE_LIB_DIR "lib/stud")

# Third-party dependencies Stud does not vendor and cannot redistribute:
# the Android NDK, the from-source ANGLE build, and a real extracted
# bionic (libc/linker64/...). tools/setup.sh puts all three here.
#
# One root, overridable, defaulting INSIDE the repo -- an earlier layout
# resolved these relative to the parent directory of the checkout, which
# only worked on the one machine the tree was developed in.
set(STUD_THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party" CACHE PATH
    "Where setup.sh puts the NDK, the ANGLE build and the extracted bionic")

if(STUD_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Real, proven, generic infra kept as-is across the 2026-09-02 reset
# (the engineering notes' own "nuke stud, recreate sober 1:1" entry): real
# bionic ELF loading/sandboxing, real ANGLE render-host + its IPC
# transport, real asset/APK extraction, real login/settings UI. None of
# this is "wiring libroblox" -- it's OS/rendering plumbing, proven
# working via extensive live testing before the reset, and rebuilding
# it from scratch would be pure waste.

# Host-side jni.h only (JNIEnv/jobject types used by AAssetManager_
# fromJava/ANativeWindow_fromSurface's signatures in android-glue and
# render-host) -- the real jnivm/FakeJni JNIEnv *implementation* now
# lives only in runtime/'s own separate bionic-toolchain FetchContent
# (runtime/CMakeLists.txt), never built for the host. This host-side
# fetch exists purely so android-glue/render-host/tools can see the
# real jni.h type declarations; nothing here links jnivm's actual
# runtime code.
include(FetchContent)
FetchContent_Declare(
    libjnivm
    GIT_REPOSITORY https://github.com/ChristopherHX/libjnivm.git
    GIT_TAG        main
)
set(JNIVM_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(JNIVM_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(libjnivm)
unset(CMAKE_POLICY_VERSION_MINIMUM)

add_subdirectory(bionic-runtime)
add_subdirectory(cpu-compat)
add_subdirectory(android-glue)
add_subdirectory(render)
add_subdirectory(stud-config)
add_subdirectory(stud-ipc)
add_subdirectory(render-host)
add_subdirectory(tools)

# runtime/: the real replacement for the old jni-bridge+process-b pair.
# Real bionic ELF output, built with the Android NDK's own toolchain --
# same real reason as before (Process B's own code must be real
# bionic-ABI code to call directly into libroblox.so's real bionic JNI
# exports on the same thread/ABI, no glibc-forwarding trampoline).
set(STUD_ANDROID_NDK_ROOT "${STUD_THIRD_PARTY_DIR}/android-ndk-r28c" CACHE PATH
    "Path to the Android NDK used to build Stud's bionic runtime process")
if(EXISTS "${STUD_ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake")
    include(ExternalProject)
    ExternalProject_Add(runtime
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
        CMAKE_ARGS
            "-DCMAKE_TOOLCHAIN_FILE=${STUD_ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake"
            "-DANDROID_ABI=x86_64"
            "-DANDROID_PLATFORM=26"
            "-DMINIZ_SRC_DIR=${miniz_SOURCE_DIR}"
            "-DMINIZ_BIN_DIR=${miniz_BINARY_DIR}"
            "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
        BUILD_ALWAYS ON
        INSTALL_COMMAND ""
        DEPENDS stud_android_glue
    )
else()
    message(WARNING "Stud: NDK not found at ${STUD_ANDROID_NDK_ROOT} -- "
                     "the real bionic runtime process will not be built. "
                     "Set -DSTUD_ANDROID_NDK_ROOT=<path> to fix.")
endif()

if(STUD_BUILD_UI)
    add_subdirectory(ui)
    # The web-view panel. Gated with the UI because it is the same kind
    # of thing -- a desktop window built on Qt -- and a headless build
    # wants neither.
    add_subdirectory(webview)
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
# linuxdeploy bundling the same tree (packaging/build-appimage.sh).
#
# The layout is deliberately relocatable -- every binary finds bionic,
# ANGLE and its sibling processes relative to its own path -- because an
# AppImage is mounted somewhere different on every run, so nothing may be
# compiled in as absolute.
#
#   <prefix>/bin/stud                      the launcher
#   <prefix>/libexec/stud/                 the other two processes, the
#                                          viewer, and Process B's own
#                                          lib64/ overlay beside it
#   <prefix>/lib/stud/angle/               ANGLE
#   <prefix>/lib/stud/android-bionic/      the real linker and libc
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
# better, and several things that draw small icons -- a window's titlebar
# among them -- do not look for one at all if the theme has no entry at
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
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/stud")

# The dependencies setup.sh fetched. They are not Stud's own code, but a
# package has to carry them or the application cannot start: ANGLE is the
# GL translation layer, and the bionic set is the real linker and libc
# that load Roblox's engine. Both are redistributable -- ANGLE is BSD,
# bionic is Apache-2.0 from AOSP -- and their licences ship beside them.
if(EXISTS "${STUD_THIRD_PARTY_DIR}/angle")
    install(DIRECTORY "${STUD_THIRD_PARTY_DIR}/angle/"
            DESTINATION "${STUD_PRIVATE_LIB_DIR}/angle"
            USE_SOURCE_PERMISSIONS)
endif()
if(EXISTS "${STUD_THIRD_PARTY_DIR}/android-bionic")
    install(DIRECTORY "${STUD_THIRD_PARTY_DIR}/android-bionic/"
            DESTINATION "${STUD_PRIVATE_LIB_DIR}/android-bionic"
            USE_SOURCE_PERMISSIONS)
endif()

# Process B and the overlay libraries it binds over /system/lib64. Built
# by the nested NDK sub-build, so they are installed from its output
# directory rather than as CMake targets of this project.
set(STUD_RUNTIME_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/runtime-prefix/src/runtime-build")
install(PROGRAMS "${STUD_RUNTIME_BUILD_DIR}/stud-runtime-bionic"
        DESTINATION "${STUD_LIBEXEC_DIR}" OPTIONAL)
install(DIRECTORY "${STUD_RUNTIME_BUILD_DIR}/lib64/"
        DESTINATION "${STUD_LIBEXEC_DIR}/lib64"
        FILES_MATCHING PATTERN "*.so*")

include(packaging/Packaging.cmake)
