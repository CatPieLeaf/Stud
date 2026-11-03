// stud-render-host: a real, ordinary glibc process hosting the system's
// EGL and GLES, the real window, and the real Vulkan loader, everything
// render/, vulkan-wsi/ and android-glue's native_window.cpp already do,
// unmodified, just running in its own process instead of linked into the
// main runtime.
//
// Why a separate process rather than loading the host's glibc graphics
// libraries into bionic Process B directly: that would also require
// hand-loading real glibc's OWN libc.so.6/libstdc++.so.6 (IFUNC resolvers
// selected by CPUID, GNU symbol versioning, glibc's own TLS models) to
// satisfy their transitive imports, dramatically harder and more fragile
// than anything hand-loaded so far in this project. A real, separate
// glibc process sidesteps that entirely: the driver loads through an
// ordinary glibc dlopen(), with no hand-parsing at all. Same pattern
// Chrome's own GPU process uses in production.
//
// Covers the full real GL/EGL symbol surface libroblox.so's own dynamic
// symbol table imports (85 entries, confirmed via `the ELF headers --dyn-syms`,
// checked). Vulkan gets a deliberately narrower treatment; see
// render_host_protocol.h's own doc comment for why.

#include "flight_recorder.h"
#include "gfxstream_host.h"
#include "socket_reader.h"
#include "stud/transport_ring.h"
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
#include "stud/video_codec.h"
#include "stud/gamepad.h"
#include "stud/render_host_protocol.h"
#include "stud/discord_rpc.h"
#include "stud/vulkan_host.h"
#include "stud/settings.h"

#include <wayland-client.h>

// Through volk, like vulkan_host.cpp: the two surface commands below are
// its globals, loaded with the engine's instance. The Wayland and Xlib
// platforms are defined for the whole target (render-host/CMakeLists.txt);
// the X11 window backend needs the Xlib WSI structs, while Xlib itself is
// still only ever reached through android-glue, which dlopens it.
#include "volk.h"

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
#include <condition_variable>
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
#include <climits>
#include <linux/futex.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
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

// Serialises every dispatch; defined next to wayland_mutex(). Declared here
// for client_wait_sync(), which takes it per poll instead of for the wait.
std::mutex& dispatch_mutex();

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

// Defined far below, next to the frame caps themselves. Declared here
// because the command-batch loop has to run it for each sub-call, and
// that loop sits well above the definition.
void throttle_before_dispatch(stud::render_host::CallId id, const std::vector<uint8_t>& in);

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


using PFN_eglInitialize = EGLBoolean (*)(EGLDisplay, EGLint*, EGLint*);
using PFN_eglBindAPI = EGLBoolean (*)(EGLenum);
using PFN_eglChooseConfig = EGLBoolean (*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
using PFN_eglCreateWindowSurface = EGLSurface (*)(EGLDisplay, EGLConfig, EGLNativeWindowType,
                                                   const EGLint*);
using PFN_eglCreateContext = EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
using PFN_eglMakeCurrent = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
using PFN_eglSwapBuffers = EGLBoolean (*)(EGLDisplay, EGLSurface);
using PFN_eglGetError = EGLint (*)(void);
using PFN_eglQueryString = const char* (*)(EGLDisplay, EGLint);
using PFN_eglQuerySurface = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint*);
using PFN_eglSwapInterval = EGLBoolean (*)(EGLDisplay, EGLint);
using PFN_glActiveTexture = void (*)(GLenum);
using PFN_glAttachShader = void (*)(GLuint, GLuint);
using PFN_glBindFramebuffer = void (*)(GLenum, GLuint);
using PFN_glBindTexture = void (*)(GLenum, GLuint);
using PFN_glCompileShader = void (*)(GLuint);
using PFN_glCreateProgram = GLuint (*)(void);
using PFN_glCreateShader = GLuint (*)(GLenum);
using PFN_glDeleteProgram = void (*)(GLuint);
using PFN_glDeleteShader = void (*)(GLuint);
using PFN_glDrawArrays = void (*)(GLenum, GLint, GLsizei);
using PFN_glFramebufferTexture2D = void (*)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_glLinkProgram = void (*)(GLuint);
using PFN_glTexParameteri = void (*)(GLenum, GLenum, GLint);
using PFN_glUniform1i = void (*)(GLint, GLint);
using PFN_glUseProgram = void (*)(GLuint);
using PFN_glViewport = void (*)(GLint, GLint, GLsizei, GLsizei);
using PFN_glGetUniformLocation = GLint (*)(GLuint, const GLchar*);
using PFN_glShaderSource = void (*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_glGetProgramInfoLog = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glGetShaderInfoLog = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glGenVertexArrays = void (*)(GLsizei, GLuint*);
using PFN_glBlitFramebuffer = void (*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
using PFN_glBindVertexArray = void (*)(GLuint);
using PFN_glGenFramebuffers = void (*)(GLsizei, GLuint*);
using PFN_glGenTextures = void (*)(GLsizei, GLuint*);
using PFN_glUniform4f = void (*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
using PFN_glGetProgramiv = void (*)(GLuint, GLenum, GLint*);
using PFN_glGetShaderiv = void (*)(GLuint, GLenum, GLint*);
using PFN_glTexImage2D = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                   const void*);
using PFN_glCompressedTexImage2D = void (*)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei,
                                             const void*);
using PFN_eglGetPlatformDisplayEXT = EGLDisplay (*)(EGLenum, void*, const EGLint*);

struct RealFns {
#define FN(name) PFN_##name name##_
    FN(eglInitialize);
    FN(eglBindAPI);
    FN(eglChooseConfig);
    FN(eglCreateWindowSurface);
    FN(eglCreateContext);
    FN(eglMakeCurrent);
    FN(eglSwapBuffers);
    FN(eglGetError);
    FN(eglQueryString);
    FN(eglQuerySurface);
    FN(eglSwapInterval);

    FN(glActiveTexture);
    FN(glAttachShader);
    FN(glBindFramebuffer);
    FN(glBindTexture);
    FN(glCompileShader);
    FN(glCreateProgram);
    FN(glCreateShader);
    FN(glDeleteProgram);
    FN(glDeleteShader);
    FN(glDrawArrays);
    FN(glFramebufferTexture2D);
    FN(glLinkProgram);
    FN(glTexParameteri);
    FN(glUniform1i);
    FN(glUseProgram);
    FN(glViewport);
    FN(glGetUniformLocation);
    FN(glShaderSource);
    FN(glGetProgramInfoLog);
    FN(glGetShaderInfoLog);
    FN(glGenVertexArrays);
    FN(eglGetPlatformDisplayEXT);
    FN(glBlitFramebuffer);
    FN(glBindVertexArray);
    FN(glGenFramebuffers);
    FN(glGenTextures);
    FN(glUniform4f);
    FN(glGetProgramiv);
    FN(glGetShaderiv);
    FN(glTexImage2D);
    FN(glCompressedTexImage2D);
#undef FN
};

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
uint64_t share_window_state(uint64_t id);
bool g_prefer_vulkan = true;
bool g_hidpi_enabled = true;
// Stud's own upscaler, and how far below the screen the engine renders
// for it, DLSS's quality presets. The output is never a setting: it is
// always the window's size in the display's own pixels.
bool g_upscaling_enabled = false;
// The engine's render size is NOT a setting; it is pinned to the
// window's logical size, because moving it moves the UI's size with it,
// and neither is the output: that is the window's own resolution, which
// the compositor shows 1:1.
bool g_discord_enabled = false;
bool g_discord_join_button = false;

// Windows Graphical Parity on the GL path: what the desktop material
// shaders sample that the Android engine never binds. Their annotations
// put the detail normal map at unit 8 and the BRDF lookup table at unit
// 11, units no Android shader uses; wherever the engine leaves one of
// those empty, the Windows client file goes there instead, the same rule
// as desktop_fill on the Vulkan path.
namespace gl_desktop_fill {

std::string g_dir;                // empty: the feature is off
constexpr GLenum kUnit[2] = {8, 11};
constexpr const char* kFile[2] = {"normaldetail.dds", "brdfLUT.dds"};
// The second environment cube the desktop shaders blend with the engine's
// (PrefilteredEnvIndoorTexture at 14, PrefilteredEnvTexture at 15): the
// Android engine leaves a 1x1 placeholder at 14, so it shows 15's cube.
constexpr GLenum kIndoorCubeUnit = 14;
constexpr GLenum kEnvCubeUnit = 15;

// One DDS file, mips and all: BC3 (the detail normal, DXT5nm) or
// R16G16_FLOAT (the lookup table), the two layouts these files use.
GLuint upload(const RealFns& fns, const std::string& path, bool repeat) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 128 || std::memcmp(b.data(), "DDS ", 4) != 0) return 0;
    uint32_t h = 0, w = 0, mips = 0;
    std::memcpy(&h, b.data() + 12, 4);
    std::memcpy(&w, b.data() + 16, 4);
    std::memcpy(&mips, b.data() + 28, 4);
    if (mips == 0) mips = 1;
    const bool block = std::memcmp(b.data() + 84, "DXT5", 4) == 0;
    uint32_t dxgi = 0;
    if (!block && std::memcmp(b.data() + 84, "DX10", 4) == 0 && b.size() >= 148) {
        std::memcpy(&dxgi, b.data() + 128, 4);
    }
    if (!block && dxgi != 34) return 0;  // DXGI_FORMAT_R16G16_FLOAT
    size_t at = block ? 128 : 148;
    GLuint tex = 0;
    fns.glGenTextures_(1, &tex);
    fns.glBindTexture_(GL_TEXTURE_2D, tex);
    uint32_t uploaded = 0;
    for (uint32_t i = 0; i < mips; ++i) {
        const uint32_t mw = std::max(1u, w >> i), mh = std::max(1u, h >> i);
        const size_t bytes = block ? size_t{(mw + 3) / 4} * ((mh + 3) / 4) * 16 : size_t{mw} * mh * 4;
        if (at + bytes > b.size()) break;
        if (block) {
            fns.glCompressedTexImage2D_(GL_TEXTURE_2D, static_cast<GLint>(i), 0x83F3 /* DXT5 */,
                                        static_cast<GLsizei>(mw), static_cast<GLsizei>(mh), 0,
                                        static_cast<GLsizei>(bytes), b.data() + at);
        } else {
            fns.glTexImage2D_(GL_TEXTURE_2D, static_cast<GLint>(i), GL_RG16F, static_cast<GLsizei>(mw),
                              static_cast<GLsizei>(mh), 0, GL_RG, GL_HALF_FLOAT, b.data() + at);
        }
        at += bytes;
        uploaded = i + 1;
    }
    mips = uploaded;
    const GLint wrap = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    fns.glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    fns.glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    fns.glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(mips - 1));
    fns.glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                         mips > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    fns.glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return tex;
}

}  // namespace gl_desktop_fill

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
// The directory this binary runs from; empty when it cannot be read.
std::string own_exe_dir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    const std::string exe(buf);
    const auto slash = exe.find_last_of('/');
    return slash == std::string::npos ? std::string() : exe.substr(0, slash);
}

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

