# Stud's environment variables

Every `STUD_*` variable Stud reads, what it does, and what it costs.

Nothing here is needed for ordinary use. Stud's defaults are what a normal
session runs, and a default session already logs enough to act on a bug report:
what Stud is and how it is configured, the milestones of a launch, and anything
that actually went wrong. These switches exist for investigating a specific
question.

**How to read the tables.** *Default* is what you get with the variable unset.
A **perf** mark means turning it on costs measurable work in a hot path — fine
for an investigation, wrong for a normal session. Variables whose name begins
`STUD_NO_` or `STUD_DISABLE_` turn something **off** that is otherwise on, so
their default is "the feature runs".

`tests/env_documented_test.cpp` fails the build if a `STUD_*` variable exists in
the source and is missing from this file, so this list cannot quietly go stale.

---

## Picking a display and a GPU

| variable | default | what it does |
|---|---|---|
| `STUD_DISPLAY_BACKEND` | auto | `x11` or `wayland`, instead of detecting. The X11 path is reachable under a Wayland compositor through XWayland. |
| `STUD_ANGLE_BACKEND` | `vulkan` | Which backend ANGLE runs the GL path on. |
| `STUD_GRAPHICS_MODE` | from settings | `vulkan` or `gl`. |
| `STUD_PRESENT_MODE` | `fifo` | `engine`, `mailbox`, `immediate`, `fifo`, `fifo-relaxed`. `engine` forwards whatever the engine asked for, which is the control for measuring whether substituting helps. |
| `STUD_BACKGROUND_FPS` | `30` | Frames per second while the window is not focused. Above 240 or below 1 disables the throttle entirely. |
| `STUD_FORCE_REFRESH_HZ` | from the compositor | What refresh rate the engine is told the display runs at. |
| `STUD_UPSCALING` | from settings | `on` or `off`, overriding the upscale pass without writing to the configuration. |
| `STUD_UPSCALER` | `ravu-ar` | Which spatial upscaler runs: `ravu-ar` (RAVU-Zoom r2 anti-ringing — a structure tensor over the neighbourhood's luma indexes a table of filter weights trained offline, and the result is clamped into the range that neighbourhood spans so it cannot trace a border around a hard edge) or `sgsr-ed` (SGSR1 weighted along an estimated edge direction; no trained table, and much the cheaper of the two). Both are history-free, which is the only kind Stud can run: the engine hands it no motion vectors. Set in the environment this beats the Settings choice and says so. |
| `STUD_MAX_WINDOWS` | `1` | Raises the window cap. Diagnostic only; more than one real window is not a supported configuration. |
| `STUD_RESIZE_SETTLE_MS` | `80` | How long a window size must hold still before it is acted on. `0` acts on every configure. |
| `STUD_WL_POLL_MS` | `50` | How long the poll loops may block before Wayland is pumped again. |
| `STUD_WAYLAND_DECORATIONS` | `auto` | Who draws the titlebar. `auto` asks the compositor when it implements xdg-decoration (KDE, sway, Hyprland, river) and falls back to libdecor when it does not (GNOME). `server` never falls back, leaving the window undecorated where the compositor declines. `client` always uses libdecor, which is the only way to see that path on a session that would otherwise never take it. Wayland only; ignored on X11, and ignored entirely when `libdecor-0.so.0` is not installed. |

## The sandbox and the engine's own bring-up

| variable | default | what it does |
|---|---|---|
| `STUD_BIONIC_DIR` | `third_party/android-bionic` | Where the extracted bionic set lives. |
| `STUD_LOG_FILE` | unset | Tees this process's output into a session log. Process A sets it for both children, so a normal launch already has one. |
| `STUD_LOG_SOCKET` | set by Process A | Where to throw log lines instead of writing the file. Process A collects them and writes the file in batches, so no other process ever waits on disk I/O to say something. Unset only if A's collector failed to start, in which case each process writes the file itself. |
| `STUD_ENABLE_V2` | on | The V2 app-bridge sequence. `0` skips it; the app does not reach Home without it. |
| `STUD_DISABLE_V2_STARTAPP` | off | Skips `StartApp`. Bisecting only. |
| `STUD_ENABLE_V2_STARTGAME` | off | Forces the `StartGameWithParam` branch on a launch with no deep link. |
| `STUD_ENABLE_V2_RESUMEGAME` | off | Calls `ResumeGameWithPlatformParams`. |
| `STUD_ENABLE_STARTAPP` | off | Also calls `StartAppWithParams` on a background thread. |
| `STUD_ACK_EXPERIENCE_START` | off | Acknowledges an experience start on the launch path. |
| `STUD_SKIP_GAME_ACTIVITY` | off | Replaces the GameActivity lifecycle with a synthetic surface. Bring-up experiment. |
| `STUD_V2_RESIZE_NOTIFY` | off | Tells the V2 bridge about a resize. Known to wedge the engine; kept for measurement. |
| `STUD_CLIENT_SETTINGS_GROUP` | the real group | Which ClientSettings group to fetch. |
| `STUD_VIDEO_CODECS` | the real list | What codec support is advertised. |
| `STUD_USER_AGENT`, `STUD_ANDROID_USER_AGENT` | built from the APK | Override the User-Agent. |
| `STUD_FORCE_THEME`, `STUD_DARK_MODE` | the desktop's | Force light or dark. |
| `STUD_TEST_METRICS_DENSITY` | the real density | Test only. |
| `STUD_DNS_TEST_HOST` | unset | Resolves a host at startup and reports it, for checking DNS inside the sandbox. |

## Input

| variable | default | what it does |
|---|---|---|
| `STUD_AGDK_INPUT` | on | The AGDK input path. `0` uses the older one. |
| `STUD_NO_MOUSE_LOCK` | off | Never lock the pointer. A camera drag then stops at the window edge. |
| `STUD_NO_FOCUS_EVENTS` | off | Stop forwarding focus changes to the engine. |
| `STUD_DRAG_LOCK` | off | Alternate drag-lock behaviour. |
| `STUD_INPUT_POLL_MS` | `4` | How often input is polled. |
| `STUD_WHEEL_SCALE` | tuned | Scroll wheel scale. |
| `STUD_NO_SMOOTH_ZOOM` | off | Disable smooth zoom. |
| `STUD_ZOOM_TAU_MS`, `STUD_ZOOM_END_EPS` | tuned | Smooth-zoom response and stopping point. |
| `STUD_NO_TEXT_SYNC`, `STUD_TEXT_DONE`, `STUD_NO_TEXT_OVERLAY`, `STUD_IME_HANDSHAKE` | off | Text entry and the overlay that draws it. |
| `STUD_SYNTHESIZE_TOUCH` | off | Send touch events as well as mouse events. |
| `STUD_STUD_CURSOR` | off | Draw Stud's stand-in cursor. Normally this just puts a second, wrong cursor on screen. |
| `STUD_PAD_FIRST_ID` | `1` | First gamepad id. |
| `STUD_INPUT_TRACE` | off | Traces pointer, key, text and gamepad events. **perf** — one line per event. |

## Textures

| variable | default | what it does |
|---|---|---|
| `STUD_TEXTURE_CACHE_MB` | from settings | Transcoded-texture cache size. `0` disables it. |
| `STUD_TEX_NO_CACHE` | off | Disable the cache regardless of the setting. |
| `STUD_TEX_CACHE_FORCE` | off | Use the cache even on a rotational disk, where it is normally skipped. |
| `STUD_TEX_BC` | off | Re-encode emulated textures to BC instead of storing them uncompressed. Uncompressed is the default; BC holds mips better against the engine's texture budget but costs CPU on every newly streamed texture. **perf** |
| `STUD_TEX_ENGINE_BUDGET_MB` | 1024 | The engine's own texture budget, which Stud scales every texture's reported size against so the engine reaches it only as real GPU memory fills. Measured at 1024 from the engine's log; set this if a newer engine uses a different one. |
| `STUD_VK_REDIRECT_STATS` | off | Reports what the engine binds into each of its memory allocations: whether an allocation that received a size-scaled texture holds anything else. **perf** — a line every 2000 binds. |
| `STUD_TEX_NO_BC7` | off | With `STUD_TEX_BC`, use BC1/BC3 for colour instead of BC7. |
| `STUD_TEX_FORCE_DECODE`, `STUD_NO_TEX_DECODE` | off | Force or forbid decoding ETC/PVRTC in software. |
| `STUD_TEX_DECODE_TRACE` | off | Reports each format decoded, and what a submit's decode cost. |
| `STUD_TEX_EARLY_DECODE` | off | Decode a texture when its copy is **recorded** rather than when it is submitted, so the cost stops landing on the frame. Measured at 38 ms on one submit against frame intervals of 39-61 ms. The submit hashes the source and falls back to decoding normally if the engine wrote to it in between, so a mismatch costs nothing but the wasted work. Off by default until it has been played on. |

## Memory and the render wire

| variable | default | what it does |
|---|---|---|
| `STUD_NO_SHARED_MEMORY` | off | Stop sharing mapped memory with the host; everything crosses the socket instead. **perf** |
| `STUD_SHARE_ALL_HOST_MEMORY` | off | Substitute an importable memory type so every host-visible allocation can be shared. Moves the engine's streaming buffers out of VRAM. |
| `STUD_VK_NO_WRITE_BARRIER` | off | Use a shadow copy instead of the MMU write barrier. **perf** — `memcmp` over everything mapped, on every submit. |
| `STUD_VK_NO_UFFD_SCAN` | off | Use the fault-handler barrier instead of kernel-side tracking. **perf** |
| `STUD_VK_FULL_FLUSH` | off | Send every mapped byte on every submit, tracking nothing. **perf**, by a factor of ~180. |
| `STUD_IPC_NO_WRITER` | off | No writer thread; every call goes down the socket on the calling thread. **perf** |
| `STUD_IPC_NO_PIPELINE` | off | Flush every request immediately rather than batching. **perf** |
| `STUD_VK_SYNC_SUBMIT`, `STUD_VK_SYNC_CMDBUF` | off | Make submit/present and command-buffer bracketing blocking round trips again. **perf** |
| `STUD_DISABLE_VULKAN` | off | Make the Vulkan loader answer nothing, so the engine falls back to GL. |

## Measuring

All of these are off by default and most cost something to turn on.

| variable | what it reports | cost |
|---|---|---|
| `STUD_VK_FRAME_TIME` | Per-frame acquire/submit/present/fence/record breakdown, worst frame, commands and KB per frame. | **perf** — brackets every command |
| `STUD_VK_SLOW_FRAMES` | One line per frame that missed its deadline, with the breakdown. | **perf** |
| `STUD_VK_MEM_STATS` | Bytes sent against bytes asked for, shared against copied mappings. | low |
| `STUD_UPSCALE_TIME` | The upscale pass's own GPU time, from a timestamp either side of it: mean and worst over each 600 frames. The way to compare upscalers — process SM% measures the machine, which does not hold still. | low — two timestamp writes a frame |
| `STUD_VK_MEMREQ_STATS` | Memory-requirement cache hit rate. | low |
| `STUD_VK_HOST_TIME` | Present queue-lock and driver time, host side. | low |
| `STUD_IPC_STATS` | Round trips per frame and their wall time. | **perf** |
| `STUD_IPC_TOP` | Which call ids the round trips go to. | **perf** |
| `STUD_IPC_CONTENTION` | How long engine threads queue behind each other for the connection. | **perf** |
| `STUD_IPC_OWNERS` | Which library opened each connection. | one line each |
| `STUD_FPS` | A frame-rate line at intervals. | low |
| `STUD_FRAME_PACING` | Frame-interval distribution. | low |
| `STUD_LONG_FRAME_MS` | How long a frame must take before the flight recorder dumps what led to it (default `50`). Needs `STUD_FLIGHT_RECORDER`; the slow-present trigger cannot see these, because the present itself is fast on a stuttering frame. | low |
| `STUD_AUDIO_STATS` | Clipping and underruns in the audio feed. | low |

## Tracing

Off by default. All of these print per call or per frame — **perf**, every one.

| variable | what it traces |
|---|---|
| `STUD_VULKAN_CALL_TRACE` | Every `vkGet*ProcAddr`. |
| `STUD_VK_TRACE_OBJECTS` | Vulkan object creation and destruction. |
| `STUD_VK_TRACE_PASSES` | What each render pass targets, and swapchain image layout transitions. |
| `STUD_VK_TRACE_BARRIERS` | Image barriers Stud drops, and why. |
| `STUD_VK_TRACE_DESC` | Descriptor template sizes. |
| `STUD_VK_CMD_STATS` | A histogram of recorded command kinds. |
| `STUD_RENDER_CALL_TRACE` | Draw and clear calls, the swap counter, shader info logs. |
| `STUD_GL_TRACE_ERRORS` | Asks GL for its error after every forwarded call, so a failure names the call that caused it. |
| `STUD_GL_TRACE_TEX` | Texture uploads and parameters. |
| `STUD_SYNC_GL_ERRORS` | Check `glGetError` after every call instead of once a frame. |
| `STUD_TRACE_CLIENT_ARRAYS` | Client-side vertex array use, reported once at exit. |
| `STUD_TRACE_CURSOR` | Which draws the engine makes for its cursor. |
| `STUD_LOOPER_TRACE` | ALooper activity. |
| `STUD_TLS_TRACE` | The TLS interposer. |
| `STUD_TIME_SWAP` | Time in `eglSwapBuffers`. |

## Capturing what is on screen

| variable | default | what it does |
|---|---|---|
| `STUD_DUMP_FRAME` | off | Reads the frame back before the swap and writes it out. The honest check for "is the window really black". **perf** |
| `STUD_DUMP_FRAME_PATH` | a temp file | Where those frames go. |
| `STUD_VK_PROBE_PIXELS` | off | Reads a swapchain image back and says whether it is all black. **perf** — a full queue wait. |
| `STUD_LOOPER_POLL_FLOOR_MS` | `1` | The smallest timeout a zero-timeout `ALooper_pollOnce` is given. It exists because an engine worker thread polls in a tight loop and expects a vsync-paced swap to throttle it; with no floor it span a core flat. `0` removes the floor, which is worth trying now that `STUD_PRESENT_PACING` supplies the pacing it was standing in for — watch a core's usage when you do. |
| `STUD_PRESENT_PACING` | on | `0` stops Stud waiting for the display before letting the engine run further ahead. On, no more than two frames are ever in flight, which is what bounds input lag: it is the half of the default that keeps `fifo` from queueing frames up to the swapchain's depth. Set `STUD_PRESENT_MODE=mailbox` and you almost certainly want this `0`, since waiting on the display caps the rate that mailbox exists to exceed. Needs a driver with `VK_KHR_present_wait`; where there is none this does nothing either way. |
| `STUD_VK_VALIDATION` | off | Turns on the Vulkan validation layers, which already ship beside ANGLE. `1` for the standard checks, `sync` for those plus synchronization validation, which finds the missing or too-narrow barriers that show up as a texture flickering on one machine and not another. **perf** — the layers cost real time on every call, so this is for chasing a bug, not for playing. |
| `STUD_WL_NO_PRESENT_PUMP` | off | Stops Stud reading the Wayland connection from the thread that just returned from `vkQueuePresentKHR`, leaving the main loop to pump it. The driver reads the same connection from inside its own present; this is the last place Stud touches it from a foreign thread. **perf** — buffer releases are then dispatched on the main loop's cadence. |
| `STUD_WL_DISPATCH_DEFAULT_QUEUE` | off | Puts back Stud's dispatch of the Vulkan driver's own default Wayland queue. Off because a client has no business dispatching another component's queue from a foreign thread; the black window this once caused did not return without it. |
| `STUD_DEBUG_VALIDATION` | off | Adds the Khronos validation layer with synchronization validation to `tools/debug-session.sh`. **perf** — the heaviest switch here; it changes the timing it is used to measure. |
| `STUD_DEBUG_CHECKPOINTS` | off | Adds a GPU checkpoint per engine command to `tools/debug-session.sh`, so a device loss names the failing command. **perf** — thousands of extra recorded commands per frame. |
| `STUD_FLIGHT_RECORDER` | off | Records submits, fence waits, presents, acquires and barriers into a ring in memory and dumps the recent history whenever the device is lost, a fence sticks, or a wait or present runs long. Nothing is printed until a trigger fires. |
| `STUD_FLIGHT_RECORDER_PATH` | stdout only | Also append each flight-recorder dump to this file, which survives a terminal that scrolled away. |
| `STUD_VK_ENGINE_CHECKPOINTS` | off | Marks the engine's own commands with GPU checkpoints, so a device loss names which command the GPU died in rather than only that Stud's pass finished. **perf** — one extra recorded command per engine command. |
| `STUD_DUMP_BAD_SHADERS` | off | Writes out any shader that fails to compile, with its log. |

## Forcing a wrong answer on purpose

Each of these makes Stud do something incorrect, to find out what depends on it.

| variable | what it forces |
|---|---|
| `STUD_VK_FORCE_CLEAR` | Paints every screen-targeting render pass magenta. |
| `STUD_VK_FORCE_LOAD_CLEAR` | Forces every attachment's load operation to CLEAR. |
| `STUD_VK_FORCE_OPAQUE_FORMAT` | Rewrites the swapchain format from UNORM to SRGB. |
| `STUD_VK_REBUILD_SWAPCHAIN` | Enables rebuilding Stud's own swapchain under the engine. Off because a rebuilt swapchain can come back with a different image count than the images the engine already holds, and the upscale chain then indexes past them. |
| `STUD_GL_FULL_EXTENSIONS` | Advertises the driver's whole extension string instead of the curated list. |

## Only used by the standalone tools

`STUD_DIAG_RUNTIME_BINARY`, `STUD_DIAG_COOKIE`, `STUD_DIAG_COOKIE_FILE` (`tools/diag_launch_direct.cpp`),
`STUD_VK_GPU` (`tools/try_vulkan_window.cpp`), `STUD_VK_UFFD_SCAN` (the write-barrier test).
