// stud-render-host: a real, ordinary glibc process hosting ANGLE, the
// real Wayland window, and the real Vulkan loader, everything render/
// vulkan-wsi/android-glue's native_window.cpp already do, unmodified,
// just running in its own process instead of linked into the main
// runtime.
//
// Why a separate process rather than hand-loading ANGLE's glibc .so
// into bionic Process B directly (this session's original plan):
// confirmed, while starting this work, that "load ANGLE via Stud's own
// loader" would also require hand-loading real glibc's OWN libc.so.6/
// libstdc++.so.6 (IFUNC resolvers selected by CPUID, GNU symbol
// versioning, glibc's own TLS models) to satisfy ANGLE's transitive
// imports, dramatically harder and more fragile than anything
// hand-loaded so far in this project. A real, separate glibc process
// for ANGLE sidesteps that entirely: ANGLE loads through an ordinary
// glibc dlopen(), with no hand-parsing at all. Same pattern
// Chrome's own GPU process uses in production. User-approved pivot from
// the original single-process plan text, proven end-to-end (real
// bionic client, inside the real sandbox, driving real ANGLE->Vulkan->
// the real NVIDIA driver->a real Wayland window over this exact IPC
// mechanism) before this full symbol buildout was attempted.
//
// Covers the full real GL/EGL symbol surface libroblox.so's own dynamic
// symbol table imports (85 entries, confirmed via `the ELF headers --dyn-syms`,
// checked). Vulkan gets a deliberately narrower treatment; see
// render_host_protocol.h's own doc comment for why.

#include "stud/session_log.h"
#include "stud/android_glue.h"
#include "stud/text_overlay.h"
#include "stud/clipboard.h"
#include "stud/stud_paths.h"
#include "stud/ndk_types.h"
#include <set>
#include <string>
#include <utility>
#include <optional>

#include "stud/render.h"
#include "stud/audio_output.h"
#include "stud/gamepad.h"
#include "stud/render_host_protocol.h"
#include "stud/discord_rpc.h"
#include "stud/vulkan_host.h"
#ifdef STUD_ENABLE_DEV_RENDER_TOGGLE
#include "stud/dev_backend_config.h"
#include "stud/settings.h"
#endif

#include <wayland-client.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WAYLAND_KHR
// The X11 window backend needs the Xlib WSI structs; Xlib itself is still
// only ever reached through android-glue, which dlopens it.
#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <wayland-egl.h>
#include <wayland-egl-backend.h>

#include <chrono>
#include <deque>
#include <mutex>
#include <sys/wait.h>
#include <fstream>
#include <thread>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <unordered_map>
#include <algorithm>
#include <csignal>
#include <filesystem>
#include <vector>

#include <dlfcn.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

// Guards every read of and dispatch on the Wayland connection:
// defined further down, next to dispatch_mutex(), where its reasoning
// lives. Declared here because the roundtrips inside dispatch() need
// it long before that point in the file.
std::mutex& wayland_mutex();

// Holds the frame back while Stud is not the window being used.
//
// Two cases, and both are ordinary: the window is hidden (minimised, on
// another workspace, fully covered), or it is visible but somebody is
// working in another window, alt-tabbed away. Rendering a game at full
// rate for either is pure waste, and on a laptop it is the difference
// between a warm machine and a hot one. The engine has no idea any of
// that is true, so the frame is paced here instead, in the one place
// every frame has to pass through.
//
// The cost of pacing on focus rather than only on visibility: a window
// deliberately watched on a second monitor while working in another one
// is throttled too. That is what the slider is for, set it high, or
// past the top for Unlimited, and this does nothing.
//
// Sleeping in the present call rather than skipping the frame on purpose:
// a dropped frame leaves the engine's own pacing and its swapchain
// bookkeeping to guess what happened, while a late one is something every
// renderer already copes with. The setting is frames per second; above
// the top of the slider's range it does nothing at all.
void throttle_while_hidden() {
    static const int background_fps = [] {
        const char* v = std::getenv("STUD_BACKGROUND_FPS");
        return v != nullptr ? std::atoi(v) : 30;
    }();
    if (background_fps > 240 || background_fps < 1) return;
    if (stud::android_glue::native_window_is_foreground()) return;

    // Says so the first time, because this is a sleep in the present path
    // and it decides the frame rate.
    //
    // "Foreground" is the compositor's word, not Stud's: on Wayland it is
    // the xdg_toplevel ACTIVATED state, on X11 the focus. If that is ever
    // wrong -- a compositor that does not send ACTIVATED, a state missed
    // across a fullscreen or resize -- then a window the user is looking
    // at is throttled to 30fps and NOTHING else in the log says why. That
    // is far too plausible a cause to leave silent; one line costs
    // nothing and rules the whole mechanism in or out of any frame-rate
    // investigation at a glance.
    static bool said = false;
    if (!said) {
        said = true;
        std::printf("stud-render-host: the window is not foreground, so frames are being paced to "
                    "%d fps (STUD_BACKGROUND_FPS, or the setting it comes from; set it above 240 "
                    "for unlimited). If the window IS the one in use, this is a bug and it is "
                    "capping the frame rate.\n",
                    background_fps);
        std::fflush(stdout);
    }

    const auto interval = std::chrono::microseconds(1000000 / background_fps);
    static auto last = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (last.time_since_epoch().count() != 0) {
        const auto since = now - last;
        if (since < interval) std::this_thread::sleep_for(interval - since);
    }
    last = std::chrono::steady_clock::now();
}

namespace {

// Roblox's own font table, read from the assets the app itself reads it
// from: `android/fonts/font-mappings.json` maps a Roblox Font enum to a
// file under `content/fonts/` and the ratio that turns a Roblox font size
// into a pixel height. Anything not in the table falls back exactly the
// way the real app does (RbxKeyboard.m): 4 -> Bold, 5 -> Light, otherwise
// Regular, at 0.795, and bold carries the real 0.04em letter spacing.
struct RobloxFont {
    std::string path;
    float ratio = 0.795f;
    float letter_spacing = 0.0f;
};

std::string& assets_dir() {
    static std::string dir;
    return dir;
}

const std::unordered_map<int32_t, RobloxFont>& font_table() {
    static const std::unordered_map<int32_t, RobloxFont> table = [] {
        std::unordered_map<int32_t, RobloxFont> t;
        if (assets_dir().empty()) return t;
        const std::string path = assets_dir() + "/android/fonts/font-mappings.json";
        std::FILE* fp = std::fopen(path.c_str(), "rb");
        if (fp == nullptr) {
            std::printf("stud-render-host: no font mapping at %s, the text overlay will use "
                        "the Source Sans fallback\n", path.c_str());
            std::fflush(stdout);
            return t;
        }
        std::string json;
        char buf[4096];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) json.append(buf, n);
        std::fclose(fp);
        // Small, fixed shape ({"enum":N,"font":"X","fromRbxFontRatio":F}
        // repeated), so it is scanned directly rather than pulling a JSON
        // library into this process for one file.
        size_t pos = 0;
        while ((pos = json.find("\"enum\"", pos)) != std::string::npos) {
            const size_t colon = json.find(':', pos);
            if (colon == std::string::npos) break;
            const int32_t id = std::atoi(json.c_str() + colon + 1);
            const size_t font_key = json.find("\"font\"", colon);
            if (font_key == std::string::npos) break;
            const size_t q1 = json.find('"', json.find(':', font_key));
            const size_t q2 = json.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) break;
            RobloxFont f;
            f.path = assets_dir() + "/content/fonts/" + json.substr(q1 + 1, q2 - q1 - 1);
            const size_t ratio_key = json.find("\"fromRbxFontRatio\"", q2);
            const size_t next_enum = json.find("\"enum\"", q2);
            if (ratio_key != std::string::npos &&
                (next_enum == std::string::npos || ratio_key < next_enum)) {
                f.ratio = static_cast<float>(std::atof(json.c_str() + json.find(':', ratio_key) + 1));
            }
            t[id] = f;
            pos = q2;
        }
        std::printf("stud-render-host: text overlay font table: %zu entries\n", t.size());
        std::fflush(stdout);
        return t;
    }();
    return table;
}

// The ids the APK's own mapping describes are legacy Enum.Font values,
// which run to 51 in this build. Anything above that is a modern
// FontFace, which the mapping does not key by id -- but it does carry the
// ratio for the FILE such a FontFace resolves to, which is what
// ratio_for_font_file() below reads.
constexpr int32_t kLastLegacyFontEnum = 51;

// What one point of TextSize is worth, in em, for the overlay.
//
// Read once. The engine sends a text update on every keystroke and a
// config file is not worth opening that often; a changed setting is
// picked up on the next launch, like every other setting here.
float text_overlay_font_ratio() {
    static const float ratio = [] {
        try {
            return stud::config::load_settings(stud::config::default_config_path())
                .text_overlay_font_ratio;
        } catch (const std::exception&) {
            // A broken or unreadable config must not cost the overlay its
            // text; the struct's own default is the honest answer.
            return stud::config::StudSettings{}.text_overlay_font_ratio;
        }
    }();
    return ratio;
}

RobloxFont roblox_font_for(int32_t font_enum) {
    const auto& table = font_table();
    const auto it = table.find(font_enum);
    if (it != table.end()) {
        // The mapping only names a file; whether it is really there is a
        // separate question, and the fallback below is the honest answer
        // when it is not.
        if (::access(it->second.path.c_str(), R_OK) == 0) return it->second;
    }
    RobloxFont f;
    if (font_enum > kLastLegacyFontEnum) {
        // A FontFace, not a legacy enum; in-game chat is one (font=100,
        // TextSize 14). The typeface is Roblox's current default rather
        // than the legacy Source Sans.
        //
        // The ratio is the configured one, because this is the case no
        // file can answer. Both ends were tried against the engine and
        // both were wrong: the mapping and the font agree on
        // 0.7936507937 for BuilderSans (upem 1000 over an
        // ascender-to-descender span of 1260), which draws visibly
        // smaller than the engine, and treating TextSize as the em
        // outright draws visibly larger. A legacy id needs none of this;
        // the mapping states its ratio and it is right.
        f.ratio = text_overlay_font_ratio();
        f.path = assets_dir() + "/content/fonts/BuilderSans-Regular.otf";
        if (::access(f.path.c_str(), R_OK) != 0) {
            f.path = assets_dir() + "/fonts/SourceSansPro-Regular.ttf";
        }
        return f;
    }
    // A legacy id the mapping does not carry: the Source Sans family,
    // whose own upem/(ascender - descender) really is 0.7955, checked
    // against the files rather than assumed.
    f.ratio = 0.795f;
    if (font_enum == 4) {
        f.path = assets_dir() + "/fonts/SourceSansPro-Bold.ttf";
        f.letter_spacing = 0.04f;
    } else if (font_enum == 5) {
        f.path = assets_dir() + "/fonts/SourceSansPro-Light.ttf";
    } else {
        f.path = assets_dir() + "/fonts/SourceSansPro-Regular.ttf";
    }
    return f;
}


using namespace stud::render_host;

// ---- real function pointer types -----------------------------------

using PFN_eglGetDisplay = EGLDisplay (*)(EGLNativeDisplayType);
using PFN_eglInitialize = EGLBoolean (*)(EGLDisplay, EGLint*, EGLint*);
using PFN_eglBindAPI = EGLBoolean (*)(EGLenum);
using PFN_eglChooseConfig = EGLBoolean (*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
using PFN_eglCreateWindowSurface = EGLSurface (*)(EGLDisplay, EGLConfig, EGLNativeWindowType,
                                                   const EGLint*);
using PFN_eglCreatePbufferSurface = EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint*);
using PFN_eglCreateContext = EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
using PFN_eglMakeCurrent = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
using PFN_eglSwapBuffers = EGLBoolean (*)(EGLDisplay, EGLSurface);
using PFN_eglGetError = EGLint (*)(void);
using PFN_eglQueryString = const char* (*)(EGLDisplay, EGLint);
using PFN_eglDestroyContext = EGLBoolean (*)(EGLDisplay, EGLContext);
using PFN_eglDestroySurface = EGLBoolean (*)(EGLDisplay, EGLSurface);
using PFN_eglGetConfigAttrib = EGLBoolean (*)(EGLDisplay, EGLConfig, EGLint, EGLint*);
using PFN_eglGetCurrentContext = EGLContext (*)(void);
using PFN_eglQuerySurface = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint*);
using PFN_eglSwapInterval = EGLBoolean (*)(EGLDisplay, EGLint);
using PFN_eglTerminate = EGLBoolean (*)(EGLDisplay);
using PFN_eglGetProcAddress = void* (*)(const char*);

using PFN_glActiveTexture = void (*)(GLenum);
using PFN_glAttachShader = void (*)(GLuint, GLuint);
using PFN_glBindBuffer = void (*)(GLenum, GLuint);
using PFN_glBindFramebuffer = void (*)(GLenum, GLuint);
using PFN_glBindRenderbuffer = void (*)(GLenum, GLuint);
using PFN_glBindTexture = void (*)(GLenum, GLuint);
using PFN_glBlendFunc = void (*)(GLenum, GLenum);
using PFN_glBlendFuncSeparate = void (*)(GLenum, GLenum, GLenum, GLenum);
using PFN_glCheckFramebufferStatus = GLenum (*)(GLenum);
using PFN_glClear = void (*)(GLbitfield);
using PFN_glClearColor = void (*)(GLfloat, GLfloat, GLfloat, GLfloat);
using PFN_glClearDepthf = void (*)(GLfloat);
using PFN_glClearStencil = void (*)(GLint);
using PFN_glColorMask = void (*)(GLboolean, GLboolean, GLboolean, GLboolean);
using PFN_glCompileShader = void (*)(GLuint);
using PFN_glCopyTexSubImage2D = void (*)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
using PFN_glCreateProgram = GLuint (*)(void);
using PFN_glCreateShader = GLuint (*)(GLenum);
using PFN_glCullFace = void (*)(GLenum);
using PFN_glDeleteProgram = void (*)(GLuint);
using PFN_glDeleteShader = void (*)(GLuint);
using PFN_glDepthFunc = void (*)(GLenum);
using PFN_glDepthMask = void (*)(GLboolean);
using PFN_glDisable = void (*)(GLenum);
using PFN_glDisableVertexAttribArray = void (*)(GLuint);
using PFN_glDrawArrays = void (*)(GLenum, GLint, GLsizei);
using PFN_glDrawElements = void (*)(GLenum, GLsizei, GLenum, const void*);
using PFN_glEnable = void (*)(GLenum);
using PFN_glEnableVertexAttribArray = void (*)(GLuint);
using PFN_glFramebufferRenderbuffer = void (*)(GLenum, GLenum, GLenum, GLuint);
using PFN_glFramebufferTexture2D = void (*)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_glGenerateMipmap = void (*)(GLenum);
using PFN_glGetError = GLenum (*)(void);
using PFN_glLinkProgram = void (*)(GLuint);
using PFN_glPixelStorei = void (*)(GLenum, GLint);
using PFN_glPolygonOffset = void (*)(GLfloat, GLfloat);
using PFN_glReleaseShaderCompiler = void (*)(void);
using PFN_glRenderbufferStorage = void (*)(GLenum, GLenum, GLsizei, GLsizei);
using PFN_glScissor = void (*)(GLint, GLint, GLsizei, GLsizei);
using PFN_glStencilFunc = void (*)(GLenum, GLint, GLuint);
using PFN_glStencilMask = void (*)(GLuint);
using PFN_glStencilOp = void (*)(GLenum, GLenum, GLenum);
using PFN_glTexParameterf = void (*)(GLenum, GLenum, GLfloat);
using PFN_glTexParameteri = void (*)(GLenum, GLenum, GLint);
using PFN_glUniform1i = void (*)(GLint, GLint);
using PFN_glUseProgram = void (*)(GLuint);
using PFN_glViewport = void (*)(GLint, GLint, GLsizei, GLsizei);
using PFN_glVertexAttribPointer = void (*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using PFN_glGetString = const GLubyte* (*)(GLenum);
using PFN_glGetUniformLocation = GLint (*)(GLuint, const GLchar*);
using PFN_glBindAttribLocation = void (*)(GLuint, GLuint, const GLchar*);
using PFN_glShaderSource = void (*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_glGetProgramInfoLog = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glGetShaderInfoLog = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glGetActiveUniform = void (*)(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*);
using PFN_glDeleteBuffers = void (*)(GLsizei, const GLuint*);
using PFN_glDeleteFramebuffers = void (*)(GLsizei, const GLuint*);
using PFN_glDeleteRenderbuffers = void (*)(GLsizei, const GLuint*);
using PFN_glDeleteTextures = void (*)(GLsizei, const GLuint*);
using PFN_glGenBuffers = void (*)(GLsizei, GLuint*);
using PFN_glGenVertexArrays = void (*)(GLsizei, GLuint*);
using PFN_glBindBufferRange = void (*)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr);
using PFN_glBindBufferBase = void (*)(GLenum, GLuint, GLuint);
using PFN_glClearBufferfv = void (*)(GLenum, GLint, const GLfloat*);
// EGL_ANGLE_platform_angle's own enum values. The system eglext.h does not
// carry them (they are ANGLE's extension, not a Khronos one), and copying
// the numbers from ANGLE's own eglext_angle.h is the honest alternative to
// adding its headers to this build for six defines.
#ifndef EGL_PLATFORM_ANGLE_ANGLE
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#endif
#ifndef EGL_PLATFORM_ANGLE_TYPE_ANGLE
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#endif
#ifndef EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE 0x3209
#endif
#ifndef EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE
#define EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE 0x320D
#endif
#ifndef EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE
#define EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE 0x320E
#endif
#ifndef EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE
#define EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE 0x3450
#endif
#ifndef EGL_PLATFORM_ANGLE_DEVICE_TYPE_SWIFTSHADER_ANGLE
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_SWIFTSHADER_ANGLE 0x3487
#endif

using PFN_eglGetPlatformDisplayEXT = EGLDisplay (*)(EGLenum, void*, const EGLint*);
using PFN_glDrawBuffers = void (*)(GLsizei, const GLenum*);
using PFN_glRenderbufferStorageMultisample = void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
using PFN_glInvalidateFramebuffer = void (*)(GLenum, GLsizei, const GLenum*);
using PFN_glBlitFramebuffer = void (*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
using PFN_glDrawArraysInstanced = void (*)(GLenum, GLint, GLsizei, GLsizei);
using PFN_glDrawElementsInstanced = void (*)(GLenum, GLsizei, GLenum, const void*, GLsizei);
using PFN_glVertexAttribDivisor = void (*)(GLuint, GLuint);
using PFN_glVertexAttribIPointer = void (*)(GLuint, GLint, GLenum, GLsizei, const void*);
using PFN_glTexStorage2D = void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
using PFN_glTexStorage3D = void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei);
using PFN_glTexSubImage3D = void (*)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void*);
using PFN_glProgramParameteri = void (*)(GLuint, GLenum, GLint);
using PFN_glGetUniformBlockIndex = GLuint (*)(GLuint, const GLchar*);
using PFN_glUniformBlockBinding = void (*)(GLuint, GLuint, GLuint);
using PFN_glGetActiveUniformBlockiv = void (*)(GLuint, GLuint, GLenum, GLint*);
using PFN_glFenceSync = GLsync (*)(GLenum, GLbitfield);
using PFN_glClientWaitSync = GLenum (*)(GLsync, GLbitfield, GLuint64);
using PFN_glWaitSync = void (*)(GLsync, GLbitfield, GLuint64);
using PFN_glDeleteSync = void (*)(GLsync);
using PFN_glIsSync = GLboolean (*)(GLsync);
using PFN_glGetSynciv = void (*)(GLsync, GLenum, GLsizei, GLsizei*, GLint*);
using PFN_glCopyImageSubData = void (*)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum,
                                        GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei);
using PFN_glBindVertexArray = void (*)(GLuint);
using PFN_glDeleteVertexArrays = void (*)(GLsizei, const GLuint*);
using PFN_glGenFramebuffers = void (*)(GLsizei, GLuint*);
using PFN_glGenRenderbuffers = void (*)(GLsizei, GLuint*);
using PFN_glGenTextures = void (*)(GLsizei, GLuint*);
using PFN_glIsEnabled = GLboolean (*)(GLenum);
using PFN_glGetIntegerv = void (*)(GLenum, GLint*);
using PFN_glTexParameterfv = void (*)(GLenum, GLenum, const GLfloat*);
using PFN_glGetProgramiv = void (*)(GLuint, GLenum, GLint*);
using PFN_glGetShaderiv = void (*)(GLuint, GLenum, GLint*);
using PFN_glGetShaderSource = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glBufferData = void (*)(GLenum, GLsizeiptr, const void*, GLenum);
using PFN_glBufferStorage = void (*)(GLenum, GLsizeiptr, const void*, GLbitfield);
using PFN_glGenQueries = void (*)(GLsizei, GLuint*);
using PFN_glDeleteQueries = void (*)(GLsizei, const GLuint*);
using PFN_glBeginQuery = void (*)(GLenum, GLuint);
using PFN_glEndQuery = void (*)(GLenum);
using PFN_glGetQueryObjectuiv = void (*)(GLuint, GLenum, GLuint*);
using PFN_glGetQueryObjectui64v = void (*)(GLuint, GLenum, GLuint64*);
using PFN_glBufferSubData = void (*)(GLenum, GLintptr, GLsizeiptr, const void*);
using PFN_glMapBufferRange = void* (*)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
using PFN_glGetVertexAttribiv = void (*)(GLuint, GLenum, GLint*);
using PFN_glGetVertexAttribPointerv = void (*)(GLuint, GLenum, void**);
using PFN_glUnmapBuffer = GLboolean (*)(GLenum);
using PFN_glTexImage2D = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                   const void*);
using PFN_glTexSubImage2D = void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                                      const void*);
using PFN_glCompressedTexImage2D = void (*)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei,
                                             const void*);
using PFN_glCompressedTexSubImage2D = void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum,
                                                GLsizei, const void*);
using PFN_glReadPixels = void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);

