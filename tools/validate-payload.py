#!/usr/bin/env python3
"""Assert that an unpacked Stud is complete and able to run.

Every package Stud ships wraps the same install tree, and
compare-package-contents.py proves a package carries that tree faithfully.
This answers the other half: whether the tree is any good in the first
place. A package can be a perfect copy of something broken -- FSR shipped
that way, the upscaler present in Settings with an empty shader behind it,
because nothing ever looked inside the binary.

Runs against any Stud root: the install tree, an extracted rpm/deb/Arch
package, an AppImage's squashfs-root, or a Flatpak /app. The private
directory moves between layouts (libexec/stud on most distributions,
lib/stud on Arch, both in an AppImage) so it is discovered, not assumed.
"""

import argparse
import struct
import sys
from pathlib import Path

SPIRV_MAGIC = b"\x03\x02\x23\x07"

# The engine's own overlay libraries, bound over /system/lib64 in the
# sandbox. Process B dlopen()s these by name; a missing one is a failure
# at load with no useful message.
BIONIC_OVERLAY = [
    "libaaudio.so", "libandroid.so", "libEGL.so", "libGLESv2.so",
    "libmediandk.so", "libnativewindow.so", "libOpenMAXAL.so",
    "libOpenSLES.so", "libvulkan.so.1",
]

# The real bionic, extracted from an Android system image. linker64 is
# Process B's PT_INTERP -- without it nothing bionic runs at all.
BIONIC_RUNTIME = [
    "ld-android.so", "libc++.so", "libc.so", "libdl.so", "libdl_android.so",
    "liblog.so", "libm.so", "linker64",
]

# ANGLE, which render-host loads for both GL paths.
ANGLE = ["libEGL.so", "libGLESv2.so", "libvulkan.so.1"]

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


def elf_interpreter(path: Path) -> str | None:
    """PT_INTERP of a 64-bit little-endian ELF, or None."""
    try:
        data = path.read_bytes()
    except OSError:
        return None
    if data[:4] != b"\x7fELF" or data[4] != 2:
        return None
    e_phoff, = struct.unpack_from("<Q", data, 0x20)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x36)
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, = struct.unpack_from("<I", data, off)
        if p_type == 3:  # PT_INTERP
            p_offset, = struct.unpack_from("<Q", data, off + 0x08)
            p_filesz, = struct.unpack_from("<Q", data, off + 0x20)
            return data[p_offset:p_offset + p_filesz].rstrip(b"\0").decode(errors="replace")
    return None


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


def find_private_dir(root: Path) -> Path | None:
    """Where this layout keeps Stud's own executables."""
    for candidate in ("usr/libexec/stud", "usr/lib/stud", "libexec/stud", "lib/stud"):
        d = root / candidate
        if (d / "stud-render-host").exists():
            return d
    return None


def find_data_dir(root: Path) -> Path | None:
    """Where ANGLE and the bionic runtime live, which is not always the same place."""
    for candidate in ("usr/lib/stud", "usr/libexec/stud", "lib/stud", "libexec/stud"):
        d = root / candidate
        if (d / "angle").is_dir() or (d / "android-bionic").is_dir():
            return d
    return None


def validate(root: Path, expect_desktop_files: bool) -> Report:
    r = Report()

    private = find_private_dir(root)
    if not r.check(private is not None, "no directory holding stud-render-host"):
        return r
    data = find_data_dir(root) or private
    r.note(f"executables: {private.relative_to(root)}")
    r.note(f"ANGLE and bionic: {data.relative_to(root)}")

    # ---- the executables ------------------------------------------------
    launcher = None
    for candidate in ("usr/bin/stud", "bin/stud"):
        if (root / candidate).exists():
            launcher = root / candidate
    r.check(launcher is not None, "no usr/bin/stud")
    if launcher:
        r.check(is_elf(launcher), "usr/bin/stud is not an ELF executable")

    for name in ("stud-render-host", "stud-runtime-bionic", "stud-webview"):
        p = private / name
        if r.check(p.exists(), f"{name} is missing"):
            r.check(is_elf(p), f"{name} is not an ELF executable")
            # Guards against a truncated or stub file, not against a small
            # one: stud-webview is legitimately ~70 KB, because it is a
            # thin wrapper and its weight is all in shared libraries.
            r.check(p.stat().st_size > 16_384, f"{name} is truncated ({p.stat().st_size} bytes)")
            r.check(elf_has_dynamic_deps(p), f"{name} links no libraries -- not a real build")

    # ---- Process B really is a bionic binary ----------------------------
    runtime = private / "stud-runtime-bionic"
    if runtime.exists():
        interp = elf_interpreter(runtime)
        r.check(interp is not None and "linker64" in interp,
                f"stud-runtime-bionic's interpreter is {interp!r}, not a bionic linker64")

    # ---- the upscaler's shaders ----------------------------------------
    # Compiled to SPIR-V at build time and embedded. Without glslc the
    # build silently emits an empty array, and FSR then does nothing at
    # all while still being offered in Settings. A correct build carries
    # three modules; a build with no shader compiler carries one.
    host = private / "stud-render-host"
    if host.exists():
        modules = host.read_bytes().count(SPIRV_MAGIC)
        r.check(modules >= 3,
                f"stud-render-host carries {modules} SPIR-V module(s), not the 3 a build "
                f"with the upscale and sharpening shaders has -- FSR would do nothing")
        r.note(f"embedded SPIR-V modules: {modules}")

    # ---- the libraries Stud brings with it ------------------------------
    for name in ANGLE:
        r.check((data / "angle" / name).exists(), f"ANGLE: {name} is missing")
    for name in BIONIC_RUNTIME:
        r.check((data / "android-bionic" / name).exists(), f"bionic: {name} is missing")
    for name in BIONIC_OVERLAY:
        r.check((private / "lib64" / name).exists(), f"the /system/lib64 overlay: {name} is missing")

    # ---- what a desktop needs to show Stud at all -----------------------
    if expect_desktop_files:
        share = root / "usr" / "share"
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
        r.check((licenses / "LICENSE").exists(), "Stud's own licence is missing")
        for component in ("angle", "android-bionic", "fidelityfx-fsr1"):
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
    args = ap.parse_args()

    label = args.label or str(args.root)
    if not args.root.is_dir():
        print(f"::error::{label}: {args.root} is not a directory")
        return 1

    r = validate(args.root, expect_desktop_files=not args.no_desktop_files)
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
