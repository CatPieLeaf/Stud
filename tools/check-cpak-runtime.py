#!/usr/bin/env python3
"""Check that both cpak images install and configure the same runtime.

packaging/cpak/Containerfile builds Stud from source; Containerfile.prebuilt
assembles the same image from an install tree CI already produced. Only the
prebuilt one is ever built by CI, so a runtime dependency or an environment
variable added to the other reaches nobody. That is what happened to the
portal platform theme and the Breeze style.

Compares the runtime stage of each: the packages its dnf install line names,
and its ENV settings. Everything else legitimately differs.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

CPAK = Path(__file__).resolve().parent.parent / "packaging" / "cpak"


def runtime_stage(text: str) -> str:
    """The last FROM block: the stage that becomes the published image."""
    return re.split(r"^FROM ", text, flags=re.MULTILINE)[-1]


def strip_comments(stage: str) -> str:
    return "\n".join(l for l in stage.splitlines() if not l.lstrip().startswith("#"))


def packages(stage: str) -> set[str]:
    match = re.search(r"^RUN dnf install(.*?)&& dnf clean all",
                      strip_comments(stage), re.MULTILINE | re.DOTALL)
    if not match:
        return set()
    words = match.group(1).replace("\\", " ").split()
    return {w for w in words if not w.startswith("-")}


def env(stage: str) -> set[str]:
    return set(re.findall(r"^ENV\s+(\S+=\S+)", strip_comments(stage), re.MULTILINE))


def main() -> int:
    files = {name: runtime_stage((CPAK / name).read_text())
             for name in ("Containerfile", "Containerfile.prebuilt")}

    problems = []
    for what, extract in (("package", packages), ("env setting", env)):
        source, prebuilt = (extract(files[n]) for n in
                            ("Containerfile", "Containerfile.prebuilt"))
        for missing in sorted(source - prebuilt):
            problems.append(f"Containerfile.prebuilt is missing the {what} {missing!r}"
                            ", the only image CI builds")
        for extra in sorted(prebuilt - source):
            problems.append(f"Containerfile is missing the {what} {extra!r}")

    for problem in problems:
        print(f"cpak runtime drift: {problem}", file=sys.stderr)
    if problems:
        print("\nBoth files describe the same running image; keep their runtime"
              "\nstage identical.", file=sys.stderr)
        return 1

    print("cpak runtime layers match")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
