# Stud as a cpak

[cpak](https://github.com/Containerpak/cpak) installs an application
from an OCI image and runs it in a rootless sandbox, with the manifest
kept in a Git repository. This directory holds both halves: the image,
and the manifest that says how it may be run.

## Building the image

The build stage compiles Stud and Rosemary, and Rosemary's cargo build
fetches its git dependencies, so it needs network. CI does not use this
file: it builds `Containerfile.prebuilt` from the install tree every other
package is made from.

```sh
podman build -f packaging/cpak/Containerfile -t ghcr.io/catpieleaf/stud:2.0.5 .
podman push ghcr.io/catpieleaf/stud:2.0.5
```

## Where the manifest lives

`cpak.json` is at the **root of the repository**, not in this directory,
and it has to be: `cpak install github.com/catpieleaf/stud` fetches that
one path and nothing else. With it in here the install fails with

    Error: failed to get manifest file: failed to get file content: 404 Not Found

cpak resolves the **default branch** unless told otherwise ("No version
specified, using the default branch: main"), so a fix to the manifest
reaches users as soon as it is on `main`. It does not need a new tag or
a new release. `-r/--release`, `-b/--branch` and `-c/--commit` pick
something else.

## Publishing the manifest

Manifest v3 requires `image` to be pinned to an immutable digest, so the
tag above is only the authoring state. `cpak` resolves it:

```sh
cpak lock cpak.json
cpak test cpak.json                    # installs temporarily and runs it
```

The locked `cpak.json` goes at the root of the repository cpak installs
from (`cpak install github.com/CatPieLeaf/Stud`).

## Why each permission is in the manifest

Nothing here is granted "just in case"; every entry is something Stud
demonstrably does:

- **`socketWayland`, `deviceDri`**, Rosemary draws the client's window
  on Wayland through Vulkan and talks to the GPU directly. `displayX11` is
  deliberately absent.
- **`deviceShm`**, Rosemary and the client share memory.
- **`network`**; Roblox.
- **`socketPulseAudio`**, audio output.
- **`notification`**, the server-region notification on joining a game
  (which the user can turn off in Settings), and Stud's own test
  notification.
- **`openURI`**, the About tab's repository link.
- **`clipboard`**, NOT granted. cpak only mediates the X11 clipboard:
  with a Wayland-only socket it refuses the manifest outright,
  `clipboard mediation requires displayX11`. Stud is a Wayland
  application and its clipboard goes through the compositor (the tray
  shells out to wl-copy), so the grant bought nothing and pulling in
  displayX11 to keep it would be a far larger permission than the
  feature is worth.
- **`filePicker` + `xdg-download` read-only**; customizations and FFlag
  imports are picked from the user's files (`openFile`), a bootstrapper's
  folder for the import (`openFolder`), and where exported logs go
  (`saveFile`). Downloads is where a downloaded one lands. Nothing else of
  the host's home is visible. A picker cpak refuses answers at once, and
  Qt's portal dialog misses an answer that quick and waits forever, so
  every kind of file dialog Stud opens has to be granted.
- **`sessionBus.talk`** to the system tray's watcher, and **`own`** of
  `org.kde.StatusNotifierItem-`, for the tray icon. It needs a cpak release
  with [Containerpak/cpak#78](https://github.com/Containerpak/cpak/pull/78),
  which lets Qt ask whether the watcher exists and lets the tray host read
  the icon and menu back; older cpak shows no tray icon.
