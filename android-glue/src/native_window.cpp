#include "stud/android_glue.h"

#include "x11_backend.h"
#include "stud/ndk_types.h"

#include <wayland-client.h>
#include <wayland-egl.h>
#include <xdg-decoration-unstable-v1-client-protocol.h>
#include <fractional-scale-v1-client-protocol.h>
#include <viewporter-client-protocol.h>

#include "wayland_overlay_deps.h"
#include <xdg-output-unstable-v1-client-protocol.h>
#include <pointer-constraints-unstable-v1-client-protocol.h>
#include <pointer-warp-v1-client-protocol.h>
#include <pointer-gestures-unstable-v1-client-protocol.h>
#include <relative-pointer-unstable-v1-client-protocol.h>
#include <xdg-activation-v1-client-protocol.h>
#include <xdg-shell-client-protocol.h>
#include <xkbcommon/xkbcommon.h>
#include "stud/key_compose.h"
#include "stud/keymap_chars.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string_view>
#include <unordered_map>

#include <sys/mman.h>
#include <unistd.h>

namespace {
// User-reported bug, fixed (the engineering notes, "window too big"):
// 1920x1080 as a hardcoded default made the window bigger than
// smaller real monitors and, combined with xdg_toplevel's own
// configure callback being a no-op below, meant it could never be
// resized either. 1280x720 is a much safer real default; real resizing
// (drag, maximize) now actually works via xdg_toplevel_configure.
// Stud's own default window size, in LOGICAL units, what the window
// should measure on the user's desktop before the compositor sends a
// real configure.
constexpr int32_t kDefaultLogicalWidth = 1280;
constexpr int32_t kDefaultLogicalHeight = 720;
// Floor for the computed default, for a desktop small enough that nine
// tenths of it would be unusable. Only reached on genuinely tiny screens.
constexpr int32_t kMinLogicalWidth = 640;
constexpr int32_t kMinLogicalHeight = 360;
// The size actually chosen for this display (see set_render_scale_120),
// which is what a new surface starts at before the compositor's first
// configure.
std::atomic<int32_t> g_default_logical_width{kDefaultLogicalWidth};
std::atomic<int32_t> g_default_logical_height{kDefaultLogicalHeight};
// ...and the live size in BUFFER pixels, which is what the engine
// renders into and what ANativeWindow_getWidth/getHeight report.
std::atomic<int32_t> g_window_width{kDefaultLogicalWidth};
std::atomic<int32_t> g_window_height{kDefaultLogicalHeight};

// Real Wayland connection state, established lazily on first use (the
// first ANativeWindow_fromSurface() call) and kept for the process's
// lifetime, matches how a real Android app's single native window
// backing works, one compositor connection reused for every
// ANativeWindow Stud ever creates.
struct WaylandConnectionState {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    // Real xdg-shell global (the engineering notes' M6 entry; Vulkan surface
    // *creation* never needed this; giving a wl_surface an actual
    // xdg_toplevel role, the real mechanism that makes it a visible,
    // mapped window rather than an inert client-side buffer target,
    // does). Null in a compositor that somehow doesn't advertise
    // xdg-shell (nonstandard today, but real Wayland clients still check
    // rather than assume), ANativeWindow_fromSurface degrades to a
    // plain, unmapped wl_surface in that case, same honest-degradation
    // pattern as a missing wl_compositor already uses.
    xdg_wm_base* wm_base = nullptr;
    // Real xdg-decoration global (the engineering notes, real user report:
    // "must have titlebar"). Null in a compositor that doesn't
    // implement it. Stud draws no decorations of its own, so the
    // window is simply left compositor-default (no titlebar) in that
    // case, same honest-degradation pattern as every other optional
    // Wayland global here.
    zxdg_decoration_manager_v1* decoration_manager = nullptr;
    // Real seat input (see the listeners above).
    wl_shm* shm = nullptr;
    // Real wl_subcompositor: how the text overlay (stud/text_overlay.h)
    // gets a surface of its own stacked above the game window. Null on a
    // compositor that somehow lacks it, in which case the overlay simply
    // does not appear, same honest-degradation pattern as every other
    // optional global here.
    wl_subcompositor* subcompositor = nullptr;
    // Real clipboard: a data device is obtained per-seat from this.
    wl_data_device_manager* data_device_manager = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;
    // Real wl_output, bound purely to learn the display's own genuine
    // physical size. Real Android reports DisplayMetrics.xdpi/ydpi from
    // the panel's actual dimensions; Stud used to synthesise them from
    // its density guess (160 * density), which makes every real
    // physical-size query, DeviceUtils.getScreenPhysicalSizeInMillimeters(),
    // which the engine's own getViewportDisplaySize() calls, an
    // invented number. The compositor already knows the truth and hands
    // it over in wl_output.geometry, in millimetres, for free.
    // Real fractional-scale + viewporter globals. Present on any
    // modern compositor; null elsewhere, in which case the integer
    // wl_output.scale path below is used instead.
    wp_fractional_scale_manager_v1* fractional_scale_manager = nullptr;
    wp_viewporter* viewporter = nullptr;
    wl_output* output = nullptr;
    // Real xdg-output: the only source of the display's LOGICAL size that
    // does not need a mapped surface. wl_output gives physical pixels and
    // an integer scale (2 on a 1.25x desktop), and the fractional scale
    // is per-surface and only arrives once a surface is mapped, far too
    // late to size the first window with.
    zxdg_output_manager_v1* xdg_output_manager = nullptr;
    // Mints the activation tokens that let a launched application raise
    // its own window. Null on a compositor without the protocol, in
    // which case a launch simply opens unfocused, the old behaviour.
    xdg_activation_v1* activation = nullptr;
    // Mouse look. The compositor stops moving the physical pointer while a
    // lock is held and reports raw motion deltas separately, which is the
    // only way to turn a camera without the real cursor walking off across
    // the desktop. Null on a compositor without the protocols, in which
    // case rotation still works and the cursor still wanders, the old
    // behaviour, not a crash.
    zwp_pointer_constraints_v1* pointer_constraints = nullptr;
    zwp_relative_pointer_manager_v1* relative_pointer_manager = nullptr;
    zwp_pointer_gestures_v1* pointer_gestures = nullptr;
    zwp_pointer_gesture_pinch_v1* pinch = nullptr;
    // The scale last reported. The engine wants a DELTA, not an
    // absolute factor.
    double pinch_scale = 1.0;
    zwp_relative_pointer_v1* relative_pointer = nullptr;
    zwp_locked_pointer_v1* locked_pointer = nullptr;
    // The compositor's own way to move the pointer. Absent on anything
    // older than KWin 6.4 / Mutter 49 / wlroots 0.19, in which case
    // nothing warps and every caller still behaves.
    wp_pointer_warp_v1* pointer_warp = nullptr;
    // The pointer kept inside the window for a camera drag. Unlike a
    // lock, it keeps its real position and keeps producing ordinary
    // motion. It just cannot leave, so the engine's cursor is driven
    // by the pointer itself, exactly as when nothing is constrained.
    zwp_confined_pointer_v1* confined_pointer = nullptr;
    zxdg_output_v1* xdg_output = nullptr;
    int32_t output_logical_w = 0;
    int32_t output_logical_h = 0;
    int32_t output_phys_mm_w = 0;
    int32_t output_phys_mm_h = 0;
    // Refresh rates the compositor reports for this output, in mHz.
    // Read from wl_output.mode rather than assumed: the engine paces
    // frames to what it believes the display can do, and with nothing
    // to read it settles for 60 on a panel that may be far faster,
    // or far slower. Whatever this machine actually has is what gets
    // reported.
    int32_t output_refresh_mhz = 0;
    std::vector<int32_t> output_supported_refresh_mhz;
    int32_t output_mode_px_w = 0;
    int32_t output_mode_px_h = 0;
    int32_t output_transform = 0;
    int32_t output_scale = 1;
    // Stud's own event queue. The default queue is deliberately left to
    // the Vulkan driver; see ensure_wayland_connection().
    ::wl_event_queue* queue = nullptr;
    bool attempted = false;
};

// Real buffer scale for Stud's own surface. A Wayland client that
// ignores the compositor's scale renders at logical size and is upscaled
// by the compositor, visibly blurry on any HiDPI output. Honouring it
// means: render into a buffer `scale` times larger, tell the compositor
// so with wl_surface_set_buffer_scale(), and report the larger size to
// the engine as the real surface size. 1 disables all of that, which is
// exactly right on a non-scaled output.
// Scale is tracked in 120ths, which is the unit the fractional-scale
// protocol itself uses: 120 = 1x, 150 = 1.25x, 240 = 2x. Integer-only
// wl_output.scale cannot express 1.25 at all. It rounds up to 2, which
// is why a 1.25x desktop made Stud render 60% more pixels than needed
// and still guess wrong about its own window size.
constexpr int32_t kScaleUnit = 120;
// THE DISPLAY'S OWN SCALE, as the compositor reports it. No Stud setting
// ever changes this: it is a property of the monitor, and Android's
// DisplayMetrics.density means exactly this quantity.
std::atomic<int32_t> g_display_scale_120{kScaleUnit};
// HOW MUCH BIGGER THE BUFFER IS than the window's logical size. A
// rendering choice, and the only thing the render-scale setting moves.
//
// These were one variable, and that was a real bug rather than an
// inelegance: turning HiDPI off stored the buffer scale (1.0) into the
// same variable the display scale was read from, so the monitor's real
// 1.25 was destroyed for the rest of the session. The engine was then
// told it was on a 1.0 display while the compositor still scaled the
// surface, which is exactly the reported "HiDPI off, window starts
// stretched, still high DPI".
std::atomic<int32_t> g_render_scale_120{kScaleUnit};
// The window's last known LOGICAL size. Latched here because the size
// itself lives on the window object and the upscaler needs it from
// elsewhere; see native_window_display_pixel_size().
std::atomic<int32_t> g_logical_width{0};
std::atomic<int32_t> g_logical_height{0};
// What the user asked for, in 120ths. 0 means "follow the display",
// which is the default and what HiDPI-on used to mean.
std::atomic<int32_t> g_requested_render_scale_120{0};
// The game window's own surface, for anything that needs to stack a
// surface of its own above it (the text overlay).
std::atomic<wl_surface*> g_primary_surface{nullptr};
int32_t buffer_px_from_logical(int32_t logical) {
    const int64_t scaled =
        (static_cast<int64_t>(logical) * g_render_scale_120.load() + kScaleUnit - 1) / kScaleUnit;
    return static_cast<int32_t>(scaled);
}

WaylandConnectionState& wayland_state() {
    static WaylandConnectionState state;
    return state;
}

// File-local: the public API exposes the latched value
// (native_window_buffer_scale) rather than this recomputation. Prefers
// the exact fractional value once the compositor has sent one; falls
// back to integer wl_output.scale until then (and on a compositor with
// no fractional-scale protocol at all).
// The DISPLAY's scale, as the system reports it, 150/120 on a 1.25x
// desktop. Always the real value, whatever the HiDPI setting says.
//
// This is deliberately separate from the buffer scale below. They are
// two different quantities and conflating them is what made Stud lose
// the system's DPI entirely the moment HiDPI was turned off: density
// was being derived from "how much bigger is the buffer than the
// window", which is 1.0 when Stud renders at logical size, rather than
// from "what is this display's scale", which is 1.25 either way.
int32_t display_scale_120() {
    const int32_t reported = g_display_scale_120.load();
    if (reported != kScaleUnit) return reported;
    // No integer fallback. wl_output.scale cannot express a fractional
    // desktop and KDE advertises 2 on a 1.25x one, taking that told the
    // engine it was on a 2x display, and because the scale is now read
    // exactly once it stayed wrong for the whole session. An unknown
    // scale is reported as 1.0, which is at least self-consistent with a
    // buffer that is not being multiplied by anything.
    return kScaleUnit;
}

// How much bigger the BUFFER is than the window's logical size. That is
// a rendering choice (sharpness), not a property of the display, so it
// is the only thing the HiDPI setting controls.
// Defined further down, where the backend decision actually lives.
stud::android_glue::DisplayBackend display_backend_impl();

int32_t effective_scale_120() {
    // The same rule on both backends, and X11 used to be exempt from it.
    //
    // It returned 1.0 unconditionally, on the reasoning that an X window's
    // size IS device pixels so there is no buffer to multiply. True as far
    // as it goes, and it made HiDPI and the upscaler dead on X11: the
    // engine always rendered at the full window size, so the upscaler
    // never had anything smaller to scale up and FSR did nothing at all,
    // which is exactly what a user with FSR enabled saw.
    //
    // What X11 really lacks is the compositor's own scaling, not the
    // split. So the split is reconstructed instead (native_window_pump_x11
    // derives a logical size from the window's device size and the
    // display's scale) and Stud does the scaling the compositor would
    // have done, through the same upscale pass.
    const int32_t requested = g_requested_render_scale_120.load();
    if (requested > 0) return requested;
    return display_scale_120();
}

// Real fix for a real, user-observed bug: ANativeWindow_fromSurface()
// used to ignore its `surface` argument entirely and unconditionally
// create a brand-new real Wayland surface/xdg_toplevel on every call,
// matching real Android's OWN semantics for a *first-ever* call on a
// given Surface, but wrong the moment the SAME real Surface jobject is
// handed in again (confirmed: GameActivity's real lifecycle drive,
// jni-bridge/src/game_engine_boot.cpp, correctly reuses one
// SurfaceStub jobject across both onSurfaceCreatedNative and
// onSurfaceChangedNative, exactly matching real AGDK/native_app_glue
// convention, both real entry points independently call
// ANativeWindow_fromSurface() internally per the real AGDK contract,
// so this function itself needs to be the one place idempotency is
// enforced). Without this, two real, independently-mapped windows
// appeared on screen for a single real launch. Keyed by the raw
// jobject pointer's identity, not its content, matches how real
// Android's own native window cache works (tied to the Surface
// object's own identity/lifetime, not anything about its contents).
std::unordered_map<jobject, ANativeWindow*>& window_cache() {
    static std::unordered_map<jobject, ANativeWindow*> cache;
    return cache;
}

// xdg_wm_base requires every ping to be answered with a pong, or the
// compositor is free to consider the client unresponsive and kill the
// connection: real, standard xdg-shell protocol requirement, not
// optional.
void xdg_wm_base_ping(void*, xdg_wm_base* wm_base, uint32_t serial) {
    xdg_wm_base_pong(wm_base, serial);
}

const xdg_wm_base_listener kWmBaseListener = {
    .ping = xdg_wm_base_ping,
};

// Real Wayland seat input. The compositor delivers pointer/keyboard
// events on the same connection this file already owns and already
// dispatches (render-host's own poll loop calls wl_display_dispatch on
// this fd), so the listeners below just push onto a small queue that
// Process B drains over the render IPC and replays into libroblox's own
// real `NativeInputInterface` entry points.
std::mutex& input_queue_mutex() {
    static std::mutex m;
    return m;
}
std::deque<stud::android_glue::HostInputEvent>& input_queue() {
    static std::deque<stud::android_glue::HostInputEvent> q;
    return q;
}
void push_input_event(stud::android_glue::HostInputEvent ev) {
    ev.surface_width = static_cast<uint32_t>(g_window_width.load());
    ev.surface_height = static_cast<uint32_t>(g_window_height.load());
    std::lock_guard<std::mutex> lock(input_queue_mutex());
    auto& q = input_queue();
    // Coalesce motion, exactly as a compositor and real Android both
    // already do (a MotionEvent carries its own batched history rather
    // than one event per sample). A drag produces motion far faster than
    // anything downstream consumes it, and Process B's poll is
    // best-effort. It skips a round rather than block the render thread.
    // Without this the queue builds a backlog and then replays positions
    // seconds old, which reads as a cursor that has stuck. Only ever
    // merges with the event immediately behind, so anything in between,
    // a button, a key, keeps its ordering with the motion around it.
    if (!q.empty() && q.back().type == ev.type) {
        auto& back = q.back();
        if (ev.type == stud::android_glue::HostInputEvent::kPointerMotion) {
            // Absolute: the newest position is the whole truth, and the
            // delta is recomputed downstream from the previous position.
            back = ev;
            return;
        }
        if (ev.type == stud::android_glue::HostInputEvent::kPointerRelative) {
            // Relative: the deltas are the payload, so they add.
            back.x += ev.x;
            back.y += ev.y;
            return;
        }
        if (ev.type == stud::android_glue::HostInputEvent::kPointerAxis) {
            back.a += ev.a;
            back.x = ev.x;
            back.y = ev.y;
            return;
        }
    }
    // Bound the queue: if nothing drains it (no Process B connected yet)
    // it must not grow without limit for the process's whole life.
    if (q.size() >= 4096) {
        q.pop_front();
        static unsigned dropped = 0;
        if (++dropped % 256 == 1) {
            std::fprintf(stderr, "stud-render-host: input queue full, dropped %u event(s)\n",
                         dropped);
        }
    }
    q.push_back(ev);
}

// Last pointer position, in real surface-local pixels. Button and scroll
// events carry no coordinates of their own in Wayland, but libroblox's
// own real callers (the app's own input handler) pass the last known position with both,
// so track it here exactly as that real code does.
float g_pointer_x = 0.0f;
float g_pointer_y = 0.0f;

// Real system pointer, drawn by Stud.
//
// Established from the app's own code, not assumed: the only class that
// suppresses Android's pointer (`RBXSurfaceView.onResolvePointerIcon`
// returning `PointerIcon.getSystemIcon(ctx, TYPE_NULL)`) has ZERO
// references anywhere in this build. It is dead code. The app runs on
// AGDK's `GameActivity`, whose own surface view does not suppress
// anything, so on a real device (and on Waydroid) **Android draws its
// ordinary white system pointer** over Roblox's window. Roblox does not
// draw a cursor of its own on the app shell at all, confirmed directly
// by reading back the real framebuffer, which renders the full logged-in
// Home UI and contains no cursor pixels anywhere.
//
// Stud is the Android system layer in this architecture, so supplying that
// pointer is Stud's job. This draws Android's shape (white fill, dark
// outline) into a real wl_shm buffer and hands it to the compositor.
struct CursorImage {
    wl_surface* surface = nullptr;
    bool attempted = false;
};
CursorImage& cursor_image() {
    static CursorImage c;
    return c;
}

// Classic pointer shape. 'X' = outline, '.' = fill, ' ' = transparent.
const char* const kCursorRows[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
    "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
    "X.....XXXXX ", "X..X..X     ", "X.X X..X    ", "XX  X..X    ", "X    X..X   ",
    "     X..X   ", "      XX    ",
};
constexpr int kCursorW = 12;
constexpr int kCursorH = static_cast<int>(sizeof(kCursorRows) / sizeof(kCursorRows[0]));

wl_surface* ensure_cursor_surface() {
    auto& c = cursor_image();
    if (c.attempted) return c.surface;
    c.attempted = true;
    auto& state = wayland_state();
    if (state.compositor == nullptr || state.shm == nullptr) return nullptr;

    const int stride = kCursorW * 4;
    const int size = stride * kCursorH;
    int fd = ::memfd_create("stud-cursor", MFD_CLOEXEC);
    if (fd < 0) return nullptr;
    if (::ftruncate(fd, size) != 0) {
        ::close(fd);
        return nullptr;
    }
    auto* px = static_cast<uint32_t*>(
        ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (px == MAP_FAILED) {
        ::close(fd);
        return nullptr;
    }
    // WL_SHM_FORMAT_ARGB8888 is premultiplied; fill and outline are both
    // fully opaque, so the stored values are the plain colours.
    for (int y = 0; y < kCursorH; ++y) {
        const char* row = kCursorRows[y];
        for (int x = 0; x < kCursorW; ++x) {
            char ch = row[x];
            uint32_t argb = 0x00000000;
            if (ch == 'X') argb = 0xff1a1a1a;
            else if (ch == '.') argb = 0xffffffff;
            px[y * kCursorW + x] = argb;
        }
    }
    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, size);
    wl_buffer* buffer =
        wl_shm_pool_create_buffer(pool, 0, kCursorW, kCursorH, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    ::close(fd);

    c.surface = wl_compositor_create_surface(state.compositor);
    wl_surface_attach(c.surface, buffer, 0, 0);
    wl_surface_damage(c.surface, 0, 0, kCursorW, kCursorH);
    wl_surface_commit(c.surface);
    return c.surface;
}

// Wayland reports surface-local pointer coordinates in LOGICAL units,
// but the engine works in buffer pixels (it renders into the scaled
// buffer and its own hit-testing is in that space), so every incoming
// coordinate has to be multiplied by the same scale the buffer uses.
// Missing this puts the cursor at half position on a 2x output.
// ...and back. Pointer events arrive in surface-local coordinates and are
// scaled up to buffer pixels, because that is the space the engine works
// in, but every request that takes a position back (a warp, a cursor
// hint) wants the surface-local one. Warping with a buffer pixel puts the
// pointer 1.25x away on a scaled display: live-caught as a drag anchored
// at 885 that warped to 1106 every time.
float unscale_pointer_coord(float v);

float scale_pointer_coord(double v) {
    return static_cast<float>(v * static_cast<double>(g_render_scale_120.load()) /
                              static_cast<double>(kScaleUnit));
}

float unscale_pointer_coord(float v) {
    const double scale = static_cast<double>(g_render_scale_120.load());
    if (scale <= 0.0) return v;
    return static_cast<float>(static_cast<double>(v) * static_cast<double>(kScaleUnit) / scale);
}

// The serial of the most recent real input event from the compositor.
// A compositor will not let a client raise a window; its own or one it
// launches, off nothing: xdg_activation_v1 tokens must carry the serial
// of an input event the user actually produced, which is what separates a
// user asking for a browser from an application stealing focus on a timer.
// Without it KWin issues a token and then ignores it, which is exactly why
// links opened behind Stud.
std::atomic<uint32_t> g_last_input_serial{0};

// Whether the pointer is currently locked in place for mouse look.
std::atomic<bool> g_pointer_locked{false};
// The serial of the last pointer ENTER, which is what a warp request
// takes, and specifically not `g_last_input_serial`, which every button
// and key event overwrites. A warp with the wrong serial is rejected.
std::atomic<uint32_t> g_pointer_enter_serial{0};
std::atomic<bool> g_pointer_confined{false};
// A surface may have exactly ONE pointer constraint. Asking for a second
// is a protocol error and the compositor kills the client, live-caught
// as `zwp_pointer_constraints_v1: error 1: the surface is already
// constrained` the moment first person (a lock) began while a camera drag
// still had the pointer confined, followed by VK_ERROR_SURFACE_LOST_KHR
// and a frozen window.
//
// So the lock wins while it lasts, and the confinement is remembered
// rather than created: whoever asked for it still wants it when the lock
// ends, and a lock already keeps the pointer inside the window anyway.
std::atomic<bool> g_confine_wanted{false};

void pointer_enter(void*, wl_pointer* pointer, uint32_t serial, wl_surface*, wl_fixed_t sx,
                    wl_fixed_t sy) {
    g_last_input_serial.store(serial);
    g_pointer_enter_serial.store(serial);
    g_pointer_x = scale_pointer_coord(wl_fixed_to_double(sx));
    g_pointer_y = scale_pointer_coord(wl_fixed_to_double(sy));
    // A Wayland client owns the pointer image over its own surface, and
    // must set it on every enter; passing a null surface hides the
    // compositor's cursor entirely.
    //
    // A Wayland client owns the pointer image over its own surface and must
    // set it on every enter. Roblox draws its own cursor in-frame, on the
    // app shell and in-game alike, and identically on Windows, and its own
    // SurfaceView asks Android for no system pointer at all, so hiding the
    // compositor's is the correct behaviour, not a workaround. Pass a null
    // surface.
    //
    // This was a stand-in Android arrow for several sessions, because the
    // engine's own cursor never reached the composited frame. That is fixed
    // (a buffer-mapping key bug corrupted the dynamic UI geometry; see
    // the engineering notes), and drawing one here now just puts a second, wrong
    // cursor on screen next to the real one. `STUD_STUD_CURSOR=1` brings the
    // stand-in back, which is only useful if the engine's own cursor
    // regresses.
    static const bool stud_cursor = std::getenv("STUD_STUD_CURSOR") != nullptr;
    wl_surface* cursor = stud_cursor ? ensure_cursor_surface() : nullptr;
    wl_pointer_set_cursor(pointer, serial, cursor, 0, 0);
    // Real Android sends ACTION_HOVER_ENTER when a mouse enters a view and
    // ACTION_HOVER_EXIT when it leaves; a view that only ever receives
    // HOVER_MOVE has no way to know the pointer is inside it at all.
    stud::android_glue::HostInputEvent ev;
    ev.type = stud::android_glue::HostInputEvent::kPointerEnter;
    ev.x = g_pointer_x;
    ev.y = g_pointer_y;
    push_input_event(ev);
}
void pointer_leave(void*, wl_pointer*, uint32_t, wl_surface*) {
    stud::android_glue::HostInputEvent ev;
    ev.type = stud::android_glue::HostInputEvent::kPointerLeave;
    ev.x = g_pointer_x;
    ev.y = g_pointer_y;
    push_input_event(ev);
}
void pointer_motion(void*, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
    g_pointer_x = scale_pointer_coord(wl_fixed_to_double(sx));
    g_pointer_y = scale_pointer_coord(wl_fixed_to_double(sy));
    stud::android_glue::HostInputEvent ev;
    ev.type = stud::android_glue::HostInputEvent::kPointerMotion;
    ev.x = g_pointer_x;
    ev.y = g_pointer_y;
    push_input_event(ev);
}
// Held input, and the focus change that has to let go of it. All defined
// further down, next to the state they touch; declared here because the
// listeners that use them come first in this file.
std::set<uint32_t>& buttons_down();
void release_all_held_buttons();
void push_window_focus(bool focused);

void pointer_button(void*, wl_pointer*, uint32_t serial, uint32_t, uint32_t button,
                    uint32_t state) {
    g_last_input_serial.store(serial);
    // Real Android button indices, matching what libroblox's own caller
    // passes (`MotionEvent.getActionButton() - 1`): PRIMARY(1)-1 = 0,
    // SECONDARY(2)-1 = 1, TERTIARY(4)-1 = 3. Wayland uses evdev codes.
    uint32_t android_button;
    switch (button) {
        case 0x110: android_button = 0; break;  // BTN_LEFT
        case 0x111: android_button = 1; break;  // BTN_RIGHT
        case 0x112: android_button = 3; break;  // BTN_MIDDLE
        default: return;                        // no real Android equivalent
    }
    stud::android_glue::HostInputEvent ev;
    ev.type = stud::android_glue::HostInputEvent::kPointerButton;
    ev.code = android_button;
    ev.x = g_pointer_x;
    ev.y = g_pointer_y;
    const bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (pressed) {
        buttons_down().insert(android_button);
    } else {
        buttons_down().erase(android_button);
    }
    ev.a = pressed ? 1.0f : 0.0f;
    push_input_event(ev);
}
// Scroll arrives in up to three flavours, and only one of them is exact.
//
// `axis` carries a continuous, surface-local distance whose scale is the
// compositor's own choice (KWin sends 15 per detent, others send 10), so
// dividing it by a constant makes one wheel notch mean a different amount of
// zoom on different desktops, the "steps are too wide" this used to have.
// `axis_discrete` (v5) is an exact integer notch count, and `axis_value120`
// (v8) is the same thing scaled by 120, which lets a high-resolution wheel
// report a genuine fraction of a notch. A v8 compositor sends value120
// *instead of* axis_discrete, so preferring it is safe rather than
// double-counting. One notch is reported as 1.0, matching what the desktop
// client sends per detent.
//
// All of it is accumulated and emitted on `frame`, which is what groups the
// events belonging to one physical scroll into one logical event.
double g_axis_notches = 0.0;      // exact, from value120/discrete
double g_axis_continuous = 0.0;   // fallback, from `axis`
bool g_axis_have_exact = false;
bool g_pointer_has_frame = false;

void emit_pending_axis() {
    double delta = g_axis_have_exact ? g_axis_notches : g_axis_continuous / 10.0;
    g_axis_notches = 0.0;
    g_axis_continuous = 0.0;
    g_axis_have_exact = false;
    if (delta == 0.0) return;
    stud::android_glue::HostInputEvent ev;
    ev.type = stud::android_glue::HostInputEvent::kPointerAxis;
    ev.x = g_pointer_x;
    ev.y = g_pointer_y;
    ev.a = static_cast<float>(delta);
    push_input_event(ev);
}

void pointer_axis(void*, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    // Wayland's vertical axis grows downward; Android's AXIS_VSCROLL is
    // positive scrolling up, so negate.
    g_axis_continuous += -wl_fixed_to_double(value);
    if (!g_pointer_has_frame) emit_pending_axis();
}
void pointer_axis_discrete(void*, wl_pointer*, uint32_t axis, int32_t discrete) {
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    g_axis_notches += -static_cast<double>(discrete);
    g_axis_have_exact = true;
}
void pointer_axis_value120(void*, wl_pointer*, uint32_t axis, int32_t value120) {
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    g_axis_notches += -static_cast<double>(value120) / 120.0;
    g_axis_have_exact = true;
}
void pointer_axis_relative_direction(void*, wl_pointer*, uint32_t, uint32_t) {}
void pointer_frame(void*, wl_pointer*) { emit_pending_axis(); }
void pointer_axis_source(void*, wl_pointer*, uint32_t) {}
void pointer_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}

const wl_pointer_listener kPointerListener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = pointer_axis_value120,
    .axis_relative_direction = pointer_axis_relative_direction,
    // Version 10 added this; Stud drives the cursor itself and does not
    // act on a compositor warp.
    .warp = nullptr,
};

