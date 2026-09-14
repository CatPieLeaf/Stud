# Vendored Wayland protocol definitions

`pointer-warp-v1` is a **staging** protocol, and a young one: it reached
`wayland-protocols` only recently, so a distribution one release behind
does not carry it. Stud's own AppImage container was exactly that case --
the build failed with

```
ninja: error: '/usr/share/wayland-protocols/staging/pointer-warp/pointer-warp-v1.xml',
  needed by 'android-glue/generated/pointer-warp-v1-client-protocol.h'
```

A protocol definition is a small XML file with a stable, versioned
interface, so carrying a copy costs 3.5 KB and removes the build's
dependency on how new the host distribution happens to be. The system
copy is still preferred when it exists; this is the fallback.

Nothing about the RUNTIME behaviour changes: the protocol is bound only
if the compositor advertises it (KWin 6.4+, Mutter 49+, wlroots 0.19+),
and every caller is written to be correct when it is absent.

`pointer-warp-v1.xml` is MIT licensed, © 2024 Neal Gompa, Xaver Hugl,
Matthias Klumpp and Vlad Zahorodnii. Its full notice is in the file's own
`<copyright>` block, as the licence requires.