// Replaces this (forked) process with stud-ui run as `stud <mode> <argument>`,
// trying each candidate in turn; exits 127 if none of them runs. The
// candidates are built before the fork, so the child only execs.
[[noreturn]] void exec_ui_helper(const std::vector<std::string>& candidates, const char* mode,
                                 const std::string& argument) {
    for (const std::string& path : candidates) {
        if (path.find('/') == std::string::npos) {
            ::execlp(path.c_str(), path.c_str(), mode, argument.c_str(), nullptr);
        } else {
            ::execl(path.c_str(), path.c_str(), mode, argument.c_str(), nullptr);
        }
    }
    ::_exit(127);
}

void run_ui_helper_detached(const char* mode, const std::string& argument) {
    const std::vector<std::string> candidates = ui_helper_candidates(own_exe_dir());
    const pid_t pid = ::fork();
    if (pid != 0) {
        if (pid > 0) ::waitpid(pid, nullptr, 0);  // the intermediate exits at once
        return;
    }
    // Child: fork again so the grandchild is reparented to init and this
    // process never has to reap it.
    if (::fork() != 0) ::_exit(0);
    ::setsid();
    exec_ui_helper(candidates, mode, argument);
}

// One stud-ui helper at a time.
//
// The calls that run one are dispatched without the dispatch lock (see
// blocks_in_the_driver()), because each is a whole Qt process starting
// up and, for the game lookup, an HTTPS round trip. They still must not
// overlap each other: two keyring helpers writing the same secret at once
// is not something either was written to survive.
std::mutex& ui_helper_mutex() {
    static std::mutex m;
    return m;
}