// The compositor's own keymap, which is the only authority on what any
// key actually produces. This used to close the fd and throw it away,
// leaving a US layout compiled into Stud as the only answer, wrong for
// every other layout on earth, and the reason "/" did nothing on a
// Brazilian ABNT2 keyboard (it sits on evdev 89, which US has no key at).
//
// Only ever touched from the compositor's own dispatch thread, the same
// one that owns keys_down().
struct XkbState {
    xkb_context* context = nullptr;
    xkb_keymap* keymap = nullptr;
    xkb_state* state = nullptr;
    // Dead keys. On ABNT2 the acute and tilde are dead keys: they produce
    // no character of their own, and the character only exists once the
    // NEXT key arrives ("'" then "a" is "a-acute"). Without a compose
    // state each half resolves to nothing typable and a Portuguese
    // speaker cannot write their own language.
    //
    // The sequences come from the system's own Compose file for the
    // user's locale, so this is the same table every other application on
    // the desktop composes with, not a list Stud invented.
    xkb_compose_table* compose_table = nullptr;
    xkb_compose_state* compose_state = nullptr;
    // Every ASCII character this layout can type with an ordinary press,
    // from collect_reachable_ascii(). See HostInputEvent::layout_chars for
    // what reads it and why. All ones until a keymap has actually been
    // compiled, so "unknown" behaves the way it did before this existed.
    uint64_t ascii_reachable[2] = {~0ull, ~0ull};
};

