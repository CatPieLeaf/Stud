#!/usr/bin/env bash
#
# Builds Stud as an AppImage with quick-sharun (pkgforge-dev/Anylinux-AppImages):
# every library bundled, glibc and Mesa included, packed as SquashFS behind
# uruntime, which mounts with or without FUSE.
#
# Usage: packaging/build-appimage.sh [build-dir]
# Runs in packaging/Containerfile.appimage (podman or docker) and writes
# build-appimage/packages/.

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

# STUD_CMAKE_ARGS passes extra flags; releases set -DSTUD_VERSION with it.
say "building"
# shellcheck disable=SC2086
cmake -S "$repo_root" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX=/usr -DSTUD_BUILD_TESTS=OFF ${STUD_CMAKE_ARGS:-} >&2
cmake --build "$build_dir" -j"$(nproc)" >&2

# quick-sharun deploys from the system, and the container is thrown away.
say "installing into /usr"
cmake --install "$build_dir" >/dev/null

# Rosemary's upscaler shaders must be there, or the upscalers do nothing.
python3 "$repo_root/tools/validate-payload.py" / --label "installed tree" \
    --rosemary-source "$build_dir/rosemary-prefix/src/rosemary" >&2 || die "the install is incomplete"

# Pinned and checked, since what they download ends up in the image.
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

# Mesa without LLVM. NVIDIA's driver is never bundled; sharun loads the host's.
say "installing Anylinux's mesa"
"$tools/get-debloated-pkgs.sh" --add-mesa

rm -rf "$appdir"
export APPDIR="$appdir"
export DESKTOP="/usr/share/applications/$app_id.desktop"
export ICON="/usr/share/icons/hicolor/512x512/apps/$app_id.png"
export DEPLOY_QT=1 DEPLOY_VULKAN=1 DEPLOY_OPENGL=1 DEPLOY_PULSE=1
# Only the UI can be traced: Rosemary exits at once without a client to run.
export STRACE_BINARY=stud

# Libraries loaded by name at runtime, which tracing the UI can't see: SDL3 and
# what it opens, Vulkan/EGL/GBM, FFmpeg, and the KF6 plugin Breeze uses.
qt_plugins="$(qmake6 -query QT_INSTALL_PLUGINS)"
# FFmpeg under the SONAME Rosemary opens.
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

# quick-sharun rewrites /usr/lib and /usr/share paths in executables, but these
# point to the host's MangoHud and the guest's macOS paths, so restore both.
for exe in /usr/bin/stud /usr/libexec/stud/rosemary; do
    strip -o "$appdir/shared/bin/${exe##*/}" "$exe"
done

# stud-ui finds Rosemary in libexec/stud; a hardlink to sharun there runs it.
mkdir -p "$appdir/libexec/stud"
ln -f "$appdir/sharun" "$appdir/libexec/stud/rosemary"
# The desktop entry and icons Stud installs, and redistributed licences.
(cd /usr/share && cp -a --parents \
    "applications/$app_id.desktop" "applications/$app_id.Player.desktop" \
    "applications/$app_id.Studio.desktop" "metainfo/$app_id.metainfo.xml" "mime/packages/$app_id.xml" \
    icons/hicolor/*/apps/"$app_id"*.png licenses/stud doc/stud \
    "$appdir/share/")
# AppImageHub only reads metainfo from usr/share/metainfo.
mkdir -p "$appdir/usr/share"
ln -s ../../share/metainfo "$appdir/usr/share/metainfo"

# Sourced by quick-sharun's AppRun before Stud starts.
cat > "$appdir/bin/stud.hook" <<'HOOK'
# Host programs Stud starts get their original library path (desktop_entry.cpp).
export STUD_HOST_LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"

# Undo AppRun.lib's cache move, so the web views' cache stays in ~/.cache.
export XDG_CACHE_HOME="$CACHEDIR"

# The host's theme plugin can't load against the bundled Qt; the portal's matches the desktop.
[ -n "${QT_QPA_PLATFORMTHEME:-}" ] || export QT_QPA_PLATFORMTHEME=xdgdesktopportal

case "${XDG_CURRENT_DESKTOP:-}" in
    *KDE*) [ -n "${QT_STYLE_OVERRIDE:-}" ] || export QT_STYLE_OVERRIDE=Breeze ;;
esac
HOOK

# QtWebEngine's helper, resources and locales are bundled but Qt looks elsewhere
# and aborts the web view. Looked up, since quick-sharun's layout follows the source.
webengine_process="$(find "$appdir/bin" -maxdepth 1 -name QtWebEngineProcess -print -quit)"
webengine_resources="$(find "$appdir" -name qtwebengine_resources.pak -printf '%h\n' -quit)"
webengine_locales="$(find "$appdir" -type d -name qtwebengine_locales -print -quit)"
[ -n "$webengine_process" ] || die "quick-sharun did not bundle QtWebEngineProcess"
[ -n "$webengine_resources" ] || die "quick-sharun did not bundle QtWebEngine's resources"
[ -n "$webengine_locales" ] || die "quick-sharun did not bundle QtWebEngine's locales"
cat >> "$appdir/bin/stud.hook" <<HOOK

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

# SquashFS, not DwarFS: AppImageHub, firejail and the classic runtime only mount SquashFS.
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
# Lets AppImageUpdate update Stud in place from the latest release's .zsync.
"$out" --appimage-addupdinfo "gh-releases-zsync|CatPieLeaf|Stud|latest|Stud-$arch.AppImage.zsync"
cat "$work/image.squashfs" >> "$out"
rm -f "$work/image.squashfs"
(cd "$packages" && zsyncmake -u "${out##*/}" -o "${out##*/}.zsync" "${out##*/}")

say "written: $out"
