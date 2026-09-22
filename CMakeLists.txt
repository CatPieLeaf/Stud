cmake_minimum_required(VERSION 3.20)
project(Stud VERSION 1.1.7 LANGUAGES CXX C ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# RelWithDebInfo, not Debug. Stud is a renderer: every GL and Vulkan call
# the engine makes crosses this codebase, hundreds of times a frame, so
# an unoptimised build is not merely slower to develop against; it is
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
# Build ONLY the parts that link Qt, against whatever Qt is present, and
# take everything else from an existing install tree.
#
# This exists for the Flatpak. Copying a binary built on one distribution
# into a runtime carrying a different Qt is what made the Flatpak crash
# inside Qt's own startup before a line of Stud ran, so there, the Qt
# side is compiled in the runtime and the rest (ANGLE, the bionic
# runtime, the render host) is installed prebuilt, which needs no Qt and
# would otherwise cost an hour of ANGLE per build.
option(STUD_BUILD_QT_ONLY
       "Build only the Qt executables; skip the render host, the bionic runtime and ANGLE" OFF)
option(STUD_SANITIZE "Build with -fsanitize=address,undefined" OFF)
# Tracy, the frame profiler, OFF and not fetched unless asked for.
#
# For the bugs that a log cannot catch: a stall every few seconds, a hang
# on joining, a frame that takes 200ms once a minute. Statistical
# profilers average those away; Tracy shows the actual frame it happened
# in, on every thread at once, with the GPU timeline beside it.
#
# Never in a shipped build. The client opens a listening socket and
# keeps a buffer per thread, which is not something a released Stud
# should carry. This also replaces adding printf timers by hand, which
# is how most of this project's measurements have been taken so far and
# which perturbs the thing being measured.
option(STUD_TRACY "Build the render host with the Tracy profiler client" OFF)
# Whether a missing shader compiler is an error rather than a warning.
#
# Without glslc the upscaler is built with an empty shader array: FSR is
# offered in Settings and does nothing. That is a reasonable degrade for
# someone who only wants the rest of Stud, and unacceptable in a package
# so the release workflow turns this on and a plain build does not.
option(STUD_REQUIRE_SHADERS
       "Fail the configure if glslc is missing, instead of building without the upscaler" OFF)

# Hardening for the glibc-side binaries.
#
# Position-independent executables and immediate binding with a read-only
# GOT are what a distribution's own build flags supply by default, and an
# Arch packaging analyser reports their absence (`lacks PIE`, `lacks FULL
# RELRO`) on binaries built without them, which a prebuilt release is,
# since it never passes through makepkg's flags.
#
# Deliberately NOT applied to the bionic sub-build: that one is compiled
# with the Android NDK toolchain against a real bionic, and its linker
# flags are its own business.
# POSITION_INDEPENDENT_CODE alone only reaches the COMPILER (-fPIE); the
# link needs -pie as well, which CMP0083 is what makes CMake add. Without
# the policy the binaries still come out ET_EXEC and the analyser is
# right to say so, checked, before this line existed.
cmake_policy(SET CMP0083 NEW)
include(CheckPIESupported)
check_pie_supported(LANGUAGES CXX)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_link_options($<$<LINK_LANGUAGE:CXX>:-Wl,-z,relro,-z,now>)
option(STUD_ENABLE_DEV_RENDER_TOGGLE "Enable the prototype-only dev render-backend config toggle" ON)

# The application's identity, in one place.
#
# This is what a desktop entry is named, what AppStream keys its
# component on, and what both of Stud's windows report as their Wayland
# app_id, a compositor ties a window back to its launcher by matching
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

# Third-party dependencies Stud does not vendor and cannot redistribute:
# the Android NDK, the from-source ANGLE build, and a real extracted
# bionic (libc/linker64/...). tools/setup.sh puts all three here.
#
# One root, overridable, defaulting INSIDE the repo, an earlier layout
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
# this is "wiring libroblox"; it's OS/rendering plumbing, proven
# working via extensive live testing before the reset, and rebuilding
# it from scratch would be pure waste.

# Host-side jni.h only (JNIEnv/jobject types used by AAssetManager_
# fromJava/ANativeWindow_fromSurface's signatures in android-glue and
# render-host), the real jnivm/FakeJni JNIEnv *implementation* now
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

# Stud's own code is warning-clean under -Wextra; this dependency is not,
# and its headers are included by nearly every target here. Marking them
# SYSTEM keeps the raised floor below pointed at Stud rather than at
# somebody else's tree, and stops a dependency update from failing a build
# that turns warnings into errors.
foreach(jnivm_target jnivm fake-jni)
    if(TARGET ${jnivm_target})
        get_target_property(jnivm_includes ${jnivm_target} INTERFACE_INCLUDE_DIRECTORIES)
        if(jnivm_includes)
            set_target_properties(${jnivm_target} PROPERTIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${jnivm_includes}")
        endif()
    endif()
endforeach()

# The warning floor for Stud's own targets.
#
# Deliberately added here rather than per target: every directory below
# inherits it, and the bionic sub-build under runtime/ is a separate CMake
# project that does not, which is correct -- it compiles against a real
# bionic with the NDK's own flags.
add_compile_options(-Wextra)

# Defined BEFORE the subdirectories, because render-host looks for the
# NDK's own glslc through it to compile the upscale shaders. Declared
# after add_subdirectory(render-host) it was still empty at the point
# that search ran, so a FRESH configure never found glslc and silently
# built an upscaler with no shader in it, while an incremental
# reconfigure found it in the cache and worked. That is why every locally
# built package had FSR and the one CI produced did not.
set(STUD_ANDROID_NDK_ROOT "${STUD_THIRD_PARTY_DIR}/android-ndk-r28c" CACHE PATH
    "Path to the Android NDK used to build Stud's bionic runtime process")

# nlohmann_json, once, for every host-side target that wants it
# (stud-config, stud-ipc and render's dev toggle). Those three each used to
# declare it themselves, guarded, which is three places to bump a version
# in. The bionic sub-build under runtime/ is a separate CMake project and
# keeps its own declaration by necessity.
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

add_subdirectory(bionic-runtime)
add_subdirectory(cpu-compat)
add_subdirectory(android-glue)
add_subdirectory(render)
add_subdirectory(stud-config)
add_subdirectory(stud-ipc)
if(NOT STUD_BUILD_QT_ONLY)
    add_subdirectory(render-host)
    add_subdirectory(tools)
endif()

# runtime/: the real replacement for the old jni-bridge+process-b pair.
# Real bionic ELF output, built with the Android NDK's own toolchain,
# same real reason as before (Process B's own code must be real
# bionic-ABI code to call directly into libroblox.so's real bionic JNI
# exports on the same thread/ABI, no glibc-forwarding trampoline).
if(STUD_BUILD_QT_ONLY)
    # Supplied prebuilt; see STUD_BUILD_QT_ONLY.
elseif(EXISTS "${STUD_ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake")
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
    message(WARNING "Stud: NDK not found at ${STUD_ANDROID_NDK_ROOT}, "
                     "the real bionic runtime process will not be built. "
                     "Set -DSTUD_ANDROID_NDK_ROOT=<path> to fix.")
endif()

if(STUD_BUILD_UI)
    add_subdirectory(ui)
    # The web-view panel. Gated with the UI because it is the same kind
    # of thing, a desktop window built on Qt, and a headless build
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
# The layout is deliberately relocatable; every binary finds bionic,
# ANGLE and its sibling processes relative to its own path, because an
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
# It is compressed here rather than left to the packaging tools: rpm's own
# brp scripts would do it, but Stud switches every one of them off
# (__os_install_post) to keep them from stripping a bionic ELF, so the
# page would ship uncompressed. Doing it once suits every format, man
# reads a .gz directly, and dpkg and makepkg both expect one.
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
# The three shader references Stud's upscalers are built from. All three
# notices travel with the build output, not just with the source tree,
# because what ships is compiled SPIR-V: a user who has the package has
# the work without having the repository.
#
# The mpv one is the one that must not be missed. FSR1 is MIT and SGSR
# is BSD-3-Clause, both of which ask only that the notice accompany the
# distribution; RAVU-Zoom is LGPL-3.0-or-later, which is copyleft, and
# AGPLv3 section 13 -- the clause that lets Stud combine with it at all
# -- requires the Corresponding Source to travel too. The hook the
# shader and its weight tables are generated from is that source, and it
# is installed here beside the licence rather than only living in the
# repository.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx-fsr1/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/stud/fidelityfx-fsr1"
        RENAME "LICENSE-FidelityFX-FSR1.txt")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/snapdragon-gsr/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/stud/snapdragon-gsr"
        RENAME "LICENSE-Snapdragon-GSR.txt")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpv-prescalers/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/stud/mpv-prescalers"
        RENAME "LICENSE-mpv-prescalers.txt")
