# Packages, built from the same install tree as `cmake --install`.
#
#   cpack -G RPM   -B <dir>      Fedora, and what Discover lists
#   cpack -G DEB   -B <dir>      Debian and Ubuntu
#   packaging/build-appimage.sh   everything in one file, any distribution
#
# Each generator's dependency list is its own, and that separation is the
# point: the Debian list names Debian's packages and lives with whatever
# Qt an Ubuntu LTS happens to carry, and none of that reaches the rpm, the
# Arch package (packaging/aur) or the AppImage, which bundles Qt
# precisely because it has to run where no list applies.

set(CPACK_PACKAGE_NAME "stud")
set(CPACK_PACKAGE_VERSION "${STUD_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "An Unofficial Open-Source Roblox Launcher for Linux")
set(CPACK_PACKAGE_DESCRIPTION
    "Stud runs the real Roblox macOS client on a Linux desktop, on \
Rosemary. It downloads the client from Roblox's own servers and runs it \
unmodified. Roblox is not included in this package.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/CatPieLeaf/Stud")
# Debian policy wants Maintainer as "Name <email>", and lintian says so
# (maintainer-address-malformed); rpm's Packager is the same shape.
set(CPACK_PACKAGE_CONTACT "CatPieLeaf <catpieleaf@proton.me>")
set(CPACK_PACKAGE_VENDOR "CatPieLeaf")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES FALSE)

set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------- RPM
set(CPACK_RPM_PACKAGE_SUMMARY "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}")
# The same text, hard-wrapped: rpm carries a description verbatim and
# rpmlint rejects a line past 79 columns (description-line-too-long).
set(CPACK_RPM_PACKAGE_DESCRIPTION
"Stud runs the real Roblox macOS client on a Linux desktop, on
Rosemary. It downloads the client from Roblox's own servers and runs it
unmodified. Roblox is not included in this package.")
set(CPACK_RPM_PACKAGE_LICENSE "AGPL-3.0-or-later AND GPL-3.0-only AND LGPL-3.0-or-later AND MIT AND BSD-3-Clause")
set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")
set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
# Autoreqprov is off below, so every library is named here, by SONAME where
# it is linked (the dependency rpm would have generated itself, without
# assuming a distribution's package name) and by package where it is opened
# at runtime: Rosemary opens SDL3 by name, and the launcher the Vulkan
# loader, and FFmpeg for video: libavcodec-free and libavutil-free are what
# Fedora's build and RPM Fusion's full one both provide, whatever the release.
# Rosemary links libcurl, zlib, libxkbcommon and libudev.
set(CPACK_RPM_PACKAGE_REQUIRES
    "qt6-qtbase-gui, qt6-qtwebengine, qt6-qtwebchannel, vulkan-loader, SDL3 >= 3.4.0, libcurl.so.4()(64bit), libz.so.1()(64bit), libxkbcommon.so.0()(64bit), libudev.so.1()(64bit), libavcodec-free, libavutil-free")
# Weak: both are Settings switches, and neither stops Stud from running.
set(CPACK_RPM_PACKAGE_SUGGESTS "mangohud, gamemode")
set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
# The dependencies are the list above, not a scan.
set(CPACK_RPM_PACKAGE_AUTOREQPROV " no")
# Directories the distribution already owns. This has to be a real CMake
# list: written as one space-and-newline separated string it is a single
# list element that matches no path at all, which is how the package came
# to claim %dir /usr/share/icons (rpmlint: standard-dir-owned-by-package).
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/bin
    /usr/lib
    /usr/libexec
    /usr/share
    /usr/share/applications
    /usr/share/doc
    /usr/share/man
    /usr/share/man/man1
    /usr/share/metainfo
    /usr/share/licenses
    /usr/share/icons
    /usr/share/icons/hicolor)
