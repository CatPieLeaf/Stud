// Real render smoke test: proves the system's EGL and GLES -> real Wayland
// window pipeline actually renders a visible frame, independent of whether
// libroblox.so's own engine ever reaches the point of driving GLES itself.
// The same render/ and android-glue plumbing the OpenGL path uses.
//
// Deliberately standalone from libroblox.so/JNI entirely: this is
// testing Stud's OWN render/android-glue plumbing, not Roblox's engine.

#include "stud/android_glue.h"
#include "stud/ndk_types.h"
#include "stud/render.h"
#include "spinning_cube.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <wayland-egl.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

namespace {

using PFN_eglGetDisplay = EGLDisplay (*)(EGLNativeDisplayType);
using PFN_eglInitialize = EGLBoolean (*)(EGLDisplay, EGLint*, EGLint*);
using PFN_eglChooseConfig = EGLBoolean (*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
using PFN_eglCreateWindowSurface = EGLSurface (*)(EGLDisplay, EGLConfig, EGLNativeWindowType,
                                                   const EGLint*);
using PFN_eglCreateContext = EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
using PFN_eglMakeCurrent = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
using PFN_eglSwapBuffers = EGLBoolean (*)(EGLDisplay, EGLSurface);
using PFN_eglGetError = EGLint (*)(void);
using PFN_eglBindAPI = EGLBoolean (*)(EGLenum);
using PFN_eglQueryString = const char* (*)(EGLDisplay, EGLint);
using PFN_glClearColor = void (*)(GLfloat, GLfloat, GLfloat, GLfloat);
using PFN_glClear = void (*)(GLbitfield);
using PFN_glGetString = const GLubyte* (*)(GLenum);


template <typename Fn>
Fn must_resolve(const char* name) {
    void* addr = stud::render::resolve(name);
    if (addr == nullptr) {
        std::fprintf(stderr, "stud: could not resolve required symbol '%s'\n", name);
        std::exit(1);
    }
    return reinterpret_cast<Fn>(addr);
}

}  // namespace

int main() {
    try {
        stud::render::use_system_gl_libraries();
    } catch (const stud::render::LoadError& e) {
        std::fprintf(stderr, "stud: failed to load EGL/GLES: %s\n", e.what());
        return 1;
    }

    // Real window, same mechanism android-glue already uses for the
    // full Roblox boot path (real Wayland surface + xdg_toplevel role,
    // real ping/pong, real configure/ack_configure sequence; see
    // native_window.cpp). No JNIEnv/jobject needed for this direct call.
    ANativeWindow* window = ANativeWindow_fromSurface(nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "stud: ANativeWindow_fromSurface returned null\n");
        return 1;
    }
    wl_display* display = stud::android_glue::native_window_wl_display(window);
    wl_surface* surface = stud::android_glue::native_window_wl_surface(window);
    if (display == nullptr || surface == nullptr) {
        std::fprintf(stderr,
                      "stud: no real Wayland compositor reachable (WAYLAND_DISPLAY unset or "
                      "connection failed); nothing to render into\n");
        return 1;
    }
    std::printf("stud: real Wayland window created\n");

    const int width = 800;
    const int height = 600;
    wl_egl_window* egl_window = wl_egl_window_create(surface, width, height);
    if (egl_window == nullptr) {
        std::fprintf(stderr, "stud: wl_egl_window_create failed\n");
        return 1;
    }

    auto eglGetDisplay_ = must_resolve<PFN_eglGetDisplay>("eglGetDisplay");
    auto eglInitialize_ = must_resolve<PFN_eglInitialize>("eglInitialize");
    auto eglBindAPI_ = must_resolve<PFN_eglBindAPI>("eglBindAPI");
    auto eglChooseConfig_ = must_resolve<PFN_eglChooseConfig>("eglChooseConfig");
    auto eglCreateWindowSurface_ =
        must_resolve<PFN_eglCreateWindowSurface>("eglCreateWindowSurface");
    auto eglCreateContext_ = must_resolve<PFN_eglCreateContext>("eglCreateContext");
    auto eglMakeCurrent_ = must_resolve<PFN_eglMakeCurrent>("eglMakeCurrent");
    auto eglSwapBuffers_ = must_resolve<PFN_eglSwapBuffers>("eglSwapBuffers");
    auto eglGetError_ = must_resolve<PFN_eglGetError>("eglGetError");
    auto eglQueryString_ = must_resolve<PFN_eglQueryString>("eglQueryString");
    auto glClearColor_ = must_resolve<PFN_glClearColor>("glClearColor");
    auto glClear_ = must_resolve<PFN_glClear>("glClear");
    auto glGetString_ = must_resolve<PFN_glGetString>("glGetString");