install(FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpv-prescalers/ravu-zoom-ar-r2-rgb.hook"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpv-prescalers/ravu-zoom-r2-rgb.hook"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/stud/mpv-prescalers")

# The four libraries compiled into Stud's own binaries. Nothing in the
# package is a separate file for any of them, which is exactly why their
# notices have to be installed: a user with the package has the code
# without having this tree.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bc7enc/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/stud/bc7enc"
        RENAME "LICENSE-bc7enc_rdo.txt")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/volk/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/stud/volk"
        RENAME "LICENSE-volk.txt")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xxhash/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/stud/xxhash"
        RENAME "LICENSE-xxHash.txt")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/stud/miniaudio"
        RENAME "LICENSE-miniaudio.txt")

# The dependencies setup.sh fetched. They are not Stud's own code, but a
# package has to carry them or the application cannot start: ANGLE is the
# GL translation layer, and the bionic set is the real linker and libc
# that load Roblox's engine. Both are redistributable; ANGLE is BSD,
# the bionic set is BSD and Apache-2.0 from AOSP, and both licences
# require their text and copyright to travel with the binaries.
#
# `tools/setup.sh` puts them in a `licenses/` directory beside each set,
# so installing the directory carries them; they are ALSO installed under
# share/licenses/stud/, which is where a distribution looks. If setup
# could not fetch them it says so loudly, a package built from a tree
# without them is one that cannot legally be distributed.
set(STUD_PREBUILT_DEPS angle android-bionic)
if(STUD_BUILD_QT_ONLY)
    set(STUD_PREBUILT_DEPS)  # supplied prebuilt, so nothing to install here
