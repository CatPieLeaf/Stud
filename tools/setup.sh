#!/usr/bin/env bash
#
# Fetch the three third-party dependencies Stud needs and cannot ship:
#
#   third_party/android-ndk-r28c/  the NDK that builds Process B, which is
#                                  a real bionic ELF, not a glibc one
#   third_party/angle/             libEGL.so + libGLESv2.so from a real
#                                  ANGLE build, plus the ICD files its
#                                  Vulkan and SwiftShader backends load
#   third_party/android-bionic/    a real linker64/libc.so/... taken from
#                                  AOSP's own prebuilt Runtime APEX, plus
#                                  liblog built from AOSP source
#
# None of these are redistributable from this repository. The NDK is a
# Google redistributable with its own licence, ANGLE is BSD but is built
# here rather than vendored (the build is large and machine-specific),
# and bionic is Android's own, fetched from AOSP the way AOSP publishes
# it rather than copied out of anyone's device.
#
# Everything lands under third_party/, which is gitignored, and which is
# where CMake looks by default (STUD_THIRD_PARTY_DIR).
#
# Usage:
#   tools/setup.sh                     everything that is missing
#   tools/setup.sh ndk angle bionic    only the named ones
#   tools/setup.sh --plan              say what it would fetch, fetch nothing
#   tools/setup.sh --help
#
# Pointing it at things you already have (no download, no build):
#   STUD_NDK_SRC=/path/to/android-ndk-r28c
#   STUD_ANGLE_SRC=/path/to/angle/out/Release
#   STUD_BIONIC_SRC=/path/to/dir/with/linker64+libc.so

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
third_party="${STUD_THIRD_PARTY_DIR:-$repo_root/third_party}"

NDK_VERSION="r28c"
NDK_URL="https://dl.google.com/android/repository/android-ndk-${NDK_VERSION}-linux.zip"
NDK_DIR="$third_party/android-ndk-${NDK_VERSION}"
ANGLE_DIR="$third_party/angle"
ANGLE_SRC_DIR="$third_party/angle-src"
DEPOT_TOOLS_DIR="$third_party/depot_tools"
BIONIC_DIR="$third_party/android-bionic"

# The real files Process B's sandbox needs. bionic_runtime.cpp checks for
# this exact set (minus linker64, which it takes as PT_INTERP), so a
# partial extraction fails loudly at launch rather than halfway through a
# dlopen.
BIONIC_FILES=(
    linker64 libc.so libm.so libdl.so libdl_android.so libc++.so liblog.so
)

# Taken when a source has them, never required: Stud builds its own and
# binds them over /system/lib64 inside the sandbox, so whatever a real
# Android has under these names is shadowed and unused. ld-android.so is
# here for a different reason -- the linker answers that soname from
# itself, so the file is never opened.
BIONIC_OPTIONAL_FILES=(
    ld-android.so libandroid.so libmediandk.so libnativewindow.so
    libEGL.so libGLESv2.so libvulkan.so.1 libaaudio.so
    libOpenSLES.so libOpenMAXAL.so libwilhelm.so
)

# Which AOSP release everything bionic-side comes from. Android 15 is
# SDK 35, which is the newest the pinned NDK can compile against --
# AOSP main is already on 37, and liblog there uses symbols no released
# NDK has.
AOSP_BRANCH="${STUD_AOSP_BRANCH:-android15-release}"
AOSP="https://android.googlesource.com"

say()  { printf '\033[1msetup:\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[1;33msetup:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31msetup:\033[0m %s\n' "$*" >&2; exit 1; }

need() {
    command -v "$1" >/dev/null 2>&1 || die "$1 is required for this step but is not installed"
}

usage() {
    sed -n '3,32p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
}

# ---------------------------------------------------------------- NDK