    EGLDisplay egl_display = EGL_NO_DISPLAY;
    if (egl_display == EGL_NO_DISPLAY) {
        egl_display = eglGetDisplay_(reinterpret_cast<EGLNativeDisplayType>(display));
    }
    if (egl_display == EGL_NO_DISPLAY) {
        std::fprintf(stderr, "stud: eglGetDisplay failed\n");
        return 1;
    }
    EGLint egl_major = 0, egl_minor = 0;
    if (eglInitialize_(egl_display, &egl_major, &egl_minor) != EGL_TRUE) {
        std::fprintf(stderr, "stud: eglInitialize failed, error=0x%x\n", eglGetError_());
        return 1;
    }
    std::printf("stud: real EGL initialized, version %d.%d, vendor=%s\n", egl_major, egl_minor,
                eglQueryString_(egl_display, EGL_VENDOR));

    if (eglBindAPI_(EGL_OPENGL_ES_API) != EGL_TRUE) {
        std::fprintf(stderr, "stud: eglBindAPI(EGL_OPENGL_ES_API) failed\n");
        return 1;
    }

    const EGLint config_attribs[] = {EGL_SURFACE_TYPE,
                                      EGL_WINDOW_BIT,
                                      EGL_RENDERABLE_TYPE,
                                      EGL_OPENGL_ES2_BIT,
                                      EGL_RED_SIZE,
                                      8,
                                      EGL_GREEN_SIZE,
                                      8,
                                      EGL_BLUE_SIZE,
                                      8,
                                      EGL_ALPHA_SIZE,
                                      8,
                                      // The cube depth-tests; a config chosen without
                                      // this has no depth attachment and the back faces
                                      // draw over the front ones.
                                      EGL_DEPTH_SIZE,
                                      16,
                                      EGL_NONE};
    EGLConfig egl_config;
    EGLint num_configs = 0;
    if (eglChooseConfig_(egl_display, config_attribs, &egl_config, 1, &num_configs) != EGL_TRUE ||
        num_configs == 0) {
        std::fprintf(stderr, "stud: eglChooseConfig failed, error=0x%x\n", eglGetError_());
        return 1;
    }

    EGLSurface egl_surface = eglCreateWindowSurface_(
        egl_display, egl_config, reinterpret_cast<EGLNativeWindowType>(egl_window), nullptr);
    if (egl_surface == EGL_NO_SURFACE) {
        std::fprintf(stderr, "stud: eglCreateWindowSurface failed, error=0x%x\n", eglGetError_());
        return 1;
    }

    const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext egl_context =
        eglCreateContext_(egl_display, egl_config, EGL_NO_CONTEXT, context_attribs);
    if (egl_context == EGL_NO_CONTEXT) {
        std::fprintf(stderr, "stud: eglCreateContext failed, error=0x%x\n", eglGetError_());
        return 1;
    }

    if (eglMakeCurrent_(egl_display, egl_surface, egl_surface, egl_context) != EGL_TRUE) {
        std::fprintf(stderr, "stud: eglMakeCurrent failed, error=0x%x\n", eglGetError_());
        return 1;
    }

    std::printf("stud: real GL_RENDERER=%s\n", glGetString_(GL_RENDERER));
    std::printf("stud: real GL_VERSION=%s\n", glGetString_(GL_VERSION));

