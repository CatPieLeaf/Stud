#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <string_view>

// Android NDK app-glue layer: AAssetManager/AAsset, AConfiguration,
// ALooper, ANativeWindow. Distinct from libc-shim (POSIX-only) and
// jni-bridge (JNI objects) -- this is Android's own native app-support API
// surface, provided here as real, working implementations, not stubs.
//
// Found necessary by actually trying to load the real extracted
// libroblox.so through the loader (tools/try_load.cpp) -- the first
// unresolved symbol was AAssetManager_fromJava, none of these 27 symbols
// (from M1's original 546-symbol survey) had a home in libc-shim or
// jni-bridge as scoped. See the engineering notes.
//
// Implemented for real:
//  - AAssetManager/AAsset (6 symbols): reads from a real directory on disk
//    (the extracted APK's assets/ folder) via mmap, matching real Android's
//    AAsset_getBuffer zero-copy semantics.
//  - AConfiguration (9 symbols): a real struct populated with Stud's
//    desktop-spoof values, matching PlatformParams/DeviceParams (see
//    jni-bridge) rather than duplicating separate spoof logic.
//  - ALooper (7 symbols): a real epoll-based event loop, same underlying
//    mechanism Android's own ALooper uses -- not faked, forwards to the
//    same epoll_create1/epoll_ctl/epoll_wait already in libc-shim's
//    safe-forward set.
//
// ANativeWindow (5 symbols): real opaque handle, proper acquire/release
// refcounting, configurable width/height (same shape as AAssetManager's
// sentinel-handle pattern above) -- AND now backed by a real Wayland
// surface (M6, see native_window_wl_display()/native_window_wl_surface()
// below): ANativeWindow_fromSurface() connects to the host compositor
// (wl_display_connect(), binds wl_compositor via the registry) and calls
// real wl_compositor_create_surface(). Real, honest degradation if no
// compositor is reachable (e.g. a headless test environment) -- returns
// an ANativeWindow with a null surface rather than crashing; callers
// that need the surface (the Vulkan WSI shim, vulkan-wsi/) check for
// null and fail clearly instead of dereferencing garbage. What's still
// not done: giving the wl_surface an xdg-shell role (xdg_toplevel) so it
// actually becomes a visible, mapped window on screen -- that's real,
// separate windowing work, more naturally M8's territory (the Qt6
// process owns the application's actual top-level window) than
// something to build ahead of time here without a concrete consumer.

// Global scope, matching real Android NDK convention (and ndk_types.h's
// own forward declaration of the same type) -- ANativeWindow is not a
// namespaced Stud type, it's the real NDK opaque handle libroblox.so's
// imports and vulkan-wsi/ both need to agree on.
struct ANativeWindow;
struct wl_display;
struct wl_event_queue;
struct wl_surface;
struct wl_egl_window;

