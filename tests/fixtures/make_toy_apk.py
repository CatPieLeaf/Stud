#!/usr/bin/env python3
# Builds a small, real zip fixture standing in for the Roblox APK's real
# structure (assets/ tree plus non-assets entries that must NOT be
# extracted) -- used by apk_extract_test.cpp. Deterministic, no external
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
    # Real APKs have plenty of non-assets/ entries -- must NOT be
    # extracted by extract_apk_assets().
    z.writestr("AndroidManifest.xml", "<manifest/>")
    z.writestr("classes.dex", b"\x00fake-dex-bytes")
    # Real Roblox APKs ship this exact path (lib/x86_64/libroblox.so,
    # confirmed via `unzip -l` against the real APK) -- used by
    # extract_apk_native_library_test.
    z.writestr("lib/x86_64/libroblox.so", b"FAKE_ELF_BYTES_NOT_A_REAL_SHARED_OBJECT")
    z.writestr("lib/arm64-v8a/libroblox.so", b"WRONG_ABI_MUST_NOT_BE_EXTRACTED")