float unpack_float(uint64_t arg) {
    uint32_t bits = static_cast<uint32_t>(arg);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
uint64_t pack_float(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

// ---- server-side opaque handle tables --------------------------------

uint64_t g_next_handle = 1;
std::unordered_map<uint64_t, EGLDisplay> g_displays;
std::unordered_map<uint64_t, EGLConfig> g_configs;
std::unordered_map<uint64_t, EGLSurface> g_surfaces;
std::unordered_map<uint64_t, EGLContext> g_contexts;
int g_window_surface_create_count = 0;  // temporary diagnostic

uint64_t store_handle(std::unordered_map<uint64_t, void*>&, void*);  // unused generic placeholder

template <typename Map, typename T>
uint64_t store(Map& map, T value) {
    uint64_t h = g_next_handle++;
    map[h] = value;
    return h;
}

struct RealFns {
#define FN(name) PFN_##name name##_
    FN(eglGetDisplay);
    FN(eglInitialize);
    FN(eglBindAPI);
    FN(eglChooseConfig);
    FN(eglCreateWindowSurface);
    FN(eglCreatePbufferSurface);
    FN(eglCreateContext);
    FN(eglMakeCurrent);
    FN(eglSwapBuffers);
    FN(eglGetError);
    FN(eglQueryString);
    FN(eglDestroyContext);
    FN(eglDestroySurface);
    FN(eglGetConfigAttrib);
    FN(eglGetCurrentContext);
    FN(eglQuerySurface);
    FN(eglSwapInterval);
    FN(eglTerminate);
    FN(eglGetProcAddress);

    FN(glActiveTexture);
    FN(glAttachShader);
    FN(glBindBuffer);
    FN(glBindFramebuffer);
    FN(glBindRenderbuffer);
    FN(glBindTexture);
    FN(glBlendFunc);
    FN(glBlendFuncSeparate);
    FN(glCheckFramebufferStatus);
    FN(glClear);
    FN(glClearColor);
    FN(glClearDepthf);
    FN(glClearStencil);
    FN(glColorMask);
    FN(glCompileShader);
    FN(glCopyTexSubImage2D);
    FN(glCreateProgram);
    FN(glCreateShader);
    FN(glCullFace);
    FN(glDeleteProgram);
    FN(glDeleteShader);
    FN(glDepthFunc);
    FN(glDepthMask);
    FN(glDisable);
    FN(glDisableVertexAttribArray);
    FN(glDrawArrays);
    FN(glDrawElements);
    FN(glEnable);
    FN(glEnableVertexAttribArray);
    FN(glFramebufferRenderbuffer);
    FN(glFramebufferTexture2D);
    FN(glGenerateMipmap);
    FN(glGetError);
    FN(glLinkProgram);
    FN(glPixelStorei);
    FN(glPolygonOffset);
    FN(glReleaseShaderCompiler);
    FN(glRenderbufferStorage);
    FN(glScissor);
    FN(glStencilFunc);
    FN(glStencilMask);
    FN(glStencilOp);
    FN(glTexParameterf);
    FN(glTexParameteri);
    FN(glUniform1i);
    FN(glUseProgram);
    FN(glViewport);
    FN(glVertexAttribPointer);
    FN(glGetString);
    FN(glGetUniformLocation);
    FN(glBindAttribLocation);
    FN(glShaderSource);
    FN(glGetProgramInfoLog);
    FN(glGetShaderInfoLog);
    FN(glGetActiveUniform);
    FN(glDeleteBuffers);
    FN(glDeleteFramebuffers);
    FN(glDeleteRenderbuffers);
    FN(glDeleteTextures);
    FN(glGenBuffers);
    FN(glGenVertexArrays);
    FN(glBindBufferRange);
    FN(glBindBufferBase);
    FN(glClearBufferfv);
    FN(eglGetPlatformDisplayEXT);
    FN(glDrawBuffers);
    FN(glRenderbufferStorageMultisample);
    FN(glInvalidateFramebuffer);
    FN(glBlitFramebuffer);
    FN(glDrawArraysInstanced);
    FN(glDrawElementsInstanced);
    FN(glVertexAttribDivisor);
    FN(glVertexAttribIPointer);
    FN(glTexStorage2D);
    FN(glTexStorage3D);
    FN(glTexSubImage3D);
    FN(glProgramParameteri);
    FN(glGetUniformBlockIndex);
    FN(glUniformBlockBinding);
    FN(glGetActiveUniformBlockiv);
    FN(glFenceSync);
    FN(glClientWaitSync);
    FN(glWaitSync);
    FN(glDeleteSync);
    FN(glIsSync);
    FN(glGetSynciv);
    FN(glCopyImageSubData);
    FN(glBindVertexArray);
    FN(glDeleteVertexArrays);
    FN(glGenFramebuffers);
    FN(glGenRenderbuffers);
    FN(glGenTextures);
    FN(glGetIntegerv);
    FN(glIsEnabled);
    FN(glTexParameterfv);
    FN(glGetProgramiv);
    FN(glGetShaderiv);
    FN(glGetShaderSource);
    FN(glBufferData);
    FN(glBufferStorage);
    FN(glGenQueries);
    FN(glDeleteQueries);
    FN(glBeginQuery);
    FN(glEndQuery);
    FN(glGetQueryObjectuiv);
    FN(glGetQueryObjectui64v);
    FN(glBufferSubData);
    FN(glMapBufferRange);
    FN(glGetVertexAttribiv);
    FN(glGetVertexAttribPointerv);
    FN(glUnmapBuffer);
    FN(glTexImage2D);
    FN(glTexSubImage2D);
    FN(glCompressedTexImage2D);
    FN(glCompressedTexSubImage2D);
    FN(glReadPixels);
#undef FN
};

// Optional: an entry point the driver may genuinely not have. The
// caller must handle a null, and does; see GlBufferStorage, which
// falls back to an ordinary mutable allocation.
template <typename Fn>
Fn may_resolve(const char* name) {
    return reinterpret_cast<Fn>(stud::render::resolve(name));
}

template <typename Fn>
Fn must_resolve(const char* name) {
    void* addr = stud::render::resolve(name);
    if (addr == nullptr) {
        std::fprintf(stderr, "stud-render-host: could not resolve required symbol '%s'\n", name);
        std::exit(1);
    }
    return reinterpret_cast<Fn>(addr);
}

struct RealWindow {
    // Wayland: the compositor connection, the EGL window and the surface.
    // X11: display is null, and the EGL native window is the X window id.
    // Which one is live is settled once by android-glue's own
    // display_backend(); nothing here decides it a second time.
    wl_display* display;
    wl_egl_window* egl_window;
    wl_surface* surface;
    void* x11_display = nullptr;
    unsigned long x11_window = 0;

    bool on_x11() const { return x11_display != nullptr; }
    // What EGL is handed: eglGetDisplay takes the X Display on X11 and
    // the wl_display on Wayland; eglCreateWindowSurface takes the X
    // window id or the wl_egl_window.
    void* egl_native_display() const {
        return on_x11() ? x11_display : static_cast<void*>(display);
    }
    EGLNativeWindowType egl_native_window() const {
        return on_x11() ? static_cast<EGLNativeWindowType>(x11_window)
                        : reinterpret_cast<EGLNativeWindowType>(egl_window);
    }
};

// Real Vulkan loader, for the one narrow interposition this module
// handles: redirecting Roblox's vkCreateAndroidSurfaceKHR (an
// Android-only extension with no Linux equivalent) to the real
// vkCreateWaylandSurfaceKHR, using the SAME Wayland display/surface
// android-glue's native_window.cpp already owns here. See
// render_host_protocol.h's own doc comment for why this is deliberately
// not a full Vulkan struct marshaller.
void* g_vulkan_handle = nullptr;
PFN_vkGetInstanceProcAddr g_real_vk_get_instance_proc_addr = nullptr;

// The one real window, captured once at startup. Anything needing it
// reads this rather than re-deriving it, re-deriving is what created a
// window per call and put a hundred of them on screen.
ANativeWindow* g_real_window = nullptr;

// Defined below, beside the other code that spawns a process.
void raise_through_kwin();

// Viewer windows that have exited and not yet been reported to
// Process B, which is what tells the app its web view is gone.
std::atomic<uint32_t> g_webview_closed{0};

// Messages a web view's page sent through its JavaScript bridge, waiting
// for Process B to collect them. A login challenge (an OTP, a captcha)
// reports that it is finished this way, so dropping these means the page
// completes and the login never does.
std::mutex g_webview_message_mutex;
// Deep links handed over by a second stud-ui. Bounded, because a queue
// nothing drains must not grow for the life of the process, and if
// several arrive before Process B looks, the newest is the one the person
// actually asked for.
std::mutex g_deep_link_mutex;
std::deque<std::string> g_deep_links;
std::deque<std::string> g_webview_messages;

// URLs the viewer refused to navigate to itself, waiting for the app to
// say whether the engine wants them. Separate from the bridge queue: one
// is a page talking to the app, the other is a navigation the app has
// first refusal on.
std::mutex g_webview_navigation_mutex;
std::deque<std::string> g_webview_navigations;

// The open viewer's stdin. It stays open for the life of the panel now,
// because the answer to a blocked navigation ("load it after all") has
// to reach it. -1 when no viewer is open.
std::atomic<int> g_webview_stdin{-1};

void queue_web_view_navigation(std::string url) {
    std::lock_guard<std::mutex> lock(g_webview_navigation_mutex);
    if (g_webview_navigations.size() >= 32) g_webview_navigations.pop_front();
    g_webview_navigations.push_back(std::move(url));
}

void queue_web_view_message(std::string message) {
    std::lock_guard<std::mutex> lock(g_webview_message_mutex);
    // A page that talks endlessly must not grow this without bound; the
    // engine only ever wants the latest exchange anyway.
    if (g_webview_messages.size() >= 64) g_webview_messages.pop_front();
    g_webview_messages.push_back(std::move(message));
}

// Reads the viewer's own stdout, forwarding the lines its bridge writes
// and passing everything else through so the viewer's logging still
// reaches the same place it always did.
void read_web_view_output(int fd) {
    std::string pending;
    char buffer[4096];
    for (;;) {
        const ssize_t got = ::read(fd, buffer, sizeof(buffer));
        if (got <= 0) break;
        pending.append(buffer, static_cast<size_t>(got));
        for (;;) {
            const auto newline = pending.find('\n');
            if (newline == std::string::npos) break;
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            static constexpr char kNavigate[] = "navigate ";
            static constexpr char kPrefix[] = "hybrid ";
            if (line.rfind(kNavigate, 0) == 0) {
                const std::string url = line.substr(sizeof(kNavigate) - 1);
                queue_web_view_navigation(url);
                // The query can carry a one-time join ticket.
                const auto cut = url.find('?');
                std::printf("stud-render-host: web view asked about %s\n",
                            (cut == std::string::npos ? url
                                                      : url.substr(0, cut) + "?<query withheld>")
                                .c_str());
            } else if (line.rfind(kPrefix, 0) == 0) {
                queue_web_view_message(line.substr(sizeof(kPrefix) - 1));
                std::printf("stud-render-host: web view sent a bridge message (%zu bytes)\n",
                            line.size() - (sizeof(kPrefix) - 1));
            } else {
                std::printf("%s\n", line.c_str());
            }
            std::fflush(stdout);
        }
    }
    ::close(fd);
}

// Web-view viewers spawned by this process. They are session leaders of
// their own (setsid, so the compositor treats the window as the viewer's
// rather than a stray child of the game window), which means nothing
// tears them down on their own when Stud exits, live-reported as a
// panel left on screen after the game window closed. Tracked so shutdown
// can close them.
//
// A fixed table of atomics rather than a vector behind a mutex, because
// this is also read from a signal handler: taking a lock there can
// deadlock outright if the signal arrives while the same thread holds it.
// Nothing here allocates, and every operation is a single atomic.
constexpr size_t kMaxWebViews = 16;
std::atomic<pid_t> g_webview_pids[kMaxWebViews];

void track_web_view(pid_t pid) {
    for (auto& slot : g_webview_pids) {
        pid_t empty = 0;
        if (slot.compare_exchange_strong(empty, pid)) return;
    }
    // Table full: the viewer still works, it just will not be closed
    // automatically. Not silent, since that is a real (if unlikely) gap.
    std::printf("stud-render-host: too many web views to track; this one will not be "
                "closed automatically\n");
    std::fflush(stdout);
}

void forget_web_view(pid_t pid) {
    for (auto& slot : g_webview_pids) {
        pid_t expected = pid;
        if (slot.compare_exchange_strong(expected, 0)) return;
    }
}

// Async-signal-safe: only atomic loads and kill().
//
// Waits briefly afterwards so the viewer is really gone before this
// process exits. Without it, shutdown raced the reaper thread that is
// still blocked in waitpid(), live-caught as `terminate called without
// an active exception`, the runtime destroying a joinable thread while
// it ran. `wait` is not signal-safe, so only the kill half runs from a
// handler; the ordinary shutdown paths ask for the wait.
void close_open_web_views(bool wait_for_exit = false);

void close_open_web_views(bool wait_for_exit) {
    for (auto& slot : g_webview_pids) {
        const pid_t pid = slot.exchange(0);
        // SIGTERM, not SIGKILL: the viewer is a Qt application and gets to
        // shut its own web engine down cleanly, the same as a window
        // manager asking it to close.
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            if (wait_for_exit) {
                // The reaper thread owns the real waitpid(); this only
                // needs the process to be on its way out, and must never
                // block shutdown if the viewer ignores SIGTERM.
                for (int i = 0; i < 100 && ::kill(pid, 0) == 0; ++i) {
                    ::usleep(10 * 1000);
                }
            }
        }
    }
}

// A per-message scratch buffer, reused across calls, avoids a fresh
// heap allocation for every single GL call (many of which have no
// buffer payload at all).
std::vector<uint8_t> g_in_scratch;
std::vector<uint8_t> g_out_scratch;

bool read_all(int fd, void* data, uint32_t len) {
    char* p = static_cast<char*>(data);
    uint32_t remaining = len;
    while (remaining > 0) {
        ssize_t n = ::read(fd, p, remaining);
        if (n < 0 && errno == EINTR) continue;  // EINTR is not a disconnect
        if (n <= 0) return false;
        p += n;
        remaining -= static_cast<uint32_t>(n);
    }
    return true;
}
bool write_all(int fd, const void* data, uint32_t len) {
    const char* p = static_cast<const char*>(data);
    uint32_t remaining = len;
    while (remaining > 0) {
        ssize_t n = ::write(fd, p, remaining);
        if (n < 0 && errno == EINTR) continue;  // EINTR is not a disconnect
        if (n <= 0) return false;
        p += n;
        remaining -= static_cast<uint32_t>(n);
    }
    return true;
}

// Real GLES3 bytes-per-texel. The old version assumed 4 components for
// anything it did not recognise, which silently over-read the caller's
// buffer by 4x for a real GL_RED upload, live-caught as a fatal
// EFAULT on the render socket (the engine uploads 128x2048 GL_RED
// glyph/mask atlases during real UI bring-up), which killed the render
// connection permanently and froze the window mid-frame.
uint32_t gl_pixel_size(GLenum format, GLenum type) {
    uint32_t components = 4;
    switch (format) {
        case GL_RED:
        case GL_RED_INTEGER:
        case GL_ALPHA:
        case GL_LUMINANCE:
        case GL_DEPTH_COMPONENT:
        case GL_STENCIL_INDEX8:
            components = 1;
            break;
        case GL_RG:
        case GL_RG_INTEGER:
        case GL_LUMINANCE_ALPHA:
        case GL_DEPTH_STENCIL:
            components = 2;
            break;
        case GL_RGB:
        case GL_RGB_INTEGER:
            components = 3;
            break;
        case GL_RGBA:
        case GL_RGBA_INTEGER:
        default:
            components = 4;
            break;
    }
    switch (type) {
        // Packed types carry the whole texel in one unit.
        case GL_UNSIGNED_SHORT_5_6_5:
        case GL_UNSIGNED_SHORT_4_4_4_4:
        case GL_UNSIGNED_SHORT_5_5_5_1:
            return 2;
        case GL_UNSIGNED_INT_2_10_10_10_REV:
        case GL_UNSIGNED_INT_10F_11F_11F_REV:
        case GL_UNSIGNED_INT_5_9_9_9_REV:
        case GL_UNSIGNED_INT_24_8:
            return 4;
        case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
            return 8;
        // Unpacked types: one unit per component.
        case GL_BYTE:
        case GL_UNSIGNED_BYTE:
            return components;
        case GL_SHORT:
        case GL_UNSIGNED_SHORT:
        case GL_HALF_FLOAT:
            return components * 2u;
        case GL_INT:
        case GL_UNSIGNED_INT:
        case GL_FLOAT:
            return components * 4u;
        default:
            return components;
    }
}

}  // namespace

// Defined further down, at the main loop that usually calls it, input
// asks for it too, so that a pointer event is never older than the moment
// it was requested. Declared out here because that definition is at file
// scope, not in the anonymous namespace below.
namespace { struct RealWindow; }
void pump_display(const RealWindow& window, bool fd_readable);
// Input's own lean dispatch; see its definition for why it is not
// pump_display.
void dispatch_input_queue(wl_display* display, bool fd_readable);

namespace {
uint64_t g_window_surface_handle = kNullHandle;
bool g_prefer_vulkan = true;
// Which backend ANGLE should translate GLES to. Empty means ANGLE's own
// default, which is Vulkan on this platform.
std::string g_angle_backend;
bool g_hidpi_enabled = true;
// Stud's own upscaler, and how far below the screen the engine renders
// for it, DLSS's quality presets. The output is never a setting: it is
// always the window's size in the display's own pixels.
bool g_upscaling_enabled = false;
// The engine's render size is NOT a setting; it is pinned to the
// window's logical size, because moving it moves the UI's size with it,
// and neither is the output: that is the window's own resolution, which
// the compositor shows 1:1.
int g_upscale_sharpness_percent = 100;
bool g_discord_enabled = false;
bool g_discord_join_button = false;

// STUD_TRACE_CURSOR=1: the experiment that separates "the engine never draws
// its own cursor" from "it draws it and the pixels never reach the screen".
// Roblox's cursor art is a 64x64 texture and belongs topmost, so record the
// bound texture (and its real dimensions) at every draw and report, per
// frame, how many draws used a 64x64 texture plus the full GL state and
// buffer bindings of the last such draw. Off by default; it only reads state
// and binds nothing, unlike the read-back that once blacked out the window.
bool cursor_trace_enabled() {
    static const bool on = std::getenv("STUD_TRACE_CURSOR") != nullptr;
    return on;
}
std::unordered_map<GLuint, std::pair<int, int>> g_tex_dims;
GLuint g_bound_texture_2d = 0;
uint64_t g_frame_draws = 0;
uint64_t g_frame_cursor_draws = 0;
GLuint g_last_cursor_tex = 0;
uint64_t g_cursor_trace_frames = 0;
std::string g_last_cursor_state;

template <typename Fns>
void note_draw_for_cursor_trace(const Fns& fns, GLsizei count = 0, GLenum index_type = 0,
                                uintptr_t index_offset = 0, GLint first = 0) {
    if (!cursor_trace_enabled()) return;
    ++g_frame_draws;
    auto it = g_tex_dims.find(g_bound_texture_2d);
    if (it == g_tex_dims.end() || it->second.first != 64 || it->second.second != 64) return;
    ++g_frame_cursor_draws;
    g_last_cursor_tex = g_bound_texture_2d;
    GLint vp[4] = {0, 0, 0, 0};
    GLint sc[4] = {0, 0, 0, 0};
    GLint prog = 0, fbo = 0, arr = 0, elem = 0, vao = 0, blend_on = 0, depth_on = 0;
    GLint src_rgb = 0, dst_rgb = 0;
    fns.glGetIntegerv_(GL_VIEWPORT, vp);
    fns.glGetIntegerv_(GL_SCISSOR_BOX, sc);
    fns.glGetIntegerv_(GL_CURRENT_PROGRAM, &prog);
    fns.glGetIntegerv_(GL_FRAMEBUFFER_BINDING, &fbo);
    fns.glGetIntegerv_(GL_ARRAY_BUFFER_BINDING, &arr);
    fns.glGetIntegerv_(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elem);
    fns.glGetIntegerv_(GL_VERTEX_ARRAY_BINDING, &vao);
    fns.glGetIntegerv_(GL_BLEND, &blend_on);
    fns.glGetIntegerv_(GL_DEPTH_TEST, &depth_on);
    fns.glGetIntegerv_(GL_BLEND_SRC_RGB, &src_rgb);
    fns.glGetIntegerv_(GL_BLEND_DST_RGB, &dst_rgb);
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "tex=%u prog=%d fbo=%d vp=[%d,%d,%d,%d] scissor=[%d,%d,%d,%d] blend=%d(%x/%x) "
                  "depthTest=%d arrayBuf=%d elemBuf=%d vao=%d",
                  g_bound_texture_2d, prog, fbo, vp[0], vp[1], vp[2], vp[3], sc[0], sc[1], sc[2],
                  sc[3], blend_on, src_rgb, dst_rgb, depth_on, arr, elem, vao);
    g_last_cursor_state = buf;

    // One-shot, and only under the trace: read the real geometry the cursor
    // draw is about to use. Every piece of surrounding state has repeatedly
    // measured healthy, so the vertex data itself is the remaining suspect,
    // and guessing at it from the client side is what has stalled this
    // investigation for several sessions.
    // Only sample well after start-up: the first 64x64 draws in a process are
    // boot-time UI, not the cursor, and dumping those wasted a whole pass.
    static int dumped = 0;
    if (g_cursor_trace_frames < 400 || dumped >= 6 || arr == 0) return;
    ++dumped;
    std::printf("stud-render-host: CURSORTRACE draw count=%d first=%d indexType=0x%x indexOffset=%zu\n",
                static_cast<int>(count), static_cast<int>(first), index_type,
                static_cast<size_t>(index_offset));
    // Read exactly the indices this draw consumes, then exactly the vertices
    // they name, using attribute 0's real stride and offset. Everything else
    // about this draw has measured healthy for several sessions; the data is
    // the only thing left, and it has never actually been looked at.
    GLint stride = 0, attrib_size = 0, attrib_type = 0;
    void* attrib_ptr = nullptr;
    fns.glGetVertexAttribiv_(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
    fns.glGetVertexAttribiv_(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attrib_size);
    fns.glGetVertexAttribiv_(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attrib_type);
    fns.glGetVertexAttribPointerv_(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &attrib_ptr);
    const size_t attrib_base = reinterpret_cast<uintptr_t>(attrib_ptr);
    std::printf("stud-render-host: CURSORTRACE attrib0 size=%d type=0x%x stride=%d offset=%zu\n",
                attrib_size, attrib_type, stride, attrib_base);
    if (elem != 0 && count > 0 && index_type == GL_UNSIGNED_SHORT) {
        const GLsizeiptr bytes = static_cast<GLsizeiptr>(count) * 2;
        if (void* ix = fns.glMapBufferRange_(GL_ELEMENT_ARRAY_BUFFER,
                                              static_cast<GLintptr>(index_offset), bytes,
                                              GL_MAP_READ_BIT)) {
            const unsigned short* u = static_cast<const unsigned short*>(ix);
            std::vector<unsigned short> idx(u, u + count);
            fns.glUnmapBuffer_(GL_ELEMENT_ARRAY_BUFFER);
            std::printf("stud-render-host: CURSORTRACE indices:");
            for (GLsizei i = 0; i < count && i < 12; ++i) std::printf(" %u", static_cast<unsigned>(idx[i]));
            std::printf("\n");
            unsigned short lo = idx[0], hi = idx[0];
            for (unsigned short v : idx) { lo = v < lo ? v : lo; hi = v > hi ? v : hi; }
            if (stride > 0) {
                const GLintptr off = static_cast<GLintptr>(attrib_base + static_cast<size_t>(lo) * stride);
                const GLsizeiptr len = static_cast<GLsizeiptr>(hi - lo + 1) * stride;
                if (void* vb = fns.glMapBufferRange_(GL_ARRAY_BUFFER, off, len, GL_MAP_READ_BIT)) {
                    std::printf("stud-render-host: CURSORTRACE verts[%u..%u] attrib0:", lo, hi);
                    for (unsigned short v = lo; v <= hi && v < lo + 8; ++v) {
                        const float* f = reinterpret_cast<const float*>(
                            static_cast<const unsigned char*>(vb) + (v - lo) * stride);
                        std::printf(" (%.2f,%.2f)", static_cast<double>(f[0]), static_cast<double>(f[1]));
                    }
                    std::printf("\n");
                    fns.glUnmapBuffer_(GL_ARRAY_BUFFER);
                }
            }
        }
    }
    std::fflush(stdout);
}

// Makes SPIRV-Cross's array-index arithmetic compile under ANGLE.
//
// Roblox's GLES shader pack contains index expressions like
//
//     CB12[((uint(POSITION.w) >> 8u) & 255u) * 1 + 0].xyz
//
// The `1` and `0` are `const int` while the left operand is `uint`.
// ESSL 3.00 has no implicit int->uint conversion, so ANGLE, which
// implements the spec strictly, rejects it:
//
//     ERROR: '*' : wrong operand types ... 'highp uint' and 'const int'
//
// Real Android GL drivers accept it, which is why the pack ships this
// way and works on a device. 35 shaders fail here without this, the
// terrain (SmoothCluster*) and part (DefaultUnified*) shaders, i.e.
// most of what a game looks like.
//
// The rewrite is deliberately narrow: ONLY inside `[...]`, and only
// where the arithmetic follows an unsigned literal's closing paren
// (`...255u) * 1 + 0`). A broader "suffix any int after a uint" pass was
// tried and made things far worse, 35 failures became 795, because
// it also rewrote signed contexts where the int was correct. Matching
// the generated shape exactly is what keeps it safe; anything that does
// not match is left for ANGLE to judge.
std::string fix_uint_index_arithmetic(const std::string& src) {
    static const std::string kMarker = "u) * ";
    if (src.find(kMarker) == std::string::npos) return src;

    std::string out;
    out.reserve(src.size() + 64);
    size_t i = 0;
    while (i < src.size()) {
        // Only rewrite between '[' and the matching ']', so ordinary
        // arithmetic elsewhere in the shader is never touched.
        if (src[i] != '[') {
            out.push_back(src[i++]);
            continue;
        }
        const size_t close = src.find(']', i);
        if (close == std::string::npos) {
            out.append(src, i, std::string::npos);
            break;
        }
        std::string index = src.substr(i, close - i + 1);
        if (index.find(kMarker) != std::string::npos) {
            std::string fixed;
            fixed.reserve(index.size() + 8);
            for (size_t j = 0; j < index.size(); ++j) {
                fixed.push_back(index[j]);
                if (index[j] != '*' && index[j] != '+') continue;
                // Copy the spaces, then suffix the bare integer that
                // follows so it is unsigned like the left operand.
                size_t k = j + 1;
                while (k < index.size() && index[k] == ' ') fixed.push_back(index[k++]);
                const size_t digits = k;
                while (k < index.size() && std::isdigit(static_cast<unsigned char>(index[k]))) {
                    fixed.push_back(index[k++]);
                }
                if (k > digits && (k >= index.size() || (index[k] != 'u' && index[k] != '.' &&
                                                          index[k] != 'f'))) {
                    fixed.push_back('u');
                }
                j = k - 1;
            }
            index = std::move(fixed);
        }
        out.append(index);
        i = close + 1;
    }
    return out;
}

// Runs stud-ui in a one-shot secret mode, writing `input` to its stdin
// and, when `capture` is non-null, collecting its stdout into it.
//
// Stud's safe storage lives behind the Secret Service, which only this
// process can reach (Process B is sandboxed, Process A has exited).
// Secrets go over pipes rather than argv or the environment, because
// /proc/<pid>/cmdline and /proc/<pid>/environ are readable by anything
// running as this user. Only the secret's NAME is ever an argument.
// Runs stud-ui one-shot and does not wait for it. Same discovery of the
// sibling binary as the keyring helper below; nothing is read back, so
// there are no pipes and no reaping beyond the double fork.
// Where the Qt half of Stud is, from render-host's own location.
//
// It is installed as `stud` (bin/stud) and only ever called `stud-ui` in
// a build tree, so a list that knew only the build-tree name worked
// here and nowhere else. That is not cosmetic: the keyring helper below
// is what persists a login, and the server-region notification goes the
// same way, so both were dead in every package (rpm, deb, Arch, AppImage
// and Flatpak alike) while working perfectly from a build tree. Caught
// in the Flatpak as `read secret "rbxas" ... absent (helper ok=0)` with
// an empty secrets directory, which is why a relaunch always asked for a
// login again.
//
// bin/stud is two levels up from libexec/stud, and from lib/stud too,
// which is where Arch puts it (namcap rejects libexec).
std::vector<std::string> ui_helper_candidates(const std::string& own_dir) {
    std::vector<std::string> paths;
    if (!own_dir.empty()) {
        paths.push_back(own_dir + "/../ui/stud-ui");   // a build tree
        paths.push_back(own_dir + "/../../bin/stud");  // any install tree
        paths.push_back(own_dir + "/stud-ui");
    }
    paths.push_back("stud");     // and, failing that, the PATH
    paths.push_back("stud-ui");
    return paths;
}

void run_ui_helper_detached(const char* mode, const std::string& argument) {
    const pid_t pid = ::fork();
    if (pid != 0) {
        if (pid > 0) ::waitpid(pid, nullptr, 0);  // the intermediate exits at once
        return;
    }
    // Child: fork again so the grandchild is reparented to init and this
    // process never has to reap it.
    if (::fork() != 0) ::_exit(0);
    ::setsid();
    std::string own_dir;
    {
        char buf[4096];
        const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            const std::string exe(buf);
            const auto slash = exe.find_last_of('/');
            if (slash != std::string::npos) own_dir = exe.substr(0, slash);
        }
    }
    for (const std::string& path : ui_helper_candidates(own_dir)) {
        if (path.find('/') == std::string::npos) {
            ::execlp(path.c_str(), path.c_str(), mode, argument.c_str(), nullptr);
        } else {
            ::execl(path.c_str(), path.c_str(), mode, argument.c_str(), nullptr);
        }
    }
    ::_exit(127);
}

