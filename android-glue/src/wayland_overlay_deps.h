#pragma once

#include <wayland-client.h>

#include <cstdint>

#include "viewporter-client-protocol.h"

// The handful of Wayland objects the text overlay needs from
// native_window.cpp, which owns the connection and the game window.
// Internal to android-glue -- the public overlay API (stud/text_overlay.h)
// deliberately exposes no Wayland types.
namespace stud::android_glue {

struct WaylandOverlayDeps {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    wl_shm* shm = nullptr;
    wp_viewporter* viewporter = nullptr;
    // The game window's own surface: the overlay is a subsurface of it,
    // so it moves, clips and stacks with the window for free.
    wl_surface* parent = nullptr;
    // Buffer pixels per 120 logical units, the same figure the window
    // itself is sized with.
    int32_t scale_120 = 120;
    // Clipboard needs both: a data device is per-seat, and taking the
    // selection has to be justified by a real input serial.
    wl_seat* seat = nullptr;
    wl_data_device_manager* data_device_manager = nullptr;
};

// Serial of the most recent real input event from the compositor.
uint32_t last_input_serial();

WaylandOverlayDeps overlay_deps();

}  // namespace stud::android_glue
