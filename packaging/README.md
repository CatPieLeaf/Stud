# Packaging

One install tree, six formats: the rpm, the deb, the Arch package, the
AppImage, the Flatpak and the cpak image. `cmake --install` lays down a
relocatable layout. The deb and the rpm are that tree packaged. The AppImage
is that tree plus its dependencies.

```
<prefix>/bin/stud                     the launcher, the only thing on PATH
<prefix>/libexec/stud/rosemary        Rosemary, which runs the macOS client
<prefix>/share/applications/...       desktop entry
<prefix>/share/metainfo/...           AppStream component
<prefix>/share/icons/hicolor/...      icon
<prefix>/share/licenses/stud/...      Stud's licence, and Rosemary's
```

The launcher finds Rosemary **relative to its own path**, which is what lets
the same tree work when installed at `/usr` and when mounted at a random path
by an AppImage. No package contains Roblox: the launcher downloads the macOS
client from Roblox's own servers at the first start.

## Building the packages

```sh
cmake -S . -B build -G Ninja
cmake --build build

cpack -G RPM -B build/packages --config build/CPackConfig.cmake   # Fedora
cpack -G DEB -B build/packages --config build/CPackConfig.cmake   # Debian/Ubuntu
packaging/build-appimage.sh                                       # anywhere
```

The build compiles Rosemary with cargo, from Stud's fork,
[rosemary-stud](https://github.com/CatPieLeaf/rosemary-stud), branch `stud`.
It needs Rust 1.99 or newer, git (Rosemary's Microlift dependencies are git
dependencies), and `glslc` for the upscalers' shaders.

The AppImage builds in an Arch container with [quick-sharun] from the
[Anylinux AppImages] project. It bundles every library, glibc and mesa
included, and packs a SquashFS image behind [uruntime].

[quick-sharun]: https://github.com/pkgforge-dev/Anylinux-AppImages/blob/main/useful-tools/quick-sharun.sh
[Anylinux AppImages]: https://github.com/pkgforge-dev/Anylinux-AppImages
[uruntime]: https://github.com/VHSgunzo/uruntime

## What is bundled, and what is depended on

**SDL3 is a runtime dependency, and it must be 3.4 or newer.** Rosemary
opens it by name rather than linking it, so a package names it in its
dependencies itself; nothing finds it by scanning the binaries. The same
goes for the Vulkan loader, which the launcher opens to list the GPUs.

The deb and the rpm depend on Qt 6.10 or newer, with QtWebEngine and
QtWebChannel: the launcher shows Roblox's sign-in and web pages itself. The
AppImage bundles all of it, down to glibc. That is the ordinary split: a
system package should use the system Qt.

GameMode and MangoHud are suggested, not required: both are Settings
switches, and neither stops Stud from running.

## Appearing in a software centre

Discover, GNOME Software and the rest read AppStream. The component lives
in `packaging/io.github.catpieleaf.Stud.metainfo.xml` and installs to
`share/metainfo/<app-id>.metainfo.xml`. A centre hides an application
whose component is incomplete, so this one carries the lot: reverse-DNS
id, `desktop-application` type, a launchable pointing at the desktop
entry, summary, description, licence, URL, OARS content rating.

Three identifiers must agree or the entry does not resolve: the AppStream
`<id>`, the desktop file's name, and the Wayland `app_id` both of Stud's
window reports. They all come from `STUD_APP_ID` in the top-level
`CMakeLists.txt`.

Validate before trusting a package:

```sh
appstreamcli validate packaging/io.github.catpieleaf.Stud.metainfo.xml
desktop-file-validate packaging/io.github.catpieleaf.Stud.desktop
```

Screenshots live in `packaging/screenshots/` and are referenced from the
component's own `<screenshots>` block in `io.github.catpieleaf.Stud.metainfo.xml`.
