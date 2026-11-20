# Flatpak / Flathub

`io.github.catpieleaf.Stud.yml` is the manifest. Flathub wants it in its
own repository (`flathub/io.github.catpieleaf.Stud`), so this copy is the
source of truth and that repository carries a copy, the same arrangement
as `packaging/aur`.

## Rosemary is built here, offline

Rosemary has to be built against the runtime it runs in: a binary built on
a newer distribution needs that distribution's glibc, and the runtime's is
older (Stud 2.0's first Flatpak failed with `GLIBC_2.43 not found`).
So the manifest builds it from Stud's fork, at the commit it names, with
the current stable Rust: Rust's own release archive, installed into the
build directory and never shipped, because Flathub's Rust extension can
trail the release Rosemary needs (it was 1.98 when Rosemary needed 1.99).

**Flathub builds have no network**, and Rosemary's Microlift dependencies
(and Microlift's wasmtime) are git dependencies cargo would fetch. So every
crate and git checkout it needs is listed in `rosemary-cargo-sources.json`,
generated from that commit's `Cargo.lock` by flatpak-builder-tools'
`flatpak-cargo-generator.py`. `tools/update-flatpak-rosemary.sh` moves the
commit to the head of the fork's branch, regenerates the file, and writes
in the current stable Rust from Rust's channel listing; CI runs
it before every Flatpak build, and after a release so main carries the
Rosemary that release built.

## What the manifest builds

- **SDL3.** Rosemary needs SDL 3.4 or newer, and the KDE runtime's SDL3
  is older, so the manifest builds it. Rosemary opens SDL3 by name, and
  `/app/lib` is searched before the runtime's.
- **wl-clipboard**, for copying a server link from the tray menu.
- **Rosemary**, from the fork, offline, as above. The SDK's `glslc`
  compiles its upscalers' shaders, and CI counts them in the binary.
- **The launcher**, with `STUD_BUILD_ROSEMARY=OFF`.

FFmpeg comes from the `org.freedesktop.Platform.ffmpeg-full` extension,
mounted at `/app/lib/ffmpeg`, because the runtime's own leaves H.264 out.
Its `version` follows the freedesktop base under the KDE runtime.

## What CI fills in

Stud's own source tarball's checksum, which exists only once the release
is published, and Rosemary's commit and Cargo sources, as above. Every
other dependency checksum is real and committed, because Flathub builds the
manifest exactly as it stands in the tree, a manifest that only works once
CI has rewritten it is one they cannot build at all.

Changing a dependency's version means changing its checksum in the same
commit. `flatpak-builder` refuses a mismatch, so a stale one fails the
build rather than installing the wrong thing.

## Filesystem access

None is requested. Customizations and FFlag imports are chosen through a
`QFileDialog`, which Qt routes to the file chooser portal inside a sandbox;
the portal returns a path under `/run/user/<uid>/doc/` that stays readable
afterwards.

## Testing it locally

```sh
flatpak install -y flathub org.kde.Sdk//6.11 org.kde.Platform//6.11
flatpak-builder --force-clean --user --install build-flatpak \
  packaging/flatpak/io.github.catpieleaf.Stud.yml
flatpak run io.github.catpieleaf.Stud
```
