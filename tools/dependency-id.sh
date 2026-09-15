#!/bin/sh
# Prints what the cached dependencies are, for a cache key to hash.
#
# The keys used to hash the whole of tools/setup.sh, which meant editing a
# comment in it threw away a cached ANGLE and rebuilt it from source: an
# hour of CI for a change that cannot alter a single byte of the result.
#
# Called with `appimage` it does the same for the AppImage tools, which
# were keyed on packaging/build-appimage.sh for the same wrong reason.
#
# What the cache actually depends on is the pinned NDK version and URL,
# the AOSP branch and the two APEX paths the bionic libraries come out of,
# and the lists naming which files are kept. A change to the procedure
# around them produces the same libraries and can reuse the same cache;
# a change to any of these cannot, and gets a new key.
set -eu
cd "$(dirname "$0")/.."
case "${1:-deps}" in
    deps)
        grep -E '^(NDK_VERSION|NDK_URL|AOSP|AOSP_BRANCH|RUNTIME_APEX_PATH|TZDATA_APEX_PATH)=' \
            tools/setup.sh
        sed -n '/^BIONIC_FILES=(/,/^)/p; /^BIONIC_OPTIONAL_FILES=(/,/^)/p;
                /^ANGLE_RUNTIME_FILES=(/,/^)/p' tools/setup.sh
        ;;
    appimage)
        # The three AppImage tools, by the URL each is downloaded from.
        grep -oE 'https://[^"]*\.AppImage' packaging/build-appimage.sh | sort -u
        ;;
    *)
        echo "usage: $0 [deps|appimage]" >&2
        exit 2
        ;;
esac
