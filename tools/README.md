# tools

Three kinds of thing live here: the dependency setup script, the release
pipeline's own checkers, and standalone diagnostics.

Only the diagnostics are optional. `setup.sh` fetches what the build needs,
`png_to_argb_header.py` is a build step (`android-glue/CMakeLists.txt` runs
it to turn the cursor PNGs into a header), and the Python checkers under
"release tooling" below are run by `.github/workflows/release.yml` -- a
release fails without them.

- **`setup.sh`**: fetches the three third-party dependencies into
  `third_party/` (the NDK, an ANGLE build, a real extracted bionic).
  `tools/setup.sh --help` explains how to point it at copies you already
  have instead of downloading or building them.

- **`try_render_window.cpp`** (`stud_try_render_window`): opens a real
  Wayland window through ANGLE and swaps real frames, with no
  `libroblox.so` and no JNI anywhere. When rendering breaks, this is how
  you find out in one minute whether the machine, the driver or the
  ANGLE build is at fault, rather than the engine.

- **`try_vulkan_window.cpp`** (`stud_try_vulkan_window`): the same idea
  for the native Vulkan path.

- **`tex_decode_compare.cpp`** (`stud_tex_decode_compare`): decodes the
  same compressed blocks with Stud's own software decoder and with a GPU
  that implements the format in hardware, and compares. This is what
  settled whether texture corruption came from the decoder or from
  somewhere else; it is built only after a first successful build, since
  it compiles the decoder's dependencies out of the runtime sub-build's
  own checkout.

- **`fill-release-checksums.py`**: after a release is published, writes
  its digests into the two AUR PKGBUILDs and the Flatpak manifest, pins
  the cpak image, regenerates `cpak.lock.json` and both `.SRCINFO` files.
  `--check` reports what is still a placeholder.

- **`png_to_argb_header.py`**: turns a PNG into the ARGB header the
  cursor and window icons are compiled from.

- **`validate-payload.py`**: asks whether an install tree, an extracted
  package or an AppImage is complete. It looks inside the binaries rather
  than at file names, which is how a release once shipped a render host
  with one embedded shader where a correct build has three.

- **`compare-package-contents.py`**: compares a built package against the
  install tree it came from, file by file, by digest. Arch relocates
  `libexec/stud` to `lib/stud`, and that one mapping is applied before
  comparing.

- **`diag_launch_direct.cpp`** (`stud_diag_launch_direct`): launches
  Process B directly against a real extracted `libroblox.so`, skipping
  Process A's login and config entirely. Useful for bring-up questions
  that do not need a real session. It takes the cookie from
  `STUD_DIAG_COOKIE_FILE`/`STUD_DIAG_COOKIE` and never from a literal,
  and it deliberately does not deep-link into a place.

Nothing here inspects or unpacks the Roblox app; the tools used to work
out how it behaves are not part of this repository and are not build
prerequisites.
