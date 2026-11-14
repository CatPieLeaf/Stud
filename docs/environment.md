# Stud's environment variables

Every `STUD_*` variable Stud reads, and what it does.

Nothing here is needed for ordinary use: Settings covers what a session needs.
These exist for packaging and for investigating a specific question.

`tests/env_documented_test.cpp` fails the build if a `STUD_*` variable exists in
the source and is missing from this file, so this list cannot quietly go stale.

---

| variable | default | what it does |
|---|---|---|
| `STUD_ROSEMARY` | unset | The Rosemary executable to run the client with, instead of the one installed with Stud or found on `PATH`. |
| `STUD_LOG_FILE` | unset | The session log. Stud sets it at launch and writes its own output and the client's there; set beforehand, Stud uses that file instead of making one under `~/.local/state/stud/logs`. |
| `STUD_HOST_LD_LIBRARY_PATH` | unset | The host's `LD_LIBRARY_PATH`, restored for programs Stud starts on the desktop's behalf. A bundle sets this. |

## What Stud sets for the client

These are Rosemary's own variables, set from Settings when Roblox starts. The
`ROSEMARY_WINDOW_*`, `ROSEMARY_STATUS_ITEMS` and `ROSEMARY_BACKGROUND_FPS`
switches come from Stud's patch to Rosemary, `packaging/rosemary/stud-launcher.patch`. The
ones below are always Settings' to decide: set in Stud's own environment, they
are not passed on. Rosemary's other variables are, so `ROSEMARY_METAL_STATS=1`
before `stud` logs Rosemary's frame statistics once a second into the session
log.

| variable | from | value |
|---|---|---|
| `ROSEMARY_PREFIX` | always | Stud's prefix, `~/.local/share/stud/prefix`. |
| `ROSEMARY_DECORATIONS` | MacOS title bar | `client` for the macOS title bar, `server` for the desktop's own. |
| `ROSEMARY_MENU` | MacOS title bar | `titlebar` with the menu bar, `none` without. |
| `ROSEMARY_CONTROL_EDIT_KEYS` | always | `1`: Ctrl for copy, cut and paste. |
| `ROSEMARY_METAL_PRESENTATION_MODE` | VSync | `fifo` when on; unset (Rosemary's `mailbox`) when off. |
| `SDL_VIDEO_DRIVER` | Display server | `wayland` on a Wayland desktop, or `x11` when chosen there and XWayland runs. |
| `ROSEMARY_WINDOW_TITLE`, `ROSEMARY_WINDOW_ICON` | always | `Stud`, and Stud's icon: the window's title bar names Stud, not the client. |
| `ROSEMARY_STATUS_ITEMS` | always | `0`: the client's menu bar extras stay out of the tray, beside Stud's own icon. |
| `ROSEMARY_BACKGROUND_FPS` | Background frame rate | The limit while no Roblox window has focus; unset for Unlimited. |
| `SDL_APP_ID` | always | Stud's application id, which gives the window Stud's icon and taskbar entry. |
| `MANGOHUD` | MangoHud overlay | `1` when on. |

Variables that make a process load code it did not ask for (`LD_*`, `DYLD_*`,
Vulkan layer and driver paths, `SDL_DYNAMIC_API`) are removed from the client's
environment.