endif()
if(NOT STUD_BUILD_QT_ONLY AND EXISTS "${STUD_THIRD_PARTY_DIR}/angle")
    install(DIRECTORY "${STUD_THIRD_PARTY_DIR}/angle/"
            DESTINATION "${STUD_PRIVATE_LIB_DIR}/angle"
            USE_SOURCE_PERMISSIONS)
endif()
if(NOT STUD_BUILD_QT_ONLY AND EXISTS "${STUD_THIRD_PARTY_DIR}/android-bionic")
    install(DIRECTORY "${STUD_THIRD_PARTY_DIR}/android-bionic/"
            DESTINATION "${STUD_PRIVATE_LIB_DIR}/android-bionic"
            USE_SOURCE_PERMISSIONS)
endif()
foreach(stud_dep ${STUD_PREBUILT_DEPS})
    if(EXISTS "${STUD_THIRD_PARTY_DIR}/${stud_dep}/licenses")
        install(DIRECTORY "${STUD_THIRD_PARTY_DIR}/${stud_dep}/licenses/"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/stud/${stud_dep}")
    else()
        message(WARNING
            "no ${STUD_THIRD_PARTY_DIR}/${stud_dep}/licenses, a package built from "
            "this tree would redistribute ${stud_dep} with no licence text. "
            "Run tools/setup.sh to fetch them.")
    endif()
endforeach()

# Process B and the overlay libraries it binds over /system/lib64. Built
# by the nested NDK sub-build, so they are installed from its output
# directory rather than as CMake targets of this project.
set(STUD_RUNTIME_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/runtime-prefix/src/runtime-build")
if(NOT STUD_BUILD_QT_ONLY)
install(PROGRAMS "${STUD_RUNTIME_BUILD_DIR}/stud-runtime-bionic"
        DESTINATION "${STUD_LIBEXEC_DIR}" OPTIONAL)
install(DIRECTORY "${STUD_RUNTIME_BUILD_DIR}/lib64/"
        DESTINATION "${STUD_LIBEXEC_DIR}/lib64"
        FILE_PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE
        FILES_MATCHING PATTERN "*.so*")
endif()

include(packaging/Packaging.cmake)