setup_ndk() {
    if [ -f "$NDK_DIR/build/cmake/android.toolchain.cmake" ]; then
        say "NDK already at $NDK_DIR"
        return
    fi
    if [ -n "${STUD_NDK_SRC:-}" ]; then
        [ -f "$STUD_NDK_SRC/build/cmake/android.toolchain.cmake" ] ||
            die "STUD_NDK_SRC=$STUD_NDK_SRC does not look like an NDK"
        say "linking NDK from $STUD_NDK_SRC"
        ln -sfn "$(cd "$STUD_NDK_SRC" && pwd)" "$NDK_DIR"
        return
    fi

    need curl; need unzip
    local zip="$third_party/android-ndk-${NDK_VERSION}-linux.zip"
    say "downloading the NDK ${NDK_VERSION} (about 700 MB)"
    curl -fL --progress-bar -o "$zip.part" "$NDK_URL"
    mv "$zip.part" "$zip"
    say "unpacking"
    unzip -q "$zip" -d "$third_party"
    rm -f "$zip"
    [ -f "$NDK_DIR/build/cmake/android.toolchain.cmake" ] ||
        die "the NDK unpacked but $NDK_DIR/build/cmake/android.toolchain.cmake is missing"
    say "NDK ready"
}

# -------------------------------------------------------------- ANGLE

# Stud loads ANGLE's own libEGL/libGLESv2 in the render host, never the
# system ones -- the whole point is that the translation layer is a known
# quantity. These are the files it actually opens at runtime; the rest of
# an ANGLE build tree (24 GB of source and objects) is not needed.
ANGLE_RUNTIME_FILES=(
    libEGL.so libGLESv2.so libvulkan.so.1 libvk_swiftshader.so
    vk_swiftshader_icd.json libVkICD_mock_icd.so libVkLayer_khronos_validation.so
)

copy_angle_runtime() {
    local from="$1"
    [ -f "$from/libEGL.so" ] && [ -f "$from/libGLESv2.so" ] ||
        die "$from has no libEGL.so/libGLESv2.so"
    mkdir -p "$ANGLE_DIR"
    local f
    for f in "${ANGLE_RUNTIME_FILES[@]}"; do
        [ -e "$from/$f" ] && cp -a "$from/$f" "$ANGLE_DIR/"
    done
    # ANGLE's Vulkan backend reads its own data directory next to the
    # libraries when one is present.
    [ -d "$from/angledata" ] && cp -a "$from/angledata" "$ANGLE_DIR/"
    say "ANGLE runtime in $ANGLE_DIR"
}

setup_angle() {
    if [ -f "$ANGLE_DIR/libEGL.so" ] && [ -f "$ANGLE_DIR/libGLESv2.so" ]; then
        say "ANGLE already at $ANGLE_DIR"
        return
    fi
    if [ -n "${STUD_ANGLE_SRC:-}" ]; then
        say "copying ANGLE from $STUD_ANGLE_SRC"
        copy_angle_runtime "$STUD_ANGLE_SRC"
        return
    fi

    warn "no prebuilt ANGLE given (STUD_ANGLE_SRC) -- building from source."
    warn "this downloads several GB and takes upwards of an hour."
    need git; need python3

    if [ ! -d "$DEPOT_TOOLS_DIR" ]; then
        say "fetching depot_tools"
        git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git \
            "$DEPOT_TOOLS_DIR"
    fi

    # depot_tools ships its own Python and CIPD packages, and unpacks them
    # the first time one of its commands runs. DEPOT_TOOLS_UPDATE=0 below
    # switches that off -- which is what we want for the self-update, and
    # not what we want for the very first unpack. On a machine where
    # depot_tools had been used before, this had already happened and the
    # difference never showed; on a clean checkout it fails outright:
    #
    #   python3_bin_reldir.txt not found. need to initialize depot_tools
    #   by running gclient, update_depot_tools or ensure_bootstrap.
    #
    # Caught by a CI run, which is a clean checkout every time.
    # ensure_bootstrap is depot_tools' own answer, and it is idempotent.
    if [ ! -f "$DEPOT_TOOLS_DIR/python3_bin_reldir.txt" ]; then
        say "bootstrapping depot_tools"
        (cd "$DEPOT_TOOLS_DIR" && ./ensure_bootstrap) ||
            warn "depot_tools bootstrap reported an error -- continuing, since it may still have done enough"
        [ -f "$DEPOT_TOOLS_DIR/python3_bin_reldir.txt" ] ||
            die "depot_tools did not bootstrap; ANGLE cannot be built from source here"
    fi

    export PATH="$DEPOT_TOOLS_DIR:$PATH"
    export DEPOT_TOOLS_UPDATE=0

    mkdir -p "$ANGLE_SRC_DIR"
    if [ ! -d "$ANGLE_SRC_DIR/angle" ]; then
        say "fetching ANGLE (this is the slow part)"
        (cd "$ANGLE_SRC_DIR" && fetch --nohooks angle && cd angle && python3 scripts/bootstrap.py)
    fi
    (
        cd "$ANGLE_SRC_DIR/angle"
        gclient sync -D
        # Matches the build this project has been developed and measured
        # against: release, Vulkan and desktop-GL backends both enabled
        # (the Settings window offers both), no tests, static.
        gn gen out/Release --args='is_debug = false
angle_enable_vulkan = true
angle_enable_gl = true
angle_enable_null = false
angle_build_tests = false
is_component_build = false'
        autoninja -C out/Release libEGL libGLESv2 vk_swiftshader
    )
    copy_angle_runtime "$ANGLE_SRC_DIR/angle/out/Release"
}

