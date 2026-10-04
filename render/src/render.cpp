#include "stud/render.h"

#include <dlfcn.h>

#include <string>

namespace stud::render {

namespace {
void* g_egl_handle = nullptr;
void* g_gles_handle = nullptr;
}  // namespace

void open_gl_libraries(const std::string& egl_path, const std::string& gles_path) {
    g_egl_handle = ::dlopen(egl_path.c_str(), RTLD_NOW);
    if (g_egl_handle == nullptr) {
        throw LoadError("stud::render: failed to open '" + egl_path + "': " + ::dlerror());
    }
    g_gles_handle = ::dlopen(gles_path.c_str(), RTLD_NOW);
    if (g_gles_handle == nullptr) {
        throw LoadError("stud::render: failed to open '" + gles_path + "': " + ::dlerror());
    }
}

void use_system_gl_libraries() {
    // The system's own EGL and GLES, through glvnd: whichever driver the
    // display and the GPU selection lead to, with nothing translating in
    // between. The versioned names are the ones a runtime (not a
    // development package) installs.
    open_gl_libraries("libEGL.so.1", "libGLESv2.so.2");
}

void* resolve(std::string_view name) {
    if (g_egl_handle == nullptr || g_gles_handle == nullptr) {
        return nullptr;
    }
    const std::string symbol(name);
    void* found = nullptr;
    if (name.rfind("egl", 0) == 0) {
        found = ::dlsym(g_egl_handle, symbol.c_str());
    } else if (name.rfind("gl", 0) == 0) {
        found = ::dlsym(g_gles_handle, symbol.c_str());
    } else {
        return nullptr;
    }
    if (found != nullptr) return found;
    // An extension entry point. A system GLES library exports the core
    // API only; everything else comes from eglGetProcAddress, which is
    // the documented way to reach it.
    using GetProcAddressFn = void* (*)(const char*);
    auto get_proc = reinterpret_cast<GetProcAddressFn>(::dlsym(g_egl_handle, "eglGetProcAddress"));
    return get_proc != nullptr ? get_proc(symbol.c_str()) : nullptr;
}

}  // namespace stud::render
