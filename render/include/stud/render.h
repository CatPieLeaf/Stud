#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

// The system's own EGL and GLES, for the engine's OpenGL path. The engine's
// GLES calls reach the driver directly; this module only opens the two
// libraries and answers `egl*`/`gl*` symbol lookups against them.

namespace stud::render {

class LoadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Opens an EGL and a GLES library, once, before resolve() is used.
// Throws LoadError if either fails to open.
void open_gl_libraries(const std::string& egl_path, const std::string& gles_path);

// open_gl_libraries() on the system's own EGL and GLES (glvnd).
void use_system_gl_libraries();

// Resolves `name` against the opened libraries if it looks like an
// EGL/GLES symbol (egl*/gl* prefix); returns nullptr for anything else or
// if the libraries haven't been opened yet. Suitable to pass directly as
// (or wrap into) a stud::linker::SymbolResolver.
void* resolve(std::string_view name);

}  // namespace stud::render
