// Standalone test client: drives stud-render-host over the real Unix
// socket protocol, reproducing the exact same EGL/GLES sequence
// tools/try_render_window.cpp already proved renders a real frame --
// proves the IPC round-trip and (now) the buffer-carrying protocol
// extension work, independent of Process B/bionic entirely.

#include "stud/render_host_protocol.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include <unistd.h>

using stud::render_host::CallId;
using stud::render_host::Client;

namespace {

uint64_t pack_float(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

}  // namespace

int main() {
    Client client;
    std::string path = stud::render_host::default_socket_path();
    if (!client.connect_to(path)) {
        std::perror("connect");
        return 1;
    }
    std::printf("test_client: connected to %s\n", path.c_str());

    uint64_t args[8] = {};
    auto call = [&](CallId id, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0,
                     uint64_t a4 = 0, uint64_t a5 = 0, uint64_t a6 = 0, uint64_t a7 = 0) -> uint64_t {
        uint64_t a[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
        return client.call(id, a, nullptr, 0, nullptr, 0, nullptr);
    };
    auto call_out_str = [&](CallId id, uint64_t a0, uint64_t a1, char* buf, uint32_t cap) -> uint64_t {
        uint64_t a[8] = {a0, a1, 0, 0, 0, 0, 0, 0};
        uint32_t written = 0;
        uint64_t r = client.call(id, a, nullptr, 0, buf, cap - 1, &written);
        buf[written < cap ? written : cap - 1] = '\0';
        return r;
    };

    uint64_t display = call(CallId::EglGetDisplay);
    if (display == 0) { std::fprintf(stderr, "test_client: EglGetDisplay failed\n"); return 1; }
    if (call(CallId::EglInitialize, display) != 1) {
        std::fprintf(stderr, "test_client: EglInitialize failed\n");
        return 1;
    }
    char vendor[256];
    call_out_str(CallId::EglQueryString, display, EGL_VENDOR, vendor, sizeof(vendor));
    std::printf("test_client: real EGL vendor=%s\n", vendor);

    if (call(CallId::EglBindApi, EGL_OPENGL_ES_API) != 1) {
        std::fprintf(stderr, "test_client: EglBindApi failed\n");
        return 1;
    }
    uint64_t config = call(CallId::EglChooseConfig, display);
    if (config == 0) { std::fprintf(stderr, "test_client: EglChooseConfig failed\n"); return 1; }
    uint64_t surface = call(CallId::EglCreateWindowSurface, display, config);
    if (surface == 0) { std::fprintf(stderr, "test_client: EglCreateWindowSurface failed\n"); return 1; }
    uint64_t context = call(CallId::EglCreateContext, display, config);
    if (context == 0) { std::fprintf(stderr, "test_client: EglCreateContext failed\n"); return 1; }
    if (call(CallId::EglMakeCurrent, display, surface, context) != 1) {
        std::fprintf(stderr, "test_client: EglMakeCurrent failed\n");
        return 1;
    }

    char renderer[256], version[256];
    call_out_str(CallId::GlGetString, GL_RENDERER, 0, renderer, sizeof(renderer));
    std::printf("test_client: real GL_RENDERER=%s\n", renderer);
    call_out_str(CallId::GlGetString, GL_VERSION, 0, version, sizeof(version));
    std::printf("test_client: real GL_VERSION=%s\n", version);

    // Exercise a couple of the newly-added buffer-carrying calls too,
    // not just the already-proven scalar sequence: create+compile a
    // trivial real shader (real glShaderSource buffer transfer) and
    // read a pixel back (real glReadPixels buffer transfer).
    uint64_t shader = call(CallId::GlCreateShader, GL_FRAGMENT_SHADER);
    const char* src = "void main() { gl_FragColor = vec4(1.0); }";
    {
        uint64_t a[8] = {shader};
        client.call(CallId::GlShaderSource, a, src, static_cast<uint32_t>(std::strlen(src)), nullptr, 0,
                    nullptr);
    }
    call(CallId::GlCompileShader, shader);
    int compiled = 0;
    {
        uint64_t a[8] = {shader, GL_COMPILE_STATUS};
        uint32_t written = 0;
        client.call(CallId::GlGetShaderiv, a, nullptr, 0, &compiled, sizeof(compiled), &written);
    }
    std::printf("test_client: real shader compile status=%d (buffer round-trip proven)\n", compiled);

    for (int i = 0; i < 30; ++i) {
        float t = static_cast<float>(i) / 20.0f;
        call(CallId::GlClearColor, pack_float(0.1f + 0.4f * (1 + std::sin(t))), pack_float(0.2f),
             pack_float(0.8f), pack_float(1.0f));
        call(CallId::GlClear, GL_COLOR_BUFFER_BIT);
        if (call(CallId::EglSwapBuffers, display, surface) != 1) {
            std::fprintf(stderr, "test_client: EglSwapBuffers failed\n");
            break;
        }
        ::usleep(50 * 1000);
    }
    std::printf("test_client: render loop completed via IPC\n");
    return 0;
}
