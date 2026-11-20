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

# Arch's FFmpeg links encoders and image libraries (x265, aom, SVT-AV1, librsvg
# and its loaders) nothing here uses. The same version with every decoder and
# demuxer but no encoders or muxers replaces it, so Rosemary and Qt WebEngine find
# what they use under the same names; FFmpeg links only codec libraries asked for.
ffmpeg_version="$(pacman -Q ffmpeg | awk '{print $2}' | sed 's/^[0-9]*://; s/-[^-]*$//')"
say "rebuilding FFmpeg $ffmpeg_version without encoders"
mkdir -p "$work"
rm -rf "$work/ffmpeg-$ffmpeg_version"
curl -fsSL "https://ffmpeg.org/releases/ffmpeg-$ffmpeg_version.tar.xz" | tar -xJ -C "$work" ||
    die "ffmpeg.org has no FFmpeg $ffmpeg_version source"
(
    cd "$work/ffmpeg-$ffmpeg_version"
    ./configure --prefix=/usr --enable-shared --disable-static --disable-programs --disable-doc \
        --disable-encoders --disable-muxers --disable-avdevice --enable-libdav1d
    make -j"$(nproc)"
    make install
) >&2 || die "FFmpeg $ffmpeg_version did not build"

tools="$work/tools"
mkdir -p "$tools"
fetch_tool() {
    curl -fsSL -o "$tools/$1" "https://raw.githubusercontent.com/pkgforge-dev/Anylinux-AppImages/main/useful-tools/$1"
    chmod +x "$tools/$1"
}
fetch_tool quick-sharun.sh
fetch_tool get-debloated-pkgs.sh

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

# Libraries an ELF in the bundle links that the bundle doesn't have.
unmet() {
    python3 - "$appdir" <<'PY'
import os, subprocess, sys
files, elves = set(), []
for top, _, names in os.walk(sys.argv[1]):
    for name in names:
        path = os.path.join(top, name)
        files.add(name)
        try:
            with open(path, 'rb') as f:
                if f.read(4) == b'\x7fELF':
                    elves.append(path)
        except OSError:
            pass
for path in elves:
    out = subprocess.run(['readelf', '-d', path], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if '(NEEDED)' in line:
            lib = line.split('[')[1].split(']')[0]
            if lib not in files:
                print(f'{os.path.relpath(path, sys.argv[1])} needs {lib}')
PY
}

# Unused here: Qt's translations (only of Qt's own dialogs; Stud is in English),
# WebEngine's other languages (pages are unaffected) and developer tools, and the QML
# modules (with Quick Controls, Kirigami and Qt Labs) and PDF support the widget web
# views never load.
before="$(unmet | sort)"
find "$appdir" -name '*.qm' -delete
find "$appdir" -path '*/qtwebengine_locales/*.pak' ! -name 'en-US.pak' -delete
find "$appdir" -name qtwebengine_devtools_resources.pak -delete
find "$appdir" -type d -path '*/qt6/qml' -prune -exec rm -rf {} +
find "$appdir" \( -name 'libQt6QuickControls2*' -o -name 'libQt6QuickDialogs2*' -o -name 'libQt6QuickTemplates2*' \
    -o -name 'libQt6Labs*' -o -name 'libKirigami*' -o -name 'libQt6Pdf*' -o -name 'libqpdf.so' \) -delete
broken="$(comm -13 <(printf '%s\n' "$before") <(unmet | sort))"
[ -z "$broken" ] || die "trimming the bundle left these unmet: $broken"

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
uruntime="$tools/uruntime"
curl -fsSL -o "$uruntime" \
    "https://github.com/VHSgunzo/uruntime/releases/latest/download/uruntime-appimage-squashfs-$arch"
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