XkbState& xkb() {
    static XkbState x;
    return x;
}

// Builds the compose state once, from the locale the user is actually
// running in. Best-effort by design: a system with no Compose file for
// its locale simply has no dead keys, which is exactly what it had
// before, rather than a failure.
void setup_compose(XkbState* x) {
    if (x->compose_state != nullptr) return;  // already built; it outlives a keymap swap
    const char* locale = std::getenv("LC_ALL");
    if (locale == nullptr || *locale == '\0') locale = std::getenv("LC_CTYPE");
    if (locale == nullptr || *locale == '\0') locale = std::getenv("LANG");
    if (locale == nullptr || *locale == '\0') locale = "C.UTF-8";
    x->compose_table =
        xkb_compose_table_new_from_locale(x->context, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (x->compose_table == nullptr) {
        std::printf("stud: no compose table for locale \"%s\", dead keys will not compose\n",
                    locale);
        std::fflush(stdout);
        return;
    }
    x->compose_state = xkb_compose_state_new(x->compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
    if (x->compose_state == nullptr) {
        xkb_compose_table_unref(x->compose_table);
        x->compose_table = nullptr;
    }
}

void keyboard_keymap(void*, wl_keyboard*, uint32_t format, int32_t fd, uint32_t size) {
    if (fd < 0) return;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        ::close(fd);
        return;
    }
    // MAP_PRIVATE, not MAP_SHARED: the compositor may hand the same file
    // to several clients, and this one has no business writing to it.
    void* mapped = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (mapped == MAP_FAILED) return;

    auto& x = xkb();
    if (x.context == nullptr) x.context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (x.context != nullptr) {
        xkb_keymap* keymap = xkb_keymap_new_from_string(x.context,
                                                        static_cast<const char*>(mapped),
                                                        XKB_KEYMAP_FORMAT_TEXT_V1,
                                                        XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (keymap != nullptr) {
            xkb_state* state = xkb_state_new(keymap);
            if (state != nullptr) {
                // Replace only once the new pair is fully built, so a
                // keymap that fails to compile leaves the old one working.
                if (x.state != nullptr) xkb_state_unref(x.state);
                if (x.keymap != nullptr) xkb_keymap_unref(x.keymap);
                x.keymap = keymap;
                x.state = state;
                stud::android_glue::collect_reachable_ascii(keymap, x.ascii_reachable);
                setup_compose(&x);
                static bool announced = false;
                if (!announced) {
                    announced = true;
                    std::printf("stud: keyboard layout from the compositor: %s\n",
                                xkb_keymap_layout_get_name(keymap, 0) != nullptr
                                    ? xkb_keymap_layout_get_name(keymap, 0)
                                    : "(unnamed)");
                    std::fflush(stdout);
                }
            } else {
                xkb_keymap_unref(keymap);
            }
        }
    }
    ::munmap(mapped, size);
}

// Fills in what the real keymap says this key produces, composing dead
// keys along the way. Leaves everything at zero when there is no keymap
// yet, which is the caller's cue to fall back to its own table.
//
// Composition only ever changes the CHARACTER, never whether the key
// event happens: the press is always delivered, so a dead key pressed
// mid-game is still a key the engine hears about. Only a press feeds the
// compose state, a release would advance the sequence a second time.
void resolve_key_from_keymap(uint32_t evdev_code, bool pressed,
                             stud::android_glue::HostInputEvent* ev) {
    auto& x = xkb();
    if (x.state == nullptr) return;
    // XKB keycodes are evdev codes plus 8, the X11 offset, which
    // Wayland keeps.
    const xkb_keycode_t keycode = evdev_code + 8;
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(x.state, keycode);
    // The keycode is always the physical key's own: "acute then a" is a
    // press of the A key that happens to type "a-acute", and reporting
    // anything else would break it as a game binding.
    ev->keysym = static_cast<uint32_t>(sym);
    ev->codepoint = xkb_state_key_get_utf32(x.state, keycode);
    ev->layout_chars[0] = x.ascii_reachable[0];
    ev->layout_chars[1] = x.ascii_reachable[1];

    if (!pressed) return;
    const stud::android_glue::ComposeResult composed =
        stud::android_glue::compose_key_press(x.compose_state, sym, ev->codepoint);
    ev->codepoint = composed.codepoint;
    ev->composed_utf8[0] = '\0';
    if (!composed.text.empty() && composed.text.size() < sizeof(ev->composed_utf8)) {
        std::memcpy(ev->composed_utf8, composed.text.data(), composed.text.size());
        ev->composed_utf8[composed.text.size()] = '\0';
    }
}
void keyboard_enter(void*, wl_keyboard*, uint32_t serial, wl_surface*, wl_array*) {
    g_last_input_serial.store(serial);
    push_window_focus(true);
}
// Defined below, once the repeat state and the held-key set they need
// exist; declared here because the listener that calls them comes first.
void release_all_held_keys();

void keyboard_leave(void*, wl_keyboard*, uint32_t, wl_surface*) {
    // Abandon any half-finished dead-key sequence. Focus went elsewhere
    // mid-sequence, and a pending acute silently swallowing the first
    // key typed on the way back is worse than losing the accent.
    if (xkb().compose_state != nullptr) xkb_compose_state_reset(xkb().compose_state);
    // Release everything still held.
    //
    // The compositor stops sending key events the moment the surface
    // loses keyboard focus, so a key released after alt-tabbing is one
    // this client never hears about, and the engine goes on holding it,
    // which in an experience means walking forever. Real Android ends the
    // gesture when a window loses focus; this does the same.
    release_all_held_keys();
    // ...and every held mouse button, for the same reason.
    release_all_held_buttons();
    // Then tell the engine itself, which keeps its own idea of what is
    // held and will not drop it just because Stud did. Real Android
    // delivers exactly this, and it is the opposite of listening in the
    // background: it says stop, rather than keep going.
    push_window_focus(false);
}
// Key repeat is the CLIENT's job on Wayland: the compositor sends a press
// and a release and nothing in between, and tells the client the rate and
// delay to synthesise the rest at (wl_keyboard.repeat_info). Nothing did,
// so holding a key produced exactly one character. Real Android repeats
// too, and marks the synthesised events as repeats, which is what
// KeyEvent.getRepeatCount() reports and why nativePassKeyEvent takes an
// isRepeat flag at all.
struct KeyRepeat {
    std::atomic<uint32_t> key{0};
    std::atomic<bool> active{false};
    // The compositor's own figures; these are the defaults it would
    // usually send, used only until it does.
    std::atomic<int32_t> rate_per_sec{25};
    std::atomic<int32_t> delay_ms{600};
    std::chrono::steady_clock::time_point next;
    std::mutex mutex;
};

KeyRepeat& key_repeat() {
    static KeyRepeat r;
    return r;
}

// Every key currently held, by evdev code. Only ever touched from the
// compositor's own dispatch thread, which is one thread.
std::set<uint32_t>& keys_down() {
    static std::set<uint32_t> k;
    return k;
}

// Every mouse button currently held, by Android button index. Same thread,
// same reason: a button still held when focus goes elsewhere is one whose
// release this client will never be told about.
std::set<uint32_t>& buttons_down() {
    static std::set<uint32_t> b;
    return b;
}

void release_all_held_keys() {
    auto& r = key_repeat();
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        r.active.store(false);
    }
    for (uint32_t key : keys_down()) {
        stud::android_glue::HostInputEvent ev;
        ev.type = stud::android_glue::HostInputEvent::kKey;
        ev.code = key;
        ev.a = 0.0f;
        resolve_key_from_keymap(key, /*pressed=*/false, &ev);
        push_input_event(ev);
    }
    keys_down().clear();
}

// The same for mouse buttons. Holding a button and clicking away leaves
// the engine holding it too, a stuck camera drag rather than a stuck
// walk, but the identical bug.
void release_all_held_buttons() {
    for (uint32_t button : buttons_down()) {
        stud::android_glue::HostInputEvent ev;
        ev.type = stud::android_glue::HostInputEvent::kPointerButton;
        ev.code = button;
        ev.x = g_pointer_x;
        ev.y = g_pointer_y;
        ev.a = 0.0f;
        push_input_event(ev);
    }
    buttons_down().clear();
}

// Tells Process B this window's keyboard focus changed, so the engine can
// be told in turn (real Android's onWindowFocusChanged).
void push_window_focus(bool focused) {
    stud::android_glue::HostInputEvent ev;
    ev.type = stud::android_glue::HostInputEvent::kWindowFocus;
    ev.a = focused ? 1.0f : 0.0f;
    push_input_event(ev);
}

void keyboard_key(void*, wl_keyboard*, uint32_t serial, uint32_t, uint32_t key, uint32_t state) {
    g_last_input_serial.store(serial);
    const bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    if (pressed) {
        keys_down().insert(key);
    } else {
        keys_down().erase(key);
    }
    {
        auto& r = key_repeat();
        std::lock_guard<std::mutex> lock(r.mutex);
        if (pressed) {
            // Only the most recent key repeats, which is what a real
            // keyboard does when a second key is pressed during a hold.
            r.key.store(key);
            r.active.store(r.rate_per_sec.load() > 0);
            r.next = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(r.delay_ms.load());
        } else if (r.key.load() == key) {
            r.active.store(false);
        }
    }
    stud::android_glue::HostInputEvent ev;
    ev.type = stud::android_glue::HostInputEvent::kKey;
    // Wayland reports the evdev keycode, which is exactly what real
    // Android's own KeyEvent.getScanCode() reports too.
    ev.code = key;
    ev.a = pressed ? 1.0f : 0.0f;
    resolve_key_from_keymap(key, pressed, &ev);
    push_input_event(ev);
}
// Shift, AltGr and the rest live here. Without them xkb resolves every
// key to its unshifted level, so a shifted "?" came back as "/".
void keyboard_modifiers(void*, wl_keyboard*, uint32_t, uint32_t depressed, uint32_t latched,
                        uint32_t locked, uint32_t group) {
    auto& x = xkb();
    if (x.state == nullptr) return;
    xkb_state_update_mask(x.state, depressed, latched, locked, 0, 0, group);
}
void keyboard_repeat_info(void*, wl_keyboard*, int32_t rate, int32_t delay) {
    auto& r = key_repeat();
    r.rate_per_sec.store(rate);
    r.delay_ms.store(delay);
    // A rate of zero is the compositor saying repeat is disabled.
    if (rate <= 0) r.active.store(false);
}

const wl_keyboard_listener kKeyboardListener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

// Raw pointer motion, delivered while the pointer is locked for mouse
// look. The compositor is not moving the cursor at this point, so this is
// the only motion there is. Roblox's own captured-pointer path reads
// exactly this (Android's AXIS_RELATIVE_X/Y) and accumulates it into the
// position it tracks itself, which is what Process B does with these.
void relative_pointer_motion(void*, zwp_relative_pointer_v1*, uint32_t, uint32_t, wl_fixed_t dx,
                             wl_fixed_t dy, wl_fixed_t, wl_fixed_t) {
    // Also while CONFINED, and that is the whole point of confining: at
    // the boundary the pointer stops moving, so ordinary motion stops
    // reporting anything, while the device carries on. These events are
    // what keeps a camera turning past the edge of the window.
    if (!g_pointer_locked.load() && !g_pointer_confined.load()) return;
    stud::android_glue::HostInputEvent ev;
    ev.type = stud::android_glue::HostInputEvent::kPointerRelative;
    ev.x = scale_pointer_coord(wl_fixed_to_double(dx));
    ev.y = scale_pointer_coord(wl_fixed_to_double(dy));
    push_input_event(ev);
}
// A touchpad pinch. The app's own handler has a mouse branch for exactly
// this (`nativePassMousePinch(x, y, (scale - last) * 3.5)`), so the
// event carries the position and the change in scale since the previous
// update, an absolute factor would make the camera jump every time a
// pinch began.
void pinch_begin(void* data, zwp_pointer_gesture_pinch_v1*, uint32_t, uint32_t, wl_surface*,
                 uint32_t) {
    auto* state = static_cast<WaylandConnectionState*>(data);
    state->pinch_scale = 1.0;
}
void pinch_update(void* data, zwp_pointer_gesture_pinch_v1*, uint32_t, wl_fixed_t, wl_fixed_t,
                  wl_fixed_t scale, wl_fixed_t) {
    auto* state = static_cast<WaylandConnectionState*>(data);
    const double now = wl_fixed_to_double(scale);
    const double delta = now - state->pinch_scale;
    state->pinch_scale = now;
    if (delta == 0.0) return;
    stud::android_glue::HostInputEvent ev{};
    ev.type = stud::android_glue::HostInputEvent::kPointerPinch;
    // The pointer does not move during a pinch, so the last known
    // position is the gesture's position, which is what the real
    // handler passes too (the first touch point's own coordinates).
    ev.x = g_pointer_x;
    ev.y = g_pointer_y;
    ev.a = static_cast<float>(delta);
    push_input_event(ev);
}
void pinch_end(void* data, zwp_pointer_gesture_pinch_v1*, uint32_t, uint32_t, int32_t) {
    auto* state = static_cast<WaylandConnectionState*>(data);
    state->pinch_scale = 1.0;
}
const zwp_pointer_gesture_pinch_v1_listener kPinchListener = {
    .begin = pinch_begin,
    .update = pinch_update,
    .end = pinch_end,
};

const zwp_relative_pointer_v1_listener kRelativePointerListener = {
    .relative_motion = relative_pointer_motion,
};

void seat_capabilities(void* data, wl_seat* seat, uint32_t caps) {
    auto* state = static_cast<WaylandConnectionState*>(data);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) != 0 && state->pointer == nullptr) {
        state->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(state->pointer, &kPointerListener, state);
        if (state->pointer_gestures != nullptr && state->pinch == nullptr) {
            state->pinch = zwp_pointer_gestures_v1_get_pinch_gesture(state->pointer_gestures,
                                                                     state->pointer);
            zwp_pointer_gesture_pinch_v1_add_listener(state->pinch, &kPinchListener, state);
        }
        if (state->relative_pointer_manager != nullptr && state->relative_pointer == nullptr) {
            state->relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(
                state->relative_pointer_manager, state->pointer);
            zwp_relative_pointer_v1_add_listener(state->relative_pointer,
                                                 &kRelativePointerListener, state);
        }
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && state->keyboard == nullptr) {
        state->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(state->keyboard, &kKeyboardListener, state);
    }
}
void seat_name(void*, wl_seat*, const char*) {}