bool run_ui_secret_helper(const char* mode, const std::string& name, const std::string& input,
                           std::string* capture) {
    std::lock_guard<std::mutex> helper_lock(ui_helper_mutex());
    int to_child[2] = {-1, -1};
    int from_child[2] = {-1, -1};
    if (::pipe2(to_child, O_CLOEXEC) != 0) return false;
    if (capture != nullptr && ::pipe2(from_child, O_CLOEXEC) != 0) {
        ::close(to_child[0]);
        ::close(to_child[1]);
        return false;
    }
    const std::vector<std::string> candidates = ui_helper_candidates(own_exe_dir());
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
        exec_ui_helper(candidates, mode, name);
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
    // Every deliberate shutdown leaves through here, so this is where it
    // is recorded -- not at the individual branches.
    //
    // Marking one branch was not enough and could not have been: closing
    // the window exits by "the engine disconnected", while Exit inside
    // Roblox exits by "the session ended", and only the first was
    // marked, so leaving from the app's own menu was reported as a
    // crash. There are four of these and they all pass here.
    //
    // Status 0 only. A non-zero exit is a failure, and a failure should
    // be reported as one rather than quietly excused. atexit() is no use
    // for this: _exit() below deliberately skips it.
    if (status == 0) stud::logging::note_clean_exit();
    std::fflush(nullptr);
    ::_exit(status);
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
        const double gap = std::chrono::duration<double, std::milli>(now - last).count();
        intervals.push_back(gap);
        // A frame that took far longer than its neighbours, dumped with
        // everything that preceded it.
        //
        // The slow-present trigger cannot see these. Measured over 45000
        // frames, half of all 240-frame windows held a frame over 49ms
        // against an 11ms median -- and only ONE slow-present dump fired
        // in the whole session, because the present itself was fast every
        // time. Whatever such a frame waits on, it is not the present,
        // and nothing was recording at the moment it happened.
        //
        // 50ms rather than a multiple of the median: the background cap
        // is 30fps, so a hidden window legitimately sits at 33ms and a
        // ratio would fire on every one of those.
        static const double long_frame_ms = [] {
            const char* v = std::getenv("STUD_LONG_FRAME_MS");
            const double parsed = v != nullptr ? std::atof(v) : 50.0;
            return parsed > 0.0 ? parsed : 50.0;
        }();
        // Not until well past startup.
        //
        // Bringing the app up legitimately takes long frames -- swapchains
        // are created, the world's images are uploaded -- and the first
        // version of this trigger spent its entire dump budget on them
        // before a game had even been joined. Measured: six dumps inside
        // the first 28 seconds, every one of them sitting on
        // swapchain-new or a burst of image creations.
        //
        // Counted in frames rather than seconds because that is what the
        // budget is spent on, and a session that renders nothing is not
        // warming up.
        static uint64_t frames_seen = 0;
        ++frames_seen;
        constexpr uint64_t kWarmupFrames = 3000;
        if (gap > long_frame_ms && frames_seen > kWarmupFrames &&
            stud::render_host::fr::enabled()) {
            // Rate-limited hard: these arrive in bursts and a dump is
            // 6000 lines. The first few carry the answer.
            static int dumped = 0;
            if (dumped < 6) {
                ++dumped;
                stud::render_host::fr::dump("a frame interval ran long");
            }
        }
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
            if (::pipe2(pipe_fds, O_CLOEXEC) != 0) return 0;
            // A second pipe, the other way: the page's JavaScript bridge
            // answers through the viewer's stdout (see
            // read_web_view_output), which is how a login challenge
            // reports that it is done.
            int out_fds[2] = {-1, -1};
            if (::pipe2(out_fds, O_CLOEXEC) != 0) {
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
                const std::string own_dir = own_exe_dir();
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
            const std::string key = "activationToken=";
            std::optional<std::string> launcher_token;
            for (size_t at = 0; at < uri.size();) {
                size_t end = uri.find('\n', at);
                if (end == std::string::npos) end = uri.size();
                if (uri.compare(at, key.size(), key) == 0) {
                    launcher_token = uri.substr(at + key.size(), end - at - key.size());
                    break;
                }
                at = end + 1;
            }
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
            // owns the Wayland surface. The token is taken out above,
            // before the payload is moved into the queue: read after, it
            // was never found, and the launcher's token was never spent.
            {
                // Said out loud, because the second stud-ui's own stderr
                // goes nowhere. It has no terminal and is not teed into
                // the session log, so a missing token was invisible
                // from both ends.
                std::printf("stud-render-host: deep link payload %zu bytes, activation token %s\n",
                            payload_bytes,
                            launcher_token ? "present" : "ABSENT");
                std::fflush(stdout);
                if (launcher_token) {
                    // Same window the activation-token getter above
                    // uses; the one real surface this process owns.
                    stud::android_glue::native_window_activate(g_real_window,
                                                               launcher_token->c_str());
                    std::printf("stud-render-host: raising the window for the new link\n");
                } else {
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
        case CallId::SetKeepScreenOn:
            stud::android_glue::native_window_set_keep_screen_on(g_real_window, a[0] != 0);
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
                if (::pipe2(fds, O_CLOEXEC) != 0) return 0;
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
                    for (; n < 7 && argv_template[n] != nullptr; ++n) {
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
            // Sized by what is buffered, never by the request alone: the
            // reply cannot be larger, and the request's length is the
            // engine's to get wrong.
            out.resize(std::min<size_t>(hdr.out_buffer_len,
                                        stud::render_host::audio_input_buffered()));
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
        case CallId::ShareWindowState:
            return share_window_state(a[0]);
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
            //
            // On a thread of its own: the lookup takes most of a second,
            // and on the connection's reader it held everything the
            // engine queued behind it right after a join. Only the newest
            // request is applied, so a slow lookup for a game already
            // left cannot overwrite what came after it.
            static std::atomic<uint64_t> latest{0};
            static std::mutex apply_mutex;
            const uint64_t mine = latest.fetch_add(1) + 1;
            std::thread([body = std::string(reinterpret_cast<const char*>(in.data()), in.size()),
                         mine] {
                const auto current = [mine] { return latest.load() == mine; };
                // The tray offers "copy server link" and is a separate
                // process, so the link is left where it can read it. Its
                // absence is what "not in a game" looks like from there.
                const std::string invite_path = []() {
                    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
                    return std::string(xdg != nullptr ? xdg : "/tmp") + "/stud/invite";
                }();
                if (body.empty()) {
                    std::lock_guard<std::mutex> lock(apply_mutex);
                    if (!current()) return;
                    std::printf("stud-render-host: Discord presence: the app shell\n");
                    std::fflush(stdout);
                    ::unlink(invite_path.c_str());
                    stud::render_host::discord_rpc_set_game({});
                    return;
                }
                std::string info;
                if (!run_ui_secret_helper("--game-info", body, std::string(), &info) || info.empty()) {
                    return;
                }
                std::lock_guard<std::mutex> lock(apply_mutex);
                if (!current()) return;
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
            }).detach();
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

// ---- Stud's upscaler on the GL path ----
//
// gfxstream's frames reach the window through the present below; when the
// window is larger than the frame they are scaled there. The scaler is
// SGSR's own GLES fragment shader, vendored by Qualcomm for exactly this:
// one pass, scaling and sharpening. RAVU, the Vulkan path's default, is a
// compute shader and is not ported. Where the shader cannot be built, or
// nothing needs scaling, the frame is copied.
bool g_gl_scale_in_stud = false;

#include "sgsr_gles_frag.h"

struct GlScaleTarget {
    GLuint program = 0;
    GLuint vao = 0;
    GLint viewport_info = -1;
    GLint source = -1;
    bool program_tried = false;
};

// SGSR's shader as vendored, with what it needs to build on GLES 3.0 and
// to match the Vulkan path's settings. Each change is a line of the
// reference, replaced whole:
//   - its fragment input carries a layout location, which GLSL ES 3.00
//     does not allow on a fragment input (3.10 does); it is matched by
//     name instead;
//   - textureGather is GLSL ES 3.10, and a context the engine asked for
//     as 3.0 cannot compile it, so the gather is four texelFetch calls
//     returning the same texels in the same order;
//   - three of its constructors carry a precision qualifier
//     (`highp vec2(...)`), which GLSL ES does not allow on a constructor
//     and a strict compiler rejects; the operands are highp already;
//   - the edge-direction variant is switched on and the sharpness pinned
//     where the Vulkan path pins it (sgsr.comp).
std::string gl_upscaler_fragment_source() {
    std::string s = kSgsrGlesFragment;
    auto replace = [&s](const std::string& from, const std::string& to) {
        for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) {
            s.replace(at, from.size(), to);
        }
    };
    replace("// #define UseEdgeDirection", "#define UseEdgeDirection");
    replace("#define EdgeSharpness 2.0", "#define EdgeSharpness 3.0");
    replace("layout(location=0) in highp vec4 in_TEXCOORD0;", "in highp vec4 in_TEXCOORD0;");
    replace("textureGather(ps0,", "studGather(ps0,");
    replace("highp vec2(", "vec2(");
    const std::string gather =
        "\n// textureGather's footprint and order, from texelFetch (GLSL ES 3.00).\n"
        "vec4 studGather(mediump sampler2D s, highp vec2 uv, int comp)\n"
        "{\n"
        "\thighp ivec2 size = textureSize(s, 0);\n"
        "\thighp ivec2 b = ivec2(floor(uv * vec2(size) - 0.5));\n"
        "\thighp ivec2 lo = clamp(b, ivec2(0), size - 1);\n"
        "\thighp ivec2 hi = clamp(b + 1, ivec2(0), size - 1);\n"
        "\treturn vec4(texelFetch(s, ivec2(lo.x, hi.y), 0)[comp],\n"
        "\t            texelFetch(s, ivec2(hi.x, hi.y), 0)[comp],\n"
        "\t            texelFetch(s, ivec2(hi.x, lo.y), 0)[comp],\n"
        "\t            texelFetch(s, ivec2(lo.x, lo.y), 0)[comp]);\n"
        "}\n";
    const std::string anchor = "uniform mediump sampler2D ps0;\n#endif\n";
    const size_t at = s.find(anchor);
    if (at != std::string::npos) s.insert(at + anchor.size(), gather);
    return s;
}

GLuint gl_compile(const RealFns& fns, GLenum type, const std::string& src) {
    GLuint sh = fns.glCreateShader_(type);
    const GLchar* text = src.c_str();
    fns.glShaderSource_(sh, 1, &text, nullptr);
    fns.glCompileShader_(sh);
    GLint ok = 0;
    fns.glGetShaderiv_(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        fns.glGetShaderInfoLog_(sh, sizeof(log), nullptr, log);
        std::printf("stud-render-host: GL upscaler shader did not compile: %s\n", log);
        std::fflush(stdout);
        fns.glDeleteShader_(sh);
        return 0;
    }
    return sh;
}

void gl_build_program(const RealFns& fns, GlScaleTarget& t) {
    t.program_tried = true;
    // One triangle covering the window, its texture coordinate running
    // 0..1 across the visible part. The source is top-down, so it is drawn
    // upside down to come out right in the bottom-up window.
    static const char* kVertex =
        "#version 300 es\n"
        "out highp vec4 in_TEXCOORD0;\n"
        "void main() {\n"
        "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "    in_TEXCOORD0 = vec4(p, 0.0, 0.0);\n"
        "    gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
        "}\n";
    GLuint vs = gl_compile(fns, GL_VERTEX_SHADER, kVertex);
    GLuint fs = gl_compile(fns, GL_FRAGMENT_SHADER, gl_upscaler_fragment_source());
    if (vs == 0 || fs == 0) {
        if (vs != 0) fns.glDeleteShader_(vs);
        if (fs != 0) fns.glDeleteShader_(fs);
        return;
    }
    GLuint prog = fns.glCreateProgram_();
    fns.glAttachShader_(prog, vs);
    fns.glAttachShader_(prog, fs);
    fns.glLinkProgram_(prog);
    fns.glDeleteShader_(vs);
    fns.glDeleteShader_(fs);
    GLint ok = 0;
    fns.glGetProgramiv_(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        fns.glGetProgramInfoLog_(prog, sizeof(log), nullptr, log);
        std::printf("stud-render-host: GL upscaler did not link: %s\n", log);
        std::fflush(stdout);
        fns.glDeleteProgram_(prog);
        return;
    }
    t.program = prog;
    t.viewport_info = fns.glGetUniformLocation_(prog, "ViewportInfo[0]");
    t.source = fns.glGetUniformLocation_(prog, "ps0");
    fns.glGenVertexArrays_(1, &t.vao);
}

// ---- OpenGL through gfxstream: presenting the guest's colour buffers ----
//
// The engine's frames are drawn by gfxstream (gfxstream_host.h) into colour
// buffers on this process's own EGL display. Each queued one is drawn here
// into the window: through SGSR when the window is larger than the frame,
// copied otherwise, then swapped. The presenting context shares gfxstream's,
// so the frame is read where it lies.
struct GfxPresenter {
    bool tried = false;
    bool ok = false;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    GlScaleTarget scaler;  // only its SGSR program and vertex array
    GLuint read_fbo = 0;
};
GfxPresenter g_gfx_presenter;

EGLDisplay gfx_platform_display(const RealFns& fns, const RealWindow& window) {
    constexpr EGLenum kPlatformX11 = 0x31D5;
    constexpr EGLenum kPlatformWayland = 0x31D8;
    if (fns.eglGetPlatformDisplayEXT_ == nullptr) return EGL_NO_DISPLAY;
    return fns.eglGetPlatformDisplayEXT_(
        window.on_x11() ? kPlatformX11 : kPlatformWayland,
        window.on_x11() ? window.x11_display : static_cast<void*>(window.display), nullptr);
}

bool gfx_presenter_init(const RealFns& fns, RealWindow& window) {
    GfxPresenter& p = g_gfx_presenter;
    p.tried = true;
    p.display = gfx_platform_display(fns, window);
    if (p.display == EGL_NO_DISPLAY || fns.eglInitialize_(p.display, nullptr, nullptr) != EGL_TRUE) {
        std::fprintf(stderr, "stud-render-host: gfxstream present: no EGL display\n");
        return false;
    }
    fns.eglBindAPI_(EGL_OPENGL_ES_API);
    constexpr EGLint kEs3Bit = 0x40;  // EGL_OPENGL_ES3_BIT
    const EGLint attribs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, kEs3Bit,
                              EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                              EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint n = 0;
    if (fns.eglChooseConfig_(p.display, attribs, &config, 1, &n) != EGL_TRUE || n < 1) {
        std::fprintf(stderr, "stud-render-host: gfxstream present: no window config\n");
        return false;
    }
    {
        std::lock_guard<std::mutex> wl_lock(wayland_mutex());
        if (window.display != nullptr) wl_display_roundtrip(window.display);
    }
    p.surface = fns.eglCreateWindowSurface_(p.display, config, window.egl_native_window(), nullptr);
    // Sharing needs the same reset strategy as gfxstream's contexts, which
    // ask for lose-on-reset whenever the display offers robustness.
    const char* exts = fns.eglQueryString_(p.display, EGL_EXTENSIONS);
    const bool robust = exts != nullptr && std::strstr(exts, "EGL_EXT_create_context_robustness");
    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3,
                                  robust ? 0x3138 : EGL_NONE,  // RESET_NOTIFICATION_STRATEGY_EXT
                                  0x31BF,                      // LOSE_CONTEXT_ON_RESET_EXT
                                  EGL_NONE};
    p.context = fns.eglCreateContext_(
        p.display, config,
        static_cast<EGLContext>(stud::render_host::gfxstream_host::shared_context()), ctx_attribs);
    if (p.surface == EGL_NO_SURFACE || p.context == EGL_NO_CONTEXT ||
        fns.eglMakeCurrent_(p.display, p.surface, p.surface, p.context) != EGL_TRUE) {
        std::fprintf(stderr, "stud-render-host: gfxstream present: window surface or context "
                             "failed (0x%x)\n", fns.eglGetError_());
        return false;
    }
    // Uncapped, as the engine asks for on its own path; the guest paces
    // itself on its swaps.
    fns.eglSwapInterval_(p.display, 0);
    fns.glGenFramebuffers_(1, &p.read_fbo);
    // Windows Graphical Parity: the files the desktop material shaders read
    // go on their units always. No Android shader uses those units; the
    // engine leaves them empty or puts its 1x1 normal-map placeholder there,
    // which the DXT5nm detail decode turns into NaN (black).
    if (!gl_desktop_fill::g_dir.empty()) {
        for (int i = 0; i < 2; ++i) {
            const GLuint texture = gl_desktop_fill::upload(
                fns, gl_desktop_fill::g_dir + "/" + gl_desktop_fill::kFile[i], i == 0);
            stud::render_host::gfxstream_host::set_unit_texture_override(gl_desktop_fill::kUnit[i],
                                                                        texture);
            std::printf("stud-render-host: Windows Graphical Parity (gfxstream): %s %s\n",
                        gl_desktop_fill::kFile[i], texture != 0 ? "on its unit" : "missing");
        }
        stud::render_host::gfxstream_host::set_cube_unit_mirror(gl_desktop_fill::kIndoorCubeUnit,
                                                                gl_desktop_fill::kEnvCubeUnit);
        fns.glBindTexture_(GL_TEXTURE_2D, 0);
        std::fflush(stdout);
    }
    return true;
}

// A frame to show: the colour buffer and the host texture behind it. The
// texture is looked up where the present arrives: gfxstream finds a colour
// buffer's image through its EGL state for the calling thread, which the
// presenter thread does not have.
struct GfxFrame {
    uint32_t color_buffer = 0;
    GLuint tex = 0;
    uint32_t w = 0;
    uint32_t h = 0;
};

uint64_t gfx_present(const RealFns& fns, RealWindow& window, const GfxFrame& frame);

// The guest's present, answered as soon as the frame before it is on
// screen: its own frame is drawn by the presenter thread meanwhile, so the
// engine encodes its next frame while this one is waited for and drawn
// rather than after. One frame in hand at most, so it never runs further
// ahead than that.
uint64_t gfx_present_behind(const RealFns& fns, RealWindow& window, uint32_t color_buffer) {
    static std::mutex m;
    static std::condition_variable changed;
    static bool in_hand = false;
    static GfxFrame next;
    static std::once_flag started;
    // ponytail: detached, holding main()'s fns and window; join it at exit
    // if render-host ever tears down while a game is still presenting.
    std::call_once(started, [&fns, &window] {
        std::thread([&fns, &window] {
            for (;;) {
                GfxFrame frame;
                {
                    std::unique_lock<std::mutex> lock(m);
                    changed.wait(lock, [] { return in_hand; });
                    frame = next;
                }
                gfx_present(fns, window, frame);
                {
                    std::lock_guard<std::mutex> lock(m);
                    in_hand = false;
                }
                changed.notify_all();
            }
        }).detach();
    });
    GfxFrame frame;
    frame.color_buffer = color_buffer;
    frame.tex = stud::render_host::gfxstream_host::color_buffer_texture(color_buffer, &frame.w,
                                                                       &frame.h);
    std::unique_lock<std::mutex> lock(m);
    changed.wait(lock, [] { return !in_hand; });
    next = frame;
    in_hand = true;
    changed.notify_all();
    return 1;
}

uint64_t gfx_present(const RealFns& fns, RealWindow& window, const GfxFrame& frame) {
    const uint32_t color_buffer = frame.color_buffer;
    GfxPresenter& p = g_gfx_presenter;
    if (!p.tried) {
        std::printf("stud-render-host: first frame from gfxstream (colour buffer %u)\n",
                    color_buffer);
        std::fflush(stdout);
        p.ok = gfx_presenter_init(fns, window);
    }
    if (!p.ok) return 0;
    stud::logging::note_window_shown();
    fns.eglMakeCurrent_(p.display, p.surface, p.surface, p.context);
    stud::render_host::gfxstream_host::wait_frame(color_buffer);

    const GLuint tex = frame.tex;
    const uint32_t w = frame.w, h = frame.h;
    if (tex == 0 || w == 0 || h == 0) {
        static bool told = false;
        if (!told) {
            told = true;
            std::fprintf(stderr, "stud-render-host: gfxstream present: colour buffer %u has no "
                                 "texture (%ux%u)\n", color_buffer, w, h);
        }
        return 0;
    }
    EGLint out_w = 0, out_h = 0;
    fns.eglQuerySurface_(p.display, p.surface, EGL_WIDTH, &out_w);
    fns.eglQuerySurface_(p.display, p.surface, EGL_HEIGHT, &out_h);
    if (out_w <= 0 || out_h <= 0) return 0;

    const bool scaling = out_w > static_cast<EGLint>(w) || out_h > static_cast<EGLint>(h);
    if (scaling && !p.scaler.program_tried) gl_build_program(fns, p.scaler);
    static bool announced = false;
    if (scaling && p.scaler.program != 0) {
        fns.glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, 0);
        fns.glViewport_(0, 0, out_w, out_h);
        fns.glUseProgram_(p.scaler.program);
        fns.glUniform4f_(p.scaler.viewport_info, 1.0f / static_cast<float>(w),
                         1.0f / static_cast<float>(h), static_cast<float>(w),
                         static_cast<float>(h));
        fns.glUniform1i_(p.scaler.source, 0);
        fns.glActiveTexture_(GL_TEXTURE0);
        fns.glBindTexture_(GL_TEXTURE_2D, tex);
        fns.glBindVertexArray_(p.scaler.vao);
        fns.glDrawArrays_(GL_TRIANGLES, 0, 3);
    } else {
        fns.glBindFramebuffer_(GL_READ_FRAMEBUFFER, p.read_fbo);
        fns.glFramebufferTexture2D_(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex,
                                    0);
        fns.glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, 0);
        // Colour buffers are top-down; the window is bottom-up.
        fns.glBlitFramebuffer_(0, 0, static_cast<GLint>(w), static_cast<GLint>(h), 0, out_h, out_w,
                               0, GL_COLOR_BUFFER_BIT, scaling ? GL_LINEAR : GL_NEAREST);
    }
    if (!announced) {
        announced = true;
        std::printf("stud-render-host: gfxstream frame %ux%u -> %dx%d (%s)\n", w, h, out_w, out_h,
                    scaling && p.scaler.program != 0 ? "SGSR" : "copy");
        std::fflush(stdout);
    }
    stud::render_host::gfxstream_host::frame_shown(color_buffer);
    const bool swapped = fns.eglSwapBuffers_(p.display, p.surface) == EGL_TRUE;
    {
        // Sent now; the answers are read by this process's own poll loop.
        // A round trip here waited on the compositor every frame.
        std::lock_guard<std::mutex> wl_lock(wayland_mutex());
        if (window.display != nullptr) wl_display_flush(window.display);
    }
    return swapped ? 1 : 0;
}

// The window size the engine is meant to render at: the compositor's size
// once it has settled (sync_vk_window_size), width in the high half.
// GetWindowSize answers with this, so Process B's surface notifications
// and the Vulkan surface extent describe the same size. They used to
// differ mid-drag: the engine was told every raw size while its
// swapchain stayed at the last settled one. Zero until the first size.
std::atomic<uint64_t> g_settled_window_size{0};

// What GetWindowSize answers: the settled size once there is one, the
// window's own until then.
uint64_t current_window_size() {
    if (const uint64_t settled = g_settled_window_size.load()) return settled;
    const auto w = static_cast<uint64_t>(ANativeWindow_getWidth(nullptr));
    const auto h = static_cast<uint64_t>(ANativeWindow_getHeight(nullptr));
    return (w << 32) | (h & 0xffffffffu);
}

// Process B's copy of both answers; see CallId::ShareWindowState. Written
// whenever either changes, so its main loop never waits on this process
// to learn them -- it used to, every 250ms, behind whatever held the
// dispatch lock: 958ms at worst in one session's stall trace.
std::atomic<std::atomic<uint64_t>*> g_window_state_page{nullptr};

void publish_window_state() {
    std::atomic<uint64_t>* page = g_window_state_page.load(std::memory_order_acquire);
    if (page == nullptr) return;
    page[0].store(current_window_size(), std::memory_order_release);
    page[1].store(stud::android_glue::window_close_requested() ? 1u : 0u,
                  std::memory_order_release);
}

uint64_t share_window_state(uint64_t id) {
    const int fd = stud::render_host::take_shared_fd(id);
    if (fd < 0) return 0;
    const auto length = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    void* p = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) return 0;
    auto* page = static_cast<std::atomic<uint64_t>*>(p);
    // A page an earlier client was given stays mapped: a publish may still
    // be writing to it, and it is one page.
    g_window_state_page.store(page, std::memory_order_release);
    stud::android_glue::set_window_state_listener(&publish_window_state);
    publish_window_state();
    page[2].store(1, std::memory_order_release);
    return 1;
}

