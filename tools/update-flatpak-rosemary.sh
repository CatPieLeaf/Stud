#!/bin/sh
# Usage: tools/update-flatpak-rosemary.sh WORKDIR
# Points the Flatpak manifest at the head of Rosemary's branch and regenerates the
# offline Cargo sources (Flatpak builds have no network). Leaves the checkout in
# WORKDIR/rosemary for the shader check and prints the commit.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
work=${1:?usage: $0 WORKDIR}
manifest="$root/packaging/flatpak/io.github.catpieleaf.Stud.yml"

repo=$(sed -n 's/^set(STUD_ROSEMARY_REPOSITORY "\([^"]*\)".*/\1/p' "$root/CMakeLists.txt")
branch=$(sed -n 's/^set(STUD_ROSEMARY_BRANCH "\([^"]*\)".*/\1/p' "$root/CMakeLists.txt")

mkdir -p "$work"
rm -rf "$work/rosemary"
git clone -q --depth 1 --branch "$branch" "$repo" "$work/rosemary"
commit=$(git -C "$work/rosemary" rev-parse HEAD)

# Flathub's documented generator.
python3 -m venv "$work/venv"
"$work/venv/bin/pip" install -q aiohttp tomlkit
curl -fsSL --retry 5 --retry-all-errors -o "$work/flatpak-cargo-generator.py" \
    https://raw.githubusercontent.com/flatpak/flatpak-builder-tools/master/cargo/flatpak-cargo-generator.py
"$work/venv/bin/python" "$work/flatpak-cargo-generator.py" "$work/rosemary/Cargo.lock" \
    -o "$root/packaging/flatpak/rosemary-cargo-sources.json"

# The commit sits on the line after the fork's url.
sed -i "\#url: $repo\$#{n;s/^\( *commit: \).*/\1$commit/}" "$manifest"
grep -q "commit: $commit" "$manifest" || { echo "no Rosemary commit line in $manifest" >&2; exit 1; }

# Current stable Rust from Rust's channel listing, used only to build: Flathub's
# Rust extension can trail what Rosemary needs.
python3 - "$manifest" <<'PY'
import re, sys, tomllib, urllib.request
manifest = sys.argv[1]
with urllib.request.urlopen("https://static.rust-lang.org/dist/channel-rust-stable.toml") as r:
    channel = tomllib.loads(r.read().decode())
target = channel["pkg"]["rust"]["target"]["x86_64-unknown-linux-gnu"]
url, sha = target["xz_url"], target["xz_hash"]
text = open(manifest).read()
text, n = re.subn(r"(url: )https://static\.rust-lang\.org/dist/\S+(\n *sha256: )\S+",
                  lambda m: m.group(1) + url + m.group(2) + sha, text)
if n != 1:
    sys.exit(f"no Rust archive source in {manifest}")
open(manifest, "w").write(text)
print(f"rust: {channel['pkg']['rust']['version']}", file=sys.stderr)
PY
echo "$commit"
