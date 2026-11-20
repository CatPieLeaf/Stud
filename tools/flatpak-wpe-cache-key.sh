#!/bin/sh
# Usage: tools/flatpak-wpe-cache-key.sh
# Prints the Actions cache key for the Flatpak's modules before Rosemary (WPE WebKit
# and what it needs). flatpak-builder rebuilds them when they, the runtime, the SDK
# or flatpak-builder itself change, so the key covers all four. Needs the flathub remote.
set -eu
manifest="$(cd "$(dirname "$0")/.." && pwd)/packaging/flatpak/io.github.catpieleaf.Stud.yml"
grep -q '^  - name: rosemary$' "$manifest" || { echo "no rosemary module in $manifest" >&2; exit 1; }

field() { sed -n "s/^$1: '\{0,1\}\([^']*\)'\{0,1\}$/\1/p" "$manifest"; }
version=$(field runtime-version)
arch=$(flatpak --default-arch)
sum=$({
    sed '/^  - name: rosemary$/,$d' "$manifest"
    for ref in "$(field runtime)" "$(field sdk)"; do
        flatpak remote-info --system --show-commit flathub "runtime/$ref/$arch/$version"
    done
    flatpak-builder --version
} | sha256sum | cut -c1-16)
echo "flatpak-wpe-$sum"
