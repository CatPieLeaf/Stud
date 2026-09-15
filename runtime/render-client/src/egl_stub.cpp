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
#include <utility>

using stud::render_host::CallId;
using stud::render_client::connection;

namespace {

// Opaque EGLDisplay/EGLSurface/EGLContext/EGLConfig handles Process B
// holds are just the render-host protocol's own small integer IDs,
// smuggled through EGL's own opaque pointer-sized handle types (real
// EGL never dereferences these itself, only ever passes them back to
// EGL/GLES calls, exactly this stub's own real usage).
template <typename T>
T from_handle(uint64_t h) { return reinterpret_cast<T>(static_cast<uintptr_t>(h)); }
uint64_t to_handle(const void* p) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)); }

}  // namespace

extern "C" {

EGLDisplay eglGetDisplay(EGLNativeDisplayType) {
    uint64_t a[8] = {};
    uint64_t h = connection().call(CallId::EglGetDisplay, a, nullptr, 0, nullptr, 0, nullptr);
    return h == 0 ? EGL_NO_DISPLAY : from_handle<EGLDisplay>(h);
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor) {
    uint64_t a[8] = {to_handle(dpy)};
    EGLBoolean ok = connection().call(CallId::EglInitialize, a, nullptr, 0, nullptr, 0, nullptr) ? EGL_TRUE : EGL_FALSE;
    // Real ANGLE version. Roblox's own code only ever logs this, never
    // branches on the exact value (confirmed nothing in the real GLES2/
    // EGL1.x core API path depends on minor-version-specific behavior);
    // 1.5 matches the real backing ANGLE build.
    if (major != nullptr) *major = 1;
    if (minor != nullptr) *minor = 5;
    return ok;
}

EGLBoolean eglBindAPI(EGLenum api) {
    uint64_t a[8] = {static_cast<uint64_t>(api)};
    return connection().call(CallId::EglBindApi, a, nullptr, 0, nullptr, 0, nullptr) ? EGL_TRUE : EGL_FALSE;
}

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint*, EGLConfig* configs, EGLint config_size,
                            EGLint* num_config) {
    if (config_size < 1) {
        if (num_config != nullptr) *num_config = 0;
        return EGL_TRUE;
    }
    uint64_t a[8] = {to_handle(dpy)};
    uint64_t h = connection().call(CallId::EglChooseConfig, a, nullptr, 0, nullptr, 0, nullptr);
    if (h == 0) {
        if (num_config != nullptr) *num_config = 0;
        return EGL_FALSE;
    }
    configs[0] = from_handle<EGLConfig>(h);
    if (num_config != nullptr) *num_config = 1;
    return EGL_TRUE;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType,
                                   const EGLint*) {
    uint64_t a[8] = {to_handle(dpy), to_handle(config)};
    uint64_t h = connection().call(CallId::EglCreateWindowSurface, a, nullptr, 0, nullptr, 0, nullptr);
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

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext, const EGLint*) {
    uint64_t a[8] = {to_handle(dpy), to_handle(config)};
    uint64_t h = connection().call(CallId::EglCreateContext, a, nullptr, 0, nullptr, 0, nullptr);
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
    // see glGetError() in gles_stub.cpp.
    //
    // Resolved by name rather than linked: the GL and EGL stubs are
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

const char* eglQueryString(EGLDisplay dpy, EGLint name) {
    static thread_local char buf[256];
    uint64_t a[8] = {to_handle(dpy), static_cast<uint64_t>(name)};
    uint32_t written = 0;
    connection().call(CallId::EglQueryString, a, nullptr, 0, buf, sizeof(buf) - 1, &written);
    buf[written < sizeof(buf) ? written : sizeof(buf) - 1] = '\0';
    return buf;
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

// Honest limitation: nothing beyond the core EGL1.x/GLES2 API this
// stub already implements has been confirmed needed (Roblox importing
// this symbol doesn't by itself prove it queries any specific extension
// at runtime), returns nullptr for anything not explicitly known,
// same "grow against real evidence, don't guess ahead" discipline this
// whole project already follows elsewhere, rather than building a
// speculative generic-dispatch trampoline pool for an unconfirmed need.
// Live-root-caused fix (the engineering notes, "what drains the
// engine's task queue"): this used to unconditionally return nullptr.
// That is not a harmless stub. Real GL code resolves its entry points
// once, up front, into its own dispatch table, and libroblox does
// exactly that. Every slot it filled this way was NULL, and the first
// call through one (an indirect call through that table, an
// ordinary GL call immediately followed by glGetError()) jumped to
// address 0. That fault landed on the very thread the engine had
// designated as its own internal "main" thread; Stud's own near_null
// recovery then pthread_exit()s that thread, and since it is the only
// thing that ever drains the engine's internal task queue, every later
// engine call queued work for a dead thread and blocked forever.
//
// Resolving against everything already loaded in this process is the
// honest answer: Stud's own libEGL.so/libGLESv2.so really do export the
// entry points they implement (each forwarding over the render-host
// IPC), so a function Stud supports resolves to Stud's own real
// implementation, and one it genuinely does not support still returns
// null: but now that is a real, specific, reportable gap rather than
// a blanket "nothing exists".
}  // extern "C"

namespace {

// Real GL code resolves its entry points once, up front, and then calls
// them without ever null-checking, so handing back a null for a
// function Stud doesn't implement is a deferred jump to address zero,
// not a graceful degradation. Returning a *named no-op* instead keeps
// the calling thread alive (which matters enormously here: the thread
// doing this resolution is the one the engine designates as its own
// internal task-queue "main" thread, and killing it wedges the whole
// process; see the engineering notes) and turns an unimplemented call into
// a precise, actionable log line naming the real function.
//
// Each slot needs its OWN function pointer so the handler can tell which
// real GL function was called. A template instantiated over a compile-
// time index gives exactly that: one real, distinct function per slot,
// no runtime code generation.
constexpr int kMaxUnimplemented = 256;
const char* g_unimplemented_names[kMaxUnimplemented];
int g_unimplemented_count = 0;

// Returns 0 in %rax, which is a safe value whether the real function's
// prototype returns void, an integer or a pointer.
template <int Index>
uintptr_t unimplemented_gl_thunk() {
    static bool reported = false;
    if (!reported) {
        reported = true;  // once per function, not once per call; these can be hot
        std::fprintf(stderr, "stud: CALLED unimplemented GL function \"%s\" (returning 0)\n",
                     g_unimplemented_names[Index]);
        std::fflush(stderr);
    }
    return 0;
}

using ThunkFn = uintptr_t (*)();

template <int... Is>
constexpr auto make_thunk_table(std::integer_sequence<int, Is...>) {
    return std::array<ThunkFn, sizeof...(Is)>{{&unimplemented_gl_thunk<Is>...}};
}

const auto g_thunks = make_thunk_table(std::make_integer_sequence<int, kMaxUnimplemented>{});

// Stable copy of the name, the caller's string is not guaranteed to
// outlive this call.
const char* intern_name(const char* procname) {
    return ::strdup(procname);
}

}  // namespace

extern "C" {

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* procname) {
    if (procname == nullptr) {
        return nullptr;
    }
    void* sym = ::dlsym(RTLD_DEFAULT, procname);
    if (sym != nullptr) {
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(sym);
    }

    // Genuinely missing. Hand back a named no-op rather than null.
    if (g_unimplemented_count < kMaxUnimplemented) {
        int index = g_unimplemented_count++;
        g_unimplemented_names[index] = intern_name(procname);
        std::fprintf(stderr,
                     "stud: eglGetProcAddress(\"%s\") -> not implemented by Stud, returning a "
                     "no-op stub\n",
                     procname);
        std::fflush(stderr);
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(g_thunks[index]);
    }

    std::fprintf(stderr,
                 "stud: eglGetProcAddress(\"%s\") -> NULL (no-op stub table full, %d entries)\n",
                 procname, kMaxUnimplemented);
    std::fflush(stderr);
    return nullptr;
}

}  // extern "C"
