# Flatpak / Flathub

`io.github.catpieleaf.Stud.yml` is the manifest. Flathub wants it in its
own repository (`flathub/io.github.catpieleaf.Stud`), so this copy is the
source of truth and that repository carries a copy, the same arrangement
as `packaging/aur`.

## Rosemary comes prebuilt, and it has to

Rosemary is built with cargo, and its Microlift dependencies (and
Microlift's wasmtime) are git dependencies cargo fetches while it builds.
**Flathub builds have no network**, so the manifest installs the Rosemary
that CI built for the release, from the release archive, with a `sha256`.
The launcher is the one part built here, against the runtime's own Qt.

If a from-source build is ever required, the way to provide one is to
vendor Rosemary's crates (`cargo vendor`) as a source bundle. It is
possible; it is a much larger manifest.

## What the manifest builds

- **SDL3.** Rosemary needs SDL 3.4 or newer, and the KDE runtime's SDL3
  is older, so the manifest builds it. Rosemary opens SDL3 by name, and
  `/app/lib` is searched before the runtime's.
- **wl-clipboard**, for copying a server link from the tray menu.
- **The launcher**, with `STUD_BUILD_ROSEMARY=OFF`.

FFmpeg comes from the `org.freedesktop.Platform.ffmpeg-full` extension,
mounted at `/app/lib/ffmpeg`, because the runtime's own leaves H.264 out.
Its `version` follows the freedesktop base under the KDE runtime.

## The one placeholder

Stud's own release archive is the only value CI fills in: that archive
does not exist until the release it belongs to is published. Every
dependency checksum is real and committed, because Flathub builds the
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
flatpak install -y flathub org.kde.Sdk//6.11 org.kde.Platform//6.11 io.qt.qtwebengine.BaseApp//6.11
flatpak-builder --force-clean --user --install build-flatpak \
  packaging/flatpak/io.github.catpieleaf.Stud.yml
flatpak run io.github.catpieleaf.Stud
```
