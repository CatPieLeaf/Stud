#pragma once

#include <cstdint>

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

int32_t width();
int32_t height();

}  // namespace stud::android_glue::x11