# ...and every size directory the icons land in, all of which belong to
# hicolor-icon-theme.
foreach(stud_icon_size 16 24 32 48 64 128 256 512)
    list(APPEND CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
        "/usr/share/icons/hicolor/${stud_icon_size}x${stud_icon_size}"
        "/usr/share/icons/hicolor/${stud_icon_size}x${stud_icon_size}/apps")
endforeach()
# No debuginfo subpackage: the binaries ship with their symbols, so a crash
# report carries function names.
set(CPACK_RPM_SPEC_MORE_DEFINE "%global debug_package %{nil}")

# ---------------------------------------------------------------- DEB
#
# Debian and Ubuntu, and deliberately nothing else: every name below is a
# Debian package name, and the older Qt that an Ubuntu LTS carries is this
# package's business alone. Nothing here reaches the rpm, the Arch package
# or the AppImage, the AppImage in particular has to work on
# distributions this list has never heard of, which is why it bundles Qt
# instead of depending on it.
set(CPACK_DEBIAN_PACKAGE_NAME "stud")
set(CPACK_DEBIAN_PACKAGE_SECTION "games")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
# Set explicitly, NOT left to CPack. Unset, CPack asks
# `dpkg --print-architecture`, and the release builds in a Fedora
# container, which has no dpkg. It cannot fail loudly there, so it
# produced a package with an EMPTY Architecture field, named
# `stud_1.1.0_.deb`, that dpkg refuses to install ("missing 'Architecture'
# field"). It shipped in a green release.
#
# amd64 rather than derived: Rosemary runs the client's x86_64 code
# natively, so Stud is x86_64 only, which the rpm, the AUR package and the
# Flatpak manifest all pin too.
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
# The same libraries as the rpm's own list.
#
# Qt is named by its Debian binary packages rather than a version, which
# is only honest if the deb really was built against a Qt no newer than
# the ones named here. Twice it was not, and each package installed
# cleanly and then refused to start: the 1.1.0 deb, built on Fedora 44,
# died on Ubuntu 26.04 with `libQt6Core.so.6: version 'Qt_6.11' not
# found`, and the 1.1.2 deb, built on ubuntu:26.04, died on Linux Mint 22
# with `version 'Qt_6.10' not found`. Qt exports a symbol version per
# release and the loader checks it, so a build runs on the Qt it was
# linked against and everything newer, and on nothing older. The release
# workflow builds the deb on ubuntu:26.04, the oldest distribution Stud 2.0
# can target: its web views need Qt 6.9 and Rosemary SDL 3.4, neither of
# which Ubuntu 24.04 has. The workflow checks the binary's own Qt floor
# against the 6.10 named here.
#
# Two names for each library, because Ubuntu 24.04 renamed them for the
# 64-bit time_t transition and 26.04 renamed them back. An alternative
# satisfies whichever one the distribution actually carries. The version
# floor is the Qt the build is made against, so apt refuses the package on
# a distribution too old to run it instead of dpkg accepting it and the
# loader failing later.
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libqt6gui6 (>= 6.10) | libqt6gui6t64 (>= 6.10), \
libqt6widgets6 (>= 6.10) | libqt6widgets6t64 (>= 6.10), \
libqt6opengl6 (>= 6.10) | libqt6opengl6t64 (>= 6.10), \
libqt6openglwidgets6 (>= 6.10) | libqt6openglwidgets6t64 (>= 6.10), \
libqt6network6 (>= 6.10) | libqt6network6t64 (>= 6.10), \
libqt6dbus6 (>= 6.10) | libqt6dbus6t64 (>= 6.10), \
libqt6webenginewidgets6 (>= 6.10) | libqt6webenginewidgets6t64 (>= 6.10), \
libqt6webchannel6 (>= 6.10) | libqt6webchannel6t64 (>= 6.10), \
libvulkan1, libsdl3-0 (>= 3.4.0), libcurl4t64 | libcurl4, zlib1g, libxkbcommon0, libudev1, ffmpeg")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "mangohud, gamemode")
# The dependencies are the list above, not a scan.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS OFF)

include(CPack)
