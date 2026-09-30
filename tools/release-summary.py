#!/usr/bin/env python3
"""A release's changelog, from the # SUMMARY section of its description.

    tools/release-summary.py --metainfo OUT.xml --changelog OUT.txt < body.md

The section starts at a heading that reads SUMMARY (any level, any case)
and ends at the next heading. Its bullet points become the list; any
other line becomes a paragraph. Markdown is reduced to plain text, since
neither AppStream nor rpm renders it.

--metainfo writes an AppStream <description> for the metainfo's
<release>, which CMake reads through STUD_RELEASE_DESCRIPTION_FILE.
--changelog writes "- item" lines for the Terra spec's %changelog entry,
which tools/bump-version.py reads.

With no such section both files are written empty and a warning says so:
a release without a summary is still a release.
"""

import argparse
import html
import re
import sys

HEADING = re.compile(r"^ {0,3}#{1,6}(\s|$)")
SUMMARY = re.compile(r"^ {0,3}#{1,6}\s*summary\s*#*\s*$", re.I)
BULLET = re.compile(r"^ {0,3}(?:[-*+]|\d+[.)])\s+(.*)")


def plain(text: str) -> str:
    text = re.sub(r"<!--.*?-->", "", text)
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)  # [text](url) -> text
    text = re.sub(r"\*\*|__|`", "", text)
    return " ".join(text.split())


def summary(body: str) -> tuple[list[str], list[str]]:
    """The section's paragraphs and bullet points, in order."""
    paragraphs: list[str] = []
    items: list[str] = []
    inside = False
    for line in body.splitlines():
        if HEADING.match(line):
            if inside:
                break
            inside = SUMMARY.match(line) is not None
            continue
        if not inside or not line.strip():
            continue
        bullet = BULLET.match(line)
        if bullet:
            items.append(plain(bullet.group(1)))
        elif items and line[:1] in (" ", "\t"):
            items[-1] = plain(items[-1] + " " + line)  # a wrapped bullet
        else:
            paragraphs.append(plain(line))
    return [p for p in paragraphs if p], [i for i in items if i]


def metainfo(paragraphs: list[str], items: list[str]) -> str:
    if not paragraphs and not items:
        return ""
    lines = ["", "      <description>"]
    lines += [f"        <p>{html.escape(p, quote=False)}</p>" for p in paragraphs]
    if items:
        lines.append("        <ul>")
        lines += [f"          <li>{html.escape(i, quote=False)}</li>" for i in items]
        lines.append("        </ul>")
    lines += ["      </description>", "    "]
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--metainfo", required=True)
    ap.add_argument("--changelog", required=True)
    args = ap.parse_args()

    paragraphs, items = summary(sys.stdin.read())
    if not paragraphs and not items:
        print("::warning::the release description has no # SUMMARY section, "
              "so this release carries no changelog")
    with open(args.metainfo, "w") as out:
        out.write(metainfo(paragraphs, items))
    with open(args.changelog, "w") as out:
        # rpm expands macros in %changelog too, so a literal % is doubled.
        out.writelines(f"- {line.replace('%', '%%')}\n" for line in paragraphs + items)
    print(f"summary: {len(paragraphs)} paragraph(s), {len(items)} item(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
