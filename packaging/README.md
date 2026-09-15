# Packaging

One install tree, three formats. `cmake --install` lays down a relocatable
layout. The deb and the rpm are that tree packaged. The AppImage is that
tree plus its dependencies.

```
<prefix>/bin/stud                     the launcher, the only thing on PATH
<prefix>/libexec/stud/                the other two processes, the web-view
                                      viewer, and Process B's lib64/ overlay
<prefix>/lib/stud/angle/              ANGLE
<prefix>/lib/stud/android-bionic/     the real linker64 and libc
<prefix>/share/applications/...       desktop entry
<prefix>/share/metainfo/...           AppStream component
<prefix>/share/icons/hicolor/...      icon
```

Every binary finds bionic, ANGLE and its sibling processes **relative to
its own path**, which is what lets the same tree work when installed at
`/usr` and when mounted at a random path by an AppImage.

## Building the packages

```sh
tools/setup.sh
cmake -S . -B build -G Ninja
cmake --build build

cpack -G RPM -B build/packages --config build/CPackConfig.cmake   # Fedora
cpack -G DEB -B build/packages --config build/CPackConfig.cmake   # Debian/Ubuntu
packaging/build-appimage.sh build                                 # anywhere
```

The AppImage script downloads [linuxdeploy] and its [Qt plugin] on first
run and caches them in `third_party/appimage-tools`.

[linuxdeploy]: https://github.com/linuxdeploy/linuxdeploy
[Qt plugin]: https://github.com/linuxdeploy/linuxdeploy-plugin-qt

## What is bundled, and what is depended on

Stud ships ANGLE and the bionic set in every format. Neither is our code.
Nothing starts without them, and both are redistributable: ANGLE is BSD,
bionic is Apache-2.0 from AOSP.

**bubblewrap is a dependency, never bundled.** Process B runs inside it
and Stud refuses to launch without it. A distribution's own build carries
the AppArmor or SELinux policy that lets it create a user namespace;
a binary copied into a package would be refused on exactly the
distributions that matter.

The deb and the rpm depend on Qt. The AppImage bundles it. That is the
ordinary split: a system package should use the system Qt.

## Appearing in a software centre

Discover, GNOME Software and the rest read AppStream. The component lives
in `packaging/stud.metainfo.xml.in` and installs to
`share/metainfo/<app-id>.metainfo.xml`. A centre hides an application
whose component is incomplete, so this one carries the lot: reverse-DNS
id, `desktop-application` type, a launchable pointing at the desktop
entry, summary, description, licence, URL, OARS content rating.

Three identifiers must agree or the entry does not resolve: the AppStream
`<id>`, the desktop file's name, and the Wayland `app_id` both of Stud's
windows report. They all come from `STUD_APP_ID` in the top-level
`CMakeLists.txt`.

Validate before trusting a package:

```sh
appstreamcli validate build/packaging/<app-id>.metainfo.xml
desktop-file-validate build/packaging/<app-id>.desktop
```

**Screenshots are missing.** A centre still lists Stud without them, just
badly. They need hosting somewhere stable, then adding to the component as
`<screenshots>`.
