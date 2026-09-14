# Flatpak / Flathub

`io.github.catpieleaf.Stud.yml` is the manifest. Flathub wants it in its
own repository (`flathub/io.github.catpieleaf.Stud`), so this copy is the
source of truth and that repository carries a copy — the same arrangement
as `packaging/aur`.

## It installs a prebuilt archive, and it has to

Stud compiles ANGLE from source because it needs both the Vulkan and the
SwiftShader backends and no distribution ships that pair. ANGLE's own
build runs `gclient sync`, which downloads dozens of dependencies while it
runs, and **Flathub builds have no network**. There is no self-contained
ANGLE source tarball to pin instead, so a from-source manifest cannot be
written today.

[Sober](https://github.com/flathub/org.vinegarhq.Sober) — the same kind of
application — resolves this the same way, shipping a prebuilt archive from
its own server with a `sha256`. Every permission in the manifest follows
theirs, with a comment saying why.

If a from-source build is ever required, the way to provide one is to
publish a vendored ANGLE checkout (post-`gclient sync`, several GB) as a
pinned source bundle. It is possible; it is a much larger build.

## The one thing to check before submitting

`--allow=devel`. Process B runs inside its own `bwrap`, with its own user
namespace — that is Stud's architecture, and it is why the GPU driver
never shares a process with the engine. A nested sandbox needs this
permission. Sober is granted it (for ptrace), so the precedent exists, but
expect a reviewer to ask, and the answer is the architecture rather than
convenience.

## The one placeholder

`PLACEHOLDER_STUD_SHA256` is Stud's own release archive, and it is the
only value CI fills in: that archive does not exist until the release it
belongs to is published. Every dependency checksum is real and committed,
because Flathub builds the manifest exactly as it stands in the tree, a
manifest that only works once CI has rewritten it is one they cannot
build at all.

Changing a dependency's version means changing its checksum in the same
commit. `flatpak-builder` refuses a mismatch, so a stale one fails the
build rather than installing the wrong thing.

## Filesystem access

None is requested. The APK is chosen through a `QFileDialog`, which Qt
routes to the file chooser portal inside a sandbox; the portal returns a
path under `/run/user/<uid>/doc/` that stays readable afterwards. Confirm
that on the first real build: pick an APK, relaunch, and check it is
still readable. If it is not, the fix is `--filesystem=xdg-download:ro`,
never a wider grant.

## Testing it locally

```sh
flatpak install -y flathub org.kde.Sdk//6.10 org.kde.Platform//6.10
flatpak-builder --force-clean --user --install build-flatpak \
  packaging/flatpak/io.github.catpieleaf.Stud.yml
flatpak run io.github.catpieleaf.Stud
```
