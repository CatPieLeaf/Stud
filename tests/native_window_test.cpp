// M6 test: ANativeWindow's real Wayland backing (see
// android-glue/src/native_window.cpp). Portable to a headless/CI
// environment with no compositor, checks honest degradation (null
// surface, no crash) there instead of requiring a live session, but
// prints which case it actually exercised so a real desktop run (this
// development machine has a live Wayland session) is visibly verified,
// not just assumed passing by accident.

#include "stud/android_glue.h"
#include "stud/ndk_types.h"

#include <cstdio>
#include <cstdlib>

namespace {

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
    std::printf("ok: %s\n", what);
}

}  // namespace

int main() {
    ANativeWindow* window = ANativeWindow_fromSurface(nullptr, nullptr);
    check(window != nullptr, "ANativeWindow_fromSurface returns a non-null handle");

    wl_surface* surface = stud::android_glue::native_window_wl_surface(window);
    wl_display* display = stud::android_glue::native_window_wl_display(window);

    if (const char* wayland_display = std::getenv("WAYLAND_DISPLAY")) {
        std::printf("WAYLAND_DISPLAY=%s set, expecting a real compositor connection\n",
                    wayland_display);
        check(surface != nullptr,
              "real Wayland session detected: ANativeWindow got a real wl_surface");
        check(display != nullptr,
              "real Wayland session detected: native_window_wl_display() returns the real "
              "wl_display");
    } else {
        std::printf("WAYLAND_DISPLAY not set, expecting honest degradation, not a crash\n");
        check(surface == nullptr, "no compositor: wl_surface is null, not garbage");
        check(display == nullptr, "no compositor: wl_display is null, not garbage");
    }

    check(stud::android_glue::native_window_wl_surface(nullptr) == nullptr,
          "native_window_wl_surface(nullptr) returns null instead of crashing");
    check(stud::android_glue::native_window_wl_display(nullptr) == nullptr,
          "native_window_wl_display(nullptr) returns null instead of crashing");

    // Standard NDK usage pattern, getWidth/getHeight/acquire/
    // release all still work exactly as before this change.
    check(ANativeWindow_getWidth(window) > 0, "ANativeWindow_getWidth reports a positive value");
    check(ANativeWindow_getHeight(window) > 0, "ANativeWindow_getHeight reports a positive value");
    ANativeWindow_acquire(window);
    ANativeWindow_release(window);
    ANativeWindow_release(window);  // drops the last real ref, frees the window and wl_surface

    std::printf("all native-window checks passed\n");
    return 0;
}
