#!/usr/bin/env python3
"""Put a release's real checksums into the AUR and Flatpak recipes.

Three files name an archive by URL and carry a checksum for it: the two
AUR PKGBUILDs and the Flathub manifest. The checksums cannot live in the
tree, because they belong to one specific release, re-cutting a tag
changes both, so they sit as placeholders until a release exists, and
this fills them in from the one that does.

Run it after a release is published, before submitting to the AUR or
Flathub:

    tools/fill-release-checksums.py 1.1.0

With no network (or to redo it from values already in hand):

    tools/fill-release-checksums.py 1.1.0 --source-sha <hex> --archive-sha <hex>

`--check` reports what is filled in and what is still a placeholder,
without writing anything.
"""

import argparse
import hashlib
import re
import sys
import urllib.request
from pathlib import Path

REPO = "CatPieLeaf/Stud"
ROOT = Path(__file__).resolve().parent.parent

MANIFEST = ROOT / "packaging/flatpak/io.github.catpieleaf.Stud.yml"
PKGBUILD = ROOT / "packaging/aur/PKGBUILD"
PKGBUILD_BIN = ROOT / "packaging/aur/PKGBUILD.stud-bin"

HEX = re.compile(r"^[0-9a-f]{64}$")


def digest_of(url: str) -> str:
    h = hashlib.sha256()
    with urllib.request.urlopen(url) as response:
        for chunk in iter(lambda: response.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("tag", help="the published tag, e.g. 1.1.0")
    ap.add_argument("--source-sha", help="sha256 of the tag's source tarball")
    ap.add_argument("--archive-sha", help="sha256 of stud-<tag>-x86_64.tar.zst")
    ap.add_argument("--check", action="store_true", help="report, do not write")
    args = ap.parse_args()

    if args.check:
        for path in (MANIFEST, PKGBUILD, PKGBUILD_BIN):
            text = path.read_text()
            pending = text.count("PLACEHOLDER_") + text.count("'SKIP'")
            print(f"{path.relative_to(ROOT)}: {'filled' if pending == 0 else f'{pending} placeholder(s)'}")
        return 0

    source_sha = args.source_sha
    archive_sha = args.archive_sha
    if source_sha is None:
        source_sha = digest_of(f"https://github.com/{REPO}/archive/refs/tags/{args.tag}.tar.gz")
        print(f"source tarball:  {source_sha}")
    if archive_sha is None:
        archive_sha = digest_of(
            f"https://github.com/{REPO}/releases/download/{args.tag}/stud-{args.tag}-x86_64.tar.zst")
        print(f"release archive: {archive_sha}")
    for name, value in (("--source-sha", source_sha), ("--archive-sha", archive_sha)):
        if not HEX.match(value):
            print(f"{name} is not a sha256: {value}", file=sys.stderr)
            return 1

    # The Flathub manifest: one archive each, named by its own placeholder.
    text = MANIFEST.read_text()
    for placeholder, value in (("PLACEHOLDER_STUD_SHA256", archive_sha),
                               ("PLACEHOLDER_SOURCE_SHA256", source_sha)):
        if placeholder not in text:
            print(f"{MANIFEST.name}: no {placeholder}, already filled?", file=sys.stderr)
            return 1
        text = text.replace(placeholder, value)
    MANIFEST.write_text(text)

    # The source PKGBUILD has one source: the tag's tarball.
    text = PKGBUILD.read_text()
    if text.count("sha256sums=('SKIP')") != 1:
        print(f"{PKGBUILD.name}: expected one SKIP", file=sys.stderr)
        return 1
    PKGBUILD.write_text(text.replace("sha256sums=('SKIP')", f"sha256sums=('{source_sha}')"))

    # stud-bin has two, in source=() order: the release archive, then the
    # tag's tarball for the Qt half it builds itself.
    text = PKGBUILD_BIN.read_text()
    old = "sha256sums=('SKIP'\n            'SKIP')"
    if old not in text:
        print(f"{PKGBUILD_BIN.name}: expected two SKIPs", file=sys.stderr)
        return 1
    PKGBUILD_BIN.write_text(
        text.replace(old, f"sha256sums=('{archive_sha}'\n            '{source_sha}')"))

    print("\nfilled. Regenerate the .SRCINFO files next (they carry the same sums):")
    print("  makepkg --printsrcinfo > packaging/aur/.SRCINFO            # from PKGBUILD")
    print("  makepkg --printsrcinfo > packaging/aur/.SRCINFO.stud-bin   # from PKGBUILD.stud-bin")
    return 0


if __name__ == "__main__":
    sys.exit(main())
