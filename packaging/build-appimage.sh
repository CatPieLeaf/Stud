#!/usr/bin/env bash
#
# Builds Stud as an AppImage: one file that runs on any distribution
# without installing anything.
#
# It bundles what a distribution's package would depend on; Qt, the
# Vulkan loader, and the libraries those pull in, alongside what Stud
# ships either way: ANGLE, the bionic set, and Process B.
#
# Usage:
#   packaging/build-appimage.sh [build-dir]
#
# The build directory defaults to ./build and must already be configured
# and built (tools/setup.sh, cmake, cmake --build). linuxdeploy and its
# Qt plugin are downloaded on first run and cached next to it.
#
# bubblewrap is deliberately NOT bundled. It needs the AppArmor or
# SELinux policy that a distribution ships with its own build to create a
# user namespace at all, so a copied-in binary would be refused on the
# distributions that matter. The AppImage checks for it and says so.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Unless we are already inside it, the whole thing runs in a container
# built from packaging/Containerfile.appimage. See that file for why:
# an AppImage has to be built against libraries at least as old as the
# oldest machine meant to run it, and linuxdeploy's own patchelf
# corrupts libraries from a current distribution outright.
#
# STUD_APPIMAGE_NO_CONTAINER=1 builds on this machine instead, which is
# useful for iterating on the script and produces an image that will only
# run on machines as new as this one.
if [ "${STUD_APPIMAGE_IN_CONTAINER:-0}" != 1 ] && [ "${STUD_APPIMAGE_NO_CONTAINER:-0}" != 1 ]; then
    engine=""
    for candidate in podman docker; do
        command -v "$candidate" >/dev/null 2>&1 && { engine="$candidate"; break; }
    done
    if [ -z "$engine" ]; then
        echo "appimage: podman or docker is needed to build in a container." >&2
        echo "appimage: STUD_APPIMAGE_NO_CONTAINER=1 builds here instead, but the" >&2
        echo "appimage: result will only run on machines as new as this one." >&2
        exit 1
    fi
    printf '\033[1mappimage:\033[0m building the image with %s\n' "$engine" >&2
    "$engine" build -f "$repo_root/packaging/Containerfile.appimage" \
        -t stud-appimage-build "$repo_root" >&2
    # tools/setup.sh links rather than copies anything it was pointed at
    # (STUD_NDK_SRC and friends), so an entry of third_party/ can be a
    # symlink out of the repository, which resolves to nothing inside a
    # container that only has the repository mounted, and the build then
    # quietly produces no Process B. Mount each such target at its own
    # path so the link resolves there too.
    link_mounts=()
    for entry in "$repo_root"/third_party/*; do
        [ -L "$entry" ] || continue
        target="$(readlink -f "$entry")" || continue
        case "$target" in "$repo_root"/*) continue;; esac
        [ -e "$target" ] && link_mounts+=(-v "$target:$target:ro,z")
    done

    printf '\033[1mappimage:\033[0m building in the container\n' >&2
    exec "$engine" run --rm \
        -v "$repo_root:/src:z" \
        "${link_mounts[@]+"${link_mounts[@]}"}" \
        -e STUD_APPIMAGE_IN_CONTAINER=1 \
        -e APPIMAGE_EXTRACT_AND_RUN=1 \
        stud-appimage-build \
        /src/packaging/build-appimage.sh "${@:-}"
fi
# Inside the container Stud is built there too: a build tree from the
# host was linked against the host's libraries, which is exactly what
# building in a container exists to avoid.
if [ "${STUD_APPIMAGE_IN_CONTAINER:-0}" = 1 ]; then
    build_dir="${1:-$repo_root/build-appimage}"
    # Configured every time, not only when there is no cache. Whether
    # Process B, ANGLE and bionic are built at all is decided at
    # configure time from what third_party/ holds, so a tree configured
    # before tools/setup.sh ran keeps producing an image without them
    # until it is configured again.
    printf '\033[1mappimage:\033[0m configuring\n' >&2
    # STUD_CMAKE_ARGS passes anything else through, release automation
    # uses it to set -DSTUD_VERSION from the tag being built, so the
    # AppImage's own metadata says what the release says.
    # shellcheck disable=SC2086
    cmake -S "$repo_root" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        ${STUD_CMAKE_ARGS:-} >&2
    printf '\033[1mappimage:\033[0m building\n' >&2
    cmake --build "$build_dir" -j"$(nproc)" >&2
else
    build_dir="${1:-$repo_root/build}"
fi
work="$build_dir/appimage"
appdir="$work/AppDir"
tools="${STUD_APPIMAGE_TOOLS:-$repo_root/third_party/appimage-tools}"

say() { printf '\033[1mappimage:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31mappimage:\033[0m %s\n' "$*" >&2; exit 1; }

[ -d "$build_dir" ] || die "no build directory at $build_dir, configure and build first"

app_id="$(grep -m1 '^set(STUD_APP_ID' "$repo_root/CMakeLists.txt" | cut -d'"' -f2)"
[ -n "$app_id" ] || die "could not read STUD_APP_ID out of CMakeLists.txt"

# linuxdeploy, and its Qt plugin, which is what knows how to bring in the
# platform plugins, the QPA bits and QtWebEngine's own helper process.
mkdir -p "$tools"
fetch_tool() {
    local name="$1" url="$2"
    if [ ! -x "$tools/$name" ]; then
        say "downloading $name"
        curl -fL --progress-bar -o "$tools/$name" "$url"
        chmod +x "$tools/$name"
    fi
}
fetch_tool linuxdeploy \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
fetch_tool linuxdeploy-plugin-qt \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"
# The image is packed with appimagetool directly rather than through
# linuxdeploy's output plugin. linuxdeploy re-scans and re-resolves the
# whole AppDir whenever it runs, so asking it to pack would undo the
# point of having moved Stud's own libraries out of its way.
fetch_tool appimagetool \
    "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"

rm -rf "$appdir"
say "installing into an AppDir"
DESTDIR="$appdir" cmake --install "$build_dir" --prefix /usr >/dev/null

# linuxdeploy wants to be told the entry point, the icon and the desktop
# entry; everything else it works out from the binary itself.
[ -x "$appdir/usr/bin/stud" ] || die "the install produced no usr/bin/stud"

# Everything Stud cannot run without, checked before anything is packed.
#
# A build with third_party/ missing configures, compiles and links
# perfectly well. It just silently produces no Process B, no ANGLE and
# no bionic, because those targets and install rules are conditional on
# the dependency being present. The result is an AppImage that starts,
# shows its window, and cannot run Roblox at all. That is exactly the
# kind of failure that looks like success in a log, so it is an error
# here rather than a warning.
for required in \
    usr/libexec/stud/stud-runtime-bionic \
    usr/lib/stud/angle/libEGL.so \
    usr/lib/stud/android-bionic/linker64 \
    usr/lib/stud/android-bionic/libc.so
do
    [ -e "$appdir/$required" ] ||
        die "the install produced no $required, run tools/setup.sh first"
done

# The helper processes are not on PATH inside an AppImage, and
# linuxdeploy only follows the libraries of what it is given, so each
# one is passed explicitly to have its dependencies bundled too.
extra_exes=()
for exe in stud-render-host stud-webview; do
    [ -f "$appdir/usr/libexec/stud/$exe" ] && extra_exes+=("--executable=$appdir/usr/libexec/stud/$exe")
done

export QMAKE="${QMAKE:-qmake6}"

# linuxdeploy ships its own binutils, and that copy is older than the
# relocation format a current distribution's Qt is built with; it
# refuses every library with a .relr.dyn section, which on Fedora is all
# of them, and the Qt plugin then fails outright. Stripping only saves
# space, so it is turned off rather than worked around.
export NO_STRIP=1

# Everything Stud ships that is NOT a host library is moved out of the
# AppDir while linuxdeploy runs, and put back afterwards.
#
# linuxdeploy walks every ELF file it finds and resolves each one's
# dependencies against the host. For bionic that is wrong in principle
# and fails in practice, libc.so needs "ld-android.so", a soname the
# Android linker answers from itself and no host has, and for ANGLE it
# is unnecessary, since that build is self-contained. Neither needs
# patching, deploying or interpreting; they only need to be in the
# finished image at the path the binaries look for them at.
private_dir="$work/private"
rm -rf "$private_dir"; mkdir -p "$private_dir/lib" "$private_dir/libexec"
[ -d "$appdir/usr/lib/stud" ] && mv "$appdir/usr/lib/stud" "$private_dir/lib/stud"
[ -d "$appdir/usr/libexec/stud/lib64" ] && mv "$appdir/usr/libexec/stud/lib64" "$private_dir/libexec/lib64"
[ -f "$appdir/usr/libexec/stud/stud-runtime-bionic" ] &&
    mv "$appdir/usr/libexec/stud/stud-runtime-bionic" "$private_dir/libexec/stud-runtime-bionic"

restore_private() {
    [ -d "$private_dir/lib/stud" ] && mv "$private_dir/lib/stud" "$appdir/usr/lib/stud"
    [ -d "$private_dir/libexec/lib64" ] && mv "$private_dir/libexec/lib64" "$appdir/usr/libexec/stud/lib64"
    [ -f "$private_dir/libexec/stud-runtime-bionic" ] &&
        mv "$private_dir/libexec/stud-runtime-bionic" "$appdir/usr/libexec/stud/stud-runtime-bionic"
    rm -rf "$private_dir"
}
trap restore_private EXIT

# linuxdeploy and its plugin are themselves AppImages, and a machine
# without FUSE cannot mount one. They both understand being unpacked
# instead, which costs nothing here.
if [ ! -e /dev/fuse ]; then
    say "no /dev/fuse, running the tools unpacked"
    export APPIMAGE_EXTRACT_AND_RUN=1
fi

# Libraries that must come from the host, not the bundle.
#
# These sit close enough to the display server and the graphics stack
# that a copy taken from the build machine is an ABI gamble on every
# other one. libxcb-util is the concrete case: bundling it segfaults
# inside its own initialiser on a machine whose xcb differs, before any
# of Stud's code runs. The rule of thumb is the AppImage project's own
# excludelist: anything X, xcb, GL, or glibc-adjacent stays outside.
#
# There is a second reason, which is what libxkbcommon and libffi are
# doing here. AppRun puts this bundle's own lib directory on
# LD_LIBRARY_PATH so that hand-copied Qt plugins can find what they need,
# and that path is inherited by the HOST code the graphics stack loads
# into render-host, a Vulkan implicit layer such as MangoHud, most
# obviously. A bundled copy of a library that layer also needs is then
# shadowing the host's, in a process neither of them is prepared for.
# Stud ships no MangoHud and never should; the least it can do is stay
# out of the way of the one the user installed.
exclude_libs=(
    "libxcb*.so*" "libX11*.so*" "libXext.so*" "libXrender.so*" "libXi.so*"
    "libGL.so*" "libGLX.so*" "libEGL.so*" "libGLdispatch.so*" "libOpenGL.so*"
    "libdrm.so*" "libgbm.so*" "libwayland-*.so*" "libvulkan.so*"
    "libxkbcommon*.so*" "libffi.so*"
    "libasound.so*" "libjack.so*" "libpipewire*.so*" "libpulse*.so*"
    "libc.so*" "libm.so*" "libdl.so*" "libpthread.so*" "librt.so*"
    "libstdc++.so*" "libgcc_s.so*"
)
exclude_args=()
for pattern in "${exclude_libs[@]}"; do exclude_args+=("--exclude-library=$pattern"); done

# Stud is a Wayland application, so the Wayland platform plugin has to
# be in the bundle, linuxdeploy-plugin-qt deploys only libqxcb.so
# unless it is told otherwise, and a bundle with no Wayland plugin falls
# back to XWayland (or, with no X at all, refuses to start).
#
# Its file name is not stable across Qt versions. Up to 6.9 there was one
# plugin per integration (libqwayland-generic.so, libqwayland-egl.so);
# 6.10 merged them into a single libqwayland.so advertising the same
# keys, so naming either set outright is a build that breaks on the next
# base image, which is exactly how this broke on ubuntu:26.04. They are
# discovered from the Qt this build is deploying from instead.
qt_plugin_dir="$("${QMAKE}" -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
[ -d "$qt_plugin_dir/platforms" ] ||
    die "$QMAKE reports no plugin directory, there is no Qt to take a platform plugin from"
wayland_platform_plugins=()
for plugin in "$qt_plugin_dir"/platforms/libqwayland*.so; do
    [ -e "$plugin" ] || continue
    wayland_platform_plugins+=("$(basename "$plugin")")
done
[ "${#wayland_platform_plugins[@]}" -gt 0 ] ||
    die "no Wayland platform plugin in $qt_plugin_dir/platforms, the bundle would fall back to XWayland"
EXTRA_PLATFORM_PLUGINS="$(IFS=';'; printf '%s' "${wayland_platform_plugins[*]}")"
export EXTRA_PLATFORM_PLUGINS
say "deploying the Wayland platform plugin: $EXTRA_PLATFORM_PLUGINS"


say "bundling dependencies"
"$tools/linuxdeploy" \
    --appdir "$appdir" \
    "${exclude_args[@]}" \
    --plugin qt \
    --desktop-file "$appdir/usr/share/applications/$app_id.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/512x512/apps/$app_id.png" \
    "${extra_exes[@]}"

# Drop the host libraries out of the bundle.
#
# --exclude-library above only governs what linuxdeploy itself deploys;
# the Qt plugin runs as its own process and copies Qt's dependencies
# regardless, which is how libxcb-util ends up in the AppDir anyway. So
# they are removed here, after everything has been bundled and before
# the image is packed.
say "removing host libraries from the bundle"
for pattern in "${exclude_libs[@]}"; do
    find "$appdir/usr/lib" -maxdepth 1 -name "$pattern" -print -delete 2>/dev/null || true
done

# Two plugin directories linuxdeploy-plugin-qt does not deploy, copied
# straight out of the Qt it just deployed from. EXTRA_PLUGINS is not a
# variable this plugin honours, tried, and it changed nothing.
#
# wayland-graphics-integration-client holds the client buffer
# integrations. Without it the Wayland platform plugin loads and then
# reports `Failed to load client buffer integration: "wayland-egl"` with
# `Available client buffer integrations: QList()`, which is a window that
# never gets a buffer.
#
# platformthemes is what makes a Qt application look like the desktop it
# runs on rather than like bare Qt. Native Breeze is out of reach here by
# construction. It is a KF6 plugin built against the host's own Qt, and
# this bundle carries a different one, but the XDG portal theme needs
# nothing except the portal and gives the desktop's real colours, fonts
# and icon theme.
#
# They are copied rather than run through linuxdeploy on purpose: every
# library either needs is already in the bundle and already loaded by the
# time Qt dlopens them (the Wayland platform plugin has libQt6WaylandClient
# open before it looks for a buffer integration), and patchelf is exactly
# what has to be kept away from these; see NO_STRIP above.
# styles is the real Breeze widget style. A widget style is a Qt plugin
# and can only load into the Qt it was built against, so the host's own
# Breeze can never load into this bundle whatever version it is, the
# only way an AppImage looks like the desktop it is running on is to
# carry a matching one. That is why this image is built on a base that
# has KF6 at all (see packaging/Containerfile.appimage).
# kf6/kwindowsystem is Breeze's own way of asking the compositor about a
# window; without it KWindowSystem says `Could not find any platform
# plugin` on every launch and answers its callers with nothing.
for plugin_dir in wayland-graphics-integration-client platformthemes styles kf6/kwindowsystem; do
    if [ -d "$qt_plugin_dir/$plugin_dir" ]; then
        say "bundling the $plugin_dir plugins"
        mkdir -p "$appdir/usr/plugins/$plugin_dir"
        cp -n "$qt_plugin_dir/$plugin_dir"/*.so "$appdir/usr/plugins/$plugin_dir/" 2>/dev/null || true
    fi
done
[ -e "$appdir/usr/plugins/wayland-graphics-integration-client/libqt-plugin-wayland-egl.so" ] ||
    die "no wayland-egl client buffer integration, the window would never get a buffer"
[ -e "$appdir/usr/plugins/platformthemes/libqxdgdesktopportal.so" ] ||
    die "no XDG portal platform theme. Stud would not follow the desktop's theme"
[ -e "$appdir/usr/plugins/styles/breeze6.so" ] ||
    die "no Breeze style. Stud would look like bare Qt next to the same build from an rpm"

# Those plugins were copied, not deployed, so nothing has brought their
# own libraries along, Breeze alone pulls a good deal of KF6. Walked
# here with ldd, skipping anything on the exclude list above (which must
# come from the host) and anything already bundled.
say "bundling what the copied plugins need"
bundle_deps_of() {
    local target="$1" resolved
    ldd "$target" 2>/dev/null | awk '$2 == "=>" && $3 ~ /^\// { print $3 }' | while read -r resolved; do
        local base; base="$(basename "$resolved")"
        [ -e "$appdir/usr/lib/$base" ] && continue
        local skip=0 pattern
        for pattern in "${exclude_libs[@]}"; do
            # shellcheck disable=SC2254
            case "$base" in $pattern) skip=1; break;; esac
        done
        [ "$skip" = 1 ] && continue
        cp -n "$resolved" "$appdir/usr/lib/" 2>/dev/null || true
    done
}
for plugin_dir in wayland-graphics-integration-client platformthemes styles kf6/kwindowsystem; do
    for plugin in "$appdir/usr/plugins/$plugin_dir"/*.so; do
        [ -e "$plugin" ] || continue
        bundle_deps_of "$plugin"
    done
done
# One more pass: the libraries just copied have dependencies of their own
# (KF6 is a graph, not a list). Repeated until nothing new appears.
for _ in 1 2 3 4 5 6; do
    before="$(find "$appdir/usr/lib" -maxdepth 1 -name '*.so*' | wc -l)"
    for lib in "$appdir/usr/lib"/*.so*; do
        [ -f "$lib" ] || continue
        bundle_deps_of "$lib"
    done
    [ "$(find "$appdir/usr/lib" -maxdepth 1 -name '*.so*' | wc -l)" = "$before" ] && break
done

# Qt's OpenSSL backend is built to dlopen libssl rather than link it
# (Ubuntu configures Qt with openssl-runtime), so linuxdeploy, which
# follows DT_NEEDED and nothing else, cannot see it and does not bring
# it. libcrypto arrives anyway, pulled in by something that does link it,
# and the result is a bundle that has half of OpenSSL, a TLS plugin that
# cannot load, and every HTTPS request failing with
# `TLS initialization failed`. Stud's login is an HTTPS request.
#
# Bundling both halves is deliberate: a bundled libcrypto beside the
# host's libssl is a worse pairing than bundling the two together.
say "bundling the OpenSSL library Qt dlopens"
for libssl in /usr/lib/x86_64-linux-gnu/libssl.so.3 /lib/x86_64-linux-gnu/libssl.so.3; do
    [ -e "$libssl" ] || continue
    cp -n "$libssl" "$appdir/usr/lib/" && break
done
[ -e "$appdir/usr/lib/libssl.so.3" ] || die "no libssl.so.3 to bundle; Stud could not log in"

# A real AppRun, replacing the symlink to the binary linuxdeploy leaves.
#
# linuxdeploy's apprun-hooks mechanism needs an AppRun that sources them,
# and it does not write one when the entry point is a plain symlink,
# so the one thing that has to happen before Qt starts is done here.
#
# Qt picks a platform theme by looking for the plugin its own desktop
# would use, which on KDE is a KF6 plugin built against the host's Qt,
# deliberately not in this bundle, and not loadable in it. Asking for the
# portal theme by name gets the desktop's real colours, fonts and icon
# theme with nothing but the portal, which every modern desktop runs.
rm -f "$appdir/AppRun"
cat > "$appdir/AppRun" <<'APPRUN'
#!/bin/sh
# Stud's AppImage entry point. Libraries and Qt plugins are already found
# through the RPATHs and qt.conf linuxdeploy wrote; this sets the one
# thing Qt cannot work out for itself inside a bundle.
here="$(dirname "$(readlink -f "$0")")"

# Qt plugins copied in rather than deployed carry no RUNPATH of their
# own, so a plugin whose own library is in this bundle cannot find it and
# fails to load with no explanation beyond "Failed to load". Stud keeps
# host libraries out of usr/lib deliberately (see exclude_libs in
# packaging/build-appimage.sh), so putting it on the search path exposes
# only the bundle's own Qt.
#
# STUD_HOST_LD_LIBRARY_PATH carries the original across, so anything Stud
# runs that belongs to the host, kbuildsycoca6, update-desktop-database,
# a browser, can be given its own environment back instead of this Qt.
export STUD_HOST_LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
export LD_LIBRARY_PATH="$here/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

[ -n "$QT_QPA_PLATFORMTHEME" ] || export QT_QPA_PLATFORMTHEME=xdgdesktopportal

# Breeze is carried in this bundle, but only a KDE desktop should get it:
# on GNOME or anywhere else the portal theme's own choice is the right
# one, and forcing Breeze there would be the same mistake in reverse.
case "${XDG_CURRENT_DESKTOP:-}" in
    *KDE*)
        [ -n "$QT_STYLE_OVERRIDE" ] ||
            [ ! -e "$here/usr/plugins/styles/breeze6.so" ] ||
            export QT_STYLE_OVERRIDE=Breeze
        ;;
esac

exec "$here/usr/bin/stud" "$@"
APPRUN
chmod +x "$appdir/AppRun"

# Put Stud's own libraries back before the image is packed.
trap - EXIT
restore_private

# A PortAudio of Stud's own, for machines that have none.
#
# render-host loads the HOST's PortAudio first and only falls back to this
# one, the system copy is built against the audio stack that machine
# actually runs (Fedora's has a real PipeWire host API; one built here
# does not), and using it is what makes Stud appear in the volume mixer as
# an ordinary application. So this lives outside usr/lib, deliberately, to
# keep it off LD_LIBRARY_PATH where it could win a race it should lose.
#
# It is copied rather than deployed for the same reason libssl is below:
# nothing links PortAudio, render-host dlopens it, so linuxdeploy cannot
# see it at all. Its own dependencies, libasound, libjack, libpipewire
# are excluded above and come from the host, which is the whole point:
# an ALSA library from another distribution looks for its plugins at that
# distribution's paths, finds no pipewire or pulse plugin, and falls back
# to talking to the hardware directly. That is exactly the reported
# "audio does not work and Stud never appears in the volume mixer".
say "bundling a fallback PortAudio"
mkdir -p "$appdir/usr/lib/stud/audio"
for candidate in /usr/lib/x86_64-linux-gnu/libportaudio.so.2 /usr/lib64/libportaudio.so.2; do
    [ -e "$candidate" ] || continue
    cp -n "$candidate" "$appdir/usr/lib/stud/audio/libportaudio.so.2"
    break
done
[ -e "$appdir/usr/lib/stud/audio/libportaudio.so.2" ] ||
    die "no libportaudio.so.2 to fall back on, a machine without PortAudio would be silent"


say "packing the image"
mkdir -p "$build_dir/packages"
out="$build_dir/packages/Stud-$(uname -m).AppImage"
rm -f "$out"
ARCH="$(uname -m)" "$tools/appimagetool" "$appdir" "$out"

say "written: $out"
ls -la "$out" >&2
