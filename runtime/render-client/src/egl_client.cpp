// Real libEGL.so, bionic-compiled, placed at /system/lib64/libEGL.so,
// exactly the path real bionic's own linker64 (Process B's real
// interpreter) resolves libroblox.so's own "libEGL.so" DT_NEEDED entry
// against (see docs/bionic-process-b.md's confirmed fallback search
// order). Every real exported EGL entry point Roblox's own dynamic
// symbol table imports (confirmed via `the ELF headers --dyn-syms`) forwards,
// over the real Unix-socket protocol proven end-to-end this session, to
// stud-render-host (Process C)'s real ANGLE instance. Roblox never
// knows the difference: same real EGL C ABI, same real symbol names,
// just implemented by forwarding instead of a local dlopen.

#include "render_client_common.h"

#include <EGL/egl.h>

#include <dlfcn.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using stud::render_host::CallId;
using stud::render_client::connection;

namespace {

// Opaque EGLDisplay/EGLSurface/EGLContext/EGLConfig handles Process B
// holds are just the render-host protocol's own small integer IDs,
// smuggled through EGL's own opaque pointer-sized handle types (real
// EGL never dereferences these itself, only ever passes them back to
// EGL/GLES calls, exactly this library's own real usage).
template <typename T>
T from_handle(uint64_t h) { return reinterpret_cast<T>(static_cast<uintptr_t>(h)); }
uint64_t to_handle(const void* p) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)); }

// An EGL attribute list, as the wire carries it: the pairs, without the
// EGL_NONE that ends them.
std::vector<EGLint> attrib_pairs(const EGLint* attribs) {
    std::vector<EGLint> list;
    if (attribs == nullptr) return list;
    for (const EGLint* p = attribs; *p != EGL_NONE; p += 2) {
        list.push_back(p[0]);
        list.push_back(p[1]);
    }
    return list;
}

}  // namespace

extern "C" {

EGLDisplay eglGetDisplay(EGLNativeDisplayType) {
    uint64_t a[8] = {};
    uint64_t h = connection().call(CallId::EglGetDisplay, a, nullptr, 0, nullptr, 0, nullptr);
    return h == 0 ? EGL_NO_DISPLAY : from_handle<EGLDisplay>(h);
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor) {
    // The host's own EGL version. This used to be a fixed 1.5, which was
    // true of the ANGLE build it was written against and of nothing else.
    uint64_t a[8] = {to_handle(dpy)};
    EGLint version[2] = {0, 0};
    uint32_t written = 0;
    const EGLBoolean ok = connection().call(CallId::EglInitialize, a, nullptr, 0, version,
                                            sizeof(version), &written)
                              ? EGL_TRUE
                              : EGL_FALSE;
    if (major != nullptr) *major = written >= sizeof(version) ? version[0] : 0;
    if (minor != nullptr) *minor = written >= sizeof(version) ? version[1] : 0;
    return ok;
}

EGLBoolean eglBindAPI(EGLenum api) {
    uint64_t a[8] = {static_cast<uint64_t>(api)};
    return connection().call(CallId::EglBindApi, a, nullptr, 0, nullptr, 0, nullptr) ? EGL_TRUE : EGL_FALSE;
}

// The engine's own attributes go to the host. They used to be dropped, and
// the host chose a fixed RGBA8 config with only EGL_OPENGL_ES2_BIT and no
// depth or stencil, whatever the engine asked for. A config without
// EGL_OPENGL_ES3_BIT cannot back a GLES 3.1 or 3.2 context, so the engine
// was held at 3.0.
EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint* attrib_list, EGLConfig* configs,
                            EGLint config_size, EGLint* num_config) {
    if (config_size < 1) {
        if (num_config != nullptr) *num_config = 0;
        return EGL_TRUE;
    }
    const std::vector<EGLint> list = attrib_pairs(attrib_list);
    uint64_t a[8] = {to_handle(dpy)};
    uint64_t h = connection().call(CallId::EglChooseConfig, a, list.data(),
                                   static_cast<uint32_t>(list.size() * sizeof(EGLint)), nullptr,
                                   0, nullptr);
    if (h == 0) {
        if (num_config != nullptr) *num_config = 0;
        return EGL_FALSE;
    }
    configs[0] = from_handle<EGLConfig>(h);
    if (num_config != nullptr) *num_config = 1;
    return EGL_TRUE;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType,
                                   const EGLint* attrib_list) {
    const std::vector<EGLint> list = attrib_pairs(attrib_list);
    uint64_t a[8] = {to_handle(dpy), to_handle(config)};
    uint64_t h = connection().call(CallId::EglCreateWindowSurface, a, list.data(),
                                   static_cast<uint32_t>(list.size() * sizeof(EGLint)), nullptr,
                                   0, nullptr);
    return h == 0 ? EGL_NO_SURFACE : from_handle<EGLSurface>(h);
}

EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint* attrib_list) {
    EGLint width = 0, height = 0;
    if (attrib_list != nullptr) {
        for (int i = 0; attrib_list[i] != EGL_NONE; i += 2) {
            if (attrib_list[i] == EGL_WIDTH) width = attrib_list[i + 1];
            if (attrib_list[i] == EGL_HEIGHT) height = attrib_list[i + 1];
        }
    }
    uint64_t a[8] = {to_handle(dpy), to_handle(config), static_cast<uint64_t>(width),
                      static_cast<uint64_t>(height)};
    uint64_t h = connection().call(CallId::EglCreatePbufferSurface, a, nullptr, 0, nullptr, 0, nullptr);
    return h == 0 ? EGL_NO_SURFACE : from_handle<EGLSurface>(h);
}

// The share context and the attribute list both go to the host. Both were
// dropped: every context was created unshared at client version 2, so a
// second context the engine made to share objects with its first shared
// nothing, and the engine saw an older GLES than the driver has.
EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share,
                            const EGLint* attribs) {
    const std::vector<EGLint> list = attrib_pairs(attribs);
    uint64_t a[8] = {to_handle(dpy), to_handle(config), to_handle(share)};
    uint64_t h = connection().call(CallId::EglCreateContext, a, list.data(),
                                   static_cast<uint32_t>(list.size() * sizeof(EGLint)), nullptr,
                                   0, nullptr);
    return h == 0 ? EGL_NO_CONTEXT : from_handle<EGLContext>(h);
}

// Which context this thread has current. Tracked here because the only
// thing that ever makes one current is this client, on this thread, so
// asking the host is a round-trip to be told something already known.
thread_local EGLContext g_current_context = EGL_NO_CONTEXT;

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface, EGLContext ctx) {
    uint64_t a[8] = {to_handle(dpy), to_handle(draw), to_handle(ctx)};
    uint64_t r = connection().call(CallId::EglMakeCurrent, a, nullptr, 0, nullptr, 0, nullptr);
    if (r) g_current_context = ctx;
    return r ? EGL_TRUE : EGL_FALSE;
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    // One real glGetError for the frame that just ended. Every check the
    // engine made during it was answered from the cache this refreshes;
    // see glGetError() in gles_client.cpp.
    //
    // Resolved by name rather than linked: the GL and EGL libraries are
    // separate shared libraries, and a machine where the GL one is not
    // loaded simply has no errors to refresh.
    using RefreshFn = void (*)();
    static RefreshFn refresh = reinterpret_cast<RefreshFn>(
        ::dlsym(RTLD_DEFAULT, "stud_refresh_gl_error_cache"));
    if (refresh != nullptr) refresh();
    uint64_t a[8] = {to_handle(dpy), to_handle(surface)};
    return connection().call(CallId::EglSwapBuffers, a, nullptr, 0, nullptr, 0, nullptr) ? EGL_TRUE : EGL_FALSE;
}

EGLint eglGetError() {
    uint64_t a[8] = {};
    return static_cast<EGLint>(connection().call(CallId::EglGetError, a, nullptr, 0, nullptr, 0, nullptr));
}