const wl_seat_listener kSeatListener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

// Real wl_output listener. Only geometry/mode carry information Stud
// wants; done/scale are required members of the listener struct and are
// genuinely nothing-to-do here (Stud reads the values it cached, it does
// not batch state across a done event).
void output_geometry(void* data, wl_output*, int32_t, int32_t, int32_t phys_mm_w,
                     int32_t phys_mm_h, int32_t, const char*, const char*, int32_t transform) {
    auto* state = static_cast<WaylandConnectionState*>(data);
    state->output_phys_mm_w = phys_mm_w;
    state->output_phys_mm_h = phys_mm_h;
    state->output_transform = transform;
}
void output_mode(void* data, wl_output*, uint32_t flags, int32_t width, int32_t height,
                 int32_t refresh_mhz) {
    auto* state = static_cast<WaylandConnectionState*>(data);
    // Every mode is a rate the display supports; the engine wants the
    // whole set, not just the active one, so it can pick a target.
    if (refresh_mhz > 0) {
        auto& all = state->output_supported_refresh_mhz;
        if (std::find(all.begin(), all.end(), refresh_mhz) == all.end()) {
            all.push_back(refresh_mhz);
        }
    }
    if ((flags & WL_OUTPUT_MODE_CURRENT) == 0) return;
    state->output_mode_px_w = width;
    state->output_mode_px_h = height;
    state->output_refresh_mhz = refresh_mhz;
}
void output_done(void*, wl_output*) {}
void output_scale(void* data, wl_output*, int32_t factor) {
    // Real integer scale the compositor applies to this output, the
    // actual meaning of "HiDPI" for a Wayland client. A client that
    // ignores it renders at logical size and gets upscaled (blurry);
    // one that honours it renders at factor x and stays sharp.
    static_cast<WaylandConnectionState*>(data)->output_scale = factor;
}

const wl_output_listener kOutputListener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    // Version 4 of the protocol added these. Stud identifies outputs by
    // the wl_output itself, so neither is needed; listed rather than left
    // out so the struct says that on purpose.
    .name = nullptr,
    .description = nullptr,
};

void xdg_output_logical_size(void* data, zxdg_output_v1*, int32_t width, int32_t height) {
    auto* state = static_cast<WaylandConnectionState*>(data);
    if (width > 0 && height > 0) {
        state->output_logical_w = width;
        state->output_logical_h = height;
    }
}
void xdg_output_noop_position(void*, zxdg_output_v1*, int32_t, int32_t) {}
void xdg_output_noop(void*, zxdg_output_v1*) {}
void xdg_output_noop_name(void*, zxdg_output_v1*, const char*) {}

const zxdg_output_v1_listener kXdgOutputListener = {
    .logical_position = xdg_output_noop_position,
    .logical_size = xdg_output_logical_size,
    .done = xdg_output_noop,
    .name = xdg_output_noop_name,
    .description = xdg_output_noop_name,
};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                      uint32_t version) {
    auto* state = static_cast<WaylandConnectionState*>(data);
    if (std::string_view(interface) == wl_shm_interface.name) {
        // Needed to hand the compositor a real cursor image (see
        // ensure_cursor_surface(). Stud draws the pointer Android itself
        // would otherwise draw).
        state->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::string_view(interface) == wl_subcompositor_interface.name) {
        state->subcompositor = static_cast<wl_subcompositor*>(
            wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    } else if (std::string_view(interface) == wl_data_device_manager_interface.name) {
        // Version 3 is what the selection-only path needs; drag and drop
        // (which Stud does not implement) is what the later versions add.
        const uint32_t bind_version = version < 3 ? version : 3;
        state->data_device_manager = static_cast<wl_data_device_manager*>(
            wl_registry_bind(registry, name, &wl_data_device_manager_interface, bind_version));
    } else if (std::string_view(interface) == wl_seat_interface.name) {
        // Real seat, the compositor's own pointer/keyboard source. Bind at
        // 8, the version wl_pointer.axis_value120 arrived in, which is the
        // only way to learn a real, high-resolution wheel's exact fraction
        // of a detent (see the axis handlers above), or lower if the
        // compositor advertises less. Everything the listener implements
        // beyond that degrades on its own: a v5 compositor simply sends
        // axis_discrete instead, and a pre-v5 one only sends `axis`.
        uint32_t bind_version = version < 8 ? version : 8;
        g_pointer_has_frame = bind_version >= 5;
        state->seat =
            static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, bind_version));
        wl_seat_add_listener(state->seat, &kSeatListener, state);
    } else if (std::string_view(interface) == wp_pointer_warp_v1_interface.name) {
        state->pointer_warp = static_cast<wp_pointer_warp_v1*>(
            wl_registry_bind(registry, name, &wp_pointer_warp_v1_interface, 1));
        std::printf("stud-render-host: the compositor can move the pointer "
                    "(wp_pointer_warp_v1)\n");
        std::fflush(stdout);
    } else if (std::string_view(interface) == zwp_pointer_constraints_v1_interface.name) {
        state->pointer_constraints = static_cast<zwp_pointer_constraints_v1*>(
            wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1));
    } else if (std::string_view(interface) == zwp_pointer_gestures_v1_interface.name) {
        // Version 1 is all this needs: pinch begin/update/end have been
        // there since the protocol's first version.
        state->pointer_gestures = static_cast<zwp_pointer_gestures_v1*>(
            wl_registry_bind(registry, name, &zwp_pointer_gestures_v1_interface, 1));
    } else if (std::string_view(interface) == zwp_relative_pointer_manager_v1_interface.name) {
        state->relative_pointer_manager = static_cast<zwp_relative_pointer_manager_v1*>(
            wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1));
    } else if (std::string_view(interface) == xdg_activation_v1_interface.name) {
        state->activation = static_cast<xdg_activation_v1*>(
            wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1));
    } else if (std::string_view(interface) == zxdg_output_manager_v1_interface.name) {
        uint32_t bind_version = version < 2 ? version : 2;
        state->xdg_output_manager = static_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, bind_version));
    } else if (std::string_view(interface) == wl_compositor_interface.name) {
        // Bind the lowest version this build's wayland-client headers
        // know about that the compositor also advertises; real
        // wl_compositor has been ABI-stable at version 4+ for years, no
        // real feature Stud needs depends on a specific version here.
        uint32_t bind_version = version < 4 ? version : 4;
        state->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, bind_version));
    } else if (std::string_view(interface) == wp_fractional_scale_manager_v1_interface.name) {
        state->fractional_scale_manager = static_cast<wp_fractional_scale_manager_v1*>(
            wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1));
    } else if (std::string_view(interface) == wp_viewporter_interface.name) {
        state->viewporter = static_cast<wp_viewporter*>(
            wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
    } else if (std::string_view(interface) == wl_output_interface.name) {
        // Bind the first output advertised. A multi-monitor host has
        // several; Stud's window lives on one of them and cannot know
        // which before it is mapped, so the first is the honest
        // best-effort answer, and it is what a single-monitor machine
        // (the common case) reports exactly. Version 2 is where
        // wl_output.scale arrived; the listener implements through it.
        if (state->output == nullptr) {
            uint32_t bind_version = version < 2 ? version : 2;
            state->output = static_cast<wl_output*>(
                wl_registry_bind(registry, name, &wl_output_interface, bind_version));
            wl_output_add_listener(state->output, &kOutputListener, state);
        }
    } else if (std::string_view(interface) == xdg_wm_base_interface.name) {
        // Up to version 6, for xdg_toplevel's `suspended` state.
        //
        // That state is the compositor saying this surface's content is
        // not visible to anyone: minimised, on another workspace, or
        // fully covered, and it is the only honest answer to "is
        // anybody looking at this", which is what the background frame
        // limit needs. Bound at 1 before, so the state was never sent.
        // The two events versions 4 and 5 add (configure_bounds,
        // wm_capabilities) are handled below; a listener with null
        // members would crash the moment a compositor sent one.
        uint32_t bind_version = version < 6 ? version : 6;
        state->wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, bind_version));
        xdg_wm_base_add_listener(state->wm_base, &kWmBaseListener, state);
    } else if (std::string_view(interface) == zxdg_decoration_manager_v1_interface.name) {
        uint32_t bind_version = version < 1 ? version : 1;
        state->decoration_manager = static_cast<zxdg_decoration_manager_v1*>(
            wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, bind_version));
    }
}

void registry_global_remove(void*, wl_registry*, uint32_t) {
    // A compositor global disappearing mid-session isn't a case Stud
    // needs to handle for a single short-lived registry round-trip at
    // startup; nothing to do.
}

// Standard integration of a foreign (non-ALooper-native) event
// source into a real Android-style event loop, the same technique
// real apps use to fold e.g. a socket or timerfd into ALooper_pollOnce()
// (the engineering notes, "real event loop" entry: runtime/main.cpp's own
// loop only ever pumps ALooper directly; without this, a live
// xdg_wm_base.ping would never get answered once a real window exists,
// since nothing would ever call wl_display_dispatch() to process it).
// wl_display_dispatch() is safe to call unconditionally here: ALooper
// only invokes this callback when the fd is actually readable
// (ALOOPER_EVENT_INPUT), so there's always real data waiting; no
// blocking-forever risk. wl_display_flush() alongside it sends any
// requests queued since the last flush (e.g. a pong reply produced by
// dispatching the ping above) immediately, rather than waiting for
// some later, unrelated write to flush the queue.
int wayland_looper_callback(int /*fd*/, int /*events*/, void* data) {
    auto* display = static_cast<wl_display*>(data);
    wl_display_dispatch(display);
    wl_display_flush(display);
    return 1;  // keep the registration
}

// Honest degradation: if no ALooper has been prepared on this
// thread yet (ALooper_prepare() not called; see runtime/main.cpp),
// there's no real loop to register with. Not an error, matches the
// same "connect if possible, work correctly either way" pattern
// ensure_wayland_connection() already uses for a missing compositor.
void register_wayland_fd_with_looper(wl_display* display) {
    ALooper* looper = ALooper_forThread();
    if (looper == nullptr) {
        return;
    }
    int fd = wl_display_get_fd(display);
    ALooper_addFd(looper, fd, /*ident=*/0, ALOOPER_EVENT_INPUT, wayland_looper_callback, display);
}

const wl_registry_listener kRegistryListener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