bool run_ui_secret_helper(const char* mode, const std::string& name, const std::string& input,
                           std::string* capture) {
    int to_child[2] = {-1, -1};
    int from_child[2] = {-1, -1};
    if (::pipe(to_child) != 0) return false;
    if (capture != nullptr && ::pipe(from_child) != 0) {
        ::close(to_child[0]);
        ::close(to_child[1]);
        return false;
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(to_child[1]);
        ::dup2(to_child[0], STDIN_FILENO);
        ::close(to_child[0]);
        if (capture != nullptr) {
            ::close(from_child[0]);
            ::dup2(from_child[1], STDOUT_FILENO);
            ::close(from_child[1]);
        }
        ::setsid();
        std::string own_dir;
        {
            char buf[4096];
            const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                const std::string exe(buf);
                const auto slash = exe.find_last_of('/');
                if (slash != std::string::npos) own_dir = exe.substr(0, slash);
            }
        }
        for (const std::string& path : ui_helper_candidates(own_dir)) {
            if (path.find('/') == std::string::npos) {
                ::execlp(path.c_str(), path.c_str(), mode, name.c_str(), nullptr);
            } else {
                ::execl(path.c_str(), path.c_str(), mode, name.c_str(), nullptr);
            }
        }
        ::_exit(127);
    }
    ::close(to_child[0]);
    if (capture != nullptr) ::close(from_child[1]);
    if (pid < 0) {
        ::close(to_child[1]);
        if (capture != nullptr) ::close(from_child[0]);
        return false;
    }
    {
        const char* bytes = input.data();
        size_t left = input.size();
        while (left > 0) {
            const ssize_t written = ::write(to_child[1], bytes, left);
            if (written <= 0) break;
            bytes += written;
            left -= static_cast<size_t>(written);
        }
    }
    ::close(to_child[1]);
    if (capture == nullptr) {
        // Nothing to read back, so do not make the render loop wait on
        // the keyring: reap in the background.
        std::thread([pid] { int status = 0; ::waitpid(pid, &status, 0); }).detach();
        return true;
    }
    char buf[4096];
    while (true) {
        const ssize_t n = ::read(from_child[0], buf, sizeof(buf));
        if (n <= 0) break;
        capture->append(buf, static_cast<size_t>(n));
    }
    ::close(from_child[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Dispatches one request. `in` is exactly `hdr.in_buffer_len` bytes
// (already read); `out` must be filled with up to `hdr.out_buffer_len`
// bytes, `*out_len` set to how many real bytes were written (may be 0).
// Leave without running static destructors or atexit handlers. Every
// shutdown path here uses it: the window closing, the engine
// disconnecting, and CallId::EndSession, which exits from inside
// dispatch() and so needs this declared above it rather than beside the
// accept loop where it used to live.
[[noreturn]] void exit_now(int status) {
    std::fflush(nullptr);
    ::_exit(status);
}

// STUD_GL_TRACE_TEX=1: what actually reaches each texture.
//
// A texture that samples as though it only has its small mips is either
// missing its big ones or being told not to use them, and those are
// different bugs. This records, per texture name, which levels were
// uploaded and every sampler parameter that can pin the level, base,
// max, min/max LOD and the min filter, and prints one line per texture
// the first time it is drawn with.
void trace_texture_upload(const char* what, GLuint texture, GLint level, GLsizei w, GLsizei h,
                          GLenum format, size_t bytes = 0, uint64_t pbo_offset_plus_one = 0,
                          size_t received = 0) {
    static const bool on = std::getenv("STUD_GL_TRACE_TEX") != nullptr;
    if (!on) return;
    // `bytes` is the imageSize the caller declared; `received` is how
    // much actually arrived over the wire. They must match, or the
    // driver reads whatever follows the buffer.
    std::printf("stud-render-host: TEX %s tex=%u level=%d %dx%d fmt=0x%x declared=%zu got=%zu "
                "pbo=%lld%s\n",
                what, texture, level, static_cast<int>(w), static_cast<int>(h), format, bytes,
                received,
                pbo_offset_plus_one == 0 ? -1LL
                                         : static_cast<long long>(pbo_offset_plus_one - 1),
                (pbo_offset_plus_one == 0 && bytes != received) ? "  <-- SHORT" : "");
    std::fflush(stdout);
}

void trace_texture_parameter(GLenum target, GLenum pname, GLint value) {
    static const bool on = std::getenv("STUD_GL_TRACE_TEX") != nullptr;
    if (!on) return;
    // Only the ones that decide which level is sampled.
    switch (pname) {
        case GL_TEXTURE_BASE_LEVEL:
        case GL_TEXTURE_MAX_LEVEL:
        case GL_TEXTURE_MIN_LOD:
        case GL_TEXTURE_MAX_LOD:
        case GL_TEXTURE_MIN_FILTER:
            break;
        default:
            return;
    }
    std::printf("stud-render-host: TEX param target=0x%x pname=0x%x value=%d\n", target, pname,
                value);
    std::fflush(stdout);
}


// STUD_FRAME_PACING=1: the distribution of frame intervals, not their
// average.
//
// A steady 60fps and a 60fps made of alternating 8ms and 40ms frames
// produce the same number in a counter and feel completely different.
// Every frame-rate measurement in this project so far has been an
// average over seconds, which cannot tell those apart, so a report of
// "the frame rate is fine but it feels bad" had nothing to answer it.
void note_frame_pacing() {
    static const bool on = std::getenv("STUD_FRAME_PACING") != nullptr;
    if (!on) return;
    using clock = std::chrono::steady_clock;
    static clock::time_point last{};
    static std::vector<double> intervals;
    const auto now = clock::now();
    if (last.time_since_epoch().count() != 0) {
        intervals.push_back(std::chrono::duration<double, std::milli>(now - last).count());
    }
    last = now;
    if (intervals.size() < 240) return;

    std::vector<double> sorted = intervals;
    std::sort(sorted.begin(), sorted.end());
    const auto at = [&sorted](double q) {
        return sorted[static_cast<size_t>(q * static_cast<double>(sorted.size() - 1))];
    };
    double total = 0.0;
    for (double v : sorted) total += v;
    const double mean = total / static_cast<double>(sorted.size());
    // Frames that took more than twice the median are what a player
    // notices; counting them says whether this is a hitch problem.
    const double median = at(0.5);
    size_t spikes = 0;
    for (double v : sorted) {
        if (v > median * 2.0) ++spikes;
    }
    std::printf("stud-render-host: PACING over %zu frames: mean %.1fms (%.0f fps) | p50 %.1f "
                "p90 %.1f p99 %.1f max %.1f | %zu frames over 2x median\n",
                sorted.size(), mean, 1000.0 / mean, median, at(0.9), at(0.99), sorted.back(),
                spikes);
    std::fflush(stdout);
    intervals.clear();
}


// Turn on the ANGLE extensions that are available but not advertised.
//
// ANGLE keeps a set of extensions "requestable": the driver underneath
// supports them, but glGetString(GL_EXTENSIONS) does not list them until
// the application asks for each by name (GL_ANGLE_request_extension).
// Nothing asked, so the engine saw no block compression at all,
// measured from its own capability line, `Caps: Texture: DXT 0 PVR 0
// ETC1 0 ETC2 1`.
//
// That costs real quality, not just memory. Opaque textures still arrive
// as ETC2, which ANGLE emulates, but textures WITH ALPHA end up stored
// uncompressed, four to eight times the size, against the engine's
// compiled-in 64MB video-memory budget. Its streamer then holds some of
// them at a low mip forever, which shows up as transparent textures
// staying blurry while everything else is sharp.
//
// Requested once per context, and quietly: an extension that is not
// requestable on this driver simply is not asked for.
void enable_requestable_extensions(const RealFns& fns) {
    static bool done = false;
    if (done || fns.glGetString_ == nullptr) return;
    done = true;

    using RequestExtensionFn = void (*)(const GLchar*);
    auto request = reinterpret_cast<RequestExtensionFn>(
        fns.eglGetProcAddress_ != nullptr
            ? reinterpret_cast<void*>(fns.eglGetProcAddress_("glRequestExtensionANGLE"))
            : nullptr);
    if (request == nullptr) return;

    // GL_REQUESTABLE_EXTENSIONS_ANGLE, from ANGLE's own gl2ext_angle.h.
    constexpr GLenum kRequestableExtensions = 0x93A8;
    const auto* available =
        reinterpret_cast<const char*>(fns.glGetString_(kRequestableExtensions));
    if (available == nullptr) return;
    const std::string requestable(available);

    // Block compression, in the order the engine prefers it. BPTC is
    // BC6H/BC7, RGTC is BC4/BC5, S3TC is BC1/BC2/BC3, between them they
    // cover every format the engine asks about.
    static const char* const kWanted[] = {
        "GL_EXT_texture_compression_s3tc",     "GL_EXT_texture_compression_dxt1",
        "GL_ANGLE_texture_compression_dxt3",   "GL_ANGLE_texture_compression_dxt5",
        "GL_EXT_texture_compression_rgtc",     "GL_EXT_texture_compression_bptc",
        "GL_EXT_texture_compression_s3tc_srgb",
    };
    std::string granted;
    for (const char* name : kWanted) {
        if (requestable.find(name) == std::string::npos) continue;
        request(name);
        if (!granted.empty()) granted += " ";
        granted += name;
    }
    if (granted.empty()) {
        std::printf("stud-render-host: no requestable texture-compression extensions\n");
    } else {
        std::printf("stud-render-host: enabled %s\n", granted.c_str());
    }
    std::fflush(stdout);
}


// Says, once per call id, that a call the client will never hear back
// about failed.
//
// A reply-free request has no channel to report anything on: the client
// queued it and moved on. That is the right trade for calls whose result
// nothing reads, but it means the host is the only place a real failure
// can still be noticed, and a failure nobody can see is exactly the kind
// of thing that later looks like an engine bug. Once per id, because a
// call that fails usually fails every frame and the useful fact is which
// one, not how many times.
void report_reply_free_failure(stud::render_host::CallId id, uint64_t result) {
    if (static_cast<int32_t>(result) == 0) return;  // VK_SUCCESS
    static std::mutex m;
    static std::set<int> said;
    std::lock_guard<std::mutex> lock(m);
    if (!said.insert(static_cast<int>(id)).second) return;
    std::printf("stud-render-host: %s returned %d, and it was sent reply-free, so the client "
                "cannot see this\n",
                stud::render_host::call_id_name(id), static_cast<int32_t>(result));
    std::fflush(stdout);
}

// STUD_RENDER_CALL_TRACE: names the draw/clear calls, the swap counter
// and the shader info logs. Read once, from the three places that trace.
bool render_call_trace_enabled() {
    static const bool on = std::getenv("STUD_RENDER_CALL_TRACE") != nullptr;
    return on;
}

// STUD_WL_POLL_MS: how long a poll may block before Wayland is pumped
// again. The Vulkan driver reads the display fd itself, so Stud's own
// queue often has events waiting while the fd never becomes readable --
// which makes this timeout, not the fd, what decides how often buffer
// releases get dispatched. Both poll loops take the same answer.
int wayland_poll_ms() {
    static const int ms = [] {
        const char* v = std::getenv("STUD_WL_POLL_MS");
        const int n = v != nullptr ? std::atoi(v) : 0;
        return n > 0 ? n : 50;
    }();
    return ms;
}

// The calls that are not graphics: secrets, the web view, deep links, the
// clipboard, audio input, rumble, refresh rates, the text overlay and
// input polling.
//
// Split out of dispatch(), which was 2331 lines and is the function every
// call from both other processes passes through. These 33 cases sat in the
// middle of it and share nothing with the GL, EGL and Vulkan cases around
// them -- no `fns`, no pixel-buffer resolution, no draw tracing -- so they
// come out whole.
//
// std::optional rather than a bool-and-out-parameter so that every case
// body below is EXACTLY what it was inside the switch: a `return <value>;`
// converts to the optional on its own. Nothing in the bodies was touched
// by the move, which is the only reason a split of this size is reviewable.
// An id that is not one of these returns nullopt and dispatch() carries on
// to its own switch.
std::optional<uint64_t> dispatch_platform_call(const Header& hdr, RealWindow& window,
                                               const std::vector<uint8_t>& in,
                                               std::vector<uint8_t>& out, uint32_t* out_len) {
    const uint64_t* a = hdr.args;
    switch (hdr.call_id) {
        case CallId::StoreSecret: {
            // "<name>\n<value>". The name is a plain slug; the value is
            // a real credential and is never logged, not even a prefix.
            const std::string payload(reinterpret_cast<const char*>(in.data()), in.size());
            const auto newline = payload.find('\n');
            if (newline == std::string::npos) return 0;
            const std::string name = payload.substr(0, newline);
            std::string value = payload.substr(newline + 1);
            if (name.empty() || value.empty()) return 0;
            const size_t value_bytes = value.size();
            value.push_back('\n');
            const bool ok = run_ui_secret_helper("--store-secret", name, value, nullptr);
            std::printf("stud-render-host: handed secret \"%s\" to the keyring helper "
                        "(%zu bytes)%s\n",
                        name.c_str(), value_bytes, ok ? "" : ", could not start it");
            std::fflush(stdout);
            return ok ? 1 : 0;
        }
        case CallId::DeleteSecret: {
            // The name only; there is no value to carry and nothing here
            // ever sees one.
            const std::string name(reinterpret_cast<const char*>(in.data()), in.size());
            if (name.empty()) return 0;
            const bool ok =
                run_ui_secret_helper("--delete-secret", name, std::string(), nullptr);
            std::printf("stud-render-host: forgot secret \"%s\"%s\n", name.c_str(),
                        ok ? "" : " (the keyring helper reported a failure)");
            std::fflush(stdout);
            return ok ? 1 : 0;
        }
        case CallId::LoadSecret: {
            const std::string name(reinterpret_cast<const char*>(in.data()), in.size());
            if (name.empty()) return 0;
            std::string value;
            const bool ok = run_ui_secret_helper("--load-secret", name, std::string(), &value);
            while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
                value.pop_back();
            }
            std::printf("stud-render-host: read secret \"%s\" from safe storage: %s (%zu bytes, "
                        "helper ok=%d)\n",
                        name.c_str(), value.empty() ? "absent" : "present", value.size(),
                        static_cast<int>(ok));
            std::fflush(stdout);
            if (value.empty()) return 0;
            const uint32_t n =
                static_cast<uint32_t>(std::min<size_t>(value.size(), hdr.out_buffer_len));
            out.resize(n);
            std::memcpy(out.data(), value.data(), n);
            *out_len = n;
            return 1;
        }
        case CallId::OpenWebView: {
            // Payload: url \n title \n cookie..., handed to the viewer
            // on its stdin so no credential is ever visible in a command
            // line or an environment block.
            std::string payload(reinterpret_cast<const char*>(in.data()), in.size());
            const auto first_newline = payload.find('\n');
            const std::string url = payload.substr(0, first_newline);

            // args[0] != 0 means the caller already knows this belongs to
            // the desktop, not the viewer: it came from the LINKING
            // protocol (GuiService:OpenBrowserWindow), not the web-view
            // one. Stud does not have to guess from the URL, and must
            // not, since blog.roblox.com is a panel while a corp.roblox.com
            // careers page is not.
            if (a[0] != 0) {
                const bool studio = url.rfind("roblox-studio:", 0) == 0;
                const bool http = url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
                if (!http && !studio) {
                    std::printf("stud-render-host: refusing to open a URL with an unexpected "
                                "scheme\n");
                    std::fflush(stdout);
                    return 0;
                }
                // Passed as one argv entry, never through a shell: a URL
                // from the engine is data, not a command line.
                //
                // NO setsid() here, deliberately. Detaching the child from
                // this session makes the compositor treat the browser as
                // an unrelated background launch, so the window opens
                // behind Stud instead of coming to the front, exactly
                // what the user saw. Keeping the session lets the
                // activation token propagate and the browser raise itself.
                // Ask the compositor for an activation token first. A
                // Wayland compositor will not let a process raise its own
                // window unencouraged; that is focus-stealing
                // prevention, so a browser launched without one opens
                // BEHIND Stud. The token, minted against Stud's own
                // surface, is how a launcher says the user asked for
                // this. Both variable names are set because which one a
                // program reads depends on its toolkit.
                const std::string token =
                    stud::android_glue::native_window_activation_token(g_real_window);
                const pid_t pid = ::fork();
                if (pid == 0) {
                    // Prefer the desktop portal, which takes the
                    // activation token as an explicit ARGUMENT.
                    //
                    // Setting XDG_ACTIVATION_TOKEN in the environment and
                    // calling xdg-open was tried and does not work: the
                    // token really is minted (logged), but xdg-open hands
                    // the URL on through a .desktop entry or D-Bus and
                    // the environment does not survive that hop, so the
                    // browser never sees it and opens unfocused.
                    //
                    // The URL is still a separate argv entry, never
                    // interpolated into a shell command.
                    if (!token.empty()) {
                        ::setenv("XDG_ACTIVATION_TOKEN", token.c_str(), 1);
                        ::setenv("DESKTOP_STARTUP_ID", token.c_str(), 1);
                        const std::string options =
                            "{'activation_token': <'" + token + "'>}";
                        ::execlp("gdbus", "gdbus", "call", "--session", "--dest",
                                 "org.freedesktop.portal.Desktop", "--object-path",
                                 "/org/freedesktop/portal/desktop", "--method",
                                 "org.freedesktop.portal.OpenURI.OpenURI", "", url.c_str(),
                                 options.c_str(), nullptr);
                        // Only reached if gdbus is missing entirely.
                    }
                    ::execlp("xdg-open", "xdg-open", url.c_str(), nullptr);
                    ::_exit(127);
                }
                if (pid > 0) {
                    std::thread([pid] { int st = 0; ::waitpid(pid, &st, 0); }).detach();
                }
                std::printf("stud-render-host: handed %s link to the desktop (activation token: "
                            "%s)\n",
                            studio ? "a Roblox Studio" : "an external",
                            token.empty() ? "none. It will open unfocused" : "yes");
                std::fflush(stdout);
                return 1;
            }

            // Everything the WebView protocol asks for opens in the
            // viewer, including blog.roblox.com, user-confirmed as the
            // right behaviour, and it is what a device does too.
            //
            // A domain test was tried and removed: it classified the
            // Newsroom as external because blog.roblox.com is not the
            // main site, which was wrong. The app's own `windowType`
            // field cannot decide it either, live-captured as EMPTY for
            // both an in-app panel (Messages) and the blog. So this
            // protocol carries no signal saying "hand this to the
            // system", and anything that really needs a browser must
            // arrive by some other route.
            //
            // Non-http URLs are still refused: this path spawns a viewer
            // holding real session cookies, and a scheme it cannot render
            // has no business here.
            if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
                std::printf("stud-render-host: refusing a non-http web-view URL\n");
                std::fflush(stdout);
                return 0;
            }
            int pipe_fds[2] = {-1, -1};
            if (::pipe(pipe_fds) != 0) return 0;
            // A second pipe, the other way: the page's JavaScript bridge
            // answers through the viewer's stdout (see
            // read_web_view_output), which is how a login challenge
            // reports that it is done.
            int out_fds[2] = {-1, -1};
            if (::pipe(out_fds) != 0) {
                ::close(pipe_fds[0]);
                ::close(pipe_fds[1]);
                return 0;
            }
            const pid_t pid = ::fork();
            if (pid == 0) {
                ::close(pipe_fds[1]);
                ::close(out_fds[0]);
                ::dup2(pipe_fds[0], STDIN_FILENO);
                ::dup2(out_fds[1], STDOUT_FILENO);
                // The viewer's stderr goes the same way as its stdout, so
                // whatever the page reports lands in Stud's own session
                // log. Qt otherwise routes its logging through journald
                // on this desktop, where a failing challenge page was
                // invisible in the one file a user can be asked for.
                ::dup2(out_fds[1], STDERR_FILENO);
                ::setenv("QT_FORCE_STDERR_LOGGING", "1", 1);
                // MangoHud belongs to the game window, never to a web
                // view. This process carries its environment (the layer,
                // and on the OpenGL paths its dlopen/dlsym shim on
                // LD_PRELOAD), and a child inherits all of it, which
                // put MangoHud inside QtWebEngine, where it crashes the
                // viewer. Live-reported: a panel that dies on open with
                // the overlay enabled.
                //
                // Stripped rather than worked around: an overlay on a
                // message list is not a thing anyone asked for, so there
                // is nothing here to preserve.
                ::unsetenv("MANGOHUD");
                ::unsetenv("MANGOHUD_CONFIG");
                ::unsetenv("MANGOHUD_CONFIGFILE");
                ::unsetenv("MANGOHUD_DLSYM");
                if (const char* preload = ::getenv("LD_PRELOAD");
                    preload != nullptr && *preload != '\0') {
                    // Keep whatever else the user preloads; drop only
                    // MangoHud's own libraries. The separator is a colon
                    // or a space, both of which the loader accepts.
                    std::string kept;
                    const std::string value(preload);
                    size_t start = 0;
                    while (start <= value.size()) {
                        const size_t end = value.find_first_of(": ", start);
                        const std::string entry =
                            value.substr(start, end == std::string::npos ? std::string::npos
                                                                         : end - start);
                        if (!entry.empty() && entry.find("angoHud") == std::string::npos &&
                            entry.find("angohud") == std::string::npos) {
                            if (!kept.empty()) kept += ':';
                            kept += entry;
                        }
                        if (end == std::string::npos) break;
                        start = end + 1;
                    }
                    if (kept.empty()) {
                        ::unsetenv("LD_PRELOAD");
                    } else {
                        ::setenv("LD_PRELOAD", kept.c_str(), 1);
                    }
                }
                // And the Vulkan layer, for a viewer that reaches a real
                // driver through QtWebEngine's own GPU process.
                ::setenv("VK_LOADER_LAYERS_DISABLE", "VK_LAYER_MANGOHUD_overlay_*", 1);
                ::close(pipe_fds[0]);
                ::close(out_fds[1]);
                ::setsid();
                // Next to this binary first, so a build tree runs its own
                // viewer rather than one that happens to be installed.
                std::string own_dir;
                {
                    char buf[4096];
                    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
                    if (n > 0) {
                        buf[n] = '\0';
                        const std::string exe(buf);
                        const auto slash = exe.find_last_of('/');
                        if (slash != std::string::npos) own_dir = exe.substr(0, slash);
                    }
                }
                if (!own_dir.empty()) {
                    // Build tree first, then an install tree, where the
                    // viewer is this binary's own neighbour.
                    const std::string sibling = own_dir + "/../webview/stud-webview";
                    ::execl(sibling.c_str(), "stud-webview", nullptr);
                    const std::string installed = own_dir + "/stud-webview";
                    ::execl(installed.c_str(), "stud-webview", nullptr);
                }
                ::execlp("stud-webview", "stud-webview", nullptr);
                ::_exit(127);
            }
            ::close(pipe_fds[0]);
            ::close(out_fds[1]);
            if (pid < 0) {
                ::close(pipe_fds[1]);
                ::close(out_fds[0]);
                return 0;
            }
            std::thread(read_web_view_output, out_fds[0]).detach();
            // The sentinel ends the request; the pipe itself stays open,
            // because a blocked navigation is answered down it later.
            // Without the sentinel the viewer would read its cookies
            // until EOF, which now never comes.
            std::string request = payload;
            if (request.empty() || request.back() != '\n') request.push_back('\n');
            request += "END\n";
            const char* bytes = request.data();
            size_t left = request.size();
            while (left > 0) {
                const ssize_t written = ::write(pipe_fds[1], bytes, left);
                if (written <= 0) break;
                bytes += written;
                left -= static_cast<size_t>(written);
            }
            {
                const int previous = g_webview_stdin.exchange(pipe_fds[1]);
                if (previous >= 0) ::close(previous);
            }
            track_web_view(pid);
            std::thread([pid] {
                int status = 0;
                ::waitpid(pid, &status, 0);
                // Reaped, so it is no longer ours to kill, and the pid
                // must not be signalled again once the kernel is free to
                // reuse it.
                forget_web_view(pid);
                {
                    const int fd = g_webview_stdin.exchange(-1);
                    if (fd >= 0) ::close(fd);
                }
                g_webview_closed.fetch_add(1, std::memory_order_relaxed);
                std::printf("stud-render-host: web-view window closed\n");
                std::fflush(stdout);
            }).detach();
            std::printf("stud-render-host: opened the web-view panel\n");
            std::fflush(stdout);
            return 1;
        }
        case CallId::PollWebViewMessage: {
            std::string message;
            {
                std::lock_guard<std::mutex> lock(g_webview_message_mutex);
                if (g_webview_messages.empty()) return 0;
                message = std::move(g_webview_messages.front());
                g_webview_messages.pop_front();
            }
            // The out-buffer is the handler's to size, same as every
            // other call that answers with bytes.
            out.assign(message.begin(), message.end());
            *out_len = static_cast<uint32_t>(out.size());
            return out.size();
        }
        case CallId::DeliverDeepLink: {
            std::string uri(reinterpret_cast<const char*>(in.data()), in.size());
            if (uri.empty()) return 0;
            // Read before the string is moved into the queue below,
            // reading it after reported 0 bytes every time, which is what
            // a moved-from string is.
            const size_t payload_bytes = uri.size();
            const bool has_token = uri.find("activationToken=") != std::string::npos;
            {
                std::lock_guard<std::mutex> lock(g_deep_link_mutex);
                // Two links queued at once means the first was never
                // acted on; keep the newest rather than replaying a stale
                // one at whatever the person just asked for.
                while (g_deep_links.size() >= 4) g_deep_links.pop_front();
                g_deep_links.push_back(std::move(uri));
            }
            // Raise the window, if the launch carried a token to do it
            // with. Stud may well be minimised or behind the browser the
            // link was clicked in, joining a game into a window nobody
            // can see is not much of an answer.
            //
            // Handled here rather than in Process B because this process
            // owns the Wayland surface; the token is consumed and does
            // not travel on with the rest of the payload.
            {
                const std::string key = "activationToken=";
                // Said out loud, because the second stud-ui's own stderr
                // goes nowhere. It has no terminal and is not teed into
                // the session log, so a missing token was invisible
                // from both ends.
                std::printf("stud-render-host: deep link payload %zu bytes, activation token %s\n",
                            payload_bytes,
                            has_token ? "present" : "ABSENT");
                std::fflush(stdout);
                size_t at = 0;
                bool raised = false;
                while (at < uri.size()) {
                    size_t end = uri.find('\n', at);
                    if (end == std::string::npos) end = uri.size();
                    if (uri.compare(at, key.size(), key) == 0) {
                        raised = true;
                        const std::string token = uri.substr(at + key.size(), end - at - key.size());
                        // Same window the activation-token getter above
                        // uses; the one real surface this process owns.
                        stud::android_glue::native_window_activate(g_real_window, token.c_str());
                        std::printf("stud-render-host: raising the window for the new link\n");
                        break;
                    }
                    at = end + 1;
                }
                if (!raised) {
                    // No token from the launcher. Mint one against this
                    // window and spend it here.
                    //
                    // Self-activation, and deliberately so: the same call
                    // the outgoing path above uses attaches the serial of
                    // a real input event on this seat, which is the thing
                    // a compositor actually checks. It is the only
                    // in-protocol way to come forward when whatever
                    // launched the link passed nothing, and a launcher
                    // that passes nothing is common, since the token only
                    // exists if the entry asked for startup notification
                    // AND the launcher honoured it.
                    //
                    // If the compositor declines, nothing happens and the
                    // window stays put, which is the same outcome as
                    // doing nothing at all.
                    const std::string own =
                        stud::android_glue::native_window_activation_token(g_real_window);
                    if (!own.empty()) {
                        stud::android_glue::native_window_activate(g_real_window, own.c_str());
                        std::printf("stud-render-host: no token given, raising with our own\n");
                    } else {
                        std::printf("stud-render-host: could not mint an activation token\n");
                    }
                    // Last resort, and compositor-specific on purpose.
                    //
                    // xdg-activation is the only way a Wayland client can
                    // be raised, and both of its paths are exhausted by
                    // this point: measured on a real KDE session, the
                    // launcher passes NO token to a URL handler even with
                    // StartupNotify set, and a token Stud mints for
                    // itself is refused, correctly, because the
                    // serial it carries is from Stud's last input, which
                    // is stale when the user was clicking in a browser.
                    // That refusal is the protocol working as designed.
                    //
                    // So this asks the compositor directly, through
                    // KWin's own scripting interface. It is NOT a general
                    // way to raise a window and must not be copied as
                    // one: it works on KDE and is a silent no-op
                    // everywhere else, where the gdbus call simply finds
                    // no org.kde.KWin to talk to.
                    raise_through_kwin();
                    std::fflush(stdout);
                }
            }
            // The URI itself is never logged: a deep link carries a
            // one-time join ticket.
            std::printf("stud-render-host: a second launch handed over a deep link\n");
            std::fflush(stdout);
            return 1;
        }
        case CallId::PollDeepLink: {
            std::string uri;
            {
                std::lock_guard<std::mutex> lock(g_deep_link_mutex);
                if (g_deep_links.empty()) return 0;
                uri = std::move(g_deep_links.front());
                g_deep_links.pop_front();
            }
            out.assign(uri.begin(), uri.end());
            *out_len = static_cast<uint32_t>(out.size());
            return out.size();
        }
        case CallId::PollWebViewNavigation: {
            std::string url;
            {
                std::lock_guard<std::mutex> lock(g_webview_navigation_mutex);
                if (g_webview_navigations.empty()) return 0;
                url = std::move(g_webview_navigations.front());
                g_webview_navigations.pop_front();
            }
            out.assign(url.begin(), url.end());
            *out_len = static_cast<uint32_t>(out.size());
            return out.size();
        }
        case CallId::WebViewLoadUrl: {
            const int fd = g_webview_stdin.load();
            if (fd < 0 || in.empty()) return 0;
            std::string line = (a[0] != 0 ? std::string("drop ") : std::string("load ")) +
                               std::string(reinterpret_cast<const char*>(in.data()), in.size()) +
                               "\n";
            size_t left = line.size();
            const char* bytes = line.data();
            while (left > 0) {
                const ssize_t written = ::write(fd, bytes, left);
                if (written <= 0) return 0;
                bytes += written;
                left -= static_cast<size_t>(written);
            }
            return 1;
        }
        case CallId::CanWarpPointer:
            return stud::android_glue::native_window_can_warp_pointer() ? 1 : 0;
        case CallId::WarpPointer:
            stud::android_glue::native_window_warp_pointer(
                g_real_window, static_cast<float>(a[0]) / 256.0f,
                static_cast<float>(a[1]) / 256.0f);
            return 0;
        case CallId::SetPointerConfined:
            stud::android_glue::native_window_set_pointer_confined(g_real_window, a[0] != 0);
            return 0;
        case CallId::CopyToClipboard: {
            if (in.empty()) return 0;
            const std::string text(reinterpret_cast<const char*>(in.data()), in.size());
            // Through a helper that keeps serving the selection after
            // this returns.
            //
            // A clipboard is not a store: whoever puts something on it
            // owns it and has to hand it over when a paste asks, so a
            // process that sets it and exits copies nothing. This one
            // cannot hold it either, on Wayland an offer needs the
            // serial of a real input event and this process has no
            // clipboard plumbing at all, so wl-copy (Wayland) and
            // xclip/xsel (X11) do it, each of which forks and stays.
            //
            // The text is never logged: an invite link carries a
            // one-time code.
            static const char* const kWayland[] = {"wl-copy", nullptr};
            static const char* const kXclip[] = {"xclip", "-selection", "clipboard", nullptr};
            static const char* const kXsel[] = {"xsel", "--input", "--clipboard", nullptr};
            const bool wayland = ::getenv("WAYLAND_DISPLAY") != nullptr;
            const char* const* candidates[3] = {};
            if (wayland) {
                candidates[0] = kWayland;
                candidates[1] = kXclip;
                candidates[2] = kXsel;
            } else {
                candidates[0] = kXclip;
                candidates[1] = kXsel;
                candidates[2] = kWayland;
            }
            for (const char* const* argv_template : candidates) {
                if (argv_template == nullptr) continue;
                int fds[2] = {-1, -1};
                if (::pipe(fds) != 0) return 0;
                const pid_t pid = ::fork();
                if (pid == 0) {
                    ::close(fds[1]);
                    ::dup2(fds[0], STDIN_FILENO);
                    ::close(fds[0]);
                    // Its own session: the helper outlives this call and
                    // must not die with the game window.
                    ::setsid();
                    char* argv[8] = {};
                    size_t n = 0;
                    for (; argv_template[n] != nullptr && n < 7; ++n) {
                        argv[n] = const_cast<char*>(argv_template[n]);
                    }
                    ::execvp(argv[0], argv);
                    ::_exit(127);
                }
                ::close(fds[0]);
                if (pid < 0) {
                    ::close(fds[1]);
                    return 0;
                }
                size_t left = text.size();
                const char* bytes = text.data();
                while (left > 0) {
                    const ssize_t written = ::write(fds[1], bytes, left);
                    if (written <= 0) break;
                    bytes += written;
                    left -= static_cast<size_t>(written);
                }
                ::close(fds[1]);
                // wl-copy and xclip both fork a server and the parent
                // exits, so this wait is short and tells us whether the
                // tool was there at all.
                int status = 0;
                ::waitpid(pid, &status, 0);
                const bool ran = WIFEXITED(status) && WEXITSTATUS(status) != 127;
                if (ran) {
                    std::printf("stud-render-host: copied %zu bytes to the clipboard via %s\n",
                                text.size(), argv_template[0]);
                    std::fflush(stdout);
                    return 1;
                }
            }
            std::printf("stud-render-host: nothing to copy with, install wl-clipboard "
                        "(Wayland) or xclip (X11)\n");
            std::fflush(stdout);
            return 0;
        }
        case CallId::CloseWebView: {
            // The app asked, so the viewer's exit is not a user closing
            // the panel, but reporting it either way is what a real
            // device does (its own activity publishes windowClosed from
            // onDestroy however it was closed), and the app ignores the
            // echo of a close it requested itself.
            close_open_web_views();
            return 1;
        }
        case CallId::AudioOpenInputStream:
            return stud::render_host::audio_open_input_stream(static_cast<int>(a[0]),
                                                              static_cast<int>(a[1]));
        case CallId::AudioReadFrames: {
            out.resize(hdr.out_buffer_len);
            const uint64_t written =
                stud::render_host::audio_read_frames(out.data(), out.size());
            out.resize(static_cast<size_t>(written));
            *out_len = static_cast<uint32_t>(written);
            return written;
        }
        case CallId::AudioCloseInputStream:
            stud::render_host::audio_close_input_stream();
            return 1;
        case CallId::SetGamepadRumble: {
            // Magnitudes arrive in 1/1000ths: the header carries ints.
            return stud::render_host::gamepad::set_rumble(
                       static_cast<int>(a[0]), static_cast<float>(a[1]) / 1000.0f,
                       static_cast<float>(a[2]) / 1000.0f, static_cast<int>(a[3]))
                       ? 1
                       : 0;
        }
        case CallId::PollWindowCloseRequested:
            return stud::android_glue::window_close_requested() ? 1u : 0u;
        case CallId::PollWebViewClosed: {
            uint32_t pending = g_webview_closed.exchange(0, std::memory_order_relaxed);
            return pending;
        }
        case CallId::GetDisplayRefreshRate:
            return static_cast<uint64_t>(stud::android_glue::display_refresh_mhz());
        case CallId::GetSupportedRefreshRates: {
            const auto rates = stud::android_glue::display_supported_refresh_mhz();
            out.resize(rates.size() * sizeof(uint32_t));
            for (std::size_t i = 0; i < rates.size(); ++i) {
                const auto value = static_cast<uint32_t>(rates[i]);
                std::memcpy(out.data() + i * sizeof(uint32_t), &value, sizeof(value));
            }
            *out_len = static_cast<uint32_t>(out.size());
            return rates.size();
        }
        case CallId::SetTextOverlay: {
            // The engine's own TextBox, standing in for the Android
            // EditText a real device would lay over the GL view. See
            // stud/text_overlay.h.
            stud::android_glue::TextOverlaySpec spec;
            spec.visible = (a[0] & 1u) != 0;
            spec.password = (a[0] & 2u) != 0;
            auto unpack_float = [](uint64_t word, int half) {
                const auto bits = static_cast<uint32_t>(half == 0 ? (word & 0xffffffffu)
                                                                  : (word >> 32));
                float value = 0.0f;
                std::memcpy(&value, &bits, sizeof(value));
                return value;
            };
            spec.x = unpack_float(a[1], 0);
            spec.y = unpack_float(a[1], 1);
            spec.width = unpack_float(a[2], 0);
            spec.height = unpack_float(a[2], 1);
            float font_size = 0.0f;
            const auto font_bits = static_cast<uint32_t>(a[3] & 0xffffffffu);
            std::memcpy(&font_size, &font_bits, sizeof(font_size));
            const auto font_enum = static_cast<int32_t>(a[3] >> 32);
            spec.argb = static_cast<uint32_t>(a[4] & 0xffffffffu);
            spec.caret = static_cast<int32_t>(a[4] >> 32);
            spec.x_alignment = static_cast<int32_t>(a[5] & 0xffffffffu);
            spec.y_alignment = static_cast<int32_t>(a[5] >> 32);
            spec.selection_begin = static_cast<int32_t>(a[6] & 0xffffffffu);
            spec.selection_end = static_cast<int32_t>(a[6] >> 32);
            spec.text.assign(reinterpret_cast<const char*>(in.data()), in.size());
            const RobloxFont font = roblox_font_for(font_enum);
            spec.font_path = font.path;
            // Roblox's TextSize, turned into an em.
            //
            // `fromRbxFontRatio` is the font's own upem/(ascender -
            // descender), verified against the real files (Arimo
            // 0.895105, HWYGOTH 0.903342, PressStart2P 0.976168, each
            // matching the APK's mapping exactly). Multiplying by it
            // makes the LINE HEIGHT equal TextSize, which is what
            // Roblox's own documentation says TextSize means.
            //
            // Every font gets its own ratio from that mapping, legacy
            // enum and modern FontFace alike; see roblox_font_for. The
            // one case that did not -- FontFace, pinned at 1.0 -- is
            // exactly the one that drew the focused chat box larger than
            // the engine draws it unfocused.
            spec.pixel_size = font_size * font.ratio;
            // The line box stays Roblox's own TextSize whatever the em is.
            spec.line_height = font_size;
            spec.letter_spacing = font.letter_spacing;
            stud::android_glue::set_text_overlay(spec);
            return 1;
        }
        case CallId::SetClipboardText: {
            stud::android_glue::clipboard_set_text(
                std::string(reinterpret_cast<const char*>(in.data()), in.size()));
            return 1;
        }
        case CallId::GetClipboardText: {
            const std::string text = stud::android_glue::clipboard_get_text();
            out.assign(text.begin(), text.end());
            *out_len = static_cast<uint32_t>(out.size());
            return out.size();
        }
        case CallId::TextOverlayOffsetAtX:
            return static_cast<uint64_t>(
                stud::android_glue::text_overlay_offset_at_x(static_cast<float>(
                    static_cast<int32_t>(a[0]))));
        case CallId::GetWindowBufferScale:
            // Waits for the compositor's real fractional scale rather
            // than answering with the integer fallback. Process B asks
            // once, before the engine starts, and keeps the answer for
            // the whole session, so a wrong answer here is wrong
            // everywhere, permanently.
            return static_cast<uint64_t>(
                stud::android_glue::native_window_wait_for_display_scale_120());
        case CallId::GetDisplayOutputGeometry: {
            int32_t px_w = 0, px_h = 0, mm_w = 0, mm_h = 0;
            stud::android_glue::display_output_geometry(&px_w, &px_h, &mm_w, &mm_h);
            auto pack = [](int32_t v) -> uint64_t {
                if (v < 0) return 0;
                return static_cast<uint64_t>(v > 0xffff ? 0xffff : v);
            };
            return (pack(px_w) << 48) | (pack(px_h) << 32) | (pack(mm_w) << 16) | pack(mm_h);
        }
        case CallId::EndSession: {
            // Exits inside the handler, so nothing is written back. The
            // caller is quitting anyway and wants the window gone before
            // it starts its own teardown; see the CallId's own comment.
            std::printf("stud-render-host: the session ended, shutting down\n");
            close_open_web_views(/*wait_for_exit=*/true);
            exit_now(0);
        }
        case CallId::SetGamePresence: {
            // "<placeId> <jobId>", or empty for the app shell. The
            // metadata lookup (name, creator, thumbnail) needs HTTPS and
            // JSON, so it runs in stud-ui one-shot, the same helper
            // shape as the keyring and the region lookup.
            std::string body(reinterpret_cast<const char*>(in.data()), in.size());
            // The tray offers "copy server link" and is a separate
            // process, so the link is left where it can read it. Its
            // absence is what "not in a game" looks like from there.
            const std::string invite_path = []() {
                const char* xdg = std::getenv("XDG_RUNTIME_DIR");
                return std::string(xdg != nullptr ? xdg : "/tmp") + "/stud/invite";
            }();
            if (body.empty()) {
                std::printf("stud-render-host: Discord presence: the app shell\n");
                std::fflush(stdout);
                ::unlink(invite_path.c_str());
                stud::render_host::discord_rpc_set_game({});
                return 0;
            }
            std::string info;
            if (!run_ui_secret_helper("--game-info", body, std::string(), &info) || info.empty()) {
                return 0;
            }
            // One field per line, in a fixed order, so no JSON parser is
            // needed on this side: name, creator, thumbnail, join url.
            stud::render_host::GamePresence presence;
            std::string* fields[] = {&presence.universe_name, &presence.creator_name,
                                      &presence.thumbnail_url, &presence.join_url};
            size_t start = 0;
            for (size_t i = 0; i < 4 && start <= info.size(); ++i) {
                const size_t nl = info.find('\n', start);
                const size_t end = nl == std::string::npos ? info.size() : nl;
                *fields[i] = info.substr(start, end - start);
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
            // Written before the join-button setting is applied: the tray
            // entry is the user asking for the link explicitly, which is a
            // different question from putting it on a public presence.
            if (!presence.join_url.empty()) {
                if (FILE* f = std::fopen(invite_path.c_str(), "w")) {
                    std::fputs(presence.join_url.c_str(), f);
                    std::fclose(f);
                }
            }
            if (!g_discord_join_button) presence.join_url.clear();
            std::printf("stud-render-host: Discord presence: %s by %s%s\n",
                        presence.universe_name.c_str(), presence.creator_name.c_str(),
                        presence.join_url.empty() ? "" : " (with a join button)");
            std::fflush(stdout);
            presence.started_at = static_cast<int64_t>(::time(nullptr));
            stud::render_host::discord_rpc_set_game(presence);
            return 0;
        }
        case CallId::NotifyServerRegion: {
            // Fire and forget: a join must never wait on a lookup, and a
            // failed one simply produces no notification.
            std::string ip(reinterpret_cast<const char*>(in.data()), in.size());
            if (ip.empty()) return 0;
            std::printf("stud-render-host: looking up the region for the game server\n");
            std::fflush(stdout);
            run_ui_helper_detached("--notify-region", ip);
            return 0;
        }
        case CallId::SetPointerLocked:
            stud::android_glue::native_window_set_pointer_locked(g_real_window, a[0] != 0);
            return 0;
        case CallId::PollInputEvents: {
            // Real seat events queued by android-glue's own Wayland
            // listeners (this process already dispatches that fd in its
            // main poll loop). Reply with as many as the client's buffer
            // can hold; the rest stay queued for the next poll.
            // Pump the display HERE, first.
            //
            // A pointer event only reaches the queue when something
            // dispatches Wayland, and the main loop does that at most
            // every STUD_WL_POLL_MS (50ms), and not at all while a busy
            // client keeps the connection saturated, which is exactly
            // when the mouse is moving. The hand could therefore be up to
            // a twentieth of a second ahead of the queue before Process B
            // even asked. Pumping at the moment input is requested makes
            // the answer as fresh as the compositor has it.
            using stud::android_glue::HostInputEvent;
            size_t capacity = hdr.out_buffer_len / sizeof(HostInputEvent);
            if (capacity == 0) return 0;
            out.resize(capacity * sizeof(HostInputEvent));
            size_t n = 0;
            // Wait for one, if the caller said it may.
            //
            // A pointer event only enters the queue when something
            // dispatches Wayland, and the main loop does that on a
            // timeout (and not at all while a busy client keeps the
            // connection saturated, which is exactly when the mouse is
            // moving). So the pump happens HERE, and rather than answer
            // "nothing yet" and be asked again a few milliseconds later,
            // the reply waits on the compositor's own fd and leaves the
            // instant an event lands.
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(static_cast<int>(a[0]));
            const bool wayland = !window.on_x11() && window.display != nullptr;
            const int wl_fd = wayland ? wl_display_get_fd(window.display) : -1;
            for (;;) {
                // Whatever the driver's own reads already put on the
                // queue, first and for free.
                if (wayland) {
                    dispatch_input_queue(window.display, false);
                } else {
                    // Events only, and nothing else.
                    //
                    // This used to call the whole display pump, which on
                    // X11 also takes the lock the present path needs and
                    // re-checks the window size, every time round a loop
                    // that runs every few milliseconds. The Wayland side
                    // has always done only what this does: drain what has
                    // already arrived.
                    std::lock_guard<std::mutex> lock(wayland_mutex());
                    stud::android_glue::native_window_pump_x11_events_only();
                }
                n = stud::android_glue::native_window_drain_input_events(
                    reinterpret_cast<HostInputEvent*>(out.data()), capacity);
                if (n > 0) break;
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) break;
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
                int wait_ms = static_cast<int>(left.count());
                if (wait_ms <= 0) wait_ms = 1;
                if (!wayland) {
                    // The X connection has a socket like any other, so
                    // wait on it rather than sleeping in a loop: the same
                    // shape as the Wayland branch below, one wake when
                    // something actually arrives instead of a timer that
                    // fires whether or not anything did.
                    const int x_fd = stud::android_glue::native_window_x11_fd();
                    if (x_fd < 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
                        continue;
                    }
                    pollfd xp{x_fd, POLLIN, 0};
                    if (::poll(&xp, 1, wait_ms) > 0 && (xp.revents & POLLIN) != 0) {
                        std::lock_guard<std::mutex> lock(wayland_mutex());
                        stud::android_glue::native_window_pump_x11_events_only();
                    }
                    n = stud::android_glue::native_window_drain_input_events(
                        reinterpret_cast<HostInputEvent*>(out.data()), capacity);
                    if (n > 0) break;
                    continue;
                }
                // Asleep until the compositor actually says something,
                // one wake, and only if there is something to read.
                pollfd wl{wl_fd, POLLIN, 0};
                if (::poll(&wl, 1, wait_ms) > 0 && (wl.revents & POLLIN) != 0) {
                    dispatch_input_queue(window.display, true);
                    n = stud::android_glue::native_window_drain_input_events(
                        reinterpret_cast<HostInputEvent*>(out.data()), capacity);
                    if (n > 0) break;
                }
                // A controller is evdev, not the compositor, so it is not
                // what this waits on. It is drained below, at most one
                // wait late, which is no worse than the timer this
                // replaces.
            }

            // Controllers are read here rather than by android-glue: they
            // come from evdev, not from the compositor, and Process B's
            // sandbox has a synthetic /dev by design. They ride the same
            // queue so there is one path in and one drain.
            if (n < capacity) {
                static std::vector<stud::render_host::gamepad::Event> pad_events;
                pad_events.clear();
                stud::render_host::gamepad::poll(pad_events);
                auto* slots = reinterpret_cast<HostInputEvent*>(out.data());
                for (const auto& pad : pad_events) {
                    if (n >= capacity) break;  // the rest arrive next poll
                    HostInputEvent& slot = slots[n++];
                    slot = HostInputEvent{};
                    switch (pad.type) {
                        case stud::render_host::gamepad::Event::kConnect:
                            slot.type = HostInputEvent::kGamepadConnect;
                            break;
                        case stud::render_host::gamepad::Event::kDisconnect:
                            slot.type = HostInputEvent::kGamepadDisconnect;
                            break;
                        case stud::render_host::gamepad::Event::kButton:
                            slot.type = HostInputEvent::kGamepadButton;
                            break;
                        case stud::render_host::gamepad::Event::kSupportedKey:
                            slot.type = HostInputEvent::kGamepadSupportedKey;
                            break;
                        case stud::render_host::gamepad::Event::kSupportedAxis:
                            slot.type = HostInputEvent::kGamepadSupportedAxis;
                            break;
                        default:
                            slot.type = HostInputEvent::kGamepadAxis;
                            break;
                    }
                    slot.code = static_cast<uint32_t>(pad.code);
                    // An axis is a vector: all three floats travel. Every
                    // other kind uses the first one only.
                    slot.x = pad.v0;
                    slot.y = pad.v1;
                    slot.a = pad.type == stud::render_host::gamepad::Event::kAxis ? pad.v2
                                                                                  : pad.v0;
                    slot.b = static_cast<float>(pad.device_id);
                }
            }
            out.resize(n * sizeof(HostInputEvent));
            *out_len = static_cast<uint32_t>(out.size());
            return n;
        }
        default:
            return std::nullopt;
    }
}


