// Process B's real entry point: a genuine bionic ELF executable, booted
// by real bionic's own linker64 (see bionic-runtime/'s process
// bootstrapper -- this binary is what it execve()s). Loads the real,
// unmodified libroblox.so via real bionic dlopen() (no Stud-authored
// ELF parsing needed for this anymore -- real linker64 already did the
// hard part). dlopen() itself is wrapped in trap_recovery below (see
// the doc comment at its call site): libroblox.so's own DT_INIT_ARRAY
// constructors run synchronously inside this call, and a real,
// evidence-based SIGSEGV during that phase was root-caused and is now
// handled (own SIGSEGV handler installed before dlopen() runs -- see
// the call site's own doc comment for why bionic's own fatal-signal
// path can't be trusted to surface it). Drives its JNI bootstrap and
// GameActivity lifecycle (jni-bridge/, ported to bionic this session),
// and renders through real bionic libEGL.so/libGLESv2.so (this
// session's own render-client stubs, forwarding to stud-render-host --
// a separate, real glibc process hosting ANGLE, see render-host/src/
// main.cpp's own doc comment for why).
//
// Reproduces the real, hard-won bring-up sequence and ordering already
// proven (across many sessions, under the old architecture) in
// runtime/src/main.cpp, with everything that was only needed for the
// old dual-ABI-same-process design removed: no stud::linker::
// load_library() (real dlopen() instead), no stud::render::resolve()
// (direct linked calls against real libEGL.so/libGLESv2.so instead, the
// exact same symbols Roblox itself calls), no tls-compat trampolines
// (same-ABI calls throughout).
//
// run_client_settings_bridge() IS called (below, gated on a real
// launch_payload being present) -- resolved differently than this
// comment used to claim: not a bionic-cross-built curl, but real
// content Process A already pre-fetched over HTTP and handed off via
// stud-ipc's own launch payload (see client_settings_bridge.h's own doc
// comment). A manual/diagnostic invocation with no --ipc-connect (no
// launch_payload at all) skips it, same degrade-gracefully treatment
// every other launch_payload-derived value gets in this file.

#include "stud/session_log.h"
#include "stud/activity_lifecycle_bridge.h"
#include "stud/activity_thread.h"
#include "stud/android_glue.h"
#include "stud/stud_paths.h"
#include "stud/app_bridge.h"
#include "stud/bionic_jvm.h"
#include "stud/bootstrap.h"
#include "stud/canonical_vm_registry.h"
#include "stud/client_settings_bridge.h"
#include "stud/device_params.h"
#include "stud/engine_v2_bridge.h"
#include "stud/system_theme_bridge.h"
#include "stud/webview_bridge.h"
#include "stud/webview_user_agent.h"
#include "stud/webview_cookies.h"
#include "stud/linking_bridge.h"
#include "stud/flag_overrides.h"
#include "stud/android_framework_stubs.h"
#include "stud/protocol_platform_stubs.h"
#include "stud/game_activity_stubs.h"
#include "stud/server_address.h"
#include "stud/game_engine_boot.h"
#include "stud/init_params.h"
#include "stud/ipc.h"
#include "stud/native_flags_bridge.h"
#include "stud/native_settings.h"
#include "render_client_common.h"
#include "stud/input_bridge.h"
#include "stud/start_app_params.h"
#include "stud/start_game_params.h"
#include "stud/ndk_types.h"
#include "stud/platform_params.h"
#include "stud/trap_recovery.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <cerrno>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <vector>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <netdb.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

// Names of the two entries Stud keeps in its own safe storage. Both are
// encrypted at rest with a key the system keyring holds; the keyring
// itself never sees a session cookie.
constexpr const char* kSessionCookieSecretName = "roblosecurity";
constexpr const char* kLocalStorageSecretName = "localstorage";
// The account switcher's own cookie. It carries the SET of signed-in
// accounts, so without persisting it only the last active account comes
// back after a restart -- every other one the user added is gone.
constexpr const char* kAccountListSecretName = "rbxas";


std::string find_named_arg(int argc, char** argv, std::string_view flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == flag) return argv[i + 1];
    }
    return "";
}

std::string cache_subdir(const char* leaf) {
    return stud::paths::cache_dir() + "/" + leaf;
}

// The engine's files directory is Android's getFilesDir(): what an app is
// expected to KEEP. Its own user settings and local storage live there,
// so it belongs in Stud's data directory, not the cache -- it used to sit
// in the cache, where a cache clean silently reset the user's graphics
// settings and forgot the signed-in account.
//
// The bulk of what the engine writes there is not settings, though: its
// asset store, the over-the-air app-shell patches and its logs ran to
// well over 100MB on this machine. Those are caches by nature, so each
// one is a symlink into the cache directory -- the engine writes through
// it without knowing, and `rm -rf ~/.cache/stud` reclaims the space
// while the settings and the login survive.
//
// Directories, deliberately, not files: the engine saves through a
// temporary file and a rename, which replaces a FILE symlink with a real
// file and silently strands the original. A rename inside a directory
// leaves the directory symlink itself untouched.
void link_engine_caches(const std::string& files_dir) {
    static constexpr const char* kCacheEntries[] = {"rbx-storage", "OTAPatchBackups", "logs",
                                                    "tmp-capture-storage"};
    const std::string app_data = files_dir + "/appData";
    const std::string overflow = stud::paths::engine_cache_overflow_dir();
    std::error_code ec;
    std::filesystem::create_directories(app_data, ec);
    for (const char* entry : kCacheEntries) {
        const std::filesystem::path target = std::filesystem::path(overflow) / entry;
        const std::filesystem::path link = std::filesystem::path(app_data) / entry;
        std::filesystem::create_directories(target, ec);
        const auto status = std::filesystem::symlink_status(link, ec);
        if (status.type() == std::filesystem::file_type::symlink) continue;
        if (std::filesystem::exists(status)) {
            // A real directory from an older layout: move what is in it
            // into the cache rather than dropping it, then link.
            std::filesystem::remove_all(target, ec);
            std::filesystem::rename(link, target, ec);
            if (ec) {
                ec.clear();
                std::filesystem::remove_all(link, ec);
            }
        }
        std::filesystem::create_directory_symlink(target, link, ec);
    }
}


std::atomic<bool> g_should_keep_running{true};
void handle_shutdown_signal(int) { g_should_keep_running.store(false, std::memory_order_relaxed); }

// Real render context: reuses the SAME ANativeWindow GameActivity's own
// onSurfaceCreatedNative already created (android-glue's per-jobject
// window_cache; under the new architecture, ANativeWindow_fromSurface's
// own forwarding stub always answers with Process C's one real window,
// so this and Roblox's own later real calls converge on the exact same
// object without any extra wiring). Direct, ordinary linked calls
// against real libEGL.so/libGLESv2.so -- no resolver needed, these ARE
// the real, exported bionic symbols now (this session's render-client
// stubs), the same ones Roblox's own compiled code calls.
struct RealRenderContext {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
};

std::optional<RealRenderContext> create_real_render_context(ANativeWindow* window) {
    if (window == nullptr) return std::nullopt;
    RealRenderContext ctx;
    ctx.display = eglGetDisplay(nullptr);
    if (ctx.display == EGL_NO_DISPLAY) {
        std::fprintf(stderr, "stud: eglGetDisplay failed\n");
        return std::nullopt;
    }
    EGLint major = 0, minor = 0;
    if (eglInitialize(ctx.display, &major, &minor) != EGL_TRUE) {
        std::fprintf(stderr, "stud: eglInitialize failed, error=0x%x\n", eglGetError());
        return std::nullopt;
    }
    std::printf("stud: real EGL initialized, version %d.%d, vendor=%s\n", major, minor,
                eglQueryString(ctx.display, EGL_VENDOR));
    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        std::fprintf(stderr, "stud: eglBindAPI failed\n");
        return std::nullopt;
    }
    EGLConfig config;
    EGLint num_configs = 0;
    if (eglChooseConfig(ctx.display, nullptr, &config, 1, &num_configs) != EGL_TRUE ||
        num_configs == 0) {
        std::fprintf(stderr, "stud: eglChooseConfig failed, error=0x%x\n", eglGetError());
        return std::nullopt;
    }
    ctx.surface =
        eglCreateWindowSurface(ctx.display, config, reinterpret_cast<EGLNativeWindowType>(window),
                                nullptr);
    if (ctx.surface == EGL_NO_SURFACE) {
        std::fprintf(stderr, "stud: eglCreateWindowSurface failed, error=0x%x\n", eglGetError());
        return std::nullopt;
    }
    const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    ctx.context = eglCreateContext(ctx.display, config, EGL_NO_CONTEXT, context_attribs);
    if (ctx.context == EGL_NO_CONTEXT) {
        std::fprintf(stderr, "stud: eglCreateContext failed, error=0x%x\n", eglGetError());
        return std::nullopt;
    }
    if (eglMakeCurrent(ctx.display, ctx.surface, ctx.surface, ctx.context) != EGL_TRUE) {
        std::fprintf(stderr, "stud: eglMakeCurrent failed, error=0x%x\n", eglGetError());
        return std::nullopt;
    }
    std::printf("stud: real GL_RENDERER=%s\n", glGetString(GL_RENDERER));
    std::printf("stud: real GL_VERSION=%s\n", glGetString(GL_VERSION));
    return ctx;
}

}  // namespace


// The engine's own renderer preference, as the real flags that decide it.
//
// All four names are present in this build (checked with `strings`), and
// they are what every other launcher uses for the same purpose. Stud
// sends them through ClientSettings rather than the preload path,
// because that is the channel the engine genuinely applies -- the same
// reasoning as the hand-edited overrides below.
//
// This does NOT weaken flag_overrides.h's locked decision. That rule is
// about the user's own FFlag file staying hand-edited and never
// generated from a toggle: these are Stud's own defaults for a setting
// that has no other way to be honoured, and the user's file is merged
// AFTER them, so anything they set by hand still wins.

// ClientSettings names a flag with its TYPE PREFIX -- a real response
// from clientsettingscdn spells them `FFlagX`, `DFFlagX`, `FIntX`,
// `DFIntX`, `FStringX`, `FLogX`, ... (checked against a live response:
// 22392 keys, every one prefixed). The engine's own getter takes the
// BARE name, which is why Stud's override file uses bare names -- but
// the merge wrote those bare names straight into applicationSettings,
// where nothing ever looked for them. That is why every flag merged
// this way has read back as unchanged.
//
// A name the user already prefixed is left alone; otherwise the prefix
// is taken from the value's type, which is the same rule the published
// flag lists follow.
std::string client_settings_key(const std::string& name, const std::string& value) {
    static const char* const kPrefixes[] = {"DFFlag", "FFlag",  "DFInt", "FInt",
                                            "DFString", "FString", "DFLog", "FLog", "SFFlag"};
    for (const char* prefix : kPrefixes) {
        if (name.rfind(prefix, 0) == 0) return name;
    }
    // Everything crosses the wire as a string, so the type is read off
    // the value the same way the engine's own settings parser has to.
    if (value == "True" || value == "False") return "FFlag" + name;
    const bool numeric = !value.empty() &&
                         value.find_first_not_of("-0123456789") == std::string::npos;
    if (numeric) return "FInt" + name;
    return "FString" + name;
}

// Stud's own engine defaults, as already-serialised ClientSettings
// values. Merged BEFORE the user's hand-edited file, so anything they
// set by hand still wins -- the same rule as the renderer flags below.
std::map<std::string, std::string> stud_default_flags() {
    return {
        // Highest texture quality, always. The engine's own quality
        // control is tuned for a phone's memory budget, and Stud is not
        // running on one -- a desktop GPU has no reason to be served
        // reduced textures. 4 is the top of the range.
        {"TextureQualityOverride", "4"},
        {"DFFlagTextureQualityOverrideEnabled", "True"},
    };
}

std::map<std::string, bool> renderer_flags_for_mode(const std::string& mode) {
    if (mode == "opengl") {
        return {{"DebugGraphicsPreferOpenGL", true},
                {"DebugGraphicsPreferVulkan", false},
                {"DebugGraphicsDisableVulkan", true},
                {"DebugGraphicsDisableOpenGL", false}};
    }
    if (mode == "vulkan") {
        return {{"DebugGraphicsPreferVulkan", true},
                {"DebugGraphicsPreferOpenGL", false},
                {"DebugGraphicsDisableVulkan", false},
                {"DebugGraphicsDisableOpenGL", false}};
    }
    return {};  // nothing asked for: leave the engine's own default alone
}