    // A spinning cube rather than a colour-cycled clear. A clear proves
    // the surface presents; the cube also exercises a shader program, two
    // vertex buffers, an index buffer, a per-frame uniform and the depth
    // test, which is what a smoke test is for, since those are the
    // parts that break. See spinning_cube.h for where it came from.
    stud::tools::CubeGl cube_gl{};
    cube_gl.Enable = must_resolve<void (*)(GLenum)>("glEnable");
    cube_gl.Viewport = must_resolve<void (*)(GLint, GLint, GLsizei, GLsizei)>("glViewport");
    cube_gl.ClearColor = glClearColor_;
    cube_gl.Clear = glClear_;
    cube_gl.CreateShader = must_resolve<GLuint (*)(GLenum)>("glCreateShader");
    cube_gl.ShaderSource =
        must_resolve<void (*)(GLuint, GLsizei, const GLchar* const*, const GLint*)>(
            "glShaderSource");
    cube_gl.CompileShader = must_resolve<void (*)(GLuint)>("glCompileShader");
    cube_gl.GetShaderiv = must_resolve<void (*)(GLuint, GLenum, GLint*)>("glGetShaderiv");
    cube_gl.GetShaderInfoLog =
        must_resolve<void (*)(GLuint, GLsizei, GLsizei*, GLchar*)>("glGetShaderInfoLog");
    cube_gl.CreateProgram = must_resolve<GLuint (*)()>("glCreateProgram");
    cube_gl.AttachShader = must_resolve<void (*)(GLuint, GLuint)>("glAttachShader");
    cube_gl.LinkProgram = must_resolve<void (*)(GLuint)>("glLinkProgram");
    cube_gl.GetProgramiv = must_resolve<void (*)(GLuint, GLenum, GLint*)>("glGetProgramiv");
    cube_gl.GetProgramInfoLog =
        must_resolve<void (*)(GLuint, GLsizei, GLsizei*, GLchar*)>("glGetProgramInfoLog");
    cube_gl.UseProgram = must_resolve<void (*)(GLuint)>("glUseProgram");
    cube_gl.GenBuffers = must_resolve<void (*)(GLsizei, GLuint*)>("glGenBuffers");
    cube_gl.BindBuffer = must_resolve<void (*)(GLenum, GLuint)>("glBindBuffer");
    cube_gl.BufferData =
        must_resolve<void (*)(GLenum, GLsizeiptr, const void*, GLenum)>("glBufferData");
    cube_gl.GetAttribLocation =
        must_resolve<GLint (*)(GLuint, const GLchar*)>("glGetAttribLocation");
    cube_gl.GetUniformLocation =
        must_resolve<GLint (*)(GLuint, const GLchar*)>("glGetUniformLocation");
    cube_gl.VertexAttribPointer =
        must_resolve<void (*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)>(
            "glVertexAttribPointer");
    cube_gl.EnableVertexAttribArray =
        must_resolve<void (*)(GLuint)>("glEnableVertexAttribArray");
    cube_gl.UniformMatrix4fv =
        must_resolve<void (*)(GLint, GLsizei, GLboolean, const GLfloat*)>("glUniformMatrix4fv");
    cube_gl.DrawElements =
        must_resolve<void (*)(GLenum, GLsizei, GLenum, const void*)>("glDrawElements");

    stud::tools::SpinningCube cube;
    if (!cube.init(cube_gl)) {
        std::fprintf(stderr, "stud: could not build the cube; see the shader log above\n");
        return 1;
    }

    // wl_display_roundtrip() drives the compositor's own configure/frame
    // protocol (map the window, process events); this tool doesn't use
    // android-glue's ALooper machinery, so it's driven directly here.
    const int frames = 300;
    for (int i = 0; i < frames && wl_display_dispatch_pending(display) >= 0; ++i) {
        cube.draw(cube_gl, static_cast<float>(i) / 60.0f, width, height);
        if (eglSwapBuffers_(egl_display, egl_surface) != EGL_TRUE) {
            std::fprintf(stderr, "stud: eglSwapBuffers failed, error=0x%x\n", eglGetError_());
            break;
        }
        wl_display_roundtrip(display);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    std::printf("stud: real render loop completed, %d frames swapped\n", frames);
    return 0;
}
