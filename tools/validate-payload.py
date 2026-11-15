#!/usr/bin/env python3
"""Assert that an unpacked Stud is complete and able to run.

Every package Stud ships wraps the same install tree, and
compare-package-contents.py proves a package carries that tree faithfully.
This answers the other half: whether the tree is any good in the first
place. A package can be a perfect copy of something broken, the upscalers
present in Settings with no shader behind them, because nothing ever looked
inside the binary.

Runs against any Stud root: the install tree, an extracted rpm/deb/Arch
package, an AppImage's squashfs-root, or a Flatpak /app. Rosemary's
directory moves between layouts, so it is discovered, not assumed.
"""

import argparse
import struct
import sys
from pathlib import Path

SPIRV_MAGIC = b"\x03\x02\x23\x07"


def count_spirv_modules(blob):
    """How many embedded SPIR-V modules a binary really carries.

    Not a count of the magic number. That sequence turns up in compiled
    code by coincidence -- it did in Stud 1's render host, once, in .text --
    so counting it said four where three were embedded, and "at least
    three" would then have passed a build that had lost a shader and kept
    the accident. That is the exact regression this file exists to catch.

    A SPIR-V header is five words: magic, version, generator, id bound,
    schema. Version is 0x00MMmm00 with a small major and minor, schema is
    reserved and zero in every version that exists, and the bound is the
    module's id count. Stray code satisfies the magic and not these.
    """
    found = 0
    at = blob.find(SPIRV_MAGIC)
    while at != -1:
        if at + 20 <= len(blob):
            _magic, version, _gen, bound, schema = struct.unpack_from("<5I", blob, at)
            major, minor = (version >> 16) & 0xFF, (version >> 8) & 0xFF
            if ((version & 0xFF000000) == 0 and (version & 0xFF) == 0
                    and 1 <= major <= 2 and minor <= 6
                    and schema == 0 and 0 < bound < (1 << 22)):
                found += 1
        at = blob.find(SPIRV_MAGIC, at + 1)
    return found

ICON_SIZES = ["16x16", "24x24", "32x32", "48x48", "64x64", "128x128", "256x256", "512x512"]


class Report:
    def __init__(self) -> None:
        self.failures: list[str] = []
        self.notes: list[str] = []

    def check(self, ok: bool, what: str) -> bool:
        if not ok:
            self.failures.append(what)
        return ok

    def note(self, what: str) -> None:
        self.notes.append(what)


def is_elf(path: Path) -> bool:
    try:
        with path.open("rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def elf_has_dynamic_deps(path: Path) -> bool:
    """True when the ELF has a dynamic section naming at least one library.

    A real Stud executable links Qt, or libc, or both. Something that
    links nothing is a stub or a truncated write, which is the shape the
    empty-shader-array bug took in a different file.
    """
    try:
        data = path.read_bytes()
    except OSError:
        return False
    if data[:4] != b"\x7fELF" or data[4] != 2:
        return False
    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 0x3A)
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        if off + e_shentsize > len(data):
            return False
        sh_type, = struct.unpack_from("<I", data, off + 4)
        if sh_type == 6:  # SHT_DYNAMIC
            sh_offset, = struct.unpack_from("<Q", data, off + 0x18)
            sh_size, = struct.unpack_from("<Q", data, off + 0x20)
            for e in range(sh_offset, sh_offset + sh_size, 16):
                d_tag, = struct.unpack_from("<q", data, e)
                if d_tag == 1:  # DT_NEEDED
                    return True
                if d_tag == 0:  # DT_NULL
                    break
    return False


def find_rosemary(root: Path) -> Path | None:
    """Rosemary moves between layouts: libexec/stud on most distributions,
    lib/stud on Arch, and either under a Flatpak's /app or an AppImage."""
    for candidate in ("usr/libexec/stud", "usr/lib/stud", "libexec/stud", "lib/stud"):
        p = root / candidate / "rosemary"
        if p.exists():
            return p
    return None


def find_sharun_dir(root: Path) -> Path | None:
    """The AppImage's sharun directory, or None for every other package.

    The AppImage carries its own glibc, and each of Stud's executables in
    it is a hardlink to sharun, a small static loader that runs the real
    binary from shared/bin through the bundled ld-linux (see
    packaging/build-appimage.sh). The file at
    the usual path is then the loader, not the program, and checking it
    would report a static binary with no libraries and no shaders.
    """
    for candidate in ("usr", "."):
        d = root / candidate
        if (d / "sharun").is_file() and (d / "shared" / "bin").is_dir():
            return d
    return None


def real_binary(sharun_dir: Path | None, p: Path) -> Path:
    """The program that runs when p is started."""
    if sharun_dir is None:
        return p
    real = sharun_dir / "shared" / "bin" / p.name
    return real if real.exists() else p


