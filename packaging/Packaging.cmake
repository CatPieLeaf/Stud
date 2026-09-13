# Packages, built from the same install tree as `cmake --install`.
#
#   cpack -G RPM   -B <dir>     Fedora, and what Discover lists
#   packaging/build-appimage.sh  everything in one file, any distribution
#
# There is deliberately no DEB generator: the AppImage is what Stud
# offers everywhere the rpm does not reach, and a third format is a third
# thing to keep working.

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
set(CPACK_PACKAGE_CONTACT "Stud contributors")
set(CPACK_PACKAGE_VENDOR "Stud contributors")
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
set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
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
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    "/usr/lib /usr/libexec /usr/share/applications /usr/share/metainfo
     /usr/share/icons /usr/share/icons/hicolor /usr/share/icons/hicolor/512x512
     /usr/share/icons/hicolor/512x512/apps /usr/share/licenses")
# Stripping or extracting build-ids from a bionic ELF or from ANGLE is
# neither useful nor safe here.
set(CPACK_RPM_SPEC_MORE_DEFINE "%global __os_install_post %{nil}
%global debug_package %{nil}
%global __brp_strip %{nil}
%global __brp_strip_static_archive %{nil}
%global __brp_check_rpaths %{nil}
%global __requires_exclude_from ^/usr/lib/stud/.*|^/usr/libexec/stud/.*$
%global __provides_exclude_from ^/usr/lib/stud/.*|^/usr/libexec/stud/.*$")

include(CPack)
