#!/usr/bin/env bash
#
# Builds Stud as an AppImage: one file that runs on any distribution
# without installing anything.
#
# The bundle is made by quick-sharun, from the Anylinux AppImages project
# (github.com/pkgforge-dev/Anylinux-AppImages). It carries every library
# Stud needs, glibc and mesa included, and starts each executable through
# sharun, so nothing about the host's libraries is a requirement. The
# image is packed as SquashFS behind uruntime, which mounts it with FUSE
# when it can and without it when it cannot.
#
# Usage:
#   packaging/build-appimage.sh [build-dir]
#
# It runs in a container built from packaging/Containerfile.appimage,
# needs podman or docker, and writes build-appimage/packages/.
#
# bubblewrap is deliberately NOT bundled. It needs the AppArmor or SELinux
# policy a distribution ships with its own build to create a user
# namespace at all, so a copied-in binary would be refused on the
# distributions that matter. Stud checks for it and says so.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "${STUD_APPIMAGE_IN_CONTAINER:-0}" != 1 ]; then
    engine=""
    for candidate in podman docker; do
        command -v "$candidate" >/dev/null 2>&1 && { engine="$candidate"; break; }
    done
    [ -n "$engine" ] || { echo "appimage: podman or docker is needed" >&2; exit 1; }
    printf '\033[1mappimage:\033[0m building the image with %s\n' "$engine" >&2
    "$engine" build -f "$repo_root/packaging/Containerfile.appimage" \
        -t stud-appimage-build "$repo_root" >&2
    # tools/setup.sh can link third_party/ entries out of the repository;
    # mount each target at its own path so the link resolves inside too.
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
        -e STUD_CMAKE_ARGS="${STUD_CMAKE_ARGS:-}" \
        stud-appimage-build \
        /src/packaging/build-appimage.sh "${@:-}"
fi

say() { printf '\033[1mappimage:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31mappimage:\033[0m %s\n' "$*" >&2; exit 1; }

build_dir="${1:-$repo_root/build-appimage}"
work="$build_dir/appimage"
appdir="$work/AppDir"
arch="$(uname -m)"

app_id="$(grep -m1 '^set(STUD_APP_ID' "$repo_root/CMakeLists.txt" | cut -d'"' -f2)"
[ -n "$app_id" ] || die "could not read STUD_APP_ID out of CMakeLists.txt"

# Configured every time: whether Process B, ANGLE and bionic are built at
# all is decided at configure time from what third_party/ holds.
# STUD_CMAKE_ARGS passes anything else through; release automation sets
# -DSTUD_VERSION with it.
say "building"
# shellcheck disable=SC2086
cmake -S "$repo_root" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX=/usr -DSTUD_BUILD_TESTS=OFF ${STUD_CMAKE_ARGS:-} >&2
cmake --build "$build_dir" -j"$(nproc)" >&2

# quick-sharun deploys from the system, not from a staging tree, and this
# container is thrown away afterwards.
say "installing into /usr"
cmake --install "$build_dir" >/dev/null

# A build with third_party/ missing still succeeds, it just has no
# Process B, no ANGLE and no bionic: a window that cannot run Roblox.
for required in \
    /usr/bin/stud \
    /usr/libexec/stud/stud-render-host \
    /usr/libexec/stud/stud-runtime-bionic \
    /usr/lib/stud/angle/libEGL.so \
    /usr/lib/stud/android-bionic/linker64 \
    /usr/lib/stud/android-bionic/libc.so
do
    [ -e "$required" ] || die "the install produced no $required, run tools/setup.sh first"
done

# Anylinux's tools, pinned to one commit and checked, since what they
# download ends up inside the image.
anylinux_rev=de2a80d6229298518cc737ced6b3fa5832053ea5
tools="$work/tools"
mkdir -p "$tools"
fetch_tool() {
    local name="$1" sha256="$2"
    curl -fsSL -o "$tools/$name" \
        "https://raw.githubusercontent.com/pkgforge-dev/Anylinux-AppImages/$anylinux_rev/useful-tools/$name"
    echo "$sha256  $tools/$name" | sha256sum -c --quiet - || die "$name does not match its pinned checksum"
    chmod +x "$tools/$name"
}
fetch_tool quick-sharun.sh 8026711b271c0d67d37075cd7e8c50dbd9fa635c012f9edb92a2c0d41573664b
fetch_tool get-debloated-pkgs.sh 217ff68aafe085d2d4ab34a50e267805af43736ad8ef880c01ef88e52db8589f

# Mesa without the LLVM it normally drags in. NVIDIA's driver is never
# bundled: sharun loads the host's, whatever libc it was built against.
say "installing Anylinux's mesa"
"$tools/get-debloated-pkgs.sh" --add-mesa

rm -rf "$appdir"
export APPDIR="$appdir"
export DESKTOP="/usr/share/applications/$app_id.desktop"
export ICON="/usr/share/icons/hicolor/512x512/apps/$app_id.png"
export DEPLOY_QT=1 DEPLOY_VULKAN=1 DEPLOY_PULSE=1
# Only the UI can be traced: render-host and the web view exit at once
# without the session stud-ui hands them.
export STRACE_BINARY=stud