// Real connection attempt: wl_display_connect() to the host compositor
// (via WAYLAND_DISPLAY/XDG_RUNTIME_DIR, same as any other Wayland
// client), then one registry round-trip to bind wl_compositor and
// xdg_wm_base. Honest degradation, not a crash, if no compositor is
// reachable (e.g. a headless test/CI environment), logs once and
// leaves display/compositor/wm_base null; callers (ANativeWindow_
// fromSurface below) check for null rather than assuming success.
// Which display server Stud is actually running on.
//
// Decided once, by what the session offers rather than by what the user
// asked for: a Wayland compositor if one answers, otherwise an X server.
// Wayland stays the preferred backend, everything in this file, and
// every measurement behind it, was built against it, and X11 is what
// keeps Stud usable on a session that has no compositor at all.
//
// STUD_DISPLAY_BACKEND=x11|wayland forces one, which is how the X11 path
// gets tested on a Wayland machine (XWayland answers DISPLAY there).
stud::android_glue::DisplayBackend& backend_storage() {
    using stud::android_glue::DisplayBackend;
    static DisplayBackend backend = DisplayBackend::Unknown;
    return backend;
}

void ensure_wayland_connection();

stud::android_glue::DisplayBackend display_backend_impl() {
    using stud::android_glue::DisplayBackend;
    DisplayBackend& backend = backend_storage();
    if (backend != DisplayBackend::Unknown) return backend;

    const char* forced = std::getenv("STUD_DISPLAY_BACKEND");
    if (forced != nullptr && std::string_view(forced) == "x11") {
        backend = stud::android_glue::x11::available() ? DisplayBackend::X11 : DisplayBackend::Wayland;
        if (backend != DisplayBackend::X11) {
            std::fprintf(stderr,
                         "stud: android-glue: STUD_DISPLAY_BACKEND=x11 but no X server answered "
                         "-- falling back to Wayland\n");
        }
        return backend;
    }
    if (forced != nullptr && std::string_view(forced) == "wayland") {
        backend = DisplayBackend::Wayland;
        return backend;
    }

    ensure_wayland_connection();
    if (wayland_state().display != nullptr) {
        backend = DisplayBackend::Wayland;
    } else if (stud::android_glue::x11::available()) {
        std::printf("stud: android-glue: no Wayland compositor, using X11\n");
        std::fflush(stdout);
        backend = DisplayBackend::X11;
    } else {
        backend = DisplayBackend::Wayland;  // nothing reachable; report as before
    }
    return backend;
}

void ensure_wayland_connection() {
    auto& state = wayland_state();
    if (state.attempted) {
        return;
    }
    state.attempted = true;

    state.display = wl_display_connect(nullptr);
    if (state.display == nullptr) {
        std::fprintf(stderr,
                      "stud: android-glue: no Wayland compositor reachable (wl_display_connect "
                      "failed), ANativeWindow objects will have no real surface backing\n");
        return;
    }

    // Stud's own objects go on their own event queue, and the DEFAULT
    // queue is left alone.
    //
    // This is not tidiness. It is the difference between a working
    // window and a black one. A Vulkan driver puts its own proxies
    // (wl_buffer, the explicit-sync timeline objects) on the default
    // queue and dispatches them itself, from inside vkQueuePresentKHR
    // and friends. If this process also dispatches the default queue, it
    // consumes those events and, having no listener for them, throws
    // them away. Live-caught in a WAYLAND_DEBUG trace as
    //   discarded wl_buffer#48.release()
    // right beside the driver's own
    //   wp_linux_drm_syncobj_surface_v1.set_acquire_point(...)
    // so the driver never learned its buffers were released, the
    // compositor never got a buffer it could show, and the window stayed
    // black even when every render pass was forced to clear to magenta.
    //
    // Proxies created from a proxy inherit its queue, so assigning the
    // registry here puts every global bound from it, and everything made
    // from those, on Stud's queue.
    state.queue = wl_display_create_queue(state.display);

    wl_registry* registry = wl_display_get_registry(state.display);
    if (state.queue != nullptr) {
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(registry), state.queue);
    }
    wl_registry_add_listener(registry, &kRegistryListener, &state);
    // Round-trip: blocks until the compositor has answered every
    // outstanding request, including the registry's initial global
    // advertisements, the standard, minimal way to synchronously
    // discover globals in a Wayland client.
    wl_display_roundtrip_queue(state.display, state.queue);
    // A second round-trip: the first one only guarantees the *globals*
    // have been advertised. wl_output's own geometry/mode events are
    // sent in response to the bind above, which happened during that
    // first round-trip, so they are still in flight at this point.
    wl_display_roundtrip_queue(state.display, state.queue);
    wl_registry_destroy(registry);

    if (state.output != nullptr) {
        std::printf("stud: android-glue: real output %dx%d px, %dx%d mm (transform=%d "
                    "scale=%d)\n",
                    state.output_mode_px_w, state.output_mode_px_h, state.output_phys_mm_w,
                    state.output_phys_mm_h, state.output_transform, state.output_scale);
        std::fflush(stdout);
    }

    if (state.compositor == nullptr) {
        std::fprintf(stderr,
                      "stud: android-glue: connected to the Wayland compositor but it never "
                      "advertised wl_compositor, ANativeWindow objects will have no real "
                      "surface backing\n");
    }
    if (state.wm_base == nullptr) {
        std::fprintf(stderr,
                      "stud: android-glue: connected to the Wayland compositor but it never "
                      "advertised xdg_wm_base, ANativeWindow objects will have no visible, "
                      "mapped window\n");
    }

    register_wayland_fd_with_looper(state.display);
}
}  // namespace

namespace stud::android_glue {
// The X11 backend lives in its own translation unit and reaches the one
// input queue through this.
void push_host_input_event(const HostInputEvent& ev) { push_input_event(ev); }

void set_native_window_size(int32_t width, int32_t height) {
    g_window_width = width;
    g_window_height = height;
}
}  // namespace stud::android_glue

// Real opaque handle with proper refcounting, now genuinely backed by a
// real wl_surface, with a real xdg_toplevel role, not just a bare
// client-side buffer target, when a compositor is reachable (see
// ensure_wayland_connection() above). `surface`/`xdg_surface`/
// `xdg_toplevel` are null if it wasn't (headless environment), checked
// by native_window_wl_surface() below, never dereferenced
// unconditionally.
struct ANativeWindow {
    std::atomic<int> ref_count{1};
    wl_surface* surface = nullptr;
    xdg_surface* shell_surface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    zxdg_toplevel_decoration_v1* decoration = nullptr;
    // Lazily-created EGL window backing (native_window_get_or_
    // create_egl_window() below), shared between whoever first
    // creates the render context (runtime/main.cpp) and this file's own
    // xdg_toplevel_configure handler, so a real compositor-driven
    // resize (drag, maximize) can actually call wl_egl_window_resize()
    // on it.
    wl_egl_window* egl_window = nullptr;
    // Real fractional-scale/viewport pair for this surface. The
    // fractional_scale object is how the compositor tells Stud the exact
    // factor for wherever the window currently is; the viewport is how
    // Stud says what logical size its (larger) buffer represents.
    wp_fractional_scale_v1* fractional_scale = nullptr;
    wp_viewport* viewport = nullptr;
    // Size the compositor asked for, in logical units. Buffer pixels are
    // derived from it and the current scale, never stored independently.
    std::atomic<int32_t> logical_width{0};
    std::atomic<int32_t> logical_height{0};
    bool configured = false;
    // The real Surface jobject this window is cached under (window_cache(),
    // below), so ANativeWindow_release() can clean up its cache entry when
    // the window is genuinely destroyed, null if this window was never
    // associated with a real jobject (surface==nullptr at creation time).
    jobject surface_key = nullptr;
};

namespace {
// Real xdg-shell requirement: the FIRST wl_surface_commit() after
// creating an xdg_toplevel only requests a configure, the surface
// isn't actually mapped/visible until the client acks that configure
// and commits again (real protocol state machine, not optional
// bookkeeping; a compositor is free to never map a surface that skips
// this). xdg_toplevel's own configure (size/state) is informational
// only for Stud's purposes right now (no real resize handling built
// yet), the ack+recommit is what matters here.
void xdg_surface_configure(void* data, xdg_surface* surface, uint32_t serial) {
    auto* window = static_cast<ANativeWindow*>(data);
    xdg_surface_ack_configure(surface, serial);
    if (!window->configured) {
        window->configured = true;
        wl_surface_commit(window->surface);
    }
}

const xdg_surface_listener kShellSurfaceListener = {
    .configure = xdg_surface_configure,
};

// Real resize handling (the engineering notes, real user report: "can't be
// resized"). Per real xdg-shell semantics, width/height of 0 means "you
// choose" (the initial configure, and some compositors' own "no
// specific size" case), only act on a real, positive suggestion (a
// user drag-resize, maximize, or fullscreen request). Resizes the real
// wl_egl_window backing (if one has been created yet; see
// native_window_get_or_create_egl_window() below) and keeps
// ANativeWindow_getWidth/getHeight in sync via the same
// set_native_window_size() this module already exposes.
// Single place that turns "logical size + current scale" into the real
// buffer size, and tells every party that needs to agree: the EGL
// window (what the engine renders into), the viewport (what logical size
// that buffer stands for), and ANativeWindow_getWidth/getHeight (what
// the engine believes its surface is). Called from both the compositor's
// configure and its preferred_scale, since either can change the answer.
// Why nothing in this file commits the surface to publish state.
//
// Wayland surface state -- the viewport destination, the opaque region,
// the buffer scale -- is double-buffered: a request stages it, and the
// NEXT wl_surface.commit publishes it, atomically, together with whatever
// buffer that commit attaches. The renderer owns those commits (the EGL
// or Vulkan driver makes one per frame), so staging here and letting the
// frame publish it is what keeps a buffer and the size it is meant to be
// shown at in the same atomic update.
//
// Committing from here instead publishes the new state against the
// buffer that happens to be attached already -- or against no buffer at
// all, before the first frame -- and does it from a different thread than
// the one presenting. On a compositor that configures once and leaves the
// window alone, the mismatched frame is over before anyone sees it. On a
// tiling compositor it is not: niri sizes the window itself in the very
// first configure and re-configures whenever a neighbour appears, so
// every one of those empty commits lands in a window that is actively
// being resized, which is the reported "flickers, showing no UI, until
// you resize it by hand" -- a manual resize ends it because it forces a
// fresh configure and a full frame that finally agree.
//
// Stud drives xdg-shell by hand rather than through a toolkit, so the
// state machine is Stud's own to get right: there is nothing underneath
// that will paper over a commit made at the wrong moment.
void apply_window_geometry(ANativeWindow* window, const char* reason) {
    if (window == nullptr) return;
    const int32_t logical_w = window->logical_width.load();
    const int32_t logical_h = window->logical_height.load();
    if (logical_w <= 0 || logical_h <= 0) return;

    // The same floor the X11 path applies; see native_window_pump_x11().
    constexpr int32_t kMinBufferPx = 64;
    int32_t buf_w = buffer_px_from_logical(logical_w);
    int32_t buf_h = buffer_px_from_logical(logical_h);
    if (buf_w < kMinBufferPx) buf_w = kMinBufferPx;
    if (buf_h < kMinBufferPx) buf_h = kMinBufferPx;
    if (buf_w == g_window_width.load() && buf_h == g_window_height.load() &&
        window->egl_window != nullptr) {
        return;
    }
    if (window->egl_window != nullptr) {
        wl_egl_window_resize(window->egl_window, buf_w, buf_h, 0, 0);
    }
    if (window->viewport != nullptr) {
        // Without this the compositor would take the buffer's own pixel
        // size as the window's logical size, which is exactly the "mini
        // window" bug in reverse: a 1.25x buffer would make the window
        // 25% larger every time the scale was applied.
        //
        // Set, NOT committed. Surface state is double-buffered: it
        // applies on the next commit, and the next commit belongs to the
        // renderer, which makes it atomically with the buffer that
        // matches it. Committing here instead publishes a new destination
        // against whatever buffer is already attached -- or against none
        // at all, before the first frame -- which is a frame the
        // compositor has to show with the two disagreeing. See
        // commit_belongs_to_the_frame() below for the whole reasoning.
        wp_viewport_set_destination(window->viewport, logical_w, logical_h);
    }
    g_logical_width.store(logical_w);
    g_logical_height.store(logical_h);
    stud::android_glue::set_native_window_size(buf_w, buf_h);
    std::printf("stud: android-glue: window %s: %dx%d logical, %dx%d buffer (scale=%d/120)\n",
                reason, logical_w, logical_h, buf_w, buf_h, g_render_scale_120.load());
    std::fflush(stdout);
}

// Real exact scale for the output this surface is actually on, which
// is the whole point of the protocol: it follows the window when it is
// dragged between differently-scaled monitors, where a single global
// output scale cannot.
void fractional_scale_preferred(void* data, wp_fractional_scale_v1*, uint32_t scale_120) {
    if (scale_120 == 0) return;
    // Always record it. This is what the DISPLAY is, and DisplayMetrics
    // reports it whether or not Stud multiplies its buffer by it. Dropping
    // it when HiDPI was off is one of the ways the system's real scale
    // used to vanish and leave the engine believing it was on a 1.0
    // display. Only the buffer decision below is conditional.
    g_display_scale_120.store(static_cast<int32_t>(scale_120));
    // The buffer follows unless the user pinned a scale of their own.
    g_render_scale_120.store(effective_scale_120());
    apply_window_geometry(static_cast<ANativeWindow*>(data), "rescaled");
}

const wp_fractional_scale_v1_listener kFractionalScaleListener = {
    .preferred_scale = fractional_scale_preferred,
};

// Whether anybody can currently see the window.
//
// Wayland answers this directly and Stud does not have to guess: the
// compositor lists `suspended` among the toplevel's states when the
// surface's content is not visible. Deliberately NOT focus: a window on a
// second monitor while you type in another one is unfocused and being
// watched, and throttling that would be worse than the waste it saves.
std::atomic<bool> g_window_visible{true};

// Whether this window is the one being used.
//
// `activated` is the compositor's own answer to "is this the focused
// window", sent in the same states array as `suspended`. Both matter and
// they are not the same question: a window can be fully visible on a
// second monitor while somebody types in another one (activated false,
// suspended false), and that is the case the background frame limit is
// really for, alt-tabbing away.
std::atomic<bool> g_window_activated{true};

void xdg_toplevel_configure(void* data, xdg_toplevel*, int32_t width, int32_t height,
                             wl_array* states) {
    if (states != nullptr) {
        bool suspended = false;
        bool activated = false;
        const auto* first = static_cast<const uint32_t*>(states->data);
        const size_t count = states->size / sizeof(uint32_t);
        for (size_t i = 0; i < count; ++i) {
            if (first[i] == XDG_TOPLEVEL_STATE_SUSPENDED) suspended = true;
            if (first[i] == XDG_TOPLEVEL_STATE_ACTIVATED) activated = true;
        }
        const bool visible = !suspended;
        if (visible != g_window_visible.exchange(visible)) {
            std::printf("stud: android-glue: window %s\n",
                         visible ? "visible again" : "no longer visible (suspended)");
            std::fflush(stdout);
        }
        if (activated != g_window_activated.exchange(activated)) {
            std::printf("stud: android-glue: window %s\n",
                         activated ? "focused" : "in the background");
            std::fflush(stdout);
        }
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    auto* window = static_cast<ANativeWindow*>(data);
    window->logical_width.store(width);
    window->logical_height.store(height);
    apply_window_geometry(window, "resized");
}

// Versions 4 and 5 of xdg_toplevel send these; nothing here acts on them,
// but the listener must carry them or libwayland calls through a null.
void xdg_toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void xdg_toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}
std::atomic<bool> g_window_close_requested{false};
void xdg_toplevel_close(void*, xdg_toplevel*) { g_window_close_requested.store(true); }

const xdg_toplevel_listener kToplevelListener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
    .configure_bounds = xdg_toplevel_configure_bounds,
    .wm_capabilities = xdg_toplevel_wm_capabilities,
};
}  // namespace

