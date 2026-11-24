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
# demuxer but no encoders or muxers replaces it, so Rosemary and GStreamer's libav
# plugin find what they use under the same names; FFmpeg links only codec libraries
# asked for.
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

# Arch's libavif links three AV1 encoders WPE WebKit never uses: the same version,
# built as Arch builds it but decoding with dav1d alone, replaces it.
avif_version="$(pacman -Q libavif | awk '{print $2}' | sed 's/^[0-9]*://; s/-[^-]*$//')"
say "rebuilding libavif $avif_version without encoders"
rm -rf "$work/libavif-$avif_version"
curl -fsSL "https://github.com/AOMediaCodec/libavif/archive/refs/tags/v$avif_version.tar.gz" | tar -xz -C "$work" ||
    die "libavif has no v$avif_version source"
(
    cmake -S "$work/libavif-$avif_version" -B "$work/libavif-$avif_version/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
        -DAVIF_CODEC_DAV1D=SYSTEM -DAVIF_CODEC_AOM=OFF -DAVIF_CODEC_RAV1E=OFF -DAVIF_CODEC_SVT=OFF \
        -DAVIF_LIBSHARPYUV=SYSTEM -DAVIF_LIBXML2=SYSTEM -DAVIF_BUILD_GDK_PIXBUF=OFF
    cmake --build "$work/libavif-$avif_version/build"
    cmake --install "$work/libavif-$avif_version/build"
) >&2 || die "libavif $avif_version did not build"

tools="$work/tools"
mkdir -p "$tools"
fetch_tool() {
    curl -fsSL -o "$tools/$1" "https://raw.githubusercontent.com/pkgforge-dev/Anylinux-AppImages/main/useful-tools/$1"
    chmod +x "$tools/$1"
}
fetch_tool quick-sharun.sh
fetch_tool get-debloated-pkgs.sh
# WPE WebKit starts its helper processes from a path fixed at build time, as
# WebKitGTK does. quick-sharun points WebKitGTK's library at a bundled copy of
# that path once it recognises WPE WebKit's library as one too; the helpers and
# the TLS module their network process needs are passed to it below, because it
# only bundles those along with GTK, which WPE WebKit doesn't use.
sed -i -e 's#^\([[:space:]]*\)\*libwebkit\*gtk-\*\.so\*)#\1*libwebkit*gtk-*.so*|*libWPEWebKit-*.so*)#' \
    -e 's#^\([[:space:]]*\)\*/libwebkit\*gtk-\*\.so\*)#\1*/libwebkit*gtk-*.so*|*/libWPEWebKit-*.so*)#' \
    "$tools/quick-sharun.sh"
[ "$(grep -c 'libWPEWebKit-\*\.so\*)' "$tools/quick-sharun.sh")" = 2 ] ||
    die "quick-sharun's WebKitGTK handling changed; WPE WebKit's helper path would not be bundled"
wpe_helper="$(find /usr/lib -name WPEWebProcess -path '*wpe-webkit*' -print -quit)"
[ -n "$wpe_helper" ] || die "WPE WebKit's helper processes are not installed"
wpe_dir="${wpe_helper%/*}"
export DEPLOY_P11KIT=1
# What WebKit sandboxes its helpers with: quick-sharun wraps bwrap so the
# bundle's paths exist inside, and points the library at the bundle's bin.
wpe_sandbox=()
for tool in bwrap xdg-dbus-proxy; do
    path="$(command -v "$tool")" || die "$tool, which WPE WebKit sandboxes its helpers with, is not installed"
    wpe_sandbox+=("$path")
done

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
# what it opens, Vulkan/EGL/GBM, FFmpeg, WPE WebKit, and the KF6 plugin Breeze uses.
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
    "/usr/lib/$(soname /usr/lib/libWPEWebKit-2.0.so)"
    "$wpe_dir" /usr/lib/gio/modules/libgiognutls.so "${wpe_sandbox[@]}"
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

# Undo AppRun.lib's cache move, so Stud's cache stays in ~/.cache.
export XDG_CACHE_HOME="$CACHEDIR"

# The host's theme plugin can't load against the bundled Qt; the portal's matches the desktop.
[ -n "${QT_QPA_PLATFORMTHEME:-}" ] || export QT_QPA_PLATFORMTHEME=xdgdesktopportal

