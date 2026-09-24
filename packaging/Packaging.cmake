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
    "Stud runs the real Roblox Android client on a Linux desktop. It loads \
Roblox's own engine and drives it through the interfaces it expects, \
supplying the framework, window, input devices and graphics stack \
itself. Roblox is not included: you supply the Android \
application package yourself.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/CatPieLeaf/Stud")
# Debian policy wants Maintainer as "Name <email>", and lintian says so
# (maintainer-address-malformed); rpm's Packager is the same shape.
set(CPACK_PACKAGE_CONTACT "CatPieLeaf <catpieleaf@proton.me>")
set(CPACK_PACKAGE_VENDOR "CatPieLeaf")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES FALSE)

# Stud ships prebuilt third-party libraries (ANGLE, and the bionic set)
# that neither distribution's tooling should try to interpret: they are
# not built against the host's libraries and have no business in the
# shared-library dependency scan or the build-id/debuginfo machinery.
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------- RPM
#
# bubblewrap is a hard requirement: Process B runs inside it, and Stud
# refuses to launch without it. It is a package rather than a bundled
# binary on purpose, a distribution's own build carries the AppArmor
# and SELinux policy that lets it create a user namespace at all.
set(CPACK_RPM_PACKAGE_SUMMARY "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}")
# The same text, hard-wrapped: rpm carries a description verbatim and
# rpmlint rejects a line past 79 columns (description-line-too-long).
set(CPACK_RPM_PACKAGE_DESCRIPTION
"Stud runs the real Roblox Android client on a Linux desktop. It loads
Roblox's own engine and drives it through the interfaces it expects,
supplying the framework, window, input devices and graphics stack
itself. Roblox is not included: you supply the Android
application package yourself.")
set(CPACK_RPM_PACKAGE_LICENSE "AGPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")
set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
# Audio names nothing here. render-host compiles miniaudio in and opens
# whichever of libasound, libpulse or libjack the machine has, by name at
# runtime, so there is no package to require: a host missing all three
# runs Stud silently rather than failing to install it.
# The render host links libxkbcommon directly (it reads the compositor's
# own keymap, so that a key reports what it types rather than what a US
# layout would have). Autoreqprov is off below, so nothing would generate
# that dependency on its own, hence naming it here, by SONAME rather
# than by package: that is the dependency rpm would have generated
# itself, it does not assume a distribution's package name, and naming
# the package instead is what rpmlint calls explicit-lib-dependency.
set(CPACK_RPM_PACKAGE_REQUIRES
    "bubblewrap, qt6-qtbase-gui, qt6-qtwebengine, qtkeychain-qt6, vulkan-loader, libxkbcommon.so.0()(64bit), libXi.so.6()(64bit)")
# Weak, both of them: mangohud is a Settings toggle, and wl-clipboard is
# what the tray's "copy server link" shells out to on Wayland. Neither
# stops Stud from running.
set(CPACK_RPM_PACKAGE_SUGGESTS "mangohud, wl-clipboard, libdecor")
# FFmpeg, for the engine's video playback and screen recording. The render
# host loads it at runtime by the major version of the headers it was built
# against (render-host/src/video_codec.cpp), so the dependency is exactly
# that major, by SONAME for rpm and by package name for deb. Weak: without
# it Stud runs and the engine is offered no video codecs.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(StudPackagedFfmpeg QUIET libavcodec libavutil libswscale)
endif()
if(StudPackagedFfmpeg_FOUND)
    string(REGEX MATCH "^[0-9]+" _stud_avcodec_major "${StudPackagedFfmpeg_libavcodec_VERSION}")
    string(REGEX MATCH "^[0-9]+" _stud_avutil_major "${StudPackagedFfmpeg_libavutil_VERSION}")
    string(REGEX MATCH "^[0-9]+" _stud_swscale_major "${StudPackagedFfmpeg_libswscale_VERSION}")
    string(APPEND CPACK_RPM_PACKAGE_SUGGESTS
        ", libavcodec.so.${_stud_avcodec_major}()(64bit)"
        ", libavutil.so.${_stud_avutil_major}()(64bit)"
        ", libswscale.so.${_stud_swscale_major}()(64bit)")
endif()
set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
# The bundled libraries are private to Stud: nothing else may resolve
# against them, and rpm must not advertise them as provided.
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
# Stripping or extracting build-ids from a bionic ELF or from ANGLE is
# neither useful nor safe here.
set(CPACK_RPM_SPEC_MORE_DEFINE "%global __os_install_post %{nil}
%global debug_package %{nil}
%global __brp_strip %{nil}
%global __brp_strip_static_archive %{nil}
%global __brp_check_rpaths %{nil}
%global __requires_exclude_from ^/usr/lib/stud/.*|^/usr/libexec/stud/.*$
%global __provides_exclude_from ^/usr/lib/stud/.*|^/usr/libexec/stud/.*$")

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
# amd64 rather than derived: everything Stud redistributes, the bionic
# runtime, ANGLE, the engine itself: is x86_64 only, which the rpm, the
# AUR package and the Flatpak manifest all pin too.
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
# The same reasoning as the rpm's own list: bubblewrap because Process B
# is launched inside it and Stud will not start without one. Audio is not
# named, because miniaudio is compiled in and opens the host's own
# libasound, libpulse or libjack by name.
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
# workflow builds the deb on ubuntu:24.04 now, the oldest distribution
# this package targets, and checks the resulting binary's own Qt floor.
#
# Two names for each of three libraries, because Ubuntu 24.04 renamed
# them for the 64-bit time_t transition and 26.04 renamed them back:
# 24.04 has only libqt6gui6t64, 26.04 has only libqt6gui6 (checked in
# both images, not assumed). An alternative satisfies whichever one the
# distribution actually carries. The version floor is the Qt the build
# is made against, so apt refuses the package on a distribution too old
# to run it instead of dpkg accepting it and the loader failing later.
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "bubblewrap, \
libqt6gui6 (>= 6.4) | libqt6gui6t64 (>= 6.4), \
libqt6widgets6 (>= 6.4) | libqt6widgets6t64 (>= 6.4), \
libqt6network6 (>= 6.4) | libqt6network6t64 (>= 6.4), \
libqt6webenginewidgets6 (>= 6.4), \
libqt6keychain1, libvulkan1, libfreetype6, \
libwayland-client0, libxkbcommon0, libxi6")
# wl-clipboard because the tray's "copy server link" shells out to
# wl-copy: a Wayland compositor only accepts a clipboard offer with the
# serial of a real input event on one of the application's own surfaces,
# and the tray menu is the desktop's own, so Qt cannot copy from there
# (ui/src/tray.cpp explains it at the call site). Recommended rather than
# required, without it that one menu entry says what is missing, and
# everything else works.
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "mangohud, wl-clipboard, libdecor-0-0")
# The same FFmpeg major the render host was built for; see the rpm half.
if(StudPackagedFfmpeg_FOUND)
    string(APPEND CPACK_DEBIAN_PACKAGE_RECOMMENDS
        ", libavcodec${_stud_avcodec_major}, libavutil${_stud_avutil_major}"
        ", libswscale${_stud_swscale_major}")
endif()
# The bundled libraries are private to Stud. Without this, dpkg-shlibdeps
# reads ANGLE and the bionic set and either invents dependencies that do
# not exist or fails outright. They are not built against the host's
# libraries and have no business in the scan.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS OFF)

include(CPack)
