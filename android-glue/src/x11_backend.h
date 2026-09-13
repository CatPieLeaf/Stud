#pragma once

#include <cstdint>
#include <string>

// Stud's X11 window, for sessions with no Wayland compositor.
//
// Wayland is still the primary backend and is untouched by this: which
// one runs is decided once, at startup, by what the session actually
// offers (see display_backend() in native_window.cpp). Everything here
// is a no-op on a Wayland session.
//
// Xlib is loaded with dlopen rather than linked, so a machine with no
// libX11 at all still runs Stud on Wayland -- the same treatment ANGLE,
// the Vulkan loader and PortAudio already get. The headers are used for
// types only; nothing here is resolved at link time.
namespace stud::android_glue::x11 {

// Whether an X server is reachable: DISPLAY is set, libX11 loads, and
// XOpenDisplay succeeds. Answered once and remembered.
bool available();

// Creates and maps the one window, at the given logical size. Returns
// false if anything failed, having reported why.
bool create_window(int32_t width, int32_t height);

// The real X display and window, for EGL and for Vulkan's WSI. Null/0
// before create_window() succeeds.
void* display();
unsigned long window();

// Drains everything the server has sent: resizes update the size below,
// and a WM close request sets close_requested(). Never blocks.
void pump();

bool close_requested();

// Whether anybody can currently see the window: false when it is
// unmapped or fully obscured.
bool visible();

// Whether this is the focused window.
bool focused();

// The X connection's socket, to poll alongside the render socket.
// -1 before the display is open.
int connection_fd();

// Mouse look. X11 has no pointer-constraints protocol, so the lock is
// the classic pointer grab plus a warp back to the centre after every
// motion -- the pointer never reaches an edge and the deltas keep
// coming. Reports relative motion instead of positions while held, the
// same as the Wayland path.
void set_pointer_locked(bool locked);

// The system clipboard, X11's way: a selection is owned by a window, and
// the owner hands the bytes over on request. Both calls are no-ops (and
// an empty string) when this backend is not the active one.
void clipboard_set(const std::string& text);
std::string clipboard_get();

// The text overlay's presentation half. The drawing is shared with the
// Wayland path (text_overlay.cpp); only the surface differs, and on X11
// that is an ARGB child window blitted with XPutImage.
// Maps the window, if it is not mapped yet. Deferred until the first
// frame is presented so the window appears WITH content, the way a
// Wayland surface does -- a Wayland window does not exist until a buffer
// is committed to it, while X11 would happily show an empty one for the
// whole of the engine's bring-up.
void ensure_mapped();

void present_text_overlay(const void* argb, int width, int height, int x, int y);
void hide_text_overlay();

// The desktop's scale, in 120ths, from X11's own answer: the Xft.dpi
// resource against a 96-dpi baseline. X11 has no fractional-scale
// protocol -- Xft.dpi is what every toolkit reads and what a desktop's
// own scale setting writes -- so this is the equivalent measurement, not
// a guess. Returns 120 when nothing says otherwise.
int32_t display_scale_120();

// The display itself: size in pixels and in millimetres, which is what
// real dots-per-inch is computed from. Returns false when the display
// cannot be asked.
bool output_geometry(int32_t& px_w, int32_t& px_h, int32_t& mm_w, int32_t& mm_h);

int32_t width();
int32_t height();

}  // namespace stud::android_glue::x11
