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
# Each lookup on its own, so a failed one stops the script instead of hashing less.
runtime=$(flatpak remote-info --system --show-commit flathub "runtime/$(field runtime)/$arch/$version")
sdk=$(flatpak remote-info --system --show-commit flathub "runtime/$(field sdk)/$arch/$version")
builder=$(flatpak-builder --version)
sum=$({
    sed '/^  - name: rosemary$/,$d' "$manifest"
    printf '%s\n' "$runtime" "$sdk" "$builder"
} | sha256sum | cut -c1-16)
echo "flatpak-wpe-$sum"