# ------------------------------------------------------------- bionic

have_all_bionic() {
    local f
    for f in "${BIONIC_FILES[@]}" tzdata; do
        [ -f "$BIONIC_DIR/$f" ] || return 1
    done
    return 0
}

copy_bionic_from_dir() {
    local from="$1"
    mkdir -p "$BIONIC_DIR"
    local f
    for f in "${BIONIC_OPTIONAL_FILES[@]}"; do
        [ -f "$from/$f" ] && cp -a "$from/$f" "$BIONIC_DIR/"
    done
    local missing=0
    for f in "${BIONIC_FILES[@]}" tzdata; do
        if [ -f "$from/$f" ]; then
            cp -a "$from/$f" "$BIONIC_DIR/"
        else
            warn "missing from $from: $f"
            missing=1
        fi
    done
    [ "$missing" = 0 ] || warn "some files were missing -- Stud will say so at launch if it needs one"
    [ -f "$BIONIC_DIR/linker64" ] && chmod 0755 "$BIONIC_DIR/linker64"
    say "bionic in $BIONIC_DIR"
    verify_bionic || true
}

# Where bionic comes from: AOSP itself.
#
# Google publishes the Runtime APEX -- the module that IS bionic on a
# modern Android -- as a prebuilt inside the AOSP tree, per architecture,
# per release branch. It is 13 MB and holds exactly what Stud needs:
# bin/linker64, lib64/bionic/{libc,libm,libdl,libdl_android}.so and
# lib64/libc++.so, as one self-consistent set. tzdata ships the same way
# in its own APEX. An APEX is a zip around an ext4 image, so debugfs
# reads it -- no root, no loop mount, and nothing to unpack but 13 MB.
#
# Since Android 10 none of this is in /system/lib64 any more: those paths
# are symlinks into the APEX, which is why pulling bionic out of a system
# image is the harder, larger and less exact way to do it.
#
# liblog is the one piece not in either APEX (it is a platform library,
# not a module), so it is built here from its own AOSP source with the
# NDK -- ~14 files, AOSP's own cflags and version script.
RUNTIME_APEX_PATH="mainline/runtime/apex/com.android.runtime-x86_64.apex"
TZDATA_APEX_PATH="mainline/tzdata/apex/com.android.tzdata-x86_64.apex"

# Downloads one file out of a googlesource repo and checks it really is
# the file that repo lists.
#
# There is no published checksum for these, but git is content-addressed:
# the tree listing gives each blob's object id, and `git hash-object` on
# what arrived has to reproduce it. That is a stronger check than a
# hash this script could carry, since it is the repository's own.
aosp_blob() {
    local repo="$1" dir="$2" name="$3" dest="$4"
    need curl; need git

    local want
    want="$(curl -fsSL "$AOSP/$repo/+/refs/heads/$AOSP_BRANCH/$dir/?format=TEXT" \
            | base64 -d 2>/dev/null | awk -v n="$name" '$4 == n { print $3 }')"
    [ -n "$want" ] || { warn "$name is not in $repo at $AOSP_BRANCH"; return 1; }

    curl -fsSL "$AOSP/$repo/+/refs/heads/$AOSP_BRANCH/$dir/$name?format=TEXT" \
        | base64 -d > "$dest" || { warn "could not download $name"; return 1; }

    local got; got="$(git hash-object "$dest")"
    if [ "$got" != "$want" ]; then
        rm -f "$dest"
        warn "$name does not match what AOSP lists (want $want, got $got)"
        return 1
    fi
}