// The whole string, kept for the life of the process as EGL requires. This
// was a 256-byte buffer, and EGL_EXTENSIONS is longer than that, so the
// engine was handed a list cut off partway through a name.
const char* eglQueryString(EGLDisplay dpy, EGLint name) {
    static std::mutex m;
    static std::map<std::pair<uint64_t, EGLint>, std::string> strings;
    uint64_t a[8] = {to_handle(dpy), static_cast<uint64_t>(name)};
    std::vector<char> buf(64 * 1024);
    uint32_t written = 0;
    connection().call(CallId::EglQueryString, a, nullptr, 0, buf.data(),
                      static_cast<uint32_t>(buf.size()), &written);
    if (written > buf.size()) written = static_cast<uint32_t>(buf.size());
    std::lock_guard<std::mutex> lock(m);
    // Assigned only when it changes: the engine may still hold the pointer
    // an earlier query returned.
    std::string& s = strings[{to_handle(dpy), name}];
    if (s.size() != written || s.compare(0, written, buf.data(), written) != 0) {
        s.assign(buf.data(), written);
    }
    return s.c_str();
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
    uint64_t a[8] = {to_handle(dpy), to_handle(ctx)};
    return connection().call(CallId::EglDestroyContext, a, nullptr, 0, nullptr, 0, nullptr) ? EGL_TRUE : EGL_FALSE;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
    uint64_t a[8] = {to_handle(dpy), to_handle(surface)};
    return connection().call(CallId::EglDestroySurface, a, nullptr, 0, nullptr, 0, nullptr) ? EGL_TRUE : EGL_FALSE;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint* value) {
    uint64_t a[8] = {to_handle(dpy), to_handle(config), static_cast<uint64_t>(attribute)};
    uint32_t written = 0;
    EGLBoolean ok = connection().call(CallId::EglGetConfigAttrib, a, nullptr, 0, value, sizeof(EGLint),
                                       &written)
                         ? EGL_TRUE
                         : EGL_FALSE;
    return ok;
}

EGLContext eglGetCurrentContext() {
    // Answered from what eglMakeCurrent recorded. The engine asks this
    // every frame and the answer cannot have changed without this client
    // changing it.
    return g_current_context;
}

EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint* value) {
    uint64_t a[8] = {to_handle(dpy), to_handle(surface), static_cast<uint64_t>(attribute)};
    return connection().call(CallId::EglQuerySurface, a, nullptr, 0, value, sizeof(EGLint), nullptr)
               ? EGL_TRUE
               : EGL_FALSE;
}

EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval) {
    uint64_t a[8] = {to_handle(dpy), static_cast<uint64_t>(interval)};
    return connection().call(CallId::EglSwapInterval, a, nullptr, 0, nullptr, 0, nullptr) ? EGL_TRUE : EGL_FALSE;
}

EGLBoolean eglTerminate(EGLDisplay dpy) {
    uint64_t a[8] = {to_handle(dpy)};
    return connection().call(CallId::EglTerminate, a, nullptr, 0, nullptr, 0, nullptr) ? EGL_TRUE : EGL_FALSE;
}

// Everything Stud's own libEGL.so/libGLESv2.so export, each forwarding to
// the host; null, with one line naming it, for anything else.
//
// This used to hand back a named do-nothing function for a name Stud did not
// implement, so that an engine calling through its dispatch table without
// null-checking did not jump to address zero, which had once killed the
// thread that drains its task queue and wedged the whole process. That kept
// it alive at the price of the engine believing it had functions it did not:
// every call to one vanished. Every GL name the engine references is now
// exported (core names and the suffixed spellings its loader falls back to),
// checked against the binary's own strings, so this null is only for a name
// a newer engine adds, and the line below says which one to implement.
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* procname) {
    if (procname == nullptr) {
        return nullptr;
    }
    void* sym = ::dlsym(RTLD_DEFAULT, procname);
    if (sym != nullptr) {
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(sym);
    }
    std::fprintf(stderr, "stud: eglGetProcAddress(\"%s\") -> NULL, not implemented by Stud\n",
                 procname);
    std::fflush(stderr);
    return nullptr;
}

}  // extern "C"
