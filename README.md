<div align="center">
  <img src=".github/logo.png" width="420"></img>
  <br/>
</div>

<div align="center">

<p align="center">
  <img src="https://img.shields.io/badge/version-1.1.6-white?logo=data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCIgZmlsbD0iYmxhY2siPjxwYXRoIGQ9Ik0xMiAxLjYgMi44IDYuOHYxMC40TDEyIDIyLjRsOS4yLTUuMlY2LjhMMTIgMS42em0wIDIuMyA3IDQtNyA0LTctNCA3LTR6TTQuOCA4LjVsNi4yIDMuNnY3LjJsLTYuMi0zLjVWOC41em0xNC40IDB2Ny4zTDEzIDE5LjN2LTcuMmw2LjItMy42eiIvPjwvc3ZnPg==&style=for-the-badge&color=9a3fbd&labelColor=EBD6F5" alt="Version">
  <img src="https://img.shields.io/badge/license-AGPLv3-white?logo=gnu&logoColor=000000&style=for-the-badge&color=4c8bf5&labelColor=D6E3FC" alt="License">
  <img src="https://img.shields.io/badge/platform-linux-white?logo=linux&logoColor=000000&style=for-the-badge&color=f5bd20&labelColor=FDEFC7" alt="Platform">
  <a href="https://discord.gg/DSrbRk6dPp"><img src="https://img.shields.io/discord/1434166231274885313?label=support&logo=discord&style=for-the-badge&color=5965f1&labelColor=D6D9FC" alt="Discord"></a>
</p>

  <p align="center">Play Roblox on Linux: the real Android app, running on your desktop.</p>
</div>

<p align="center">Stud is a <b>launcher</b>. It takes the real, unmodified Roblox Android app and gives it everything it needs to run (the Android framework, a window, a mouse and keyboard, a graphics stack) so it runs on a Linux desktop instead. Roblox itself is never touched, never patched, and never shipped from here.</p>

<div align="center">

