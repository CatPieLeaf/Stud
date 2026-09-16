#!/usr/bin/env python3
"""Check the Arch packages declare the same, and the right, dependencies.

Three PKGBUILDs describe one package: PKGBUILD builds from source,
PKGBUILD.stud-bin is what the AUR serves, and PKGBUILD.bin is what CI turns
an install tree into. A dependency named by only some of them is a broken
package for whoever installs the other one, and PKGBUILD.bin drifted exactly
that way: it shipped 1.1.1 without libglvnd or libxext.

Two checks:

  * the three depends and optdepends lists are identical. This is the one
    that fails the build, and it is what PKGBUILD.bin broke.
  * with --tree, which package owns each library the built binaries link.
    A soname no package owns is a real error: it would not resolve on a
    user's machine either. One whose owner is undeclared is reported and
    does not fail, because Arch satisfies it transitively (libstdc++ and
    libgcc through gcc-libs, the Qt libraries through qt6-webengine) and
    namcap accepts exactly that. Read the list when adding a dependency;
    declaring what is linked directly is still better packaging.

Resolving a soname needs Arch's own file database, so that half runs only
where pacman does and says plainly when it is skipped.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

AUR = Path(__file__).resolve().parent.parent / "packaging" / "aur"
PKGBUILDS = ("PKGBUILD", "PKGBUILD.stud-bin", "PKGBUILD.bin")


def array(text: str, name: str) -> list[str]:
    """The entries of a bash array literal, comments and quotes removed."""
    match = re.search(rf"^{name}=\((.*?)^\)", text, re.MULTILINE | re.DOTALL)
    if not match:
        return []
    body = "\n".join(l for l in match.group(1).splitlines()
                     if not l.lstrip().startswith("#"))
    return [e.strip("'\"") for e in re.findall(r"'[^']*'|\"[^\"]*\"", body)]


def check_lists() -> list[str]:
    declared = {name: {field: array((AUR / name).read_text(), field)
                       for field in ("depends", "optdepends")}
                for name in PKGBUILDS}
    problems = []
    reference = PKGBUILDS[0]
    for field in ("depends", "optdepends"):
        expected = set(declared[reference][field])
        for name in PKGBUILDS[1:]:
            found = set(declared[name][field])
            for missing in sorted(expected - found):
                problems.append(f"{name} does not declare the {field} {missing!r}"
                                f", which {reference} does")
            for extra in sorted(found - expected):
                problems.append(f"{reference} does not declare the {field} {extra!r}"
                                f", which {name} does")
    return problems


def needed_sonames(tree: Path) -> tuple[set[str], set[str]]:
    """What the tree's ELFs import, and what the tree itself provides."""
    needed: set[str] = set()
    provided: set[str] = set()
    for path in tree.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open("rb") as handle:
            if handle.read(4) != b"\x7fELF":
                continue
        provided.add(path.name)
        # Anything under android-bionic or lib64 is Android's own, resolved by
        # Stud's linker64 inside the sandbox and never by the host loader.
        if {"android-bionic", "lib64"} & set(path.parts):
            continue
        out = subprocess.run(["readelf", "-d", str(path)],
                             capture_output=True, text=True).stdout
        needed |= set(re.findall(r"Shared library: \[([^\]]+)\]", out))
    return needed, provided


def owning_packages(sonames: set[str]) -> dict[str, str]:
    """Which Arch package ships each soname, via pacman's file database."""
    subprocess.run(["pacman", "-Fy"], capture_output=True)
    owners = {}
    for soname in sorted(sonames):
        out = subprocess.run(["pacman", "-F", f"usr/lib/{soname}"],
                             capture_output=True, text=True).stdout
        # "usr/lib/libX11.so.6 is owned by extra/libx11 1.8.13-1"
        match = re.search(r"is owned by \S+/([\w.+-]+)\s", out)
        if match:
            owners[soname] = match.group(1)
    return owners


def check_tree(tree: Path) -> list[str]:
    if not shutil.which("pacman"):
        print("skipping the linkage check: no pacman, so a soname cannot be"
              " resolved to a package (run this on Arch)", file=sys.stderr)
        return []
    needed, provided = needed_sonames(tree)
    wanted = needed - provided
    owners = owning_packages(wanted)
    declared = set(array((AUR / PKGBUILDS[0]).read_text(), "depends"))

    undeclared = sorted((soname, owners[soname]) for soname in owners
                        if owners[soname] not in declared)
    if undeclared:
        print("linked directly, satisfied only through another package's own"
              " dependencies:", file=sys.stderr)
        for soname, package in undeclared:
            print(f"  {soname} -> {package}", file=sys.stderr)

    return [f"no Arch package owns {soname}, linked by the installed binaries"
            for soname in sorted(wanted - set(owners))]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path,
                        help="an install tree, to check what its ELFs link")
    args = parser.parse_args()

    problems = check_lists()
    if args.tree:
        problems += check_tree(args.tree)

    for problem in problems:
        print(f"AUR dependency drift: {problem}", file=sys.stderr)
    if problems:
        print("\nAll three PKGBUILDs describe one package; keep their"
              "\ndependency lists identical, and matching what is linked.",
              file=sys.stderr)
        return 1

    print(f"the {len(PKGBUILDS)} Arch packages declare the same dependencies"
          + (", and every library they link" if args.tree else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