uint64_t dispatch(const Header& hdr, const RealFns& fns, RealWindow& window,
                   const std::vector<uint8_t>& in, std::vector<uint8_t>& out, uint32_t* out_len) {
    const uint64_t* a = hdr.args;
    *out_len = 0;
    // Real pixel-source resolution: when a real GL_PIXEL_UNPACK_BUFFER is
    // bound on the client, `pixels` is a byte offset into it, and the client
    // sends no pixel bytes at all; see Header's own doc comment.
    auto pixels_ptr = [&]() -> const void* {
        if (hdr.pixel_buffer_offset_plus_one != 0) {
            return reinterpret_cast<const void*>(hdr.pixel_buffer_offset_plus_one - 1);
        }
        return in.empty() ? nullptr : in.data();
    };
    // Temporary diagnostic answering a real, concrete question: does
    // the engine ever issue a single real draw/clear call, or does it only
    // ever swap empty frames? STUD_RENDER_CALL_TRACE already exists for
    // EglSwapBuffers alone (see that case below), extended here to the
    // three calls that actually put pixels in a frame, since "the swap
    // loop runs" and "something real gets drawn" are two different real
    // facts and this project had only ever confirmed the first one.
    const bool draw_trace = render_call_trace_enabled();
    if (draw_trace && (hdr.call_id == CallId::GlClear || hdr.call_id == CallId::GlDrawArrays ||
                        hdr.call_id == CallId::GlDrawElements)) {
        std::printf("stud-render-host: real draw/clear call id=%d\n", static_cast<int>(hdr.call_id));
    }
    switch (hdr.call_id) {
        // ---- EGL ----
        case CallId::EglGetDisplay: {
            // Plain eglGetDisplay is the default and the path that has
            // rendered for this project's whole history. STUD_ANGLE_BACKEND
            // opts into asking ANGLE for a specific backend instead, which
            // needs eglGetPlatformDisplayEXT because the platform type is an
            // attribute of display creation and nothing else can express it
            // ANGLE_DEFAULT_PLATFORM was tried first and measured to have
            // no effect here, and "gl" does not mean desktop GL to it
            // anyway (it maps to native GLES).
            //
            // This was tried once before and reverted for producing a
            // display that initialised, reported success and rendered
            // nothing. That measurement was taken while the window-size
            // mismatch was making the engine rebuild its surface every
            // frame, so it is worth re-measuring, but only behind an
            // opt-in, and only trusting a frame dump, never a log line.
            EGLDisplay d = EGL_NO_DISPLAY;
            if (!g_angle_backend.empty() && fns.eglGetPlatformDisplayEXT_ != nullptr) {
                const std::string_view want(g_angle_backend);
                EGLint type = 0;
                EGLint device = 0;
                if (want == "gl") {
                    type = EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE;
                } else if (want == "gles") {
                    type = EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE;
                } else if (want == "vulkan") {
                    type = EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE;
                } else if (want == "swiftshader") {
                    type = EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE;
                    device = EGL_PLATFORM_ANGLE_DEVICE_TYPE_SWIFTSHADER_ANGLE;
                }
                if (type != 0) {
                    std::vector<EGLint> attribs{EGL_PLATFORM_ANGLE_TYPE_ANGLE, type};
                    if (device != 0) {
                        attribs.push_back(EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE);
                        attribs.push_back(device);
                    }
                    attribs.push_back(EGL_NONE);
                    d = fns.eglGetPlatformDisplayEXT_(EGL_PLATFORM_ANGLE_ANGLE,
                                                       static_cast<void*>(window.display),
                                                       attribs.data());
                    std::printf("stud-render-host: ANGLE backend \"%s\" -> %s\n",
                                g_angle_backend.c_str(),
                                d == EGL_NO_DISPLAY ? "refused, falling back" : "granted");
                    std::fflush(stdout);
                }
            }
            if (d == EGL_NO_DISPLAY) {
                d = fns.eglGetDisplay_(
                    reinterpret_cast<EGLNativeDisplayType>(window.egl_native_display()));
            }
            return d == EGL_NO_DISPLAY ? kNullHandle : store(g_displays, d);
        }
        case CallId::EglInitialize: {
            EGLint major = 0, minor = 0;
            return fns.eglInitialize_(g_displays.at(a[0]), &major, &minor) == EGL_TRUE;
        }
        case CallId::EglBindApi:
            return fns.eglBindAPI_(static_cast<EGLenum>(a[0])) == EGL_TRUE;
        case CallId::EglChooseConfig: {
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
                                              EGL_NONE};
            EGLConfig config;
            EGLint num_configs = 0;
            if (fns.eglChooseConfig_(g_displays.at(a[0]), config_attribs, &config, 1, &num_configs) !=
                    EGL_TRUE ||
                num_configs == 0) {
                std::fprintf(stderr, "stud-render-host: eglChooseConfig failed, error=0x%x\n",
                              fns.eglGetError_());
                return kNullHandle;
            }
            return store(g_configs, config);
        }
        case CallId::EglCreateWindowSurface: {
            // Testable hypothesis (the engineering notes, "no kde window
            // at all" investigation): stud_try_render_window (a standalone
            // tool doing the exact same real window+EGL setup sequence)
            // succeeds immediately at startup, while this same sequence in
            // stud-render-host only runs many real seconds later, after
            // Process B works through a long chain of 8s-bounded blocking
            // calls, during which this process's own Wayland connection
            // may sit without a fresh dispatch/roundtrip. A stale client-
            // side view of the connection (a compositor-sent event never
            // processed) is a real, plausible reason eglCreateWindowSurface
            // -> vkCreateWaylandSurfaceKHR could fail where the standalone
            // tool doesn't. Cheap, safe to always do regardless of outcome.
            {
                std::lock_guard<std::mutex> wl_lock(wayland_mutex());
                wl_display_roundtrip(window.display);
            }
            EGLSurface s = fns.eglCreateWindowSurface_(
                g_displays.at(a[0]), g_configs.at(a[1]),
                window.egl_native_window(), nullptr);
            if (s == EGL_NO_SURFACE) {
                std::fprintf(stderr,
                              "stud-render-host: eglCreateWindowSurface failed, error=0x%x\n",
                              fns.eglGetError_());
            } else {
                ++g_window_surface_create_count;
            }
            if (s == EGL_NO_SURFACE) return kNullHandle;
            // ANGLE can hand back the *same* EGLSurface for repeated creates
            // against the one native window this process owns (confirmed
            // live: both calls returned 0x1). Minting a fresh handle each
            // time would alias several handles onto one surface, the engine
            // then destroys an older handle, the shared surface dies, and
            // every later swap fails with EGL_BAD_SURFACE (0x300d) while the
            // engine keeps drawing into nothing. Deduplicate on the real
            // EGLSurface instead, which keeps destroy honest (see
            // EglDestroySurface below) rather than pinning one handle for the
            // life of the process.
            for (const auto& kv : g_surfaces) {
                if (kv.second == s) return kv.first;
            }
            uint64_t h = store(g_surfaces, s);
            g_window_surface_handle = h;
            return h;
        }
        case CallId::EglCreatePbufferSurface: {
            const EGLint attribs[] = {EGL_WIDTH, static_cast<EGLint>(a[1]), EGL_HEIGHT,
                                       static_cast<EGLint>(a[2]), EGL_NONE};
            EGLSurface s = fns.eglCreatePbufferSurface_(g_displays.at(a[0]), g_configs.at(a[1]),
                                                          attribs);
            return s == EGL_NO_SURFACE ? kNullHandle : store(g_surfaces, s);
        }
        case CallId::EglCreateContext: {
            const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
            EGLContext ctx = fns.eglCreateContext_(g_displays.at(a[0]), g_configs.at(a[1]),
                                                     EGL_NO_CONTEXT, context_attribs);
            return ctx == EGL_NO_CONTEXT ? kNullHandle : store(g_contexts, ctx);
        }
        case CallId::EglMakeCurrent: {
            EGLSurface s = a[1] == kNullHandle ? EGL_NO_SURFACE : g_surfaces.at(a[1]);
            EGLContext c = a[2] == kNullHandle ? EGL_NO_CONTEXT : g_contexts.at(a[2]);
            EGLBoolean ok = fns.eglMakeCurrent_(g_displays.at(a[0]), s, s, c);
            if (ok == EGL_TRUE && c != EGL_NO_CONTEXT) enable_requestable_extensions(fns);
            if (ok != EGL_TRUE) {
                std::printf("stud-render-host: eglMakeCurrent failed dpy=%llu surf=%llu ctx=%llu "
                            "egl_error=0x%x\n",
                            static_cast<unsigned long long>(a[0]),
                            static_cast<unsigned long long>(a[1]),
                            static_cast<unsigned long long>(a[2]),
                            fns.eglGetError_());
            }
            return ok == EGL_TRUE;
        }
        case CallId::EglSwapBuffers: {
            note_frame_pacing();
            // Shown on the first frame, like the Vulkan path, so the
            // window never appears empty. No-op on Wayland.
            stud::android_glue::x11_ensure_mapped();
            // The background frame limit is applied before the dispatch
            // lock is taken; see throttle_before_dispatch().
            // Real frame-rate measurement, env-gated (STUD_FPS=1, or
            // STUD_FPS=<seconds> for a different window). Counts swaps and
            // reports once per window, per-swap tracing
            // (STUD_RENDER_CALL_TRACE) is far too heavy to measure a real
            // in-game session with, since it prints for every draw call as
            // well and changes the thing being measured.
            {
                static const char* fps_env = std::getenv("STUD_FPS");
                if (fps_env != nullptr) {
                    static const double window_s =
                        std::atof(fps_env) > 0.0 ? std::atof(fps_env) : 5.0;
                    static auto window_start = std::chrono::steady_clock::now();
                    static uint64_t window_swaps = 0;
                    ++window_swaps;
                    const auto now = std::chrono::steady_clock::now();
                    const double elapsed =
                        std::chrono::duration<double>(now - window_start).count();
                    if (elapsed >= window_s) {
                        std::printf("stud-render-host: FPS %.1f (%llu swaps in %.1fs)\n",
                                    static_cast<double>(window_swaps) / elapsed,
                                    static_cast<unsigned long long>(window_swaps), elapsed);
                        std::fflush(stdout);
                        window_start = now;
                        window_swaps = 0;
                    }
                }
            }
            // Honest pixel check, env-gated and off by default: read the
            // default framebuffer back BEFORE the swap (after it the back
            // buffer is undefined). Draw counters and "the app reached Home"
            // both look healthy while the screen is black; this is the only
            // check that answers "did anything actually get drawn". It binds
            // nothing and changes no state; the read-back that once blacked
            // out the window bound an FBO, which this deliberately does not.
            {
                static const char* dump_every_env = std::getenv("STUD_DUMP_FRAME");
                static const int dump_every = dump_every_env ? std::atoi(dump_every_env) : 0;
                static uint64_t swap_seen = 0;
                if (dump_every > 0 && (++swap_seen % static_cast<uint64_t>(dump_every)) == 0) {
                    const int w = ANativeWindow_getWidth(nullptr);
                    const int h = ANativeWindow_getHeight(nullptr);
                    GLint fb_binding = -1, read_fb = -1, read_buf = -1;
                    // Must go through the resolved ANGLE entry points: the
                    // plain glXxx symbols this process links against are a
                    // different libGLESv2 with no context, so they silently
                    // do nothing (live-caught: every value stayed at its -1
                    // sentinel and glGetError() read 0).
                    fns.glGetIntegerv_(GL_FRAMEBUFFER_BINDING, &fb_binding);
                    fns.glGetIntegerv_(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
                    fns.glGetIntegerv_(GL_READ_BUFFER, &read_buf);
                    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4);
                    fns.glReadPixels_(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                    const char* path_base = std::getenv("STUD_DUMP_FRAME_PATH");
                    if (path_base == nullptr) path_base = "/tmp/stud_frame.ppm";
                    // Numbered, so consecutive frames do not overwrite each
                    // other: comparing one frame with the NEXT one is the
                    // whole point of dumping more than one, and a fixed
                    // name silently made that impossible.
                    std::string numbered(path_base);
                    {
                        const size_t dot = numbered.rfind('.');
                        char suffix[32];
                        std::snprintf(suffix, sizeof(suffix), "_%04llu",
                                      static_cast<unsigned long long>(swap_seen));
                        if (dot == std::string::npos) {
                            numbered += suffix;
                        } else {
                            numbered.insert(dot, suffix);
                        }
                    }
                    const char* path = numbered.c_str();
                    size_t nonblack = 0;
                    size_t white = 0;
                    for (size_t i = 0; i + 3 < px.size(); i += 4) {
                        if (px[i] || px[i + 1] || px[i + 2]) ++nonblack;
                        if (px[i] == 255 && px[i + 1] == 255 && px[i + 2] == 255) ++white;
                    }
                    if (FILE* f = std::fopen(path, "wb")) {
                        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
                        // glReadPixels' origin is bottom-left; PPM is top-down.
                        for (int y = h - 1; y >= 0; --y) {
                            for (int x = 0; x < w; ++x) {
                                const unsigned char* p = &px[(static_cast<size_t>(y) * w + x) * 4];
                                std::fwrite(p, 1, 3, f);
                            }
                        }
                        std::fclose(f);
                    }
                    std::printf("stud-render-host: DUMP frame #%llu %dx%d non-black=%zu of %zu "
                                "white=%zu drawFbo=%d readFbo=%d readBuf=0x%x glErr=0x%x -> %s\n",
                                static_cast<unsigned long long>(swap_seen), w, h, nonblack,
                                px.size() / 4, white, fb_binding, read_fb, read_buf,
                                fns.glGetError_(), path);
                    std::fflush(stdout);
                }
            }
            if (cursor_trace_enabled()) {
                std::printf("stud-render-host: CURSORTRACE frame draws=%llu 64x64draws=%llu %s\n",
                            static_cast<unsigned long long>(g_frame_draws),
                            static_cast<unsigned long long>(g_frame_cursor_draws),
                            g_frame_cursor_draws > 0 ? g_last_cursor_state.c_str() : "(no 64x64 draw)");
                std::fflush(stdout);
                ++g_cursor_trace_frames;
                g_frame_draws = 0;
                g_frame_cursor_draws = 0;
            }
            // Says whether the real EGL surface actually followed a window
            // resize. Distinguishes "ANGLE never resized the swapchain" from
            // "it did and the engine is still drawing at the old size", which
            // look identical on screen (a larger frame with the old image in
            // one corner and uncleared garbage in the rest).
            {
                static EGLint last_w = 0, last_h = 0;
                EGLint sw = 0, sh = 0;
                fns.eglQuerySurface_(g_displays.at(a[0]), g_surfaces.at(a[1]), EGL_WIDTH, &sw);
                fns.eglQuerySurface_(g_displays.at(a[0]), g_surfaces.at(a[1]), EGL_HEIGHT, &sh);
                if (sw != last_w || sh != last_h) {
                    last_w = sw;
                    last_h = sh;
                    std::printf("stud-render-host: EGL surface size now %dx%d (window %dx%d)\n", sw,
                                sh, ANativeWindow_getWidth(nullptr), ANativeWindow_getHeight(nullptr));
                    std::fflush(stdout);
                }
            }
            // The clock reads follow the switch, not just the print. Both
            // are cheap (clock_gettime is a vDSO call, not a syscall), but
            // measuring something nobody reads is still the wrong shape --
            // and this is the same conditional idiom the Vulkan client's
            // own frame timing uses.
            static const bool time_swap = std::getenv("STUD_TIME_SWAP") != nullptr;
            const auto t_swap_start = time_swap ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
            uint64_t r = fns.eglSwapBuffers_(g_displays.at(a[0]), g_surfaces.at(a[1])) == EGL_TRUE;
            const auto t_after_swap = time_swap ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
            if (r == 0) {
                // A failing swap presents nothing: the engine draws a full
                // frame and the window stays black. Report the real EGL error
                // once. It is the difference between "context lost" and a
                // plain bad-surface/bad-match.
                static bool told = false;
                if (!told) {
                    told = true;
                    std::printf("stud-render-host: eglSwapBuffers FAILED egl_error=0x%x dpy=%llu "
                                "surf=%llu\n",
                                fns.eglGetError_(), (unsigned long long)a[0],
                                (unsigned long long)a[1]);
                    std::fflush(stdout);
                }
            }
            {
                std::lock_guard<std::mutex> wl_lock(wayland_mutex());
                wl_display_roundtrip(window.display);
            }
            if (time_swap) {
                static auto last_frame = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                auto ms = [](auto d) {
                    return std::chrono::duration_cast<std::chrono::microseconds>(d).count() / 1000.0;
                };
                std::printf("stud-render-host: SWAPTIME egl=%.2fms roundtrip=%.2fms frame=%.2fms\n",
                            ms(t_after_swap - t_swap_start), ms(now - t_after_swap),
                            ms(now - last_frame));
                std::fflush(stdout);
                last_frame = now;
            }
            // Evidence-based confirmation of the first actual
            // rendered frame (~Stud plan, "Phase 5" verification: "confirm
            // the first frame via render-host's own dispatch log on
            // EglSwapBuffers"), not just a stdout claim from the bionic
            // side. Env-gated (same convention as
            // STUD_VULKAN_CALL_TRACE) since every real frame would
            // otherwise spam this.
            const bool trace = render_call_trace_enabled();
            static uint64_t frame_count = 0;
            if (trace) {
                ++frame_count;
                std::printf("stud-render-host: real EglSwapBuffers dispatched (frame #%llu, ok=%llu)\n",
                            static_cast<unsigned long long>(frame_count),
                            static_cast<unsigned long long>(r));
                std::fflush(stdout);
            }
            return r;
        }
        case CallId::EglGetError:
            return static_cast<uint64_t>(fns.eglGetError_());
        case CallId::EglQueryString: {
            const char* s = fns.eglQueryString_(g_displays.at(a[0]), static_cast<EGLint>(a[1]));
            if (s != nullptr) {
                size_t len = std::strlen(s);
                out.assign(s, s + len);
                *out_len = static_cast<uint32_t>(len);
            }
            return 1;
        }
        case CallId::EglDestroyContext:
            return fns.eglDestroyContext_(g_displays.at(a[0]), g_contexts.at(a[1])) == EGL_TRUE;
        case CallId::EglDestroySurface: {
            // Honour the destroy. The engine legitimately destroys and
            // recreates its window surface (a resize, a RenderView rebuild);
            // refusing meant the next create hit EGL_BAD_ALLOC because the
            // old surface still held the native window, and the engine
            // abandoned the frame. Create dedupes on the real EGLSurface, so
            // one handle only ever names one live surface.
            auto it = g_surfaces.find(a[1]);
            if (it == g_surfaces.end()) return EGL_TRUE;
            EGLBoolean ok = fns.eglDestroySurface_(g_displays.at(a[0]), it->second);
            g_surfaces.erase(it);
            if (a[1] == g_window_surface_handle) g_window_surface_handle = kNullHandle;
            return ok == EGL_TRUE;
        }
        case CallId::EglGetConfigAttrib: {
            EGLint value = 0;
            EGLBoolean ok = fns.eglGetConfigAttrib_(g_displays.at(a[0]), g_configs.at(a[1]),
                                                      static_cast<EGLint>(a[2]), &value);
            out.resize(sizeof(EGLint));
            std::memcpy(out.data(), &value, sizeof(EGLint));
            *out_len = sizeof(EGLint);
            return ok == EGL_TRUE;
        }
        case CallId::EglGetCurrentContext: {
            EGLContext c = fns.eglGetCurrentContext_();
            for (auto& [h, ctx] : g_contexts) {
                if (ctx == c) return h;
            }
            return kNullHandle;
        }
        case CallId::EglQuerySurface: {
            EGLint value = 0;
            EGLBoolean ok = fns.eglQuerySurface_(g_displays.at(a[0]), g_surfaces.at(a[1]),
                                                   static_cast<EGLint>(a[2]), &value);
            out.resize(sizeof(EGLint));
            std::memcpy(out.data(), &value, sizeof(EGLint));
            *out_len = sizeof(EGLint);
            return ok == EGL_TRUE;
        }
        case CallId::EglSwapInterval:
            return fns.eglSwapInterval_(g_displays.at(a[0]), static_cast<EGLint>(a[1])) == EGL_TRUE;
        case CallId::EglTerminate:
            return fns.eglTerminate_(g_displays.at(a[0])) == EGL_TRUE;
        case CallId::EglGetProcAddress: {
            // Real pointer, meaningful only to Process C itself.
            // Process B's own eglGetProcAddress stub does NOT hand this
            // raw value back to Roblox (a foreign-process function
            // pointer would be nonsense to call directly); it maps the
            // queried name to one of its own local generic forwarding
            // trampolines instead. This response exists only so Process
            // B's stub can confirm "yes, this name is real" (nonzero)
            // vs "not found" (zero).
            void* p = fns.eglGetProcAddress_(in.empty() ? "" : reinterpret_cast<const char*>(in.data()));
            return p != nullptr ? 1 : 0;
        }

        // ---- GLES2: scalar state ----
        case CallId::GlActiveTexture: fns.glActiveTexture_(static_cast<GLenum>(a[0])); return 0;
        case CallId::GlAttachShader: fns.glAttachShader_(static_cast<GLuint>(a[0]), static_cast<GLuint>(a[1])); return 0;
        case CallId::GlBindBuffer: fns.glBindBuffer_(static_cast<GLenum>(a[0]), static_cast<GLuint>(a[1])); return 0;
        case CallId::GlBindFramebuffer: fns.glBindFramebuffer_(static_cast<GLenum>(a[0]), static_cast<GLuint>(a[1])); return 0;
        case CallId::GlBindRenderbuffer: fns.glBindRenderbuffer_(static_cast<GLenum>(a[0]), static_cast<GLuint>(a[1])); return 0;
        case CallId::GlBindTexture:
            if (static_cast<GLenum>(a[0]) == GL_TEXTURE_2D) g_bound_texture_2d = static_cast<GLuint>(a[1]);
            fns.glBindTexture_(static_cast<GLenum>(a[0]), static_cast<GLuint>(a[1]));
            return 0;
        case CallId::GlBlendFunc: fns.glBlendFunc_(static_cast<GLenum>(a[0]), static_cast<GLenum>(a[1])); return 0;
        case CallId::GlBlendFuncSeparate: fns.glBlendFuncSeparate_(static_cast<GLenum>(a[0]), static_cast<GLenum>(a[1]), static_cast<GLenum>(a[2]), static_cast<GLenum>(a[3])); return 0;
                case CallId::GlCheckFramebufferStatus: return fns.glCheckFramebufferStatus_(static_cast<GLenum>(a[0]));
        case CallId::GlClear: fns.glClear_(static_cast<GLbitfield>(a[0])); return 0;
        case CallId::GlClearColor: fns.glClearColor_(unpack_float(a[0]), unpack_float(a[1]), unpack_float(a[2]), unpack_float(a[3])); return 0;
        case CallId::GlClearDepthf: fns.glClearDepthf_(unpack_float(a[0])); return 0;
        case CallId::GlClearStencil: fns.glClearStencil_(static_cast<GLint>(a[0])); return 0;
        case CallId::GlColorMask: fns.glColorMask_(static_cast<GLboolean>(a[0]), static_cast<GLboolean>(a[1]), static_cast<GLboolean>(a[2]), static_cast<GLboolean>(a[3])); return 0;
        case CallId::GlCompileShader: {
            GLuint sh = static_cast<GLuint>(a[0]);
            fns.glCompileShader_(sh);
            // STUD_DUMP_BAD_SHADERS=<dir>: write the source of any shader
            // that fails to compile, plus its info log. The engine's own
            // log only reports the name and the error line, which is not
            // enough to tell a genuinely invalid shader from one ANGLE is
            // rejecting more strictly than the drivers Roblox ships
            // against. Off unless the env var is set; writes nothing on
            // success.
            static const char* dump_dir = std::getenv("STUD_DUMP_BAD_SHADERS");
            if (dump_dir != nullptr) {
                GLint ok = GL_TRUE;
                fns.glGetShaderiv_(sh, GL_COMPILE_STATUS, &ok);
                if (ok == GL_FALSE) {
                    static int seq = 0;
                    char path[512];
                    std::snprintf(path, sizeof(path), "%s/bad_shader_%d.txt", dump_dir, seq++);
                    if (FILE* f = std::fopen(path, "w")) {
                        GLint srclen = 0;
                        fns.glGetShaderiv_(sh, GL_SHADER_SOURCE_LENGTH, &srclen);
                        if (srclen > 0) {
                            std::vector<char> buf(static_cast<size_t>(srclen));
                            GLsizei got = 0;
                            fns.glGetShaderSource_(sh, srclen, &got, buf.data());
                            std::fwrite(buf.data(), 1, static_cast<size_t>(got), f);
                        }
                        GLint loglen = 0;
                        fns.glGetShaderiv_(sh, GL_INFO_LOG_LENGTH, &loglen);
                        if (loglen > 0) {
                            std::vector<char> log(static_cast<size_t>(loglen));
                            GLsizei got = 0;
                            fns.glGetShaderInfoLog_(sh, loglen, &got, log.data());
                            std::fprintf(f, "\n\n===== INFO LOG =====\n");
                            std::fwrite(log.data(), 1, static_cast<size_t>(got), f);
                        }
                        std::fclose(f);
                        std::printf("stud-render-host: dumped failing shader to %s\n", path);
                        std::fflush(stdout);
                    }
                }
            }
            return 0;
        }
        case CallId::GlCopyTexSubImage2D: fns.glCopyTexSubImage2D_(static_cast<GLenum>(a[0]), static_cast<GLint>(a[1]), static_cast<GLint>(a[2]), static_cast<GLint>(a[3]), static_cast<GLint>(a[4]), static_cast<GLint>(a[5]), 0, 0); return 0;
        case CallId::GlCreateProgram: return fns.glCreateProgram_();
        case CallId::GlCreateShader: return fns.glCreateShader_(static_cast<GLenum>(a[0]));
        case CallId::GlCullFace: fns.glCullFace_(static_cast<GLenum>(a[0])); return 0;
        case CallId::GlDeleteProgram: fns.glDeleteProgram_(static_cast<GLuint>(a[0])); return 0;
        case CallId::GlDeleteShader: fns.glDeleteShader_(static_cast<GLuint>(a[0])); return 0;
        case CallId::GlDepthFunc: fns.glDepthFunc_(static_cast<GLenum>(a[0])); return 0;
        case CallId::GlDepthMask: fns.glDepthMask_(static_cast<GLboolean>(a[0])); return 0;
        case CallId::GlDisable: fns.glDisable_(static_cast<GLenum>(a[0])); return 0;
        case CallId::GlDisableVertexAttribArray: fns.glDisableVertexAttribArray_(static_cast<GLuint>(a[0])); return 0;
        case CallId::GlDrawArrays:
            note_draw_for_cursor_trace(fns, static_cast<GLsizei>(a[2]), 0, 0, static_cast<GLint>(a[1]));
            fns.glDrawArrays_(static_cast<GLenum>(a[0]), static_cast<GLint>(a[1]), static_cast<GLsizei>(a[2]));
            return 0;
        case CallId::GlDrawElements:
            // `indices` (a[3]) is a real VBO-relative byte OFFSET here,
            // not a client-side pointer; see glVertexAttribPointer's
            // own handling below for why that's the real, common case
            // this forwards correctly, and the documented limitation for
            // genuine client-side index arrays.
            note_draw_for_cursor_trace(fns, static_cast<GLsizei>(a[1]), static_cast<GLenum>(a[2]),
                                       static_cast<uintptr_t>(a[3]));
            fns.glDrawElements_(static_cast<GLenum>(a[0]), static_cast<GLsizei>(a[1]), static_cast<GLenum>(a[2]), reinterpret_cast<const void*>(a[3]));
            return 0;
        case CallId::GlEnable: fns.glEnable_(static_cast<GLenum>(a[0])); return 0;
        case CallId::GlEnableVertexAttribArray: fns.glEnableVertexAttribArray_(static_cast<GLuint>(a[0])); return 0;
        case CallId::GlFramebufferRenderbuffer: fns.glFramebufferRenderbuffer_(static_cast<GLenum>(a[0]), static_cast<GLenum>(a[1]), static_cast<GLenum>(a[2]), static_cast<GLuint>(a[3])); return 0;
        case CallId::GlFramebufferTexture2D: fns.glFramebufferTexture2D_(static_cast<GLenum>(a[0]), static_cast<GLenum>(a[1]), static_cast<GLenum>(a[2]), static_cast<GLuint>(a[3]), static_cast<GLint>(a[4])); return 0;
        case CallId::GlGenerateMipmap: fns.glGenerateMipmap_(static_cast<GLenum>(a[0])); return 0;
        case CallId::GlGetError: return fns.glGetError_();
        case CallId::GlLinkProgram: {
            fns.glLinkProgram_(static_cast<GLuint>(a[0]));
            // Real diagnostic, env-gated: libroblox's own FLog reports
            // "failed to link shader program X,Y," with no reason. The
            // real reason only exists here, in ANGLE's own info log.
            const bool trace = render_call_trace_enabled();
            if (trace) {
                GLint status = 0;
                fns.glGetProgramiv_(static_cast<GLuint>(a[0]), GL_LINK_STATUS, &status);
                if (status != GL_TRUE) {
                    char log[2048];
                    GLsizei len = 0;
                    fns.glGetProgramInfoLog_(static_cast<GLuint>(a[0]), sizeof(log), &len, log);
                    log[len < static_cast<GLsizei>(sizeof(log)) ? len : sizeof(log) - 1] = '\0';
                    std::printf("stud-render-host: shader program %llu failed to link: %s\n",
                                static_cast<unsigned long long>(a[0]), log);
                }
            }
            return 0;
        }
        case CallId::GlPixelStorei: fns.glPixelStorei_(static_cast<GLenum>(a[0]), static_cast<GLint>(a[1])); return 0;
        case CallId::GlPolygonOffset: fns.glPolygonOffset_(unpack_float(a[0]), unpack_float(a[1])); return 0;
        case CallId::GlReleaseShaderCompiler: fns.glReleaseShaderCompiler_(); return 0;
        case CallId::GlRenderbufferStorage: fns.glRenderbufferStorage_(static_cast<GLenum>(a[0]), static_cast<GLenum>(a[1]), static_cast<GLsizei>(a[2]), static_cast<GLsizei>(a[3])); return 0;
        case CallId::GlScissor: fns.glScissor_(static_cast<GLint>(a[0]), static_cast<GLint>(a[1]), static_cast<GLsizei>(a[2]), static_cast<GLsizei>(a[3])); return 0;
        case CallId::GlStencilFunc: fns.glStencilFunc_(static_cast<GLenum>(a[0]), static_cast<GLint>(a[1]), static_cast<GLuint>(a[2])); return 0;
        case CallId::GlStencilMask: fns.glStencilMask_(static_cast<GLuint>(a[0])); return 0;
        case CallId::GlStencilOp: fns.glStencilOp_(static_cast<GLenum>(a[0]), static_cast<GLenum>(a[1]), static_cast<GLenum>(a[2])); return 0;
        case CallId::GlTexParameterf: fns.glTexParameterf_(static_cast<GLenum>(a[0]), static_cast<GLenum>(a[1]), unpack_float(a[2])); return 0;
        case CallId::GlTexParameteri:
            trace_texture_parameter(static_cast<GLenum>(a[0]), static_cast<GLenum>(a[1]),
                                    static_cast<GLint>(a[2]));
            fns.glTexParameteri_(static_cast<GLenum>(a[0]), static_cast<GLenum>(a[1]), static_cast<GLint>(a[2])); return 0;
        case CallId::GlUniform1i: fns.glUniform1i_(static_cast<GLint>(a[0]), static_cast<GLint>(a[1])); return 0;
        case CallId::GlUseProgram: fns.glUseProgram_(static_cast<GLuint>(a[0])); return 0;
        case CallId::GlViewport: fns.glViewport_(static_cast<GLint>(a[0]), static_cast<GLint>(a[1]), static_cast<GLsizei>(a[2]), static_cast<GLsizei>(a[3])); return 0;
        case CallId::GlVertexAttribPointer:
            // `pointer` (a[5]) is a VBO-relative byte offset, correct
            // and sufficient for the overwhelmingly common real-world
            // case (a GL_ARRAY_BUFFER bound via glBindBuffer before this
            // call, per modern GLES usage). Genuine client-side vertex
            // arrays (a real CPU pointer, no VBO bound) are NOT
            // supported by this forwarding, would need the pointed-to
            // data copied into the outgoing buffer at every draw call
            // using it, not yet implemented since nothing has confirmed
            // Roblox actually relies on that (rare in modern engines;
            // flagged, checked at).
            fns.glVertexAttribPointer_(static_cast<GLuint>(a[0]), static_cast<GLint>(a[1]), static_cast<GLenum>(a[2]), static_cast<GLboolean>(a[3]), static_cast<GLsizei>(a[4]), reinterpret_cast<const void*>(a[5]));
            return 0;

        // ---- GLES2: string in/out ----
        case CallId::GlGetString: {
            const GLubyte* s = fns.glGetString_(static_cast<GLenum>(a[0]));
            if (s != nullptr) {
                size_t len = std::strlen(reinterpret_cast<const char*>(s));
                out.assign(s, s + len);
                *out_len = static_cast<uint32_t>(len);
            }
            return 1;
        }
        case CallId::GlGetUniformLocation: {
            std::string name(reinterpret_cast<const char*>(in.data()), in.size());
            return static_cast<uint64_t>(static_cast<uint32_t>(fns.glGetUniformLocation_(static_cast<GLuint>(a[0]), name.c_str())));
        }
        case CallId::GlBindAttribLocation: {
            std::string name(reinterpret_cast<const char*>(in.data()), in.size());
            fns.glBindAttribLocation_(static_cast<GLuint>(a[0]), static_cast<GLuint>(a[1]), name.c_str());
            return 0;
        }
        case CallId::GlShaderSource: {
            // in-buffer shape: one NUL-terminated source string
            // (Roblox's real shader-compile call sites always pass a
            // single concatenated source, count=1, the real GLES2 API
            // allows a `count`-way array, but nothing has shown Roblox
            // using more than one; grown against real evidence if that
            // changes).
            std::string src(reinterpret_cast<const char*>(in.data()), in.size());
            src = fix_uint_index_arithmetic(src);
            const char* src_ptr = src.c_str();
            fns.glShaderSource_(static_cast<GLuint>(a[0]), 1, &src_ptr, nullptr);
            return 0;
        }
        case CallId::GlGetProgramInfoLog: {
            GLsizei length = 0;
            out.resize(hdr.out_buffer_len > 0 ? hdr.out_buffer_len : 1);
            fns.glGetProgramInfoLog_(static_cast<GLuint>(a[0]), static_cast<GLsizei>(out.size()), &length,
                                      reinterpret_cast<GLchar*>(out.data()));
            *out_len = static_cast<uint32_t>(length > 0 ? length : 0);
            return 1;
        }
        case CallId::GlGetShaderInfoLog: {
            GLsizei length = 0;
            out.resize(hdr.out_buffer_len > 0 ? hdr.out_buffer_len : 1);
            fns.glGetShaderInfoLog_(static_cast<GLuint>(a[0]), static_cast<GLsizei>(out.size()), &length,
                                     reinterpret_cast<GLchar*>(out.data()));
            *out_len = static_cast<uint32_t>(length > 0 ? length : 0);
            return 1;
        }
        case CallId::GlGetActiveUniform: {
            GLsizei length = 0;
            GLint size = 0;
            GLenum type = 0;
            std::vector<char> name_buf(hdr.out_buffer_len > 0 ? hdr.out_buffer_len : 1);
            fns.glGetActiveUniform_(static_cast<GLuint>(a[0]), static_cast<GLuint>(a[1]),
                                     static_cast<GLsizei>(name_buf.size()), &length, &size, &type,
                                     name_buf.data());
            // out layout: [GLint size][GLenum type][name bytes...]
            out.resize(sizeof(GLint) + sizeof(GLenum) + static_cast<size_t>(length > 0 ? length : 0));
            std::memcpy(out.data(), &size, sizeof(GLint));
            std::memcpy(out.data() + sizeof(GLint), &type, sizeof(GLenum));
            if (length > 0) {
                std::memcpy(out.data() + sizeof(GLint) + sizeof(GLenum), name_buf.data(),
                            static_cast<size_t>(length));
            }
            *out_len = static_cast<uint32_t>(out.size());
            return 1;
        }

        // ---- GLES2: fixed-count-N id arrays ----
        case CallId::GlDeleteBuffers: fns.glDeleteBuffers_(static_cast<GLsizei>(a[0]), reinterpret_cast<const GLuint*>(in.data())); return 0;
        case CallId::GlDeleteFramebuffers: fns.glDeleteFramebuffers_(static_cast<GLsizei>(a[0]), reinterpret_cast<const GLuint*>(in.data())); return 0;
        case CallId::GlDeleteRenderbuffers: fns.glDeleteRenderbuffers_(static_cast<GLsizei>(a[0]), reinterpret_cast<const GLuint*>(in.data())); return 0;
        case CallId::GlDeleteTextures: fns.glDeleteTextures_(static_cast<GLsizei>(a[0]), reinterpret_cast<const GLuint*>(in.data())); return 0;
        // Real GLES3 sync objects. The host's own real GLsync pointer
        // is the handle the client carries; it never dereferences it.
        // GLES3 sync objects are DELIBERATELY INERT. Implementing them for
        // real (this session) hung the GPU: the kernel reported
        //     nouveau: stud-render-hos: job timeout, channel 10 killed!
        //     nouveau: gsp: rc ... fault_addr:0 fault_type:0
        //     nouveau: fifo: errored - disabling channel
        // i.e. a submitted job that never completes, not a page fault. That
        // kills the EGL context (eglSwapBuffers -> EGL_CONTEXT_LOST 0x300e),
        // after which glCheckFramebufferStatus returns 0 and the engine aborts
        // with "Unsupported framebuffer configuration" / RBXCRASH:
        // OutOfMemoryGraphics. The window goes black.
        //
        // Measured, same build, only this changed:
        //     sync real  -> 145 draws,  OOM=2, context lost at frame #2
        //     sync inert -> 11223 draws, OOM=0, app renders and is clickable
        //
        // Why it hangs is not yet established. The likely mechanism is
        // glWaitSync inserting a GPU-side wait on a sync object whose handle
        // does not survive the client/host round-trip, so the GPU waits on
        // something that never signals. A correct implementation must prove
        // the handle round-trip AND that the engine's fences actually signal
        // before being switched back on. Reporting "no sync support" is a
        // valid GLES answer; hanging the GPU is not.
        case CallId::GlFenceSync:
            return 0;
        case CallId::GlClientWaitSync:
            return 0x911A;  // GL_ALREADY_SIGNALED
        case CallId::GlWaitSync:
            return 0;
        case CallId::GlDeleteSync:
            return 0;
        // Consistent with the inert sync objects above: handles are always 0,
        // so never dereference one.
        case CallId::GlIsSync:
            return 0;
        case CallId::GlGetSynciv:
            return 0;
        case CallId::GlCopyImageSubData: {
            if (in.size() < 15 * sizeof(int32_t)) return 0;
            const auto* p = reinterpret_cast<const int32_t*>(in.data());
            fns.glCopyImageSubData_(static_cast<GLuint>(p[0]), static_cast<GLenum>(p[1]), p[2], p[3],
                                    p[4], p[5], static_cast<GLuint>(p[6]),
                                    static_cast<GLenum>(p[7]), p[8], p[9], p[10], p[11], p[12],
                                    p[13], p[14]);
            return 0;
        }
        case CallId::GetWindowSize: {
            uint64_t w = static_cast<uint64_t>(ANativeWindow_getWidth(nullptr));
            uint64_t h = static_cast<uint64_t>(ANativeWindow_getHeight(nullptr));
            return (w << 32) | (h & 0xffffffffu);
        }
        case CallId::AudioOpenStream:
            return stud::render_host::audio_open_stream(static_cast<int>(a[0]),
                                                        static_cast<int>(a[1]),
                                                        static_cast<int>(a[2]));
        case CallId::AudioWriteFrames:
            return stud::render_host::audio_write_frames(a[0], in.data(), in.size());
        case CallId::AudioCloseStream:
            stud::render_host::audio_close_stream(a[0]);
            return 1;

        // Vulkan, instance level. The real driver lives only here.
        case CallId::VkEnumerateInstanceVersion:
            return stud::render_host::vk_enumerate_instance_version(out, out_len);
        case CallId::VkEnumerateInstanceExtensionProperties:
            return stud::render_host::vk_enumerate_instance_extension_properties(
                in, static_cast<uint32_t>(a[0]), out, out_len);
        case CallId::VkEnumerateInstanceLayerProperties:
            return stud::render_host::vk_enumerate_instance_layer_properties(
                static_cast<uint32_t>(a[0]), out, out_len);
        case CallId::VkCreateInstance:
            return stud::render_host::vk_create_instance(in, out, out_len);
        case CallId::VkEnumeratePhysicalDevices:
            return stud::render_host::vk_enumerate_physical_devices(static_cast<uint32_t>(a[0]),
                                                                     out, out_len);
        case CallId::VkGetPhysicalDeviceProperties:
            return stud::render_host::vk_get_physical_device_properties(a[0], out, out_len);
        case CallId::VkGetPhysicalDeviceFeatures:
            return stud::render_host::vk_get_physical_device_features(a[0], out, out_len);
        case CallId::VkGetPhysicalDeviceMemoryProperties:
            return stud::render_host::vk_get_physical_device_memory_properties(a[0], out, out_len);
        case CallId::VkGetPhysicalDeviceQueueFamilyProperties:
            return stud::render_host::vk_get_physical_device_queue_family_properties(
                a[0], static_cast<uint32_t>(a[1]), out, out_len);
        case CallId::VkEnumerateDeviceExtensionProperties:
            return stud::render_host::vk_enumerate_device_extension_properties(
                a[0], in, static_cast<uint32_t>(a[1]), out, out_len);
        case CallId::VkGetPhysicalDeviceFeatures2:
            return stud::render_host::vk_get_physical_device_features2(
                a[0], in, static_cast<uint32_t>(a[1]), out, out_len);
        case CallId::VkCreateDevice:
            return stud::render_host::vk_create_device(a[0], in, out, out_len);
        case CallId::VkGetPhysicalDeviceFormatProperties:
            return stud::render_host::vk_get_physical_device_format_properties(
                a[0], static_cast<uint32_t>(a[1]), out, out_len);
        case CallId::VkGetPhysicalDeviceImageFormatProperties:
            return stud::render_host::vk_get_physical_device_image_format_properties(
                a[0], static_cast<uint32_t>(a[1]), static_cast<uint32_t>(a[2]),
                static_cast<uint32_t>(a[3]), static_cast<uint32_t>(a[4]),
                static_cast<uint32_t>(a[5]), out, out_len);
        case CallId::VkGetDeviceQueue:
            return stud::render_host::vk_get_device_queue(
                static_cast<uint32_t>(a[1]), static_cast<uint32_t>(a[2]), out, out_len);
        case CallId::VkCreateCommandPool:
            return stud::render_host::vk_create_command_pool(
                static_cast<uint32_t>(a[1]), static_cast<uint32_t>(a[2]), out, out_len);
        case CallId::VkCreateSemaphore:
            return stud::render_host::vk_create_semaphore(static_cast<uint32_t>(a[1]), out,
                                                           out_len);
        case CallId::VkCreateFence:
            return stud::render_host::vk_create_fence(static_cast<uint32_t>(a[1]), out, out_len);
        case CallId::VkCreateQueryPool:
            return stud::render_host::vk_create_query_pool(
                static_cast<uint32_t>(a[1]), static_cast<uint32_t>(a[2]),
                static_cast<uint32_t>(a[3]), static_cast<uint32_t>(a[4]), out, out_len);
        case CallId::VkCreatePipelineCache:
            return stud::render_host::vk_create_pipeline_cache(static_cast<uint32_t>(a[1]), in,
                                                                out, out_len);
        case CallId::VkGetPipelineCacheData:
            return stud::render_host::vk_get_pipeline_cache_data(
                a[1], static_cast<uint32_t>(a[2]), out, out_len);
        case CallId::VkDestroyPipelineCache:
            return stud::render_host::vk_destroy_pipeline_cache(a[1]);
        case CallId::VkCreateImage:
            return stud::render_host::vk_create_image(in, out, out_len);
        case CallId::VkGetImageMemoryRequirements:
            return stud::render_host::vk_get_image_memory_requirements(a[1], out, out_len);
        case CallId::VkGetPhysicalDeviceSurfaceCapabilitiesKHR:
            return stud::render_host::vk_get_physical_device_surface_capabilities(a[0], a[1], out,
                                                                                   out_len);
        case CallId::VkCreateBuffer:
            return stud::render_host::vk_create_buffer(
                static_cast<uint32_t>(a[1]), a[2], static_cast<uint32_t>(a[3]),
                static_cast<uint32_t>(a[4]), out, out_len);
        case CallId::VkGetBufferMemoryRequirements:
            return stud::render_host::vk_get_buffer_memory_requirements(a[1], out, out_len);
        case CallId::VkBindBufferMemory:
            return stud::render_host::vk_bind_buffer_memory(a[1], a[2], a[3]);
        case CallId::VkCreateImageView:
            return stud::render_host::vk_create_image_view(in, out, out_len);
        case CallId::VkCreateShaderModule:
            return stud::render_host::vk_create_shader_module(in, out, out_len);
        case CallId::VkDestroyHandle:
            return stud::render_host::vk_destroy_handle(static_cast<uint32_t>(a[1]), a[2]);
        case CallId::VkCreateRenderPass:
            return stud::render_host::vk_create_render_pass(in, out, out_len);
        case CallId::VkCreateFramebuffer:
            return stud::render_host::vk_create_framebuffer(in, out, out_len);
        case CallId::VkCreateSampler:
            return stud::render_host::vk_create_sampler(in, out, out_len);
        case CallId::VkCreatePipelineLayout:
            return stud::render_host::vk_create_pipeline_layout(in, out, out_len);
        case CallId::VkCreateDescriptorSetLayout:
            return stud::render_host::vk_create_descriptor_set_layout(in, out, out_len);
        case CallId::VkCreateDescriptorPool:
            return stud::render_host::vk_create_descriptor_pool(in, out, out_len);
        case CallId::VkAllocateDescriptorSets:
            return stud::render_host::vk_allocate_descriptor_sets(in, out, out_len);
        case CallId::VkResetDescriptorPool:
            return stud::render_host::vk_reset_descriptor_pool(a[1],
                                                                static_cast<uint32_t>(a[2]));
        case CallId::VkCreateDescriptorUpdateTemplate:
            return stud::render_host::vk_create_descriptor_update_template(in, out, out_len);
        case CallId::VkUpdateDescriptorSetWithTemplate:
            return stud::render_host::vk_update_descriptor_set_with_template(a[1], a[2], in);
        case CallId::VkCreateGraphicsPipelines:
            return stud::render_host::vk_create_graphics_pipelines(in, out, out_len);
        case CallId::VkCreateComputePipelines:
            return stud::render_host::vk_create_compute_pipelines(in, out, out_len);
        case CallId::VkAllocateCommandBuffers:
            return stud::render_host::vk_allocate_command_buffers(
                a[1], static_cast<uint32_t>(a[2]), static_cast<uint32_t>(a[3]), out, out_len);
        // Both are normally sent reply-free (see sync_command_buffer_calls()
        // in the Vulkan client), so the client cannot see these results and
        // this is the only place a failure can be noticed at all. Said once
        // each rather than per frame: a command buffer that fails to open
        // fails every frame.
        case CallId::VkBeginCommandBuffer: {
            const uint64_t r = stud::render_host::vk_begin_command_buffer(
                a[0], static_cast<uint32_t>(a[1]));
            report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkEndCommandBuffer: {
            const uint64_t r = stud::render_host::vk_end_command_buffer(a[0]);
            report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkResetCommandPool:
            return stud::render_host::vk_reset_command_pool(a[1], static_cast<uint32_t>(a[2]));
        // Reply-free in the normal configuration, like the command-buffer
        // bracket above, so this is the only place a failure can be seen.
        case CallId::VkQueueSubmit: {
            const uint64_t r = stud::render_host::vk_queue_submit(a[0], a[1], in);
            report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkWaitForFences:
            return stud::render_host::vk_wait_for_fences(in, static_cast<uint32_t>(a[1]), a[2]);
        case CallId::VkResetFences:
            return stud::render_host::vk_reset_fences(in);
        case CallId::VkAcquireNextImageKHR:
            return stud::render_host::vk_acquire_next_image(a[1], a[2], a[3], a[4], out, out_len);
        case CallId::VkQueuePresentKHR: {
            note_frame_pacing();
            // The throttle happens BEFORE the dispatch lock is taken;
            // see throttle_before_dispatch().
            const uint64_t r = stud::render_host::vk_queue_present(a[0], in);
            // OUT_OF_DATE and SUBOPTIMAL are the compositor telling the
            // engine to rebuild, not failures, and they happen on every
            // resize. Reporting them would be noise, and the rebuild path
            // already says what it did.
            const int32_t v = static_cast<int32_t>(r);
            if (v != -1000001004 && v != 1000001003) report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkGetQueryPoolResults:
            return stud::render_host::vk_get_query_pool_results(
                a[1], static_cast<uint32_t>(a[2]), static_cast<uint32_t>(a[3]),
                static_cast<uint32_t>(a[4]), static_cast<uint32_t>(a[5]), out, out_len);
        case CallId::VkCmdRecord:
            return stud::render_host::vk_cmd_record(a[0], static_cast<uint32_t>(a[1]), in);
        case CallId::VkCmdRecordBatch: {
            // [u64 command buffer][u32 kind][u32 length][payload] repeated.
            // Executed strictly in order, which is what makes a batch
            // identical to the same commands sent one at a time.
            size_t off = 0;
            while (off + 16 <= in.size()) {
                uint64_t cb = 0;
                uint32_t kind = 0;
                uint32_t len = 0;
                std::memcpy(&cb, in.data() + off, sizeof(cb));
                std::memcpy(&kind, in.data() + off + 8, sizeof(kind));
                std::memcpy(&len, in.data() + off + 12, sizeof(len));
                off += 16;
                if (off + len > in.size()) break;
                // Read where it already is: copying each command out of
                // the batch first is a copy of the whole batch per frame.
                stud::render_host::vk_cmd_record(cb, kind, in.data() + off, len);
                off += len;
            }
            return 0;
        }
        case CallId::VkHostHasProc:
            return stud::render_host::vk_host_has_proc(in);
        case CallId::VkDeviceWaitIdle:
            return stud::render_host::vk_device_wait_idle();
        case CallId::VkAllocateMemory: {
            // a[4] non-zero: the client mapped a file for this allocation
            // and wants it shared rather than copied. The name is derived
            // from the id so nothing has to travel in the buffer, which
            // still carries the pNext chain.
            std::string shared;
            if (a[4] != 0) {
                shared = stud::render_host::shared_memory_path(a[4]);
            }
            return stud::render_host::vk_allocate_memory(a[1], static_cast<uint32_t>(a[2]), in,
                                                          static_cast<uint32_t>(a[3]), out, out_len,
                                                          shared);
        }
        case CallId::VkBindImageMemory:
            return stud::render_host::vk_bind_image_memory(a[1], a[2], a[3]);
        case CallId::VkFreeMemory:
            return stud::render_host::vk_free_memory(a[1]);
        case CallId::VkMapMemory:
            return stud::render_host::vk_map_memory(a[1], a[2], a[3],
                                                     static_cast<uint32_t>(a[4]));
        case CallId::VkWriteMappedMemory:
            return stud::render_host::vk_write_mapped_memory(a[1], a[2], in);
        case CallId::VkReadMappedMemory:
            return stud::render_host::vk_read_mapped_memory(a[1], a[2], a[3], out, out_len);
        case CallId::VkShareMappedMemory:
            return stud::render_host::vk_share_mapped_memory(a[1], a[2], a[3]);
        case CallId::VkWriteSharedMappedMemory:
            return stud::render_host::vk_write_shared_mapped_memory(a[1], a[2], a[3]);
        case CallId::VkUnmapMemory:
            return stud::render_host::vk_unmap_memory(a[1]);
        case CallId::VkFlushMappedMemoryRanges:
            return stud::render_host::vk_flush_mapped_memory_ranges(a[1], a[2], a[3]);
        case CallId::VkGetPhysicalDeviceSurfaceFormatsKHR:
            return stud::render_host::vk_get_surface_formats(a[0], a[1],
                                                              static_cast<uint32_t>(a[2]), out,
                                                              out_len);
        case CallId::VkGetPhysicalDeviceSurfacePresentModesKHR:
            return stud::render_host::vk_get_surface_present_modes(
                a[0], a[1], static_cast<uint32_t>(a[2]), out, out_len);
        case CallId::VkGetPhysicalDeviceSurfaceSupportKHR:
            return stud::render_host::vk_get_surface_support(a[0], static_cast<uint32_t>(a[1]),
                                                              a[2], out, out_len);
        case CallId::VkCreateSwapchainKHR:
            return stud::render_host::vk_create_swapchain(in, out, out_len);
        case CallId::VkGetSwapchainImagesKHR:
            return stud::render_host::vk_get_swapchain_images(a[1], static_cast<uint32_t>(a[2]),
                                                               out, out_len);
        case CallId::VkGetPhysicalDeviceImageFormatProperties2:
            return stud::render_host::vk_get_image_format_properties2(
                a[0], in, static_cast<uint32_t>(a[1]), out, out_len);
        // Everything that is not graphics lives in its own function; see
        // dispatch_platform_call().
        default:
            if (auto handled = dispatch_platform_call(hdr, window, in, out, out_len)) {
                return *handled;
            }
            break;
        case CallId::GlTexStorage2D:
            trace_texture_upload("storage", g_bound_texture_2d, static_cast<GLint>(a[1]),
                                 static_cast<GLsizei>(a[3]), static_cast<GLsizei>(a[4]),
                                 static_cast<GLenum>(a[2]));
            if (cursor_trace_enabled()) {
                g_tex_dims[g_bound_texture_2d] = {static_cast<int>(a[3]), static_cast<int>(a[4])};
            }
            fns.glTexStorage2D_(static_cast<GLenum>(a[0]), static_cast<GLsizei>(a[1]),
                                static_cast<GLenum>(a[2]), static_cast<GLsizei>(a[3]),
                                static_cast<GLsizei>(a[4]));
            return 0;
        case CallId::GlTexStorage3D:
            fns.glTexStorage3D_(static_cast<GLenum>(a[0]), static_cast<GLsizei>(a[1]),
                                static_cast<GLenum>(a[2]), static_cast<GLsizei>(a[3]),
                                static_cast<GLsizei>(a[4]), static_cast<GLsizei>(a[5]));
            return 0;
        case CallId::GlTexSubImage3D: {
            // 10 real int args exceed the 8 header slots, so `format`
            // and `type` ride at the front of the in-buffer, ahead of
            // the real pixel data (see the client side's own comment).
            if (in.size() < 2 * sizeof(uint64_t)) return 0;
            const auto* extra = reinterpret_cast<const uint64_t*>(in.data());
            auto format = static_cast<GLenum>(extra[0]);
            auto type = static_cast<GLenum>(extra[1]);
            const void* pixels =
                hdr.pixel_buffer_offset_plus_one != 0
                    ? reinterpret_cast<const void*>(hdr.pixel_buffer_offset_plus_one - 1)
                    : (in.size() > 2 * sizeof(uint64_t)
                           ? static_cast<const void*>(in.data() + 2 * sizeof(uint64_t))
                           : nullptr);
            fns.glTexSubImage3D_(static_cast<GLenum>(a[0]), static_cast<GLint>(a[1]),
                                 static_cast<GLint>(a[2]), static_cast<GLint>(a[3]),
                                 static_cast<GLint>(a[4]), static_cast<GLsizei>(a[5]),
                                 static_cast<GLsizei>(a[6]), static_cast<GLsizei>(a[7]), format,
                                 type, pixels);
            return 0;
        }
        case CallId::GlProgramParameteri:
            fns.glProgramParameteri_(static_cast<GLuint>(a[0]), static_cast<GLenum>(a[1]),
                                     static_cast<GLint>(a[2]));
            return 0;
        case CallId::GlUniformBlockBinding:
            fns.glUniformBlockBinding_(static_cast<GLuint>(a[0]), static_cast<GLuint>(a[1]),
                                       static_cast<GLuint>(a[2]));
            return 0;
        case CallId::GlGetUniformBlockIndex: {
            // NUL-terminated block name arrives in the in-buffer.
            std::string name(reinterpret_cast<const char*>(in.data()), in.size());
            if (!name.empty() && name.back() == '\0') name.pop_back();
            return fns.glGetUniformBlockIndex_(static_cast<GLuint>(a[0]), name.c_str());
        }
        case CallId::GlGetActiveUniformBlockiv: {
            auto program = static_cast<GLuint>(a[0]);
            auto index = static_cast<GLuint>(a[1]);
            auto pname = static_cast<GLenum>(a[2]);
            // Most pnames write a single int; ACTIVE_UNIFORM_INDICES
            // writes one per active uniform in the block, so size it
            // from the real count rather than guessing.
            GLint count = 1;
            if (pname == GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES) {
                fns.glGetActiveUniformBlockiv_(program, index, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS,
                                               &count);
                if (count < 0) count = 0;
            }
            out.resize(static_cast<size_t>(count) * sizeof(GLint));
            if (count > 0) {
                fns.glGetActiveUniformBlockiv_(program, index, pname,
                                               reinterpret_cast<GLint*>(out.data()));
            }
            *out_len = static_cast<uint32_t>(out.size());
            return 1;
        }
        case CallId::GlBindBufferBase:
            fns.glBindBufferBase_(static_cast<GLenum>(a[0]), static_cast<GLuint>(a[1]),
                                  static_cast<GLuint>(a[2]));
            return 0;
        case CallId::GlBindBufferRange:
            fns.glBindBufferRange_(static_cast<GLenum>(a[0]), static_cast<GLuint>(a[1]),
                                   static_cast<GLuint>(a[2]), static_cast<GLintptr>(a[3]),
                                   static_cast<GLsizeiptr>(a[4]));
            return 0;
        case CallId::GlClearBufferfv:
            fns.glClearBufferfv_(static_cast<GLenum>(a[0]), static_cast<GLint>(a[1]),
                                 reinterpret_cast<const GLfloat*>(in.data()));
            return 0;
        case CallId::GlDrawBuffers:
            fns.glDrawBuffers_(static_cast<GLsizei>(a[0]),
                               reinterpret_cast<const GLenum*>(in.data()));
            return 0;
        case CallId::GlRenderbufferStorageMultisample:
            fns.glRenderbufferStorageMultisample_(
                static_cast<GLenum>(a[0]), static_cast<GLsizei>(a[1]), static_cast<GLenum>(a[2]),
                static_cast<GLsizei>(a[3]), static_cast<GLsizei>(a[4]));
            return 0;
        case CallId::GlDrawArraysInstanced:
            fns.glDrawArraysInstanced_(static_cast<GLenum>(a[0]), static_cast<GLint>(a[1]),
                                        static_cast<GLsizei>(a[2]), static_cast<GLsizei>(a[3]));
            return 0;
        case CallId::GlDrawElementsInstanced:
            fns.glDrawElementsInstanced_(static_cast<GLenum>(a[0]), static_cast<GLsizei>(a[1]),
                                          static_cast<GLenum>(a[2]),
                                          reinterpret_cast<const void*>(a[3]),
                                          static_cast<GLsizei>(a[4]));
            return 0;
        case CallId::GlVertexAttribDivisor:
            fns.glVertexAttribDivisor_(static_cast<GLuint>(a[0]), static_cast<GLuint>(a[1]));
            return 0;
        case CallId::GlVertexAttribIPointer:
            fns.glVertexAttribIPointer_(static_cast<GLuint>(a[0]), static_cast<GLint>(a[1]),
                                         static_cast<GLenum>(a[2]), static_cast<GLsizei>(a[3]),
                                         reinterpret_cast<const void*>(a[4]));
            return 0;
        case CallId::GlBlitFramebuffer: {
            uint32_t tail[2] = {0, 0};
            if (in.size() >= sizeof(tail)) std::memcpy(tail, in.data(), sizeof(tail));
            fns.glBlitFramebuffer_(
                static_cast<GLint>(static_cast<int64_t>(a[0])), static_cast<GLint>(static_cast<int64_t>(a[1])),
                static_cast<GLint>(static_cast<int64_t>(a[2])), static_cast<GLint>(static_cast<int64_t>(a[3])),
                static_cast<GLint>(static_cast<int64_t>(a[4])), static_cast<GLint>(static_cast<int64_t>(a[5])),
                static_cast<GLint>(static_cast<int64_t>(a[6])), static_cast<GLint>(static_cast<int64_t>(a[7])),
                static_cast<GLbitfield>(tail[0]), static_cast<GLenum>(tail[1]));
            return 0;
        }
        case CallId::GlInvalidateFramebuffer:
            fns.glInvalidateFramebuffer_(static_cast<GLenum>(a[0]), static_cast<GLsizei>(a[1]),
                                          reinterpret_cast<const GLenum*>(in.data()));
            return 0;
        case CallId::GlBindVertexArray: fns.glBindVertexArray_(static_cast<GLuint>(a[0])); return 0;
        case CallId::GlDeleteVertexArrays: fns.glDeleteVertexArrays_(static_cast<GLsizei>(a[0]), reinterpret_cast<const GLuint*>(in.data())); return 0;
        case CallId::GlGenVertexArrays: {
            out.resize(static_cast<size_t>(a[0]) * sizeof(GLuint));
            fns.glGenVertexArrays_(static_cast<GLsizei>(a[0]), reinterpret_cast<GLuint*>(out.data()));
            *out_len = static_cast<uint32_t>(out.size());
            return 1;
        }
        case CallId::GlGenBuffers: {
            out.resize(static_cast<size_t>(a[0]) * sizeof(GLuint));
            fns.glGenBuffers_(static_cast<GLsizei>(a[0]), reinterpret_cast<GLuint*>(out.data()));
            *out_len = static_cast<uint32_t>(out.size());
            return 1;
        }
        case CallId::GlGenFramebuffers: {
            out.resize(static_cast<size_t>(a[0]) * sizeof(GLuint));
            fns.glGenFramebuffers_(static_cast<GLsizei>(a[0]), reinterpret_cast<GLuint*>(out.data()));
            *out_len = static_cast<uint32_t>(out.size());
            return 1;
        }
        case CallId::GlGenRenderbuffers: {
            out.resize(static_cast<size_t>(a[0]) * sizeof(GLuint));
            fns.glGenRenderbuffers_(static_cast<GLsizei>(a[0]), reinterpret_cast<GLuint*>(out.data()));
            *out_len = static_cast<uint32_t>(out.size());
            return 1;
        }
        case CallId::GlGenTextures: {
            out.resize(static_cast<size_t>(a[0]) * sizeof(GLuint));
            fns.glGenTextures_(static_cast<GLsizei>(a[0]), reinterpret_cast<GLuint*>(out.data()));
            *out_len = static_cast<uint32_t>(out.size());
            return 1;
        }

        // ---- GLES2: small out-param arrays ----
        case CallId::GlGetIntegerv: {
            // Generous fixed count (16) covers every real
            // GLES2 pname's true count (the largest, GL_ALIASED_*_RANGE/
            // viewport-shaped queries, need at most 4), always reading
            // a few extra, harmless ints past what a given pname truly
            // uses is safe (the real driver only ever writes the pname's
            // own true count; the rest of the buffer is simply unused,
            // not read).
            GLint values[16] = {};
            fns.glGetIntegerv_(static_cast<GLenum>(a[0]), values);
            // Report ZERO program-binary formats. Stud's GL forwarding
            // implements glGetProgramBinary but NOT glProgramBinary (a no-op
            // stub), so with binaries "supported" the engine saves a real
            // binary, later believes it reloaded a program that was never
            // actually loaded, and draws with it, which hangs the GPU
            // (nouveau: "job timeout, channel killed") and loses the EGL
            // context, blacking out the window. Advertising no binary formats
            // is a truthful answer for this transport and makes the engine
            // compile its shaders normally.
            if (static_cast<GLenum>(a[0]) == 0x87FE /*GL_NUM_PROGRAM_BINARY_FORMATS*/) {
                values[0] = 0;
            }
            out.resize(sizeof(values));
            std::memcpy(out.data(), values, sizeof(values));
            *out_len = static_cast<uint32_t>(out.size());
            return 1;
        }
        case CallId::GlTexParameterfv: {
            GLfloat values[4] = {unpack_float(a[2]), 0, 0, 0};
            fns.glTexParameterfv_(static_cast<GLenum>(a[0]), static_cast<GLenum>(a[1]), values);
            return 0;
        }
        case CallId::GlGetProgramiv: {
            GLint value = 0;
            fns.glGetProgramiv_(static_cast<GLuint>(a[0]), static_cast<GLenum>(a[1]), &value);
            out.resize(sizeof(GLint));
            std::memcpy(out.data(), &value, sizeof(GLint));
            *out_len = sizeof(GLint);
            return 1;
        }
        case CallId::GlGetShaderiv: {
            GLint value = 0;
            fns.glGetShaderiv_(static_cast<GLuint>(a[0]), static_cast<GLenum>(a[1]), &value);
            out.resize(sizeof(GLint));
            std::memcpy(out.data(), &value, sizeof(GLint));
            *out_len = sizeof(GLint);
            return 1;
        }

        // ---- GLES2: bulk buffer transfer ----
        case CallId::GlBufferData:
            // Bug found in testing: `usage` rides in a[3] (the client
            // leaves a[2] as a zero placeholder for the data pointer it
            // cannot send), but this read a[2], so every single
            // glBufferData call in this project's history passed usage=0,
            // an invalid enum, and the driver allocated no storage at
            // all. Caught by a real stud-render-host SIGSEGV inside
            // ANGLE's own rx::vk::DescriptorSetDescBuilder::
            // updateOneUniformBuffer during the engine's first real
            // glDrawArrays: a uniform buffer that was never actually
            // allocated has no BufferHelper behind it.
            fns.glBufferData_(static_cast<GLenum>(a[0]), static_cast<GLsizeiptr>(a[1]),
                               in.empty() ? nullptr : in.data(), static_cast<GLenum>(a[3]));
            return 0;
        case CallId::GlCommandBatch: {
            // Unpacks what call_void() packed: argc, a flags byte, the
            // 16-bit call id, argc arguments, then the payload length and
            // PBO offset only if they were used. Each call is dispatched
            // in order, exactly as if it had arrived as its own request.
            const uint8_t* p = in.data();
            const uint8_t* end = in.data() + in.size();
            while (end - p >= 4) {
                const uint8_t argc = *p++;
                const uint8_t bits = *p++;
                uint16_t id16 = 0;
                std::memcpy(&id16, p, sizeof(id16));
                p += sizeof(id16);
                if (argc > 8 || end - p < static_cast<ptrdiff_t>(argc) * 8) break;

                Header sub{};
                sub.call_id = static_cast<CallId>(id16);
                sub.flags = Header::kNoReply;
                if (argc > 0) {
                    std::memcpy(sub.args, p, static_cast<size_t>(argc) * sizeof(uint64_t));
                    p += static_cast<size_t>(argc) * sizeof(uint64_t);
                }
                const uint8_t* payload = nullptr;
                uint32_t payload_len = 0;
                if ((bits & 1u) != 0) {
                    if (end - p < 4) break;
                    std::memcpy(&payload_len, p, sizeof(payload_len));
                    p += sizeof(payload_len);
                    if (end - p < static_cast<ptrdiff_t>(payload_len)) break;
                    payload = p;
                    p += payload_len;
                }
                if ((bits & 2u) != 0) {
                    if (end - p < 8) break;
                    std::memcpy(&sub.pixel_buffer_offset_plus_one, p,
                                sizeof(sub.pixel_buffer_offset_plus_one));
                    p += sizeof(sub.pixel_buffer_offset_plus_one);
                }
                sub.in_buffer_len = payload_len;

                // Reused across the whole batch rather than allocated
                // per call, a batch holds thousands of them.
                static thread_local std::vector<uint8_t> sub_in;
                static thread_local std::vector<uint8_t> sub_out;
                sub_in.assign(payload, payload + payload_len);
                sub_out.clear();
                uint32_t sub_out_len = 0;
                dispatch(sub, fns, window, sub_in, sub_out, &sub_out_len);
            }
            return 0;
        }
        case CallId::GlGenQueries: {
            const auto n = static_cast<GLsizei>(a[0]);
            if (fns.glGenQueries_ == nullptr || n <= 0) return 0;
            out.resize(static_cast<size_t>(n) * sizeof(GLuint));
            fns.glGenQueries_(n, reinterpret_cast<GLuint*>(out.data()));
            *out_len = static_cast<uint32_t>(out.size());
            return 1;
        }
        case CallId::GlDeleteQueries:
            if (fns.glDeleteQueries_ == nullptr || in.empty()) return 0;
            fns.glDeleteQueries_(static_cast<GLsizei>(a[0]),
                                 reinterpret_cast<const GLuint*>(in.data()));
            return 0;
        case CallId::GlBeginQuery:
            if (fns.glBeginQuery_ == nullptr) return 0;
            fns.glBeginQuery_(static_cast<GLenum>(a[0]), static_cast<GLuint>(a[1]));
            return 0;
        case CallId::GlEndQuery:
            if (fns.glEndQuery_ == nullptr) return 0;
            fns.glEndQuery_(static_cast<GLenum>(a[0]));
            return 0;
        case CallId::GlGetQueryObjectuiv: {
            if (fns.glGetQueryObjectuiv_ == nullptr) return 0;
            GLuint value = 0;
            fns.glGetQueryObjectuiv_(static_cast<GLuint>(a[0]), static_cast<GLenum>(a[1]), &value);
            out.resize(sizeof(value));
            std::memcpy(out.data(), &value, sizeof(value));
            *out_len = sizeof(value);
            return 1;
        }
        case CallId::GlGetQueryObjectui64v: {
            if (fns.glGetQueryObjectui64v_ == nullptr) return 0;
            GLuint64 value = 0;
            fns.glGetQueryObjectui64v_(static_cast<GLuint>(a[0]), static_cast<GLenum>(a[1]),
                                       &value);
            out.resize(sizeof(value));
            std::memcpy(out.data(), &value, sizeof(value));
            *out_len = sizeof(value);
            return 1;
        }
        case CallId::GlBufferStorage:
            if (fns.glBufferStorage_ == nullptr) {
                // No immutable storage on this driver: an ordinary
                // mutable allocation of the same size is a correct
                // substitute for everything the engine does with it.
                fns.glBufferData_(static_cast<GLenum>(a[0]), static_cast<GLsizeiptr>(a[1]),
                                  in.empty() ? nullptr : in.data(), GL_DYNAMIC_DRAW);
                return 0;
            }
            fns.glBufferStorage_(static_cast<GLenum>(a[0]), static_cast<GLsizeiptr>(a[1]),
                                 in.empty() ? nullptr : in.data(),
                                 static_cast<GLbitfield>(a[2]));
            return 0;
        case CallId::GlBufferSubData:
            fns.glBufferSubData_(static_cast<GLenum>(a[0]), static_cast<GLintptr>(a[1]),
                                  static_cast<GLsizeiptr>(a[2]), in.data());
            return 0;
        case CallId::GlGetBufferSubData: {
            // a: target, offset, length. GLES has no glGetBufferSubData, so
            // read the range back through a real read mapping, the client
            // needs the buffer's current bytes to honour a non-invalidating
            // write map (see the CallId's own comment in the protocol header).
            const GLenum target = static_cast<GLenum>(a[0]);
            const GLsizeiptr length = static_cast<GLsizeiptr>(a[2]);
            if (length <= 0) return 0;
            void* p = fns.glMapBufferRange_(target, static_cast<GLintptr>(a[1]), length,
                                             GL_MAP_READ_BIT);
            if (p == nullptr) return 0;
            out.assign(static_cast<const unsigned char*>(p),
                       static_cast<const unsigned char*>(p) + length);
            fns.glUnmapBuffer_(target);
            *out_len = static_cast<uint32_t>(length);
            return 1;
        }
        case CallId::GlTexImage2D:
            // a: target,level,internalformat,width,height,border,format,type
            if (cursor_trace_enabled() && static_cast<GLint>(a[1]) == 0) {
                g_tex_dims[g_bound_texture_2d] = {static_cast<int>(a[3]), static_cast<int>(a[4])};
            }
            fns.glTexImage2D_(static_cast<GLenum>(a[0]), static_cast<GLint>(a[1]), static_cast<GLint>(a[2]),
                               static_cast<GLsizei>(a[3]), static_cast<GLsizei>(a[4]), static_cast<GLint>(a[5]),
                               static_cast<GLenum>(a[6]), static_cast<GLenum>(a[7]), pixels_ptr());
            return 0;
        case CallId::GlTexSubImage2D:
            // a: target,level,xoffset,yoffset,width,height,format,type
            fns.glTexSubImage2D_(static_cast<GLenum>(a[0]), static_cast<GLint>(a[1]), static_cast<GLint>(a[2]),
                                  static_cast<GLint>(a[3]), static_cast<GLsizei>(a[4]), static_cast<GLsizei>(a[5]),
                                  static_cast<GLenum>(a[6]), static_cast<GLenum>(a[7]), pixels_ptr());
            return 0;
        case CallId::GlCompressedTexImage2D:
            trace_texture_upload("compressed", g_bound_texture_2d, static_cast<GLint>(a[1]),
                                 static_cast<GLsizei>(a[3]), static_cast<GLsizei>(a[4]),
                                 static_cast<GLenum>(a[2]));
            // a[6] is the real imageSize. It cannot be derived from the
            // in-buffer size when the pixels come from a real PBO instead.
            fns.glCompressedTexImage2D_(static_cast<GLenum>(a[0]), static_cast<GLint>(a[1]),
                                         static_cast<GLenum>(a[2]), static_cast<GLsizei>(a[3]),
                                         static_cast<GLsizei>(a[4]), static_cast<GLint>(a[5]),
                                         static_cast<GLsizei>(a[6]), pixels_ptr());
            return 0;
        case CallId::GlCompressedTexSubImage2D:
            trace_texture_upload("compressed-sub", g_bound_texture_2d, static_cast<GLint>(a[1]),
                                 static_cast<GLsizei>(a[4]), static_cast<GLsizei>(a[5]),
                                 static_cast<GLenum>(a[6]), static_cast<size_t>(a[7]),
                                 hdr.pixel_buffer_offset_plus_one, in.size());
            fns.glCompressedTexSubImage2D_(static_cast<GLenum>(a[0]), static_cast<GLint>(a[1]),
                                            static_cast<GLint>(a[2]), static_cast<GLint>(a[3]),
                                            static_cast<GLsizei>(a[4]), static_cast<GLsizei>(a[5]),
                                            static_cast<GLenum>(a[6]), static_cast<GLsizei>(a[7]),
                                            pixels_ptr());
            return 0;
        case CallId::GlReadPixels: {
            GLenum format = static_cast<GLenum>(a[4]);
            GLenum type = static_cast<GLenum>(a[5]);
            uint32_t bytes = static_cast<uint32_t>(a[2]) * static_cast<uint32_t>(a[3]) * gl_pixel_size(format, type);
            out.resize(bytes);
            fns.glReadPixels_(static_cast<GLint>(a[0]), static_cast<GLint>(a[1]), static_cast<GLsizei>(a[2]),
                               static_cast<GLsizei>(a[3]), format, type, out.data());
            *out_len = bytes;
            return 1;
        }

        case CallId::VkCreateWaylandSurfaceForAndroidSurface: {
            if (g_real_vk_get_instance_proc_addr == nullptr) return kNullHandle;
            // X11 answers the same request with an Xlib surface.
            //
            // The call id still says Wayland because it is the engine's
            // vkCreateAndroidSurfaceKHR being redirected, and which WSI
            // sits underneath is this process's business alone. Handing a
            // Wayland surface to a driver on an X11 session is not a
            // graceful failure either: live-caught as a SIGSEGV inside
            // the NVIDIA driver at wl_proxy_create_wrapper, reached from
            // vkGetPhysicalDeviceSurfaceSupportKHR.
            if (window.on_x11()) {
                auto create_xlib = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(
                    g_real_vk_get_instance_proc_addr(reinterpret_cast<VkInstance>(a[0]),
                                                       "vkCreateXlibSurfaceKHR"));
                if (create_xlib == nullptr) {
                    std::fprintf(stderr,
                                  "stud-render-host: the Vulkan driver has no "
                                  "vkCreateXlibSurfaceKHR, use the OpenGL render path on X11\n");
                    std::fflush(stderr);
                    return kNullHandle;
                }
                VkXlibSurfaceCreateInfoKHR xinfo{};
                xinfo.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
                // The driver's own connection, not the one the event pump
                // reads; see native_window_x11_vk_display().
                void* vk_dpy = stud::android_glue::native_window_x11_vk_display();
                if (vk_dpy == nullptr) vk_dpy = window.x11_display;
                xinfo.dpy = static_cast<Display*>(vk_dpy);
                xinfo.window = static_cast<::Window>(window.x11_window);
                VkSurfaceKHR xsurface = VK_NULL_HANDLE;
                VkResult xr = create_xlib(reinterpret_cast<VkInstance>(a[0]), &xinfo, nullptr,
                                           &xsurface);
                if (xr != VK_SUCCESS) {
                    std::fprintf(stderr, "stud-render-host: vkCreateXlibSurfaceKHR -> %d\n",
                                  static_cast<int>(xr));
                    std::fflush(stderr);
                    return kNullHandle;
                }
                return reinterpret_cast<uint64_t>(xsurface);
            }
            auto create_wayland = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
                g_real_vk_get_instance_proc_addr(reinterpret_cast<VkInstance>(a[0]),
                                                   "vkCreateWaylandSurfaceKHR"));
            if (create_wayland == nullptr) return kNullHandle;
            VkWaylandSurfaceCreateInfoKHR info{};
            info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
            info.display = window.display;
            info.surface = window.surface;
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            VkResult r = create_wayland(reinterpret_cast<VkInstance>(a[0]), &info, nullptr, &surface);
            if (r != VK_SUCCESS) return kNullHandle;
            // Vulkan attaches its own buffers without ever going through
            // wl_egl_window_create, which is where the EGL path applies
            // the surface's logical-size/scale state. Do it here so the
            // surface is configured the same way on both paths.
            // NEVER ANativeWindow_fromSurface(nullptr, nullptr) here.
            // That is the call that once spawned a window per invocation,
            // and the guard added since would make it return null anyway.
            // The window this process owns is captured at startup.
            stud::android_glue::native_window_apply_surface_scale(g_real_window);
            stud::android_glue::native_window_set_opaque(g_real_window);
            return reinterpret_cast<uint64_t>(surface);
        }

        // Process C owns exactly one real window for this MVP (created
        // once, at startup), a fixed sentinel handle (1) stands in for
        // "the" ANativeWindow; acquire/release are real no-ops here
        // since Process C's own window lifetime isn't tied to whatever
        // refcounting Roblox's side does with it.
        case CallId::ANativeWindowFromSurface:
            return 1;
        // Caught in testing: regression (the engineering notes, "Three different
        // 'real' window sizes existed for one window"): these two returned
        // hardcoded 800x600 while the same process sized its wl_egl_window,
        // and answered CallId::GetWindowSize, from android-glue's own real
        // ANativeWindow. libroblox.so imports ANativeWindow_getWidth/getHeight
        // directly (render-client/src/native_window_forward.cpp forwards them
        // here), so the engine believed its window was 800x600 while its EGL
        // surface really was 1280x720. DeviceGL then hit
        // "updateMainFramebuffer needs to resize" on every single frame,
        // tried to recreate the window surface, failed (only one window
        // surface can exist), and gave up on the frame, which is exactly
        // the black window with a handful of draw calls per frame. One
        // source of truth: android-glue's own real window.
        case CallId::ANativeWindowGetWidth:
            return static_cast<uint64_t>(ANativeWindow_getWidth(nullptr));
        case CallId::ANativeWindowGetHeight:
            return static_cast<uint64_t>(ANativeWindow_getHeight(nullptr));
        case CallId::ANativeWindowAcquire:
        case CallId::ANativeWindowRelease:
            return 0;
    }
    return kNullHandle;
}

}  // namespace


// Serialises every dispatch: the GL state and EGL context the handlers
// touch are not thread-safe, and extra client connections are served on
// their own threads (see the accept path).
std::mutex& dispatch_mutex() {
    static std::mutex m;
    return m;
}

// Everything that reads or dispatches the Wayland connection takes this.
//
// Three different threads touch that one connection: the accept loop, the
// primary client's own loop, and every secondary-connection thread right
// after it presents: and, inside dispatch(), the roundtrips around
// eglCreateWindowSurface and eglSwapBuffers. Only the last of those is
// serialised by dispatch_mutex, so the pumps were free to run against a
// roundtrip, and against each other.
//
// libwayland is thread-safe only per queue and only when one thread at a
// time is inside the prepare_read/read_events dance. Two threads
// dispatching the same default queue can hand the same event to two
// listeners, and android-glue's listeners keep ordinary non-atomic state
// (the window size, the key-repeat timer, the resize bookkeeping). The
// reported symptom was a SIGSEGV in stud-render-host three seconds into a
// launch on a machine whose compositor resized the window at startup,
// the one configuration that puts compositor events and presents in the
// same instant.
//
// Lock order is dispatch_mutex -> wayland_mutex, and nothing ever takes
// them the other way round, so the pair cannot deadlock. Nothing here
// blocks indefinitely either: the pump only reads when poll() has already
// said the fd is readable, and a roundtrip waits on a reply the
// compositor owes it.
std::mutex& wayland_mutex() {
    static std::mutex m;
    return m;
}

// Multi-thread-safe, never-blocking Wayland pump.
//
// wl_display_dispatch() blocks until it can dispatch at least one event.
// That is fine for a single-threaded client, but ANGLE's own Vulkan WSI
// code reads the same default queue from whichever thread is inside
// eglSwapBuffers, so poll() can report the display fd readable and, by
// the time this thread calls dispatch, another thread has already drained
// it. dispatch() then waits for the *next* event, which may never come.
//
// Live-caught exactly that, mid game-launch: the main loop parked forever
// in wl_display_dispatch while its own render connection had 219KB
// unread and Process B's engine thread was blocked writing into it. The
// engine never got its reply, so the start-game task never ran and the
// join stalled at `stepDataModelJob: No DM yet`.
//
// prepare_read/read_events is libwayland's own answer to this. The
// display fd is non-blocking, and cancel_read backs the reservation out
// when there is nothing to read, so no path through here can block.
// The Vulkan layer is told the window size rather than deriving one
// (see vk_set_window_size's own comment: deriving means
// ANativeWindow_fromSurface(nullptr, nullptr), which CREATES windows).
// It was told once, at startup, so after a resize the engine still got
// the boot size back from vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
// rebuilt its swapchain at that size, and the compositor scaled the
// result up to the real window: blurry, and stretched to the new aspect
// ratio because the viewport's destination is the new logical size while
// the buffer still had the old one (circles became ovals, live-reported).
// Reading the size the compositor already gave android-glue costs
// nothing and creates nothing, so keep it current every pump.
void sync_vk_window_size() {
    if (g_real_window == nullptr) return;
    const auto w = static_cast<uint32_t>(ANativeWindow_getWidth(g_real_window));
    const auto h = static_cast<uint32_t>(ANativeWindow_getHeight(g_real_window));
    if (w == 0 || h == 0) return;
    static uint32_t last_w = 0, last_h = 0;
    if (w == last_w && h == last_h) return;

    // Let the drag FINISH before the engine is told.
    //
    // Every size change here becomes a new surface extent, and a new
    // surface extent makes the engine tear its swapchain down and build
    // another. On Wayland a drag arrives as a handful of configure
    // events, so that is a handful of rebuilds. X11 reports every pixel:
    // one drag produced a rebuild per pixel, and one of those teardowns
    // caught work that was still pending on the swapchain it was
    // destroying -- after which the device can never go idle, and the
    // engine's own vkDeviceWaitIdle on the next rebuild never returns.
    // Live-caught as `dispatch call=154` stuck for ever and a dead
    // window.
    //
    // So the newest size is remembered and applied once it has held still
    // briefly. Mid-drag the engine keeps rendering at the size it has,
    // which the server scales into the window exactly as it does for
    // every other resizing program, and one rebuild happens at the end.
    // The first size is applied at once: there is nothing to settle yet,
    // and the engine cannot start without it.
    static uint32_t pending_w = 0;
    static uint32_t pending_h = 0;
    static auto size_changed_at = std::chrono::steady_clock::now();
    static auto last_applied_at = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    const bool first_size = last_w == 0 || last_h == 0;
    // A size that arrives out of the blue is applied AT ONCE; only the
    // ones that follow it are made to settle.
    //
    // Toggling fullscreen is a single change, and waiting for it to
    // settle was pure delay -- the window was already its new size, with
    // the engine still rendering the old one into a corner of it. A drag
    // is hundreds of changes, and those still collapse into one rebuild
    // at the end, which is what stops the engine from being asked to
    // rebuild faster than a rebuild takes (measured: 35ms, nearly all of
    // it inside the driver).
    const bool settled_before =
        std::chrono::steady_clock::now() - last_applied_at > std::chrono::milliseconds(200);
    if (!first_size && !settled_before) {
        if (w != pending_w || h != pending_h) {
            pending_w = w;
            pending_h = h;
            size_changed_at = std::chrono::steady_clock::now();
        }
        static const int settle_ms = [] {
            const char* v = std::getenv("STUD_RESIZE_SETTLE_MS");
            // Short: this is the window in which the engine is still
            // rendering at the old size while the window is already the
            // new one, and on X11 the uncovered strip has nothing in it
            // (the window has no background, deliberately -- see
            // x11_backend.cpp -- so it shows through). Long enough to
            // collapse a drag's worth of sizes into one rebuild, short
            // enough that the strip is a flicker rather than a hole.
            const int n = v != nullptr ? std::atoi(v) : 80;
            return n >= 0 ? n : 80;
        }();
        if (std::chrono::steady_clock::now() - size_changed_at <
            std::chrono::milliseconds(settle_ms)) {
            return;
        }
    }

    last_w = w;
    last_h = h;
    last_applied_at = std::chrono::steady_clock::now();
    stud::render_host::vk_set_window_size(w, h);
    // What the upscaler writes: the window in the display's own pixels,
    // which is a different number from the one above whenever the engine
    // is rendering below the screen. Zero while upscaling is off, which
    // disables the path entirely.
    // On X11 the pass runs whether or not the filter is switched on.
    //
    // Wayland scales the buffer to the window itself: a surface carries
    // its own scale and viewport, so rendering below the window's real
    // resolution costs nothing but sharpness. X has no such thing. A
    // buffer smaller than the window would simply be a smaller picture in
    // the corner of it, so something has to scale it, and the only
    // something is this pass. With the filter on that is FSR; with it off
    // it is a plain scaled blit, which is exactly what the Wayland
    // compositor would have done.
    const bool scale_in_stud =
        g_upscaling_enabled ||
        stud::android_glue::display_backend() == stud::android_glue::DisplayBackend::X11;
    stud::render_host::vk_set_upscale_use_compute(g_upscaling_enabled);
    if (scale_in_stud) {
        int32_t real_w = 0;
        int32_t real_h = 0;
        stud::android_glue::native_window_display_pixel_size(&real_w, &real_h);
        if (real_w > 0 && real_h > 0) {
            // The window's own pixels, and nothing else: the compositor
            // then shows the frame 1:1, with no second resample.
            stud::render_host::vk_set_upscale_output_size(static_cast<uint32_t>(real_w),
                                                           static_cast<uint32_t>(real_h));
        }
    }
    std::printf("stud-render-host: window size now %ux%u (Vulkan surface extent updated)\n", w, h);
    std::fflush(stdout);
}

void pump_wayland(wl_display* display, bool fd_readable);

// Drains whichever display server is in use.
//
// The Wayland path is unchanged and still does the prepare_read/
// read_events dance under wayland_mutex. X11 has no such queue
// discipline to get wrong, Xlib is drained from one place here, but
// it takes the same lock, because the lock is what keeps two threads out
// of the display connection at once and that is just as true of Xlib.
void pump_display(const RealWindow& window, bool fd_readable) {
    if (window.on_x11()) {
        std::lock_guard<std::mutex> lock(wayland_mutex());
        sync_vk_window_size();
        stud::android_glue::native_window_pump_x11();
        stud::android_glue::native_window_pump_key_repeat();
        return;
    }
    pump_wayland(window.display, fd_readable);
}

// Stud's own queue, and NOTHING else.
//
// pump_wayland does a great deal beside dispatching: it syncs the
// swapchain size, repeats held keys, blinks the caret, and drains the
// driver's default queue. All of that belongs to the main loop's
// cadence, and doing it on every cycle of the input wait (250 times a
// second) measured as render-host 8% -> 24% and the engine thread
// 25% -> 145% at an idle Home screen. This is the part input actually
// needs.
void dispatch_input_queue(wl_display* display, bool fd_readable) {
    if (display == nullptr) return;
    wl_event_queue* queue = stud::android_glue::native_window_wl_queue();
    if (queue == nullptr) return;
    std::lock_guard<std::mutex> lock(wayland_mutex());
    while (wl_display_prepare_read_queue(display, queue) != 0) {
        wl_display_dispatch_queue_pending(display, queue);
    }
    wl_display_flush(display);
    if (fd_readable) {
        wl_display_read_events(display);
    } else {
        wl_display_cancel_read(display);
    }
    wl_display_dispatch_queue_pending(display, queue);
}

void pump_wayland(wl_display* display, bool fd_readable) {
    std::lock_guard<std::mutex> lock(wayland_mutex());
    sync_vk_window_size();
    // Held keys repeat from here: the compositor sends only a press and a
    // release, so the events in between are the client's to make.
    stud::android_glue::native_window_pump_key_repeat();
    // Blink the text overlay's caret. Cheap and self-limiting: it does
    // nothing at all unless a TextBox is focused.
    {
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::milliseconds(530)) {
            last = now;
            stud::android_glue::tick_text_overlay();
        }
    }
    // Only ever Stud's OWN queue. The default queue belongs to the Vulkan
    // driver, which dispatches it itself from inside its present path;
    // dispatching it here consumed its wl_buffer.release events and threw
    // them away (WAYLAND_DEBUG: "discarded wl_buffer#48.release()"), so
    // the compositor never got a showable buffer and the window was black
    // no matter what was drawn into it.
    //
    // read_events still distributes to every queue; that part is shared
    // and correct, but the dispatching is per-queue and stays ours.
    wl_event_queue* queue = stud::android_glue::native_window_wl_queue();
    if (queue == nullptr) {
        // No queue means no Wayland connection; nothing to pump.
        return;
    }
    while (wl_display_prepare_read_queue(display, queue) != 0) {
        wl_display_dispatch_queue_pending(display, queue);
    }
    wl_display_flush(display);
    if (fd_readable) {
        wl_display_read_events(display);
    } else {
        wl_display_cancel_read(display);
    }
    wl_display_dispatch_queue_pending(display, queue);

    // The default queue still has to be drained, and by this process.
    //
    // The Vulkan driver puts its wl_buffer proxies on the default queue
    // but only dispatches it from inside its own present path, which for
    // this engine runs about once a second. Everything queued in between
    // just accumulates, live-caught as 46 discarded wl_buffer.release
    // events per buffer in a WAYLAND_DEBUG trace, meaning the driver
    // never learned its buffers came back and had none free to render
    // into. Attaches and commits still looked perfect, and the window
    // stayed black.
    //
    // Dispatching it here is safe: a proxy with no listener discards its
    // events either way, and the driver's own dispatch remains
    // authoritative for anything it does listen to.
    wl_display_dispatch_pending(display);
}

// Services one client connection to completion, on its own thread. Used
// for Process B's connections beyond the first, each of its stub
// libraries (libGLESv2, libaaudio, ...) links its own copy of the client
// and opens its own socket.

// The calls that BLOCK in the driver, waiting for the GPU.
//
// Everything else is dispatched under one process-wide mutex, which is
// what keeps concurrent connections from reaching the driver at once.
// These four cannot be: each one waits on work that only finishes if the
// rest of the process keeps running, the main loop pumps the display
// and the X11 event queue, and a connection thread does the presenting.
//
// Live-caught as a hang on X11: a drag-resize leaves presents queued
// against an out-of-date swapchain, a connection thread enters
// vkDeviceWaitIdle to tear it down, and the driver never returns because
// the GPU is waiting on a present that only the blocked main loop could
// carry forward. Both threads then wait on each other forever, and it
// grows more likely the longer the drag, which is exactly the reported
// "freezes for good if I hold it too long".
//
// Vulkan's own rule makes this safe: these entry points require external
// synchronisation on the queue or the fences they name, not on the whole
// device, and each of them is already called from one thread at a time.
// The background frame limit, applied BEFORE the dispatch lock is taken.
//
// It used to sleep inside the dispatch handler, which holds the one
// process-wide lock, so throttling to a few frames a second also held
// audio, input polling and everything else on every other connection for
// the same tens of milliseconds. Audio is fed from its own connection
// and stutters if its writes are made to wait; the point of the setting
// is to stop drawing a window nobody is looking at, not to slow the
// process down.
void throttle_before_dispatch(stud::render_host::CallId id) {
    if (id == stud::render_host::CallId::VkQueuePresentKHR ||
        id == stud::render_host::CallId::EglSwapBuffers) {
        throttle_while_hidden();
    }
}

bool blocks_in_the_driver(stud::render_host::CallId id) {
    switch (id) {
        case stud::render_host::CallId::VkDeviceWaitIdle:
        case stud::render_host::CallId::VkWaitForFences:
        case stud::render_host::CallId::VkAcquireNextImageKHR:
        case stud::render_host::CallId::VkQueuePresentKHR:
        // Building a swapchain is a driver call like the waits above, and
        // on X11 it is the longest of them: the driver has to retire
        // everything outstanding on the old swapchain first, and measured
        // during a maximise it spent 22012ms doing so. Held under the
        // dispatch lock, that is 22 seconds in which render-host answers
        // nothing at all -- no input, no window size, no other
        // connection -- which is exactly what a black, dead window on
        // resize looked like. It touches one swapchain and the surface,
        // not the shared GL state the lock exists for.
        case stud::render_host::CallId::VkCreateSwapchainKHR:
        // Audio blocks until the device has taken the samples; that is
        // what paces the engine's mixer, so holding the dispatch lock
        // across it makes every other connection wait on the sound card.
        case stud::render_host::CallId::AudioWriteFrames:
        case stud::render_host::CallId::AudioReadFrames:
        // Input may WAIT for an event to arrive (see PollInputEvents in
        // the protocol header). It holds no driver state at all, and
        // holding the dispatch lock across that wait would stall every
        // other connection for the length of it.
        case stud::render_host::CallId::PollInputEvents:
            return true;
        default:
            return false;
    }
}

// STUD_GL_TRACE_ERRORS=1: ask the real GL for its error after every
// forwarded GL call and name the first call that produced each one.
//
// The client answers glGetError from a cache refreshed once a frame, so
// the engine's own error checks no longer say WHICH call failed. When
// something uploads and does not appear, a texture stuck at its
// smallest mip, say. That is the one thing worth knowing, and it is
// too expensive to leave on.
void trace_gl_error_after(stud::render_host::CallId id, const RealFns& fns) {
    static const bool on = std::getenv("STUD_GL_TRACE_ERRORS") != nullptr;
    if (!on || fns.glGetError_ == nullptr) return;
    const char* name = stud::render_host::call_id_name(id);
    if (name == nullptr || name[0] != 'G' || name[1] != 'l') return;  // GL calls only
    const GLenum err = fns.glGetError_();
    if (err == GL_NO_ERROR) return;
    // Once per call/error pair: a failing call usually fails every frame,
    // and the interesting fact is which ones, not how many times.
    static std::set<std::pair<std::string, unsigned>> seen;
    if (!seen.insert({name, err}).second) return;
    std::printf("stud-render-host: GL ERROR 0x%x after %s\n", err, name);
    std::fflush(stdout);
}


// Ask KWin to focus Stud's own window. See the call site for why this
// exists at all, and why it is deliberately the last thing tried.
// In the anonymous namespace, matching the declaration above, both
// blocks in this file are the same namespace.
namespace {
void raise_through_kwin() {
    // The script goes in Stud's own data directory, not the runtime
    // directory: KWin has to READ this path, and it runs on the host. A
    // Flatpak's runtime directory is private to the sandbox (the real
    // one is buried under /run/user/<uid>/.flatpak/<instance>/), so KWin
    // was handed a path that does not exist for it and the raise
    // silently did nothing, a game opened from the browser stayed
    // behind the window in front. The data directory is a real host path
    // in both cases (~/.local/share/stud, or ~/.var/app/<id>/data/stud).
    const std::string dir = stud::paths::data_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string js_path = dir + "/kwin-raise.js";
    {
        std::ofstream js(js_path, std::ios::trunc);
        if (!js) return;
        // windowList() is KWin 6, clientList() KWin 5; activeWindow was
        // activeClient before 6. Both spellings, so this does not depend
        // on which one the session happens to be running.
        js << "var l = (typeof workspace.windowList === 'function')\n"
              "        ? workspace.windowList() : workspace.clientList();\n"
              "for (var i = 0; i < l.length; i++) {\n"
              "  var w = l[i]; if (!w) continue;\n"
              "  var c = String(w.resourceClass || '');\n"
              "  if (c.indexOf('" STUD_APP_ID "') !== -1 || c.toLowerCase() === 'stud') {\n"
              "    if ('activeWindow' in workspace) workspace.activeWindow = w;\n"
              "    else workspace.activeClient = w;\n"
              "    break;\n"
              "  }\n"
              "}\n";
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
        // Load, run, unload. A script left loaded would accumulate one
        // entry per link clicked for the life of the session.
        const std::string cmd =
            "id=$(gdbus call --session --dest org.kde.KWin --object-path /Scripting "
            "--method org.kde.kwin.Scripting.loadScript '" + js_path + "' studraise "
            "2>/dev/null | grep -oE '[0-9]+' | head -1); "
            "[ -n \"$id\" ] && gdbus call --session --dest org.kde.KWin "
            "--object-path /Scripting/Script$id --method org.kde.kwin.Script.run >/dev/null 2>&1; "
            "gdbus call --session --dest org.kde.KWin --object-path /Scripting "
            "--method org.kde.kwin.Scripting.unloadScript studraise >/dev/null 2>&1";
        ::execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        ::_exit(127);
    }
    if (pid > 0) {
        // Reaped off-thread so this never blocks the dispatch it runs on.
        std::thread([pid] { int st = 0; ::waitpid(pid, &st, 0); }).detach();
    }
}
}  // namespace

void serve_connection_thread(int conn_fd, const RealFns& fns, RealWindow& real_window) {
    std::vector<unsigned char> in_scratch;
    std::vector<unsigned char> out_scratch;
    for (;;) {
        Header hdr{};
        if (!read_all(conn_fd, &hdr, sizeof(hdr))) break;
        in_scratch.resize(hdr.in_buffer_len);
        if (hdr.in_buffer_len > 0 && !read_all(conn_fd, in_scratch.data(), hdr.in_buffer_len)) {
            break;
        }
        out_scratch.clear();
        uint32_t out_len = 0;
        uint64_t result = 0;
        throttle_before_dispatch(static_cast<stud::render_host::CallId>(hdr.call_id));
        if (blocks_in_the_driver(static_cast<stud::render_host::CallId>(hdr.call_id))) {
            result = dispatch(hdr, fns, real_window, in_scratch, out_scratch, &out_len);
        } else {
            std::lock_guard<std::mutex> lock(dispatch_mutex());
            result = dispatch(hdr, fns, real_window, in_scratch, out_scratch, &out_len);
        }
        // Pump Wayland here, off this thread's own back, right after the
        // dispatch mutex is released.
        //
        // Presentation runs on THIS thread: the engine's Vulkan client is
        // a secondary connection, so vkQueuePresentKHR is handled here,
        // not on the main loop. A Wayland-backed driver needs the display
        // dispatched to finish presenting, and the main loop, the only
        // thing that pumped it, spends its time blocked on the same
        // dispatch mutex this thread just held. So buffers were attached
        // and committed, every call returned VK_SUCCESS, and nothing was
        // ever shown.
        //
        // Live-caught by printing the presenting thread id: presents ran
        // on a secondary connection thread while the main loop sat
        // elsewhere. The standalone stud_try_vulkan_window test, which
        // does everything on one thread, presented 300 frames correctly
        // on the same GPU and window. That is what narrowed it here.
        //
        // The `true` is fd_readable, and it is deliberate rather than an
        // oversight, which is worth writing down because it reads as one:
        // the main loop below asks poll() and passes the answer, and this
        // caller asserts it on every present.
        //
        // It is safe. libwayland reads the socket with MSG_DONTWAIT, so
        // wl_display_read_events() on an empty socket returns instead of
        // blocking -- and a present path that blocked on an empty socket
        // would have wedged Stud on its first frame rather than rarely.
        // Asking poll() here instead was tried on paper and is worse: it
        // costs the same syscall it saves, and it opens a window where
        // data arriving between the poll and the read is left for the
        // next pump, which delays exactly the buffer-release events this
        // call exists to dispatch.
        if (hdr.call_id == CallId::VkQueuePresentKHR) {
            pump_display(real_window, true);
        }
        if ((hdr.flags & Header::kNoReply) != 0) continue;
        ResponseHeader resp{result, out_len};
        if (!write_all(conn_fd, &resp, sizeof(resp))) break;
        if (out_len > 0 && !write_all(conn_fd, out_scratch.data(), out_len)) break;
    }
    ::close(conn_fd);
    std::printf("stud-render-host: secondary client disconnected\n");
    std::fflush(stdout);
}

// Accepts anything already waiting in the backlog and hands each one to
// its own thread. Called whenever the main loop has just accepted a
// connection, and again each time round, so a library that connects later
// (libaaudio only connects when the engine first opens audio) is picked
// up promptly rather than blocking forever.
// Leaves immediately rather than returning through main().
//
// Static destruction here is a real hazard, not a tidiness question: the
// audio device's destructor joins its opener thread, ANGLE tears down a
// context this process may no longer own, and any of it can block. While
// it blocks the Wayland connection is still open and the surface still
// mapped, so the compositor keeps pinging a client that has stopped
// answering, which is exactly KDE's "Stud is not responding". The
// window is going away; the kernel reclaims everything this process
// holds, so there is nothing here worth the risk of hanging.
// Holds the single-instance lock for as long as this process lives.
//
// The lock lives with render-host rather than with stud-ui because
// stud-ui is designed to hand off and exit immediately, a lock it held
// would be released the moment the game started. render-host lives
// exactly as long as the session does, and the kernel releases an flock
// when the holder dies however it dies, so there is no stale state to
// clean up after a crash or a kill.
bool acquire_single_instance_lock() {
    const std::string socket_path = stud::render_host::default_socket_path();
    const auto slash = socket_path.find_last_of('/');
    if (slash == std::string::npos) return true;
    const std::string path = socket_path.substr(0, slash) + "/stud.lock";
    // Deliberately never closed: the lock is held for the process's whole
    // life, and closing any fd on this file would drop it.
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return true;  // Cannot lock: do not refuse to start over it.
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    return true;
}

void serve_extra_connections(int listen_fd, const RealFns& fns, RealWindow& real_window) {
    for (;;) {
        pollfd pfd{listen_fd, POLLIN, 0};
        if (::poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) return;
        int extra_fd = ::accept(listen_fd, nullptr, nullptr);
        if (extra_fd < 0) return;
        std::printf("stud-render-host: secondary client connected\n");
        std::fflush(stdout);
        std::thread(serve_connection_thread, extra_fd, std::cref(fns),
                    std::ref(real_window)).detach();
    }
}


int main(int argc, char** argv) {
    // Tee this process's output into the session log Process A named, so
    // one file carries all three processes' diagnostics. Nothing is taken
    // away from the terminal or the journal (stud/session_log.h).
    stud::logging::start_session_log_from_env();
    stud::logging::install_crash_reporter("stud-render-host");

    // A web-view viewer is a session leader of its own, so nothing takes
    // it down with this process, live-reported as a panel still on
    // screen after Stud had closed. The ordinary shutdown paths close
    // them explicitly; this covers being killed instead. Handler-safe:
    // kill() and _exit() are both async-signal-safe, and the pid list is
    // only appended to from the dispatch thread.
    struct sigaction sa{};
    sa.sa_handler = +[](int sig) {
        close_open_web_views();
        // Default disposition, then re-raise, so the exit status is what
        // the signal would really have produced.
        ::signal(sig, SIG_DFL);
        ::raise(sig);
    };
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGHUP, &sa, nullptr);

    // Real Stud-level graphics-mode selection, from Process A's Settings
    // (never an FFlag; see the call site in ui/src/main.cpp).
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--background-fps" && i + 1 < argc) {
            // Settings' own slider. STUD_BACKGROUND_FPS still wins, since
            // that is what a measurement run sets.
            if (std::getenv("STUD_BACKGROUND_FPS") == nullptr) {
                ::setenv("STUD_BACKGROUND_FPS", argv[i + 1], 0);
            }
        }
        if (std::string_view(argv[i]) == "--graphics-mode") {
            g_prefer_vulkan = std::string_view(argv[i + 1]) != "opengl";
        } else if (std::string_view(argv[i]) == "--assets-dir") {
            // Roblox's own extracted assets: where the text overlay finds
            // the engine's real fonts and their mapping table.
            assets_dir() = argv[i + 1];
        } else if (std::string_view(argv[i]) == "--discord-presence") {
            g_discord_enabled = std::string_view(argv[i + 1]) == "on";
        } else if (std::string_view(argv[i]) == "--discord-join-button") {
            g_discord_join_button = std::string_view(argv[i + 1]) == "on";
        } else if (std::string_view(argv[i]) == "--hidpi") {
            g_hidpi_enabled = std::string_view(argv[i + 1]) != "off";
        } else if (std::string_view(argv[i]) == "--upscaling") {
            g_upscaling_enabled = std::string_view(argv[i + 1]) == "on";
            // STUD_UPSCALING=off|on overrides the setting without writing
            // to it, so the pass can be A/B'd against a frame-pacing
            // question in one relaunch and the user's own configuration
            // is left exactly as they set it.
            if (const char* env = std::getenv("STUD_UPSCALING")) {
                g_upscaling_enabled = std::string_view(env) == "on";
                std::printf("stud-render-host: STUD_UPSCALING=%s overrides the setting\n", env);
                std::fflush(stdout);
            }
        } else if (std::string_view(argv[i]) == "--upscale-sharpness") {
            const int percent = std::atoi(argv[i + 1]);
            if (percent >= 0 && percent <= 125) g_upscale_sharpness_percent = percent;
        }
    }
    // Latch the scale before any window or surface exists, so the very
    // first buffer is already the right size.
    // HiDPI on: the buffer follows the display's scale (0 means exactly
    // that). Off: the buffer is the window's logical size and the
    // compositor upscales it. Either way the DISPLAY's own scale is left
    // alone. That separation is what makes the off state merely blurry
    // instead of also wrong.
    stud::android_glue::set_render_scale_120(g_hidpi_enabled ? 0 : 120);

    // Stud's own upscaler.
    //
    // The engine renders below the screen's resolution at scale 1.0:
    // correct UI, rounded corners, SSAO, all of it, and Stud builds the
    // presented frame from that image rather than letting the compositor
    // stretch it. The engine's own scale is never touched, because it
    // cannot be: below 1.0 it draws square corners, above it drops SSAO.
    //
    // The quality preset is how far below the screen the engine renders.
    // The OUTPUT is not a setting: it is always the window's size in the
    // display's own pixels, pushed on every resize by sync_vk_window_size.
    if (g_upscaling_enabled) {
        // The engine is PINNED to the window's logical size, what Stud
        // renders with HiDPI off, and never moves. That is not a
        // simplification: raising it lays the UI out in more pixels and
        // makes it visibly small, lowering it makes it large, and
        // compensating through the engine's own DPI scale makes it draw
        // square corners. All three measured on screen.
        stud::android_glue::set_render_scale_120(120);
        stud::render_host::vk_set_upscale_sharpness_percent(g_upscale_sharpness_percent);
        std::printf("stud-render-host: upscaling: the engine renders at the window's logical "
                    "size, Stud writes the window's own resolution, sharpening %d%%\n",
                    g_upscale_sharpness_percent);
        std::fflush(stdout);
    }

    // Open the audio device now, not when a sound first plays: Stud should
    // appear in the desktop's volume mixer from launch, like any other
    // application, so its volume can be set before anything makes noise.
    // Opening talks to the audio server and can block, so it happens on
    // its own thread, never on the dispatch loop.
    stud::render_host::audio_start_output_device();
    // Caught in testing: diagnostic bug: this process's stdout is a
    // redirected file (stud-ui starts it detached, inheriting stdout),
    // so libc block-buffers it and nothing written after the first
    // partial block ever reaches the log while the process stays alive
    // which made every per-call diagnostic here look like "the call
    // never happened" rather than "the line is still in the buffer."
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    // The user's own graphics-mode choice, honoured rather than merely
    // recorded. Choosing OpenGL means this process does not offer Vulkan
    // at all, so vkCreateInstance answers VK_ERROR_INCOMPATIBLE_DRIVER
    // and the engine takes its own GLES path, the same thing it does
    // on a device whose driver it cannot use. Nothing here names an
    // FFlag or depends on any Roblox build's internals.
    if (!acquire_single_instance_lock()) {
        std::fprintf(stderr, "stud-render-host: another instance is already running\n");
        return 0;
    }

    // Discord rich presence, if the user enabled it. Connecting when
    // Discord is not running is not an error; it retries on the next
    // presence change.
    if (g_discord_enabled) {
        stud::render_host::discord_rpc_start(stud::render_host::kDiscordApplicationId);
    }

    stud::render_host::vk_set_vulkan_enabled(g_prefer_vulkan);
    std::printf("stud-render-host: graphics mode: %s\n", g_prefer_vulkan ? "vulkan" : "opengl");

    // The GPU chosen in Settings, which nothing read back until now, so
    // picking a GPU there changed nothing on either path.
    uint32_t preferred_gpu = 0;
    try {
        preferred_gpu = stud::config::load_settings(stud::config::default_config_path()).gpu.device_index;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stud-render-host: ignoring the saved GPU choice: %s\n", e.what());
    }
    stud::render_host::vk_set_preferred_device_index(preferred_gpu);

    std::string egl_path, gles_path;
