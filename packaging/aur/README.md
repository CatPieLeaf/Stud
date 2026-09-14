# AUR packaging

`PKGBUILD` and `.SRCINFO` for the [`stud`](https://aur.archlinux.org/packages/stud)
package. They live here rather than only in the AUR repository so the
packaging is versioned with the code it builds -- a dependency added to
Stud and not to the PKGBUILD is a broken package, and that is easier to
notice in the same commit.

## Publishing a release

The AUR repository is a separate git repository, and the only two files
it carries are these:

```sh
git clone ssh://aur@aur.archlinux.org/stud.git aur-stud
cp packaging/aur/PKGBUILD packaging/aur/.SRCINFO aur-stud/
cd aur-stud
# The checksum of the tarball GitHub actually serves for this tag.
updpkgsums && makepkg --printsrcinfo > .SRCINFO
git commit -am "stud 1.1.0" && git push
```

`sha256sums` is `SKIP` in the tree on purpose: the real checksum belongs
to one specific tarball, and a stale one in version control is worse than
none. `updpkgsums` fills it in at publish time, and CI does the same when
it validates the build.

## Why it takes an hour

ANGLE, compiled from source. Stud needs both its Vulkan and its
SwiftShader backends, and no distribution ships that combination -- so
`tools/setup.sh` builds it. Everything else in the build is minutes.

A prebuilt `pkg.tar.zst` is attached to every GitHub release for anyone
who would rather not wait.
