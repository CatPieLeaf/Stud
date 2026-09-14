# Packages, built from the same install tree as `cmake --install`.
#
#   cpack -G RPM   -B <dir>      Fedora, and what Discover lists
#   cpack -G DEB   -B <dir>      Debian and Ubuntu
#   packaging/build-appimage.sh   everything in one file, any distribution
#
# Each generator's dependency list is its own, and that separation is the
# point: the Debian list names Debian's packages and lives with whatever
# Qt an Ubuntu LTS happens to carry, and none of that reaches the rpm, the
# Arch package (packaging/aur) or the AppImage -- which bundles Qt
# precisely because it has to run where no list applies.

set(CPACK_PACKAGE_NAME "stud")
set(CPACK_PACKAGE_VERSION "${STUD_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Play Roblox on Linux")
set(CPACK_PACKAGE_DESCRIPTION
    "Stud runs the real Roblox Android client on a Linux desktop. It loads \
Roblox's own engine and drives it through the same interfaces an Android \
phone would, supplying the framework, window, input devices and graphics \
stack itself. Roblox is not included: you supply the Android application \
package yourself.")
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
# binary on purpose -- a distribution's own build carries the AppArmor
# and SELinux policy that lets it create a user namespace at all.
set(CPACK_RPM_PACKAGE_SUMMARY "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}")
# The same text, hard-wrapped: rpm carries a description verbatim and
# rpmlint rejects a line past 79 columns (description-line-too-long).
set(CPACK_RPM_PACKAGE_DESCRIPTION
"Stud runs the real Roblox Android client on a Linux desktop. It loads
Roblox's own engine and drives it through the same interfaces an Android
phone would, supplying the framework, window, input devices and graphics
stack itself. Roblox is not included: you supply the Android application
package yourself.")
set(CPACK_RPM_PACKAGE_LICENSE "AGPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")
set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
# portaudio is a real requirement, not an optional extra: the engine's own
# FMOD initialises an audio device when a game starts and fails outright
# without one. render-host loads it by name at runtime rather than linking
# it, so nothing here would notice it missing -- hence naming it.
set(CPACK_RPM_PACKAGE_REQUIRES
    "bubblewrap, qt6-qtbase-gui, qt6-qtwebengine, qtkeychain-qt6, vulkan-loader, portaudio")
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
# or the AppImage -- the AppImage in particular has to work on
# distributions this list has never heard of, which is why it bundles Qt
# instead of depending on it.
set(CPACK_DEBIAN_PACKAGE_NAME "stud")
set(CPACK_DEBIAN_PACKAGE_SECTION "games")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
# The same reasoning as the rpm's own list: bubblewrap because Process B
# is launched inside it and Stud will not start without one, and
# portaudio because render-host loads it by name at runtime rather than
# linking it, so no dependency scan would ever find it.
#
# Qt is named by its Debian binary packages rather than a version:
# whatever the distribution has is what this package builds against, and
# pinning a minimum here would only make the package uninstallable on the
# release it was built for.
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "bubblewrap, libqt6gui6, libqt6widgets6, libqt6network6, libqt6webenginewidgets6, \
libqt6keychain1, libvulkan1, libportaudio2, libfreetype6, \
libwayland-client0, libxkbcommon0")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "mangohud")
# The bundled libraries are private to Stud. Without this, dpkg-shlibdeps
# reads ANGLE and the bionic set and either invents dependencies that do
# not exist or fails outright -- they are not built against the host's
# libraries and have no business in the scan.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS OFF)

include(CPack)