#ifdef STUD_ENABLE_DEV_RENDER_TOGGLE
    // Which backend ANGLE translates GLES to. Only meaningful in OpenGL
    // mode, in Vulkan mode the engine makes no GLES call at all, so
    // there is nothing for ANGLE to translate.
    std::optional<stud::render::DevRenderBackendConfig> dev_backend;
    try {
        dev_backend = stud::render::load_dev_render_backend_config(stud::config::default_config_path());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stud-render-host: ignoring the render backend setting: %s\n", e.what());
    }
    if (dev_backend && !g_prefer_vulkan) {
        switch (dev_backend->mode) {
            case stud::render::DevRenderBackendMode::kAngleDesktopGL:
                g_angle_backend = "gl";
                break;
            case stud::render::DevRenderBackendMode::kAngleSwiftShader:
                g_angle_backend = "swiftshader";
                break;
            case stud::render::DevRenderBackendMode::kAngleVulkan:
                break;
        }
    }
    if (egl_path.empty()) {
        // Looked up every launch, never read out of the config: where
        // ANGLE is depends on how Stud was started, not on what some
        // earlier run happened to find. See dev_backend_config.h.
        try {
            auto paths = stud::render::shipped_angle_paths();
            egl_path = paths.egl_path;
            gles_path = paths.gles_path;
        } catch (const stud::render::ShippedAngleNotFound& e) {
            std::fprintf(stderr, "stud-render-host: %s\n", e.what());
            return 1;
        }
    }
