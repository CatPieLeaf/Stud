// M6 test: proves stud::render's dispatch mechanism (egl*/gl* prefix
// routing to two separately dlopen'd libraries) against portable, locally
// built fixtures, not a real ANGLE build, since depending on one being
// present at a fixed system path would break on any clean machine/CI. Real
// validation against a genuine ANGLE build (e.g. a system's bundled
// Chromium/CEF copy) is done manually via tools/try_load.cpp's optional
// ANGLE-path arguments, not part of this automated suite. See
// the engineering notes, milestone M6.

#include "stud/render.h"

#include <cstdio>
#include <cstdlib>

namespace {

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
    std::printf("ok: %s\n", what);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <path-to-toy_egl_lib.so> <path-to-toy_gles_lib.so>\n", argv[0]);
        return 1;
    }

    check(stud::render::resolve("eglGetError") == nullptr,
          "resolve() returns nullptr before set_angle_library_paths() is called");

    stud::render::set_angle_library_paths(argv[1], argv[2]);

    auto* egl_fn = reinterpret_cast<int (*)()>(stud::render::resolve("eglGetError"));
    check(egl_fn != nullptr, "resolve(\"eglGetError\") is non-null after opening the EGL fixture");
    check(egl_fn() == 0x3000, "resolved eglGetError() returns the real EGL_SUCCESS value");

    auto* egl_marker = reinterpret_cast<int (*)()>(stud::render::resolve("eglStudToyMarker"));
    check(egl_marker != nullptr && egl_marker() == 111,
          "an egl-prefixed name correctly dispatches to the EGL library, not the GLES one");

    auto* gles_fn = reinterpret_cast<unsigned int (*)()>(stud::render::resolve("glGetError"));
    check(gles_fn != nullptr, "resolve(\"glGetError\") is non-null after opening the GLES fixture");
    check(gles_fn() == 0, "resolved glGetError() returns the real GL_NO_ERROR value");

    auto* gles_marker = reinterpret_cast<int (*)()>(stud::render::resolve("glStudToyMarker"));
    check(gles_marker != nullptr && gles_marker() == 222,
          "a gl-prefixed name correctly dispatches to the GLES library, not the EGL one");

    check(stud::render::resolve("strlen") == nullptr,
          "a non-egl/gl-prefixed name correctly falls outside this module's scope");

    bool threw = false;
    try {
        stud::render::set_angle_library_paths("/does/not/exist.so", argv[2]);
    } catch (const stud::render::LoadError&) {
        threw = true;
    }
    check(threw, "set_angle_library_paths() throws LoadError for a nonexistent library, doesn't "
                 "silently leave stale state");

    std::printf("all render dispatch checks passed\n");
    return 0;
}
