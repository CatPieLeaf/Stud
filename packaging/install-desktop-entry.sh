#!/bin/bash
# Installs Stud's desktop entry and icon into the user's own
# ~/.local/share tree. Re-runnable; safe to run after every build.
#
# The icon has to land in the hicolor theme under the exact name the desktop
# entry's Icon= key uses (the application id), at real sizes, a bare Icon=stud with
# nothing installed silently renders as no icon at all, which is what Stud
# shipped with.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
stud_ui="${1:-$here/../build/ui/stud-ui}"
if [[ ! -x "$stud_ui" ]]; then
    echo "install-desktop-entry.sh: no stud-ui at $stud_ui (build first, or pass its path)" >&2
    exit 1
fi
stud_ui="$(readlink -f "$stud_ui")"

# The application id, read from the one place that defines it, so a
# build-tree install and a packaged one register the same entry.
app_id="$(grep -m1 '^set(STUD_APP_ID' "$here/../CMakeLists.txt" | cut -d'"' -f2)"
: "${app_id:?could not read STUD_APP_ID out of CMakeLists.txt}"

apps="$HOME/.local/share/applications"
icons="$HOME/.local/share/icons/hicolor"
mkdir -p "$apps"

for size in 16 24 32 48 64 128 256 512; do
    dir="$icons/${size}x${size}/apps"
    mkdir -p "$dir"
    if command -v magick >/dev/null 2>&1; then
        magick "$here/stud.png" -resize "${size}x${size}" "$dir/$app_id.png"
    else
        cp "$here/stud.png" "$dir/$app_id.png"
    fi
done
cp "$here/stud.png" "$icons/512x512/apps/$app_id.png"

# Some icon loaders (KDE's among them) will not read a theme directory that
# has no index.theme of its own, even though the same theme is indexed in
# /usr/share/icons. Without this the icons above are installed correctly and
# still never resolve.
if [[ ! -f "$icons/index.theme" ]]; then
    {
        echo "[Icon Theme]"
        echo "Name=hicolor"
        echo "Comment=Fallback icon theme"
        echo "Directories=$(printf '%s/apps,' 16x16 24x24 32x32 48x48 64x64 128x128 256x256 512x512 | sed 's/,$//')"
        for size in 16 24 32 48 64 128 256 512; do
            echo
            echo "[${size}x${size}/apps]"
            echo "Size=$size"
            echo "Context=Applications"
            echo "Type=Threshold"
        done
    } > "$icons/index.theme"
fi

# Icon= is written as an ABSOLUTE PATH rather than the theme name "stud".
# The themed name does resolve (verified with a real icon-theme lookup), but it
# depends on every consumer picking up a newly-added user icon directory, and
# an absolute path is what actually shows up reliably; it is also what other
# user-installed entries on this system do. The hicolor copies above are still
# installed, because that is the correct thing for anything that does use the
# theme.
icon_path="$icons/512x512/apps/$app_id.png"
sed -e "s|@STUD_EXEC@|$stud_ui|g" -e "s|@STUD_APP_ID@|$app_id|g" \
    -e "s|^Icon=.*$|Icon=$icon_path|" \
    "$here/stud.desktop.in" | grep -v '^#' > "$apps/$app_id.desktop"
chmod 644 "$apps/$app_id.desktop"

command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -f -t "$icons" || true
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$apps" || true
command -v xdg-desktop-menu >/dev/null 2>&1 && xdg-desktop-menu forceupdate || true
# KDE keeps its own service/desktop-entry index; a new .desktop is invisible to
# the launcher and to window-to-icon matching until this is rebuilt.
for k in kbuildsycoca6 kbuildsycoca5; do
    if command -v "$k" >/dev/null 2>&1; then
        "$k" --noincremental 2>&1 | sed "s/^/$k: /"
        break
    fi
done

echo "installed: $apps/$app_id.desktop"
echo "installed: $icons/{16..512}/apps/$app_id.png"
