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
# ...and neither ANGLE's libEGL/libGLESv2/libvulkan nor Android's stubs of the
# same names may be advertised as system provides: they are Stud's private
# copies, loaded by path.
%global __provides_exclude_from ^%{_prefix}/lib/stud/(angle|android-bionic)/.*|^%{_libexecdir}/stud/lib64/.*

Name:           stud
Version:        1.1.1
Release:        1%{?dist}
Summary:        Play Roblox on Linux, runs the real, unmodified Roblox Android app

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
# portaudio is opened by name at runtime, so no ELF records it. The engine's
# own audio backend opens a device when a game starts and fails without it.
Requires:       portaudio
Requires:       hicolor-icon-theme
# Everything else Stud links is found by rpm itself from the ELFs: Qt, Wayland,
# libxkbcommon, freetype, OpenSSL, libX11 and libXext through ANGLE.

Provides:       bundled(angle)
Provides:       bundled(swiftshader)
Provides:       bundled(vulkan-loader)
Provides:       bundled(fidelityfx-fsr1)

%description
Stud runs the real, unmodified Roblox Android app on a Linux desktop. The
user supplies the APK from a device they own; Stud distributes none of it.

There is no emulator and no virtual machine. Stud loads the app's own native
library against a real bionic libc and linker, then answers the Android
platform calls it makes: windowing, input, audio and asset access. Those land
on Wayland, evdev and PortAudio, and rendering goes through ANGLE. The GPU
driver runs in a separate process from the Android code, because a vendor
driver spawns threads of its own and those threads do not survive foreign TLS.

%prep
%autosetup -c -n %{name}-%{version}

%build
# Deliberately empty. See the comment at the top of this file.

%install
cp -a usr %{buildroot}/

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/%{appid}.desktop
appstream-util validate-relax --nonet %{buildroot}%{_datadir}/metainfo/%{appid}.metainfo.xml

%files
%license %{_datadir}/licenses/%{name}/LICENSE
%license %{_datadir}/licenses/%{name}/LICENSE.exception
%license %{_datadir}/licenses/%{name}/NOTICE.md
%license %{_datadir}/licenses/%{name}/android-bionic/
%license %{_datadir}/licenses/%{name}/angle/
%license %{_datadir}/licenses/%{name}/fidelityfx-fsr1/
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
* Tue Sep 15 2026 CatPieLeaf <catpieleaf@proton.me> - 1.1.1-1
- Initial package