// Defined below the per-domain dispatchers it hands to; a command batch
// (VoidCallBatch) calls back into it for each call it carries.
uint64_t dispatch(const Header& hdr, const RealFns& fns, RealWindow& window,
                  const std::vector<uint8_t>& in, std::vector<uint8_t>& out, uint32_t* out_len);

// Split out of dispatch(), whose switch it was part of: Vulkan: everything vulkan_host.cpp implements, by call id.
// Answers nullopt for any id that is not one of these, and a case that
// breaks out answers kNullHandle, exactly as it did inside dispatch().
std::optional<uint64_t> dispatch_vk_call(const Header& hdr, RealWindow& window,
                                         const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                                         uint32_t* out_len) {
    const uint64_t* a = hdr.args;
    switch (hdr.call_id) {
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
        // a[7], when set, is the number the client gave the new object; the
        // call is then reply-free (see answer_handle in vulkan_host.cpp).
        case CallId::VkCreateCommandPool: {
            const uint64_t r = stud::render_host::vk_create_command_pool(
                static_cast<uint32_t>(a[1]), static_cast<uint32_t>(a[2]), out, out_len, a[7]);
            if (a[7] != 0) report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkCreateSemaphore: {
            const uint64_t r = stud::render_host::vk_create_semaphore(static_cast<uint32_t>(a[1]), out,
                                                                    out_len, a[7]);
            if (a[7] != 0) report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkCreateFence: {
            const uint64_t r = stud::render_host::vk_create_fence(static_cast<uint32_t>(a[1]), out, out_len,
                                                                a[7]);
            if (a[7] != 0) report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkCreateQueryPool: {
            const uint64_t r = stud::render_host::vk_create_query_pool(
                static_cast<uint32_t>(a[1]), static_cast<uint32_t>(a[2]),
                static_cast<uint32_t>(a[3]), static_cast<uint32_t>(a[4]), out, out_len, a[7]);
            if (a[7] != 0) report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkCreatePipelineCache:
            return stud::render_host::vk_create_pipeline_cache(static_cast<uint32_t>(a[1]), in,
                                                                out, out_len);
        case CallId::VkGetPipelineCacheData:
            return stud::render_host::vk_get_pipeline_cache_data(
                a[1], static_cast<uint32_t>(a[2]), out, out_len);
        case CallId::VkDestroyPipelineCache:
            return stud::render_host::vk_destroy_pipeline_cache(a[1]);
        // a[1], when set, is the handle the client chose; the call is then
        // reply-free, and this is the only place a failure can be seen.
        case CallId::VkCreateImage: {
            const uint64_t r = stud::render_host::vk_create_image(in, out, out_len, a[1]);
            if (a[1] != 0) report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkGetImageMemoryRequirements:
            return stud::render_host::vk_get_image_memory_requirements(a[1], out, out_len);
        case CallId::VkGetPhysicalDeviceSurfaceCapabilitiesKHR:
            return stud::render_host::vk_get_physical_device_surface_capabilities(a[0], a[1], out,
                                                                                   out_len);
        // Reply-free: the client numbered it; see kBufferHandleTag.
        case CallId::VkCreateBuffer: {
            const uint64_t r = stud::render_host::vk_create_buffer(
                static_cast<uint32_t>(a[1]), a[2], static_cast<uint32_t>(a[3]),
                static_cast<uint32_t>(a[4]), a[5]);
            report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkGetBufferMemoryRequirements:
            return stud::render_host::vk_get_buffer_memory_requirements(a[1], out, out_len);
        // Sent reply-free by the Vulkan client, so this is the only place
        // a failed bind can be seen.
        case CallId::VkBindBufferMemory: {
            const uint64_t r = stud::render_host::vk_bind_buffer_memory(a[1], a[2], a[3]);
            report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkCreateImageView: {
            const uint64_t r = stud::render_host::vk_create_image_view(in, out, out_len, a[1]);
            if (a[1] != 0) report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkCreateShaderModule: {
            const uint64_t r = stud::render_host::vk_create_shader_module(in, out, out_len, a[7]);
            if (a[7] != 0) report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkDestroyHandle:
            return stud::render_host::vk_destroy_handle(static_cast<uint32_t>(a[1]), a[2]);
        case CallId::VkCreateRenderPass: {
            const uint64_t r = stud::render_host::vk_create_render_pass(in, out, out_len, a[7]);
            if (a[7] != 0) report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkCreateFramebuffer: {
            const uint64_t r = stud::render_host::vk_create_framebuffer(in, out, out_len, a[1]);
            if (a[1] != 0) report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkCreateSampler: {
            const uint64_t r = stud::render_host::vk_create_sampler(in, out, out_len, a[7]);
            if (a[7] != 0) report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkCreatePipelineLayout: {
            const uint64_t r = stud::render_host::vk_create_pipeline_layout(in, out, out_len, a[7]);
            if (a[7] != 0) report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkCreateDescriptorSetLayout: {
            const uint64_t r = stud::render_host::vk_create_descriptor_set_layout(in, out, out_len, a[7]);
            if (a[7] != 0) report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkCreateDescriptorPool: {
            const uint64_t r = stud::render_host::vk_create_descriptor_pool(in, out, out_len, a[7]);
            if (a[7] != 0) report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkAllocateDescriptorSets:
            return stud::render_host::vk_allocate_descriptor_sets(in, out, out_len);
        case CallId::VkResetDescriptorPool:
            return stud::render_host::vk_reset_descriptor_pool(a[1],
                                                                static_cast<uint32_t>(a[2]));
        case CallId::VkCreateDescriptorUpdateTemplate: {
            const uint64_t r = stud::render_host::vk_create_descriptor_update_template(in, out, out_len, a[7]);
            if (a[7] != 0) report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkUpdateDescriptorSetWithTemplate:
            return stud::render_host::vk_update_descriptor_set_with_template(a[1], a[2], in);
        case CallId::VkCreateGraphicsPipelines: {
            const uint64_t r = stud::render_host::vk_create_graphics_pipelines(in, out, out_len, a[7]);
            if (a[7] != 0) report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkCreateComputePipelines: {
            const uint64_t r = stud::render_host::vk_create_compute_pipelines(in, out, out_len, a[7]);
            if (a[7] != 0) report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkAllocateCommandBuffers:
        {
            // Reply-free when the client numbered them: `in` carries one
            // number per buffer.
            const uint64_t r = stud::render_host::vk_allocate_command_buffers(
                a[1], static_cast<uint32_t>(a[2]), static_cast<uint32_t>(a[3]), in, out,
                out_len);
            if (!in.empty()) report_reply_free_failure(hdr.call_id, r);
            return r;
        }
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
        case CallId::VkAllocateMemory:
            // a[4] non-zero: the client mapped a memfd for this allocation,
            // sent it over the fd channel under this id, and wants it
            // shared rather than copied. The buffer still carries the
            // pNext chain.
        // a[7], when set, is the client's number for GPU-only memory; the
        // call is then reply-free.
        {
            const uint64_t r = stud::render_host::vk_allocate_memory(
                a[1], static_cast<uint32_t>(a[2]), in, static_cast<uint32_t>(a[3]), out, out_len,
                a[4], a[7]);
            if (a[7] != 0) report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        // Reply-free from the client, like VkBindBufferMemory.
        case CallId::VkBindImageMemory: {
            const uint64_t r = stud::render_host::vk_bind_image_memory(a[1], a[2], a[3]);
            report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkFreeMemory:
            return stud::render_host::vk_free_memory(a[1]);
        // Reply-free whenever the memory is shared with the client, and the
        // write and flush always; see stud_vkMapMemory.
        case CallId::VkMapMemory: {
            const uint64_t r = stud::render_host::vk_map_memory(a[1], a[2], a[3],
                                                                static_cast<uint32_t>(a[4]));
            report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkWriteMappedMemory: {
            const uint64_t r = stud::render_host::vk_write_mapped_memory(a[1], a[2], in);
            report_reply_free_failure(hdr.call_id, r);
            return r;
        }
        case CallId::VkReadMappedMemory:
            return stud::render_host::vk_read_mapped_memory(a[1], a[2], a[3], out, out_len);
        case CallId::VkShareMappedMemory:
            return stud::render_host::vk_share_mapped_memory(a[1], a[2], a[3]);
        case CallId::VkShareSubmitFeedback:
            return stud::render_host::vk_share_submit_feedback(a[0], a[1]);
        case CallId::VkReadExportedMemory:
            return stud::render_host::vk_read_exported_memory(a[1], a[2], a[3], out, out_len);
        case CallId::VkAcquireTake:
            return stud::render_host::vk_acquire_take(a[1], a[2], a[4], a[3]);
        case CallId::VkShareQueryResults:
            return stud::render_host::vk_share_query_results(a[1], a[2], a[3],
                                                             static_cast<uint32_t>(a[4]));
        case CallId::VkWriteSharedMappedMemory:
            return stud::render_host::vk_write_shared_mapped_memory(a[1], a[2], a[3]);
        case CallId::VkUnmapMemory:
            return stud::render_host::vk_unmap_memory(a[1]);
        case CallId::VkFlushMappedMemoryRanges: {
            const uint64_t r = stud::render_host::vk_flush_mapped_memory_ranges(a[1], a[2], a[3]);
            report_reply_free_failure(hdr.call_id, r);
            return r;
        }
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
        // Roblox's vkCreateAndroidSurfaceKHR (an Android-only extension with
        // no Linux equivalent), redirected to the real vkCreateWaylandSurfaceKHR
        // on the SAME Wayland display/surface android-glue's native_window.cpp
        // already owns here. See render_host_protocol.h's own doc comment for
        // why this is deliberately not a full Vulkan struct marshaller.
        //
        // volk's globals were loaded with the engine's instance, the one
        // this names: vulkan_host.cpp's own loader, not a second libvulkan
        // opened here for this one call.
        case CallId::VkCreateWaylandSurfaceForAndroidSurface: {
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
                const PFN_vkCreateXlibSurfaceKHR create_xlib = vkCreateXlibSurfaceKHR;
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
                // A window of the driver's own, not the main one; see
                // native_window_x11_content_window().
                xinfo.window = static_cast<::Window>(
                    stud::android_glue::native_window_x11_content_window());
                if (xinfo.window == 0) xinfo.window = static_cast<::Window>(window.x11_window);
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
            const PFN_vkCreateWaylandSurfaceKHR create_wayland = vkCreateWaylandSurfaceKHR;
            if (create_wayland == nullptr) return kNullHandle;
            VkWaylandSurfaceCreateInfoKHR info{};
            info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
            info.display = window.display;
            // A surface of the driver's own, not the window's; see
            // native_window_content_surface().
            info.surface = stud::android_glue::native_window_content_surface(g_real_window);
            if (info.surface == nullptr) info.surface = window.surface;
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

        default:
            return std::nullopt;
    }
    return kNullHandle;
}

uint64_t dispatch(const Header& hdr, const RealFns& fns, RealWindow& window,
                   const std::vector<uint8_t>& in, std::vector<uint8_t>& out, uint32_t* out_len) {
    const uint64_t* a = hdr.args;
    *out_len = 0;
    // Command buffers are replayed on workers; see replay in vulkan_host.cpp.
    // Recorded commands and the reply-free Begin/End that bracket them go
    // to their pool's queue. The few Vulkan calls that cannot touch what a
    // worker touches run as they are; every other Vulkan call waits for the
    // workers first, so it sees what it always saw. Calls that are not
    // Vulkan -- audio, input, GL -- never wait.
    if (stud::render_host::replay::enabled()) {
        const bool reply_free = (hdr.flags & Header::kNoReply) != 0;
        switch (hdr.call_id) {
            case CallId::VkCmdRecordBatch:
                stud::render_host::replay::enqueue_records(in.data(), in.size());
                return 0;
            case CallId::VkBeginCommandBuffer:
                if (reply_free) {
                    stud::render_host::replay::enqueue_begin(a[0], static_cast<uint32_t>(a[1]));
                    return 0;
                }
                stud::render_host::replay::wait_all();
                break;
            case CallId::VkEndCommandBuffer:
                if (reply_free) {
                    stud::render_host::replay::enqueue_end(a[0]);
                    return 0;
                }
                stud::render_host::replay::wait_all();
                break;
            case CallId::VkAllocateDescriptorSets:
            case CallId::VkUpdateDescriptorSetWithTemplate:
            case CallId::VkWriteSharedMappedMemory:
            case CallId::VkWriteMappedMemory:
                break;
            // Numbered by the client, it only reads objects this thread
            // already made and is compiled on a worker; see
            // pipeline_compiles in vulkan_host.cpp.
            case CallId::VkCreateGraphicsPipelines:
                if (hdr.args[7] == 0) stud::render_host::replay::wait_all();
                break;
            default:
                if (std::strncmp(stud::render_host::call_id_name(hdr.call_id), "Vk", 2) == 0) {
                    stud::render_host::replay::wait_all();
                }
                break;
        }
    }
    if (auto handled = dispatch_vk_call(hdr, window, in, out, out_len)) return *handled;
    switch (hdr.call_id) {
        // OpenGL mode: a frame gfxstream drew, presented in the window.
        case CallId::GlPresentColorBuffer:
            return gfx_present_behind(fns, window, static_cast<uint32_t>(a[0]));
        case CallId::VoidCallBatch: {
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
        case CallId::GetWindowSize: {
            return current_window_size();
        }
        case CallId::AudioOpenStream:
            return stud::render_host::audio_open_stream(static_cast<int>(a[0]),
                                                        static_cast<int>(a[1]),
                                                        static_cast<int>(a[2]));
        case CallId::AudioWriteFrames:
            return stud::render_host::audio_write_frames(a[0], in.data(), in.size());
        case CallId::AudioShareState:
            return stud::render_host::audio_share_state(a[0]);
        case CallId::AudioGetUnderruns:
            return stud::render_host::audio_underruns();

        // Video decoding; see video_codec.h. Each takes the decoder's own
        // lock, not the dispatch lock (see blocks_in_the_driver).
        case CallId::VideoDecoderSupported:
        case CallId::VideoDecoderCreate: {
            const std::string mime(reinterpret_cast<const char*>(in.data()),
                                   strnlen(reinterpret_cast<const char*>(in.data()), in.size()));
            return hdr.call_id == CallId::VideoDecoderSupported
                       ? (stud::render_host::video_decoder_supported(mime.c_str()) ? 1 : 0)
                       : stud::render_host::video_decoder_create(mime.c_str());
        }
        case CallId::VideoDecoderConfigure:
            return static_cast<uint64_t>(static_cast<int64_t>(
                stud::render_host::video_decoder_configure(
                    a[0], static_cast<uint32_t>(a[1]), static_cast<uint32_t>(a[2]), in.data(),
                    static_cast<uint32_t>(in.size()))));
        case CallId::VideoDecoderQueue:
            return static_cast<uint64_t>(static_cast<int64_t>(
                stud::render_host::video_decoder_queue(a[0], in.data(),
                                                       static_cast<uint32_t>(in.size()),
                                                       static_cast<int64_t>(a[1]), a[2] != 0)));
        case CallId::VideoDecoderDequeue:
            return static_cast<uint64_t>(static_cast<int64_t>(
                stud::render_host::video_decoder_dequeue(a[0], out, out_len)));
        case CallId::VideoDecoderFlush:
            stud::render_host::video_decoder_flush(a[0]);
            return 0;
        case CallId::VideoDecoderDestroy:
            stud::render_host::video_decoder_destroy(a[0]);
            return 0;
        case CallId::VideoEncoderSupported:
        case CallId::VideoEncoderCreate: {
            const std::string mime(reinterpret_cast<const char*>(in.data()),
                                   strnlen(reinterpret_cast<const char*>(in.data()), in.size()));
            if (hdr.call_id == CallId::VideoEncoderCreate) {
                return stud::render_host::video_encoder_create(mime.c_str());
            }
            const auto support = stud::render_host::video_encoder_supported(mime.c_str());
            return (support.supported ? 1u : 0u) | (support.hardware ? 2u : 0u);
        }
        case CallId::VideoEncoderConfigure:
            return static_cast<uint64_t>(static_cast<int64_t>(
                stud::render_host::video_encoder_configure(
                    a[0], static_cast<uint32_t>(a[1]), static_cast<uint32_t>(a[2]),
                    static_cast<uint32_t>(a[3]), static_cast<uint32_t>(a[4]),
                    static_cast<uint32_t>(a[5]), static_cast<uint32_t>(a[6]))));
        case CallId::VideoEncoderQueue:
            return static_cast<uint64_t>(static_cast<int64_t>(
                stud::render_host::video_encoder_queue(a[0], in.data(),
                                                       static_cast<uint32_t>(in.size()),
                                                       static_cast<int64_t>(a[1]), a[2] != 0)));
        case CallId::VideoEncoderDequeue:
            return static_cast<uint64_t>(static_cast<int64_t>(
                stud::render_host::video_encoder_dequeue(a[0], out, out_len)));
        case CallId::VideoEncoderConfig:
            out = stud::render_host::video_encoder_config(a[0]);
            *out_len = static_cast<uint32_t>(out.size());
            return 1;
        case CallId::VideoEncoderDestroy:
            stud::render_host::video_encoder_destroy(a[0]);
            return 0;
        case CallId::AudioCloseStream:
            stud::render_host::audio_close_stream(a[0]);
            return 1;

        // Everything that is not graphics lives in its own function; see
        // dispatch_platform_call().
        default:
            if (auto handled = dispatch_platform_call(hdr, window, in, out, out_len)) {
                return *handled;
            }
            break;
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


// Serialises every dispatch: the driver state the handlers touch is not
// thread-safe, and extra client connections are served on
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
// That is fine for a single-threaded client, but the EGL driver's own
// window-system code reads the same default queue from whichever thread
// is inside eglSwapBuffers, so poll() can report the display fd readable and, by
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
    if (w == last_w && h == last_h) {
        // No rebuild is coming for a size the engine already has.
        stud::android_glue::native_window_x11_resize_settled();
        return;
    }

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
    g_settled_window_size.store((static_cast<uint64_t>(w) << 32) | h);
    publish_window_state();
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
    //
    // THAT REASONING IS WRONG, and this is the switch that proves it.
    //
    // A WAYLAND_DEBUG trace of a real session shows 454 of these in
    // thirty seconds:
    //
    //   discarded wl_buffer#N.release()
    //
    // and the buffers are created with
    // zwp_linux_buffer_params_v1.create_immed -- dmabuf, which makes
    // them the VULKAN DRIVER'S swapchain buffers, not Stud's own wl_shm
    // cursor or overlay buffers. "Discarded" is libwayland's word for an
    // event delivered to a proxy with no listener and then thrown away.
    // Once it is thrown away the driver can never see it, no matter how
    // often it dispatches its own queue afterwards.
    //
    // What that costs was finally measured rather than guessed: a
    // vkQueuePresentKHR that took 12006ms with 0.0ms of it waiting for
    // Stud's queue, and the presenting thread sampled DURING the stall
    // in state R with wchan 0 -- running, not sleeping. The driver is
    // not blocked waiting for a buffer, it is SPINNING waiting for one
    // that Stud already threw the release event for.
    //
    // OFF by default now, and the two claims above are both settled.
    //
    // Measured with WAYLAND_DEBUG on this machine, with the call and
    // without it: the `discarded wl_buffer.release` events appear either
    // way, in the same proportion. Stud is not destroying them. The
    // driver is using EXPLICIT SYNC -- 8734 drm_syncobj references in
    // one trace -- where a buffer comes back through a syncobj release
    // point and wl_buffer.release is vestigial. Nobody listens to it, so
    // libwayland discards it, whoever dispatches.
    //
    // The black window the comment above records did NOT come back with
    // the call removed: checked directly, by running without it and
    // asking. So the second comment's justification is wrong on the
    // mechanism and wrong on the consequence, and what is left is a
    // process dispatching ANOTHER COMPONENT'S event queue from a foreign
    // thread, which delivers that component's callbacks wherever Stud
    // happens to be standing. That is worth not doing on principle even
    // with no bug attached to it.
    //
    // STUD_WL_DISPATCH_DEFAULT_QUEUE=1 puts it back, because this file's
    // history is two people being sure in opposite directions and the
    // cost of being wrong again is a black window.
    static const bool dispatch_default_queue =
        std::getenv("STUD_WL_DISPATCH_DEFAULT_QUEUE") != nullptr;
    if (dispatch_default_queue) wl_display_dispatch_pending(display);
    // And libdecor, which has to be pumped separately because it keeps
    // its objects on the default queue. A no-op unless this process is
    // actually drawing its own decorations.
    stud::android_glue::wayland_dispatch_decorations();
}

// Services one client connection to completion, on its own thread. Used
// for Process B's connections beyond the first, each of its client
// libraries (libvulkan, libaaudio, ...) links its own copy of the client
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
//
// A present can also arrive inside a VoidCallBatch: it is sent reply-free,
// and reply-free calls travel packed into batches. The batch's own id is
// all the dispatch loop sees, so the present inside one was never paced
// and the background cap did nothing at all. A batch is looked through,
// in the same record format VoidCallBatch's handler reads, for a present
// among its calls.
bool batch_carries_present(const std::vector<uint8_t>& in) {
    const uint8_t* p = in.data();
    const uint8_t* end = in.data() + in.size();
    while (end - p >= 4) {
        const uint8_t argc = *p++;
        const uint8_t bits = *p++;
        uint16_t id16 = 0;
        std::memcpy(&id16, p, sizeof(id16));
        p += sizeof(id16);
        const auto id = static_cast<stud::render_host::CallId>(id16);
        if (id == stud::render_host::CallId::VkQueuePresentKHR ||
            id == stud::render_host::CallId::GlPresentColorBuffer) {
            return true;
        }
        if (argc > 8 || end - p < static_cast<ptrdiff_t>(argc) * 8) return false;
        p += static_cast<size_t>(argc) * sizeof(uint64_t);
        if ((bits & 1u) != 0) {
            if (end - p < 4) return false;
            uint32_t len = 0;
            std::memcpy(&len, p, sizeof(len));
            p += sizeof(len);
            if (end - p < static_cast<ptrdiff_t>(len)) return false;
            p += len;
        }
        if ((bits & 2u) != 0) {
            if (end - p < 8) return false;
            p += sizeof(uint64_t);
        }
    }
    return false;
}

void throttle_before_dispatch(stud::render_host::CallId id, const std::vector<uint8_t>& in) {
    if (id == stud::render_host::CallId::VkQueuePresentKHR ||
        id == stud::render_host::CallId::GlPresentColorBuffer ||
        (id == stud::render_host::CallId::VoidCallBatch && batch_carries_present(in))) {
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
        case stud::render_host::CallId::AudioShareState:
        case stud::render_host::CallId::AudioGetUnderruns:
        case stud::render_host::CallId::AudioReadFrames:
        // Input may WAIT for an event to arrive (see PollInputEvents in
        // the protocol header). It holds no driver state at all, and
        // holding the dispatch lock across that wait would stall every
        // other connection for the length of it.
        case stud::render_host::CallId::PollInputEvents:
        // Read nothing the lock guards; waiting for it is what put the
        // window-size poll behind every driver call.
        case stud::render_host::CallId::GetWindowSize:
        case stud::render_host::CallId::PollWindowCloseRequested:
        case stud::render_host::CallId::ShareWindowState:
        // Video decoding touches no GL state and can take milliseconds a
        // frame; each decoder has its own lock.
        case stud::render_host::CallId::VideoDecoderSupported:
        case stud::render_host::CallId::VideoDecoderCreate:
        case stud::render_host::CallId::VideoDecoderConfigure:
        case stud::render_host::CallId::VideoDecoderQueue:
        case stud::render_host::CallId::VideoDecoderDequeue:
        case stud::render_host::CallId::VideoDecoderFlush:
        case stud::render_host::CallId::VideoDecoderDestroy:
        case stud::render_host::CallId::VideoEncoderSupported:
        case stud::render_host::CallId::VideoEncoderCreate:
        case stud::render_host::CallId::VideoEncoderConfigure:
        case stud::render_host::CallId::VideoEncoderQueue:
        case stud::render_host::CallId::VideoEncoderDequeue:
        case stud::render_host::CallId::VideoEncoderConfig:
        case stud::render_host::CallId::VideoEncoderDestroy:
        // Each of these starts stud-ui and waits for it: a Qt process
        // starting up, ~150-250 ms, and for the game lookup an HTTPS
        // round trip on top. Measured at launch: four of them back to back
        // (two keyring reads, two stores), and the engine's first Vulkan
        // call arrived only after the last had finished. Under the
        // dispatch lock, every other connection -- the engine's graphics
        // among them -- waited out every one. They share no state with
        // anything the lock protects; run_ui_secret_helper() serialises
        // them among themselves.
        case stud::render_host::CallId::StoreSecret:
        case stud::render_host::CallId::DeleteSecret:
        case stud::render_host::CallId::LoadSecret:
        case stud::render_host::CallId::SetGamePresence:
            return true;
        default:
            return false;
    }
}

// A batch made only of calls exempt above is exempt too.
//
// Reply-free calls travel packed into a VoidCallBatch, and the dispatch
// loop sees only the batch's id. The audio feeder sends its bursts
// reply-free, so without this each one waited for the dispatch lock behind
// the engine's graphics -- pipeline builds, submits -- and arrived late
// enough to empty the ring: silence padded mid-waveform, heard as pops.
// Stops at the first call that is not exempt, which in a graphics batch is
// the first one.
bool batch_is_lock_free(const std::vector<uint8_t>& in) {
    const uint8_t* p = in.data();
    const uint8_t* end = in.data() + in.size();
    while (end - p >= 4) {
        const uint8_t argc = *p++;
        const uint8_t bits = *p++;
        uint16_t id16 = 0;
        std::memcpy(&id16, p, sizeof(id16));
        p += sizeof(id16);
        if (!blocks_in_the_driver(static_cast<stud::render_host::CallId>(id16))) return false;
        if (argc > 8 || end - p < static_cast<ptrdiff_t>(argc) * 8) return false;
        p += static_cast<size_t>(argc) * sizeof(uint64_t);
        if ((bits & 1u) != 0) {
            if (end - p < 4) return false;
            uint32_t len = 0;
            std::memcpy(&len, p, sizeof(len));
            p += sizeof(len);
            if (end - p < static_cast<ptrdiff_t>(len)) return false;
            p += len;
        }
        if ((bits & 2u) != 0) {
            if (end - p < 8) return false;
            p += sizeof(uint64_t);
        }
    }
    return p == end;
}

bool dispatches_without_lock(stud::render_host::CallId id, const std::vector<uint8_t>& in) {
    return blocks_in_the_driver(id) ||
           (id == stud::render_host::CallId::VoidCallBatch && batch_is_lock_free(in));
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

// The shared-memory fd channel (CallId::SharedMemoryFdChannel).
//
// Process B backs the engine's host-visible memory with memfds rather
// than files in the runtime directory, and a memfd has no name to open,
// so the fd itself crosses over: an 8-byte allocation id per message,
// with the fd attached as SCM_RIGHTS, answered by one byte. The fd is
// parked under its id until the VkAllocateMemory or VkShareMappedMemory
// that names it maps it.
//
// A connection of its own because ancillary data travels with a
// particular byte of the stream: on a connection other threads write
// into, nothing guarantees which request the fd lands beside.
void serve_shared_fd_channel(int conn_fd) {
    for (;;) {
        uint64_t id = 0;
        char control[CMSG_SPACE(sizeof(int))] = {};
        iovec iov{&id, sizeof(id)};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        ssize_t n = ::recvmsg(conn_fd, &msg, MSG_CMSG_CLOEXEC);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        int fd = -1;
        for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
                c->cmsg_len >= CMSG_LEN(sizeof(int))) {
                std::memcpy(&fd, CMSG_DATA(c), sizeof(fd));
            }
        }
        // The id normally arrives whole with its fd; finish it if not.
        if (static_cast<size_t>(n) < sizeof(id) &&
            !read_all(conn_fd, reinterpret_cast<char*>(&id) + n,
                      static_cast<uint32_t>(sizeof(id) - static_cast<size_t>(n)))) {
            if (fd >= 0) ::close(fd);
            return;
        }
        // No fd attached: the client is fetching one exported for it
        // (design C), answered the same way with the fd riding the byte.
        if (fd < 0 && id != 0) {
            const int out_fd = stud::render_host::take_exported_fd(id);
            uint8_t found = out_fd >= 0 ? 1 : 0;
            char out_control[CMSG_SPACE(sizeof(int))] = {};
            iovec out_iov{&found, sizeof(found)};
            msghdr out{};
            out.msg_iov = &out_iov;
            out.msg_iovlen = 1;
            if (out_fd >= 0) {
                out.msg_control = out_control;
                out.msg_controllen = sizeof(out_control);
                cmsghdr* c = CMSG_FIRSTHDR(&out);
                c->cmsg_level = SOL_SOCKET;
                c->cmsg_type = SCM_RIGHTS;
                c->cmsg_len = CMSG_LEN(sizeof(int));
                std::memcpy(CMSG_DATA(c), &out_fd, sizeof(out_fd));
            }
            ssize_t sent;
            do {
                sent = ::sendmsg(conn_fd, &out, MSG_NOSIGNAL);
            } while (sent < 0 && errno == EINTR);
            if (out_fd >= 0) ::close(out_fd);
            if (sent != 1) return;
            continue;
        }
        const uint8_t kept = (fd >= 0 && id != 0) ? 1 : 0;
        if (kept) {
            stud::render_host::register_shared_fd(id, fd);
        } else if (fd >= 0) {
            ::close(fd);
        }
        if (!write_all(conn_fd, &kept, sizeof(kept))) return;
    }
}

// Sizes `buf` for a request's payload, or says the connection is lost.
//
// A call the protocol does not define means the stream has lost its
// framing, and the length beside it is garbage: allocating it first
// asked for up to 4 GB on a corrupt header. An allocation that still
// fails ends the connection rather than this process.
bool size_request_buffer(std::vector<unsigned char>& buf, const Header& hdr) {
    if (std::strcmp(stud::render_host::call_id_name(hdr.call_id), "<unknown>") == 0) {
        std::fprintf(stderr,
                     "stud-render-host: request with unknown call id %d, closing the connection\n",
                     static_cast<int>(hdr.call_id));
        return false;
    }
    try {
        buf.resize(hdr.in_buffer_len);
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr,
                     "stud-render-host: could not hold a %u-byte request, closing the connection\n",
                     hdr.in_buffer_len);
        return false;
    }
    return true;
}

void serve_connection_thread(int conn_fd, const RealFns& fns, RealWindow& real_window) {
    std::vector<unsigned char> in_scratch;
    std::vector<unsigned char> out_scratch;
    // The first header is read on its own: it is where a connection turns
    // into the shared-fd channel, whose bytes carry SCM_RIGHTS and must
    // reach recvmsg() rather than a buffer (see socket_reader.h).
    Header hdr{};
    bool serving = read_all(conn_fd, &hdr, sizeof(hdr));
    if (serving && hdr.call_id == stud::render_host::CallId::SharedMemoryFdChannel) {
        serve_shared_fd_channel(conn_fd);
        serving = false;
    }
    stud::render_host::SocketReader reader(conn_fd);
    // Once the client asks for it, requests come out of shared memory
    // instead of the socket (see stud/transport_ring.h); replies still go
    // back on the socket.
    std::unique_ptr<stud::render_host::TransportRing> ring;
    const auto read_request = [&](void* dst, uint32_t len) {
        return ring ? ring->read(dst, len, conn_fd) : reader.read(dst, len);
    };
    for (bool first = true; serving; first = false) {
        if (!first && !read_request(&hdr, sizeof(hdr))) break;
        if (!size_request_buffer(in_scratch, hdr)) break;
        if (hdr.in_buffer_len > 0 && !read_request(in_scratch.data(), hdr.in_buffer_len)) {
            break;
        }
        out_scratch.clear();
        uint32_t out_len = 0;
        uint64_t result = 0;
        throttle_before_dispatch(static_cast<stud::render_host::CallId>(hdr.call_id), in_scratch);
        if (hdr.call_id == CallId::TransportRing && !ring) {
            // The last request this socket carries: the client writes
            // nothing more to it until it has this answer, and nothing
            // at all once the answer is 1.
            const int memory_fd = stud::render_host::take_shared_fd(hdr.args[0]);
            const int consumer_fd = stud::render_host::take_shared_fd(hdr.args[1]);
            const int producer_fd = stud::render_host::take_shared_fd(hdr.args[2]);
            auto attached = std::make_unique<stud::render_host::TransportRing>();
            if (attached->attach(memory_fd, consumer_fd, producer_fd,
                                 static_cast<size_t>(hdr.args[3]))) {
                ring = std::move(attached);
                result = 1;
            }
            std::printf("stud-render-host: a client's requests now travel %s\n",
                        ring ? "through a shared-memory ring" : "over the socket");
            std::fflush(stdout);
        } else if (dispatches_without_lock(static_cast<stud::render_host::CallId>(hdr.call_id),
                                           in_scratch)) {
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
        //
        // STUD_WL_NO_PRESENT_PUMP=1 leaves this out, and it is the last
        // thing Stud does that no ordinary game does.
        //
        // A normal Vulkan game does not share its Wayland connection
        // with the driver: the toolkit owns the display and the driver's
        // WSI is the only other reader. Stud reads it HERE, with
        // prepare_read / read_events / dispatch, on the very thread that
        // just returned from vkQueuePresentKHR, after every present --
        // and the driver then enters the next present and reads the same
        // connection itself. libwayland coordinates reads per
        // connection, so a read or a cancel from Stud lands in the
        // middle of the driver's own.
        //
        // The freeze is a 12006ms spin inside the driver's present
        // (thread in state R, wchan 0,
        // GPU idle, no Xid), and on Stud's side the frame it happens on
        // is indistinguishable from the eighty healthy frames before it
        // -- same barriers, same two command buffers, every other
        // present in that window under 247us. Nothing in what Stud
        // SUBMITS explains it, which leaves what Stud does to the
        // connection the driver is using.
        //
        // A switch rather than a deletion: this pump exists because
        // presents once did not complete without it (the main loop was
        // blocked on the same dispatch mutex, so nothing serviced the
        // display and every frame returned VK_SUCCESS while showing
        // nothing). The main loop still pumps on its own cadence, so
        // removing this should cost latency rather than correctness --
        // should, which is why it is measured and not assumed.
        static const bool no_present_pump =
            std::getenv("STUD_WL_NO_PRESENT_PUMP") != nullptr;
        if (hdr.call_id == CallId::VkQueuePresentKHR && !no_present_pump) {
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
// audio device's destructor joins its opener thread, EGL tears down a
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
    // A write into a child that already exited (a closed web view, a
    // keyring helper that never read its input) is an EPIPE every caller
    // already handles, not a reason for the game to lose its window.
    // Caught rather than ignored: a caught signal is reset by exec, so
    // the helpers this process starts still get the default.
    struct sigaction pipe_action {};
    pipe_action.sa_handler = [](int) {};
    ::sigaction(SIGPIPE, &pipe_action, nullptr);

    // A web-view viewer is a session leader of its own, so nothing takes
    // it down with this process, live-reported as a panel still on
    // screen after Stud had closed. The ordinary shutdown paths close
    // them explicitly; this covers being killed instead. Handler-safe:
    // kill() and _exit() are both async-signal-safe, and the pid list is
    // only appended to from the dispatch thread.
    struct sigaction sa{};
    sa.sa_handler = +[](int sig) {
        close_open_web_views();
        // Asked for, not suffered: TERM/INT/HUP are how the tray's Exit
        // and an ordinary close reach this process. Recording it is what
        // stops Process A reporting a crash for a shutdown the user
        // chose. Anything that does NOT come through here -- SIGKILL,
        // the OOM killer, a driver fault -- leaves no note, and that
        // absence is the crash signal.
        stud::logging::note_clean_exit();
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
        } else if (std::string_view(argv[i]) == "--desktop-materials") {
            stud::render_host::vk_set_desktop_materials_dir(argv[i + 1]);
            gl_desktop_fill::g_dir = argv[i + 1];
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
        } else if (std::string_view(argv[i]) == "--upscaler") {
            // Say so when the environment is about to win, because it
            // wins SILENTLY and outlives the in-app restart button: the
            // button restarts the children, which inherit this process's
            // environment, so a STUD_UPSCALER left over from a test run
            // defeats every subsequent change in Settings. Caught
            // exactly that way -- a filter picked in Settings, restarted,
            // and a different one ran.
            if (const char* env = std::getenv("STUD_UPSCALER")) {
                if (std::string_view(env) != std::string_view(argv[i + 1])) {
                    std::printf("stud-render-host: STUD_UPSCALER=%s in the environment "
                                "overrides the \"%s\" chosen in Settings. Unset it to let "
                                "Settings decide.\n",
                                env, argv[i + 1]);
                    std::fflush(stdout);
                }
            }
            // TEMPORARY, with the rest of the filter comparison. Set as
            // the environment variable the render side already reads, so
            // there is one parser for these names and it lives next to
            // the shaders it chooses between; an unknown one falls back
            // to fsr there and says so.
            //
            // setenv's third argument is 0 deliberately: STUD_UPSCALER
            // set by hand outranks the saved setting, so a test run does
            // not have to go through Settings and change what is saved.
            ::setenv("STUD_UPSCALER", argv[i + 1], 0);
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
        std::printf("stud-render-host: upscaling: the engine renders at the window's logical "
                    "size, Stud writes the window's own resolution, sharpening pinned at "
                    "maximum\n");
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

    // The system's own EGL and GLES, loaded in Vulkan mode as well: nothing
    // calls into them there, but the engine's GL entry points still resolve.
    try {
        stud::render::use_system_gl_libraries();
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
    // The GL path scales in Stud whenever the Vulkan path would: with the
    // upscaler on, and always on X11, where nothing else scales a buffer
    // smaller than the window (see sync_vk_window_size). On Wayland the
    // window surface is then at the display's pixels, set before it exists.
    g_gl_scale_in_stud = !g_prefer_vulkan && (g_upscaling_enabled || on_x11);
    if (g_gl_scale_in_stud && !on_x11) {
        stud::android_glue::native_window_set_egl_at_display_size(true);
    }
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

    // OpenGL mode's engine GLES through gfxstream, on this window's own EGL
    // display.
    if (!g_prefer_vulkan && stud::render_host::gfxstream_host::available()) {
        constexpr unsigned int kPlatformX11 = 0x31D5;
        constexpr unsigned int kPlatformWayland = 0x31D8;
        const std::string sock = stud::render_host::default_socket_path();
        const std::string dir = sock.substr(0, sock.find_last_of('/'));
        stud::render_host::gfxstream_host::start(
            real_window.on_x11() ? kPlatformX11 : kPlatformWayland,
            real_window.on_x11() ? real_window.x11_display
                                 : static_cast<void*>(real_window.display),
            dir + "/gfxstream.sock");
    }

    RealFns fns{};
#define RESOLVE(name) fns.name##_ = must_resolve<PFN_##name>(#name)
    RESOLVE(eglInitialize); RESOLVE(eglBindAPI); RESOLVE(eglChooseConfig);
    RESOLVE(eglCreateWindowSurface); RESOLVE(eglCreateContext); RESOLVE(eglMakeCurrent);
    RESOLVE(eglSwapBuffers); RESOLVE(eglGetError); RESOLVE(eglQueryString);
    RESOLVE(eglQuerySurface); RESOLVE(eglSwapInterval);
    // Optional on purpose: EGL_EXT_platform_base is not on every EGL, and
    // making it required is what killed render-host at startup the first
    // time an EGL without it was selected.
    fns.eglGetPlatformDisplayEXT_ =
        reinterpret_cast<PFN_eglGetPlatformDisplayEXT>(stud::render::resolve("eglGetPlatformDisplayEXT"));
    RESOLVE(glActiveTexture); RESOLVE(glAttachShader); RESOLVE(glBindFramebuffer);
    RESOLVE(glBindTexture); RESOLVE(glCompileShader); RESOLVE(glCreateProgram);
    RESOLVE(glCreateShader); RESOLVE(glDeleteProgram); RESOLVE(glDeleteShader);
    RESOLVE(glDrawArrays); RESOLVE(glFramebufferTexture2D); RESOLVE(glLinkProgram);
    RESOLVE(glTexParameteri); RESOLVE(glUniform1i); RESOLVE(glUseProgram); RESOLVE(glViewport);
    RESOLVE(glGetUniformLocation); RESOLVE(glShaderSource); RESOLVE(glGetProgramInfoLog);
    RESOLVE(glGetShaderInfoLog); RESOLVE(glGenVertexArrays); RESOLVE(glBindVertexArray);
    RESOLVE(glBlitFramebuffer); RESOLVE(glGenFramebuffers); RESOLVE(glGenTextures);
    RESOLVE(glUniform4f); RESOLVE(glGetProgramiv); RESOLVE(glGetShaderiv); RESOLVE(glTexImage2D);
    RESOLVE(glCompressedTexImage2D);
#undef RESOLVE


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
        // comment: it can park this loop forever once EGL reads the
        // same queue from a render thread.
        pump_display(real_window, (pfds[1].revents & POLLIN) != 0);
        if (!(pfds[0].revents & POLLIN)) continue;
        int conn_fd = ::accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0) { std::perror("stud-render-host: accept"); continue; }
        std::printf("stud-render-host: client connected\n");
        // Process B connects more than once: each of its client libraries
        // (libvulkan, libaaudio, ...) links its own copy of the client and
        // opens its own socket. This loop services one connection inline,
        // so any further one used to sit in the accept backlog forever,
        // live-caught as the app freezing the instant a game started,
        // because that is when the engine's FMOD opens its audio device
        // and libaaudio's very first call blocked waiting for a reply that
        // could never come.
        //
        // Extra connections are served on their own threads, serialised
        // against this one by a dispatch mutex (the driver state they
        // share is not thread-safe). The first connection keeps running
        // inline so it also pumps the compositor.
        serve_extra_connections(listen_fd, fns, real_window);
        // The first connection never carries the shared-fd channel, which only
        // ever arrives on a socket of its own; see serve_connection_thread().
        stud::render_host::SocketReader reader(conn_fd);
        // As on the other connections: once the client asks for it, its
        // requests come out of shared memory; replies stay on the socket.
        std::unique_ptr<stud::render_host::TransportRing> ring;
        const auto read_request = [&](void* dst, uint32_t len) {
            return ring ? ring->read(dst, len, conn_fd) : reader.read(dst, len);
        };
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
            // itself a large share of the per-frame cost. What the reader
            // already holds comes first: a whole request can be sitting there
            // with the socket itself empty, and poll() would wait past it.
            serve_extra_connections(listen_fd, fns, real_window);
            if (ring) {
                // A request the client is still writing is read whole by
                // read_request() below, so any byte means one is coming.
                if (ring->readable() == 0) {
                    pollfd wl{wl_fd, POLLIN, 0};
                    if (!ring->wait(conn_fd, wl, wayland_poll_ms())) {
                        std::printf("stud-render-host: client disconnected\n");
                        break;
                    }
                    pump_display(real_window, (wl.revents & POLLIN) != 0);
                    if (ring->readable() == 0) continue;
                }
            } else {
                int pending = 0;
                const bool have_buffered =
                    reader.buffered() + (::ioctl(conn_fd, FIONREAD, &pending) == 0
                                             ? static_cast<size_t>(pending) : 0) >=
                    sizeof(Header);
                if (!have_buffered) {
                    ::poll(cpfds, 2, wayland_poll_ms());
                    pump_display(real_window, (cpfds[1].revents & POLLIN) != 0);
                    if (!(cpfds[0].revents & POLLIN)) continue;
                }
            }
            Header hdr{};
            if (!read_request(&hdr, sizeof(hdr))) {
                std::printf("stud-render-host: client disconnected\n");
                break;
            }
            if (!size_request_buffer(g_in_scratch, hdr)) break;
            if (hdr.in_buffer_len > 0 && !read_request(g_in_scratch.data(), hdr.in_buffer_len)) {
                break;
            }
            g_out_scratch.clear();
            uint32_t out_len = 0;
            uint64_t result = 0;
            // Same exemption as the connection threads: a blocking
            // driver wait must not hold the dispatch lock, or the two
            // wait on each other.
            throttle_before_dispatch(static_cast<stud::render_host::CallId>(hdr.call_id),
                                     g_in_scratch);
            if (hdr.call_id == CallId::TransportRing && !ring) {
                // The last request this socket carries, as on the other
                // connections.
                const int memory_fd = stud::render_host::take_shared_fd(hdr.args[0]);
                const int consumer_fd = stud::render_host::take_shared_fd(hdr.args[1]);
                const int producer_fd = stud::render_host::take_shared_fd(hdr.args[2]);
                auto attached = std::make_unique<stud::render_host::TransportRing>();
                if (attached->attach(memory_fd, consumer_fd, producer_fd,
                                     static_cast<size_t>(hdr.args[3]))) {
                    ring = std::move(attached);
                    result = 1;
                }
                std::printf("stud-render-host: the engine's requests now travel %s\n",
                            ring ? "through a shared-memory ring" : "over the socket");
                std::fflush(stdout);
            } else if (dispatches_without_lock(static_cast<stud::render_host::CallId>(hdr.call_id),
                                               g_in_scratch)) {
                result = dispatch(hdr, fns, real_window, g_in_scratch, g_out_scratch, &out_len);
            } else {
                std::lock_guard<std::mutex> lock(dispatch_mutex());
                result = dispatch(hdr, fns, real_window, g_in_scratch, g_out_scratch, &out_len);
            }
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