case "${XDG_CURRENT_DESKTOP:-}" in
    *KDE*) [ -n "${QT_STYLE_OVERRIDE:-}" ] || export QT_STYLE_OVERRIDE=Breeze ;;
esac

# quick-sharun's path rewrite keeps WebKit's sandbox from mounting /usr/lib*,
# so its web process reaches the host's GBM backends (NVIDIA's) only via /lib*.
for dir in /usr/lib*/gbm /usr/lib/*/gbm; do
    [ -d "${dir#/usr}" ] && GBM_BACKENDS_PATH="${GBM_BACKENDS_PATH:+$GBM_BACKENDS_PATH:}${dir#/usr}"
done
[ -z "${GBM_BACKENDS_PATH:-}" ] || export GBM_BACKENDS_PATH
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
# the QML modules (with Quick Controls, Kirigami and Qt Labs) and PDF support a
# widget application never loads, Qt's Wayland compositor (Stud is only a client),
# GStreamer's developer tools, WPE WebKit's demo MiniBrowser, and the WebRTC
# plugins with what only they link:
# Roblox's pages make no calls, and voice chat runs in the client.
before="$(unmet | sort)"
find "$appdir" -name '*.qm' -delete
find "$appdir" -type d \( -path '*/qt6/qml' -o -path '*/plugins/wayland-graphics-integration-server' \) \
    -prune -exec rm -rf {} +
find "$appdir" \( -name 'libQt6QuickControls2*' -o -name 'libQt6QuickDialogs2*' -o -name 'libQt6QuickTemplates2*' \
    -o -name 'libQt6Labs*' -o -name 'libKirigami*' -o -name 'libQt6Pdf*' -o -name 'libqpdf.so' \
    -o -name 'libQt6WaylandCompositor*' -o -name 'libQt6WaylandEglCompositor*' \
    -o -name 'gst-hotdoc-plugins-scanner' -o -name 'gst-completion-helper' \
    -o -name 'libgstnice.so' -o -name 'libgstwebrtc.so' -o -name 'libgstdtls.so' -o -name 'libgstsrtp.so' \
    -o -name 'libgstsctp.so' -o -name 'MiniBrowser' \) -delete
# What only the files above linked, once nothing left in the bundle needs it.
for orphan in libnice libsrtp2 libgnutls libleancrypto; do
    aside="$work/aside/$orphan"
    rm -rf "$aside" && mkdir -p "$aside"
    while IFS= read -r lib; do
        mkdir -p "$aside/${lib%/*}" && mv "$appdir/$lib" "$aside/$lib"
    done < <(cd "$appdir" && find . -name "$orphan.so*")
    if unmet | grep -q "needs $orphan\.so"; then
        cp -a "$aside/." "$appdir/"
    fi
    rm -rf "$aside"
done
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
# Where WPE WebKit's library looks for its helpers once quick-sharun has moved its path.
for helper in WPEWebProcess WPENetworkProcess; do
    [ -e "$appdir/lib/${wpe_dir##*/}/$helper" ] || die "no $helper in the bundle's lib/${wpe_dir##*/}"
done
[ -n "$(find "$appdir" -path '*/gio/modules/libgiognutls.so' -print -quit)" ] ||
    die "no TLS module for WPE WebKit's network process in the bundle"
for tool in bwrap xdg-dbus-proxy; do
    [ -e "$appdir/bin/$tool" ] || die "no $tool in the bundle's bin, where WPE WebKit's sandbox looks"
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
# xz with its x86 branch filter: about a tenth smaller than zstd for a tree that is mostly libraries.
"$uruntime" --appimage-mksquashfs "$appdir" "$work/image.squashfs" \
    -comp xz -Xbcj x86 -b 1M -all-root -noappend -no-progress -quiet
cp "$uruntime" "$out"
# Lets AppImageUpdate update Stud in place from the latest release's .zsync.
"$out" --appimage-addupdinfo "gh-releases-zsync|CatPieLeaf|Stud|latest|Stud-$arch.AppImage.zsync"
cat "$work/image.squashfs" >> "$out"
rm -f "$work/image.squashfs"
(cd "$packages" && zsyncmake -u "${out##*/}" -o "${out##*/}.zsync" "${out##*/}")

say "written: $out"