#else
    std::fprintf(stderr, "stud-render-host: built without STUD_ENABLE_DEV_RENDER_TOGGLE\n");
    return 1;
#endif
    // STUD_ANGLE_BACKEND still overrides, so a backend can be tried
    // without going through the Settings window.
    if (const char* env = std::getenv("STUD_ANGLE_BACKEND"); env != nullptr) g_angle_backend = env;
    std::printf("stud-render-host: ANGLE backend: %s\n",
                g_angle_backend.empty() ? "vulkan (default)" : g_angle_backend.c_str());
    std::printf("stud-render-host: ANGLE build: %s / %s\n", egl_path.c_str(), gles_path.c_str());

    try {
        stud::render::set_angle_library_paths(egl_path, gles_path);
    } catch (const stud::render::LoadError& e) {
        std::fprintf(stderr, "stud-render-host: failed to load render backend: %s\n", e.what());
        return 1;
    }

    ANativeWindow* window = ANativeWindow_fromSurface(nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "stud-render-host: ANativeWindow_fromSurface returned null\n");
        return 1;
    }
    // Hand the Vulkan layer this one window's size, once. It must never
    // derive a window itself: ANativeWindow_fromSurface(nullptr, nullptr)
    // CREATES a new Wayland window whenever the window cache is empty,
    // and a null Surface never populates that cache, so calling it from
    // a query the engine repeats spawned over a hundred real windows.
    g_real_window = window;

    // Controllers, if any are plugged in and readable.
    stud::render_host::gamepad::init();
    stud::render_host::vk_set_window_size(
        static_cast<uint32_t>(ANativeWindow_getWidth(window)),
        static_cast<uint32_t>(ANativeWindow_getHeight(window)));

    const bool on_x11 =
        stud::android_glue::display_backend() == stud::android_glue::DisplayBackend::X11;
    wl_display* display = nullptr;
    wl_surface* surface = nullptr;
    void* x11_display = nullptr;
    unsigned long x11_window = 0;
    if (on_x11) {
        x11_display = stud::android_glue::native_window_x11_display();
        x11_window = stud::android_glue::native_window_x11_window();
        if (x11_display == nullptr || x11_window == 0) {
            std::fprintf(stderr, "stud-render-host: the X11 backend has no window\n");
            return 1;
        }
        stud::render_host::vk_set_on_x11(true);
        std::printf("stud-render-host: display backend: X11\n");
        std::fflush(stdout);
    } else {
        display = stud::android_glue::native_window_wl_display(window);
        surface = stud::android_glue::native_window_wl_surface(window);
        if (display == nullptr || surface == nullptr) {
            std::fprintf(stderr,
                         "stud-render-host: no display server reachable, neither a Wayland "
                         "compositor nor an X server answered\n");
            return 1;
        }
    }
    // The window-size unification (confirmed good): render-host, Process B's
    // lifecycle surface and DisplayMetrics all take their size from
    // android-glue's own ANativeWindow, instead of three different literals.
    const int32_t win_w = ANativeWindow_getWidth(window);
    const int32_t win_h = ANativeWindow_getHeight(window);
    // Must go through android-glue rather than calling wl_egl_window_create()
    // directly: android-glue's xdg_toplevel configure handler resizes
    // `window->egl_window`, and creating the wl_egl_window behind its back
    // left that null forever. The compositor's resize requests arrived and
    // were silently dropped, the frame grew while the buffer stayed at its
    // original size, showing the desktop through the rest of the window.
    // X11 has no wl_egl_window: EGL takes the X window id directly, so
    // there is nothing to create and nothing to resize behind the
    // server's back.
    wl_egl_window* egl_window = nullptr;
    if (!on_x11) {
        egl_window =
            stud::android_glue::native_window_get_or_create_egl_window(window, win_w, win_h);
        if (egl_window == nullptr) {
            std::fprintf(stderr, "stud-render-host: wl_egl_window_create failed\n");
            return 1;
        }
    }
    RealWindow real_window{display, egl_window, surface, x11_display, x11_window};

    RealFns fns{};
