%global appid io.github.catpieleaf.Stud

# Nothing here is compiled: the release archive CI built from the tag is
# installed as it stands. ANGLE cannot be built in a build system without
# network (its own build runs `gclient sync`), and the bionic set is
# extracted from a real Android system image, so a from-source package is
# not possible here. packaging/aur/README.md has the whole reasoning.
%global debug_package %{nil}

# Stripping a bionic ELF or ANGLE is neither useful nor safe, and these are
# already-built binaries: there is nothing to strip that upstream did not mean.
%global __brp_strip %{nil}
%global __brp_strip_comment_note %{nil}
%global __brp_strip_static_archive %{nil}

# Android's own libraries are resolved by Stud's own linker64 inside bwrap,
# never by the system loader. Left alone, rpm reads their NEEDED entries and
# demands libc.so, libdl.so, liblog.so and ld-android.so from the
# distribution, which no package can satisfy.
%global __requires_exclude_from ^%{_prefix}/lib/stud/android-bionic/.*|^%{_libexecdir}/stud/lib64/.*|^%{_libexecdir}/stud/stud-runtime-bionic$
# ...and neither ANGLE's libEGL/libGLESv2/libvulkan nor Stud's own Android libraries of the
# same names may be advertised as system provides: they are Stud's private
# copies, loaded by path.
%global __provides_exclude_from ^%{_prefix}/lib/stud/(angle|android-bionic)/.*|^%{_libexecdir}/stud/lib64/.*

Name:           stud
Version:        1.1.8
Release:        1%{?dist}
Summary:        An Unofficial Open-Source Roblox Launcher for Linux

License:        AGPL-3.0-or-later
URL:            https://github.com/CatPieLeaf/Stud
Source0:        %{url}/releases/download/%{version}/stud-%{version}-x86_64.tar.zst
Packager:       CatPieLeaf <catpieleaf@proton.me>

# The archive is built for x86_64 only, and the Roblox app ships no other
# architecture Stud could host.
ExclusiveArch:  x86_64

BuildRequires:  anda-srpm-macros
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib
BuildRequires:  zstd

# bubblewrap is not optional: the Android process runs inside it and Stud
# refuses to start without one. It is a package rather than a bundled binary
# on purpose, since a distribution's own build carries the SELinux policy
# that lets it create a user namespace at all.
Requires:       bubblewrap
# Audio requires no package: miniaudio is compiled into render-host and
# opens whichever of libasound, libpulse or libjack the host has, by name.
Requires:       hicolor-icon-theme
# FFmpeg is loaded at runtime, not linked, so rpm finds no dependency on it.
# It is what the engine's video playback and screen recording use; without
# it Stud runs and simply offers the engine no video codecs. Either
# Fedora's own build or RPM Fusion's full one.
Recommends:     (libavcodec-free or ffmpeg-libs)
# The system GLES the DesktopGL render path uses is opened at runtime, by
# SONAME, so rpm cannot see it either.
Requires:       libEGL.so.1()(64bit)
Requires:       libGLESv2.so.2()(64bit)
# The X11 keyboard layout is read from the server through these, opened at
# runtime too. Weak: without them Stud falls back to the layout names the
# server publishes.
Recommends:     libxkbcommon-x11.so.0()(64bit)
Recommends:     libX11-xcb.so.1()(64bit)
# Everything else Stud links is found by rpm itself from the ELFs: Qt, Wayland,
# libxkbcommon, freetype, OpenSSL, libX11 and libXext through ANGLE.

Provides:       bundled(angle)
Provides:       bundled(swiftshader)
Provides:       bundled(vulkan-loader)
Provides:       bundled(fidelityfx-fsr1)
Provides:       bundled(snapdragon-gsr)
Provides:       bundled(mpv-prescalers)
Provides:       bundled(volk)
Provides:       bundled(xxhash)
Provides:       bundled(miniaudio)

%description
Stud runs the real, unmodified Roblox app on your Linux desktop, in its own
window, with your mouse and keyboard. No browser, no emulator, no virtual
machine.

Links from a browser open straight into the experience, and the mouse and
keyboard behave the way they do in the desktop client. The game can render
below your screen's resolution and still fill it. Discord Rich
Presence carries a button friends can join through, and a tray menu names
the country a server is in when you join one.

Roblox is not included. You supply the Android application package yourself,
and Roblox remains subject to its own terms. Stud is an independent project,
not affiliated with, endorsed by or approved by Roblox Corporation.

%prep
%autosetup -c -n %{name}-%{version}

%build
# Deliberately empty: this package installs the release archive, which CI
# already built from the tag. ANGLE's own build fetches its dependencies
# as it runs, and the bionic set is extracted from an Android system
# image rather than compiled, so there is nothing to build here.

%install
cp -a usr %{buildroot}/

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/%{appid}.desktop
appstream-util validate-relax --nonet %{buildroot}%{_datadir}/metainfo/%{appid}.metainfo.xml

%files
# The whole directory, not a list of it: every vendored notice the build
# installs belongs here, and a list is what fell behind the ones added
# since and left rpmbuild failing on unpackaged files.
%license %{_datadir}/licenses/%{name}/
%doc %{_datadir}/doc/%{name}/README.md
%doc %{_datadir}/doc/%{name}/copyright
%{_bindir}/%{name}
%{_prefix}/lib/%{name}/
%{_libexecdir}/%{name}/
%{_datadir}/applications/%{appid}.desktop
%{_datadir}/metainfo/%{appid}.metainfo.xml
%{_datadir}/icons/hicolor/*/apps/%{appid}.png
%{_mandir}/man1/%{name}.1*

%changelog
* Thu Sep 24 2026 CatPieLeaf <catpieleaf@proton.me> - 1.1.8-1
- Update to 1.1.8

* Tue Sep 22 2026 CatPieLeaf <catpieleaf@proton.me> - 1.1.7-1
- Update to 1.1.7

* Mon Sep 21 2026 CatPieLeaf <catpieleaf@proton.me> - 1.1.6-1
- Update to 1.1.6

* Thu Sep 17 2026 CatPieLeaf <catpieleaf@proton.me> - 1.1.5-1
- Update to 1.1.5

* Thu Sep 17 2026 CatPieLeaf <catpieleaf@proton.me> - 1.1.4-1
- Update to 1.1.4

* Wed Sep 16 2026 CatPieLeaf <catpieleaf@proton.me> - 1.1.3-1
- Update to 1.1.3

* Tue Sep 15 2026 CatPieLeaf <catpieleaf@proton.me> - 1.1.2-1
- Update to 1.1.2

* Tue Sep 15 2026 CatPieLeaf <catpieleaf@proton.me> - 1.1.1-1
- Initial package
