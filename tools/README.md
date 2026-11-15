# tools

Two kinds of thing live here: the release pipeline's own scripts, and code
the build shares.

- **`spinning_cube.h`**: the spinning stud on the welcome page and in
  Settings' About tab, drawn with plain OpenGL.

- **`bump-version.py`**: writes a new version into every file that names
  it, and writes the release's entry in the metainfo and the Terra spec's
  changelog. The release workflow runs it with the tag's version and commits
  the result.

- **`release-summary.py`**: reads the `## SUMMARY:` section of a release
  description, for the metainfo's `<release>` entry and the Terra spec's
  changelog entry, which `bump-version.py` writes. The release workflow
  runs both.

- **`fill-release-checksums.py`**: after a release is published, writes
  its digests into the two AUR PKGBUILDs and the Flatpak manifest, pins
  the cpak image, regenerates `cpak.lock.json` and both `.SRCINFO` files.
  `--check` reports what is still a placeholder.

- **`update-flatpak-rosemary.sh`**: points the Flatpak manifest's Rosemary
  module at the head of the fork's branch and regenerates its offline Cargo
  sources (`packaging/flatpak/rosemary-cargo-sources.json`). The release
  workflow runs it before every Flatpak build and after each release.

- **`validate-payload.py`**, **`compare-package-contents.py`**,
  **`check-aur-depends.py`** and **`check-cpak-runtime.py`**: the
  release's package checks. `validate-payload.py --rosemary-source` also
  checks Rosemary embeds every upscaler shader it was built from.

Nothing here inspects or unpacks the Roblox client.
