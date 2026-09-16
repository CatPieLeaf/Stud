# Stud as a cpak

[cpak](https://github.com/Containerpak/cpak) installs an application
from an OCI image and runs it in a rootless sandbox, with the manifest
kept in a Git repository. This directory holds both halves: the image,
and the manifest that says how it may be run.

## Building the image

The build context needs `third_party/` populated (`tools/setup.sh`),
the same as any other build of Stud; ANGLE builds from source and
takes hours, so it is not fetched inside the image build.

```sh
podman build -f packaging/cpak/Containerfile -t ghcr.io/catpieleaf/stud:1.1.2 .
podman push ghcr.io/catpieleaf/stud:1.1.2
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

- **`userNamespaces`**, the one that is not obvious. Process B is a
  real bionic ELF and Stud runs it inside its own mount, PID and IPC
  namespaces via `bwrap`, including `--unshare-user --uid 0 --gid 0`
  (the Android property area refuses to map a file this process does not
  own as root). That sandbox has to nest inside cpak's own, so cpak has
  to allow this one to create user namespaces. Without it Stud starts
  and Process B cannot.
- **`socketWayland`, `deviceDri`**, the render host owns the real
  Wayland surface and talks to the GPU directly. There is no X11 path:
  `displayX11` is deliberately absent.
- **`deviceShm`**, the Vulkan client backs host-visible allocations
  with shared memory.
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
- **`filePicker` + `xdg-download` read-only**; Stud does not
  distribute Roblox; the user supplies the APK, and Downloads is where a
  downloaded one lands. Nothing else of the host's home is visible.
- **`sessionBus.talk`**, KWallet only. cpak rejects a grant naming
  `org.freedesktop.secrets` at all (`session bus policy cannot call
  org.freedesktop.secrets`); it mediates the Secret Service itself rather
  than letting an application address it. Whether QtKeychain reaches it
  through that mediation is untested; see the gap below.

  Live-tested inside cpak and it works: `secret "localstorage" stored
  (encrypted, key in the keyring)`, with the encrypted file on disk in the
  sandbox's own data directory. So the keyring is NOT the gap this file
  used to warn about.

  The system tray IS a gap, and it is cpak's to fix. Qt decides whether a
  tray exists by asking the BUS DAEMON whether org.kde.StatusNotifierWatcher
  has an owner, and cpak rejects a policy that names org.freedesktop.DBus
  ("session bus policy cannot call org.freedesktop.DBus"); it mediates
  the daemon itself. Granting talk to the watcher and its properties, and
  own of org.kde.StatusNotifierItem-, is not enough: the question is asked
  before any of that, so Qt answers "no tray" and Stud logs `no system
  tray available, exiting after launch`. Verified from the host: no Stud
  item ever appears in the watcher's RegisteredStatusNotifierItems while a
  cpak session runs.

  The login cookie is stored encrypted under
  Stud's own data directory, and the key that decrypts it is held
  through the Secret Service (and KWallet on KDE). Without the keyring
  the stored cookie cannot be read at all.

## Known gap: the keyring's own item paths

Stud stores the session cookie through QtKeychain, which after
`SearchItems` calls `GetSecret`/`Delete` on the *item* object the
keyring returns, `/org/freedesktop/secrets/collection/<name>/<n>`,
a path that only exists at runtime.

cpak's `DBusCallGrant` takes an exact object path, so that call cannot
be expressed in this manifest. If cpak's sandbox blocks it, login will
fail to persist on a cpak install specifically. This is the one piece
of Stud's cpak support that is not confirmed working, and it needs
either a wildcard/prefix form in the grant or the Secret portal; it is
not something Stud can work around on its own without storing the
cookie somewhere it must never be stored.