[Screenshots](#screenshots) · [Why?](#why) · [Features](#features) · [Status](#status) · [What Stud is not](#not)

[How it works](#how) · [Building](#building) · [Installation](#installation) · [Where things live](#files) · [Credits](#credits)

</div>

<br>

<p align="center">
  <a href="https://github.com/CatPieLeaf/Stud/releases"><img src=".github/get-it-on-github.png" alt="Get it on GitHub" height="70"></a>
  <a href="https://cpak.it/store/apps/stud"><img src=".github/get-it-on-cpak.png" alt="Get it on cpak" height="70"></a>
</p>

<br>
<div align="center">
  <h4 id="screenshots"><img src=".github/headers/screenshots.png" width="800" alt="Screenshots"></h4>
</div>

<div align="center">
  <img src="packaging/screenshots/home.png" width="700"></img>
  <p><i>The real Roblox home screen, on a Linux desktop.</i></p>
</div>

<table align="center">
  <tr>
    <td><a href="packaging/screenshots/game1.png"><img src="packaging/screenshots/game1.png" width="260"></a></td>
    <td><a href="packaging/screenshots/game2.png"><img src="packaging/screenshots/game2.png" width="260"></a></td>
    <td><a href="packaging/screenshots/game3.png"><img src="packaging/screenshots/game3.png" width="260"></a></td>
  </tr>
</table>

<table align="center">
  <tr>
    <td><a href="packaging/screenshots/settings1.png"><img src="packaging/screenshots/settings1.png" width="195"></a></td>
    <td><a href="packaging/screenshots/settings2.png"><img src="packaging/screenshots/settings2.png" width="195"></a></td>
    <td><a href="packaging/screenshots/settings3.png"><img src="packaging/screenshots/settings3.png" width="195"></a></td>
    <td><a href="packaging/screenshots/settings4.png"><img src="packaging/screenshots/settings4.png" width="195"></a></td>
  </tr>
</table>

<br>

<div align="center">
  <h4 id="why"><img src=".github/headers/why.png" width="800" alt="Why?"></h4>
</div>

<div align="center">
  <p><i>Honestly? Because the existing options to play Roblox on Linux kept falling over. NVIDIA, Vulkan, mid-game, often enough that playing stopped being the point and finding out why started being the point. That is one person's experience on one set of hardware, not a verdict on anyone else's work; your mileage may genuinely vary.<br><br>
  The first version of Stud made things worse in an interesting way: one process doing everything, Android code and desktop code taking turns on the same threads, a graphics driver spinning up worker threads nobody asked for in the middle of it. It crashed in places that made no sense until it became clear the design itself was the bug.<br><br>
  So it got taken apart. Three processes now, each with one job, borrowing the shape a web browser has used for years, and most of the crashes stopped being possible rather than being fixed.<br><br>
  This is a project made entirely by one person, in two months, who just wanted to have a better experience playing Roblox.</i></p>
</div>

<br>

<div align="center">
  <h4 id="features"><img src=".github/headers/features.png" width="800" alt="Features"></h4>
</div>

 - Runs the **real, unmodified** Roblox Android app
 - Three separate processes in the shape of a browser's, the same split CEF and Chromium use, so the GPU driver never shares a process with the engine. No crashes with NVIDIA at all.
 - Vulkan by default, with OpenGL through ANGLE, OpenGL, and software-rendering in Settings
 - **AMD FSR upscaling**: the game renders below your screen's resolution and Stud rebuilds the frame at full size, with adjustable sharpening
 - Smooth zoom in/out just like Windows client
 - Your login is encrypted on disk with AES-256-GCM, and only the key lives in the system keyring, the same safe-storage arrangement Chromium uses
 - `roblox://` links from a browser open straight into the experience
 - In-app web panels (Messages, account pages, login challenges) and private servers joined from the server list
 - Copy Link in an experience puts the real invite link on your clipboard
 - Discord Rich Presence, with a join button
 - Tells you which country the game server is in when you join
 - MangoHud overlay toggle, HiDPI and UI scaling, GPU picker, system tray
 - Caps the frame rate while nothing can see the window: minimised, covered, or on another workspace
 - Export every session log as one tarball, for when you file a bug
 - Ships as an **rpm**, a **deb**, an Arch **pkg.tar.zst**, a universal **AppImage**, and a **Flatpak**, plus an [AUR](https://aur.archlinux.org/packages/stud-bin) package and a [cpak](https://github.com/Containerpak/cpak)

<br>

<div align="center">
  <h4 id="status"><img src=".github/headers/workinprogress.png" width="800" alt="Work in progress"></h4>
</div>

> [!WARNING]
> **Stud is very unstable.** It boots, it logs in, it renders the real app and it joins real games. It also breaks, hangs and does strange things, sometimes for reasons nobody has measured yet. For now, treat it as something to tinker with, not something to rely on.

**Contributions of any kind are welcome**: code, bug reports, a log from a machine that isn't ours, a GPU we've never tested on, a screenshot of something rendering wrong. Nothing is too small. Open an [issue](https://github.com/CatPieLeaf/Stud/issues) and say what happened; the tray's **Export logs** puts every session in one file for you.

<br>

<div align="center">
  <h4 id="not"><img src=".github/headers/whatstudisnot.png" width="800" alt="What Stud is not"></h4>
</div>

 - **Not affiliated with Roblox.** Stud is an independent project, not endorsed, supported or approved by Roblox Corporation in any way.
 - **Not a distributor.** No Roblox APK is included here, mirrored here, or downloaded by anything here. You supply your own copy, and Roblox remains subject to its own terms.
 - **Not a modding tool.** Stud does not inject code, patch the client, alter game behaviour or give you anything you would not have on a regular client, and it will not gain support for doing so. Requests for that will be ignored and closed.
 - **Not borrowed.** Stud shares no code with any similar project. Not a single line comes from Sober or anything like it; everything here was written for this project.
 - **Not an emulator.** There is no Android system image and no virtual machine. The app's own code runs directly, with the pieces it expects supplied around it.

<br>

<div align="center">
  <h4 id="how"><img src=".github/headers/howitworks.png" width="800" alt="How it works"></h4>
</div>

Three processes, talking over a Unix socket:

| process | runs as | does |
|---|---|---|
| `stud-ui` | glibc, Qt6 | settings, deep links, starting the other two, then gets out of the way |
| `stud-runtime-bionic` | real bionic, inside `bwrap` | the engine, the JNI bridge, the Android framework stand-in |
| `stud-render-host` | glibc | Wayland or X11, ANGLE, the real Vulkan surface, audio |

Every GL and Vulkan call the engine makes is forwarded from the bionic process to the render host over that socket. It is the only place the two worlds meet, and it is Stud's own code on both sides.

<br>

<div align="center">
  <h4 id="building"><img src=".github/headers/building.png" width="800" alt="Building"></h4>
</div>

> [!NOTE]
> The long part is ANGLE, which builds from source and takes about an hour. Everything else is minutes. You need roughly **10 GB** of free space for the dependencies.

### 1 - Install the system packages

Qt 6 (base, webengine, keychain), Wayland client and `wayland-egl`, EGL/GLESv2 headers, the Vulkan loader and headers, PortAudio, `bubblewrap`, `cmake`, `ninja` and a C++20 compiler. `wl-clipboard` (or `xclip` on X11) is what "copy link" copies with.

```bash
sudo dnf install cmake ninja-build gcc-c++ qt6-qtbase-devel qt6-qtwebengine-devel \
  qtkeychain-qt6-devel wayland-devel wayland-protocols-devel libxkbcommon-devel \
  libglvnd-devel vulkan-loader-devel vulkan-headers freetype-devel \
  portaudio-devel openssl-devel bubblewrap wl-clipboard
```

<details>
<summary>Debian / Ubuntu</summary>

```bash
sudo apt install cmake ninja-build build-essential qt6-base-dev qt6-webengine-dev \
  qtkeychain-qt6-dev libwayland-dev wayland-protocols libwayland-egl-backend-dev \
  libxkbcommon-dev libegl1-mesa-dev libgles2-mesa-dev libvulkan-dev libfreetype-dev \
  portaudio19-dev libssl-dev bubblewrap wl-clipboard
```
</details>

### 2 - Dependencies

Three things are not in this repository and cannot be: Google's NDK, an ANGLE build, and a real bionic taken from AOSP. This gets all three into `third_party/`.

```bash
tools/setup.sh --plan   # what it would fetch, and from where
tools/setup.sh          # actually fetch it
```

> [!TIP]
> Already have an NDK or an ANGLE build lying around? Point at them and skip the downloads:
> ```bash
> STUD_NDK_SRC=/path/to/android-ndk-r28c STUD_ANGLE_SRC=/path/to/angle/out/Release tools/setup.sh
> ```

### 3 - Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

### 4 - Run it

```bash
./build/ui/stud-ui
```

First launch opens Settings, because Stud has no Roblox APK yet. Point it at one you obtained yourself and save. That extracts what it needs; every launch after that goes straight to Roblox.

<details>
<summary>Making packages</summary>

All three ship everything Stud needs (ANGLE, the bionic set, the runtime process) on top of what they declare as dependencies. None of them contains Roblox.

```bash
cd build
cpack -G RPM                  # build/stud-<version>-1.x86_64.rpm
cpack -G DEB                  # build/stud_<version>_amd64.deb
cd ..
packaging/build-appimage.sh   # build-appimage/packages/Stud-x86_64.AppImage
```

A deb built this way runs on the distribution that built it and anything newer, and on nothing older: Qt exports a symbol version per release and the loader checks it, so the floor a package sets is whatever Qt it was linked against. The released deb is built on the oldest Ubuntu it targets for exactly that reason; the deb job in `.github/workflows/release.yml` records the three versions this was measured on.

The AppImage builds itself inside a container so that what it links against is a decision rather than an accident of the machine that built it. It needs `podman` or `docker`.
</details>

<details>
<summary>Running the tests</summary>

```bash
cd build && ctest
```
</details>

<br>

<div align="center">
  <h4 id="installation"><img src=".github/headers/installation.png" width="800" alt="Installation"></h4>
</div>

Pre-built packages are on the [Releases](https://github.com/CatPieLeaf/Stud/releases) page.

## 🔵 R P M  ( F E D O R A )

```bash
sudo dnf install ./stud-*.x86_64.rpm
```

Stud then shows up in Discover, GNOME Software and your application menu like anything else.

## 🟣 D E B  ( D E B I A N  /  U B U N T U )

```bash
sudo apt install ./stud_*_amd64.deb
```

Built on Ubuntu 24.04, against the oldest Qt it claims to support: a Qt program runs
on the version it was built against and everything newer, never anything older, so
this installs and starts on 24.04, Debian 13, 26.04 and the Mint releases built on
them.

## 🔷 A R C H  ( P A C M A N )

```bash
sudo pacman -U stud-*-x86_64.pkg.tar.zst
```

Or from the [AUR](https://aur.archlinux.org/packages/stud-bin): `paru -S stud-bin`,
which installs the same prebuilt package:

```bash
git clone https://aur.archlinux.org/stud-bin.git && cd stud-bin && makepkg -si
```

To build from source instead, use the PKGBUILD in this repository at
`packaging/aur/` and expect around an hour, almost all of it ANGLE. It is not
the AUR package because it downloads ANGLE's own dependencies while it builds,
which a clean chroot cannot do. `packaging/aur/README.md` explains.

## 🔶 F L A T P A K

```bash
flatpak install --user ./stud-*-x86_64.flatpak
flatpak run io.github.catpieleaf.Stud
```

Not on Flathub yet, so it is a file rather than a remote. The bundle is attached to
each release and carries everything it needs. The manifest it is built from is in
`packaging/flatpak`.

## ⬛ C P A K

```bash
cpak install github.com/CatPieLeaf/Stud
```

[cpak](https://github.com/Containerpak/cpak) installs from an OCI image and runs it
rootless. The image is published to `ghcr.io/catpieleaf/stud` with each release, and
Stud is listed in the [cpak store](https://cpak.it/store/apps/stud).

## 🟠 A P P I M A G E  ( A N Y _ D I S T R O )

```bash
chmod +x Stud-x86_64.AppImage
./Stud-x86_64.AppImage
```

> [!TIP]
> An AppImage installs nothing, so nothing knows it exists yet. Run this once to get a taskbar icon and to make `roblox://` links from your browser open Stud:
> ```bash
> ./Stud-x86_64.AppImage --install-desktop-entry
> ```

> [!NOTE]
> `bubblewrap` is deliberately **not** bundled in the AppImage: it needs the AppArmor or SELinux policy your own distribution ships alongside it. Install it from your package manager (`bubblewrap`); the AppImage will tell you if it is missing. The rpm, deb and Arch packages depend on it, so those pull it in for you, and the Flatpak builds its own inside the sandbox.
>
> Most packages here are built on a current distribution, which puts a floor of glibc 2.43 under them: Fedora 44+, Ubuntu 26.04+ and current rolling releases. That includes the AppImage, which also carries its own Qt and Breeze. The deb is the exception, deliberately: it is built on Ubuntu 24.04, against the oldest Qt it claims to support, so it also runs on 24.04, Debian 13 and the Mint releases built on them. Older than that, build from source.

<br>

<div align="center">
  <h4 id="files"><img src=".github/headers/wherethingslive.png" width="800" alt="Where things live"></h4>
</div>

| path | what |
|---|---|
| `~/.config/stud/` | `config.json`, and `flags.json` if you hand-write FFlag overrides |
| `~/.local/share/stud/` | the encrypted session cookie, and the engine's own data |
| `~/.cache/stud/` | the extracted APK, assets and caches, safe to delete |
| `~/.local/state/stud/logs/` | one log per session, written by all three processes |

That last one is the file to read when something goes wrong, and the one to attach to a bug report.

In a Flatpak or cpak install the same four live under the sandbox's own home, so `~/.config/stud/` becomes `~/.var/app/io.github.catpieleaf.Stud/config/stud/` and the rest follow.

<br>

<div align="center">
  <h4 id="credits"><img src=".github/headers/credits.png" width="800" alt="Credits"></h4>
</div>

 - Roblox is a trademark of Roblox Corporation. Stud is an independent, non-commercial project and is not affiliated with, endorsed by or supported by them.
 - [RakuOS](https://repo.rakuos.org/): Stud is not affiliated with RakuOS either. They were nice and let Stud redirect its support to [their Discord](https://discord.gg/DSrbRk6dPp).
 - [ANGLE](https://chromium.googlesource.com/angle/angle): the GL translation layer (BSD), shipped with [SwiftShader](https://swiftshader.googlesource.com/SwiftShader), the [Vulkan loader](https://github.com/KhronosGroup/Vulkan-Loader), [validation layers](https://github.com/KhronosGroup/Vulkan-ValidationLayers) and [Vulkan-Tools](https://github.com/KhronosGroup/Vulkan-Tools) (Apache-2.0)
 - [bionic](https://android.googlesource.com/platform/bionic/): Android's own C library, taken from AOSP's prebuilt Runtime APEX (BSD / Apache-2.0)
 - [libjnivm](https://github.com/ChristopherHX/libjnivm): the JNI virtual machine Stud's Java layer stands on (MIT)
 - [mpv-prescalers](https://github.com/bjin/mpv-prescalers): Stud's upscaler is RAVU-Zoom, anti-ringing, generated from the vendored hook at `third_party/mpv-prescalers` (LGPL-3.0-or-later). A structure tensor over the neighbourhood's luma indexes a table of filter weights trained offline, and the result is clamped into the range that neighbourhood spans so it cannot trace a border around a hard edge
 - [Snapdragon Game Super Resolution](https://github.com/SnapdragonStudios/snapdragon-gsr): the fallback upscaler, its edge-direction variant, ported from the reference at `third_party/snapdragon-gsr` (BSD-3-Clause)
 - [AMD FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR): RCAS, the sharpening pass that runs after RAVU, ported from AMD's own reference headers at `third_party/fidelityfx-fsr1` (MIT)
 - [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo): every emulated texture is re-encoded with it, rgbcx for BC1/BC3/BC4/BC5 and bc7enc for BC7, vendored at `third_party/bc7enc` (MIT / public domain). It replaced Stud's own block encoders, which were slower on three of the four formats and had no partitioned BC7 mode, so a 4x4 block holding two distinct colours came out as a visible square on a normal map
 - [nlohmann/json](https://github.com/nlohmann/json), [miniz](https://github.com/richgel999/miniz), [detex](https://github.com/hglm/detex), [PVRTDecompress](https://github.com/powervr-graphics/Native_SDK), [PortAudio](https://www.portaudio.com/) (MIT)
 - Qt, and on the AppImage the Breeze widget style (LGPL)
 - [Boblox Classic](https://www.deviantart.com/ripoof/art/Roblox-Classic-FONT-880246616): the typeface in Stud's logo, by ripoof. The logo is an image; the font itself is not shipped with Stud

Every binary Stud redistributes carries its own licence and copyright notice. `tools/setup.sh` fetches them, and a package installs them to `/usr/share/licenses/stud/` and beside the libraries themselves.

Stud itself is **AGPLv3**, with one additional permission under section 7 ([`LICENSE.exception`](LICENSE.exception)) covering the Roblox engine Stud loads but never distributes. Stud is and always will be Open Source.

RAVU-Zoom is LGPL-3.0-or-later, the one copyleft shader in that list. LGPLv3 "incorporates the terms and conditions of version 3 of the GNU General Public License", and its section 2(b) permits conveying a copy "under the GNU GPL, with none of the additional permissions of this License applicable to that copy". AGPLv3 section 13 grants "permission to link or combine any covered work with a work licensed under version 3 of the GNU General Public License into a single combined work, and to convey the resulting work". The hook the shader is generated from ships in the tree, as the Corresponding Source section 13 requires.

[`NOTICE.md`](NOTICE.md) covers the rest: affiliation, trademarks, your Roblox account, and what does and does not leave your machine.

---
<br>

<div align="center">
  <img src=".github/icon.png" width="140"></img>
</div>