# Everything Stud loads by name at runtime and so no tracer would see:
# render-host's window system and Vulkan, miniaudio's backends, and the
# KF6 plugin Breeze asks the compositor through.
qt_plugins="$(qmake6 -query QT_INSTALL_PLUGINS)"
dlopened=(
    /usr/lib/libvulkan.so.1
    /usr/lib/libX11-xcb.so.1 /usr/lib/libXext.so.6 /usr/lib/libXi.so.6 /usr/lib/libXrender.so.1
    /usr/lib/libxkbcommon-x11.so.0 /usr/lib/libdecor-0.so.0
    /usr/lib/libasound.so.2 /usr/lib/libpulse.so.0
    "$qt_plugins"/kf6/kwindowsystem/*.so
)

say "bundling"
"$tools/quick-sharun.sh" \
    /usr/bin/stud \
    /usr/libexec/stud/stud-render-host \
    /usr/libexec/stud/stud-webview \
    "${dlopened[@]}"

# quick-sharun rewrites every /usr/lib and /usr/share it finds in an
# executable to point into the bundle. Stud's are not its own files: the
# host's MangoHud, and paths inside Process B's Android sandbox
# (/system/usr/share/i18n). Its executables go back in unrewritten.
for exe in /usr/bin/stud /usr/libexec/stud/stud-render-host /usr/libexec/stud/stud-webview; do
    strip -o "$appdir/shared/bin/${exe##*/}" "$exe"
done
# Bundling glycin adds a bwrap wrapper to bin/, which AppRun puts first on
# PATH. Process B's sandbox must be the host's own bwrap, untouched.
rm -f "$appdir/bin/bwrap"

# stud-ui starts its siblings by path, from libexec/stud beside bin/.
# A hardlink to sharun there runs the same shared/bin binary.
mkdir -p "$appdir/libexec/stud"
for exe in stud-render-host stud-webview; do
    ln -f "$appdir/sharun" "$appdir/libexec/stud/$exe"
done

# Stud's own files that are not host libraries, copied as they are:
# ANGLE is self-contained, and bionic and Process B are Android ELF that
# no host tool may patch, strip or resolve.
say "adding ANGLE, bionic and Process B"
cp -a /usr/lib/stud "$appdir/lib/stud"
cp -a /usr/libexec/stud/stud-runtime-bionic /usr/libexec/stud/lib64 "$appdir/libexec/stud/"
# Stud's own share/ files: the desktop entry and icons it installs for
# the user, and the licences of everything it redistributes.
(cd /usr/share && cp -a --parents \
    "applications/$app_id.desktop" "metainfo/$app_id.metainfo.xml" \
    icons/hicolor/*/apps/"$app_id.png" licenses/stud doc/stud \
    "$appdir/share/")

# What the old AppRun set, sourced by quick-sharun's AppRun before Stud
# starts.
cat > "$appdir/bin/stud.hook" <<'HOOK'
# Host programs Stud starts (desktop tools, a browser) get back exactly
# the library path they would have had; see ui/src/desktop_entry.cpp.
export STUD_HOST_LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"

# AppRun.lib moves XDG_CACHE_HOME to ~/.cache/AppImage-Cache. Stud's cache
# (the extracted APK, the engine's state) stays where every other package
# of Stud keeps it; CACHEDIR is the value from before the move.
export XDG_CACHE_HOME="$CACHEDIR"

# The host's own platform theme is a plugin built against the host's Qt
# and cannot load here; the portal one gives the desktop's real colours,
# fonts and icons.
[ -n "${QT_QPA_PLATFORMTHEME:-}" ] || export QT_QPA_PLATFORMTHEME=xdgdesktopportal

# Breeze only on KDE; anywhere else the portal theme's choice is right.
case "${XDG_CURRENT_DESKTOP:-}" in
    *KDE*) [ -n "${QT_STYLE_OVERRIDE:-}" ] || export QT_STYLE_OVERRIDE=Breeze ;;
esac
HOOK

for plugin in \
    platforms/libqwayland.so \
    wayland-graphics-integration-client/libqt-plugin-wayland-egl.so \
    platformthemes/libqxdgdesktopportal.so \
    styles/breeze6.so
do
    [ -n "$(find "$appdir" -path "*/plugins/$plugin" -print -quit)" ] ||
        die "no $plugin in the bundle"
done

# Packed as SquashFS behind uruntime, not the DwarFS quick-sharun makes:
# AppImageHub, firejail --appimage and the classic AppImage runtime can
# only mount SquashFS, and AppImageHub tests every image that way.
uruntime_version=v0.8.1
uruntime_sha256=2153e68d63f570c7ebe3d1f38df0fa16814f34d839c5cce2ee42667e327708e8
uruntime="$tools/uruntime-$uruntime_version"
curl -fsSL -o "$uruntime" \
    "https://github.com/VHSgunzo/uruntime/releases/download/$uruntime_version/uruntime-appimage-squashfs-$arch"
echo "$uruntime_sha256  $uruntime" | sha256sum -c --quiet - || die "uruntime does not match its pinned checksum"
chmod +x "$uruntime"

say "packing the image"
packages="$build_dir/packages"
out="$packages/Stud-$arch.AppImage"
mkdir -p "$packages"
rm -f "$out" "$out.zsync" "$work/image.squashfs"
"$uruntime" --appimage-mksquashfs "$appdir" "$work/image.squashfs" \
    -comp zstd -Xcompression-level 19 -b 1M -all-root -noappend -no-progress -quiet
cp "$uruntime" "$out"
# AppImageUpdate and other updaters update Stud in place from the newest
# GitHub release, through the .zsync published beside it.
"$out" --appimage-addupdinfo "gh-releases-zsync|CatPieLeaf|Stud|latest|Stud-$arch.AppImage.zsync"
cat "$work/image.squashfs" >> "$out"
rm -f "$work/image.squashfs"
(cd "$packages" && zsyncmake -u "${out##*/}" -o "${out##*/}.zsync" "${out##*/}")

say "written: $out"
