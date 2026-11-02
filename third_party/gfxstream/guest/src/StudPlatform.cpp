// The platform pieces an emulator image would supply to gfxstream's guest:
// sync-fd handling, gralloc and the ANativeWindow helper.
//
// Sync is complete. Gralloc creates host colour buffers through
// renderControl; buffer allocation and window surfaces answer "not
// available" until the window helper (phase 0.4) is written, which a
// pbuffer-only test does not reach.
#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

#include <system/graphics.h>

#include "HostConnection.h"
#include "StudWindow.h"
#include "Sync.h"
#include "gl2_client_context.h"
#include "gl_client_context.h"
#include "gfxstream/guest/ANativeWindow.h"
#include "gfxstream/guest/GfxStreamGralloc.h"

namespace gfxstream {
namespace {

class StudSync : public SyncHelper {
public:
    // A sync file is readable once signalled.
    int wait(int syncFd, int timeoutMilliseconds) override {
        if (syncFd < 0) return 0;
        pollfd p{syncFd, POLLIN, 0};
        int n;
        do {
            n = ::poll(&p, 1, timeoutMilliseconds);
        } while (n < 0 && errno == EINTR);
        if (n > 0 && (p.revents & POLLIN)) return 0;
        errno = n == 0 ? ETIME : errno;
        return -1;
    }
    void debugPrint(int) override {}
    int dup(int syncFd) override { return ::dup(syncFd); }
    int close(int syncFd) override { return ::close(syncFd); }
};

class StudGralloc : public Gralloc {
public:
    GrallocType getGrallocType() override { return GRALLOC_TYPE_EMULATED; }
    uint32_t createColorBuffer(int width, int height, uint32_t glformat) override {
        HostConnection* conn = HostConnection::get();
        if (conn == nullptr) return 0;
        ExtendedRCEncoderContext* rc = conn->rcEncoder();
        return rc->rcCreateColorBuffer(rc, width, height, glformat);
    }
    void acquire(AHardwareBuffer*) override {}
    void release(AHardwareBuffer*) override {}
    int allocate(uint32_t, uint32_t, uint32_t, uint64_t, AHardwareBuffer**) override {
        return -ENOSYS;
    }
    int lock(AHardwareBuffer*, uint8_t**) override { return -ENOSYS; }
    int lockPlanes(AHardwareBuffer*, std::vector<LockedPlane>*) override { return -ENOSYS; }
    int unlock(AHardwareBuffer*) override { return -ENOSYS; }
    const native_handle_t* getNativeHandle(const AHardwareBuffer*) override { return nullptr; }
    uint32_t getHostHandle(const native_handle_t*) override { return 0; }
    uint32_t getHostHandle(const AHardwareBuffer*) override { return 0; }
    int getFormat(const native_handle_t*) override { return -1; }
    int getFormat(const AHardwareBuffer*) override { return -1; }
    uint32_t getWidth(const AHardwareBuffer*) override { return 0; }
    uint32_t getHeight(const AHardwareBuffer*) override { return 0; }
    size_t getAllocatedSize(const native_handle_t*) override { return 0; }
    size_t getAllocatedSize(const AHardwareBuffer*) override { return 0; }
    int getId(const AHardwareBuffer*, uint64_t*) override { return -ENOSYS; }
};

// A window buffer: one host colour buffer at the window's size.
struct WindowBuffer {
    uint32_t handle = 0;
    int width = 0;
    int height = 0;
};

// Each window's buffers, cycled: the one being drawn and the ones queued
// before it. Enough that the presenter can still be reading one while the
// next is drawn.
constexpr size_t kBuffersPerWindow = 3;
struct WindowBuffers {
    WindowBuffer buffers[kBuffersPerWindow];
    size_t next = 0;
};

std::atomic<StudFramePresenter> g_presenter{nullptr};

// The window's size, from the ANativeWindow functions of whatever
// libandroid the process has (Stud's), looked up once.
using SizeFn = int32_t (*)(void*);
int window_dimension(void* window, const char* fn_name) {
    auto fn = reinterpret_cast<SizeFn>(::dlsym(RTLD_DEFAULT, fn_name));
    return fn != nullptr ? fn(window) : 0;
}

class StudWindowHelper : public ANativeWindowHelper {
public:
    bool isValid(EGLNativeWindowType window) override { return window != nullptr; }
    bool isValid(EGLClientBuffer buffer) override { return buffer != nullptr; }
    void acquire(EGLNativeWindowType) override {}
    void release(EGLNativeWindowType window) override {
        std::lock_guard<std::mutex> lock(m_);
        auto it = windows_.find(window);
        if (it == windows_.end()) return;
        for (WindowBuffer& b : it->second.buffers) close_buffer(b);
        windows_.erase(it);
    }
    void acquire(EGLClientBuffer) override {}
    void release(EGLClientBuffer) override {}
    int getConsumerUsage(EGLNativeWindowType, int* usage) override {
        *usage = 0;
        return 0;
    }
    void setUsage(EGLNativeWindowType, int) override {}
    int getWidth(EGLNativeWindowType window) override {
        return window_dimension(window, "ANativeWindow_getWidth");
    }
    int getHeight(EGLNativeWindowType window) override {
        return window_dimension(window, "ANativeWindow_getHeight");
    }
    int getWidth(EGLClientBuffer buffer) override {
        return static_cast<WindowBuffer*>(buffer)->width;
    }
    int getHeight(EGLClientBuffer buffer) override {
        return static_cast<WindowBuffer*>(buffer)->height;
    }
    int getFormat(EGLClientBuffer, Gralloc*) override { return HAL_PIXEL_FORMAT_RGBA_8888; }
    int getHostHandle(EGLClientBuffer buffer, Gralloc*) override {
        return static_cast<int>(static_cast<WindowBuffer*>(buffer)->handle);
    }
    void setSwapInterval(EGLNativeWindowType, int) override {}
    int queueBuffer(EGLNativeWindowType window, EGLClientBuffer buffer, int fence) override {
        if (fence >= 0) ::close(fence);
        const auto* b = static_cast<WindowBuffer*>(buffer);
        StudFramePresenter present = g_presenter.load();
        // Stud's own, from its libandroid, unless a test set one.
        static const auto stud_present = reinterpret_cast<StudFramePresenter>(
            ::dlsym(RTLD_DEFAULT, "stud_present_color_buffer"));
        if (present == nullptr) present = stud_present;
        static bool told = false;
        if (!told) {
            told = true;
            std::fprintf(stderr, "gfxstream (Stud): first frame queued, colour buffer %u %dx%d, %s\n",
                         b->handle, b->width, b->height,
                         present != nullptr ? "presenting" : "NO PRESENTER");
        }
        // The frame's commands and its flush reach the host first: the
        // presenter waits for that flush before drawing the colour buffer.
        if (HostConnection* conn = HostConnection::get()) conn->flush();
        if (present != nullptr) present(window, b->handle, b->width, b->height);
        return 0;
    }
    int dequeueBuffer(EGLNativeWindowType window, EGLClientBuffer* out, int* fence) override {
        *fence = -1;
        const int width = getWidth(window);
        const int height = getHeight(window);
        if (width <= 0 || height <= 0) return -EINVAL;
        std::lock_guard<std::mutex> lock(m_);
        WindowBuffers& w = windows_[window];
        WindowBuffer& b = w.buffers[w.next];
        w.next = (w.next + 1) % kBuffersPerWindow;
        // A resized window gets buffers of its new size.
        if (b.handle == 0 || b.width != width || b.height != height) {
            close_buffer(b);
            HostConnection* conn = HostConnection::get();
            if (conn == nullptr) return -ENODEV;
            ExtendedRCEncoderContext* rc = conn->rcEncoder();
            b.handle = rc->rcCreateColorBuffer(rc, width, height, 0x1908 /* GL_RGBA */);
            b.width = width;
            b.height = height;
            if (b.handle == 0) return -ENOMEM;
            // The window's own reference, as Android's gralloc holds one: the
            // host drops a buffer the moment the window surface lets go of it
            // and nothing else holds it, which is every swap.
            rc->rcOpenColorBuffer2(rc, b.handle);
        }
        *out = &b;
        return 0;
    }
    int cancelBuffer(EGLNativeWindowType, EGLClientBuffer) override { return 0; }

private:
    static void close_buffer(WindowBuffer& b) {
        if (b.handle == 0) return;
        if (HostConnection* conn = HostConnection::get()) {
            ExtendedRCEncoderContext* rc = conn->rcEncoder();
            rc->rcCloseColorBuffer(rc, b.handle);
        }
        b = WindowBuffer{};
    }

