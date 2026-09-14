#!/usr/bin/env python3
# Builds a small, real zip fixture standing in for the Roblox APK's real
# structure (assets/ tree plus non-assets entries that must NOT be
# extracted), used by apk_extract_test.cpp. Deterministic, no external
# tools beyond the stdlib.
import sys
import zipfile

out_path = sys.argv[1]

with zipfile.ZipFile(out_path, "w") as z:
    z.writestr(
        "assets/ssl/cacert.pem",
        "-----BEGIN CERTIFICATE-----\nFAKE_BUT_REAL_BYTES\n-----END CERTIFICATE-----\n",
    )
    z.writestr("assets/top_level.txt", "hello from a top-level asset\n")
    # Real APKs have plenty of non-assets/ entries, must NOT be
    # extracted by extract_apk_assets().
    z.writestr("AndroidManifest.xml", "<manifest/>")
    z.writestr("classes.dex", b"\x00fake-dex-bytes")
    # Real Roblox APKs ship this exact path (lib/x86_64/libroblox.so,
    # confirmed via `unzip -l` against the real APK), used by
    # extract_apk_native_library_test.
    z.writestr("lib/x86_64/libroblox.so", b"FAKE_ELF_BYTES_NOT_A_REAL_SHARED_OBJECT")
    z.writestr("lib/arm64-v8a/libroblox.so", b"WRONG_ABI_MUST_NOT_BE_EXTRACTED")

# Bundle fixtures, built beside the plain APK.
#
# A split-APK bundle is just a zip of .apk members. The two shapes differ
# only in naming, which is the whole reason .xapk needed its own support:
#
#   .apkm / .apks  base.apk + config.<abi>.apk           (APKMirror, SAI)
#   .xapk          <package>.apk + config.<abi>.apk      (APKPure)
#                  plus a manifest.json describing the set
#
# Each member here is a real, readable APK so the extractor can be asked
# to do its real job against them.
import os


def member_apk(native_bytes: bytes, asset_name: str, asset_text: str) -> bytes:
    import io

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as m:
        m.writestr("AndroidManifest.xml", "<manifest/>")
        m.writestr(f"assets/{asset_name}", asset_text)
        if native_bytes:
            m.writestr("lib/x86_64/libroblox.so", native_bytes)
    return buf.getvalue()


base_dir = os.path.dirname(out_path) or "."

# base.apk carries the assets; the x86_64 split carries the library, which
# is exactly how Roblox's own bundle is laid out.
base_member = member_apk(b"", "from_base.txt", "base asset\n")
x86_member = member_apk(b"FAKE_ELF_BYTES_NOT_A_REAL_SHARED_OBJECT", "from_split.txt", "split asset\n")
arm_member = member_apk(b"WRONG_ABI_MUST_NOT_BE_EXTRACTED", "from_arm.txt", "arm asset\n")

with zipfile.ZipFile(os.path.join(base_dir, "toy_bundle.apkm"), "w") as z:
    # Deliberately not first: the base has to be found by name, not by
    # being the first member seen.
    z.writestr("config.arm64_v8a.apk", arm_member)
    z.writestr("config.x86_64.apk", x86_member)
    z.writestr("base.apk", base_member)

with zipfile.ZipFile(os.path.join(base_dir, "toy_bundle.xapk"), "w") as z:
    z.writestr("manifest.json", '{"package_name": "com.roblox.client"}')
    z.writestr("config.arm64_v8a.apk", arm_member)
    z.writestr("config.x86_64.apk", x86_member)
    # Named after the package, the way an .xapk does it; no "base.apk"
    # anywhere in the archive.
    z.writestr("com.roblox.client.apk", base_member)
