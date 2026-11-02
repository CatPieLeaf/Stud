// Confirmed-live gap (this session): libroblox.so's own dynamic
// symbol table directly references ANativeWindow_fromSurface/getWidth/
// getHeight/acquire/release (an eager/data-bound import, same class as
// AMediaFormat_delete). These must be real, exported libandroid.so
// symbols, not just entries in android-glue's old resolver table.
// Process C owns the real ANativeWindow implementation entirely (see
// android-glue/src/native_window.cpp, deliberately excluded from this
// bionic build for the same glibc-only reason the GLES driver itself is)
// forwards over the same proven IPC mechanism as the GL/EGL clients.

#include "render_client_common.h"

#include "stud/ndk_types.h"

extern "C" {

ANativeWindow* ANativeWindow_fromSurface(JNIEnv*, jobject) {
    uint64_t a[8] = {};
    uint64_t h = stud::render_client::connection().call(stud::render_host::CallId::ANativeWindowFromSurface,
                                                          a, nullptr, 0, nullptr, 0, nullptr);
    return reinterpret_cast<ANativeWindow*>(static_cast<uintptr_t>(h));
}

// The size render-host has published (the same one the engine is told),
// read from the shared page: gfxstream asks on every frame it dequeues, and
// two round trips to render-host per frame were waits on the engine's
// render thread. Zero until render-host has filled the page.
static uint64_t published_window_size() {
    const std::atomic<uint64_t>* page = stud::render_client::window_state_page();
    if (page == nullptr || page[2].load(std::memory_order_acquire) == 0) return 0;
    return page[0].load(std::memory_order_acquire);
}

int32_t ANativeWindow_getWidth(ANativeWindow*) {
    if (const uint64_t size = published_window_size()) return static_cast<int32_t>(size >> 32);
    uint64_t a[8] = {};
    return static_cast<int32_t>(
        stud::render_client::connection().call(stud::render_host::CallId::ANativeWindowGetWidth, a,
                                                 nullptr, 0, nullptr, 0, nullptr));
}

int32_t ANativeWindow_getHeight(ANativeWindow*) {
    if (const uint64_t size = published_window_size()) {
        return static_cast<int32_t>(size & 0xffffffffu);
    }
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

// OpenGL mode through gfxstream: gfxstream's EGL (its window helper) calls
// this, found by name, with each frame it queues; render-host draws that
// colour buffer into the window. The guest no longer waits for the GPU to
// finish a frame (render-host's presenter waits for it on the GPU), so this
// waits for render-host's answer instead: the engine stays within about a
// frame of what is shown rather than queueing frames ahead of it.
__attribute__((visibility("default"))) void stud_present_color_buffer(void* /*window*/,
                                                                        uint32_t color_buffer,
                                                                        int /*width*/,
                                                                        int /*height*/) {
    uint64_t a[8] = {color_buffer};
    stud::render_client::connection().call(stud::render_host::CallId::GlPresentColorBuffer, a,
                                           nullptr, 0, nullptr, 0, nullptr);
}

}  // extern "C"
