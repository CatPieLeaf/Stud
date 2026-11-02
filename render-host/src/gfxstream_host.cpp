#include "gfxstream_host.h"

#ifdef STUD_HAVE_GFXSTREAM

#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#include "gfxstream/host/features.h"
#include "render-utils/RenderChannel.h"
#include "render-utils/RenderLib.h"
#include "render-utils/Renderer.h"
#include "render-utils/render_api.h"

// The embedder functions Stud's gfxstream patch adds (see
// cmake/gfxstream/0001-stud-egl-on-egl.patch).
extern "C" {
void gfxstream_set_native_display(unsigned int platform, void* native_display);
void* gfxstream_create_shared_context();
unsigned int gfxstream_color_buffer_texture(uint32_t handle, uint32_t* width, uint32_t* height);
void gfxstream_set_unit_texture_override(unsigned int unit, unsigned int texture);
void gfxstream_set_cube_unit_mirror(unsigned int unit, unsigned int source);
void gfxstream_set_clip_control_zero_to_one(bool enable);
void gfxstream_asg_layout(uint64_t* ring_storage_size, uint64_t* buffer_size);
void gfxstream_asg_set_renderer(void* renderer);
void* gfxstream_asg_create(void* memory, uint64_t size, uint32_t puid);
void gfxstream_asg_notify(void* context);
void gfxstream_asg_destroy(void* context);
void gfxstream_wait_frame(uint32_t color_buffer);
void gfxstream_frame_shown(uint32_t color_buffer);
}

