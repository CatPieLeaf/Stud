#!/usr/bin/env python3
"""Put a release's real checksums into the AUR and Flatpak recipes.

Four files name something by URL and carry a digest for it: the two AUR
PKGBUILDs, the Flathub manifest, and the cpak manifest (whose image must
be pinned to a digest, not a tag, before `cpak lock` will accept it). The checksums cannot live in the
tree, because they belong to one specific release, re-cutting a tag
changes both, so they sit as placeholders until a release exists, and
this fills them in from the one that does.

Run it after a release is published, before submitting to the AUR or
Flathub:

    tools/fill-release-checksums.py 1.1.8

With no network (or to redo it from values already in hand):

    tools/fill-release-checksums.py 1.1.8 --source-sha <hex> --archive-sha <hex>

`--check` reports what is filled in and what is still a placeholder,
without writing anything.
"""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

REPO = "CatPieLeaf/Stud"
ROOT = Path(__file__).resolve().parent.parent

MANIFEST = ROOT / "packaging/flatpak/io.github.catpieleaf.Stud.yml"
CPAK = ROOT / "cpak.json"
PKGBUILD = ROOT / "packaging/aur/PKGBUILD"
PKGBUILD_BIN = ROOT / "packaging/aur/PKGBUILD.stud-bin"

HEX = re.compile(r"^[0-9a-f]{64}$")


