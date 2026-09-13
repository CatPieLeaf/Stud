#include "stud/render.h"

#include <cstdlib>
#include <dlfcn.h>

namespace stud::render {

namespace {
void* g_egl_handle = nullptr;
void* g_gles_handle = nullptr;
}  // namespace

void set_angle_library_paths(const std::string& egl_path, const std::string& gles_path) {
    // Real, previously-missing piece of the locked "Vulkan by default on
    // first launch" decision (the engineering notes, "Rendering architecture"):
    // ANGLE does NOT default to its Vulkan backend on this platform --
    // confirmed directly, real testing (tools/try_render_window.cpp):
    // without this env var, ANGLE silently picks a GL-passthrough-shaped
    // backend (GL_RENDERER mentions raw Mesa, no "Vulkan" in the
    // string); with it, GL_RENDERER genuinely reports "ANGLE (..., Vulkan
    // 1.4...)". Matches the exact mechanism the engineering notes' own "Nvidia
    // stability" section already documents from mcpelauncher-client's
    // precedent (`setenv("ANGLE_DEFAULT_PLATFORM", "vulkan", true)`) --
    // never actually wired into Stud's own code until now. Harmless for
    // the Zink dev-backend path too: Zink's own Mesa/EGL stack has no
    // knowledge of this ANGLE-specific variable, so setting it
    // unconditionally here (the one, single dlopen point both backends
    // share) doesn't affect Zink at all, and setting it BEFORE dlopen is
    // safe/early enough since ANGLE only reads it lazily on the first
    // real EGL call, not at library-load time.
    ::setenv("ANGLE_DEFAULT_PLATFORM", "vulkan", 1);

    g_egl_handle = ::dlopen(egl_path.c_str(), RTLD_NOW);
    if (g_egl_handle == nullptr) {
        throw LoadError("stud::render: failed to open EGL library '" + egl_path +
                         "': " + ::dlerror());
    }
    g_gles_handle = ::dlopen(gles_path.c_str(), RTLD_NOW);
    if (g_gles_handle == nullptr) {
        throw LoadError("stud::render: failed to open GLES library '" + gles_path +
                         "': " + ::dlerror());
    }
}

void* resolve(std::string_view name) {
    if (g_egl_handle == nullptr || g_gles_handle == nullptr) {
        return nullptr;
    }
    if (name.rfind("egl", 0) == 0) {
        return ::dlsym(g_egl_handle, std::string(name).c_str());
    }
    if (name.rfind("gl", 0) == 0) {
        return ::dlsym(g_gles_handle, std::string(name).c_str());
    }
    return nullptr;
}

}  // namespace stud::render