    std::mutex m_;
    std::map<EGLNativeWindowType, WindowBuffers> windows_;
};

}  // namespace

SyncHelper* createPlatformSyncHelper() { return new StudSync(); }
Gralloc* createPlatformGralloc(int32_t) { return new StudGralloc(); }
ANativeWindowHelper* createPlatformANativeWindowHelper() { return new StudWindowHelper(); }

}  // namespace gfxstream

extern "C" void stud_gfxstream_set_presenter(StudFramePresenter presenter) {
    gfxstream::g_presenter.store(presenter);
}

// GLES1's context accessor. HostConnection builds a GLES1 encoder on demand;
// Android defines this in libGLESv1_CM's gl_entry.cpp, which Stud does not
// ship, so nothing ever calls through it.
void gl_client_context_t::setContextAccessor(CONTEXT_ACCESSOR_TYPE*) {}

// GLES2's, for the entry points libGLESv2_enc exports on Android. Here
// gl2.cpp includes those entry points with its own context getter, so the
// accessor has nothing to feed.
void gl2_client_context_t::setContextAccessor(CONTEXT_ACCESSOR_TYPE*) {}

// No virtio-gpu device: the host only offers virtio-gpu native fences over
// virtio-gpu, so egl.cpp's export path that asks for one is never taken.
VirtGpuDevice* VirtGpuDevice::getInstance(enum VirtGpuCapset, int) { return nullptr; }