#define RESOLVE(name) fns.name##_ = must_resolve<PFN_##name>(#name)
#define RESOLVE_OPTIONAL(name) fns.name##_ = may_resolve<PFN_##name>(#name)
    RESOLVE(eglGetDisplay); RESOLVE(eglInitialize); RESOLVE(eglBindAPI); RESOLVE(eglChooseConfig);
    RESOLVE(eglCreateWindowSurface); RESOLVE(eglCreatePbufferSurface); RESOLVE(eglCreateContext);
    RESOLVE(eglMakeCurrent); RESOLVE(eglSwapBuffers); RESOLVE(eglGetError); RESOLVE(eglQueryString);
    RESOLVE(eglDestroyContext); RESOLVE(eglDestroySurface); RESOLVE(eglGetConfigAttrib);
    RESOLVE(eglGetCurrentContext); RESOLVE(eglQuerySurface); RESOLVE(eglSwapInterval);
    RESOLVE(eglTerminate); RESOLVE(eglGetProcAddress);
    // Optional on purpose: only ANGLE exports it, and host Mesa (the Zink
    // path) does not, making it required is what killed render-host at
    // startup the first time Zink was actually selected.
    fns.eglGetPlatformDisplayEXT_ =
        reinterpret_cast<PFN_eglGetPlatformDisplayEXT>(stud::render::resolve("eglGetPlatformDisplayEXT"));
    RESOLVE(glActiveTexture); RESOLVE(glAttachShader); RESOLVE(glBindBuffer); RESOLVE(glBindFramebuffer);
    RESOLVE(glBindRenderbuffer); RESOLVE(glBindTexture); RESOLVE(glBlendFunc); RESOLVE(glBlendFuncSeparate);
    RESOLVE(glCheckFramebufferStatus); RESOLVE(glClear); RESOLVE(glClearColor); RESOLVE(glClearDepthf);
    RESOLVE(glClearStencil); RESOLVE(glColorMask); RESOLVE(glCompileShader); RESOLVE(glCopyTexSubImage2D);
    RESOLVE(glCreateProgram); RESOLVE(glCreateShader); RESOLVE(glCullFace); RESOLVE(glDeleteProgram);
    RESOLVE(glDeleteShader); RESOLVE(glDepthFunc); RESOLVE(glDepthMask); RESOLVE(glDisable);
    RESOLVE(glDisableVertexAttribArray); RESOLVE(glDrawArrays); RESOLVE(glDrawElements); RESOLVE(glEnable);
    RESOLVE(glEnableVertexAttribArray); RESOLVE(glFramebufferRenderbuffer); RESOLVE(glFramebufferTexture2D);
    RESOLVE(glGenerateMipmap); RESOLVE(glGetError); RESOLVE(glLinkProgram); RESOLVE(glPixelStorei);
    RESOLVE(glPolygonOffset); RESOLVE(glReleaseShaderCompiler); RESOLVE(glRenderbufferStorage);
    RESOLVE(glScissor); RESOLVE(glStencilFunc); RESOLVE(glStencilMask); RESOLVE(glStencilOp);
    RESOLVE(glTexParameterf); RESOLVE(glTexParameteri); RESOLVE(glUniform1i); RESOLVE(glUseProgram);
    RESOLVE(glViewport); RESOLVE(glVertexAttribPointer); RESOLVE(glGetString); RESOLVE(glGetUniformLocation);
    RESOLVE(glBindAttribLocation); RESOLVE(glShaderSource); RESOLVE(glGetProgramInfoLog);
    RESOLVE(glGetShaderInfoLog); RESOLVE(glGetActiveUniform); RESOLVE(glDeleteBuffers);
    RESOLVE(glDeleteFramebuffers); RESOLVE(glDeleteRenderbuffers); RESOLVE(glDeleteTextures);
    RESOLVE(glGenVertexArrays); RESOLVE(glBindVertexArray); RESOLVE(glDeleteVertexArrays);
    RESOLVE(glBindBufferRange); RESOLVE(glClearBufferfv); RESOLVE(glDrawBuffers);
    RESOLVE(glRenderbufferStorageMultisample); RESOLVE(glInvalidateFramebuffer);
    RESOLVE(glBlitFramebuffer);
    RESOLVE(glDrawArraysInstanced); RESOLVE(glDrawElementsInstanced);
    RESOLVE(glVertexAttribDivisor); RESOLVE(glVertexAttribIPointer);
    RESOLVE(glBindBufferBase);
    RESOLVE(glFenceSync); RESOLVE(glClientWaitSync); RESOLVE(glWaitSync);
    RESOLVE(glDeleteSync); RESOLVE(glIsSync); RESOLVE(glGetSynciv);
    RESOLVE(glCopyImageSubData);
    RESOLVE(glTexStorage2D); RESOLVE(glTexStorage3D); RESOLVE(glTexSubImage3D);
    RESOLVE(glProgramParameteri); RESOLVE(glGetUniformBlockIndex);
    RESOLVE(glUniformBlockBinding); RESOLVE(glGetActiveUniformBlockiv);
    RESOLVE(glGenBuffers); RESOLVE(glGenFramebuffers); RESOLVE(glGenRenderbuffers); RESOLVE(glGenTextures);
    RESOLVE(glGetIntegerv); RESOLVE(glIsEnabled); RESOLVE(glTexParameterfv); RESOLVE(glGetProgramiv); RESOLVE(glGetShaderiv); RESOLVE(glGetShaderSource);
    RESOLVE_OPTIONAL(glGenQueries); RESOLVE_OPTIONAL(glDeleteQueries);
    RESOLVE_OPTIONAL(glBeginQuery); RESOLVE_OPTIONAL(glEndQuery);
    RESOLVE_OPTIONAL(glGetQueryObjectuiv); RESOLVE_OPTIONAL(glGetQueryObjectui64v);
    RESOLVE(glBufferData); RESOLVE_OPTIONAL(glBufferStorage); RESOLVE(glBufferSubData); RESOLVE(glTexImage2D); RESOLVE(glTexSubImage2D);
    RESOLVE(glMapBufferRange); RESOLVE(glUnmapBuffer);
    RESOLVE(glGetVertexAttribiv); RESOLVE(glGetVertexAttribPointerv);
    RESOLVE(glCompressedTexImage2D); RESOLVE(glCompressedTexSubImage2D); RESOLVE(glReadPixels);