int main(int argc, char** argv) {
    // Before anything else prints: the session log Process A named, so
    // this process's whole bring-up is in it (stud/session_log.h).
    stud::logging::start_session_log_from_env();
    // Process B's own fatal-signal handling is trap_recovery's, which
    // recovers rather than reports -- it installs itself later and must
    // win, so nothing is installed here.

    // Unbuffered: a fatal, uncaught signal skips atexit/stdio-flush
    // handlers entirely, so any buffered stdout written before it is
    // silently lost -- real, repeated cost this session when a crash
    // happened early enough that stdout's ~4KB buffer never filled.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Real fix for a genuine, confirmed-live diagnostic gap: bionic's
    // own abort() (and Scudo/dlmalloc heap-corruption/double-free
    // detection, and assertion failures) logs a real explanatory
    // message via liblog, at ANDROID_LOG_FATAL, BEFORE calling abort()
    // -- the actual reason for a crash, not just "SIGABRT happened".
    // liblog's default backend targets logd over a /dev/socket/logdw
    // Unix socket, which doesn't exist in this bwrap sandbox (no init,
    // no logd) -- every one of these diagnostic messages was being
    // silently dropped, matching this session's own repeated "libc:
    // failed to connect to tombstoned" observation. liblog exposes a
    // real, public API for exactly this case,
    // __android_log_set_logger(__android_log_stderr_logger), redirecting
    // every log message (this process's own and bionic's internal ones
    // alike) straight to stderr instead, no logd required -- confirmed
    // exported by the real, extracted liblog.so (the library's exported symbols liblog.so).
    // Resolved via dlsym rather than declared/called directly: both
    // symbols are __INTRODUCED_IN(30) in the NDK headers, but
    // process-b targets ANDROID_PLATFORM=26 to match Roblox's own
    // minSdkVersion - the real symbols exist in the bound liblog.so
    // regardless (bionic doesn't strip newer symbols from older-target
    // binaries at runtime, only the compile-time header declares them
    // API-30+), so this sidesteps the compile-time guard the same way
    // stud::linker::LoadedLibrary already does for libroblox.so's own
    // symbols elsewhere in this project.
    using LoggerFn = void (*)(const void* log_message);
    using SetLoggerFn = void (*)(LoggerFn logger);
    if (void* set_logger_sym = ::dlsym(RTLD_DEFAULT, "__android_log_set_logger")) {
        if (void* stderr_logger_sym = ::dlsym(RTLD_DEFAULT, "__android_log_stderr_logger")) {
            reinterpret_cast<SetLoggerFn>(set_logger_sym)(
                reinterpret_cast<LoggerFn>(stderr_logger_sym));
        }
    }

    // Real logd sink (the engineering notes, "FLog output has never
    // appeared"). Live-caught via a live syscall trace: this process repeatedly calls
    // connect("/dev/socket/logdw") and gets ENOENT, over and over --
    // i.e. libroblox's own logging really is being emitted, straight to
    // Android's logd write socket, bypassing the public
    // __android_log_* API entirely (which is exactly why interposing
    // those functions previously observed ZERO calls). The socket
    // simply doesn't exist in Stud's sandbox, so every message is
    // dropped by the kernel.
    //
    // Creating a real SOCK_DGRAM socket at that path and reading it
    // gives Stud the engine's own real log stream -- the single most
    // valuable diagnostic this project has lacked for its whole
    // history. Bound before libroblox.so is ever dlopen()'d so nothing
    // is missed.
    //
    // Wire format is liblog's own: a packed header
    // (uint8 log_id, uint16 tid, uint32 sec, uint32 nsec) followed by
    // one priority byte, then NUL-terminated tag and message.
    {
        ::mkdir("/dev/socket", 0755);
        ::unlink("/dev/socket/logdw");
        int logd_fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (logd_fd >= 0) {
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", "/dev/socket/logdw");
            if (::bind(logd_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                std::printf("stud: logd sink listening on /dev/socket/logdw\n");
                std::fflush(stdout);
                std::thread([logd_fd] {
                    std::vector<char> buf(8192);
                    for (;;) {
                        ssize_t n = ::recv(logd_fd, buf.data(), buf.size(), 0);
                        if (n <= 0) {
                            if (n < 0 && (errno == EINTR)) continue;
                            if (n < 0) break;
                            continue;
                        }
                        constexpr size_t kHeader = 1 + 2 + 4 + 4;  // packed liblog header
                        if (static_cast<size_t>(n) <= kHeader + 1) continue;
                        const char* payload = buf.data() + kHeader;
                        size_t remaining = static_cast<size_t>(n) - kHeader;
                        int priority = static_cast<unsigned char>(payload[0]);
                        const char* tag = payload + 1;
                        size_t tag_max = remaining - 1;
                        size_t tag_len = ::strnlen(tag, tag_max);
                        const char* message = tag + tag_len + 1;
                        if (tag_len + 1 >= tag_max) continue;
                        size_t msg_max = tag_max - tag_len - 1;
                        size_t msg_len = ::strnlen(message, msg_max);
                        static const char* kPrio = "??VDIWEF";
                        char prio_char = (priority >= 0 && priority < 8) ? kPrio[priority] : '?';
                        std::printf("[logd:%c/%.*s] %.*s\n", prio_char, static_cast<int>(tag_len),
                                    tag, static_cast<int>(msg_len), message);
                        std::fflush(stdout);
                    }
                }).detach();
            } else {
                std::fprintf(stderr, "stud: could not bind /dev/socket/logdw: %s\n",
                             std::strerror(errno));
                ::close(logd_fd);
            }
        }
    }

    // Real, live-caught gap: on a real Android device, __system_properties_init()
    // is called once by Zygote/app_process during real system boot, long
    // before any app process is even forked -- Stud has no Zygote/
    // app_process layer at all, so nothing ever called it, meaning every
    // __system_property_get() anywhere in libroblox.so (not just the
    // real, live-traced DNS case that found this whole gap -- see
    // the engineering notes) was silently returning empty regardless of
    // whether a real, correctly-formatted property area file existed at
    // /dev/__properties__ (confirmed live: even with a byte-verified
    // real file bind-mounted there, /proc/<pid>/maps showed it was
    // never even mmap'd -- the real function that would do that was
    // simply never invoked). Real, confirmed-exported symbol (the library's exported symbols:
    // __system_properties_init@@LIBC_Q) -- called here, as early as
    // possible, same dlsym pattern as __android_log_set_logger above
    // (real reason for dlsym over a direct call: this project's own
    // NDK headers guard newer API-level symbols behind
    // __INTRODUCED_IN, but the real symbol exists in the bound libc.so
    // regardless of process-b's ANDROID_PLATFORM=26 build target).
    using SystemPropertiesInitFn = int (*)(void);
    if (void* init_sym = ::dlsym(RTLD_DEFAULT, "__system_properties_init")) {
        int rc = reinterpret_cast<SystemPropertiesInitFn>(init_sym)();
        std::printf("stud: __system_properties_init() -> %d\n", rc);
    } else {
        std::fprintf(stderr, "stud: __system_properties_init not found, skipping\n");
    }

    if (argc < 2) {
        std::fprintf(stderr,
                      "usage: %s <libroblox.so path> [--apk <path>] [--ipc-connect <socket>] "
                      "[--flag-overrides <path>] [--dns-servers <ip1,ip2>]\n",
                      argv[0]);
        return 1;
    }
    std::string so_path = argv[1];
    std::string apk_path = find_named_arg(argc, argv, "--apk");
    std::string ipc_socket_path = find_named_arg(argc, argv, "--ipc-connect");
    std::string flag_overrides_path = find_named_arg(argc, argv, "--flag-overrides");
    // How large the app draws everything, in 120ths (120 = 1.0). Absent or
    // 0 means "follow the display" -- which, with HiDPI off, means 1.0
    // rather than the desktop's scale: the buffer is then the window's
    // logical size, so laying out at 1.25 would draw everything a quarter
    // too large into a buffer that is not being scaled to match.
    const bool hidpi_enabled = find_named_arg(argc, argv, "--hidpi") != "off";
    // "Follow DPI" in Settings: lay out at the display's own scale, at
    // the cost of the engine's full render path. See where the scale is
    // decided, below, for what that costs and why it is off by default.
    const bool follow_dpi = find_named_arg(argc, argv, "--follow-dpi") == "on";
    // Which renderer the ENGINE should pick, which is a different
    // question from which backend render-host hands it. Settings' own
    // "Render path" used to answer only the second half: the engine
    // still chose Vulkan on its own, so picking OpenGL changed nothing
    // it actually did. See renderer_flags_for_mode() below.
    const std::string graphics_mode = find_named_arg(argc, argv, "--graphics-mode");
    const bool smooth_zoom_setting = find_named_arg(argc, argv, "--smooth-zoom") != "off";
    stud::jni_bridge::set_smooth_zoom_enabled(smooth_zoom_setting);
    std::printf("stud: smooth zoom %s\n", smooth_zoom_setting ? "on" : "off (per-notch, as Sober)");
    std::fflush(stdout);

    // Real fix for the DNS/join investigation documented at length in
    // the engineering notes' gap #1: net.dns1/net.dns2 (this project's
    // earlier guess) don't exist as literal strings anywhere in this
    // libc.so -- ground-truth-traced in the real,
    // exported _resolv_set_nameservers_for_net instead. Real signature
    // confirmed from the real body (matches AOSP's own documented
    // convention): int(unsigned netid, const char** servers, unsigned
    // numservers, const char* domains, const res_params* params) --
    // params==NULL takes the real default-timeout/retry path (seen in
    // its own param_5==NULL branch). netid=0 (NETID_UNSET)
    // is what an unspecified-network resolver lookup uses. This is the
    // real, direct mechanism netd normally drives on a real device;
    // Stud has no netd, so it's called here directly instead, same
    // dlsym-for-newer-symbol pattern as __system_properties_init above.
    // servers/domains come from Process A's own real /etc/resolv.conf
    // parse (bionic_runtime.cpp's real_host_nameservers()), threaded in
    // via --dns-servers rather than re-parsed here, since this is real
    // bionic code with no <fstream>/<sstream> convenience to spare.
    {
        std::string dns_servers_arg = find_named_arg(argc, argv, "--dns-servers");
        if (!dns_servers_arg.empty()) {
            std::vector<std::string> server_storage;
            size_t start = 0;
            while (start <= dns_servers_arg.size()) {
                size_t comma = dns_servers_arg.find(',', start);
                std::string piece = dns_servers_arg.substr(
                    start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!piece.empty()) server_storage.push_back(piece);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            using ResolvSetNameserversFn =
                int (*)(unsigned, const char**, unsigned, const char*, const void*);
            if (void* setns_sym =
                    ::dlsym(RTLD_DEFAULT, "_resolv_set_nameservers_for_net")) {
                std::vector<const char*> server_ptrs;
                for (const auto& s : server_storage) server_ptrs.push_back(s.c_str());
                int rc = reinterpret_cast<ResolvSetNameserversFn>(setns_sym)(
                    0, server_ptrs.data(), static_cast<unsigned>(server_ptrs.size()), "", nullptr);
                std::printf("stud: _resolv_set_nameservers_for_net(netid=0, %zu servers) -> %d\n",
                            server_ptrs.size(), rc);
            } else {
                std::fprintf(stderr,
                              "stud: _resolv_set_nameservers_for_net not found, skipping\n");
            }
        }
    }

    // Real, env-gated end-to-end proof for the fix above: registering a
    // nameserver via _resolv_set_nameservers_for_net succeeding (rc==0)
    // only proves the resolver cache accepted the entry -- it doesn't by
    // itself prove a real hostname lookup actually reaches that
    // nameserver and gets a real answer. Roblox's own real join-related
    // DNS lookups only happen deep inside an async, opaque task-queue
    // worker (see the engineering notes gap #1) that a bare, no-login diagnostic
    // launch never reaches, so this is the direct, narrow way to
    // live-test the resolver path itself in isolation, independent of
    // login/APK-config state this environment doesn't have. Off by
    // default -- real getaddrinfo() against an attacker-uncontrolled env
    // var would be a poor default to ship.
    if (const char* dns_test_host = std::getenv("STUD_DNS_TEST_HOST")) {
        struct addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* result = nullptr;
        int rc = ::getaddrinfo(dns_test_host, nullptr, &hints, &result);
        if (rc != 0) {
            std::printf("stud: getaddrinfo(\"%s\") -> error %d (%s)\n", dns_test_host, rc,
                        ::gai_strerror(rc));
        } else {
            char addr_buf[INET6_ADDRSTRLEN] = {};
            for (struct addrinfo* p = result; p != nullptr; p = p->ai_next) {
                const void* addr_ptr =
                    p->ai_family == AF_INET
                        ? static_cast<const void*>(
                              &reinterpret_cast<struct sockaddr_in*>(p->ai_addr)->sin_addr)
                        : static_cast<const void*>(
                              &reinterpret_cast<struct sockaddr_in6*>(p->ai_addr)->sin6_addr);
                ::inet_ntop(p->ai_family, addr_ptr, addr_buf, sizeof(addr_buf));
                std::printf("stud: getaddrinfo(\"%s\") -> %s\n", dns_test_host, addr_buf);
            }
            ::freeaddrinfo(result);
        }
    }

    // Real UI-process handoff (the session cookie plus base URLs Process
    // A already owns) -- optional: every diagnostic/manual invocation of
    // this binary omits it, same as the old architecture's own
    // --ipc-connect convention.
    std::optional<stud::ipc::LaunchPayload> launch_payload;
    if (!ipc_socket_path.empty()) {
        try {
            launch_payload = stud::ipc::receive_launch_payload(ipc_socket_path);
            std::printf("stud: received launch payload via IPC (session cookie %s, %zu bytes)\n",
                        launch_payload->session_cookie.empty() ? "absent" : "present",
                        launch_payload->session_cookie.size());
        } catch (const stud::ipc::IpcError& e) {
            std::fprintf(stderr, "stud: IPC launch handoff failed: %s\n", e.what());
        }
    }
    if (launch_payload) {
        // Before anything reads it: StartAppParams' selectedTheme is built
        // from this, and the app is told the same value through the real
        // system-theme protocol.
        stud::jni_bridge::set_system_dark_mode(launch_payload->system_dark_mode);
    }


    // Real, confirmed-live gap: Roblox's own code calls
    // boost::filesystem::canonical() on a real "android" directory,
    // expected to already exist as a sibling of the cache/files/assets
    // dirs below (all sharing $HOME/.cache/stud as their parent) --
    // canonical() throws on a missing directory (confirmed via a real
    // crash: "No such file or directory"). Distinct from, and NOT
    // subsumed by, the real --chdir fix elsewhere in this project (that
    // one fixed a wrong/read-only *cwd*; this is a specific *named*
    // directory Roblox expects regardless of cwd) -- directly
    // reverified this session by removing this exact block and
    // reproducing the identical crash string on a clean run, then
    // restoring it. Real provenance still not pinned to a specific
    // native call site (no stack trace available), but the fix itself
    // is concrete and now doubly falsified-if-wrong, not just assumed.
    {
        std::error_code ec;
        std::filesystem::create_directories(cache_subdir("android"), ec);
    }

    // Real app version, from the configured APK's own manifest, before
    // anything can ask for it. Every consumer below reads this rather
    // than a literal.
    //
    // It starts EMPTY on purpose. This used to be seeded with a version
    // string, which meant a failed manifest read silently reported a
    // build the user is not running -- indistinguishable, everywhere it
    // is sent, from a real reading. An empty version is visibly wrong,
    // which is what an unreadable APK should look like.
    std::string real_app_version;
    if (!apk_path.empty()) {
        const std::string manifest_version = stud::android_glue::apk_version_name(apk_path);
        if (!manifest_version.empty()) {
            real_app_version = manifest_version;
        } else {
            std::fprintf(stderr,
                         "stud: warning: could not read versionName from %s -- reporting no "
                         "version rather than inventing one\n",
                         apk_path.c_str());
        }
    }
    stud::jni_bridge::set_real_app_version(real_app_version);
    std::printf("stud: app version: %s\n", real_app_version.c_str());
    std::fflush(stdout);

    std::string asset_dir;
    if (!apk_path.empty()) {
        asset_dir = cache_subdir("assets");
        // Clear everything derived from the previous APK when the user
        // picks a different one. Extraction alone only overwrites files
        // that still exist in the new build, so assets deleted between
        // versions used to linger forever, mixed in with the new ones --
        // and the engine's own caches (rbx-storage, the flag cache, the
        // decompressed-model cache) are keyed to the build that wrote
        // them. Live-reported: "stud is still using roblox 2.733. it is
        // not clearing stuff whenever a new apk is saved."
        {
            const std::string fingerprint = stud::android_glue::apk_source_fingerprint(apk_path);
            const std::string stamp_path = cache_subdir("assets.source");
            std::string previous;
            if (std::ifstream stamp{stamp_path}) {
                std::getline(stamp, previous);
            }
            if (!fingerprint.empty() && previous != fingerprint) {
                std::error_code ec;
                if (!previous.empty()) {
                    std::printf("stud: configured APK changed -- clearing extracted assets and "
                                "engine caches\n");
                    std::fflush(stdout);
                }
                std::filesystem::remove_all(asset_dir, ec);
                std::filesystem::remove_all(cache_subdir("cache"), ec);
                // files/ holds the previous build's provisioned CA bundle
                // and preferences; android_id/ deliberately survives, since
                // the device identity is Stud's, not the APK's.
                std::filesystem::remove_all(stud::paths::engine_files_dir(), ec);
                std::ofstream out{stamp_path, std::ios::trunc};
                out << fingerprint << "\n";
            }
        }
        try {
            stud::android_glue::extract_apk_assets(apk_path, asset_dir);
            std::printf("stud: extracted assets from %s into %s\n", apk_path.c_str(), asset_dir.c_str());
            // Provision the engine's CA bundle here, before dlopen(), not
            // only in the settings bootstrap further down. The engine
            // issues its first HTTPS request within ~100ms of loading,
            // which beat the later copy: live-caught as a single
            // `SSL certificate ... unable to get local issuer certificate`
            // at 0.099s, with every later request fine. The copy down
            // there stays -- it is the one that runs when the engine is
            // driven without an APK path -- and both are idempotent.
            {
                std::error_code cert_ec;
                const std::string early_exe_dir = stud::paths::engine_files_dir() + "/exe";
                std::filesystem::create_directories(early_exe_dir, cert_ec);
                if (!cert_ec) {
                    std::filesystem::copy_file(asset_dir + "/ssl/cacert.pem",
                                                early_exe_dir + "/cacert.pem",
                                                std::filesystem::copy_options::overwrite_existing,
                                                cert_ec);
                }
                if (cert_ec) {
                    std::fprintf(stderr, "stud: warning: failed to pre-provision %s/cacert.pem: %s\n",
                                 early_exe_dir.c_str(), cert_ec.message().c_str());
                }
            }
        } catch (const stud::android_glue::ExtractError& e) {
            std::fprintf(stderr, "stud: %s\n", e.what());
            return 1;
        }
    } else {
        asset_dir = "/tmp/stud-assets-placeholder";
    }
    stud::android_glue::set_asset_base_directory(asset_dir);

    // Real, structural reorder (the engineering notes, "build real framework,
    // run it for real" direction): this whole block used to run AFTER
    // dlopen() below. Real, live-caught evidence this session: the
    // Djinni classloader-bootstrap idiom (FindClass(NativeObjectManager)
    // -> GetObjectClass -> getClassLoader() -> loadClass/findClass)
    // fires from inside libroblox.so's own real DT_INIT_ARRAY static
    // constructors -- i.e. DURING dlopen() itself, before dlopen() even
    // returns -- so registering FakeJni stub classes (including
    // ClassLoaderStub/ClassMetaStub, see android_framework_stubs.h) only
    // after dlopen() returns is structurally too late for this specific
    // idiom, even though it was already correctly timed for
    // JNI_OnLoad()-triggered lookups (a separate, later, explicit call
    // Stud makes itself). Matches real Android's own actual process
    // order too: a real device's framework classes are already loaded
    // and available in the process before an app's own native libraries
    // are dlopen()'d, never the other way around. Moving the Jvm
    // itself, the canonical-VM registration, and every stub-class
    // registration ahead of dlopen() fixes the ordering for both real
    // bootstrap moments at once, not just JNI_OnLoad's.
    stud::jni_bridge::BionicAwareJvm jvm;
    stud::jni_bridge::set_process_wide_jvm_for_thread_attach(&jvm);
    stud::jni_bridge::register_canonical_java_vm(jvm.GetBionicSafeJavaVM());
    jvm.registerClass<stud::jni_bridge::StartAppParams>();
    jvm.registerClass<stud::jni_bridge::StartGameParams>();
    jvm.registerClass<stud::jni_bridge::PlatformParams>();
    jvm.registerClass<stud::jni_bridge::PlatformParamsWithLuaFlags>();
    jvm.registerClass<stud::jni_bridge::DeviceParams>();
    jvm.registerClass<stud::jni_bridge::InitParams>();
    stud::jni_bridge::register_android_framework_stubs(jvm);
    stud::jni_bridge::register_game_activity_stubs(jvm);
    stud::jni_bridge::register_protocol_platform_stubs(jvm);

    // Real, live-caught ordering bug: libroblox reads
    // `NativeGLJavaInterface.getDeviceStaticParams()` during its OWN
    // JNI_OnLoad/static-constructor phase -- i.e. inside dlopen() below,
    // long before any Stud bring-up call runs. Setting it later (which is
    // where this used to live, next to the V2 params) was always too late:
    // the engine logged `W/JNIMain: DeviceStaticParams is null.` on line
    // 39 of every capture and cached that null. Set it here, before
    // dlopen(), so the engine's very first read gets the real object.
    // These builders are pure value construction -- they need neither the
    // loaded library nor a live JNI call -- so running them this early is
    // safe.
    stud::jni_bridge::NativeGLJavaInterfaceStub::setDeviceStaticParams(
        stud::jni_bridge::build_desktop_device_static_params(
            stud::jni_bridge::build_desktop_device_params("34", "Stud", real_app_version, "1920x1080",
                                                           1920, 1080, 16384)));

    // Real dlopen() -- real bionic's own linker64 resolves libroblox.so's
    // entire dependency graph natively. No Stud-authored ELF
    // parsing/resolver chain involved for this anymore.
    //
    // Armed with the trap-recovery handler for this call specifically:
    // libroblox.so's own DT_INIT_ARRAY constructors run synchronously
    // inside dlopen(), before this function returns, and (real, evidence-
    // based finding this session) can SIGSEGV during that phase. Left
    // unhandled, real bionic's own built-in fatal-signal handler catches
    // it first, fails to reach tombstoned (sandboxed, no such service),
    // and its own fallback unwindstack-based backtrace logic then
    // SIGSEGVs a second time walking Process B's stack -- so a naive
    // coredump capture shows only that secondary crash (inside
    // unwindstack::MapInfo::CreateMemory, confirmed via `coredumpctl
    // debug` + `add-symbol-file` against the real linker64 binary), not
    // the real, original fault site. Installing our own SIGSEGV handler
    // before dlopen() runs means bionic never gets first crack at it.
    void* lib_handle = nullptr;
    bool dlopen_completed = stud::jni_bridge::call_trapping_abort_with_result(
        [](const char* path) { return ::dlopen(path, RTLD_NOW); }, lib_handle, so_path.c_str());
    if (!dlopen_completed) {
        std::fprintf(stderr, "stud: dlopen(%s) trapped a fatal signal mid-call\n", so_path.c_str());
        return 1;
    }
    if (lib_handle == nullptr) {
        std::fprintf(stderr, "stud: dlopen(%s) failed: %s\n", so_path.c_str(), ::dlerror());
        return 1;
    }
    std::printf("stud: loaded %s successfully (handle=%p)\n", so_path.c_str(), lib_handle);
    stud::linker::LoadedLibrary lib(lib_handle);

    jvm.attachLibrary("");

    stud::jni_bridge::JniOnLoadResult on_load_result;
    auto post_ctor_hook = stud::jni_bridge::make_post_constructor_hook(jvm, so_path, on_load_result);
    post_ctor_hook(lib);
    if (on_load_result.found) {
        std::printf(on_load_result.trapped_abort
                        ? "stud: JNI_OnLoad() called abort() mid-call -- trapped, continuing\n"
                        : "stud: JNI_OnLoad() returned %d\n",
                    on_load_result.result);
    }

    // Real display facts, asked once of the process that owns the window
    // (Process C -- the only one with a compositor connection) and reused
    // for every params build below. These used to be hardcoded
    // `1.0f, 340, 190`: a dpi scale that ignored the desktop's own
    // scaling, and a viewport size in millimetres that described no real
    // display at all. The engine lays its whole UI out from these, so a
    // 1.25x desktop rendered every element 1.25x too small.
    float real_density = 1.0f;
    // The density the app lays out against: the display's while HiDPI is
    // on, 1.0 while it is off. Distinct from real_density, which always
    // describes the SCREEN and is what the User-Agent reports.
    float layout_density = 1.0f;
    // Deliberately NOT the desktop's scale factor. Tried live and
    // reverted on the user's own comparison against Sober: feeding the
    // real 1.25 here makes the engine lay its whole UI out 1.25x larger,
    // which is bigger than Sober draws it on the same machine. The
    // sharpness win of HiDPI comes from the buffer being scaled (the
    // compositor maps it 1:1 to physical pixels); the engine's own UI
    // scale is a separate, independent choice, and 1.0 is the one that
    // matches the reference client. Keep the two apart -- raising this
    // does not make anything sharper, only larger.
    // Set from the display's real scale, just below, once it is known.
    //
    // This is the Lua app's own UI scale, and it has to match the buffer
    // upscale or Home comes out the wrong physical size. With a 1.25x
    // buffer and a 1.0 dpiScale the app lays its UI out for 1728 device
    // pixels as if they were 1728 points -- sharp, but visibly smaller
    // than the same UI in-game, which does not go through this path.
    // Live-reported exactly that way: "in-game dpi is correct, home is
    // sharper and smaller".
    //
    // An earlier session set this to 1.0 deliberately, on a side-by-side
    // where 1.25 looked larger than Sober. That comparison was made when
    // the buffer, DisplayMetrics and the window disagreed with each other
    // anyway, so it was not measuring this knob alone; with the rest now
    // consistent, the scale that matches the buffer is the correct one.
    float engine_dpi_scale = 1.0f;
    int real_viewport_mm_w = 340;
    int real_viewport_mm_h = 190;
    {
        uint64_t args[8] = {};
        uint64_t scale_120 = stud::render_client::connection().call(
            stud::render_host::CallId::GetWindowBufferScale, args, nullptr, 0, nullptr, 0, nullptr);
        // Sanity range: 0.5x to 8x. Outside that is not a real desktop
        // scale, so keep 1.0 rather than trust it.
        if (scale_120 >= 60 && scale_120 <= 960) {
            real_density = static_cast<float>(scale_120) / 120.0f;
        }
        uint64_t geom = stud::render_client::connection().call(
            stud::render_host::CallId::GetDisplayOutputGeometry, args, nullptr, 0, nullptr, 0,
            nullptr);
        const int out_px_w = static_cast<int>((geom >> 48) & 0xffff);
        const int out_px_h = static_cast<int>((geom >> 32) & 0xffff);
        const int mm_w = static_cast<int>((geom >> 16) & 0xffff);
        const int mm_h = static_cast<int>(geom & 0xffff);
        if (mm_w > 0 && mm_h > 0) {
            real_viewport_mm_w = mm_w;
            real_viewport_mm_h = mm_h;
        }
        // Publish it here, not several hundred lines later. The
        // User-Agent is built a few lines below and describes the
        // SCREEN; without this it fell back to the window's size and a
        // synthesised 160dpi, and the agent that reached Roblox was not
        // the one that was tested.
        stud::jni_bridge::set_real_display_output_geometry(out_px_w, out_px_h, mm_w, mm_h);
        // The real density, for anything that describes the display.
        // DisplayMetrics itself is seeded 1.0 below, deliberately.
        stud::jni_bridge::set_measured_display_density(real_density);
        // 1.0 unless "Follow DPI" is on, in both HiDPI modes.
        //
        // The engine picks its render technique from this one number: at
        // a screen DPI scale above 1.0 it takes its simplified path --
        // no SSAO, flatter shadows -- and the graphics-quality level is
        // read and then discarded by that branch, so nothing else brings
        // them back (the engineering notes, "HiDPI turned SSAO off"). The same
        // number is the size of a UI point, so raising it to match a
        // scaled desktop is exactly what costs the SSAO.
        //
        // Live-measured at 1.00 (SSAO), 1.10 (none) and 1.25 (none) --
        // the boundary is exactly 1.0, and no graphics-quality setting
        // overrides it. So the default keeps the render path (the thing
        // the reference client does not have at all) and "Follow DPI"
        // is the way to ask for the other side of the trade, warned
        // about where it is offered. At 1.0 a UI point is one buffer
        // pixel, which also makes the input conversions the identity
        // whichever way HiDPI is set.
        // Only "Follow DPI" moves this off 1.0, and only while HiDPI is
        // on -- the setting is greyed out otherwise. With HiDPI off the
        // buffer is the window's logical size and the compositor scales
        // it, so the engine is left at 1.0 there.
        layout_density = (follow_dpi && hidpi_enabled) ? real_density : 1.0f;
        engine_dpi_scale = layout_density;
        std::printf("stud: engine layout scale %.2f (follow DPI %s, hidpi %s, display %.2f)\n",
                    static_cast<double>(engine_dpi_scale), follow_dpi ? "on" : "off",
                    hidpi_enabled ? "on" : "off", static_cast<double>(real_density));
        std::fflush(stdout);
        // TEST ONLY, off unless set: DisplayMetrics density reported to the
        // engine, independent of PlatformParams.dpiScale (SSAO investigation).
        if (const char* v = std::getenv("STUD_TEST_METRICS_DENSITY")) {
            layout_density = static_cast<float>(std::atof(v));
            std::printf("stud: TEST DisplayMetrics density forced to %.2f\n",
                        static_cast<double>(layout_density));
        }
        std::printf("stud: real display: density=%.2f, viewport %dx%d mm\n",
                    static_cast<double>(real_density), real_viewport_mm_w, real_viewport_mm_h);
        std::fflush(stdout);
    }

    auto platform_params = stud::jni_bridge::build_desktop_platform_params_with_lua_flags(
        asset_dir, engine_dpi_scale, real_viewport_mm_w, real_viewport_mm_h);
    auto device_params = stud::jni_bridge::build_desktop_device_params(
        "34", "Stud", real_app_version, "1920x1080", 1920, 1080, 16384);
    // Real, confirmed against the app's own code, previously-missing call (the engineering notes,
    // "instantiate controllers" investigation): the real D2()
    // (setInitParamsForEngine) calls this FIRST, strictly before
    // building InitParams -- see bootstrap.h's own doc comment on
    // run_native_set_device_info() for the full real trace. Matching
    // that exact real order here, not just calling it somewhere.
    try {
        auto set_device_info_result =
            stud::jni_bridge::run_native_set_device_info(jvm, lib, device_params);
        std::printf("stud: run_native_set_device_info() complete: called=%d trapped_abort=%d\n",
                    set_device_info_result.called, set_device_info_result.trapped_abort);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stud: run_native_set_device_info() failed: %s\n", e.what());
    }
    // The real app's own User-Agent, not a Stud placeholder. This is the
    // agent the engine's own HTTP client sends on every request it makes,
    // including the join. Stud hosts the real, unmodified app, so this is
    // an honest description of what is running -- see
    // build_real_user_agent().
    // Seed the display metrics BEFORE building the User-Agent.
    //
    // The agent describes this screen -- size in pixels, dpi, and size in
    // density-independent pixels -- and it is built here because
    // nativeAppBridgeAppStart needs it. The metrics used to be seeded
    // several hundred lines later, so the agent reported the 800x600
    // pre-window default and every screen number in it was wrong.
    // Process C owns the window and can answer at any time, so ask now.
    {
        uint64_t size_args[8] = {};
        const uint64_t packed = stud::render_client::connection().call(
            stud::render_host::CallId::GetWindowSize, size_args, nullptr, 0, nullptr, 0, nullptr);
        if (packed != 0) {
            // ONE density, decided here, from the system, and never
            // changed again for the life of the process.
            //
            // This used to seed 1.0 and then re-seed the real scale
            // several hundred lines later, so DisplayMetrics genuinely
            // changed underneath a running app: everything laid out
            // during bring-up used one DPI and anything that re-laid out
            // afterwards -- switching accounts re-inits the Lua app --
            // used another. That is the "DPI changes on events, and
            // differs between launches" the user reported, and it was
            // Stud's own doing.
            stud::jni_bridge::set_real_display_metrics(static_cast<int>(packed >> 32),
                                                        static_cast<int>(packed & 0xffffffffu),
                                                        layout_density);
        }
    }
    const std::string real_user_agent = stud::jni_bridge::build_real_user_agent();
    auto init_params = stud::jni_bridge::build_desktop_init_params(
        platform_params, device_params, "https://www.roblox.com", real_user_agent);
    stud::jni_bridge::FlagOverrides overrides;
    if (!flag_overrides_path.empty()) {
        try {
            overrides = stud::jni_bridge::FlagOverrides::load_from_file(flag_overrides_path);
            std::printf("stud: loaded %zu FFlag override(s) from %s\n", overrides.size(),
                        flag_overrides_path.c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "stud: failed to load FFlag overrides from %s: %s\n",
                         flag_overrides_path.c_str(), e.what());
        }
    }

    // Real device precondition GameActivity's own init code depends on.
    ALooper_prepare(0);

    // STUD_VULKAN_CALL_TRACE=1: report why the engine cannot load Vulkan.
    //
    // The engine logs only "Mode 6 failed: Unable to load Vulkan API" and
    // then falls back to the glsles3 shader pack. a live syscall trace shows it really
    // does open /system/lib64/libvulkan.so.1 successfully and then tries
    // the "libvulkan.so" fallback, which means dlopen opened the file and
    // failed to *load* it -- and bionic puts that reason in dlerror(),
    // which never reaches any log. Doing the same dlopen here surfaces it.
    if (const char* vk_trace = std::getenv("STUD_VULKAN_CALL_TRACE");
        vk_trace != nullptr && std::string_view(vk_trace) == "1") {
        ::dlerror();
        void* vk = ::dlopen("libvulkan.so.1", RTLD_NOW);
        if (vk == nullptr) {
            const char* err = ::dlerror();
            std::printf("stud: vulkan probe: dlopen(libvulkan.so.1) FAILED: %s\n",
                        err != nullptr ? err : "(no dlerror)");
        } else {
            std::printf("stud: vulkan probe: dlopen(libvulkan.so.1) ok, vkGetInstanceProcAddr=%p\n",
                        ::dlsym(vk, "vkGetInstanceProcAddr"));
        }
        std::fflush(stdout);
    }

    // Real, live-caught ordering bug, fixed here: this block used to run
    // much later (after run_app_bridge_start()/run_preload_bootstrap()),
    // matching neither a real device nor real Sober. A real device sets
    // up Context.getCacheDir()/getFilesDir() before any native call at
    // all; a real Sober capture confirms this too -- its own log shows
    // `RbxStorage::init [INIT]`/`[DONE]` succeeding within ~1ms of
    // `nativeAppBridgeAppStart`, immediately after `SingleSurfaceApp::
    // initializeSingleton`/`instantiate controllers`. Running this block
    // after run_app_bridge_start() instead (the old order) meant
    // AppStart's own internal `initializeSingleton` call ran with no
    // cache directory configured yet -- live-caught, real, and
    // previously undocumented: `RbxStorage::getStorageInterface failed
    // to initialize RbxStorage subsystem` / `Failed to get cache
    // directory: Path does not exist: ""`, immediately followed (same
    // test) by a real SIGSEGV inside `nativeAppBridgeV2InitWithParams`
    // -- consistent with later code assuming a working storage layer
    // that was never actually initialized. Moved here, before every
    // other native call, to match real device/Sober ordering.
    stud::jni_bridge::NativeSettingsConfig native_settings_config;
    native_settings_config.base_url = "https://www.roblox.com";
    native_settings_config.api_base_url = "https://apis.roblox.com";
    if (launch_payload) {
        native_settings_config.base_url = launch_payload->base_url;
        native_settings_config.api_base_url = launch_payload->api_base_url;
        // `session_cookie` is the raw cookie VALUE only (see
        // ui/src/main.cpp, which prepends the name itself for its own
        // real HTTP requests). The engine's own real caller for
        // `nativeSetMultipleCookies` passes a full cookie string --
        // confirmed against the app's own code (the app's own HTTP/cookie layer's `P0()`), which builds it from
        // the real cookie manager and even sanity-checks it with
        // `contains(".ROBLOSECURITY=_")`. Passing the bare value here
        // handed the engine a malformed cookie, so its own auth check
        // failed and the real Lua app routed to the logged-out
        // `Landing` screen instead of `Home` (live-observed via
        // gameActivity_onAppReady). Send the real, correctly-named
        // cookie string.
        if (!launch_payload->session_cookie.empty()) {
            native_settings_config.cookies = ".ROBLOSECURITY=" + launch_payload->session_cookie;
            stud::jni_bridge::seed_current_session_cookie(".ROBLOSECURITY",
                                                           launch_payload->session_cookie);
        }
    }
    std::string files_dir = stud::paths::engine_files_dir();
    link_engine_caches(files_dir);
    stud::jni_bridge::set_native_user_interface_files_dir(files_dir);

    // Real, persistent local storage, loaded before anything can read it.
    // This is where a login made inside the app itself survives a
    // restart: the engine writes the signed-in user and its session
    // material through the LocalStorage platform protocol, and reads them
    // back next launch to decide Home vs the logged-out screen. With
    // several accounts added, it holds one session credential per
    // account -- so it goes through Stud's safe storage (encrypted at
    // rest, key in the system keyring), never a file of its own.
    //
    // This process is sandboxed away from the Secret Service, so both
    // directions go through render-host, which runs stud-ui one-shot.
    // Restore the account switcher's own cookie. Without it the engine
    // comes back knowing only the account whose .ROBLOSECURITY was
    // stored, and every other account the user added is gone -- which is
    // exactly what "the 2nd account's login disappears" was.
    {
        std::vector<char> buffer(64 * 1024);
        uint32_t written = 0;
        uint64_t args[8] = {};
        const uint64_t ok = stud::render_client::connection().call(
            stud::render_host::CallId::LoadSecret, args, kAccountListSecretName,
            static_cast<uint32_t>(std::strlen(kAccountListSecretName)), buffer.data(),
            static_cast<uint32_t>(buffer.size()), &written);
        if (ok != 0 && written > 0) {
            const std::string value(buffer.data(), std::min<size_t>(written, buffer.size()));
            if (!native_settings_config.cookies.empty()) native_settings_config.cookies += "; ";
            native_settings_config.cookies += "rbxas=" + value;
            stud::jni_bridge::seed_current_session_cookie("rbxas", value);
            std::printf("stud: restored the account-list cookie (%zu bytes) -- every signed-in "
                        "account should come back, not just the active one\n",
                        value.size());
            std::fflush(stdout);
        }
    }

    {
        stud::jni_bridge::LocalStoragePlatformStub::set_storage_hooks(
            [](const std::string& document) {
                std::string payload = std::string(kLocalStorageSecretName) + "\n" + document;
                uint64_t args[8] = {};
                stud::render_client::connection().call(
                    stud::render_host::CallId::StoreSecret, args, payload.data(),
                    static_cast<uint32_t>(payload.size()), nullptr, 0, nullptr);
            },
            [] {
                std::vector<char> buffer(64 * 1024);
                uint32_t written = 0;
                uint64_t args[8] = {};
                const uint64_t ok = stud::render_client::connection().call(
                    stud::render_host::CallId::LoadSecret, args, kLocalStorageSecretName,
                    static_cast<uint32_t>(std::strlen(kLocalStorageSecretName)), buffer.data(),
                    static_cast<uint32_t>(buffer.size()), &written);
                if (ok == 0 || written == 0) return std::string();
                return std::string(buffer.data(), std::min<size_t>(written, buffer.size()));
            });
        stud::jni_bridge::LocalStoragePlatformStub::load();

        // One-time move off the plaintext file an earlier build wrote.
        // That file held a real .ROBLOSECURITY in the clear; reading it
        // once into safe storage and removing it is the only way an
        // existing install stops leaving one on disk. Nothing is written
        // back to it, and no keyring entry other than Stud's own
        // safe-storage key is touched.
        std::string data_home;
        if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
            data_home = xdg;
        } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
            data_home = std::string(home) + "/.local/share";
        }
        if (!data_home.empty()) {
            const std::string legacy = data_home + "/stud/localstorage.json";
            std::error_code ec;
            if (std::filesystem::exists(legacy, ec)) {
                std::string text;
                {
                    std::ifstream in(legacy, std::ios::binary);
                    text.assign(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
                }
                stud::jni_bridge::LocalStoragePlatformStub::load_from(text);
                stud::jni_bridge::LocalStoragePlatformStub::save_now();
                std::filesystem::remove(legacy, ec);
                std::printf("stud: moved the local-storage document out of the plaintext file "
                            "and into safe storage (%zu bytes)\n",
                            text.size());
                std::fflush(stdout);
            }
        }
    }
    if (launch_payload && launch_payload->authenticated_user_id != 0) {
        stud::jni_bridge::set_native_user_identity(launch_payload->authenticated_user_id,
                                                    launch_payload->authenticated_username,
                                                    launch_payload->authenticated_display_name);
        std::printf("stud: real authenticated user identity available: id=%lld username=%s\n",
                    launch_payload->authenticated_user_id,
                    launch_payload->authenticated_username.c_str());
        // Seed the real local-storage platform protocol with the same
        // real identity. The Lua app asks *this* protocol who is signed
        // in (IPlatformLocalStorageHandler.getCurrentUser); returning 0
        // is what made it route to the logged-out `Landing` screen.
        stud::jni_bridge::LocalStoragePlatformStub::set_real_current_user(
            static_cast<long long>(launch_payload->authenticated_user_id));
        if (!launch_payload->session_cookie.empty()) {
            // Seeded under the real cookie name the engine itself
            // sanity-checks for (`.ROBLOSECURITY`, per the app's own HTTP/cookie layer). Any
            // other key the app actually asks for shows up in the
            // LocalStorage.* "MISS" logs, by name, so the real key set
            // can be learned from evidence rather than guessed at.
            stud::jni_bridge::LocalStoragePlatformStub::seed_secure_value(
                static_cast<long long>(launch_payload->authenticated_user_id), ".ROBLOSECURITY",
                launch_payload->session_cookie);
        }
    }
    std::string cache_dir = cache_subdir("cache");
    {
        std::error_code ec;
        std::filesystem::create_directories(files_dir, ec);
        if (!ec) native_settings_config.files_directory = files_dir;
        std::filesystem::create_directories(cache_dir, ec);
        if (!ec) {
            native_settings_config.cache_directory = cache_dir;
            native_settings_config.base_internal_directory = files_dir;
            native_settings_config.base_external_directory = cache_dir;
        }
        std::string external_dir = files_dir + "/external";
        std::filesystem::create_directories(external_dir, ec);
        if (!ec) native_settings_config.external_directory = external_dir;

        native_settings_config.roblox_version = real_app_version;

        stud::jni_bridge::set_activity_context_directories(files_dir, cache_dir);

        // Deliberately NOT setting the engine's own "FramerateCap" here.
        // A working Sober install has 144 in that field of
        // GlobalBasicSettings_13.xml, which made it look load-bearing --
        // but tested directly, with the file set to 30 the engine still
        // ran at 57fps. It does not read that value on this path.

        // Real, live-caught, load-bearing fix: the engine's own internal
        // HTTP client looks for a CA bundle at `<filesDir>/exe/cacert.pem`
        // (confirmed via a real `DFLog::HttpTraceError` capture -- the
        // exact path it logged, not a guess) -- NOT relative to cwd, and
        // NOT the `assets/ssl/cacert.pem` path Stud already extracts from
        // the APK. Without this file present at exactly this path, EVERY
        // real outbound HTTPS request the engine makes fails at the TLS
        // trust-anchor stage (`error:12`/`HttpError: Unknown url`,
        // confirmed live for mobile-client-version, browser-tracker-api,
        // ecsv2, experience-signals-ingest, ephemeralcounters -- a real,
        // total networking blackout, not one endpoint) -- directly
        // implicating this as a root cause (or a major contributor to)
        // gap #1's "no real game join" mystery, since whatever internal
        // call resolves a join ticket is exactly this kind of HTTPS
        // request. Sourced from the same real, already-extracted APK
        // asset (`assets/ssl/cacert.pem`) `run_preload_bootstrap()`'s own
        // `asset_dir` already points at -- just copied to the second real
        // path the engine separately expects it at.
        std::string exe_dir = files_dir + "/exe";
        std::filesystem::create_directories(exe_dir, ec);
        if (!ec) {
            std::filesystem::copy_file(asset_dir + "/ssl/cacert.pem", exe_dir + "/cacert.pem",
                                        std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                std::fprintf(stderr, "stud: warning: failed to provision %s/cacert.pem: %s\n",
                             exe_dir.c_str(), ec.message().c_str());
            }
        }
    }
    // Real ordering fix (live-caught via the engine's own FLog, now
    // visible through Stud's logd sink): the engine logs
    // `[FLog::LocalStorageHandler] Not available on the current
    // platform.` at ~0.16s -- BEFORE this registration used to run
    // (it sat much later, after asset-manager setup). A platform impl
    // registered after the engine has already asked whether the
    // platform supports local storage is too late to matter. On a real
    // device the app's own DEX performs every `setPlatformImpl` during
    // startup, before engine subsystems query availability, so this
    // matches real device order rather than inventing one.
    // The system-theme protocol, registered here for the same reason as
    // the platform implementations below: the Lua app settles on a theme
    // while it starts, and anything registered after that is too late to
    // decide what the app opens in. Measured -- with this block where it
    // used to be (next to the web-view protocol, far down this function)
    // the app had already reached Home ~650 lines of log earlier, so the
    // first thing the user saw was light on a dark desktop.
    //
    // The app reads the value back through
    // SystemThemeProtocol.getSystemTheme(); publishing once is what makes
    // it ask at all rather than wait for a change that may never come.
    stud::jni_bridge::run_system_theme_bootstrap(
        jvm, lib, [](const std::string& theme_name) {
            std::printf("stud: web view will open pages in %s\n", theme_name.c_str());
            std::fflush(stdout);
        });
    stud::jni_bridge::publish_system_theme_updated(jvm, lib);

    try {
        auto protocol_result = stud::jni_bridge::run_protocol_platform_stubs_bootstrap(jvm, lib);
        std::printf(
            "stud: run_protocol_platform_stubs_bootstrap() complete: "
            "app_age_signals=%d/%d bug_reporter=%d/%d pin_shortcut=%d/%d "
            "device_display=%d/%d local_storage=%d/%d design_foundations=%d/%d "
            "(called/trapped_abort)\n",
            protocol_result.app_age_signals_set_platform_impl_called,
            protocol_result.app_age_signals_set_platform_impl_trapped_abort,
            protocol_result.bug_reporter_set_platform_impl_called,
            protocol_result.bug_reporter_set_platform_impl_trapped_abort,
            protocol_result.pin_shortcut_set_platform_impl_called,
            protocol_result.pin_shortcut_set_platform_impl_trapped_abort,
            protocol_result.device_display_set_platform_impl_called,
            protocol_result.device_display_set_platform_impl_trapped_abort,
            protocol_result.local_storage_set_platform_impl_called,
            protocol_result.local_storage_set_platform_impl_trapped_abort,
            protocol_result.design_foundations_set_platform_impl_called,
            protocol_result.design_foundations_set_platform_impl_trapped_abort);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stud: run_protocol_platform_stubs_bootstrap() failed: %s\n",
                     e.what());
    }

    try {
        auto settings_result =
            stud::jni_bridge::run_native_settings_bootstrap(jvm, lib, native_settings_config);
        std::printf("stud: run_native_settings_bootstrap() complete: set_base_url=%d "
                    "set_multiple_cookies=%d set_files_directory=%d set_cache_directory=%d\n",
                    settings_result.set_base_url_called, settings_result.set_multiple_cookies_called,
                    settings_result.set_files_directory_called,
                    settings_result.set_cache_directory_called);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stud: run_native_settings_bootstrap() failed: %s\n", e.what());
        return 1;
    }

    // Real device order (see client_settings_bridge.h's own doc
    // comment): nativeInitClientSettings runs before
    // nativeAppBridgeAppStart. Content was pre-fetched by the UI process
    // before this process even started (no launch_payload -- e.g. a
    // manual/diagnostic invocation without --ipc-connect -- just skips
    // this, same degrade-gracefully treatment every other launch_payload-
    // derived value already gets in this file).
    if (launch_payload) {
        // Merge the hand-edited flag overrides into the ClientSettings
        // payload the engine is about to consume.
        //
        // nativePreloadFlagOverrides has never actually changed any
        // runtime value in this project's history (the engineering notes' own
        // "third independent data point"): it only stashes the JSON for a
        // lazy getFlags() consumer, and anything read before that consumer
        // fires keeps its compiled-in default. Re-confirmed live today --
        // `loaded 4 FFlag override(s)` and `preload_flag_overrides=1`, and
        // still zero observable effect.
        //
        // ClientSettings is the path the engine genuinely does apply, so
        // the overrides ride in there instead. This does NOT change the
        // locked decision in flag_overrides.h: overrides still come only
        // from the raw, hand-edited file, never from a Settings toggle --
        // only how they reach the engine changes.
        std::string client_settings_body = launch_payload->client_settings_body;

        const std::map<std::string, bool> renderer_flags =
            renderer_flags_for_mode(graphics_mode);
        if (!client_settings_body.empty()) {
            try {
                auto doc = nlohmann::json::parse(client_settings_body);
                auto& app = doc["applicationSettings"];
                if (!app.is_object()) app = nlohmann::json::object();
                // Stud's own defaults first, so a hand-edited override
                // of the same flag below replaces any of them.
                for (const auto& [name, value] : stud_default_flags()) {
                    app[client_settings_key(name, value)] = value;
                }
                for (const auto& [name, value] : renderer_flags) {
                    const std::string text = value ? "True" : "False";
                    app[client_settings_key(name, text)] = text;
                }
                if (!renderer_flags.empty()) {
                    std::printf("stud: render path \"%s\": asked the engine for %zu renderer "
                                "flag(s)\n",
                                graphics_mode.c_str(), renderer_flags.size());
                }
                auto wire = nlohmann::json::parse(overrides.to_wire_format());
                size_t merged = 0;
                for (auto it = wire.begin(); it != wire.end(); ++it) {
                    // Real responses carry every value as a string, and
                    // booleans are capitalised -- checked against a live
                    // clientsettingscdn response, which spells them
                    // exactly "True"/"False". Stud used to write "true"/
                    // "false", which is a different string to any parser
                    // that compares them literally.
                    const std::string text = it.value().is_string()
                                                 ? it.value().get<std::string>()
                                                 : it.value().dump();
                    app[client_settings_key(it.key(), text)] = text;
                    ++merged;
                }
                client_settings_body = doc.dump();
                std::printf("stud: merged %zu FFlag override(s) into ClientSettings\n", merged);
                std::fflush(stdout);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "stud: could not merge FFlag overrides into ClientSettings: %s\n",
                             e.what());
                client_settings_body = launch_payload->client_settings_body;
            }
        }
        try {
            auto client_settings_result = stud::jni_bridge::run_client_settings_bridge(
                jvm, lib, client_settings_body,
                launch_payload->client_settings_http_status);
            std::printf(
                "stud: run_client_settings_bridge() complete: http_fetch_succeeded=%d "
                "init_client_settings_called=%d init_client_settings_result=%d post_init_called=%d\n",
                client_settings_result.http_fetch_succeeded,
                client_settings_result.init_client_settings_called,
                client_settings_result.init_client_settings_result,
                client_settings_result.post_init_called);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "stud: run_client_settings_bridge() failed: %s\n", e.what());
        }
        // Ask the engine what it actually believes each overridden flag
        // is. "The call did not trap" has never been evidence that an
        // override took: this reads the value back through the engine's
        // own exported getter, which is the only honest check.
        stud::jni_bridge::report_flag_override_state(jvm, lib, overrides, "after ClientSettings");
    }
    // Real, live-caught, structural fix (the engineering notes): a real
    // device's Application dispatches a full Activity-lifecycle callback
    // sequence (19 real native methods on JNIActivityLifecycleCallbacks,
    // confirmed against the app's own code) as real Activities are created/shown/torn
    // down -- Stud had never called any of them, since it has no
    // ART/real Activity to generate them. Real, live evidence a worker
    // thread genuinely blocks forever (an absl::Mutex::Await with an
    // infinite timeout, found by live inspection) waiting on
    // state only these callbacks update.
    //
    // Real, corrected order: RobloxApplication.onCreate() (confirmed against the app's own code
    // real Application subclass) is the actual first real callback of
    // the whole process, before any Activity -- it calls
    // JNIAAssetManagerSetup.a() (hands libroblox.so a real AssetManager
    // reference) before registering anything else. Dispatched first here
    // to match.
    try {
        bool asset_manager_ok = stud::jni_bridge::run_asset_manager_setup_bridge(jvm, lib);
        std::printf("stud: run_asset_manager_setup_bridge() complete: ok=%d\n", asset_manager_ok);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stud: run_asset_manager_setup_bridge() failed: %s\n", e.what());
    }
    // Real, confirmed against the app's own code (LocalStorageManager.a(context)): real storage
    // subsystem bring-up, separate from the AssetManager-only call above.
    try {
        bool storage_ok = stud::jni_bridge::run_local_storage_manager_bootstrap(jvm, lib, files_dir,
                                                                                  cache_dir);
        std::printf("stud: run_local_storage_manager_bootstrap() complete: ok=%d\n", storage_ok);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stud: run_local_storage_manager_bootstrap() failed: %s\n", e.what());
    }
    // Real Djinni protocol-interface registration (the engineering notes'
    // "Djinni protocol bindings" scoping entry): real DEX code would
    // call each protocol's own `<X>Core.setPlatformImpl(...)` once at
    // real app startup; Stud never runs DEX, so this makes that same
    // real call itself. Not boot-critical (peripheral features, not on
    // the render/join critical path) -- placed here, non-blocking,
    // rather than gating anything on it.
    // Real WebView cookie-jar sync registration (the engineering notes'
    // CookieProtocol entry): same "real DEX would call this once at
    // startup, Stud calls it itself instead" shape as the Djinni
    // protocols above. Not boot-critical -- non-blocking.
    try {
        auto cookie_result = stud::jni_bridge::run_cookie_protocol_bootstrap(jvm, lib);
        std::printf("stud: run_cookie_protocol_bootstrap() complete: called=%d trapped_abort=%d\n",
                    cookie_result.called, cookie_result.trapped_abort);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stud: run_cookie_protocol_bootstrap() failed: %s\n", e.what());
    }

    // The engine asks what user agent its web views send, through
    // NativeGLJavaInterface.getWebViewUserAgent(), and expects the
    // answer back asynchronously on its own setter. Installed here, well
    // before anything can ask: the engine reports this string to Roblox
    // when it creates a login challenge, and the page that answers the
    // challenge runs in Stud's own viewer -- so both sides have to name
    // the same client. Saying nothing left the challenge attached to an
    // empty one, and the page failed with "something went wrong" while
    // the viewer stayed open waiting for a completion that never came.
    {
        const std::string webview_agent =
            stud::webview::user_agent("RobloxApp/" + real_app_version);
        std::printf("stud: webview user agent: %s\n", webview_agent.c_str());
        std::fflush(stdout);
        stud::jni_bridge::set_webview_user_agent_reporter(
            [&jvm, &lib, agent = webview_agent]() {
                stud::jni_bridge::report_webview_user_agent(jvm, lib, agent);
            });
    }
    //
    // Real, corrected order and activity names (this project's own real
    // logcat, `~/Stud/roblox_logcat2.txt`'s `InitHelper` tag output,
    // analyzed properly this session -- an earlier attempt guessed a
    // wrong activity name, "MainGameActivity", which doesn't exist, and
    // dispatched it in the wrong position): `InitHelper` logs
    // `setView=[ActivitySplash]` at real app start, then
    // `unsetView=[ActivitySplash]` followed by `setView=[ActivityNativeMain]`
    // -- and only *after* that does `nativeAppBridgeAppStart` ever fire.
    // `ActivityNativeMain` (confirmed: a real class with a real
    // SurfaceView + NativeGLInterface/NativeGLJavaInterface fields) is
    // the real activity the app bridge actually runs under, not the
    // splash screen. Matches Android's own standard, documented
    // single-task activity-switch order: outgoing activity onPause, new
    // activity onCreate/onStart/onResume, outgoing activity onStop.
    try {
        auto splash_result =
            stud::jni_bridge::run_activity_lifecycle_bridge(jvm, lib, "ActivitySplash");
        std::printf(
            "stud: run_activity_lifecycle_bridge(ActivitySplash) complete: pre_created=%d "
            "created=%d post_created=%d pre_started=%d started=%d post_started=%d "
            "pre_resumed=%d resumed=%d post_resumed=%d any_trapped_abort=%d\n",
            splash_result.pre_created_called, splash_result.created_called,
            splash_result.post_created_called, splash_result.pre_started_called,
            splash_result.started_called, splash_result.post_started_called,
            splash_result.pre_resumed_called, splash_result.resumed_called,
            splash_result.post_resumed_called, splash_result.any_trapped_abort);

        // Real position: `ActivitySplash.onCreate()` calls this right
        // after its own `super.onCreate()` (which is what drives the
        // real lifecycle callbacks dispatched just above), before
        // anything else in that method -- unconditional on a real
        // device, no flag gate. See activity_lifecycle_bridge.h.
        bool app_shell_reporter_ok = stud::jni_bridge::run_app_shell_reporter_init(jvm, lib);
        std::printf("stud: run_app_shell_reporter_init() complete: ok=%d\n", app_shell_reporter_ok);

        bool set_active_ok = stud::jni_bridge::run_app_lifecycle_native_adapter_set_active(jvm, lib);
        std::printf("stud: run_app_lifecycle_native_adapter_set_active() complete: ok=%d\n",
                    set_active_ok);

        auto splash_pause_stop_result =
            stud::jni_bridge::run_activity_pause_stop_bridge(jvm, lib, "ActivitySplash");
        std::printf(
            "stud: run_activity_pause_stop_bridge(ActivitySplash) complete: pre_paused=%d "
            "paused=%d post_paused=%d pre_stopped=%d stopped=%d post_stopped=%d "
            "any_trapped_abort=%d\n",
            splash_pause_stop_result.pre_paused_called, splash_pause_stop_result.paused_called,
            splash_pause_stop_result.post_paused_called,
            splash_pause_stop_result.pre_stopped_called, splash_pause_stop_result.stopped_called,
            splash_pause_stop_result.post_stopped_called,
            splash_pause_stop_result.any_trapped_abort);

        auto native_main_result =
            stud::jni_bridge::run_activity_lifecycle_bridge(jvm, lib, "ActivityNativeMain");
        std::printf(
            "stud: run_activity_lifecycle_bridge(ActivityNativeMain) complete: pre_created=%d "
            "created=%d post_created=%d pre_started=%d started=%d post_started=%d "
            "pre_resumed=%d resumed=%d post_resumed=%d any_trapped_abort=%d\n",
            native_main_result.pre_created_called, native_main_result.created_called,
            native_main_result.post_created_called, native_main_result.pre_started_called,
            native_main_result.started_called, native_main_result.post_started_called,
            native_main_result.pre_resumed_called, native_main_result.resumed_called,
            native_main_result.post_resumed_called, native_main_result.any_trapped_abort);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stud: activity lifecycle dispatch failed: %s\n", e.what());
    }

    try {
        // Real `InitHelper.O()`/`startAppBridge` order: setIsFirstInstall()
        // immediately before the real nativeAppBridgeAppStart call below.
        // Real first-run semantics replicated honestly via a real marker
        // file under Stud's own real files directory -- matching the real
        // `InitHelper.z(Context)`'s own SharedPreferences flag (default
        // true, flipped false once read), not a hardcoded value.
        std::string first_run_marker = files_dir + "/.stud_first_run_done";
        bool is_first_install = !std::filesystem::exists(first_run_marker);
        if (is_first_install) {
            std::error_code marker_ec;
            std::filesystem::create_directories(files_dir, marker_ec);
            std::ofstream(first_run_marker).put('\n');
        }
        bool first_install_ok =
            stud::jni_bridge::run_set_is_first_install(jvm, lib, is_first_install);
        std::printf("stud: run_set_is_first_install(%d) complete: ok=%d\n",
                    static_cast<int>(is_first_install), first_install_ok);

        std::string base_url = launch_payload ? launch_payload->base_url : "";
    // Tell the engine what the display can actually do. Read from the
    // compositor at runtime, so this follows whatever panel is attached
    // rather than any number written down here.
    {
        uint64_t rate_args[8] = {};
        const uint64_t current_mhz = stud::render_client::connection().call(
            stud::render_host::CallId::GetDisplayRefreshRate, rate_args, nullptr, 0, nullptr, 0,
            nullptr);
        std::vector<uint32_t> supported_mhz(16, 0);
        uint32_t supported_len = 0;
        stud::render_client::connection().call(
            stud::render_host::CallId::GetSupportedRefreshRates, rate_args, nullptr, 0,
            supported_mhz.data(), static_cast<uint32_t>(supported_mhz.size() * sizeof(uint32_t)),
            &supported_len);
        std::vector<float> supported_hz;
        for (uint32_t i = 0; i < supported_len / sizeof(uint32_t); ++i) {
            if (supported_mhz[i] > 0) supported_hz.push_back(supported_mhz[i] / 1000.0f);
        }
        float current_hz = static_cast<float>(current_mhz) / 1000.0f;
        // STUD_FORCE_REFRESH_HZ=<n>: report a rate the display does not
        // have. Purely a test lever -- if frames follow it, the engine
        // is pacing to what Stud reports; if they do not, it is pacing
        // to something else and reporting is not the lever.
        if (const char* forced = std::getenv("STUD_FORCE_REFRESH_HZ")) {
            const float value = std::strtof(forced, nullptr);
            if (value > 0.0f) current_hz = value;
        }
        if (current_hz > 0.0f) {
            const bool ok = stud::jni_bridge::run_pass_display_refresh_rates(jvm, lib, current_hz,
                                                                            supported_hz);
            std::printf("stud: display refresh: %.3f Hz current, %zu supported -> %s\n",
                        static_cast<double>(current_hz), supported_hz.size(),
                        ok ? "reported" : "not reported");
        } else {
            std::printf("stud: display refresh: compositor reported none, leaving the engine's "
                        "own default\n");
        }
        std::fflush(stdout);
    }
        auto app_bridge_result = stud::jni_bridge::run_app_bridge_start(jvm, lib, base_url);
        std::printf("stud: run_app_bridge_start() complete: app_start_called=%d trapped_abort=%d\n",
                    app_bridge_result.app_start_called, app_bridge_result.app_start_trapped_abort);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stud: run_app_bridge_start() failed: %s\n", e.what());
        return 1;
    }
    // Real fix for the onSurfaceCreatedNative hang (proven under the old
    // architecture, still needed): native_app_glue's own android_main()
    // returns without looping, so its own looper is never polled again
    // by anyone unless something else drains it -- a dedicated
    // background thread, started before drive_game_activity_lifecycle()
    // blocks on it below, is required.
    // The 20ms interval here was measured and is NOT the frame-rate cap,
    // though it looks like one: this thread drains the looper the
    // engine's render loop waits on, and it was the only thread in the
    // process sleeping on a timer (a live syscall trace: nanosleep(20ms) in a tight
    // loop, and 20ms is exactly the ~50fps being observed). Tested by
    // polling continuously instead -- the frame rate did not move and
    // the process went from 36% to 64% CPU, so the engine is not waiting
    // on this. Left at 20ms; do not "fix" it again without a measurement
    // showing frames actually gated on it.
    std::thread orphaned_looper_poller([]() {
        while (g_should_keep_running.load(std::memory_order_relaxed)) {
            stud::android_glue::poll_orphaned_loopers_once();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    orphaned_looper_poller.detach();

    try {
        auto preload_result = stud::jni_bridge::run_preload_bootstrap(jvm, lib, asset_dir, overrides);
        std::printf(
            "stud: run_preload_bootstrap() complete: set_asset_path=%d preload_flag_overrides=%d\n",
            preload_result.set_asset_path_called, preload_result.preload_flag_overrides_called);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stud: run_preload_bootstrap() failed: %s\n", e.what());
        return 1;
    }
    // Real ASMA order (the app's own app-shell manager, the real app-shell manager class):
    // E(context) = nativeGameGlobalInit/nativeUpdateAdapterInit, then
    // j(d)/initializeDataModel = nativeAppBridgeV2InitWithParams, and
    // only THEN F(surface) = StartAppWithParams. Stud used to run the
    // whole V2 sequence after drive_game_activity_lifecycle() had
    // already handed the engine a surface via onSurfaceCreatedNative --
    // live-caught in the engine's own FLog (logd sink): the Lua app
    // reached `setStage: (stage:LuaApp)` + `userDidLogin` at 2.034s, and
    // `nativeAppBridgeV2Init` only arrived at 2.115s, resetting
    // `setStage: (stage:Native)` and destroying the live
    // SurfaceController/RenderView underneath it. Build the V2 params
    // and run the early-init half BEFORE the surface is handed over.
    auto v2_platform_params = stud::jni_bridge::build_desktop_platform_params_with_lua_flags(
        asset_dir, engine_dpi_scale, real_viewport_mm_w, real_viewport_mm_h);
    auto v2_device_params = stud::jni_bridge::build_desktop_device_params(
        "34", "Stud", real_app_version, "1920x1080", 1920, 1080, 16384);
    // setDeviceStaticParams is NOT called here any more -- it has to run
    // before dlopen(), see the call site up by register_game_activity_stubs().
    try {
        bool base_url_ok = stud::jni_bridge::run_base_url_protocol_init(jvm, lib);
        std::printf("stud: run_base_url_protocol_init() complete: ok=%d\n", base_url_ok);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stud: run_base_url_protocol_init() failed: %s\n", e.what());
    }
    try {
        bool web_login_ok = stud::jni_bridge::run_web_login_protocol_init(jvm, lib);
        std::printf("stud: run_web_login_protocol_init() complete: ok=%d\n", web_login_ok);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stud: run_web_login_protocol_init() failed: %s\n", e.what());
    }
    auto v2_init_params = stud::jni_bridge::build_desktop_init_params(
        v2_platform_params, v2_device_params, "https://www.roblox.com", real_user_agent);

    stud::jni_bridge::EngineV2BridgeResult v2_early;
    const bool v2_enabled = std::getenv("STUD_ENABLE_V2") == nullptr ||
                            std::string_view(std::getenv("STUD_ENABLE_V2")) != "0";
    if (v2_enabled) {
        try {
            v2_early = stud::jni_bridge::run_engine_v2_early_init(jvm, lib, v2_init_params);
            std::printf("stud: run_engine_v2_early_init() complete: init=%d/abort=%d/still_running=%d\n",
                        v2_early.init_with_params_called, v2_early.init_with_params_trapped_abort,
                        v2_early.init_with_params_still_running);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "stud: run_engine_v2_early_init() failed: %s\n", e.what());
        }
    }

    // Real screen metrics, asked of the process that actually owns the window
    // (confirmed good). set_real_display_metrics() existed but was never
    // called, so the engine saw a built-in 800x600 default while render-host's
    // window was a different size again; the app's own input handler divides
    // every coordinate by this density, and the engine sizes its viewport from
    // these metrics, so all three have to agree.
    int32_t real_window_width = 1280;
    int32_t real_window_height = 720;
    {
        uint64_t size_args[8] = {};
        uint64_t packed = stud::render_client::connection().call(
            stud::render_host::CallId::GetWindowSize, size_args, nullptr, 0, nullptr, 0, nullptr);
        if (packed != 0) {
            real_window_width = static_cast<int32_t>(packed >> 32);
            real_window_height = static_cast<int32_t>(packed & 0xffffffffu);
        }
    }
    // Size only -- the density was decided once during bring-up and must
    // not move. Re-seeding it here is what made the DPI change mid-session.
    stud::jni_bridge::set_real_display_metrics(real_window_width, real_window_height, layout_density);

    // Real display geometry, from the same process for the same reason.
    // This is the OUTPUT, not the window: dots-per-inch is a property of
    // the panel, so a window occupying part of it has the same pixel
    // pitch. DisplayMetrics.xdpi/ydpi used to be synthesised as
    // `160 * density`, which described no real hardware and made
    // DeviceUtils.getScreenPhysicalSizeInMillimeters() -- which the
    // engine's own getViewportDisplaySize() calls -- an invented number.
    {
        uint64_t geom_args[8] = {};
        uint64_t packed = stud::render_client::connection().call(
            stud::render_host::CallId::GetDisplayOutputGeometry, geom_args, nullptr, 0, nullptr, 0,
            nullptr);
        stud::jni_bridge::set_real_display_output_geometry(
            static_cast<int>((packed >> 48) & 0xffff), static_cast<int>((packed >> 32) & 0xffff),
            static_cast<int>((packed >> 16) & 0xffff), static_cast<int>(packed & 0xffff));
    }

    // GameActivity_initializeNativeCode starts libroblox's own
    // android_main, and that is a whole second application:
    //   [FLog::NativeMain]   [android_main] Create a new NativeEngine:
    //   [FLog::NativeEngine] initializing.
    //   [FLog::NativeDM]     initialize: state:1 / bootstrapTheApp_ /
    //                        initEngine_ / startLuaApp_ /
    //                        listenForExperienceLaunchRequest_
    // -- Roblox's newer standalone GameActivity path, with its own
    // DataModel bindings and its own experience-launch listener, running
    // alongside the legacy app-bridge sequence Stud also drives.
    //
    // A real Sober session that joins a game successfully has ZERO
    // FLog::NativeDM and ZERO FLog::NativeEngine lines: it uses the
    // legacy path only. STUD_SKIP_GAME_ACTIVITY=1 drops Stud to that
    // same single path, to test whether running both is what leaves a
    // launch stuck at `stepDataModelJob: No DM yet` -- the surface still
    // reaches the engine through UpdateSurfaceApp, and android-glue's
    // ANativeWindow_fromSurface returns the real window regardless of
    // which jobject it is handed.
    const char* skip_ga_env = std::getenv("STUD_SKIP_GAME_ACTIVITY");
    const bool skip_game_activity = skip_ga_env != nullptr && std::string_view(skip_ga_env) == "1";

    bool init_params_delivered = false;
    stud::jni_bridge::GameActivityLifecycleResult lifecycle{};
    if (skip_game_activity) {
        std::printf("stud: STUD_SKIP_GAME_ACTIVITY=1 -- not starting the engine's own "
                    "android_main/NativeEngine; legacy app-bridge path only\n");
        std::fflush(stdout);
        init_params_delivered = true;
        // Everything downstream keys off lifecycle.surface, and there is
        // exactly one real window either way -- android-glue's
        // ANativeWindow_fromSurface ignores the jobject and returns it.
        lifecycle.surface = std::make_shared<stud::jni_bridge::SurfaceStub>();
        try {
            auto init_result = stud::jni_bridge::run_init_params_bootstrap(jvm, lib, init_params);
            std::printf("stud: run_init_params_bootstrap() complete (no GameActivity): "
                        "app_bridge_set_init_params=%d\n",
                        init_result.app_bridge_set_init_params_called);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "stud: run_init_params_bootstrap() failed: %s\n", e.what());
        }
    } else
    lifecycle = stud::jni_bridge::drive_game_activity_lifecycle(
        jvm, lib, cache_subdir("runtime-scratch"), cache_subdir("runtime-scratch"),
        cache_subdir("runtime-scratch"), real_window_width, real_window_height, [&]() {
            if (init_params_delivered) return;
            init_params_delivered = true;
            try {
                auto init_result = stud::jni_bridge::run_init_params_bootstrap(jvm, lib, init_params);
                std::printf(
                    "stud: run_init_params_bootstrap() complete (via bootstrapTheApp callback): "
                    "app_bridge_set_init_params=%d\n",
                    init_result.app_bridge_set_init_params_called);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "stud: run_init_params_bootstrap() failed: %s\n", e.what());
            }
        });
    if (lifecycle.initialize_native_code_found && !init_params_delivered) {
        std::fprintf(stderr,
                      "stud: bootstrapTheApp() callback was never invoked by the engine -- "
                      "delivering InitParams directly as a fallback\n");
        try {
            stud::jni_bridge::run_init_params_bootstrap(jvm, lib, init_params);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "stud: fallback run_init_params_bootstrap() failed: %s\n", e.what());
        }
    }

    try {
        auto flags_result = stud::jni_bridge::run_native_flags_bridge(jvm, lib);
        std::printf("stud: run_native_flags_bridge() complete: called=%d trapped_abort=%d\n",
                    flags_result.called, flags_result.trapped_abort);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stud: run_native_flags_bridge() failed: %s\n", e.what());
    }

    if (lifecycle.initialize_native_code_found) {
        std::printf("stud: GameActivity lifecycle: initializeNativeCode=%s onStart=%s onResume=%s "
                    "onWindowFocusChanged=%s onSurfaceCreated=%s onSurfaceChanged=%s\n",
                    lifecycle.initialize_native_code_trapped_abort ? "trapped" : "ok",
                    lifecycle.on_start_trapped_abort ? "trapped" : lifecycle.on_start_called ? "ok" : "not-found",
                    lifecycle.on_resume_trapped_abort ? "trapped" : lifecycle.on_resume_called ? "ok" : "not-found",
                    lifecycle.on_window_focus_changed_trapped_abort ? "trapped" : lifecycle.on_window_focus_changed_called ? "ok" : "not-found",
                    lifecycle.on_surface_created_trapped_abort ? "trapped" : lifecycle.on_surface_created_called ? "ok" : "not-found",
                    lifecycle.on_surface_changed_trapped_abort ? "trapped" : lifecycle.on_surface_changed_called ? "ok" : "not-found");
    } else {
        std::printf("stud: GameActivity_initializeNativeCode not found -- skipping lifecycle\n");
    }

    // V2 app-bridge sequence -- the real mechanism that boots Roblox's
    // own engine/UI (nativeAppBridgeV2StartAppWithParams triggers
    // startLuaApp/setStage(LuaApp), confirmed via real FLog output).
    // Defaults ON now: a real, evidence-based reversal of the old
    // "default off, matching the old architecture's proven-safest
    // default" stance (that reasoning predates two real fixes made this
    // session -- the ThreadContext-abort-leak patch and every one of
    // these four calls running through run_bounded_v2_call()'s
    // background-thread + bounded-wait machinery, see
    // engine_v2_bridge.cpp -- and is stale). Proven crash-free across
    // several full, real end-to-end runs this session (real
    // stud-render-host, real fetched ClientSettings, a real stored
    // session cookie). Without this, Process B never does anything but
    // sit in a blank, cleared render loop forever -- confirmed live,
    // not theoretical (a real user report: "stud is not responding,
    // and its only a black window" was exactly this). STUD_ENABLE_V2=0
    // to disable, for isolating a regression.
    // Real "ASMA.start" step (see activity_lifecycle_bridge.h): put the
    // engine's own internal task scheduler into foreground mode BEFORE
    // the V2 sequence below posts any real work to it.
    // Real ActivityNativeMain.onStart() pair, and a hard precondition for
    // any join: the engine will not send HTTP while it thinks the app is
    // suspended. See run_app_foreground_bridge()'s own comment.
    try {
        bool fg_ok = stud::jni_bridge::run_app_foreground_bridge(jvm, lib);
        std::printf("stud: run_app_foreground_bridge() complete: ok=%d\n", fg_ok);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stud: run_app_foreground_bridge() failed: %s\n", e.what());
    }

    try {
        bool scheduler_ok = stud::jni_bridge::run_set_task_scheduler_foreground(jvm, lib);
        std::printf("stud: run_set_task_scheduler_foreground() complete: ok=%d\n", scheduler_ok);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stud: run_set_task_scheduler_foreground() failed: %s\n", e.what());
    }

    if (lifecycle.surface && v2_enabled) {
        // Real deep-link join fields (see start_game_params.h's own
        // DeepLinkJoinInfo doc comment) -- zero/empty if this launch
        // wasn't a real roblox-player:// deep link (e.g. a bare
        // already-logged-in launch), same honest-degradation pattern as
        // every other launch_payload-derived value in this function.
        stud::jni_bridge::DeepLinkJoinInfo deep_link;
        if (launch_payload) {
            deep_link.place_id = launch_payload->deep_link_place_id;
            deep_link.join_attempt_id = launch_payload->deep_link_join_attempt_id;
            deep_link.referred_by_player_id = launch_payload->deep_link_referred_by_player_id;
            deep_link.join_attempt_origin = launch_payload->deep_link_join_attempt_origin;
            deep_link.game_instance_id = launch_payload->deep_link_game_instance_id;
            deep_link.launch_data = launch_payload->game_info;
        }
        try {
            // Again, once the engine has loaded its own settings: a flag
            // can be right here and replaced a moment later.
            stud::jni_bridge::report_flag_override_state(jvm, lib, overrides, "after engine boot");
            auto v2_result = stud::jni_bridge::run_engine_v2_sequence(
                jvm, lib, v2_platform_params, v2_device_params, v2_init_params, lifecycle.surface,
                deep_link, /*skip_early_init=*/true);
            std::printf(
                "stud: run_engine_v2_sequence() complete: "
                "lua_app_dm=%d/abort=%d update_surface_app=%d/abort=%d "
                "update_surface_game=%d/abort=%d "
                "start_app=%d/abort=%d/still_running=%d "
                "init=%d/abort=%d/still_running=%d "
                "resume_game=%d/abort=%d/still_running=%d "
                "start_game=%d/abort=%d/still_running=%d result=%d\n",
                v2_result.lua_app_dm_called, v2_result.lua_app_dm_trapped_abort,
                v2_result.update_surface_app_called, v2_result.update_surface_app_trapped_abort,
                v2_result.update_surface_game_called, v2_result.update_surface_game_trapped_abort,
                v2_result.start_app_with_params_called, v2_result.start_app_with_params_trapped_abort,
                v2_result.start_app_with_params_still_running, v2_result.init_with_params_called,
                v2_result.init_with_params_trapped_abort, v2_result.init_with_params_still_running,
                v2_result.resume_game_called, v2_result.resume_game_trapped_abort,
                v2_result.resume_game_still_running, v2_result.start_game_called,
                v2_result.start_game_trapped_abort, v2_result.start_game_still_running,
                v2_result.start_game_result);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "stud: run_engine_v2_sequence() failed: %s\n", e.what());
        }
    }

    if (lifecycle.surface) {
        // Real, live-caught race (the engineering notes, "keep going" entry
        // after the getNativeHelper fix): render-host's own real window
        // surface can only ever be held by ONE live EGL context at a
        // time. Confirmed live: when Roblox's own engine (via its own,
        // separate, async "app thread") creates its real surface FIRST,
        // this fallback's own later attempt correctly, harmlessly fails
        // (EGL_BAD_ALLOC, already-documented, expected) -- but the
        // reverse is NOT harmless: if THIS call reaches the surface
        // first, Roblox's own engine's later attempt fails instead, and
        // since only one surface can ever exist, Roblox's own engine is
        // then PERMANENTLY blocked from ever rendering for the rest of
        // that process's life, not just delayed. This fallback exists to
        // prove the render pipeline works when Roblox's engine could
        // never reach real render setup at all -- now that it's
        // confirmed capable of that (this same session), racing ahead of
        // it here is actively harmful, not just redundant. A real,
        // bounded delay -- giving Roblox's own async render thread a
        // genuine head start -- is a simple, safe mitigation: this
        // fallback still eventually runs if Roblox's engine truly never
        // gets there, but no longer wins a race it has no business
        // winning.
        // Removed, deliberately: Stud used to create its OWN fallback EGL
        // window surface here (behind a 15s delay, to avoid winning a race
        // it had no business winning). Roblox's own engine now reliably
        // creates and owns the real window surface itself, every run --
        // there is only ever one, so this fallback could at best fail with
        // EGL_BAD_ALLOC and at worst permanently lock the engine out of
        // rendering. Its original purpose (proving the pipeline works when
        // the engine could not reach render setup at all) is long since
        // served. The delay also sat directly in front of input bring-up,
        // which is real, user-visible latency for no benefit.

        // A separate, older, single-call path to the same
        // nativeAppBridgeV2StartAppWithParams the V2 sequence above
        // already calls as its first of four (now default-on -- see
        // that block's own doc comment). Stays opt-in and default OFF
        // deliberately: with STUD_ENABLE_V2 now on by default, enabling
        // this too would invoke StartAppWithParams a second, redundant
        // time on its own separate background thread, racing the V2
        // sequence's own call into Roblox's internal LuaApp-shell init --
        // untested and not worth the risk when the V2 sequence already
        // covers this. Only meaningful with STUD_ENABLE_V2=0.
        if (std::getenv("STUD_ENABLE_STARTAPP") != nullptr &&
            std::string_view(std::getenv("STUD_ENABLE_STARTAPP")) == "1") {
            auto lua_shell_platform_params =
                stud::jni_bridge::build_desktop_platform_params_with_lua_flags(
                    asset_dir, engine_dpi_scale, real_viewport_mm_w, real_viewport_mm_h);
            stud::jni_bridge::start_app_with_params_background(jvm, lib, lua_shell_platform_params,
                                                                lifecycle.surface);
        }
    }

    // Real desktop input. Process C owns the only real wl_seat in this
    // architecture; this starts the poll thread that pulls its events over
    // the render IPC and replays them through libroblox's own real
    // NativeInputInterface entry points (see input_bridge.h).
    stud::jni_bridge::start_input_bridge(jvm, lib, lifecycle.activity,
                                        static_cast<long>(lifecycle.game_activity_ptr));

    // Acknowledge a Lua-initiated experience launch. On a real device (and in
    // Sober) the platform answers the engine's start-game request, and only
    // then does the engine create its NetworkClient and join; without it a game
    // picked from the Home screen sits on the loading screen forever. See
    // acknowledge_experience_start()'s own doc comment for the Sober evidence.
    // OFF by default, and deliberately so. Acknowledging works -- the engine
    // acts on it immediately -- but Stud cannot yet fill in WHICH experience:
    // build_desktop_start_game_params() has no launch request to read, so it
    // sends placeId 0 and the server answers "not authorized to join this
    // experience". That is a worse, more misleading outcome than the loading
    // screen simply waiting. The real place id lives in the experience-launch
    // request the Lua app publishes on the MessageBus, which Stud does not
    // subscribe to yet -- that is the next piece of work, and once it lands
    // this becomes unconditional.
    // Learn WHICH experience the Lua app is launching, straight from the
    // request it publishes on the engine's own MessageBus. This is the
    // real mechanism the app's own Java uses (the app's own message-bus subscriber subscribes to
    // JNIExperienceProtocol.getLaunchId(); the app's own launch-request parser parses the JSON), and
    // it is what the acknowledgement below needs: without it Stud could
    // only send placeId 0, which the server rejects as "not authorized to
    // join this experience".
    // Starting the game is the platform's answer to the launch request --
    // that is exactly what the real client does (the app's own launch-request parser parses the
    // request, the app's own app-shell helper calls nativeAppBridgeV2StartGameWithParam with
    // it), and it is why the engine sets stage:UGCGame on receiving the
    // request and then waits.
    //
    // Driven from the request itself, ONCE. It used to hang off
    // gameActivity_onExperienceStart(), which fires again for every launch
    // attempt -- so the second call arrived when the engine was already in
    // UGCGame, which it handles by leaving the experience and relaunching:
    // live-caught as an endless leaveUGCGameInternal/launchUGCGameInternal
    // loop and a "not authorized to join this experience" in the UI, both
    // of which are gone without it.
    std::function<void()> start_game_for_launch_request = [&jvm, &lib, v2_platform_params,
                                                            v2_device_params, lifecycle]() {
        stud::jni_bridge::acknowledge_experience_start(jvm, lib, v2_platform_params,
                                                        v2_device_params, lifecycle.surface);
    };

    stud::jni_bridge::MessageBusRawCallbackStub::on_message =
        [&start_game_for_launch_request](const std::string& json) {
        // Parsed with the same field name the real handler reads
        // (the app's own launch-request parser: jSONObject.optLong("placeId")). Logged by id only --
        // the request also carries join tickets, which are credentials.
        try {
            const auto doc = nlohmann::json::parse(json);
            if (!doc.is_object()) return;
            auto number = [&doc](const char* key) -> long long {
                if (!doc.contains(key)) return 0;
                if (doc[key].is_number()) return doc[key].get<long long>();
                if (doc[key].is_string()) {
                    return std::strtoll(doc[key].get<std::string>().c_str(), nullptr, 10);
                }
                return 0;
            };
            auto text = [&doc](const char* key) -> std::string {
                if (!doc.contains(key) || !doc[key].is_string()) return {};
                return doc[key].get<std::string>();
            };

            // Key names only, never values: this JSON carries join
            // tickets, which are credentials. Which fields the Lua app
            // actually sends is the open question -- Sober's successful
            // join has the instance id and server address in hand at
            // NetworkClient:Create, so that data has to arrive here.
            {
                std::string keys;
                for (auto it = doc.begin(); it != doc.end(); ++it) {
                    if (!keys.empty()) keys += ", ";
                    keys += it.key();
                }
                std::printf("stud: experience-launch request keys: %s\n", keys.c_str());
                std::fflush(stdout);
            }
            stud::jni_bridge::NativeHelperStub::LaunchRequest request;
            request.place_id = number("placeId");
            request.referred_by_player_id = number("referredByPlayerId");
            request.join_attempt_id = text("joinAttemptId");
            request.join_attempt_origin = text("joinAttemptOrigin");
            request.game_join_context = text("gameJoinContext");
            request.launch_data = text("launchData");
            request.event_id = text("eventId");
            request.access_code = text("accessCode");
            request.link_code = text("linkCode");
            request.game_instance_id = text("gameInstanceId");
            if (request.place_id > 0) {
                // Which fields arrived, never their contents: the request
                // carries join tickets, which are credentials.
                std::printf("stud: experience-launch request: placeId=%lld (joinAttemptId=%s "
                            "origin=%s gameJoinContext=%s launchData=%s instance=%s)\n",
                            request.place_id, request.join_attempt_id.empty() ? "no" : "yes",
                            request.join_attempt_origin.empty() ? "no" : "yes",
                            request.game_join_context.empty() ? "no" : "yes",
                            request.launch_data.empty() ? "no" : "yes",
                            request.game_instance_id.empty() ? "no" : "yes");
                std::fflush(stdout);
                stud::jni_bridge::NativeHelperStub::set_last_launch_request(std::move(request));
                // Stud does NOT start the game itself here. The engine's
                // own listener (`listenForExperienceLaunchRequest_`) is
                // subscribed to this same topic and launches the
                // experience on its own -- a second
                // nativeAppBridgeV2StartGameWithParam arrives while the
                // first launch is still coming up, and the engine's
                // `launchUGCGame: (stage:UGCGame)` branch responds by
                // running `leaveUGCGameInternal` and starting over. Live
                // capture of exactly that: the first launch reaches
                // `replaceDataModel (stage:4)` and `submitStartGameTask`
                // (the same point a successful Sober join reaches), then
                // the duplicate tears it down and the retry sits at
                // `stepDataModelJob: No DM yet` forever.
                //
                // The request is still parsed and stored above, because
                // NativeHelperStub hands its fields back to the engine
                // when asked. STUD_ACK_EXPERIENCE_START=1 restores the
                // explicit call for comparison against Sober, which does
                // make it (its engine evidently does not self-launch).
                static const bool ack_experience_start = [] {
                    const char* v = std::getenv("STUD_ACK_EXPERIENCE_START");
                    return v != nullptr && std::string_view(v) == "1";
                }();
                if (ack_experience_start && start_game_for_launch_request) {
                    start_game_for_launch_request();
                }
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "stud: experience-launch request was not JSON (%s)\n", e.what());
        }
    };
    // Send a refreshed login to the keyring. Only the value changes hands
    // -- it is never logged, and never written to Stud's own config or
    // cache. render-host does the actual storing because this process is
    // sandboxed away from the Secret Service and Process A has already
    // exited by the time anyone logs in.
    stud::jni_bridge::set_account_list_cookie_sink([](const std::string& value) {
        static std::string last;
        if (value == last) return;
        last = value;
        std::string payload = std::string(kAccountListSecretName) + "\n" + value;
        uint64_t args[8] = {};
        stud::render_client::connection().call(
            stud::render_host::CallId::StoreSecret, args, payload.data(),
            static_cast<uint32_t>(payload.size()), nullptr, 0, nullptr);
        std::printf("stud: engine produced a refreshed account-list cookie (%zu bytes) -- "
                    "persisting so every signed-in account survives a restart\n",
                    value.size());
        std::fflush(stdout);
    });
    stud::jni_bridge::set_session_cookie_sink([](const std::string& value) {
        static std::string last;
        if (value == last) return;  // the engine re-sends an unchanged cookie often
        last = value;
        std::string payload = std::string(kSessionCookieSecretName) + "\n" + value;
        uint64_t args[8] = {};
        stud::render_client::connection().call(
            stud::render_host::CallId::StoreSecret, args, payload.data(),
            static_cast<uint32_t>(payload.size()), nullptr, 0, nullptr);
        std::printf("stud: engine produced a refreshed session cookie (%zu bytes) -- persisting\n",
                    value.size());
        std::fflush(stdout);
    });
    stud::jni_bridge::subscribe_to_experience_launch(jvm, lib);
    // The web-view protocol, which nothing has ever answered in this
    // process (see webview_bridge.h). Registering the availability
    // handler is what gives the Lua app a reason to send an open request
    // at all; until a real viewer window exists the request is reported
    // and nothing is opened, which is no worse than the current silence
    // and tells us the real URL the app wants.
    // "Open this in a browser" -- the Settings footer links (About Us,
    // Careers, Parents) and anything else calling
    // GuiService:OpenBrowserWindow. These are NOT web-view panels, and
    // nothing was subscribed to the protocol they use, so clicking one
    // did nothing at all: no window, no error, no log line.
    stud::jni_bridge::run_linking_protocol_bootstrap(
        jvm, lib, [](const std::string& url) {
            // Only render-host can reach the desktop, so this rides the
            // same IPC as the web-view panel and is routed there.
            std::string payload = url + "\n\n\n";
            uint64_t args[8] = {};
            args[0] = 1;  // external: hand to the desktop, not the viewer
            const uint64_t ok = stud::render_client::connection().call(
                stud::render_host::CallId::OpenWebView, args, payload.data(),
                static_cast<uint32_t>(payload.size()), nullptr, 0, nullptr);
            return ok != 0;
        });

    // The other half of the same protocol: a URL the web-view panel was
    // about to navigate to, offered to the engine first. This is how a
    // private server is joined -- see linking_bridge.h.
    stud::jni_bridge::run_linking_url_detection_bootstrap(
        jvm, lib, [&jvm, &lib](const std::string& url, bool registered) {
            if (registered) {
                stud::jni_bridge::hand_url_to_engine(jvm, lib, url);
                // The panel exists for the page the user was on, and the
                // app has just moved past it -- a private-server join
                // leaves the experience loading behind a server list
                // nobody can act on any more. A device does the same: the
                // web activity is finished as the launch takes over.
                uint64_t close_args[8] = {};
                stud::render_client::connection().call(
                    stud::render_host::CallId::CloseWebView, close_args, nullptr, 0, nullptr, 0,
                    nullptr);
            }
            // Either way the viewer is told: it loads the page when the
            // engine declined, and drops it when the engine took it --
            // without the second, its own "nobody answered" deadline
            // would load the page on top of a launching experience.
            uint64_t args[8] = {};
            args[0] = registered ? 1 : 0;
            stud::render_client::connection().call(
                stud::render_host::CallId::WebViewLoadUrl, args, url.data(),
                static_cast<uint32_t>(url.size()), nullptr, 0, nullptr);
        });

    stud::jni_bridge::run_webview_protocol_bootstrap(
        jvm, lib,
        [&jvm, &lib, web_view_agent = stud::jni_bridge::build_web_view_user_agent()](
            const std::string& url, const std::string& title) {
            // url \n title \n app token \n cookie... -- the viewer reads
            // this on its stdin.
            //
            // The cookies are read out of the engine's own jar right
            // now, not replayed from a copy taken at login. Roblox lets
            // the user switch between several signed-in accounts inside
            // the app, and a switch replaces the session in that jar --
            // so asking at open time is what makes a panel show the
            // account the user is actually on. Stud never logs in
            // anywhere itself.
            // Line 3 is the whole User-Agent the panel sends -- the
            // real app's own web-view agent, built from this machine's
            // own measurements (app_bridge.h's
            // build_web_view_user_agent). Roblox serves these pages without the site's
            // header and footer when the request identifies the Roblox
            // app rather than a plain browser -- which is why the same
            // URL looks like a bare message list inside the real app and
            // like the whole website in an ordinary web view. The
            // version is the real one read from the configured APK, not
            // a literal.
            std::string payload = url + "\n" + title + "\n" + web_view_agent + "\n";
            // The page's own origin, so the jar that is asked for is the
            // one that page will actually send.
            std::string cookie_url = url;
            if (const auto scheme_end = cookie_url.find("://"); scheme_end != std::string::npos) {
                if (const auto path = cookie_url.find('/', scheme_end + 3);
                    path != std::string::npos) {
                    cookie_url.resize(path);
                }
            }
            auto cookies = stud::jni_bridge::engine_cookies_for_url(jvm, lib, cookie_url);
            // Merge in the session the engine is using right now. The
            // per-domain jar alone is not enough: after switching
            // accounts, www.roblox.com's jar holds only GuestData
            // (live-observed) because the new session was filed under
            // the auth/apis domains, and a panel opened then was signed
            // out entirely.
            const auto live = stud::jni_bridge::current_session_cookies();
            for (const auto& header : live) {
                const auto eq = header.find('=');
                const std::string name = eq == std::string::npos ? header : header.substr(0, eq);
                bool already = false;
                for (const auto& have : cookies) {
                    if (have.rfind(name + "=", 0) == 0) {
                        already = true;
                        break;
                    }
                }
                if (!already) cookies.push_back(header);
            }
            // The theme the app is running in, as the site's own cookie.
            //
            // This is the real mechanism, not a trick: the platform half
            // of the system-theme protocol sets `RBXThemeOverride` on the
            // Roblox domain and the site renders itself in that theme.
            // Without it a panel opens light inside a dark app, which is
            // exactly what Stud did before it answered this protocol at
            // all. `RBXHideThemeSetting` matches the real client too --
            // the account page stops offering a theme picker that the app
            // is already driving.
            const std::string theme = stud::jni_bridge::current_theme_name();
            if (!theme.empty()) {
                cookies.push_back("RBXThemeOverride=" + theme + "; Path=/");
                cookies.push_back("RBXHideThemeSetting=True; Path=/my/account");
            }
            for (const auto& header : cookies) {
                payload += header;
                payload += "\n";
            }
            std::printf("stud: web view opening with %zu live cookie(s) from the current "
                        "session (names: %s)\n",
                        cookies.size(), stud::jni_bridge::cookie_names(cookies).c_str());
            std::fflush(stdout);
            uint64_t args[8] = {};
            stud::render_client::connection().call(
                stud::render_host::CallId::OpenWebView, args, payload.data(),
                static_cast<uint32_t>(payload.size()), nullptr, 0, nullptr);
        },
        [] {
            // The engine is finished with the panel -- a login challenge
            // that has just been answered, most often. Nothing acted on
            // this before, so the challenge window stayed on screen after
            // a completed OTP with the app already signed in behind it.
            uint64_t args[8] = {};
            stud::render_client::connection().call(
                stud::render_host::CallId::CloseWebView, args, nullptr, 0, nullptr, 0, nullptr);
        });

    // gameActivity_onExperienceStart() is deliberately NOT used to start
    // the game any more -- see start_game_for_launch_request above for why
    // (it fires per attempt, and a second start makes the engine leave and
    // relaunch forever).

    // The engine's task scheduler runs at a low BACKGROUND frequency unless it
    // is told otherwise, and Stud only ever told it once -- during bring-up,
    // long before the app was actually up. Measured: 2 swaps in a 50s idle run
    // without this, 267 with it. That is the real reason the app felt slow and
    // why animations only advanced while the mouse moved (each input event woke
    // the engine to render one frame).
    //
    // Asserted here, now that the app really is up, and again whenever the Lua
    // app comes back to the foreground -- leaving a game or an experience
    // ending, both of which are real signals Stud already receives. A real
    // device would also flip it back to background on pause; Stud has no pause
    // yet, so it does not claim to.
    auto assert_foreground = [&jvm, &lib]() {
        stud::jni_bridge::run_set_task_scheduler_foreground(jvm, lib);
    };
    assert_foreground();

    // Roblox's own in-game leave button. The engine reports the return to
    // its app shell, and with the setting on that is Stud's cue to end the
    // session rather than sit on the home screen.
    //
    // Only after a real experience: this same callback fires while the app
    // shell is starting, so acting on it unconditionally would quit during
    // launch. The engine's own experience flag is what tells them apart --
    // it is still set at this point and cleared by onExperienceStop.
    const bool close_on_leave = find_named_arg(argc, argv, "--close-on-leave") == "on";
    stud::jni_bridge::NativeHelperStub::on_returned_to_app = [assert_foreground, close_on_leave]() {
        assert_foreground();
        if (!close_on_leave) return;
        if (!stud::jni_bridge::NativeHelperStub::experience_is_loaded()) return;
        std::printf("stud: left the experience and closeOnLeave is set -- shutting down\n");
        std::fflush(stdout);
        // Close the window here, not after teardown.
        //
        // Setting the flag alone left the home screen on screen for about
        // half a second: the main loop takes up to its own poll interval
        // to notice, then LeaveGame and DestroyApp run -- and through all
        // of it the engine is already back on its app shell and rendering
        // it. Measured on a real leave: decision at t+332.972, teardown
        // started at t+333.176, finished around t+333.4, window gone only
        // after that. The setting says close on leaving, so the window
        // goes now and the teardown below happens behind it.
        //
        // render-host exits inside this call and never replies, so it
        // reads as a lost connection. That is fine and expected: this
        // process is on its way out, and its own client already treats a
        // dead connection as calls that quietly do nothing.
        uint64_t end_args[8] = {};
        stud::render_client::connection().call(stud::render_host::CallId::EndSession, end_args,
                                                nullptr, 0, nullptr, 0, nullptr);
        g_should_keep_running.store(false, std::memory_order_relaxed);
    };

    // Links the engine wants another application to handle -- Roblox
    // Studio above all. render-host decides what is a Roblox page (its
    // own panel) and what belongs to the desktop.
    stud::jni_bridge::JNIAppRestarterStub::on_open_external_url = [](const std::string& url) {
        std::string payload = url + "\n\n\n";  // url, empty title, empty app token
        uint64_t args[8] = {};
        stud::render_client::connection().call(
            stud::render_host::CallId::OpenWebView, args, payload.data(),
            static_cast<uint32_t>(payload.size()), nullptr, 0, nullptr);
    };

    // Watch what the engine's own cookie jar holds when the account
    // changes. Two real, user-reported symptoms to explain: a web-view
    // panel opened after switching accounts is signed out entirely (it
    // only ever carries the account Stud booted with), and a second
    // account's login does not survive a restart.
    //
    // Names and counts only -- these are real credentials.
    stud::jni_bridge::NativeHelperStub::on_account_changed = [&jvm, &lib](const char* what) {
        const auto cookies = stud::jni_bridge::engine_cookies_for_url(jvm, lib,
                                                                       "https://www.roblox.com");
        std::printf("stud: account %s -- engine jar now holds %zu cookie(s) (names: %s)\n", what,
                    cookies.size(), stud::jni_bridge::cookie_names(cookies).c_str());
        std::fflush(stdout);
    };

    // Whether to name the game server's region on a join, and the last
    // one reported -- so a single join notifies once rather than every
    // 250ms loop iteration.
    const bool notify_server_region = find_named_arg(argc, argv, "--notify-region") != "off";
    bool reported_server_region = false;
    // Whether to report the current experience to Discord, and the last
    // one reported -- so one join updates the presence once rather than
    // every 250ms loop iteration.
    const bool discord_presence_enabled = find_named_arg(argc, argv, "--discord-presence") == "on";
    long long reported_place_id = -1;

    std::printf("stud: entering the real event loop (Ctrl+C to stop) ...\n");
    std::fflush(stdout);
    std::signal(SIGINT, handle_shutdown_signal);
    std::signal(SIGTERM, handle_shutdown_signal);
    // Whether the loop ended because render-host went away, rather
    // than because Stud was asked to stop. The two need different
    // shutdowns -- see where it is read, below.
    bool render_host_gone = false;
    while (g_should_keep_running.load(std::memory_order_relaxed)) {
        int fd = 0, events = 0;
        void* data = nullptr;
        ALooper_pollOnce(250, &fd, &events, &data);
        // Real ActivityThread-equivalent driver (see stud/
        // activity_thread.h's own doc comment): this real process main
        // thread is the real main Looper's own thread now, not a
        // separate spawned worker -- drains any real Handler.post()/
        // runOnUiThread() work Roblox's own native code queued, right
        // here, interleaved with real native-window-event polling and
        // rendering, matching real Android's own single-main-thread
        // Looper contract.
        stud::jni_bridge::LooperStub::getMainLooper()->drain_pending(jvm);
        // A/B for the idle-pacing problem (STUD_KEEP_FOREGROUND=1): Stud tells
        // the engine's task scheduler it is in the foreground exactly once, at
        // bring-up, long before the app is actually up. If anything flips it
        // back to background mode afterwards, the scheduler runs at its low
        // background frequency -- which is exactly what "the animation only
        // advances while the mouse moves" looks like.

        // The Discord presence, once per experience change. The place id
        // is the engine's own answer (gameActivity_onGameLoaded), and
        // leaving the experience clears it back to Stud's own identity.
        if (discord_presence_enabled) {
            const bool in_experience = stud::jni_bridge::NativeHelperStub::experience_is_loaded();
            const long long place = in_experience
                                        ? stud::jni_bridge::NativeHelperStub::last_place_id()
                                        : 0;
            if (place != reported_place_id) {
                reported_place_id = place;
                const std::string body = place > 0 ? std::to_string(place) : std::string();
                uint64_t presence_args[8] = {};
                stud::render_client::connection().call(
                    stud::render_host::CallId::SetGamePresence, presence_args,
                    body.empty() ? nullptr : body.data(), static_cast<uint32_t>(body.size()),
                    nullptr, 0, nullptr);
            }
        }

        // The server region, once per join.
        //
        // The engine's own join line names a 10.x UDMUX address that
        // locates nothing, so the routable server is read off the engine's
        // own UDP socket instead -- and only once it has actually
        // connected, which is why this is polled rather than done at the
        // join callback. Rechecked while an experience is loaded so a
        // server hop within one session is reported too.
        if (notify_server_region) {
            const bool in_experience = stud::jni_bridge::NativeHelperStub::experience_is_loaded();
            if (!in_experience) {
                // Left the experience: arm it again for the next join.
                reported_server_region = false;
            } else if (!reported_server_region) {
                const std::string server = stud::jni_bridge::game_server_address();
                if (!server.empty()) {
                    // Once per experience, deliberately. Re-reporting on
                    // any change looked reasonable and was wrong: leaving a
                    // game is not instant, so for a moment the game socket
                    // is gone while the experience still reads as loaded,
                    // and whatever OTHER public UDP peer the engine happens
                    // to hold then becomes the only candidate -- which was
                    // reported as the "server region" on the way back to
                    // the home screen, naming somewhere the user never
                    // joined and sending an unrelated address to be looked
                    // up. Sampling once, on the way in, cannot do that.
                    reported_server_region = true;
                    uint64_t region_args[8] = {};
                    stud::render_client::connection().call(
                        stud::render_host::CallId::NotifyServerRegion, region_args, server.data(),
                        static_cast<uint32_t>(server.size()), nullptr, 0, nullptr);
                }
            }
        }

        // Real window resizes. Process C owns the window and the compositor
        // talks to it, so this side has to ask; the protocol is deliberately
        // client-initiated, and once per 250ms loop iteration is far cheaper
        // than a second channel. Real Android re-delivers onSurfaceChanged on
        // every geometry change -- without it the engine keeps rendering at
        // the size it was told at boot while the window has already grown,
        // which showed up as the old, smaller image sitting in a larger frame
        // with the desktop visible through the rest of it.
        {
            uint64_t size_args[8] = {};
            uint64_t packed = stud::render_client::connection().call(
                stud::render_host::CallId::GetWindowSize, size_args, nullptr, 0, nullptr, 0,
                nullptr);
            if (packed != 0) {
                const int32_t w = static_cast<int32_t>(packed >> 32);
                const int32_t h = static_cast<int32_t>(packed & 0xffffffffu);
                if (w > 0 && h > 0 && (w != real_window_width || h != real_window_height)) {
                    real_window_width = w;
                    real_window_height = h;
                    stud::jni_bridge::set_real_display_metrics(w, h, layout_density);
                    // AGDK's own callback, for correctness -- a real device
                    // sends it on every geometry change. Live-measured, the
                    // engine ignores it for sizing: its surface handling is
                    // entirely in the V2 app bridge, so the call below is what
                    // actually moves its render targets.
                    stud::jni_bridge::dispatch_surface_changed(jvm, lifecycle, w, h);
                    stud::jni_bridge::notify_surface_resized(jvm, lib, v2_platform_params,
                                                             lifecycle.surface);
                }
            }
        }
        // A closed web-view panel has to be reported back, or the Lua
        // app keeps showing the placeholder screen it puts behind one.
        {
            uint64_t webview_args[8] = {};
            const uint64_t closed = stud::render_client::connection().call(
                stud::render_host::CallId::PollWebViewClosed, webview_args, nullptr, 0, nullptr, 0,
                nullptr);
            for (uint64_t i = 0; i < closed; ++i) {
                stud::jni_bridge::publish_webview_closed(jvm, lib);
            }
        }
        // A navigation the panel handed over rather than following. The
        // engine is asked whether it wants the URL; its answer arrives
        // on the subscription registered at bring-up.
        {
            std::vector<char> url(8 * 1024);
            for (;;) {
                uint64_t args[8] = {};
                uint32_t written = 0;
                const uint64_t got = stud::render_client::connection().call(
                    stud::render_host::CallId::PollWebViewNavigation, args, nullptr, 0, url.data(),
                    static_cast<uint32_t>(url.size()), &written);
                if (got == 0 || written == 0) break;
                stud::jni_bridge::ask_engine_about_url(
                    jvm, lib,
                    std::string(url.data(), std::min<size_t>(written, url.size())));
            }
        }
        // Anything the page sent through its JavaScript bridge, in the
        // order it sent it. A login challenge answers this way, so a
        // dropped message means an OTP or captcha that completes on
        // screen and never finishes the login.
        {
            std::vector<char> message(64 * 1024);
            for (;;) {
                uint64_t args[8] = {};
                uint32_t written = 0;
                const uint64_t got = stud::render_client::connection().call(
                    stud::render_host::CallId::PollWebViewMessage, args, nullptr, 0, message.data(),
                    static_cast<uint32_t>(message.size()), &written);
                if (got == 0 || written == 0) break;
                stud::jni_bridge::signal_webview_javascript(
                    jvm, lib, std::string(message.data(), std::min<size_t>(written, message.size())));
            }
        }
        // Real, user-reported bug fixed: closing the real window
        // (stud-render-host, Process C) used to leave this process
        // running forever. Treat a lost render connection as this
        // process's own real shutdown signal, same as SIGINT/SIGTERM.
        // Checked unconditionally now -- it used to sit inside Stud's
        // own fallback-render-context block, which no longer exists.
        if (!stud::render_client::connection().connected()) {
            std::printf("stud: render-host connection lost -- shutting down\n");
            g_should_keep_running.store(false, std::memory_order_relaxed);
            render_host_gone = true;
        }
    }
    // With no render host there is nothing to shut down gracefully.
    //
    // LeaveGame and DestroyApp make the engine do real render work, and
    // if the render host has gone that work runs against a dead
    // connection: the engine's own threads are still mid-frame, their GL
    // and Vulkan calls fail, and one of them faults. The trap handler
    // then re-raises to produce a core -- which is why killing
    // render-host left a trail of linker64 SIGSEGV cores with si_code
    // SI_TKILL (a signal Stud sent itself, not a fault the CPU took),
    // while closing the window never did.
    //
    // The teardown belongs to the window-close path, where the host is
    // still there to answer. Here the honest thing is to stop.
    if (render_host_gone) {
        std::printf("stud: no render host to shut down against -- exiting\n");
        std::fflush(stdout);
        std::fflush(stderr);
        ::_exit(0);
    }

    // Real graceful shutdown pair (see engine_v2_bridge.h's own UPDATE 2
    // doc comment: four real Sober journalctl captures all show this as
    // the real titlebar-close sequence) -- LeaveGame then DestroyApp,
    // both real, confirmed-safe to call even if no game was ever
    // joined. Previously this process just exited raw here.
    try {
        stud::jni_bridge::run_engine_v2_teardown(jvm, lib);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stud: run_engine_v2_teardown() failed: %s\n", e.what());
    }
    std::printf("stud: shutting down\n");
    // Leave immediately instead of returning through main().
    //
    // The engine's own threads are still running here and there is no way
    // to ask them to stop -- LeaveGame and DestroyApp above are the only
    // shutdown contract the engine offers, and they are bounded calls
    // precisely because they do not reliably return. Returning from main
    // then runs static destructors underneath those threads, which tears
    // the JNI layer down while they are still calling into it: live-caught
    // as `terminating due to uncaught exception of type std::runtime_error:
    // Invalid Reference, not a Global Reference` on a libroblox thread,
    // with no checkpoint armed, so the process aborted instead of exiting.
    // Seconds of engine work also kept running between "shutting down" and
    // that abort, which is the delay on closing the window.
    //
    // The process is exiting; the kernel reclaims the memory, the sockets
    // and the file descriptors either way.
    std::fflush(nullptr);
    ::_exit(0);
}
