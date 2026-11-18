# Packages from the same install tree as `cmake --install`:
#
#   cpack -G RPM   -B <dir>      Fedora
#   cpack -G DEB   -B <dir>      Debian and Ubuntu
#   packaging/build-appimage.sh   any distribution
#
# Each format keeps its own dependency list; the AppImage bundles Qt instead.

set(CPACK_PACKAGE_NAME "stud")
set(CPACK_PACKAGE_VERSION "${STUD_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "An Unofficial Open-Source Roblox Launcher for Linux")
set(CPACK_PACKAGE_DESCRIPTION
    "Stud runs the real Roblox macOS client on a Linux desktop, on \
Rosemary. It downloads the client from Roblox's own servers and runs it \
unmodified. Roblox is not included in this package.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/CatPieLeaf/Stud")
# "Name <email>", as lintian and rpm expect.
set(CPACK_PACKAGE_CONTACT "CatPieLeaf <catpieleaf@proton.me>")
set(CPACK_PACKAGE_VENDOR "CatPieLeaf")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES FALSE)

set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------- RPM
set(CPACK_RPM_PACKAGE_SUMMARY "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}")
# Hard-wrapped: rpmlint rejects lines past 79 columns.
set(CPACK_RPM_PACKAGE_DESCRIPTION
"Stud runs the real Roblox macOS client on a Linux desktop, on
Rosemary. It downloads the client from Roblox's own servers and runs it
unmodified. Roblox is not included in this package.")
set(CPACK_RPM_PACKAGE_LICENSE "AGPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")
set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
# Autoreqprov is off, so every library is listed: by SONAME where linked, by
# package where opened at runtime (SDL3, the Vulkan loader, FFmpeg). Fedora's
# and RPM Fusion's FFmpeg both provide libavcodec-free and libavutil-free.
set(CPACK_RPM_PACKAGE_REQUIRES
    "qt6-qtbase-gui, qt6-qtwebengine, qt6-qtwebchannel, vulkan-loader, SDL3 >= 3.4.0, libcurl.so.4()(64bit), libcrypto.so.3()(64bit), libz.so.1()(64bit), libxkbcommon.so.0()(64bit), libudev.so.1()(64bit), libavcodec-free, libavutil-free, gamemode")
set(CPACK_RPM_PACKAGE_SUGGESTS "mangohud")
set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
set(CPACK_RPM_PACKAGE_AUTOREQPROV " no")
# Must be a real CMake list: a single newline-separated string matches nothing,
# and the package ended up owning /usr/share/icons.
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
# Each icon size directory belongs to hicolor-icon-theme.
foreach(stud_icon_size 16 24 32 48 64 128 256 512)
    list(APPEND CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
        "/usr/share/icons/hicolor/${stud_icon_size}x${stud_icon_size}"
        "/usr/share/icons/hicolor/${stud_icon_size}x${stud_icon_size}/apps")
endforeach()
# Ship the binaries as built, symbols included, for readable crash reports: no
# debuginfo split or stripping, which also keeps the rpm identical to the
# install tree the release checks.
set(CPACK_RPM_SPEC_MORE_DEFINE "%global __os_install_post %{nil}
%global debug_package %{nil}
%global __brp_strip %{nil}
%global __brp_strip_static_archive %{nil}")

# ---------------------------------------------------------------- DEB
set(CPACK_DEBIAN_PACKAGE_NAME "stud")
set(CPACK_DEBIAN_PACKAGE_SECTION "games")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
# Set explicitly: unset, CPack asks dpkg, which the Fedora build container lacks,
# and silently produced a package dpkg refused. x86_64 only, like every format.
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
# Qt is named by Debian package with a version floor, so apt refuses a system too
# old to run the build instead of the loader failing at start (Qt symbols are
# versioned). Built on ubuntu:26.04, the oldest with Qt 6.9 and SDL 3.4; the
# workflow checks the binary's Qt floor against 6.10. Each library has two names
# because Ubuntu 24.04 renamed them for 64-bit time_t and 26.04 renamed them back.
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libqt6gui6 (>= 6.10) | libqt6gui6t64 (>= 6.10), \
libqt6widgets6 (>= 6.10) | libqt6widgets6t64 (>= 6.10), \
libqt6opengl6 (>= 6.10) | libqt6opengl6t64 (>= 6.10), \
libqt6openglwidgets6 (>= 6.10) | libqt6openglwidgets6t64 (>= 6.10), \
libqt6network6 (>= 6.10) | libqt6network6t64 (>= 6.10), \
libqt6dbus6 (>= 6.10) | libqt6dbus6t64 (>= 6.10), \
libqt6webenginewidgets6 (>= 6.10) | libqt6webenginewidgets6t64 (>= 6.10), \
libqt6webchannel6 (>= 6.10) | libqt6webchannel6t64 (>= 6.10), \
libvulkan1, libsdl3-0 (>= 3.4.0), libcurl4t64 | libcurl4, libssl3t64 | libssl3, zlib1g, libxkbcommon0, libudev1, ffmpeg, gamemode")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "mangohud")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS OFF)

include(CPack)
