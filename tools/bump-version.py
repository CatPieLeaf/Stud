#!/usr/bin/env python3
"""Bump Stud's version everywhere, and write a release's changelog entries.

    tools/bump-version.py <version> [--description FILE] [--changelog FILE]

The current version is the one CMakeLists.txt declares; every file below
that names it is rewritten to the new one. That part is skipped when the
tree already has the new version.

The changelog entries are written either way:

  - the metainfo gets this release's <release>, newest first, dated --date,
    with the <description> in FILE; an entry already there for this
    version is replaced, so a re-run writes the same thing;
  - the Terra spec gets a %changelog entry with the "- item" lines in
    FILE, or "- Update to <version>", unless it has one for this version.

tools/release-summary.py writes both files from the ## SUMMARY: section of
the release description. The release workflow runs the two for every
published release, with the version taken from the tag, and commits the
result to main together with the release's checksums
(tools/fill-release-checksums.py, which also regenerates the .SRCINFO
files and cpak.lock.json this touches).
"""

import argparse
import datetime
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Every file that carries the version as text. The metainfo and the spec
# are not here: their entries are written below.
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
SPEC = ROOT / "packaging/terra/stud.spec"
METAINFO = ROOT / "packaging/stud.metainfo.xml.in"


def read(path: str | None) -> str:
    return Path(path).read_text().strip() if path else ""


def bump(old: str, new: str) -> bool:
    # Not part of a longer number: 1.1.1 must not match inside 1.1.10.
    version = re.compile(r"(?<![\d.])" + re.escape(old) + r"(?!\d)")
    for name in FILES:
        path = ROOT / name
        text, n = version.subn(new, path.read_text())
        if n == 0:
            print(f"{name}: no {old} found", file=sys.stderr)
            return False
        path.write_text(text)
        print(f"{name}: {n}")
    text, n = re.subn(r"^(Version:\s*)\S+", rf"\g<1>{new}", SPEC.read_text(), count=1, flags=re.M)
    if n != 1:
        print(f"{SPEC.name}: no Version:", file=sys.stderr)
        return False
    SPEC.write_text(text)
    print(f"{SPEC.name}: Version")
    return True


def metainfo_entry(new: str, date: datetime.date, description: str) -> bool:
    text = METAINFO.read_text()
    # This version's entry, self-closed or with a description.
    text = re.sub(r'    <release version="' + re.escape(new) + r'"[^>]*?(?:/>|>.*?</release>)\n',
                  "", text, flags=re.S)
    entry = (f'    <release version="{new}" date="{date.isoformat()}">\n      {description}\n    </release>\n'
             if description else f'    <release version="{new}" date="{date.isoformat()}"/>\n')
    text, n = re.subn(r"(  <releases>\n)", lambda m: m.group(1) + entry, text, count=1)
    if n != 1:
        print(f"{METAINFO.name}: no <releases>", file=sys.stderr)
        return False
    METAINFO.write_text(text)
    print(f"{METAINFO.name}: {new} entry")
    return True


def spec_entry(new: str, date: datetime.date, changelog: str) -> bool:
    head, sep, entries = SPEC.read_text().partition("%changelog\n")
    if re.search(r"^\* .* - " + re.escape(new) + r"-\d+$", entries, re.M):
        print(f"{SPEC.name}: already has a {new} entry")
        return True
    # Signed like the latest entry.
    who = re.match(r"\* \w{3} \w{3} \d{2} \d{4} (.+?) - ", entries)
    if not sep or who is None:
        print(f"{SPEC.name}: no %changelog entry to follow", file=sys.stderr)
        return False
    entry = f"* {date.strftime('%a %b %d %Y')} {who.group(1)} - {new}-1\n{changelog or f'- Update to {new}'}\n\n"
    SPEC.write_text(head + sep + entry + entries)
    print(f"{SPEC.name}: {new} changelog entry")
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("version")
    ap.add_argument("--description", help="the metainfo entry's <description>")
    ap.add_argument("--changelog", help="the spec entry's \"- item\" lines")
    ap.add_argument("--date", type=datetime.date.fromisoformat, default=datetime.date.today(),
                    help="the release date, YYYY-MM-DD (default: today)")
    args = ap.parse_args()
    new = args.version.removeprefix("v")
    if not re.fullmatch(r"\d+(\.\d+)+", new):
        print(f"not a version: {args.version}", file=sys.stderr)
        return 2
    old = re.search(r"^project\(Stud VERSION ([0-9.]+)",
                    (ROOT / "CMakeLists.txt").read_text(), re.M).group(1)
    if old == new:
        print(f"already {new}, version left alone")
    elif not bump(old, new):
        return 1
    ok = (metainfo_entry(new, args.date, read(args.description))
          and spec_entry(new, args.date, read(args.changelog)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
