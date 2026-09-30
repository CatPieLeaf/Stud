#!/usr/bin/env python3
"""Bump Stud's version everywhere it is written down.

    tools/bump-version.py <version>

The current version is the one CMakeLists.txt declares; every file below
that names it is rewritten to the new one, and the Terra spec gets a
changelog entry. Nothing happens when the tree already has that version.

The release workflow runs this for every published release, with the
version taken from the tag, and commits the result to main together with
the release's checksums (tools/fill-release-checksums.py), which also
regenerates the .SRCINFO files and cpak.lock.json this touches.
"""

import datetime
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Every file that carries the version as text. The metainfo is not here:
# its one <release> is filled in by CMake from STUD_VERSION at build time.
FILES = (
    "CMakeLists.txt",
    "README.md",
    "cpak.json",
    "cpak.lock.json",
    "packaging/aur/.SRCINFO",
    "packaging/aur/.SRCINFO.stud-bin",
    "packaging/aur/PKGBUILD",
    "packaging/aur/PKGBUILD.bin",
    "packaging/aur/PKGBUILD.stud-bin",
    "packaging/aur/README.md",
    "packaging/cpak/README.md",
    "packaging/flatpak/io.github.catpieleaf.Stud.yml",
    "tools/fill-release-checksums.py",
)
SPEC = "packaging/terra/stud.spec"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    new = sys.argv[1].removeprefix("v")
    if not re.fullmatch(r"\d+(\.\d+)+", new):
        print(f"not a version: {sys.argv[1]}", file=sys.stderr)
        return 2
    old = re.search(r"^project\(Stud VERSION ([0-9.]+)",
                    (ROOT / "CMakeLists.txt").read_text(), re.M).group(1)
    if old == new:
        print(f"already {new}, nothing to bump")
        return 0

    # Not part of a longer number: 1.1.1 must not match inside 1.1.10.
    version = re.compile(r"(?<![\d.])" + re.escape(old) + r"(?!\d)")
    for name in FILES:
        path = ROOT / name
        text, n = version.subn(new, path.read_text())
        if n == 0:
            print(f"{name}: no {old} found", file=sys.stderr)
            return 1
        path.write_text(text)
        print(f"{name}: {n}")

    # The spec's Version, and a changelog entry signed like the latest one.
    # Earlier entries name their own versions and are left alone.
    path = ROOT / SPEC
    head, sep, changelog = path.read_text().partition("%changelog\n")
    head, n = re.subn(r"^(Version:\s*)\S+", rf"\g<1>{new}", head, count=1, flags=re.M)
    who = re.match(r"\* \w{3} \w{3} \d{2} \d{4} (.+?) - ", changelog)
    if n != 1 or not sep or who is None:
        print(f"{SPEC}: no Version: or %changelog entry to follow", file=sys.stderr)
        return 1
    today = datetime.date.today().strftime("%a %b %d %Y")
    entry = f"* {today} {who.group(1)} - {new}-1\n- Update to {new}\n\n"
    path.write_text(head + sep + entry + changelog)
    print(f"{SPEC}: Version and changelog")
    return 0


if __name__ == "__main__":
    sys.exit(main())