extern "C" {

ANativeWindow* ANativeWindow_fromSurface(JNIEnv* /*env*/, jobject surface) {
    // No real Java Surface object exists in Stud's model (Stud's own
    // process owns the window, not a Java-side Surface), the jobject
    // itself carries no real content Stud reads, same pattern as
    // AAssetManager_fromJava's. Its IDENTITY still matters, though;
    // see window_cache()'s own doc comment above for the real bug this
    // fixes (two windows appearing for one launch).
    if (surface != nullptr) {
        auto& cache = window_cache();
        auto it = cache.find(surface);
        if (it != cache.end()) {
            it->second->ref_count.fetch_add(1);
            return it->second;
        }
    } else if (!window_cache().empty()) {
        // User-reported bug, fixed (the engineering notes, "still two
        // invisible windows" entry): confirmed via a real diagnostic
        // trace that once the real production window already exists
        // (window_cache() non-empty), Roblox's own compiled code
        // sometimes calls this real, exported NDK symbol directly with
        // a genuinely null Surface (some internal defensive/fallback
        // path, not a Stud call site. Every one of Stud's own real
        // call sites always passes the real, non-null surface jobject).
        // Treating that as "create a fresh standalone window" (the
        // right behavior for the null-surface case BEFORE any real
        // window exists; see below) spawned a second, real,
        // independently-mapped, never-fully-configured (hence
        // invisible) window. A null Surface once a real one already
        // exists has no legitimate window to create; degrade honestly
        // instead.
        return nullptr;
    }

    // Hard guard against creating a second real window from a null
    // Surface. The cache is keyed by jobject identity, so a null Surface
    // never populates it, which used to mean every null-Surface call
    // fell through here and mapped a brand new Wayland window. One
    // caller repeating that in a loop put over a hundred windows on the
    // user's desktop. Exactly one window is ever legitimate from this
    // path, so remember that it happened rather than relying on a cache
    // that this path cannot fill.
    static bool created_null_surface_window = false;
    if (surface == nullptr) {
        if (created_null_surface_window) {
            std::fprintf(stderr,
                         "stud: ANativeWindow_fromSurface(null) called again, refusing to "
                         "create a second window\n");
            std::fflush(stderr);
            return nullptr;
        }
        created_null_surface_window = true;
    }

    // Hard cap, by construction rather than by correct logic.
    //
    // Stud maps exactly one real window. Every mechanism that is supposed
    // to guarantee that, the jobject cache, the null-Surface guard
    // above, is logic that can be wrong, and when it was wrong the
    // result was over a hundred real windows on the user's desktop
    // before anyone could react. A runaway window loop is not something
    // to detect politely and continue through: it is unusable, and every
    // window past the first is already a bug.
    //
    // STUD_MAX_WINDOWS raises the cap for anything that legitimately
    // needs more than one; it is not a workaround for the loop above.
    static int windows_created = 0;
    static const int max_windows = [] {
        const char* v = std::getenv("STUD_MAX_WINDOWS");
        const int n = v != nullptr ? std::atoi(v) : 0;
        return n > 0 ? n : 1;
    }();
    if (windows_created >= max_windows) {
        std::fprintf(stderr,
                     "stud: refusing to create window #%d (cap %d), something is looping. "
                     "Set STUD_MAX_WINDOWS to raise the cap if this is genuinely needed.\n",
                     windows_created + 1, max_windows);
        std::fflush(stderr);
        return nullptr;
    }
    ++windows_created;

    auto* window = new ANativeWindow();

    // X11 takes the whole of the rest of this function: it has no
    // compositor, no xdg_surface roles and no fractional-scale protocol,
    // so none of the Wayland setup below applies to it. The window it
    // maps is reported through the same ANativeWindow the engine already
    // holds, and through native_window_x11_display()/_window() for EGL.
    if (stud::android_glue::display_backend() == stud::android_glue::DisplayBackend::X11) {
        const int32_t w = g_default_logical_width.load();
        const int32_t h = g_default_logical_height.load();
        // The scale FIRST, because an X window is created in device
        // pixels: a 1382-logical window on a 1.25x desktop is 1728 of
        // them, and a Wayland one covers exactly that much screen. Read
        // before the window exists so the engine's very first swapchain
        // is already the right size -- it asks once, and a chain it
        // created at the wrong size is one the upscaler can never be
        // given anything to do with.
        const int32_t scale = stud::android_glue::x11::display_scale_120();
        if (scale > 0) g_display_scale_120.store(scale);
        g_render_scale_120.store(effective_scale_120());
        // Nearest both ways, so that turning the device size back into a
        // logical one (native_window_pump_x11) lands on the number it
        // started from. Rounding up here and up again there differed by a
        // pixel, and a pixel is a swapchain rebuild.
        const int32_t device_w =
            static_cast<int32_t>((static_cast<int64_t>(w) * g_display_scale_120.load() +
                                  kScaleUnit / 2) / kScaleUnit);
        const int32_t device_h =
            static_cast<int32_t>((static_cast<int64_t>(h) * g_display_scale_120.load() +
                                  kScaleUnit / 2) / kScaleUnit);
        if (!stud::android_glue::x11::create_window(device_w, device_h)) {
            delete window;
            return nullptr;
        }
        window->logical_width.store(w);
        window->logical_height.store(h);
        g_logical_width.store(w);
        g_logical_height.store(h);
        // What the engine renders into: the logical size times the render
        // scale, exactly as on Wayland. HiDPI on makes that the device
        // size again; HiDPI off leaves it smaller, and the upscale pass
        // covers the difference.
        g_window_width.store(buffer_px_from_logical(w));
        g_window_height.store(buffer_px_from_logical(h));
        std::printf("stud: android-glue: X11 window %dx%d device px, %dx%d logical, engine "
                    "renders %dx%d\n",
                    device_w, device_h, w, h, g_window_width.load(), g_window_height.load());
        std::fflush(stdout);
        if (surface != nullptr) {
            window->surface_key = surface;
            window_cache()[surface] = window;
        }
        return window;
    }

    ensure_wayland_connection();
    auto& state = wayland_state();
    if (state.compositor != nullptr) {
        window->surface = wl_compositor_create_surface(state.compositor);

        // Ask for the exact scale before the surface is ever mapped, so
        // the first buffer is already the right size.
        if (state.fractional_scale_manager != nullptr && state.viewporter != nullptr) {
            window->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
                state.fractional_scale_manager, window->surface);
            wp_fractional_scale_v1_add_listener(window->fractional_scale,
                                                 &kFractionalScaleListener, window);
            window->viewport = wp_viewporter_get_viewport(state.viewporter, window->surface);
        }
        window->logical_width.store(g_default_logical_width.load());
        window->logical_height.store(g_default_logical_height.load());

        if (state.wm_base != nullptr) {
            window->shell_surface = xdg_wm_base_get_xdg_surface(state.wm_base, window->surface);
            xdg_surface_add_listener(window->shell_surface, &kShellSurfaceListener, window);
            window->toplevel = xdg_surface_get_toplevel(window->shell_surface);
            xdg_toplevel_add_listener(window->toplevel, &kToplevelListener, window);
            xdg_toplevel_set_title(window->toplevel, "Stud");
            // Must match StartupWMClass in packaging/stud.desktop: on Wayland
            // this app_id is the only thing a compositor can use to tie this
            // window back to a desktop entry, and therefore to an icon.
            xdg_toplevel_set_app_id(window->toplevel, STUD_APP_ID);
            std::printf("stud: android-glue: xdg_toplevel app_id=\"%s\" title=\"Stud\"\n",
                         STUD_APP_ID);
            std::fflush(stdout);
            // Explicit request for a server-side (compositor-drawn)
            // titlebar (the engineering notes, real user report: "must have
            // titlebar"). Stud draws no client-side decorations of its
            // own, so without this the compositor is free to leave the
            // window fully undecorated (observed on this real
            // compositor). Honest degradation if the global isn't
            // advertised, same pattern as every other optional Wayland
            // feature here.
            if (state.decoration_manager != nullptr) {
                window->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
                    state.decoration_manager, window->toplevel);
                zxdg_toplevel_decoration_v1_set_mode(
                    window->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
            }
            // Triggers the compositor's first xdg_surface.configure,
            // handled by xdg_surface_configure() above, which acks it
            // and commits again, completing the real map sequence.
            wl_surface_commit(window->surface);
            wl_display_roundtrip_queue(state.display, state.queue);
        }
    }
    if (surface != nullptr) {
        window->surface_key = surface;
        window_cache()[surface] = window;
    }
    // The overlay parents itself to the game window, so it needs the one
    // surface the engine actually renders into.
    g_primary_surface.store(window->surface);
    return window;
}

int32_t ANativeWindow_getWidth(ANativeWindow* /*window*/) { return g_window_width.load(); }

int32_t ANativeWindow_getHeight(ANativeWindow* /*window*/) { return g_window_height.load(); }

void ANativeWindow_acquire(ANativeWindow* window) { window->ref_count.fetch_add(1); }

void ANativeWindow_release(ANativeWindow* window) {
    if (window->ref_count.fetch_sub(1) == 1) {
        if (window->surface_key != nullptr) {
            window_cache().erase(window->surface_key);
        }
        if (window->decoration != nullptr) {
            zxdg_toplevel_decoration_v1_destroy(window->decoration);
        }
        if (window->toplevel != nullptr) {
            xdg_toplevel_destroy(window->toplevel);
        }
        if (window->shell_surface != nullptr) {
            xdg_surface_destroy(window->shell_surface);
        }
        if (window->surface != nullptr) {
            wl_surface_destroy(window->surface);
        }
        delete window;
    }
}

}  // extern "C"