# An APEX is a zip whose apex_payload.img is an ext4 filesystem.
apex_payload() {
    local apex="$1" out="$2"
    need unzip
    rm -rf "$out"; mkdir -p "$out"
    unzip -q -o "$apex" apex_payload.img -d "$out" || return 1
    [ -s "$out/apex_payload.img" ] || return 1
}

# liblog, built from AOSP source. The flags are AOSP's own, copied from
# system/logging/liblog/Android.bp, including the version script -- so
# the result exports the same surface a device's does.
#
# -static-libstdc++ because the NDK's libc++ and the platform's are
# deliberately ABI-incompatible (inline namespace __ndk1 vs __1), so this
# cannot link against the libc++.so shipped above, and must not drag in
# libc++_shared.so, which nothing in the sandbox provides.
build_liblog() {
    local work="$third_party/.liblog-src"
    local clang="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++"
    local strip="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
    if [ ! -x "$clang" ]; then
        warn "no NDK at $NDK_DIR -- run 'tools/setup.sh ndk' first"
        return 1
    fi
    need curl; need tar

    rm -rf "$work"; mkdir -p "$work/src" "$work/inc"
    say "fetching liblog source from AOSP ($AOSP_BRANCH)"
    curl -fsSL "$AOSP/platform/system/logging/+archive/refs/heads/$AOSP_BRANCH/liblog.tar.gz" \
        | tar xz -C "$work/src" || { warn "could not fetch liblog source"; return 1; }

    # Header-only dependencies, named by liblog's own header_libs.
    local d
    for d in "system/libbase:include:base" \
             "system/core:libcutils/include:cutils" \
             "system/core:libutils/include:utils" \
             "system/core:libsystem/include:system"; do
        # Separate statements on purpose: `local a=.. b=$a` expands every
        # argument before it assigns any of them, so b would be unbound.
        local repo="${d%%:*}"
        local rest="${d#*:}"
        local path="${rest%%:*}"
        local into="${rest##*:}"
        mkdir -p "$work/inc/$into"
        curl -fsSL "$AOSP/platform/$repo/+archive/refs/heads/$AOSP_BRANCH/$path.tar.gz" \
            | tar xz -C "$work/inc/$into" 2>/dev/null || true
    done
    # utils/Errors.h is a symlink out of that archive into libutils/binder.
    if [ -L "$work/inc/utils/utils/Errors.h" ]; then
        mkdir -p "$work/inc/binder"
        curl -fsSL "$AOSP/platform/system/core/+archive/refs/heads/$AOSP_BRANCH/libutils/binder/include.tar.gz" \
            | tar xz -C "$work/inc/binder" 2>/dev/null || true
        rm -f "$work/inc/utils/utils/Errors.h"
        cp "$work/inc/binder/utils/Errors.h" "$work/inc/utils/utils/Errors.h" 2>/dev/null || true
    fi

    say "building liblog with the NDK"
    # --no-undefined so a missing piece is a link error here rather than a
    # dlopen failure inside the sandbox much later.
    "$clang" --target=x86_64-linux-android35 -shared -fPIC -O2 \
        -static-libstdc++ -ffunction-sections -fdata-sections \
        -o "$BIONIC_DIR/liblog.so" "$work"/src/*.cpp \
        -I "$work/src/include" -I "$work/inc/base" -I "$work/inc/cutils" \
        -I "$work/inc/utils" -I "$work/inc/system" \
        -DLIBLOG_LOG_TAG=1006 -DSNET_EVENT_LOG_TAG=1397638484 -DANDROID_DEBUGGABLE=0 \
        -Wl,--version-script="$work/src/liblog.map.txt" \
        -Wl,-soname,liblog.so -Wl,--no-undefined -Wl,--gc-sections -w \
        || { warn "liblog did not build"; return 1; }
    [ -x "$strip" ] && "$strip" "$BIONIC_DIR/liblog.so"
    rm -rf "$work"
}

fetch_bionic_from_aosp() {
    mkdir -p "$BIONIC_DIR"
    local work="$third_party/.apex"
    rm -rf "$work"; mkdir -p "$work"

    say "fetching the Runtime APEX from AOSP ($AOSP_BRANCH, 13 MB)"
    aosp_blob platform/prebuilts/runtime "$(dirname "$RUNTIME_APEX_PATH")" \
        "$(basename "$RUNTIME_APEX_PATH")" "$work/runtime.apex" || return 1
    apex_payload "$work/runtime.apex" "$work/rt" || { warn "bad Runtime APEX"; return 1; }

    need debugfs
    local f src
    for f in "${BIONIC_FILES[@]}"; do
        [ "$f" = liblog.so ] && continue          # built from source below
        case "$f" in
            linker64) src="/bin/linker64" ;;
            libc++.so) src="/lib64/libc++.so" ;;
            *)        src="/lib64/bionic/$f" ;;
        esac
        debugfs -R "dump $src $BIONIC_DIR/$f" "$work/rt/apex_payload.img" >/dev/null 2>&1 || true
        [ -s "$BIONIC_DIR/$f" ] || { warn "not in the Runtime APEX: $src"; return 1; }
    done
    debugfs -R "dump /lib64/ld-android.so $BIONIC_DIR/ld-android.so" \
        "$work/rt/apex_payload.img" >/dev/null 2>&1 || true
    chmod 0755 "$BIONIC_DIR/linker64"

    say "fetching the tzdata APEX from AOSP (1 MB)"
    aosp_blob platform/prebuilts/runtime "$(dirname "$TZDATA_APEX_PATH")" \
        "$(basename "$TZDATA_APEX_PATH")" "$work/tzdata.apex" || return 1
    apex_payload "$work/tzdata.apex" "$work/tz" || { warn "bad tzdata APEX"; return 1; }
    debugfs -R "dump /etc/tz/tzdata $BIONIC_DIR/tzdata" \
        "$work/tz/apex_payload.img" >/dev/null 2>&1 || true
    [ -s "$BIONIC_DIR/tzdata" ] || { warn "no tzdata in the tzdata APEX"; return 1; }

    rm -rf "$work"
    build_liblog || return 1
    say "bionic assembled from AOSP $AOSP_BRANCH in $BIONIC_DIR"
}

# What Stud actually resolves out of these libraries at runtime, by name.
# An image from the wrong Android version can be missing one of them and
# the failure would otherwise be a puzzling runtime one -- DNS silently
# not resolving, or the property area never mapping -- a long way from
# the image that caused it.
verify_bionic() {
    local nm=""
    command -v nm >/dev/null 2>&1 && nm="nm -D --defined-only"
    if [ -z "$nm" ] && command -v readelf >/dev/null 2>&1; then nm="readelf -W --dyn-syms"; fi
    if [ -z "$nm" ]; then
        warn "cannot list the extracted libc's exported symbols -- skipping the check"
        return 0
    fi

    local libc_syms missing=""
    libc_syms="$($nm "$BIONIC_DIR/libc.so" 2>/dev/null || true)"
    # Plain substring match on purpose: these names are unique, and a real
    # bionic symbol carries a version suffix (__system_properties_init@@
    # LIBC_Q) that an anchored pattern would miss.
    local sym
    for sym in __system_properties_init _resolv_set_nameservers_for_net; do
        grep -q "$sym" <<<"$libc_syms" || missing="$missing $sym"
    done
    local log_syms
    log_syms="$($nm "$BIONIC_DIR/liblog.so" 2>/dev/null || true)"
    for sym in __android_log_set_logger __android_log_stderr_logger; do
        grep -q "$sym" <<<"$log_syms" || missing="$missing $sym"
    done

    if [ -n "$missing" ]; then
        warn "this bionic does not export:$missing"
        warn "Stud resolves those by name at runtime -- DNS or the property"
        warn "area will not work against this image. Try a different build"
        warn "(STUD_AOSP_BRANCH=<branch>) or a different source."
        return 1
    fi
    say "checked: the extracted bionic exports what Stud resolves by name"
}

setup_bionic() {
    if have_all_bionic; then
        say "bionic already at $BIONIC_DIR"
        return
    fi
    if [ -n "${STUD_BIONIC_SRC:-}" ]; then
        say "copying bionic from $STUD_BIONIC_SRC"
        copy_bionic_from_dir "$STUD_BIONIC_SRC"
        return
    fi

    fetch_bionic_from_aosp && { verify_bionic || true; return; }

    cat >&2 <<EOF
setup: could not assemble bionic from AOSP.

Stud runs the real Roblox Android library against a real bionic, and
takes it from AOSP's own prebuilt Runtime APEX -- that download is what
just failed. Check the network, or point this at a directory that
already holds the libraries:

    STUD_BIONIC_SRC=/path/to/dir tools/setup.sh bionic

That directory needs: ${BIONIC_FILES[*]} tzdata
EOF
    return 1
}

# ----------------------------------------------------------------- go

case "${1:-}" in -h|--help) usage ;; esac

steps=()
plan_only=0
for arg in "$@"; do
    case "$arg" in
        -n|--plan|--dry-run) plan_only=1 ;;
        *) steps+=("$arg") ;;
    esac
done
[ ${#steps[@]} -eq 0 ] && steps=(ndk angle bionic)

# Say what this will fetch, where from, and how big, before fetching any
# of it -- these are large downloads and one of them is a source build.
print_plan() {
    printf '\n\033[1mStud setup plan\033[0m -- everything lands in %s\n\n' "$third_party"
    local step
    for step in "${steps[@]}"; do
        case "$step" in
            ndk)
                echo "  android-ndk-${NDK_VERSION}/            ~700 MB download, 2.2 GB unpacked"
                echo "      from ${NDK_URL}"
                echo "      builds Process B, which is a real bionic ELF"
                [ -n "${STUD_NDK_SRC:-}" ] && echo "      SKIPPED: linking your copy at $STUD_NDK_SRC"
                [ -f "$NDK_DIR/build/cmake/android.toolchain.cmake" ] && echo "      SKIPPED: already present"
                ;;
            angle)
                echo "  angle/                        55 MB of built libraries"
                if [ -n "${STUD_ANGLE_SRC:-}" ]; then
                    echo "      copied from $STUD_ANGLE_SRC"
                elif [ -f "$ANGLE_DIR/libEGL.so" ]; then
                    echo "      SKIPPED: already present"
                else
                    echo "      BUILT FROM SOURCE: depot_tools + an ANGLE checkout,"
                    echo "      several GB downloaded and upwards of an hour of compiling."
                    echo "      Set STUD_ANGLE_SRC=<dir with libEGL.so/libGLESv2.so> to skip it."
                fi
                echo "      the GL translation layer the render host runs on"
                ;;
            bionic)
                echo "  android-bionic/               ~14 MB downloaded"
                if [ -n "${STUD_BIONIC_SRC:-}" ]; then
                    echo "      copied from $STUD_BIONIC_SRC"
                elif have_all_bionic; then
                    echo "      SKIPPED: already present"
                else
                    echo "      from AOSP $AOSP_BRANCH:"
                    echo "        $AOSP/platform/prebuilts/runtime"
                    echo "          the prebuilt Runtime APEX (bionic itself) and tzdata,"
                    echo "          each checked against the blob id AOSP lists"
                    echo "        $AOSP/platform/system/logging"
                    echo "          liblog source, built here with the NDK"
                    echo "      (nothing Android is redistributed by this repository)"
                fi
                echo "      the real linker and libc that load libroblox.so"
                ;;
        esac
        echo
    done
}

print_plan
if [ "$plan_only" = 1 ]; then
    say "plan only, nothing fetched (drop --plan to run it)"
    exit 0
fi

mkdir -p "$third_party"

failed=0
for step in "${steps[@]}"; do
    case "$step" in
        ndk)    setup_ndk ;;
        angle)  setup_angle ;;
        bionic) setup_bionic || failed=1 ;;
        *)      die "unknown step '$step' (expected: ndk, angle, bionic)" ;;
    esac
done

if [ "$failed" = 0 ]; then
    say "done -- now: cmake -S . -B build -G Ninja && cmake --build build"
else
    warn "finished with something missing (see above); Stud will not run until it is there"
fi
exit "$failed"
