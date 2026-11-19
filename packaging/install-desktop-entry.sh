#!/bin/bash
# Installs Stud's desktop entry and icons into ~/.local/share. Safe to re-run.
# The icon must be in hicolor under the application id, or the entry shows none.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
stud_ui="${1:-$here/../build/ui/stud-ui}"
if [[ ! -x "$stud_ui" ]]; then
    echo "install-desktop-entry.sh: no stud-ui at $stud_ui (build first, or pass its path)" >&2
    exit 1
fi
stud_ui="$(readlink -f "$stud_ui")"

# Read from CMakeLists.txt so build-tree and packaged installs agree.
app_id="$(grep -m1 '^set(STUD_APP_ID' "$here/../CMakeLists.txt" | cut -d'"' -f2)"
: "${app_id:?could not read STUD_APP_ID out of CMakeLists.txt}"

apps="$HOME/.local/share/applications"
icons="$HOME/.local/share/icons/hicolor"
mkdir -p "$apps"

install_icon() {
    local source=$1 name=$2
    for size in 16 24 32 48 64 128 256 512; do
        dir="$icons/${size}x${size}/apps"
        mkdir -p "$dir"
        if command -v magick >/dev/null 2>&1; then
            magick "$source" -resize "${size}x${size}" "$dir/$name.png"
        else
            cp "$source" "$dir/$name.png"
        fi
    done
    cp "$source" "$icons/512x512/apps/$name.png"
}
install_icon "$here/stud.png" "$app_id"
install_icon "$here/../ui/resources/studred.png" "$app_id-player"
install_icon "$here/../ui/resources/studblu.png" "$app_id-studio"

# KDE's loader ignores a user theme directory without its own index.theme.
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

# An absolute Icon= path shows up at once; a theme name waits for every consumer
# to notice the new icon directory. The hicolor copies serve anything using the theme.
icon_path="$icons/512x512/apps/$app_id.png"
# The installed entries, pointed at this build and at the icons above.
for entry in "$app_id" "$app_id.Player" "$app_id.Studio"; do
    sed -e "s|^Exec=stud|Exec=$stud_ui|" \
        -e "s|^Icon=$app_id-player$|Icon=$icons/512x512/apps/$app_id-player.png|" \
        -e "s|^Icon=$app_id-studio$|Icon=$icons/512x512/apps/$app_id-studio.png|" \
        -e "s|^Icon=$app_id$|Icon=$icon_path|" \
        "$here/$entry.desktop" | grep -v '^#' > "$apps/$entry.desktop"
    chmod 644 "$apps/$entry.desktop"
done
mime="$HOME/.local/share/mime"
mkdir -p "$mime/packages"
cp "$here/stud-mime.xml" "$mime/packages/$app_id.xml"
command -v update-mime-database >/dev/null 2>&1 && update-mime-database "$mime" || true

command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -f -t "$icons" || true
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$apps" || true
command -v xdg-desktop-menu >/dev/null 2>&1 && xdg-desktop-menu forceupdate || true
# KDE won't show a new entry, or match windows to its icon, until its index is rebuilt.
for k in kbuildsycoca6 kbuildsycoca5; do
    if command -v "$k" >/dev/null 2>&1; then
        "$k" --noincremental 2>&1 | sed "s/^/$k: /"
        break
    fi
done

echo "installed: $apps/$app_id.desktop"
echo "installed: $icons/{16..512}/apps/$app_id.png"