namespace stud::android_glue {

DisplayBackend display_backend() { return display_backend_impl(); }

void* native_window_x11_display() {
    return display_backend() == DisplayBackend::X11 ? x11::display() : nullptr;
}

void* native_window_x11_vk_display() {
    return display_backend() == DisplayBackend::X11 ? x11::vk_display() : nullptr;
}

void x11_ensure_mapped() {
    if (display_backend() != DisplayBackend::X11) return;
    x11::ensure_mapped();
}

unsigned long native_window_x11_window() {
    return display_backend() == DisplayBackend::X11 ? x11::window() : 0;
}

bool native_window_is_visible() {
    if (display_backend() == DisplayBackend::X11) return x11::visible();
    return g_window_visible.load();
}

bool native_window_is_foreground() {
    if (display_backend() == DisplayBackend::X11) return x11::visible() && x11::focused();
    return g_window_visible.load() && g_window_activated.load();
}

int native_window_x11_fd() {
    return display_backend() == DisplayBackend::X11 ? x11::connection_fd() : -1;
}

::wl_display* native_window_wl_display(::ANativeWindow* window) {
    if (window == nullptr || window->surface == nullptr) {
        return nullptr;
    }
    return wayland_state().display;
}

void display_output_geometry(int32_t* px_w, int32_t* px_h, int32_t* mm_w, int32_t* mm_h) {
    if (display_backend() == DisplayBackend::X11) {
        int32_t w = 0, h = 0, mw = 0, mh = 0;
        x11::output_geometry(w, h, mw, mh);
        if (px_w != nullptr) *px_w = w;
        if (px_h != nullptr) *px_h = h;
        if (mm_w != nullptr) *mm_w = mw;
        if (mm_h != nullptr) *mm_h = mh;
        return;
    }
    ensure_wayland_connection();
    const auto& state = wayland_state();
    // A rotated output reports its mode in the panel's own orientation
    // while geometry's millimetres describe the physical panel, so for
    // a 90/270 transform the two disagree about which axis is which.
    // Swap the pixel axes to match, which keeps dots-per-inch correct on
    // a rotated monitor instead of transposing it.
    const bool rotated = state.output_transform == WL_OUTPUT_TRANSFORM_90 ||
                         state.output_transform == WL_OUTPUT_TRANSFORM_270 ||
                         state.output_transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
                         state.output_transform == WL_OUTPUT_TRANSFORM_FLIPPED_270;
    if (px_w != nullptr) *px_w = rotated ? state.output_mode_px_h : state.output_mode_px_w;
    if (px_h != nullptr) *px_h = rotated ? state.output_mode_px_w : state.output_mode_px_h;
    if (mm_w != nullptr) *mm_w = state.output_phys_mm_w;
    if (mm_h != nullptr) *mm_h = state.output_phys_mm_h;
}

int32_t display_refresh_mhz() { return wayland_state().output_refresh_mhz; }

std::vector<int32_t> display_supported_refresh_mhz() {
    return wayland_state().output_supported_refresh_mhz;
}

void set_render_scale_120(int32_t requested_scale_120) {
    ensure_wayland_connection();
    g_requested_render_scale_120.store(requested_scale_120 > 0 ? requested_scale_120 : 0);
    // Learn the display's real scale BEFORE sizing anything. The default
    // window size is worked out just below, in logical units, and
    // converting the output's physical mode into logical units needs the
    // scale, without it the desktop looked like 1920x1080 logical
    // instead of 1536x864, so Stud asked for a 1728x972 window on an
    // 864-tall desktop and the compositor immediately configured it back
    // down. That configure is a resize, and resizes are what made the DPI
    // appear to change by itself.
    native_window_wait_for_display_scale_120();
    g_render_scale_120.store(effective_scale_120());
    // Pick the real default window size now that both the scale and the
    // output's own size are known, in LOGICAL units, which is what a
    // window is measured in.
    //
    // Always derived from the actual desktop. It used to keep a fixed
    // 1280x720 unless the computed size was LARGER, which meant the one
    // case the computation exists for, a display too small for
    // 1280x720, was the one case it did not cover, and Stud opened a
    // window bigger than the screen. The floor is now a genuinely small
    // window rather than a guess at a common desktop.
    const int32_t scale_120 = display_scale_120();
    int32_t logical_w = kDefaultLogicalWidth;
    int32_t logical_h = kDefaultLogicalHeight;
    const auto& st = wayland_state();
    if (st.output_mode_px_w > 0 && st.output_mode_px_h > 0 && scale_120 > 0) {
        const int32_t out_logical_w = st.output_mode_px_w * kScaleUnit / scale_120;
        const int32_t out_logical_h = st.output_mode_px_h * kScaleUnit / scale_120;
        // Nine tenths of the desktop, which is what other clients open at:
        // clearly a window rather than a fullscreen surface, and it never
        // needs the compositor to configure it smaller on the first frame.
        logical_w = out_logical_w * 9 / 10;
        logical_h = out_logical_h * 9 / 10;
        if (logical_w < kMinLogicalWidth) logical_w = kMinLogicalWidth;
        if (logical_h < kMinLogicalHeight) logical_h = kMinLogicalHeight;
        // Never larger than the desktop itself, however small that is.
        if (logical_w > out_logical_w) logical_w = out_logical_w;
        if (logical_h > out_logical_h) logical_h = out_logical_h;
    }
    g_default_logical_width.store(logical_w);
    g_default_logical_height.store(logical_h);
    g_window_width.store(buffer_px_from_logical(logical_w));
    g_window_height.store(buffer_px_from_logical(logical_h));
    std::printf("stud: android-glue: default window %dx%d logical (output %dx%d px)\n", logical_w,
                logical_h, st.output_mode_px_w, st.output_mode_px_h);
    std::printf("stud: android-glue: display scale %d/120, render scale %d/120 (%s)\n",
                display_scale_120(), g_render_scale_120.load(),
                g_requested_render_scale_120.load() > 0 ? "set in Settings"
                                                         : "following the display");
    std::fflush(stdout);
}

// The DISPLAY's scale, which is what Android's DisplayMetrics.density
// means. Not the buffer scale: those diverge whenever HiDPI rendering is
// off, and reporting the buffer's told the engine this was a 1.0 display
// on a 1.25 desktop.
int32_t native_window_buffer_scale_120() { return display_scale_120(); }

// Pairs the output with xdg-output once both globals have arrived, and
// waits for its logical_size. Safe to call repeatedly. Defined here
// rather than beside the other helpers because it needs the listener,
// which is declared with the rest of the registry handling below them.
static void ensure_xdg_output() {
    auto& state = wayland_state();
    if (state.xdg_output != nullptr) return;
    if (state.xdg_output_manager == nullptr || state.output == nullptr) return;
    state.xdg_output =
        zxdg_output_manager_v1_get_xdg_output(state.xdg_output_manager, state.output);
    if (state.xdg_output == nullptr) return;
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(state.xdg_output), state.queue);
    zxdg_output_v1_add_listener(state.xdg_output, &kXdgOutputListener, &state);
    for (int attempt = 0; attempt < 4 && state.output_logical_w == 0; ++attempt) {
        if (wl_display_roundtrip_queue(state.display, state.queue) < 0) break;
    }
}

namespace {
std::string g_pending_activation_token;

void activation_token_done(void*, xdg_activation_token_v1*, const char* token) {
    if (token != nullptr) g_pending_activation_token = token;
}
const xdg_activation_token_v1_listener kActivationTokenListener = {
    .done = activation_token_done,
};
}  // namespace

void native_window_set_pointer_locked(ANativeWindow* window, bool locked) {
    if (display_backend() == DisplayBackend::X11) {
        x11::set_pointer_locked(locked);
        return;
    }
    auto& state = wayland_state();
    if (locked == g_pointer_locked.load()) return;
    if (locked && state.confined_pointer != nullptr) {
        // Make room: one constraint per surface, and the lock is the
        // stronger of the two.
        zwp_confined_pointer_v1_destroy(state.confined_pointer);
        state.confined_pointer = nullptr;
        g_pointer_confined.store(false);
    }
    if (locked) {
        if (state.pointer_constraints == nullptr || state.pointer == nullptr || window == nullptr ||
            window->surface == nullptr) {
            return;
        }
        // PERSISTENT, not ONESHOT: mouse look lasts as long as the button is
        // held, and a oneshot lock ends itself the first time the pointer
        // would have left the surface, which under a lock is immediately
        // the point of the exercise.
        state.locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
            state.pointer_constraints, window->surface, state.pointer, nullptr,
            ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        if (state.locked_pointer == nullptr) return;
        // No set_cursor_position_hint: the compositor puts the cursor back
        // where it was locked when the lock is released, which is exactly
        // the behaviour a camera drag should have.
        g_pointer_locked.store(true);
    } else {
        if (state.locked_pointer != nullptr) {
            // Deliberately no set_cursor_position_hint. The compositor
            // does not move the pointer while it is locked, so when the
            // lock ends it is still where the drag began, which is where
            // the engine's own pinned cursor is. Placing it anywhere else
            // is a guess about engine state this process does not have.
            zwp_locked_pointer_v1_destroy(state.locked_pointer);
            state.locked_pointer = nullptr;
        }
        g_pointer_locked.store(false);
        // Whatever wanted the pointer confined still wants it.
        if (g_confine_wanted.load() && state.pointer_constraints != nullptr &&
            state.pointer != nullptr && window != nullptr && window->surface != nullptr) {
            state.confined_pointer = zwp_pointer_constraints_v1_confine_pointer(
                state.pointer_constraints, window->surface, state.pointer, nullptr,
                ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
            g_pointer_confined.store(state.confined_pointer != nullptr);
        }
    }
    if (state.display != nullptr) wl_display_flush(state.display);
}

void native_window_warp_pointer(ANativeWindow* window, float x, float y) {
    if (display_backend() == DisplayBackend::X11) {
        x11::warp_pointer(static_cast<int>(x), static_cast<int>(y));
        return;
    }
    auto& state = wayland_state();
    if (state.pointer_warp == nullptr || state.pointer == nullptr || window == nullptr ||
        window->surface == nullptr) {
        return;
    }
    // The compositor honours this while the surface has pointer focus,
    // "including when it has an implicit pointer grab", which is
    // exactly the case that matters here, a button held down mid-drag.
    // It rejects a position outside the surface, so clamp rather than
    // hand it something it will throw away.
    const float w = static_cast<float>(g_window_width.load());
    const float h = static_cast<float>(g_window_height.load());
    const float cx = w > 0.0f ? std::min(std::max(x, 0.0f), w - 1.0f) : x;
    const float cy = h > 0.0f ? std::min(std::max(y, 0.0f), h - 1.0f) : y;
    // Surface-local, not buffer pixels; see unscale_pointer_coord().
    wp_pointer_warp_v1_warp_pointer(state.pointer_warp, window->surface, state.pointer,
                                     wl_fixed_from_double(unscale_pointer_coord(cx)),
                                     wl_fixed_from_double(unscale_pointer_coord(cy)),
                                     g_pointer_enter_serial.load());
    if (state.display != nullptr) wl_display_flush(state.display);
    g_pointer_x = cx;
    g_pointer_y = cy;
}

bool native_window_can_warp_pointer() {
    if (display_backend() == DisplayBackend::X11) return true;
    return wayland_state().pointer_warp != nullptr;
}

void native_window_set_pointer_confined(ANativeWindow* window, bool confined) {
    if (display_backend() == DisplayBackend::X11) {
        // X11 confines with the grab it already takes for mouse look,
        // XGrabPointer's confine_to is this same window.
        x11::set_pointer_confined(confined);
        g_pointer_confined.store(confined);
        return;
    }
    auto& state = wayland_state();
    g_confine_wanted.store(confined);
    if (confined == g_pointer_confined.load()) return;
    if (confined) {
        if (state.pointer_constraints == nullptr || state.pointer == nullptr || window == nullptr ||
            window->surface == nullptr) {
            return;
        }
        // Never alongside a lock: that is the protocol error that kills
        // the client. The want is recorded above and honoured when the
        // lock ends.
        if (g_pointer_locked.load()) return;
        // No region: the whole surface. PERSISTENT, because a camera drag
        // lasts as long as the button is held and a oneshot confinement
        // ends itself the first time the pointer reaches the boundary,
        // which is exactly when it is needed.
        state.confined_pointer = zwp_pointer_constraints_v1_confine_pointer(
            state.pointer_constraints, window->surface, state.pointer, nullptr,
            ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        if (state.confined_pointer == nullptr) return;
        g_pointer_confined.store(true);
    } else {
        if (state.confined_pointer != nullptr) {
            zwp_confined_pointer_v1_destroy(state.confined_pointer);
            state.confined_pointer = nullptr;
        }
        g_pointer_confined.store(false);
    }
    if (state.display != nullptr) wl_display_flush(state.display);
}

void native_window_activate(ANativeWindow* window, const char* token) {
    // The other direction: a token MINTED by whoever launched us, spent
    // here to raise this window.
    //
    // Wayland gives a client no way to raise itself; that is the whole
    // point of the protocol, so the only thing that can bring Stud
    // forward is a token from the process the user actually acted in. A
    // browser click travels as XDG_ACTIVATION_TOKEN to the second
    // stud-ui, which hands it here along with the link.
    //
    // Without a token the window simply stays where it is. That is the
    // honest outcome and not a failure: the compositor is refusing focus
    // theft, which is exactly what it should do for an app raising itself
    // off a timer.
    if (token == nullptr || *token == '\0') return;
    auto& state = wayland_state();
    if (state.activation == nullptr || window == nullptr || window->surface == nullptr) return;
    xdg_activation_v1_activate(state.activation, token, window->surface);
    if (state.display != nullptr) wl_display_flush(state.display);
}

std::string native_window_activation_token(ANativeWindow* window) {
    auto& state = wayland_state();
    if (state.activation == nullptr || state.display == nullptr || state.queue == nullptr) {
        return {};
    }
    g_pending_activation_token.clear();
    xdg_activation_token_v1* tok = xdg_activation_v1_get_activation_token(state.activation);
    if (tok == nullptr) return {};
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(tok), state.queue);
    xdg_activation_token_v1_add_listener(tok, &kActivationTokenListener, nullptr);
    // The serial is what makes the token count. A compositor treats
    // activation as a transfer of focus the user asked for, so it wants the
    // input event that asked: xdg_activation_v1 says the serial "should" be
    // set, and KWin reads it as "must", a token minted without one is
    // issued, handed over, and then quietly ignored as focus stealing. That
    // is the whole reason links opened behind Stud, and passing the serial
    // of the click that hit the link is both what fixes it and what is
    // actually true about the launch.
    const uint32_t serial = g_last_input_serial.load();
    if (state.seat != nullptr && serial != 0) {
        xdg_activation_token_v1_set_serial(tok, serial, state.seat);
    }
    // Tie the token to this surface, so the compositor knows the launch
    // came from a window the user is actually interacting with.
    if (window != nullptr && window->surface != nullptr) {
        xdg_activation_token_v1_set_surface(tok, window->surface);
    }
    xdg_activation_token_v1_commit(tok);
    for (int i = 0; i < 8 && g_pending_activation_token.empty(); ++i) {
        if (wl_display_roundtrip_queue(state.display, state.queue) < 0) break;
    }
    xdg_activation_token_v1_destroy(tok);
    return g_pending_activation_token;
}

// X11 pointer coordinates are DEVICE pixels; the engine's are buffer
// pixels. They were the same number until the X11 backend learned the
// logical/buffer split, and then every pointer position was out by the
// display's scale -- the cursor appearing somewhere other than where the
// mouse was, further off the further from the window's corner.
//
// Wayland needs no equivalent: its pointer arrives in surface-logical
// units, which scale_pointer_coord() already converts.
float native_window_pointer_px_from_device(float device_px) {
    const int32_t display = display_scale_120();
    if (display <= 0) return device_px;
    return static_cast<float>(static_cast<double>(device_px) *
                              static_cast<double>(g_render_scale_120.load()) /
                              static_cast<double>(display));
}

float native_window_device_px_from_pointer(float pointer_px) {
    const int32_t render = g_render_scale_120.load();
    if (render <= 0) return pointer_px;
    return static_cast<float>(static_cast<double>(pointer_px) *
                              static_cast<double>(display_scale_120()) /
                              static_cast<double>(render));
}

void native_window_display_pixel_size(int32_t* width, int32_t* height) {
    // X11 knows this exactly, so it is not recomputed.
    //
    // Deriving it from the logical size rounds twice -- device to logical
    // on the way in, logical back to device here -- and a 1382px window
    // came back as 1383, which is not the window. The swapchain was then
    // built one pixel wider than the thing it presents to and every
    // present returned OUT_OF_DATE.
    if (display_backend() == DisplayBackend::X11) {
        if (width != nullptr) *width = x11::width();
        if (height != nullptr) *height = x11::height();
        return;
    }
    // From the window's LOGICAL size and the display's own scale, not from
    // the buffer: the buffer is whatever the engine renders at, which is
    // exactly what this is not.
    const int32_t scale = display_scale_120();
    const int32_t logical_w = g_logical_width.load();
    const int32_t logical_h = g_logical_height.load();
    if (logical_w <= 0 || logical_h <= 0) {
        if (width != nullptr) *width = 0;
        if (height != nullptr) *height = 0;
        return;
    }
    if (width != nullptr) {
        *width = static_cast<int32_t>((static_cast<int64_t>(logical_w) * scale + kScaleUnit - 1) /
                                      kScaleUnit);
    }
    if (height != nullptr) {
        *height = static_cast<int32_t>((static_cast<int64_t>(logical_h) * scale + kScaleUnit - 1) /
                                      kScaleUnit);
    }
}

int32_t native_window_wait_for_display_scale_120() {
    if (display_backend() == DisplayBackend::X11) {
        // X11 has no fractional-scale protocol. Xft.dpi is what a
        // desktop's scale setting writes and what every toolkit reads,
        // so it is the same measurement by a different route.
        const int32_t scale = x11::display_scale_120();
        if (g_display_scale_120.load() == kScaleUnit && scale != kScaleUnit) {
            g_display_scale_120.store(scale);
            g_render_scale_120.store(effective_scale_120());
            std::printf("stud: android-glue: display scale %d/120 from Xft.dpi\n", scale);
            std::fflush(stdout);
        }
        return display_scale_120();
    }
    auto& state = wayland_state();
    if (g_display_scale_120.load() != kScaleUnit) return display_scale_120();

    // Ask xdg-output for the desktop's logical size and derive the scale
    // from it: physical mode pixels / logical size is exactly the factor,
    // and both are known before any surface exists.
    ensure_xdg_output();
    if (state.output_logical_w > 0 && state.output_mode_px_w > 0) {
        const int32_t derived =
            state.output_mode_px_w * kScaleUnit / state.output_logical_w;
        if (derived > 0) {
            g_display_scale_120.store(derived);
            std::printf("stud: android-glue: display scale %d/120 from xdg-output "
                        "(%dx%d px over %dx%d logical)\n",
                        derived, state.output_mode_px_w, state.output_mode_px_h,
                        state.output_logical_w, state.output_logical_h);
            std::fflush(stdout);
            return display_scale_120();
        }
    }
    if (state.display == nullptr || state.queue == nullptr || state.compositor == nullptr ||
        state.fractional_scale_manager == nullptr) {
        return display_scale_120();
    }

    // Ask the compositor with a throwaway surface.
    //
    // The scale is wanted ONCE, before the engine starts, so that every
    // number derived from it is decided together and none of them can
    // change later. But the real window does not exist yet at that point
    // it is created when the engine asks for a surface, and a
    // fractional scale is a property the compositor reports PER SURFACE.
    // With nothing to report about, the answer was the integer
    // wl_output.scale, which is 2 on a 1.25x desktop.
    //
    // This surface is never given a role and never mapped, so nothing
    // appears on screen; it exists only to be something the compositor
    // can answer about. Destroyed immediately either way.
    wl_surface* probe = wl_compositor_create_surface(state.compositor);
    if (probe == nullptr) return display_scale_120();
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(probe), state.queue);
    wp_fractional_scale_v1* probe_scale =
        wp_fractional_scale_manager_v1_get_fractional_scale(state.fractional_scale_manager, probe);
    if (probe_scale != nullptr) {
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(probe_scale), state.queue);
        wp_fractional_scale_v1_add_listener(probe_scale, &kFractionalScaleListener, nullptr);
        wl_surface_commit(probe);
        // Bounded: a compositor that answers nothing must not hang bring-up.
        for (int attempt = 0; attempt < 8 && g_display_scale_120.load() == kScaleUnit; ++attempt) {
            if (wl_display_roundtrip_queue(state.display, state.queue) < 0) break;
        }
        wp_fractional_scale_v1_destroy(probe_scale);
    }
    wl_surface_destroy(probe);
    const int32_t answer = display_scale_120();
    std::printf("stud: android-glue: display scale probed: %d/120%s\n", answer,
                answer == kScaleUnit ? " (compositor reported none, treating as 1.0)" : "");
    std::fflush(stdout);
    return answer;
}