def validate(root: Path, expect_desktop_files: bool, rosemary_source: Path | None) -> Report:
    r = Report()

    rosemary = find_rosemary(root)
    if not r.check(rosemary is not None, "no libexec/stud/rosemary or lib/stud/rosemary"):
        return r
    r.note(f"rosemary: {rosemary.relative_to(root)}")

    # ---- the AppImage's own glibc, when it carries one -------------------
    sharun_dir = find_sharun_dir(root)
    if sharun_dir is not None:
        r.note(f"sharun layout: {sharun_dir.relative_to(root) if sharun_dir != root else '.'}")
        shared_lib = sharun_dir / "shared" / "lib"
        for name in ("ld-linux-x86-64.so.2", "libc.so.6", "libstdc++.so.6"):
            r.check((shared_lib / name).exists(),
                    f"the bundled runtime is missing {name}; every executable would fail to start")
        sharun_bytes = (sharun_dir / "sharun").read_bytes()
        for launcher_path in (sharun_dir / "bin" / "stud", rosemary):
            if launcher_path.exists():
                r.check(launcher_path.read_bytes() == sharun_bytes,
                        f"{launcher_path.relative_to(root)} is not sharun, so it would run "
                        f"without the bundled glibc")
                r.check((sharun_dir / "shared" / "bin" / launcher_path.name).exists(),
                        f"no shared/bin/{launcher_path.name} behind "
                        f"{launcher_path.relative_to(root)}")

    # ---- the executables ------------------------------------------------
    launcher = None
    for candidate in ("usr/bin/stud", "bin/stud"):
        if (root / candidate).exists():
            launcher = root / candidate
    if r.check(launcher is not None, "no usr/bin/stud"):
        launcher = real_binary(sharun_dir, launcher)
        r.check(is_elf(launcher), "stud is not an ELF executable")
        r.check(elf_has_dynamic_deps(launcher), "stud links no libraries, not a real build")

    rosemary = real_binary(sharun_dir, rosemary)
    r.check(is_elf(rosemary), "rosemary is not an ELF executable")
    r.check(elf_has_dynamic_deps(rosemary), "rosemary links no libraries, not a real build")

    # ---- the upscalers' shaders -----------------------------------------
    # Rosemary's build compiles crates/metal/upscale/*.comp with glslc and
    # embeds them. Without glslc the build still succeeds and the upscalers
    # in Settings do nothing, so the binary is counted against the sources
    # it was built from.
    modules = count_spirv_modules(rosemary.read_bytes())
    r.note(f"embedded SPIR-V modules: {modules}")
    if rosemary_source is not None:
        sources = sorted((rosemary_source / "crates" / "metal" / "upscale").glob("*.comp"))
        if r.check(bool(sources), f"no shader sources under {rosemary_source}/crates/metal/upscale"):
            r.check(modules == len(sources),
                    f"rosemary embeds {modules} SPIR-V module(s) and was built from "
                    f"{len(sources)} shader source(s); glslc (shaderc) was missing from the build, "
                    f"and upscaling would be offered in Settings and do nothing")

    # ---- what a desktop needs to show Stud at all -----------------------
    if expect_desktop_files:
        share = root / "usr" / "share"
        if not (share / "applications").is_dir():
            share = root / "share"  # the AppImage's and Flatpak's flat layout
        r.check((share / "applications" / "io.github.catpieleaf.Stud.desktop").exists(),
                "no .desktop entry")
        r.check((share / "metainfo" / "io.github.catpieleaf.Stud.metainfo.xml").exists(),
                "no AppStream metainfo")
        for size in ICON_SIZES:
            icon = share / "icons" / "hicolor" / size / "apps" / "io.github.catpieleaf.Stud.png"
            r.check(icon.exists(), f"no {size} icon")

        # ---- the licences of everything redistributed -------------------
        # Shipping someone else's binary without its licence is the one
        # failure here that is a legal problem rather than a bug.
        licenses = share / "licenses" / "stud"
        for name in ("LICENSE", "LICENSE.exception", "NOTICE.md"):
            r.check((licenses / name).exists(), f"Stud's {name} is missing")
        for component in ("rosemary", "volk", "miniz"):
            d = licenses / component
            r.check(d.is_dir() and any(d.iterdir()),
                    f"no licence text for {component}, which this package redistributes")

    return r


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("root", type=Path, help="an unpacked Stud (install tree, package, AppImage)")
    ap.add_argument("--label", default=None)
    ap.add_argument("--no-desktop-files", action="store_true",
                    help="skip the .desktop/metainfo/icon/licence checks")
    ap.add_argument("--rosemary-source", type=Path, default=None,
                    help="the Rosemary source tree the binary was built from; its upscaler "
                         "shader count is what the binary must embed")
    args = ap.parse_args()

    label = args.label or str(args.root)
    if not args.root.is_dir():
        print(f"::error::{label}: {args.root} is not a directory")
        return 1

    r = validate(args.root, expect_desktop_files=not args.no_desktop_files,
                 rosemary_source=args.rosemary_source)
    print(f"{label}:")
    for n in r.notes:
        print(f"  {n}")
    if r.failures:
        for f in r.failures:
            print(f"::error::{label}: {f}")
        print(f"  {len(r.failures)} problem(s)")
        return 1
    print("  complete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
