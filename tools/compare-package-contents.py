#!/usr/bin/env python3
"""Check that a built package carries exactly the install tree.

Every package Stud ships is a wrapper around one `cmake --install` tree.
They are supposed to hold the same files, byte for byte -- and when they
have not, it was found by someone installing a package and noticing
something missing, one piece at a time. This compares the payload
against the tree instead, so a package that is short a file fails the
build that made it.

Arch relocates /usr/libexec/stud to /usr/lib/stud (namcap rejects
libexec), so that one mapping is applied before comparing rather than
being reported as hundreds of differences.
"""

import argparse
import hashlib
import subprocess
import sys
import tarfile
from pathlib import Path


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def tree_contents(root: Path) -> dict[str, str]:
    """Every regular file in the reference tree, by path, with its digest."""
    out = {}
    for p in root.rglob("*"):
        if p.is_symlink() or not p.is_file():
            continue
        out["/" + str(p.relative_to(root))] = sha256(p)
    return out


def extract(package: Path, kind: str, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    if kind == "rpm":
        cpio = subprocess.run(["rpm2cpio", str(package)], check=True, stdout=subprocess.PIPE)
        subprocess.run(["cpio", "-idmu", "--quiet", "-D", str(dest)],
                       input=cpio.stdout, check=True)
    elif kind == "deb":
        subprocess.run(["dpkg-deb", "-x", str(package), str(dest)], check=True)
    elif kind == "arch":
        with tarfile.open(package, "r:*") as tf:
            members = [m for m in tf.getmembers() if not m.name.startswith(".")]
            tf.extractall(dest, members=members, filter="tar")
    else:
        raise SystemExit(f"unknown package kind {kind}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", required=True, type=Path, help="DESTDIR of cmake --install")
    ap.add_argument("--package", required=True, type=Path)
    ap.add_argument("--kind", required=True, choices=["rpm", "deb", "arch"])
    ap.add_argument("--work", required=True, type=Path)
    # For a format this machine cannot unpack itself: a deb needs dpkg,
    # which the release runner does not have, so it is extracted in a
    # real Debian container and the result handed here already unpacked.
    ap.add_argument("--already-extracted", action="store_true",
                    help="--work already holds the unpacked payload")
    args = ap.parse_args()

    reference = tree_contents(args.tree)
    if not reference:
        print(f"::error::the reference tree {args.tree} is empty")
        return 1

    if not args.already_extracted:
        extract(args.package, args.kind, args.work)
    packaged = tree_contents(args.work)
    if not packaged:
        print(f"::error::{args.package.name}: the unpacked payload is empty")
        return 1

    if args.kind == "arch":
        # The one relocation Arch packaging requires, applied to the
        # reference rather than treated as a difference.
        reference = {p.replace("/usr/libexec/stud", "/usr/lib/stud", 1): d
                     for p, d in reference.items()}

    missing = sorted(set(reference) - set(packaged))
    extra = sorted(set(packaged) - set(reference))
    changed = sorted(p for p in set(reference) & set(packaged)
                     if reference[p] != packaged[p])

    print(f"{args.package.name}: {len(packaged)} file(s) against "
          f"{len(reference)} in the install tree")

    # Files a packaging system adds itself are not a discrepancy.
    ignorable = ("/usr/share/doc/stud/changelog", "/usr/share/doc/stud/copyright.gz")
    extra = [p for p in extra if not p.startswith(ignorable)]

    status = 0
    for label, items in (("missing from the package", missing),
                         ("in the package but not the install tree", extra),
                         ("different bytes", changed)):
        if items:
            status = 1
            print(f"::error::{args.package.name}: {len(items)} file(s) {label}")
            for p in items[:25]:
                print(f"    {p}")
            if len(items) > 25:
                print(f"    ... and {len(items) - 25} more")
    if status == 0:
        print(f"  identical to the install tree")
    return status


if __name__ == "__main__":
    sys.exit(main())