#undef RESOLVE

    // Real Vulkan loader, for the vkCreateAndroidSurfaceKHR redirect,
    // same real system libvulkan.so.1 (or ANGLE's own bundled one) any
    // native Vulkan app on this host would load.
    g_vulkan_handle = ::dlopen("libvulkan.so.1", RTLD_NOW);
    if (g_vulkan_handle != nullptr) {
        g_real_vk_get_instance_proc_addr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(::dlsym(g_vulkan_handle, "vkGetInstanceProcAddr"));
    }

    std::string socket_path = stud::render_host::default_socket_path();
    std::string parent = socket_path.substr(0, socket_path.find_last_of('/'));
    ::mkdir(parent.c_str(), 0700);
    ::unlink(socket_path.c_str());

    int listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) { std::perror("stud-render-host: socket"); return 1; }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::perror("stud-render-host: bind"); return 1;
    }
    if (::listen(listen_fd, 1) != 0) { std::perror("stud-render-host: listen"); return 1; }
    std::printf("stud-render-host: listening on %s\n", socket_path.c_str());

    // User-reported bug, fixed: both accept() and read_all() below
    // used to block indefinitely with nothing servicing this window's
    // own Wayland connection in between, so xdg_wm_base's real ping
    // (which MUST be answered with a pong or the compositor marks the
    // window "not responding") went unanswered for however long Process
    // B took to send its next request (e.g. this project's own ~8s-per-
    // call bounded waits during real engine bring-up, well past a real
    // compositor's own ping timeout). Fixed by polling the real Wayland
    // display fd (wl_display_get_fd()) alongside the socket fd instead
    // of blocking on the socket alone, dispatching real Wayland events
    // on every tick regardless of client activity. Same poll also
    // services xdg_toplevel's close event (see native_window.cpp's own
    // fix, close used to be a real no-op, so the window could never
    // be closed at all): checked every tick, both here and in the inner
    // per-connection loop, so a click closes the window promptly
    // whether or not a client happens to be connected.
    // The display server's own socket, whichever it is: poll()ing it is
    // what lets the loop sleep instead of spinning, and it is the same
    // idea either way.
    int wl_fd = real_window.on_x11() ? stud::android_glue::native_window_x11_fd()
                                      : wl_display_get_fd(real_window.display);
    for (;;) {
        if (stud::android_glue::window_close_requested()) {
            std::printf("stud-render-host: window close requested, shutting down\n");
            close_open_web_views(/*wait_for_exit=*/true);
            exit_now(0);
        }
        pollfd pfds[2] = {{listen_fd, POLLIN, 0}, {wl_fd, POLLIN, 0}};
        // STUD_WL_POLL_MS: how long this loop may block before pumping
        // Wayland again. The Vulkan driver reads the display fd itself,
        // so Stud's own queue often has events waiting while this fd
        // never becomes readable: meaning this timeout, not the fd, is
        // what decides how often buffer releases get dispatched.
        ::poll(pfds, 2, wayland_poll_ms());
        // Never wl_display_dispatch() here; see pump_wayland's own
        // comment: it can park this loop forever once ANGLE reads the
        // same queue from a render thread.
        pump_display(real_window, (pfds[1].revents & POLLIN) != 0);
        if (!(pfds[0].revents & POLLIN)) continue;
        int conn_fd = ::accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0) { std::perror("stud-render-host: accept"); continue; }
        std::printf("stud-render-host: client connected\n");
        // Process B connects more than once: each of its stub libraries
        // (libGLESv2, libaaudio, ...) links its own copy of the client and
        // opens its own socket. This loop services one connection inline,
        // so any further one used to sit in the accept backlog forever,
        // live-caught as the app freezing the instant a game started,
        // because that is when the engine's FMOD opens its audio device
        // and libaaudio's very first call blocked waiting for a reply that
        // could never come.
        //
        // Extra connections are served on their own threads, serialised
        // against this one by a dispatch mutex (the GL state and EGL
        // context they share are not thread-safe). The GL connection keeps
        // running inline so it also pumps the compositor, exactly as
        // before.
        serve_extra_connections(listen_fd, fns, real_window);
        for (;;) {
            if (stud::android_glue::window_close_requested()) {
                // Not straight out of the door. Leaving a running
                // experience is a real message to a real Roblox server,
                // and only the engine can send it -- which it does when
                // Process B calls nativeAppBridgeV2LeaveGame on its way
                // out. That call does real render work, so this process
                // has to still be here to answer it.
                //
                // Exiting the instant the X was clicked took the
                // connection away first: Process B saw a dead host,
                // skipped its own teardown (there being nothing to tear
                // down against) and exited, so the leave was never sent
                // and the account stayed in the server until the server
                // timed it out. Live-reported.
                //
                // So the close is reported through
                // CallId::PollWindowCloseRequested, which Process B polls
                // every tick, and this loop keeps serving until it
                // disconnects of its own accord. Bounded, because a
                // teardown that hangs must not leave a window nobody can
                // close: at that point the leave is lost either way and
                // the window is what the user asked for.
                static auto closing_since = std::chrono::steady_clock::now();
                static bool announced = false;
                if (!announced) {
                    announced = true;
                    closing_since = std::chrono::steady_clock::now();
                    std::printf("stud-render-host: window close requested, letting the engine "
                                "leave the experience first\n");
                    std::fflush(stdout);
                }
                const double waited_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - closing_since).count();
                if (waited_ms > 6000.0) {
                    std::printf("stud-render-host: the engine did not finish leaving in %.0fms; "
                                "closing anyway\n", waited_ms);
                    close_open_web_views(/*wait_for_exit=*/true);
                    ::close(conn_fd);
                    ::close(listen_fd);
                    exit_now(0);
                }
            }
            pollfd cpfds[2] = {{conn_fd, POLLIN, 0}, {wl_fd, POLLIN, 0}};
            // Pipelined requests arrive in bulk, so drain what is already
            // buffered before going back to poll(): one poll per request was
            // itself a large share of the per-frame cost. FIONREAD is an
            // honest "is there already a whole request waiting", checked.
            serve_extra_connections(listen_fd, fns, real_window);
            int pending = 0;
            const bool have_buffered =
                ::ioctl(conn_fd, FIONREAD, &pending) == 0 &&
                static_cast<size_t>(pending) >= sizeof(Header);
            if (!have_buffered) {
                ::poll(cpfds, 2, wayland_poll_ms());
                pump_display(real_window, (cpfds[1].revents & POLLIN) != 0);
                if (!(cpfds[0].revents & POLLIN)) continue;
            }
            Header hdr{};
            if (!read_all(conn_fd, &hdr, sizeof(hdr))) {
                std::printf("stud-render-host: client disconnected\n");
                break;
            }
            g_in_scratch.resize(hdr.in_buffer_len);
            if (hdr.in_buffer_len > 0 && !read_all(conn_fd, g_in_scratch.data(), hdr.in_buffer_len)) {
                break;
            }
            g_out_scratch.clear();
            uint32_t out_len = 0;
            uint64_t result = 0;
            // Same exemption as the connection threads: a blocking
            // driver wait must not hold the dispatch lock, or the two
            // wait on each other.
            throttle_before_dispatch(static_cast<stud::render_host::CallId>(hdr.call_id));
            if (blocks_in_the_driver(static_cast<stud::render_host::CallId>(hdr.call_id))) {
                result = dispatch(hdr, fns, real_window, g_in_scratch, g_out_scratch, &out_len);
            } else {
                std::lock_guard<std::mutex> lock(dispatch_mutex());
                result = dispatch(hdr, fns, real_window, g_in_scratch, g_out_scratch, &out_len);
            }
            trace_gl_error_after(static_cast<stud::render_host::CallId>(hdr.call_id), fns);
            // A pipelined request wants no answer, writing one would desync
            // the stream, since the client is not going to read it.
            if ((hdr.flags & Header::kNoReply) != 0) continue;
            ResponseHeader resp{result, out_len};
            if (!write_all(conn_fd, &resp, sizeof(resp))) break;
            if (out_len > 0 && !write_all(conn_fd, g_out_scratch.data(), out_len)) break;
        }
        // The engine's connection is gone, so the session is over.
        //
        // This loop used to go back to accept(), on the reasoning that
        // render-host should outlive one client dying. Nothing ever
        // reconnects: Process A exits right after launching, so the only
        // client that ever arrives is the Process B it started. Waiting
        // for a second one left a real window on screen with no engine
        // behind it, live-reported as Stud freezing when the in-game
        // leave button is used with "Close Stud when leaving a game" on,
        // which is exactly the case where Process B exits first and the
        // window is meant to go with it.
        std::printf("stud-render-host: the engine disconnected, shutting down\n");
        // Let go of the Discord socket. discord_rpc_set_game({}) already
        // clears what is displayed when a game ends; this is the other
        // half, for the process ending.
        stud::render_host::discord_rpc_stop();
        close_open_web_views(/*wait_for_exit=*/true);
        ::close(conn_fd);
        ::close(listen_fd);
        exit_now(0);
    }
}