// Tells the compositor what logical size this surface's buffers stand
// for. The EGL path has always done this as part of creating its
// wl_egl_window; Vulkan attaches its own buffers and never goes through
// that, so it has to be done explicitly or the surface is left in
// whatever state the xdg handshake put it in.
::wl_event_queue* native_window_wl_queue() { return wayland_state().queue; }

// Marks the whole surface opaque.
//
// Vulkan's swapchain images on this driver are AR24, an alpha format,
// even when the swapchain asks for VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
// (live-confirmed: compositeAlpha=0x1, and the buffer still arrives as
// DRM format 875709016/AR24 where a working reference used
// 875713112/XR24). compositeAlpha describes how the SURFACE is composited,
// not what the buffer's alpha channel contains, so a compositor is within
// its rights to honour those alpha bits. Whatever the engine leaves in
// them then decides the window's transparency, and a window composited
// fully transparent is indistinguishable from a black one.
//
// wl_surface.set_opaque_region is the protocol's own answer: it tells the
// compositor to ignore alpha for this area entirely. The GL path never
// needed it because EGL picks an opaque visual; Vulkan does not.
void native_window_set_opaque(::ANativeWindow* window) {
    if (window == nullptr || window->surface == nullptr) return;
    auto& state = wayland_state();
    if (state.compositor == nullptr) return;
    wl_region* region = wl_compositor_create_region(state.compositor);
    if (region == nullptr) return;
    // The region is in surface-local (logical) coordinates, the same space
    // the viewport destination uses.
    wl_region_add(region, 0, 0, window->logical_width.load(), window->logical_height.load());
    wl_surface_set_opaque_region(window->surface, region);
    wl_region_destroy(region);
}

void native_window_apply_surface_scale(::ANativeWindow* window) {
    if (window == nullptr || window->surface == nullptr) return;
    if (window->viewport != nullptr) {
        // Viewport path: the destination carries the logical size, and
        // buffer_scale must stay 1 (the two are mutually exclusive by
        // protocol).
        wp_viewport_set_destination(window->viewport, window->logical_width.load(),
                                     window->logical_height.load());
        return;
    }
    const int32_t integer_scale = g_render_scale_120.load() / kScaleUnit;
    if (integer_scale > 1) {
        wl_surface_set_buffer_scale(window->surface, integer_scale);
    }
}

::wl_egl_window* native_window_get_or_create_egl_window(::ANativeWindow* window, int32_t width,
                                                          int32_t height) {
    if (window == nullptr) {
        return nullptr;
    }
    if (window->egl_window != nullptr) {
        return window->egl_window;
    }
    if (window->surface == nullptr) {
        return nullptr;
    }
    // `width`/`height` already arrive in buffer pixels: the caller reads
    // them from ANativeWindow_getWidth/getHeight, which this module keeps
    // in buffer space. What the compositor still needs told is what
    // logical size that buffer represents, otherwise it takes the
    // buffer's own pixel count as the window size and the window comes
    // out the wrong size on screen.
    if (window->viewport != nullptr) {
        // Viewport path: the destination carries the logical size, and
        // buffer_scale must stay 1 (the two mechanisms are mutually
        // exclusive by protocol).
        wp_viewport_set_destination(window->viewport, window->logical_width.load(),
                                     window->logical_height.load());
    } else {
        // No fractional-scale/viewporter on this compositor: fall back to
        // integer buffer scale, which is all wl_output.scale can express.
        const int32_t integer_scale = g_render_scale_120.load() / kScaleUnit;
        if (integer_scale > 1) {
            wl_surface_set_buffer_scale(window->surface, integer_scale);
        }
    }
    window->egl_window = wl_egl_window_create(window->surface, width, height);
    return window->egl_window;
}

::wl_surface* native_window_wl_surface(::ANativeWindow* window) {
    if (window == nullptr) {
        return nullptr;
    }
    return window->surface;
}

WaylandOverlayDeps overlay_deps() {
    const auto& state = wayland_state();
    WaylandOverlayDeps d;
    d.display = state.display;
    d.compositor = state.compositor;
    d.subcompositor = state.subcompositor;
    d.shm = state.shm;
    d.viewporter = state.viewporter;
    d.parent = g_primary_surface.load();
    d.scale_120 = g_render_scale_120.load();
    d.seat = state.seat;
    d.data_device_manager = state.data_device_manager;
    return d;
}

uint32_t last_input_serial() { return g_last_input_serial.load(); }

bool window_close_requested() {
    // X11 reports the window manager's close request through its own
    // event queue, not through the Wayland flag.
    if (display_backend() == DisplayBackend::X11) return x11::close_requested();
    return g_window_close_requested.load();
}

// Drains the X server's event queue, when that is the backend in use.
//
// Called from render-host's own display pump, beside the Wayland one,
// a resize arrives here as ConfigureNotify and has to reach the size
// ANativeWindow_getWidth/getHeight report, which is what the engine, the
// swapchain and DisplayMetrics all read.
void native_window_pump_x11_events_only() {
    if (display_backend() != DisplayBackend::X11) return;
    x11::pump();
}

void native_window_pump_x11() {
    if (display_backend() != DisplayBackend::X11) return;
    x11::pump();
    // The X window's size is DEVICE pixels; everything above this line
    // thinks in the Wayland quantities, so they are reconstructed here:
    //
    //   device  = what X reports, the pixels the screen really has
    //   logical = device / display scale, the size a Wayland compositor
    //             would have called the window
    //   buffer  = logical * render scale, what the engine renders into
    //             and what ANativeWindow_getWidth reports
    //
    // With HiDPI on the render scale IS the display scale, so the buffer
    // comes back to the device size and nothing has changed. With it off
    // the buffer is the logical size, which is smaller, and the upscale
    // pass writes the device size, which is what the compositor does for
    // itself on Wayland.
    const int32_t device_w = x11::width();
    const int32_t device_h = x11::height();
    if (device_w <= 0 || device_h <= 0) return;
    const int32_t scale = display_scale_120();
    // NEAREST, not ceiling. Rounding up here disagreed with the rounding
    // up that produced the device size in the first place, so a window
    // created at 1382 logical came back as 1383 on the very first pump
    // and the swapchain was rebuilt for a pixel that had not moved.
    const int32_t logical_w = static_cast<int32_t>(
        (static_cast<int64_t>(device_w) * kScaleUnit + scale / 2) / scale);
    const int32_t logical_h = static_cast<int32_t>(
        (static_cast<int64_t>(device_h) * kScaleUnit + scale / 2) / scale);
    g_logical_width.store(logical_w);
    g_logical_height.store(logical_h);
    // A floor on what the engine is ever told.
    //
    // Shrinking a window far enough, or minimising it, can leave the
    // server reporting a handful of pixels, and every one of those is a
    // swapchain the driver may refuse and a render target the engine
    // rebuilds for nothing. 64 is below any usable window and above every
    // driver's minimum.
    constexpr int32_t kMinBufferPx = 64;
    int32_t buffer_w = buffer_px_from_logical(logical_w);
    int32_t buffer_h = buffer_px_from_logical(logical_h);
    // Never larger than the window itself.
    //
    // The logical size is the device size divided by the display scale and
    // rounded, and the buffer size multiplies it back up, rounding up --
    // so the round trip can land a pixel PAST where it started: a 971-px
    // window gives 777 logical, and 777 back up gives 972.
    //
    // On Wayland that overshoot is invisible, because the viewport scales
    // whatever the buffer is to the window's logical size. X11 has no
    // viewport: the buffer IS the window, so one pixel of overshoot is a
    // swapchain that does not match the window it presents to, with
    // nothing to cover the difference -- live-caught as a black screen
    // with HiDPI on, the engine rendering 1728x972 into a 1728x971
    // window.
    //
    // At the display's own scale the answer is not a rounded division at
    // all: the buffer is exactly the pixels the window has.
    if (effective_scale_120() == display_scale_120()) {
        buffer_w = device_w;
        buffer_h = device_h;
    } else {
        if (buffer_w > device_w) buffer_w = device_w;
        if (buffer_h > device_h) buffer_h = device_h;
    }
    if (buffer_w < kMinBufferPx) buffer_w = kMinBufferPx;
    if (buffer_h < kMinBufferPx) buffer_h = kMinBufferPx;
    if (buffer_w != g_window_width.load() || buffer_h != g_window_height.load()) {
        g_window_width.store(buffer_w);
        g_window_height.store(buffer_h);
        std::printf("stud: android-glue: X11 window %dx%d device px, %dx%d logical, engine "
                    "renders %dx%d\n",
                    device_w, device_h, logical_w, logical_h, buffer_w, buffer_h);
        std::fflush(stdout);
    }
}

void native_window_pump_key_repeat() {
    auto& r = key_repeat();
    if (!r.active.load()) return;
    const int32_t rate = r.rate_per_sec.load();
    if (rate <= 0) return;
    uint32_t key = 0;
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        if (!r.active.load()) return;
        const auto now = std::chrono::steady_clock::now();
        if (now < r.next) return;
        key = r.key.load();
        // Advance from the deadline, not from now, so the repeat rate
        // does not drift with however often this is called.
        r.next += std::chrono::microseconds(1000000 / rate);
        if (r.next < now) r.next = now + std::chrono::microseconds(1000000 / rate);
    }
    HostInputEvent ev;
    ev.type = HostInputEvent::kKey;
    ev.code = key;
    ev.a = 1.0f;
    // Marks this as a repeat rather than a fresh press, matching what a
    // real device reports through KeyEvent.getRepeatCount().
    ev.b = 1.0f;
    resolve_key_from_keymap(key, /*pressed=*/true, &ev);
    push_input_event(ev);
}

size_t native_window_drain_input_events(HostInputEvent* out, size_t max) {
    if (out == nullptr || max == 0) return 0;
    std::lock_guard<std::mutex> lock(input_queue_mutex());
    auto& q = input_queue();
    size_t n = q.size() < max ? q.size() : max;
    for (size_t i = 0; i < n; ++i) {
        out[i] = q.front();
        q.pop_front();
    }
    return n;
}
}  // namespace stud::android_glue