def digest_of(url: str) -> str:
    h = hashlib.sha256()
    with urllib.request.urlopen(url) as response:
        for chunk in iter(lambda: response.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def registry_digest(repo: str, tag: str) -> str:
    """The published image's own digest, straight from the registry.

    Anonymous pull token, then the manifest's Docker-Content-Digest. Only
    ghcr is handled, which is the only registry this project pushes to;
    anything else returns empty and leaves the image on its tag.
    """
    if not repo.startswith("ghcr.io/"):
        return ""
    name = repo[len("ghcr.io/"):]
    try:
        with urllib.request.urlopen(
                f"https://ghcr.io/token?scope=repository:{name}:pull&service=ghcr.io") as r:
            token = json.load(r)["token"]
        request = urllib.request.Request(
            f"https://ghcr.io/v2/{name}/manifests/{tag}",
            headers={"Authorization": f"Bearer {token}",
                     "Accept": "application/vnd.oci.image.manifest.v1+json, "
                               "application/vnd.oci.image.index.v1+json"})
        with urllib.request.urlopen(request) as r:
            return r.headers.get("Docker-Content-Digest", "")
    except Exception as exc:  # noqa: BLE001, report and carry on
        print(f"could not read the image digest ({exc}); left on its tag", file=sys.stderr)
        return ""


def report_change(where: str, what: str, old: str, new: str) -> None:
    """Say when a value is REPLACED, not merely written.

    A changed sum means the file it describes was republished, which is
    worth seeing go past: it is also what a wrong --tag looks like.
    """
    if old == new:
        return
    if HEX.match(old):
        print(f"{where}: {what} changed\n    {old}\n -> {new}")
    else:
        print(f"{where}: {what} filled in")


def regenerate_srcinfo() -> bool:
    """Rewrite both .SRCINFO files, which carry the sums just filled in.

    The AUR reads .SRCINFO and not the PKGBUILD, so a stale one publishes
    the wrong version and the wrong checksums. makepkg is Arch's, so this
    uses it where it exists and an archlinux container otherwise, the
    same thing the release workflow does to check they are current.
    """
    pairs = (("PKGBUILD", ".SRCINFO"), ("PKGBUILD.stud-bin", ".SRCINFO.stud-bin"))
    aur = ROOT / "packaging/aur"
    if shutil.which("makepkg"):
        for pkgbuild, srcinfo in pairs:
            shutil.copy(aur / pkgbuild, aur / "PKGBUILD.tmp")
            out = subprocess.run(["makepkg", "--printsrcinfo", "-p", "PKGBUILD.tmp"],
                                 cwd=aur, capture_output=True, text=True)
            (aur / "PKGBUILD.tmp").unlink()
            if out.returncode != 0:
                print(f"makepkg failed for {pkgbuild}: {out.stderr.strip()}", file=sys.stderr)
                return False
            (aur / srcinfo).write_text(out.stdout)
            print(f".SRCINFO:        {srcinfo} regenerated")
        return True
    if shutil.which("podman"):
        script = (
            "pacman -Sy --noconfirm --needed base-devel >/dev/null 2>&1; useradd -m b; "
            'for p in "PKGBUILD:.SRCINFO" "PKGBUILD.stud-bin:.SRCINFO.stud-bin"; do '
            'install -o b -m644 /work/packaging/aur/${p%%:*} /home/b/PKGBUILD; '
            'su b -c "cd /home/b && makepkg --printsrcinfo" > /work/packaging/aur/${p##*:}; done')
        rc = subprocess.run(["podman", "run", "--rm", "-v", f"{ROOT}:/work:z",
                             "docker.io/library/archlinux:base-devel", "sh", "-c", script],
                            capture_output=True).returncode
        if rc != 0:
            print("the archlinux container could not regenerate the .SRCINFO files",
                  file=sys.stderr)
            return False
        print(".SRCINFO:        both regenerated (archlinux container)")
        return True
    print("neither makepkg nor podman is available, regenerate the .SRCINFO files by hand:",
          file=sys.stderr)
    print("  makepkg --printsrcinfo > packaging/aur/.SRCINFO", file=sys.stderr)
    print("  makepkg --printsrcinfo > packaging/aur/.SRCINFO.stud-bin", file=sys.stderr)
    return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("tag", help="the published tag, e.g. 1.1.5")
    ap.add_argument("--source-sha", help="sha256 of the tag's source tarball")
    ap.add_argument("--archive-sha", help="sha256 of stud-<tag>-x86_64.tar.zst")
    ap.add_argument("--image-digest", help="sha256:... of the published cpak image")
    ap.add_argument("--check", action="store_true", help="report, do not write")
    args = ap.parse_args()

    if args.check:
        for path in (MANIFEST, PKGBUILD, PKGBUILD_BIN):
            text = path.read_text()
            pending = text.count("PLACEHOLDER_") + text.count("'SKIP'")
            print(f"{path.relative_to(ROOT)}: {'filled' if pending == 0 else f'{pending} placeholder(s)'}")
        image = json.loads(CPAK.read_text())["image"]
        pinned = "@sha256:" in image
        print(f"{CPAK.relative_to(ROOT)}: {'pinned' if pinned else 'on a tag'} ({image})")

        # Pinned is not the same as current. A rebuilt release publishes a
        # new image under the same tag, and a manifest still pinned to the
        # old digest installs the old Stud, which looks exactly like a fix
        # that did not work. Cheap to ask, so ask.
        stale = False
        if pinned:
            repo, _, digest = image.partition("@")
            published = registry_digest(repo, args.tag)
            if published and published != digest:
                print(f"{CPAK.relative_to(ROOT)}: STALE. {args.tag} now publishes\n"
                      f"    {published}\n"
                      f"  refill with --image-digest {published}", file=sys.stderr)
                stale = True
        # Non-zero when anything is still a placeholder, so this can gate a
        # release step rather than only being read by a person.
        pending = any("PLACEHOLDER_" in p.read_text() or "'SKIP'" in p.read_text()
                      for p in (MANIFEST, PKGBUILD, PKGBUILD_BIN))
        return 1 if (pending or not pinned or stale) else 0

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

    # A release can be rebuilt: delete a tag, push it again, and every asset
    # is republished under the same name with different bytes. So these are
    # matched by where they sit rather than by a placeholder that exists only
    # until the first fill. An already-filled value is the stale case that
    # most needs rewriting, and skipping it silently left AUR and Flathub
    # advertising sums no published file has.

    # The Flathub manifest: the sha256 under each of Stud's own two urls.
    # Only those; every other source in the file is a third-party pin.
    text = MANIFEST.read_text()
    for url_marker, value in (
            (f"/releases/download/{args.tag}/stud-{args.tag}-x86_64.tar.zst", archive_sha),
            (f"/archive/refs/tags/{args.tag}.tar.gz", source_sha)):
        pattern = re.compile(
            r"(url:\s*\S*" + re.escape(url_marker) + r"\s*\n\s*sha256:\s*)(\S+)")
        match = pattern.search(text)
        if match is None:
            print(f"{MANIFEST.name}: no source found for {url_marker}", file=sys.stderr)
            return 1
        report_change(MANIFEST.name, url_marker, match.group(2), value)
        text = pattern.sub(lambda m: m.group(1) + value, text, count=1)
    MANIFEST.write_text(text)

    # The source PKGBUILD has one source: the tag's tarball.
    text = PKGBUILD.read_text()
    single = re.compile(r"sha256sums=\('(\S+)'\)")
    match = single.search(text)
    if match is None:
        print(f"{PKGBUILD.name}: no sha256sums= line found", file=sys.stderr)
        return 1
    report_change(PKGBUILD.name, "source tarball", match.group(1), source_sha)
    PKGBUILD.write_text(single.sub(f"sha256sums=('{source_sha}')", text, count=1))

    # stud-bin has two, in source=() order: the release archive, then the
    # tag's tarball for the Qt half it builds itself.
    text = PKGBUILD_BIN.read_text()
    pair = re.compile(r"sha256sums=\('(\S+)'\n(\s+)'(\S+)'\)")
    match = pair.search(text)
    if match is not None:
        report_change(PKGBUILD_BIN.name, "release archive", match.group(1), archive_sha)
        report_change(PKGBUILD_BIN.name, "source tarball", match.group(3), source_sha)
        PKGBUILD_BIN.write_text(pair.sub(
            f"sha256sums=('{archive_sha}'\n{match.group(2)}'{source_sha}')", text, count=1))
    else:
        print(f"{PKGBUILD_BIN.name}: already filled, left alone")

    # cpak wants the image itself pinned: a tag can be moved, a digest
    # cannot, and `cpak lock` refuses a manifest that only names a tag
    # ("manifest version 3.0 requires a digest-pinned image").
    image = json.loads(CPAK.read_text())["image"]
    repo = image.split("@", 1)[0].rsplit(":", 1)[0]
    digest = args.image_digest or registry_digest(repo, args.tag)
    if digest:
        data = json.loads(CPAK.read_text())
        data["image"] = f"{repo}@{digest}"
        CPAK.write_text(json.dumps(data, indent=2) + "\n")
        print(f"cpak image:      {digest}")

        # cpak validates the lock against the manifest at install time
        # ("cpak.lock.json does not match the root manifest"), so it is
        # regenerated here rather than left to go stale.
        lock = ROOT / "cpak.lock.json"
        if shutil.which("cpak"):
            rc = subprocess.run(["cpak", "lock", str(CPAK)], cwd=ROOT).returncode
            print("cpak lock:       " + ("written" if rc == 0 else f"FAILED (rc={rc})"))
        elif lock.exists():
            print(f"cpak is not installed, {lock.name} is now STALE, regenerate it with "
                  "`cpak lock cpak.json`", file=sys.stderr)

    if not regenerate_srcinfo():
        return 1
    print("\nDone. Commit these, then copy them to the AUR and Flathub repositories.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
