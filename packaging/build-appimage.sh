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
    printf '\033[1mappimage:\033[0m building in the container\n' >&2
    exec "$engine" run --rm \
        -v "$repo_root:/src:z" \
        -e STUD_APPIMAGE_IN_CONTAINER=1 \
        -e STUD_CMAKE_ARGS="${STUD_CMAKE_ARGS:-}" \
        -e STUD_ROSEMARY_TARGET_DIR="${STUD_ROSEMARY_TARGET_DIR:-}" \
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

# STUD_CMAKE_ARGS passes anything through; release automation sets
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

# Stud, Rosemary, and Rosemary's upscaler shaders: one built without glslc
# scales plainly, and the upscalers in Settings would do nothing.
python3 "$repo_root/tools/validate-payload.py" / --label "installed tree" \
    --rosemary-source "$build_dir/rosemary-prefix/src/rosemary" >&2 || die "the install is incomplete"

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
export DEPLOY_QT=1 DEPLOY_VULKAN=1 DEPLOY_OPENGL=1 DEPLOY_PULSE=1
# Only the UI can be traced: Rosemary exits at once without the client
# stud-ui hands it.
export STRACE_BINARY=stud

# Everything loaded by name at runtime, which no tracer of the UI sees:
# Rosemary's SDL3 and, through it, the window system, sound and input
# libraries SDL opens itself; its Vulkan, EGL and GBM; FFmpeg for video;
# and the KF6 plugin Breeze asks the compositor through.
qt_plugins="$(qmake6 -query QT_INSTALL_PLUGINS)"
# FFmpeg by the name Rosemary opens, whichever release the image carries.
soname() { objdump -p "$1" | awk '/SONAME/ { print $2 }'; }
dlopened=(
    /usr/lib/libSDL3.so.0
    /usr/lib/libvulkan.so.1 /usr/lib/libEGL.so.1 /usr/lib/libgbm.so.1
    /usr/lib/libwayland-client.so.0 /usr/lib/libwayland-egl.so.1 /usr/lib/libwayland-cursor.so.0
    /usr/lib/libdecor-0.so.0 /usr/lib/libxkbcommon.so.0
    /usr/lib/libX11.so.6 /usr/lib/libXext.so.6 /usr/lib/libXi.so.6 /usr/lib/libXrandr.so.2
    /usr/lib/libXcursor.so.1 /usr/lib/libXfixes.so.3 /usr/lib/libXss.so.1
    /usr/lib/libpipewire-0.3.so.0 /usr/lib/libpulse.so.0 /usr/lib/libasound.so.2
    "/usr/lib/$(soname /usr/lib/libavcodec.so)" "/usr/lib/$(soname /usr/lib/libavutil.so)"
    /usr/lib/libxkbcommon-x11.so.0
    "$qt_plugins"/kf6/kwindowsystem/*.so
)

say "bundling"
"$tools/quick-sharun.sh" \
    /usr/bin/stud \
    /usr/libexec/stud/rosemary \
    "${dlopened[@]}"

# quick-sharun rewrites every /usr/lib and /usr/share it finds in an
# executable to point into the bundle. Stud's are not its own files: the
# host's MangoHud, and the macOS paths Rosemary hands the client
# (/usr/lib/libSystem.B.dylib is the guest's, not the bundle's). Both
# executables go back in unrewritten.
for exe in /usr/bin/stud /usr/libexec/stud/rosemary; do
    strip -o "$appdir/shared/bin/${exe##*/}" "$exe"
done

# stud-ui starts Rosemary by path, from libexec/stud beside bin/. A
# hardlink to sharun there runs the same shared/bin binary.
mkdir -p "$appdir/libexec/stud"
ln -f "$appdir/sharun" "$appdir/libexec/stud/rosemary"
# Stud's own share/ files: the desktop entry and icons it installs for
# the user, and the licences of everything it redistributes.
(cd /usr/share && cp -a --parents \
    "applications/$app_id.desktop" "metainfo/$app_id.metainfo.xml" \
    icons/hicolor/*/apps/"$app_id"*.png licenses/stud doc/stud \
    "$appdir/share/")
# AppImageHub reads the AppStream metainfo only from usr/share/metainfo.
mkdir -p "$appdir/usr/share"
ln -s ../../share/metainfo "$appdir/usr/share/metainfo"

# What the old AppRun set, sourced by quick-sharun's AppRun before Stud
# starts.
cat > "$appdir/bin/stud.hook" <<'HOOK'
# Host programs Stud starts (desktop tools, a browser) get back exactly
# the library path they would have had; see ui/src/desktop_entry.cpp.
export STUD_HOST_LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"

# AppRun.lib moves XDG_CACHE_HOME to ~/.cache/AppImage-Cache. Stud's cache
# (the web views') stays where every other package of Stud keeps it;
# CACHEDIR is the value from before the move.
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

# QtWebEngine, for the login page: its helper process, its resources and
# its locales are all bundled, but Qt looks for them under share/qt6 and
# beside the executable, finds none of them, and aborts the web view at
# start. The Flatpak sets the same three for the same reason. Looked up, since where
# quick-sharun puts each follows the source's layout.
webengine_process="$(find "$appdir/bin" -maxdepth 1 -name QtWebEngineProcess -print -quit)"
webengine_resources="$(find "$appdir" -name qtwebengine_resources.pak -printf '%h\n' -quit)"
webengine_locales="$(find "$appdir" -type d -name qtwebengine_locales -print -quit)"
[ -n "$webengine_process" ] || die "quick-sharun did not bundle QtWebEngineProcess"
[ -n "$webengine_resources" ] || die "quick-sharun did not bundle QtWebEngine's resources"
[ -n "$webengine_locales" ] || die "quick-sharun did not bundle QtWebEngine's locales"
cat >> "$appdir/bin/stud.hook" <<HOOK

# QtWebEngine's own files; see build-appimage.sh.
export QTWEBENGINEPROCESS_PATH="\$APPDIR/${webengine_process#"$appdir"/}"
export QTWEBENGINE_RESOURCES_PATH="\$APPDIR/${webengine_resources#"$appdir"/}"
export QTWEBENGINE_LOCALES_PATH="\$APPDIR/${webengine_locales#"$appdir"/}"
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
