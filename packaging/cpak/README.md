# Stud as a cpak

[cpak](https://github.com/Containerpak/cpak) installs an application
from an OCI image and runs it in a rootless sandbox, with the manifest
kept in a Git repository. This directory holds both halves: the image,
and the manifest that says how it may be run.

## Building the image

The build context needs `third_party/` populated (`tools/setup.sh`),
the same as any other build of Stud -- ANGLE builds from source and
takes hours, so it is not fetched inside the image build.

```sh
podman build -f packaging/cpak/Containerfile -t ghcr.io/catpieleaf/stud:0.1.0 .
podman push ghcr.io/catpieleaf/stud:0.1.0
```

## Publishing the manifest

Manifest v3 requires `image` to be pinned to an immutable digest, so the
tag above is only the authoring state. `cpak` resolves it:

```sh
cpak lock packaging/cpak/cpak.json
cpak test packaging/cpak/cpak.json     # installs temporarily and runs it
```

The locked `cpak.json` goes at the root of the repository cpak installs
from (`cpak install github.com/CatPieLeaf/Stud`).

## Why each permission is in the manifest

Nothing here is granted "just in case" -- every entry is something Stud
demonstrably does:

- **`userNamespaces`** -- the one that is not obvious. Process B is a
  real bionic ELF and Stud runs it inside its own mount, PID and IPC
  namespaces via `bwrap`, including `--unshare-user --uid 0 --gid 0`
  (the Android property area refuses to map a file this process does not
  own as root). That sandbox has to nest inside cpak's own, so cpak has
  to allow this one to create user namespaces. Without it Stud starts
  and Process B cannot.
- **`socketWayland`, `deviceDri`** -- the render host owns the real
  Wayland surface and talks to the GPU directly. There is no X11 path:
  `displayX11` is deliberately absent.
- **`deviceShm`** -- the Vulkan client backs host-visible allocations
  with shared memory.
- **`network`** -- Roblox.
- **`socketPulseAudio`** -- audio output.
- **`notification`** -- the server-region notification on joining a game
  (which the user can turn off in Settings), and Stud's own test
  notification.
- **`openURI`** -- the About tab's repository link.
- **`clipboard`** -- text boxes in the app.
- **`filePicker` + `xdg-download` read-only** -- Stud does not
  distribute Roblox; the user supplies the APK, and Downloads is where a
  downloaded one lands. Nothing else of the host's home is visible.
- **`sessionBus.talk`** -- the login cookie is stored through the
  Secret Service (and KWallet on KDE), never in Stud's own config or
  cache.

## Known gap: the keyring's own item paths

Stud stores the session cookie through QtKeychain, which after
`SearchItems` calls `GetSecret`/`Delete` on the *item* object the
keyring returns -- `/org/freedesktop/secrets/collection/<name>/<n>`,
a path that only exists at runtime.

cpak's `DBusCallGrant` takes an exact object path, so that call cannot
be expressed in this manifest. If cpak's sandbox blocks it, login will
fail to persist on a cpak install specifically. This is the one piece
of Stud's cpak support that is not confirmed working, and it needs
either a wildcard/prefix form in the grant or the Secret portal; it is
not something Stud can work around on its own without storing the
cookie somewhere it must never be stored.