namespace stud::android_glue {

class NotYetSupported : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Must be called once before any AAssetManager_* symbol is used --
// establishes the real filesystem directory AAssetManager_open() reads
// from (the extracted APK's assets/ folder, or wherever Stud unpacked it).
void set_asset_base_directory(const std::string& path);

class ExtractError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Real APK unpacking (M7): extracts every entry under "assets/" inside
// the user-provided Roblox APK at `apk_path` into `dest_dir`, stripping
// the "assets/" prefix -- so `dest_dir` is immediately usable as-is with
// set_asset_base_directory() (AAssetManager_open() joins its base
// directory directly with the filename Roblox requests, e.g.
// "ssl/cacert.pem", no "assets/" prefix). Creates `dest_dir` and any
// needed subdirectories if they don't already exist. Throws ExtractError
// on a missing/corrupt APK; does nothing (not an error) for entries
// outside "assets/" -- the manifest, dex files, native libs, etc. are
// irrelevant to what AAssetManager serves.
// `apk_path` may also be an APKMirror .apkm or SAI .apks split-APK
// bundle: detected by content (a base.apk member), not by extension, its
// members are unpacked once into the cache and every one of them that
// carries assets is extracted, base first. Splits for ABIs Stud cannot
// run are skipped.
void extract_apk_assets(const std::string& apk_path, const std::string& dest_dir);

// Real APK native-library extraction (M8): pulls a single named native
// library (e.g. "libroblox.so") out of the APK's real "lib/x86_64/"
// path -- the same directory structure confirmed present in the real
// Roblox APK (M1's own symbol survey) -- and writes it to
// `dest_path`. Real, not a stub -- reuses the same miniz-based zip
// reading as extract_apk_assets(). x86_64-only, matching this whole
// project's scope (no ARM translation, see the engineering notes' core
// technical insight). Throws ExtractError if the APK is missing/corrupt
// or doesn't contain "lib/x86_64/<name>".
// Accepts a bundle here too (see extract_apk_assets above); in that case
// the library comes from whichever split actually carries the x86_64
// ABI, and a bundle with no x86_64 split is a clear error rather than a
// missing-entry one.
void extract_apk_native_library(const std::string& apk_path, const std::string& library_name,
                                 const std::string& dest_path);

// The real versionName the chosen APK (or bundle) declares in its own
// compiled AndroidManifest.xml -- e.g. "2.737.1584". Empty when it
// cannot be read, which callers must treat as "unknown" rather than
// substituting a guess: Stud used to report a hardcoded version to the
// engine, which is exactly the kind of plausible-looking fake value that
// is worse than an honest gap.
std::string apk_version_name(const std::string& apk_path);

// Identity of the chosen APK/bundle as a short string (path, size,
// mtime). Used to notice that the user picked a different build, so
// everything extracted from the previous one can be cleared instead of
// silently mixed with the new one. Empty if the file cannot be stat'd.
std::string apk_source_fingerprint(const std::string& apk_path);

// The real .apk files backing a selection: itself for a plain APK, or a
// bundle's own unpacked members (base first). Exposed for metadata
// readers; extraction callers do not need it.
std::vector<std::string> resolve_apk_sources_for_metadata(const std::string& path);

// The one, shared, XDG-compliant cache path both the UI (extracts +
// attempts the mimalloc patch here once, at APK import time -- see
// ui/src/settings_window.cpp) and the runtime process (just loads
// whatever's already there, per-launch, no re-extraction -- see
// ui/src/main.cpp's launch_game()) agree on for the extracted
// libroblox.so, so neither can silently drift onto a different path for
// what's meant to be the one cached, already-patched-or-not file.
// ($XDG_CACHE_HOME/stud/libroblox.so, or ~/.cache/stud/libroblox.so.)
std::string default_libroblox_cache_path();

// Where the APK's assets are extracted -- the same directory Process B
// uses, but as the HOST sees it. Process C needs it for the engine's own
// fonts (see stud/text_overlay.h); Process B is sandboxed and sees a
// different path, so it cannot supply this one.
std::string default_assets_cache_dir();

// Sets the dimensions ANativeWindow_getWidth/getHeight report -- just the
// values the engine sees when it queries window size, independent of
// whatever the real backing wl_surface's actual size ends up being.
void set_native_window_size(int32_t width, int32_t height);

// True once the compositor has sent a real xdg_toplevel.close event
// (the user clicked the window's own close button/gesture) -- real,
// user-reported bug fixed: the close listener used to be an empty
// no-op, so the window could never actually be closed at all. render-
// host's own main loop polls this and exits cleanly once true; doesn't
// force-close anything itself (no real window-manager-level "kill"
// semantics here, just an honest, checkable request flag).
bool window_close_requested();

// Real accessors onto ANativeWindow's Wayland backing, for the Vulkan WSI
// shim (vulkan-wsi/) to build a VkWaylandSurfaceCreateInfoKHR from a
// VkAndroidSurfaceCreateInfoKHR's ANativeWindow* -- deliberately narrow
// (just these two pointers, not the whole struct layout) so vulkan-wsi
// doesn't need to know ANativeWindow's internal representation. Both
// return nullptr if `window` is null or no Wayland compositor was
// reachable when it was created (see the ANativeWindow note above) --
// callers must check, not assume non-null.
::wl_display* native_window_wl_display(::ANativeWindow* window);
::wl_surface* native_window_wl_surface(::ANativeWindow* window);

// Real, lazily-created wl_egl_window backing for `window`, shared
// between whoever creates the real render context (runtime/main.cpp)
// and native_window.cpp's own xdg_toplevel_configure handler, so a
// real compositor-driven resize (drag, maximize) can actually resize
// it. Returns nullptr under the same honest-degradation conditions as
// native_window_wl_surface(). Idempotent -- a second call with the
// same window returns the already-created one, `width`/`height`
// ignored in that case.
// Turns real HiDPI buffer scaling on or off, and latches the scale to
// use. Called once by the process that owns the window, from Stud's own
// Settings toggle. With it on and a scaled output, Stud renders into a
// buffer `scale` times larger and tells the compositor so, which is what
// keeps the image sharp instead of upscaled; with it off the surface is
// always 1:1.
// The render scale, in 120ths (120 = 1x, 150 = 1.25x, 240 = 2x), which is
// how much bigger the buffer is than the window's logical size. 0 means
// "follow the display", which is the default and the right answer unless
// someone is trading sharpness for frame rate.
//
// This never touches the DISPLAY's own scale -- Android's
// DisplayMetrics.density keeps reporting what the monitor really is. The
// two used to be one value, and a scale of 1.0 erased the monitor's real
// one for the whole session.
//
// Must be called before any window exists: the default window size is
// derived from the display's scale and is decided here.
void set_render_scale_120(int32_t requested_scale_120);

// The scale actually in effect, in 120ths (120 = 1x, 150 = 1.25x,
// 240 = 2x) -- the unit the Wayland fractional-scale protocol itself
// uses, and the only one that can express a fractionally-scaled desktop.
// Buffer pixels = logical units * this / 120.
int32_t native_window_buffer_scale_120();

// Waits, briefly and boundedly, for the compositor to report this
// surface's fractional scale, then returns it.
//
// Needed because the scale is asked for exactly once now, before the
// engine starts, and answering early gets the integer wl_output.scale
// fallback -- which KDE reports as 2 on a 1.25x desktop. Seeding that
// tells the engine it is on a 2x display and every metric derived from
// it is wrong for the whole session.
int32_t native_window_wait_for_display_scale_120();

// An xdg-activation token for launching another application from this
// window. A Wayland compositor deliberately will not let an arbitrary
// process steal focus; handing the launched program a token minted
// against this surface is how a launcher says "the user asked for this",
// and is what makes a browser opened from Stud come to the front instead
// of appearing behind it. Empty when the compositor has no
// xdg_activation_v1, in which case the launch is simply unfocused.
std::string native_window_activation_token(ANativeWindow* window);

// Locks the pointer in place and switches it to raw relative motion, or
// releases it. This is mouse look: the compositor stops moving the cursor
// and reports deltas through kPointerRelative instead.
//
// Nothing is warped on release -- a locked pointer never moved, so it is
// still where the drag began. A no-op on a compositor without the pointer
// constraints protocol.
void native_window_set_pointer_locked(ANativeWindow* window, bool locked);

// Applies the surface's logical-size/scale state. The EGL path does this
// while creating its wl_egl_window; Vulkan attaches buffers directly and
// must ask for it explicitly.
void native_window_apply_surface_scale(::ANativeWindow* window);

// Marks the surface fully opaque, so the compositor ignores the alpha
// channel of whatever buffers are attached. Vulkan swapchain images can
// be an alpha format even with an opaque compositeAlpha -- see the
// implementation.
void native_window_set_opaque(::ANativeWindow* window);

// Stud's own Wayland event queue. Everything this process owns lives on
// it; the DEFAULT queue belongs to the Vulkan driver and must not be
// dispatched here -- doing so eats the driver's buffer-release events
// and leaves the window black. See ensure_wayland_connection().
::wl_event_queue* native_window_wl_queue();

// Real geometry of the compositor's first advertised output: its
// current mode in pixels (wl_output.mode) and its physical size in
// millimetres (wl_output.geometry). Any of these is zero when no
// compositor is reachable or it reported no such value -- an honest
// "unknown" callers must handle, not a value to substitute a guess for.
void display_output_geometry(int32_t* px_w, int32_t* px_h, int32_t* mm_w, int32_t* mm_h);

// The output's current refresh rate in mHz (144000 == 144Hz), and every
// rate it reports supporting. Both come from the compositor's own
// wl_output.mode events -- whatever display this machine actually has,
// never a fixed number. Zero/empty when the compositor has not said.
int32_t display_refresh_mhz();
std::vector<int32_t> display_supported_refresh_mhz();

::wl_egl_window* native_window_get_or_create_egl_window(::ANativeWindow* window, int32_t width,
                                                          int32_t height);

// Resolves `name` to a real function address for any of the 27 NDK
// app-glue symbols, or throws NotYetSupported for the ANativeWindow_*
// family. Returns nullptr for anything outside this module's scope, same
// convention as stud::libc_shim::resolve. Suitable to pass directly as (or
// wrap into) a stud::linker::SymbolResolver.
void* resolve(std::string_view name);

// True if `name` resolves to a real DATA object (the 9
// `AMEDIAFORMAT_KEY_*` string-constant pointers) rather than a callable
// function. See stud::libc_shim::is_data_symbol's header comment for why
// this distinction is queryable -- same reasoning applies here: wrapping
// one of these in a call trampoline breaks it, since Roblox's code
// dereferences the resolved address directly as data, never calls it.
bool is_data_symbol(std::string_view name);

// Real fix for a real, confirmed gap in real, public native_app_glue
// (NDK/AGDK library code, statically linked into libroblox.so alongside
// GameActivity -- confirmed via a real reference build,
// the engineering notes' "onSurfaceCreatedNative hang" entry): native_app_glue's
// own contract requires the app-provided android_main() to call
// ALooper_pollOnce() in a loop on its own thread to ever dispatch a
// posted command (e.g. APP_CMD_INIT_WINDOW, which is what
// onSurfaceCreatedNative's own wait blocks on) -- if android_main returns
// without looping (confirmed: Roblox's own compiled android_main does
// exactly this), that thread exits and nothing can ever poll its looper
// again, since ALooper_forThread()/ALooper_pollOnce() are strictly
// per-calling-thread by real Android convention.
//
// The real, underlying epoll fd has no actual kernel-level thread
// affinity, though (confirmed via the real source too: android_app_destroy()
// never calls ALooper_release() or closes it -- it's leaked, staying a
// valid, open fd). So a DIFFERENT thread can safely epoll_wait() on it.
// This function does exactly that: polls every ALooper ever created via
// ALooper_prepare() *other than* the calling thread's own (a real,
// version-independent, zero-libroblox.so-bytes-touched fix -- this is
// pure Stud-side ALooper-implementation behavior, not a patch to
// anything Roblox or even the NDK ships), and for each ready fd
// registered with a null callback (native_app_glue's own registration
// style, see ALooper_addFd's real call site in android_app_entry),
// interprets the returned data pointer as a real, stable, public
// `struct android_poll_source { int32_t id; void* app; void
// (*process)(void* app, void* source); }` (this exact 24-byte ABI shape
// has been part of the public NDK header for over a decade, essentially
// never changes -- a genuinely stable target, unlike anything inside
// libroblox.so itself) and calls its `process` function directly --
// exactly what a real, correctly-looping android_main would have done
// itself. Safe to call repeatedly, non-blocking (each poll uses a
// zero timeout) -- meant to be called once per iteration of Stud's own
// real event loop (runtime/main.cpp), alongside that thread's own
// ALooper_pollOnce() call.
// Returns whether it actually dispatched anything, so a caller can loop
// again straight away instead of sleeping through work that is already
// waiting -- see the poller in runtime/src/main.cpp.
bool poll_orphaned_loopers_once();

// Real Wayland seat input, bridged out of the process that actually owns
// the compositor connection (Process C, render-host) so Process B can feed
// it to libroblox's own real `NativeInputInterface` entry points. Kept
// deliberately flat/POD so it crosses the render IPC boundary as raw bytes
// with no serialisation layer of its own.
struct HostInputEvent {
    enum Type : uint32_t {
        kPointerMotion = 1,  // x, y = surface-local position
        kPointerButton = 2,  // x, y = position; code = Android button index; a != 0 => pressed
        kPointerAxis = 3,    // x, y = position; a = scroll delta (positive = up/away)
        kKey = 4,            // code = evdev scan code; a != 0 => pressed
        kPointerEnter = 5,   // pointer entered the surface; x, y = position
        kPointerLeave = 6,   // pointer left the surface
        // Raw motion while the pointer is locked for mouse look; x, y are
        // the delta, not a position. Only ever sent while locked, so it
        // never competes with kPointerMotion.
        kPointerRelative = 7,
    };
    uint32_t type = 0;
    uint32_t code = 0;
    float x = 0.0f;
    float y = 0.0f;
    float a = 0.0f;
    // kKey: non-zero when this press was synthesised by key repeat rather
    // than typed. Real Android reports the same thing through
    // KeyEvent.getRepeatCount(), which is why nativePassKeyEvent takes an
    // isRepeat flag.
    float b = 0.0f;
    // Real surface size at the moment the event happened. libroblox's own
    // touch entry point takes the view's width/height with every event, and
    // only this process knows the live window size.
    uint32_t surface_width = 0;
    uint32_t surface_height = 0;
};

// Copies up to `max` queued real input events into `out` and removes them
// from the queue; returns how many were written. Safe to call with no
// compositor connection (returns 0).
size_t native_window_drain_input_events(HostInputEvent* out, size_t max);

// Generates held-key repeats. Wayland sends only a press and a release --
// synthesising what comes between is the client's job, at the rate and
// delay the compositor reports (wl_keyboard.repeat_info). Call from
// whoever pumps Wayland; does nothing unless a key is actually held.
void native_window_pump_key_repeat();

}  // namespace stud::android_glue
