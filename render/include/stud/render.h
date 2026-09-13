#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

// EGL/GLESv2 resolution wired directly to a real ANGLE build -- no custom
// EGL/GLES wrapper code, matching the locked M6 design (see
// the engineering notes, "Rendering architecture"): Roblox's GLES calls go
// straight into ANGLE, which translates to real Vulkan itself. This module
// only needs to `dlopen` ANGLE's own `libEGL.so`/`libGLESv2.so` and answer
// `egl*`/`gl*` symbol lookups against them via `dlsym` -- the actual
// translation work is entirely ANGLE's, not Stud's.
//
// Validated against a real ANGLE build already present on the development
// system (CEF's bundled libEGL.so/libGLESv2.so -- confirmed genuine ANGLE
// via its Vulkan-backend-specific EGL extensions like
// `eglLockVulkanQueueANGLE`, not Mesa's raw GLES). This is a **temporary
// stand-in for validating the wiring approach**, not Stud's real ANGLE
// story -- Stud cannot ship depending on some other installed application's
// files being present. Building/vendoring Stud's own ANGLE copy (Google's
// build system: gclient/depot_tools + GN/Ninja, not CMake, dozens of
// fetched dependencies, hours of build time) remains real, separate,
// substantial work. See the engineering notes, milestone M6.

namespace stud::render {

class LoadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Must be called once before resolve() is used. Opens both libraries
// (RTLD_NOW | RTLD_GLOBAL isn't used -- kept process-local via a private
// handle) and throws LoadError if either fails to open.
void set_angle_library_paths(const std::string& egl_path, const std::string& gles_path);

// Resolves `name` against the opened ANGLE libraries if it looks like an
// EGL/GLES symbol (egl*/gl* prefix); returns nullptr for anything else or
// if the libraries haven't been opened yet. Suitable to pass directly as
// (or wrap into) a stud::linker::SymbolResolver.
void* resolve(std::string_view name);

}  // namespace stud::render
