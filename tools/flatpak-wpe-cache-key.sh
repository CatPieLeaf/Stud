#!/bin/sh
# Usage: tools/flatpak-wpe-cache-key.sh
# Prints the Actions cache key for the Flatpak's modules before Rosemary (WPE WebKit
# and what it needs). flatpak-builder rebuilds them when they, the runtime, the SDK
# or flatpak-builder itself change, so the key covers all four. The manifest is read
# as flatpak-builder parses it, so comments and formatting leave the key alone.
# Needs the flathub remote.
set -eu
manifest="$(cd "$(dirname "$0")/.." && pwd)/packaging/flatpak/io.github.catpieleaf.Stud.yml"
parsed=$(flatpak-builder --show-manifest "$manifest")

# The manifest's own settings and the modules before Rosemary, in a stable order.
build=$(printf '%s' "$parsed" | python3 -c '
import json, sys
manifest = json.load(sys.stdin)
modules = manifest.pop("modules")
names = [module.get("name") if isinstance(module, dict) else module for module in modules]
if "rosemary" not in names:
    sys.exit("no rosemary module in the manifest")
manifest["modules"] = modules[:names.index("rosemary")]
print(json.dumps(manifest, sort_keys=True))
')
field() { printf '%s' "$parsed" | python3 -c 'import json, sys; print(json.load(sys.stdin)[sys.argv[1]])' "$1"; }
version=$(field runtime-version)
arch=$(flatpak --default-arch)
# Each lookup on its own, so a failed one stops the script instead of hashing less.
runtime=$(flatpak remote-info --system --show-commit flathub "runtime/$(field runtime)/$arch/$version")
sdk=$(flatpak remote-info --system --show-commit flathub "runtime/$(field sdk)/$arch/$version")
builder=$(flatpak-builder --version)
sum=$(printf '%s\n' "$build" "$runtime" "$sdk" "$builder" | sha256sum | cut -c1-16)
echo "flatpak-wpe-$sum"
