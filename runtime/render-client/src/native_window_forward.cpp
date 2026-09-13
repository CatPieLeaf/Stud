// Real, confirmed-live gap (this session): libroblox.so's own dynamic
// symbol table directly references ANativeWindow_fromSurface/getWidth/
// getHeight/acquire/release (an eager/data-bound import, same class as
// AMediaFormat_delete) -- these must be real, exported libandroid.so
// symbols, not just entries in android-glue's old resolver table.
// Process C owns the real ANativeWindow implementation entirely (see
// android-glue/src/native_window.cpp, deliberately excluded from this
// bionic build for the same glibc-only-Wayland reason ANGLE itself is)
// -- forwards over the same proven IPC mechanism as the GL/EGL stubs.

#include "render_client_common.h"

#include "stud/ndk_types.h"

extern "C" {

ANativeWindow* ANativeWindow_fromSurface(JNIEnv*, jobject) {
    uint64_t a[8] = {};
    uint64_t h = stud::render_client::connection().call(stud::render_host::CallId::ANativeWindowFromSurface,
                                                          a, nullptr, 0, nullptr, 0, nullptr);
    return reinterpret_cast<ANativeWindow*>(static_cast<uintptr_t>(h));
}

int32_t ANativeWindow_getWidth(ANativeWindow*) {
    uint64_t a[8] = {};
    return static_cast<int32_t>(
        stud::render_client::connection().call(stud::render_host::CallId::ANativeWindowGetWidth, a,
                                                 nullptr, 0, nullptr, 0, nullptr));
}

int32_t ANativeWindow_getHeight(ANativeWindow*) {
    uint64_t a[8] = {};
    return static_cast<int32_t>(
        stud::render_client::connection().call(stud::render_host::CallId::ANativeWindowGetHeight, a,
                                                 nullptr, 0, nullptr, 0, nullptr));
}

void ANativeWindow_acquire(ANativeWindow*) {
    uint64_t a[8] = {};
    stud::render_client::connection().call(stud::render_host::CallId::ANativeWindowAcquire, a, nullptr, 0,
                                            nullptr, 0, nullptr);
}

void ANativeWindow_release(ANativeWindow*) {
    uint64_t a[8] = {};
    stud::render_client::connection().call(stud::render_host::CallId::ANativeWindowRelease, a, nullptr, 0,
                                            nullptr, 0, nullptr);
}

}  // extern "C"