namespace stud::render_host::gfxstream_host {
namespace {

using gfxstream::RenderChannel;
using gfxstream::RendererPtr;

gfxstream::RenderLibPtr g_lib;
RendererPtr g_renderer;
void* g_shared_context = nullptr;

bool send_all(int fd, const void* data, size_t len) {
    const auto* p = static_cast<const char*>(data);
    while (len > 0) {
        const ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

// Socket to channel: everything the guest encoders write.
void pump_in(int fd, const std::shared_ptr<RenderChannel>& channel) {
    constexpr size_t kChunk = 64 * 1024;
    for (;;) {
        // Received into the buffer the channel keeps, not copied into it.
        RenderChannel::Buffer b;
        b.resize_noinit(kChunk);
        const ssize_t n = ::recv(fd, b.data(), kChunk, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        b.resize_noinit(static_cast<size_t>(n));
        for (;;) {
            const auto r = channel->tryWrite(std::move(b));
            if (r == RenderChannel::IoResult::Ok) break;
            if (r == RenderChannel::IoResult::Error) return;
            channel->waitUntilWritable();
        }
    }
    channel->stop();
}

// Channel to socket: the decoder's replies.
void pump_out(int fd, std::shared_ptr<RenderChannel> channel) {
    for (;;) {
        RenderChannel::Buffer b;
        const auto r = channel->tryRead(&b);
        if (r == RenderChannel::IoResult::Ok) {
            if (!send_all(fd, b.data(), b.size())) break;
            continue;
        }
        if (r == RenderChannel::IoResult::Error) break;
        channel->waitUntilReadable();
    }
    ::shutdown(fd, SHUT_RDWR);
}

// A render channel on gfxstream's ring transport (one per GL thread): the
// guest's commands go through memory both sides map, read by gfxstream's
// render thread where the guest writes them, with no copy and no thread in
// between. The guest sends its process id; this side answers with the
// memory (an fd) and its layout, then rings gfxstream's doorbell for every
// byte the guest sends, which it does only while that render thread sleeps.
void serve_ring(int fd) {
    uint64_t puid = 0;
    if (::recv(fd, &puid, sizeof(puid), MSG_WAITALL) != static_cast<ssize_t>(sizeof(puid))) {
        return;
    }
    uint64_t layout[2] = {};  // ring storage size, write buffer size
    gfxstream_asg_layout(&layout[0], &layout[1]);
    const size_t size = static_cast<size_t>(layout[0] + layout[1]);
    const int mem = static_cast<int>(::syscall(SYS_memfd_create, "stud-gfxstream-ring", MFD_CLOEXEC));
    if (mem < 0 || ::ftruncate(mem, static_cast<off_t>(size)) != 0) {
        if (mem >= 0) ::close(mem);
        return;
    }
    void* memory = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, mem, 0);
    if (memory == MAP_FAILED) {
        ::close(mem);
        return;
    }
    // Started, and its ring set up, before the guest sees the memory.
    void* context = gfxstream_asg_create(memory, size, static_cast<uint32_t>(puid));

    iovec iov{layout, sizeof(layout)};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    cmsghdr* cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cm), &mem, sizeof(int));
    const bool sent = ::sendmsg(fd, &msg, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(layout));
    ::close(mem);
    if (sent) {
        char doorbell[64];
        for (;;) {
            const ssize_t n = ::recv(fd, doorbell, sizeof(doorbell), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            gfxstream_asg_notify(context);
        }
    }
    gfxstream_asg_destroy(context);
    ::munmap(memory, size);
}

// One guest connection. Its first byte says what it is: 'P' the process
// handshake (a fresh process id, the connection held open for the process's
// life), 'R' a render channel over this socket, 'A' one on the ring
// transport (both one per GL thread).
void serve(int fd) {
    static std::atomic<uint64_t> next_puid{1};
    char tag = 0;
    if (::recv(fd, &tag, 1, MSG_WAITALL) != 1) {
        ::close(fd);
        return;
    }
    if (tag == 'P') {
        const uint64_t puid = next_puid.fetch_add(1);
        g_renderer->onGuestGraphicsProcessCreate(puid);
        send_all(fd, &puid, sizeof(puid));
        char ignored;
        while (::recv(fd, &ignored, 1, 0) > 0) {
        }
        g_renderer->cleanupProcGLObjects(puid);
        ::close(fd);
        return;
    }
    if (tag == 'A') {
        serve_ring(fd);
        ::close(fd);
        return;
    }
    if (tag != 'R') {
        ::close(fd);
        return;
    }
    std::shared_ptr<RenderChannel> channel = g_renderer->createRenderChannel(nullptr, 0);
    if (!channel) {
        std::fprintf(stderr, "stud-render-host: gfxstream: no render channel\n");
        ::close(fd);
        return;
    }
    std::thread out(pump_out, fd, channel);
    pump_in(fd, channel);
    out.join();
    ::close(fd);
}

}  // namespace

bool available() { return true; }

bool start(unsigned int egl_platform, void* native_display, const std::string& socket_path) {
    // GLES on the system's own EGL and GLES driver, on render-host's
    // display, with no window of gfxstream's own.
    ::setenv("ANDROID_EGL_ON_EGL", "1", 1);
    ::setenv("ANDROID_EMU_HEADLESS", "1", 1);
    gfxstream_set_native_display(egl_platform, native_display);

    gfxstream::host::FeatureSet features;
    GFXSTREAM_SET_BOOL_FEATURE_ON_CONDITION(&features, EglOnEgl, true);
    GFXSTREAM_SET_BOOL_FEATURE_ON_CONDITION(&features, GlesDynamicVersion, true);
    GFXSTREAM_SET_BOOL_FEATURE_ON_CONDITION(&features, HostComposition, true);
    GFXSTREAM_SET_BOOL_FEATURE_ON_CONDITION(&features, GlAsyncSwap, false);
    // Buffer unmaps and make-currents the guest sends without waiting for an
    // answer: otherwise each of the engine's per-frame buffer unmaps is a
    // round trip its render thread waits out.
    GFXSTREAM_SET_BOOL_FEATURE_ON_CONDITION(&features, AsyncComposeSupport, true);
    // Compressed textures (ASTC, ETC2) handed to the driver to decode where it
    // lists every format of the kind. Otherwise gfxstream decodes them on the
    // CPU, and this build has no ASTC decoder: ASTC textures came out empty.
    GFXSTREAM_SET_BOOL_FEATURE_ON_CONDITION(&features, NativeTextureDecompression, true);
    GFXSTREAM_SET_BOOL_FEATURE_ON_CONDITION(&features, GlDirectMem, false);
    GFXSTREAM_SET_BOOL_FEATURE_ON_CONDITION(&features, GlDma, false);
    GFXSTREAM_SET_BOOL_FEATURE_ON_CONDITION(&features, GlPipeChecksum, false);
    GFXSTREAM_SET_BOOL_FEATURE_ON_CONDITION(&features, NoDelayCloseColorBuffer, true);
    GFXSTREAM_SET_BOOL_FEATURE_ON_CONDITION(&features, Vulkan, false);

    g_lib = gfxstream::initLibrary();
    g_lib->setRenderer(SELECTED_RENDERER_HOST);
    // The display size only seeds gfxstream's own (unused) composition
    // target; the guest's buffers take the window's size.
    g_renderer = g_lib->initRenderer(64, 64, features, /*useSubWindow=*/false);
    if (!g_renderer) {
        std::fprintf(stderr, "stud-render-host: gfxstream: the renderer did not start\n");
        return false;
    }
    g_shared_context = gfxstream_create_shared_context();
    gfxstream_asg_set_renderer(g_renderer.get());
    // The engine projects depth to 0..1 (reversed, infinite far), as for
    // Vulkan. GLES maps it to 0.5 + z/2, where a float depth buffer has
    // too little precision far off and surfaces meeting at a seam show
    // through each other as thin lines; 0..1 stores it as Vulkan does.
    gfxstream_set_clip_control_zero_to_one(true);

    const int s = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    ::unlink(socket_path.c_str());
    if (s < 0 || ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(s, 16) != 0) {
        std::fprintf(stderr, "stud-render-host: gfxstream: cannot listen on %s: %s\n",
                     socket_path.c_str(), std::strerror(errno));
        if (s >= 0) ::close(s);
        return false;
    }
    std::thread([s] {
        for (;;) {
            const int fd = ::accept4(s, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EINTR) continue;
                return;
            }
            std::thread(serve, fd).detach();
        }
    }).detach();
    std::printf("stud-render-host: OpenGL through gfxstream, guest connections on %s\n",
                socket_path.c_str());
    std::fflush(stdout);
    return true;
}

void* shared_context() { return g_shared_context; }

unsigned int color_buffer_texture(uint32_t handle, uint32_t* width, uint32_t* height) {
    return gfxstream_color_buffer_texture(handle, width, height);
}

void set_unit_texture_override(unsigned int unit, unsigned int texture) {
    gfxstream_set_unit_texture_override(unit, texture);
}

void set_cube_unit_mirror(unsigned int unit, unsigned int source) {
    gfxstream_set_cube_unit_mirror(unit, source);
}

void wait_frame(uint32_t color_buffer) { gfxstream_wait_frame(color_buffer); }

void frame_shown(uint32_t color_buffer) { gfxstream_frame_shown(color_buffer); }

}  // namespace stud::render_host::gfxstream_host

#else  // !STUD_HAVE_GFXSTREAM

namespace stud::render_host::gfxstream_host {
bool available() { return false; }
bool start(unsigned int, void*, const std::string&) { return false; }
void* shared_context() { return nullptr; }
unsigned int color_buffer_texture(uint32_t, uint32_t*, uint32_t*) { return 0; }
void set_unit_texture_override(unsigned int, unsigned int) {}
void set_cube_unit_mirror(unsigned int, unsigned int) {}
void wait_frame(uint32_t) {}
void frame_shown(uint32_t) {}
}  // namespace stud::render_host::gfxstream_host

#endif
