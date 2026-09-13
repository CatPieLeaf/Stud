// Auth/launcher UI process, separate from the game runtime (mirrors
// Sober's service/runtime split -- confirmed against reference/sober-oss's
// own architecture notes, see the engineering notes' M8 writeup: architecture
// read only, no code copied). Plain Qt6 Widgets (revised from the
// original "Qt6 + libplasma" decision -- see the engineering notes'
// locked-decisions table), QtWebEngine for the Roblox web login flow.
// See the engineering notes, milestone M8.
//
// Two real entry points into this one binary:
//  - Normal launch (no args, e.g. from a desktop icon): show settings
//    (after login, if not already logged in).
//  - "Magnet link" launch: the OS invokes `stud-ui <uri>` because the
//    user clicked Play on roblox.com in their browser and the browser
//    handed off a real roblox-player:/roblox: launch URI to whichever
//    app is registered for it (packaging/stud.desktop) -- the exact
//    same mechanism qBittorrent uses for magnet: links. Skips straight
//    to launching the specific game the link names (after login, if
//    needed), no settings window shown.

#include "credential_store.h"
#include "safe_storage.h"

#include <iostream>
#include "desktop_entry.h"
#include "diagnose.h"
#include "launch_uri.h"
#include "notifications.h"
#include "settings_window.h"
#include "tray.h"
#include "stud/android_glue.h"
#include "stud/bionic_runtime.h"
#include "stud/ipc.h"
#include "stud/settings.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>
#include <QEventLoop>
#include <QFile>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include "stud/session_log.h"
#include "stud/stud_paths.h"
#include <QDateTime>
#include <QDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QCoreApplication>
#include <QTimer>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {

// A previous game session (from an earlier launch the user never fully
// closed, or a crashed/leftover process) left running blocks the new
// one's window from being the only one on screen -- real, observed
// behavior under the old architecture (two windows appearing on a
// relaunch), not theoretical; the same risk applies to the new two-
// process (Process B + stud-render-host) design. Both are meant to be
// single-instance per user (like Sober). Scans /proc directly rather
// than shelling out to pkill -- no assumption that procps is installed
// on every target distro.
//
// stud-render-host is a plain process -- /proc/[pid]/exe resolves to it
// directly. Process B is NOT: it's execve()'d by bwrap inside a real,
// separate PID namespace (bionic_runtime::launch_process_b()'s own
// --unshare-pid), so the PID visible from here is bwrap's own, and
// /proc/[pid]/exe for THAT pid resolves to bwrap itself, not stud-
// runtime-bionic. /proc/[pid]/cmdline (bwrap's real argv, which
// includes the target executable's path) is what actually identifies
// it from outside the sandbox.
void terminate_stale_processes() {
    pid_t self = ::getpid();
    std::vector<pid_t> victims;
    for (const auto& entry : std::filesystem::directory_iterator("/proc")) {
        if (!entry.is_directory()) continue;
        const std::string name = entry.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit)) continue;
        pid_t pid = std::atoi(name.c_str());
        if (pid == self) continue;

        std::error_code ec;
        std::filesystem::path exe = std::filesystem::read_symlink(entry.path() / "exe", ec);
        if (!ec && exe.filename() == "stud-render-host") {
            victims.push_back(pid);
            continue;
        }

        // bwrap-wrapped Process B: match by cmdline instead (real
        // bionic_runtime::launch_process_b() bakes the full
        // stud-runtime-bionic path into bwrap's own argv).
        if (!ec && exe.filename() == "bwrap") {
            std::ifstream cmdline_file(entry.path() / "cmdline", std::ios::binary);
            std::string cmdline((std::istreambuf_iterator<char>(cmdline_file)),
                                 std::istreambuf_iterator<char>());
            if (cmdline.find("stud-runtime-bionic") != std::string::npos) {
                victims.push_back(pid);
            }
        }
    }
    for (pid_t pid : victims) {
        ::kill(pid, SIGTERM);
    }
    if (victims.empty()) return;
    // Give each a real chance to run its own clean shutdown path before
    // a new one starts (avoids two processes racing for the same window
    // briefly); short and bounded, not a hang risk either way.
    for (int i = 0; i < 20; ++i) {
        bool any_alive = false;
        for (pid_t pid : victims) {
            if (::kill(pid, 0) == 0) any_alive = true;
        }
        if (!any_alive) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (pid_t pid : victims) {
        if (::kill(pid, 0) == 0) {
            ::kill(pid, SIGKILL);  // still alive after the grace period -- force it
        }
    }
}


// Key under stud::ui::kKeychainService this app's real session cookie
// is stored as -- distinct from the cookie's own literal name
// (".ROBLOSECURITY", which has characters not worth carrying into a
// keychain key name).
constexpr auto kSessionCookieKey = "roblosecurity";

// Real, best-effort search for a sibling binary: first where this dev
// build's own layout puts it, then PATH, matching however a real
// install eventually lays these out. No formal install step exists yet
// (M11/packaging) -- this is the honest, current state, not a permanent
// design.
QString find_render_host_binary() {
    QString sibling = QCoreApplication::applicationDirPath() + "/../render-host/stud-render-host";
    if (QFile::exists(sibling)) {
        return sibling;
    }
    // An install tree: <prefix>/bin/stud alongside
    // <prefix>/libexec/stud/stud-render-host. Relative, because an
    // AppImage is mounted at a different path every run.
    QString libexec =
        QCoreApplication::applicationDirPath() + "/../libexec/stud/stud-render-host";
    if (QFile::exists(libexec)) {
        return libexec;
    }
    return QStandardPaths::findExecutable("stud-render-host");
}

// Process B (stud-runtime-bionic) is built as a nested ExternalProject
// (see the top-level CMakeLists.txt's own doc comment for why -- a real
// bionic NDK-toolchain build can't share one CMake configure with this
// host/glibc one) -- its dev-build output lives under runtime-prefix/,
// not a plain sibling directory the way every other Stud binary does.
QString find_process_b_binary() {
    QString nested = QCoreApplication::applicationDirPath() +
                      "/../runtime-prefix/src/runtime-build/stud-runtime-bionic";
    if (QFile::exists(nested)) {
        return nested;
    }
    // An install tree. Its own lib64/ overlay directory sits beside it
    // there, which is what bionic_runtime.cpp binds over /system/lib64.
    QString libexec =
        QCoreApplication::applicationDirPath() + "/../libexec/stud/stud-runtime-bionic";
    if (QFile::exists(libexec)) {
        return libexec;
    }
    return QStandardPaths::findExecutable("stud-runtime-bionic");
}

// $XDG_RUNTIME_DIR/stud/render-host.sock -- same convention stud-ipc's
// own default_socket_path() uses, matching stud::render_host::
// default_socket_path()'s real implementation (render-host/) without
// pulling that module into stud-ui just for one path string.
std::string render_host_socket_path() {
    if (const char* xdg_runtime_dir = std::getenv("XDG_RUNTIME_DIR")) {
        return std::string(xdg_runtime_dir) + "/stud/render-host.sock";
    }
    return "/tmp/stud-" + std::to_string(::getuid()) + "/render-host.sock";
}

// Real, bounded, condition-based wait -- not a guessed timing window
// (this project's own hard-won debugging discipline, see BOOT_PROGRESS.md's
// "no arbitrary timing windows" lesson): polls for the real socket FILE
// stud-render-host's own bind()/listen() creates, so Process B's first
// connection attempt (its render-client stubs lazily connect on first
// real EGL/GLES call) doesn't race a render-host that hasn't started
// listening yet. Returns false (caller decides what to do) if it never
// appears within a generous, real bound -- render-host's own real
// startup work (dlopen'ing ANGLE, opening a Wayland connection) is fast
// in practice, so 10s is slack, not a tight guess.
// True when a live Stud already owns the session.
//
// Tested with the same flock render-host holds for its whole life, not by
// looking for processes: a process can be a leftover from a crash, while
// a held lock means something is genuinely running right now. The kernel
// releases the lock when its holder dies however it dies, so this can
// never be stale.
bool another_instance_is_running() {
    const std::string socket_path = render_host_socket_path();
    const auto slash = socket_path.find_last_of('/');
    if (slash == std::string::npos) return false;
    const std::string dir = socket_path.substr(0, slash);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string path = dir + "/stud.lock";
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return false;  // Cannot tell: let the launch proceed.
    const bool free_to_take = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
    if (free_to_take) ::flock(fd, LOCK_UN);
    ::close(fd);
    return !free_to_take;
}

// MangoHud, pointed at the API Stud actually presents with.
//
// MangoHud instruments the process that talks to the driver, which is
// render-host -- but which API that process presents with depends on the
// render path, and MangoHud has a separate hook for each.
//
// * Vulkan path: render-host calls the real driver itself, so MangoHud's
//   Vulkan layer sits exactly where it belongs. MANGOHUD=1 enables it and
//   nothing else is needed.
//
// * Every other path renders through Stud's own ANGLE build. There the
//   Vulkan layer is the wrong hook and an actively harmful one: ANGLE's
//   use of Vulkan is an implementation detail, and ANGLE recreates its
//   swapchain whenever the engine tears down and rebuilds its EGL window
//   surface -- which it does on joining a game. MangoHud has a
//   long-standing crash on swapchain recreation
//   (flightlessmango/MangoHud#1259, #1774), and that is the live-reported
//   "Stud crashed with MangoHud on the ANGLE path when joining a game".
//   The layer is therefore switched off BY NAME through the Vulkan
//   loader's own VK_LOADER_LAYERS_DISABLE, which also covers a user who
//   has MANGOHUD=1 in their own environment and never asked Stud for it.
//
//   What replaces it is MangoHud's OpenGL hook, which is what the
//   `mangohud` wrapper script itself sets up: MANGOHUD=1 plus its shim on
//   LD_PRELOAD. The shim is needed rather than plain symbol interposition
//   because render-host resolves every entry point with dlsym() against
//   ANGLE's own handle, which ordinary LD_PRELOAD interposition never
//   sees; the shim hooks dlopen/dlsym themselves.
//
//   Measured, on this machine, so neither half is assumed: with the shim
//   preloaded the ANGLE desktop-GL backend really does load
//   libMangoHud_opengl.so and MangoHud logs real per-frame FPS for it
//   (that path presents through the system EGL, which the shim can hook).
//   The ANGLE-to-Vulkan and SwiftShader backends do not -- they present
//   through their own vkQueuePresentKHR and never touch the system EGL --
//   so on those two there is no overlay to offer once the crashing layer
//   is off. That is the honest trade: the Vulkan render path and the
//   OpenGL one show the HUD, the two in between do not crash.
// What VK_LOADER_LAYERS_DISABLE said before Stud touched it, so the value
// this process needs for itself is never mistaken for the user's own.
QString g_host_layers_disable;
bool g_layers_disable_was_set = false;

// MangoHud must not run in THIS process.
//
// It belongs to render-host, which is the process that talks to the
// driver and draws the frames worth measuring. Stud's UI uses Vulkan too
// -- it enumerates the machine's GPUs with it -- and that is enough to
// load the layer, which then starts its own sampling threads inside a Qt
// widgets application that has no frames at all.
//
// Live-caught, from a user running with MANGOHUD=1 already in their
// environment: `stud` died with SIGSEGV on a thread named
// `mangohud-nvidia`, inside libMangoHud's own NVIDIA sampler, and the
// Settings window would not open. Nothing Stud did asked for that; it
// was inherited.
//
// So the layer is switched off by name for this process only, before any
// Vulkan call. The user's own value is kept and handed back to
// render-host untouched, so asking for the overlay still gets it exactly
// where it belongs.
void keep_mangohud_out_of_this_process() {
    if (::getenv("MANGOHUD") == nullptr && ::getenv("MANGOHUD_CONFIG") == nullptr) return;
    if (const char* existing = ::getenv("VK_LOADER_LAYERS_DISABLE")) {
        g_host_layers_disable = QString::fromLocal8Bit(existing);
        g_layers_disable_was_set = true;
    }
    const QString disabled =
        g_layers_disable_was_set && !g_host_layers_disable.isEmpty()
            ? g_host_layers_disable + QStringLiteral(",VK_LAYER_MANGOHUD_overlay_*")
            : QStringLiteral("VK_LAYER_MANGOHUD_overlay_*");
    ::setenv("VK_LOADER_LAYERS_DISABLE", disabled.toLocal8Bit().constData(), 1);
}

void apply_mangohud_environment(QProcessEnvironment& env, bool enabled, bool vulkan_render_path) {
    // Undo what keep_mangohud_out_of_this_process() did to our own
    // environment before deciding anything: that entry was for Stud's UI,
    // and render-host is the process the overlay is for.
    if (g_layers_disable_was_set) {
        env.insert(QStringLiteral("VK_LOADER_LAYERS_DISABLE"), g_host_layers_disable);
    } else {
        env.remove(QStringLiteral("VK_LOADER_LAYERS_DISABLE"));
    }

    // MangoHud's picmip rewrites every sampler's mip LOD bias
    // (overlay_CreateSampler: `if (picmip > -17 && picmip < 17)
    // mipLodBias = picmip`). A MangoHud.conf carrying picmip=-16 -- as this
    // machine's does -- makes every texture in Roblox ultra sharp and
    // aliased, and it is not a choice Stud's user made for Stud. -17 is
    // MangoHud's own default, the value that means "leave the sampler
    // alone", so it is forced here whenever MangoHud will run at all,
    // including when MANGOHUD=1 came from the user's own environment.
    //
    // read_cfg is required, not decoration: with MANGOHUD_CONFIG set and no
    // read_cfg, MangoHud does not read the config file at all and the
    // user's HUD layout is gone. With it, the file is read and the
    // environment is applied after, so picmip=-17 wins. Verified with two
    // screenshots of the same Vulkan window, with and without this: the HUD
    // is identical. A MANGOHUD_CONFIG the user already set is appended to
    // rather than given read_cfg, since leaving it out was their choice.
    if (enabled || env.contains("MANGOHUD")) {
        const QString existing = env.value("MANGOHUD_CONFIG");
        env.insert("MANGOHUD_CONFIG", existing.isEmpty()
                                          ? QStringLiteral("read_cfg,picmip=-17")
                                          : existing + QStringLiteral(",picmip=-17"));
    }
    if (enabled && vulkan_render_path) {
        env.insert("MANGOHUD", "1");
        return;
    }
    if (!enabled) {
        // Nothing to do unless the environment already carries it, in
        // which case the guard below still matters on the ANGLE paths.
        if (!env.contains("MANGOHUD") || vulkan_render_path) return;
    } else {
        env.insert("MANGOHUD", "1");
    }
    // An ANGLE path, with MangoHud on one way or the other.
    const QString existing = env.value("VK_LOADER_LAYERS_DISABLE");
    const QString disabled = QStringLiteral("VK_LAYER_MANGOHUD_overlay_*");
    env.insert("VK_LOADER_LAYERS_DISABLE",
                existing.isEmpty() ? disabled : existing + "," + disabled);
    if (!enabled) return;
    // $LIB is expanded by the dynamic linker itself (lib64 or lib), which
    // is exactly how MangoHud's own wrapper spells this.
    const QString shim = QStringLiteral("/usr/$LIB/mangohud/libMangoHud_shim.so");
    // Where distributions actually put it: lib64 (Fedora/Arch), plain lib,
    // and Debian/Ubuntu multiarch. $LIB above expands correctly on all of
    // them -- this check only has to answer "is it installed at all", so
    // do not preload a file that is not there.
    static const char* const kShimDirs[] = {
        "/usr/lib64/mangohud", "/usr/lib/mangohud",
        "/usr/lib/x86_64-linux-gnu/mangohud", "/usr/local/lib64/mangohud",
        "/usr/local/lib/mangohud",
    };
    bool shim_installed = false;
    for (const char* dir : kShimDirs) {
        if (QFile::exists(QString::fromLatin1(dir) + "/libMangoHud_shim.so")) {
            shim_installed = true;
            break;
        }
    }
    if (!shim_installed) return;
    const QString preload = env.value("LD_PRELOAD");
    if (preload.contains("libMangoHud_shim.so")) return;
    env.insert("LD_PRELOAD", preload.isEmpty() ? shim : preload + ":" + shim);
}

bool wait_for_render_host_socket() {
    // Only the file, deliberately: the first connection this process's
    // render-host accepts becomes its primary client, so probing with a
    // real connect() would hand it a client that immediately disappears.
    // What made the old check unsafe was not the check itself but that a
    // render-host killed rather than shut down leaves its socket file
    // behind, so this returned true on the previous run's leftover and
    // Process B could reach a render-host started with the PREVIOUS
    // settings -- which is what "I chose OpenGL and it stayed on Zink"
    // looks like from outside. The leftover is removed before the new one
    // starts (see remove_stale_render_host_socket), so the file appearing
    // now can only be the new render-host's own bind().
    QString path = QString::fromStdString(render_host_socket_path());
    for (int i = 0; i < 200; ++i) {
        if (QFile::exists(path)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// Removes shared-memory files no live Stud can own any more. Safe only
// because terminate_stale_processes() has just run: nothing of Stud's is
// left to hold them.
//
// These are the Vulkan client's host-visible allocations, unlinked as
// soon as the host imports them now -- but a build without that fix, or a
// hard kill mid-allocation, leaves them behind, and nothing else ever
// removes them. They live in $XDG_RUNTIME_DIR, which is a tmpfs that also
// holds the Wayland socket, D-Bus and the session's own state, so filling
// it does not just waste memory: it takes the desktop down with it. Found
// at 129 files and 3.1GB, that tmpfs 100% full with 8K free.
void remove_orphaned_shared_memory() {
    const std::string socket_path = render_host_socket_path();
    const auto slash = socket_path.find_last_of('/');
    if (slash == std::string::npos) return;
    const std::string dir = socket_path.substr(0, slash);
    std::error_code ec;
    uintmax_t removed_bytes = 0;
    int removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (entry.path().filename().string().rfind("mem-", 0) != 0) continue;
        std::error_code size_ec;
        const uintmax_t size = std::filesystem::file_size(entry.path(), size_ec);
        std::error_code rm_ec;
        if (std::filesystem::remove(entry.path(), rm_ec)) {
            ++removed;
            if (!size_ec) removed_bytes += size;
        }
    }
    if (removed > 0) {
        std::printf("stud: removed %d orphaned shared-memory file(s), %.1f MB\n", removed,
                    static_cast<double>(removed_bytes) / (1024.0 * 1024.0));
        std::fflush(stdout);
    }
}

// Removes a socket file left behind by a render-host that was killed
// rather than shut down. Called after the stale processes are gone and
// before the new one starts, so nothing that outlived its process can be
// mistaken for the new one.
void remove_stale_render_host_socket() {
    ::unlink(render_host_socket_path().c_str());
}

// Real, blocking HTTPS GET against Roblox's own ClientSettings endpoint
// (see jni-bridge/include/stud/client_settings_bridge.h's own doc
// comment for the ground-truth-traced URL and why this fetch has to
// happen somewhere other than inside the real device's Java layer,
// which Stud doesn't have). Runs here, in the UI process, rather than
// in Process B: this process is already a real glibc process doing
// other network-adjacent work (the login flow), whereas cross-building
// curl for bionic would be substantial standalone work for one fixed-
// URL GET. QNetworkAccessManager + a local QEventLoop gives a real,
// synchronous-from-the-caller's-perspective wait without blocking on a
// raw socket read -- ordinary Qt idiom, no new dependency (Qt6::Network
// is already pulled in transitively by Qt6::WebEngineWidgets, linked
// explicitly below for clarity).
// Must match what Process B hands nativeInitClientSettings -- fetching
// one group and declaring another would apply the wrong policy silently.
QString client_settings_group_name() {
    const QByteArray env = qgetenv("STUD_CLIENT_SETTINGS_GROUP");
    return env.isEmpty() ? QStringLiteral("PCDesktopClient") : QString::fromUtf8(env);
}

void fetch_client_settings(stud::ipc::LaunchPayload& payload) {
    QNetworkAccessManager manager;
    QNetworkRequest request(
        QUrl(QStringLiteral("https://clientsettingscdn.roblox.com/v2/settings/application/%1")
                 .arg(client_settings_group_name())));
    std::unique_ptr<QNetworkReply> reply(manager.get(request));
    QEventLoop loop;
    QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    payload.client_settings_http_status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toLongLong();
    if (reply->error() == QNetworkReply::NoError) {
        payload.client_settings_body = reply->readAll().toStdString();
    } else {
        std::fprintf(stderr, "stud: client settings fetch failed: %s\n",
                     reply->errorString().toUtf8().constData());
    }
}

// Roblox's own real, public, widely-used-by-third-party-tools endpoint
// for "who is this .ROBLOSECURITY cookie logged in as" -- real JSON
// `{"id":..., "name":..., "displayName":...}` on success. Investigated
// this session as the likely real fix for a native crash traced
// to UserController::didLogin() dereferencing a null
// singleton: Stud's own NativeUserJavaInterface stub previously always
// reported a placeholder userId of 0 regardless of whatever real
// cookie was supplied, which real native code is very plausibly
// reading as "not logged in" and never constructing UserController as
// a result. No-ops (leaves payload's authenticated_* fields at their
// zero/empty defaults) if there's no cookie or the fetch fails --
// same honest-degradation pattern as fetch_client_settings() above.
void fetch_authenticated_user(stud::ipc::LaunchPayload& payload) {
    if (payload.session_cookie.empty()) {
        return;
    }
    QNetworkAccessManager manager;
    QNetworkRequest request(QUrl("https://users.roblox.com/v1/users/authenticated"));
    // payload.session_cookie is just the raw cookie VALUE (see
    // login_window.cpp's extractSessionCookieValue()) -- needs the real
    // cookie name prefixed back on for a real Cookie header.
    request.setRawHeader("Cookie",
                          QByteArray::fromStdString(".ROBLOSECURITY=" + payload.session_cookie));
    std::unique_ptr<QNetworkReply> reply(manager.get(request));
    QEventLoop loop;
    QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        std::fprintf(stderr, "stud: authenticated-user fetch failed: %s\n",
                     reply->errorString().toUtf8().constData());
        return;
    }
    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isObject()) {
        std::fprintf(stderr, "stud: authenticated-user fetch: response wasn't a JSON object\n");
        return;
    }
    QJsonObject obj = doc.object();
    payload.authenticated_user_id = static_cast<long long>(obj.value("id").toDouble());
    payload.authenticated_username = obj.value("name").toString().toStdString();
    payload.authenticated_display_name = obj.value("displayName").toString().toStdString();
    std::printf("stud: authenticated as real user id=%lld username=%s\n",
                payload.authenticated_user_id, payload.authenticated_username.c_str());
}

// Real, public, community-documented Roblox endpoint
// (PlaceLauncher.ashx?request=RequestGame) that every real third-party
// Roblox launcher uses to turn a deep link's opaque join ticket into
// real server-join info -- confirmed real this session from a real,
// live roblox-player:// URI's own placelauncherurl field (a complete,
// ready-to-call URL Roblox's own client embeds in the deep link).
// Deliberately just fetches and logs the raw response rather than
// parsing it into StartGameParams fields: this project has no
// live-captured real response to ground-truth the current exact JSON
// schema against (Roblox has changed it before, and guessing field
// names risks silently feeding wrong data into a real game-join call
// rather than an honest placeholder). No-ops if there's no
// place_launcher_url or no cookie, same honest-degradation pattern as
// fetch_client_settings()/fetch_authenticated_user() above.
void fetch_place_launcher_info(stud::ipc::LaunchPayload& payload) {
    if (payload.place_launcher_url.empty() || payload.session_cookie.empty()) {
        return;
    }
    QNetworkAccessManager manager;
    QNetworkRequest request(QUrl(QString::fromStdString(payload.place_launcher_url)));
    request.setRawHeader("Cookie",
                          QByteArray::fromStdString(".ROBLOSECURITY=" + payload.session_cookie));
    std::unique_ptr<QNetworkReply> reply(manager.get(request));
    QEventLoop loop;
    QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        std::fprintf(stderr, "stud: place-launcher fetch failed: %s\n",
                     reply->errorString().toUtf8().constData());
        return;
    }
    payload.place_launcher_response = reply->readAll().toStdString();
    std::printf("stud: real place-launcher response (%zu bytes): %s\n",
                payload.place_launcher_response.size(), payload.place_launcher_response.c_str());
}

// Real launch flow: extracts the native library + assets from the
// user's configured APK (already done once, in Settings), launches
// stud-render-host (Process C: real glibc, hosts ANGLE -- see
// render-host/src/main.cpp), waits for it to be ready, then launches
// the real bionic Process B (stud-runtime-bionic, sandboxed via
// bionic_runtime::launch_process_b()) and hands off the real session
// cookie over stud-ipc, exactly as before.

// One log file per session, named for when it started, kept where a log
// belongs (~/.local/state/stud/logs -- stud/stud_paths.h). All three
// processes append to the same file: Process B and render-host read the
// path out of STUD_LOG_FILE, which is exported here before either is
// spawned.
//
// Written for the case Stud is actually used in -- a desktop launch,
// where the only record today is the systemd journal, which a user
// cannot be asked to produce and which keeps nothing at all on a system
// without persistent journald.
void start_session_log() {
    const QString dir = QString::fromStdString(stud::paths::log_dir());
    QDir().mkpath(dir);

    // Keep the last few sessions and no more. A log nobody reads is not
    // worth unbounded disk, and the interesting one is almost always the
    // most recent.
    constexpr int kSessionsKept = 10;
    QDir log_dir(dir);
    const QStringList old = log_dir.entryList({QStringLiteral("session-*.log")},
                                               QDir::Files, QDir::Name);
    for (int i = 0; i < old.size() - (kSessionsKept - 1); ++i) {
        QFile::remove(log_dir.filePath(old.at(i)));
    }

    const QString path =
        log_dir.filePath(QStringLiteral("session-%1.log")
                             .arg(QDateTime::currentDateTime().toString(
                                 QStringLiteral("yyyyMMdd-hhmmss"))));
    // Exported before anything is spawned, so both other processes
    // inherit it -- render-host through its QProcess environment, and
    // Process B through bwrap's own --setenv (see config.extra_env).
    qputenv("STUD_LOG_FILE", path.toUtf8());
    stud::logging::start_session_log(path.toStdString());
    stud::logging::install_crash_reporter("stud-ui");
    std::printf("stud: session log: %s\n", path.toUtf8().constData());
}

void launch_game(const std::optional<stud::ui::LaunchUri>& launch_uri) {
    stud::config::StudSettings settings;
    try {
        settings = stud::config::load_settings(stud::config::default_config_path());
    } catch (const stud::config::SettingsError&) {
        // Fall through with defaults -- apk_path will be empty, caught below.
    }

    if (settings.apk_path.empty()) {
        // main() opens Settings when nothing is configured, so reaching
        // here means the APK went missing between that check and this
        // one. Say that, rather than the old text telling the user to
        // open Stud and go to Settings -- which was what they had just
        // done, and from a fresh AppImage was not even possible.
        QMessageBox::warning(nullptr, "Stud",
                              "No Roblox APK is configured. Choose one in Settings, from the tray "
                              "or from Stud's Settings action.");
        return;
    }

    QString render_host_binary = find_render_host_binary();
    QString process_b_binary = find_process_b_binary();
    if (render_host_binary.isEmpty() || process_b_binary.isEmpty()) {
        QMessageBox::critical(nullptr, "Stud",
                               "Could not find the stud-render-host and/or stud-runtime-bionic "
                               "binaries.");
        return;
    }

    // Real, once-per-import work (extraction) already happened in
    // Settings when this APK was selected (settings_window.cpp's
    // onSaveClicked()). Every actual game launch just uses whatever's
    // already cached there, directly -- no re-extraction.
    // If the cached extraction is missing, just redo it here rather
    // than telling the user to go re-save Settings. The cache lives
    // under ~/.cache, which anything (a cache cleaner, a manual `rm`,
    // a disk-space sweep) may legitimately delete at any time -- a
    // regenerable cache going missing is a normal condition, not a
    // misconfiguration, and the APK path is already known. Settings
    // still does the same extraction at APK-selection time; this is
    // purely the "cache went away" recovery path.
    std::string so_path = stud::android_glue::default_libroblox_cache_path();
    // Re-extract when the configured APK is a different file than the one
    // this cache came from, not only when the cache is gone: a user who
    // points Stud at a newer build has to actually get that build.
    const std::string launch_fingerprint =
        stud::android_glue::apk_source_fingerprint(settings.apk_path);
    const std::string launch_stamp_path = so_path + ".source";
    std::string launch_cached_fingerprint;
    {
        std::ifstream stamp{launch_stamp_path};
        if (stamp) std::getline(stamp, launch_cached_fingerprint);
    }
    if (!QFile::exists(QString::fromStdString(so_path)) ||
        (!launch_fingerprint.empty() && launch_cached_fingerprint != launch_fingerprint)) {
        try {
            stud::android_glue::extract_apk_native_library(settings.apk_path, "libroblox.so",
                                                            so_path);
            std::ofstream(launch_stamp_path, std::ios::trunc) << launch_fingerprint << "\n";
        } catch (const stud::android_glue::ExtractError& e) {
            QMessageBox::critical(
                nullptr, "Stud",
                QString("Could not extract libroblox.so from the configured APK:\n%1\n\nAPK: %2")
                    .arg(e.what())
                    .arg(QString::fromStdString(settings.apk_path)));
            return;
        }
    }

    // One instance, and a second launch says so rather than taking the
    // running one down. Killing it was the old behaviour and it is the
    // wrong answer for the two ways this actually happens: launching the
    // desktop entry again, and clicking a game link in a browser while
    // already playing. Both used to end the session in progress.
    if (another_instance_is_running()) {
        QMessageBox::information(nullptr, "Stud",
                                  "Stud is already running.\n\nClose the existing window before "
                                  "starting it again.");
        return;
    }

    start_session_log();
    // The first thing in every log: what machine this is, what is
    // configured, and what hardware was found. A log that arrives
    // without it costs a round of questions before anyone can start.
    stud::ui::write_diagnostics();

    terminate_stale_processes();
    remove_stale_render_host_socket();
    remove_orphaned_shared_memory();

    // Real wiring for the Settings "graphics mode" control, which used to
    // round-trip through settings.json and be read by nothing at all.
    // Deliberately NOT an FFlag: flag_overrides.h's locked decision is that
    // FFlag overrides come only from the raw hand-edited file, never from a
    // UI toggle. This drives the one thing the toggle can honestly control
    // in Stud's architecture -- which backend the vendored ANGLE uses for
    // the real render context (real native Vulkan, or GLES) -- matching this
    // project's own "ANGLE for both paths" constraint.
    QStringList render_host_args;
    // STUD_GRAPHICS_MODE overrides the saved choice for one run.
    //
    // Which renderer the engine uses changes which half of the render
    // client is even reachable -- the GL forwarding layer is untouched in
    // Vulkan mode -- so comparing the two is a routine part of chasing a
    // rendering bug, and editing Settings between every run is a way to
    // leave the wrong value saved.
    QString graphics_mode_arg;
    {
        const QString forced = qEnvironmentVariable("STUD_GRAPHICS_MODE");
        const bool vulkan = forced.isEmpty()
                                ? settings.graphics_mode == stud::config::GraphicsMode::kVulkan
                                : forced != QStringLiteral("opengl");
        graphics_mode_arg = vulkan ? QStringLiteral("vulkan") : QStringLiteral("opengl");
        render_host_args << "--graphics-mode" << graphics_mode_arg;
    }
    // Real HiDPI selection. Process C owns the window, so it is the only
    // process that can honour the compositor's scale -- this toggle had
    // round-tripped through settings.json and been read by nothing.
    render_host_args << "--hidpi" << (settings.hidpi ? "on" : "off");
    render_host_args << "--background-fps" << QString::number(settings.background_fps);
    render_host_args << "--discord-presence" << (settings.discord_rich_presence ? "on" : "off");
    render_host_args << "--discord-join-button" << (settings.discord_join_button ? "on" : "off");
    // Where Roblox's own assets were extracted. Process C draws the text
    // overlay for a focused TextBox and needs the engine's real fonts to
    // draw it in -- Process B is sandboxed and sees a different path, so
    // the host-side one has to come from here.
    render_host_args << "--assets-dir" << QString::fromStdString(stud::android_glue::default_assets_cache_dir());
    // Spawned through a QProcess rather than the static helper so it can
    // carry an environment: MangoHud hooks whichever process talks to the
    // driver, which is this one.
    QProcess render_host;
    // Give the child the REAL stdout, not this process's tee pipe. The
    // pipe's only reader is this process's own tee thread, and Process A
    // exits within a second of launching -- after which a child writing
    // to it would lose its output entirely. Each child opens the session
    // log itself (stud/session_log.h).
    if (const int passthrough = stud::logging::passthrough_stdout_fd(); passthrough >= 0) {
        render_host.setChildProcessModifier([passthrough] {
            ::dup2(passthrough, STDOUT_FILENO);
            ::dup2(passthrough, STDERR_FILENO);
        });
    }
    render_host.setProgram(render_host_binary);
    render_host.setArguments(render_host_args);
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        apply_mangohud_environment(env, settings.mangohud,
                                    settings.graphics_mode == stud::config::GraphicsMode::kVulkan);
        render_host.setProcessEnvironment(env);
    }
    if (!render_host.startDetached()) {
        QMessageBox::critical(nullptr, "Stud", "Failed to start stud-render-host.");
        return;
    }
    if (!wait_for_render_host_socket()) {
        QMessageBox::critical(nullptr, "Stud",
                               "stud-render-host did not become ready in time (no real Wayland "
                               "compositor reachable, or ANGLE failed to load -- check its stderr).");
        return;
    }

    stud::ipc::LaunchPayload payload;
    // Safe storage only. The cookie is never read from, written to, or
    // migrated through the keyring directly -- the keyring holds nothing
    // but the safe-storage key (see safe_storage.h). A login stored by an
    // older build simply is not found, and the user logs in again, which
    // is the correct outcome: it means there is exactly one path a
    // credential can travel, with no legacy branch that could put a
    // plaintext value back into the keyring.
    if (auto cookie = stud::ui::load_secret(kSessionCookieKey)) {
        payload.session_cookie = cookie->toStdString();
    }
    // The desktop's light/dark setting, from the process that has a
    // desktop to ask. Qt reads the same freedesktop appearance setting
    // (`org.freedesktop.appearance color-scheme`) that every other
    // desktop application uses, so Stud follows whatever the user set
    // rather than deciding for itself. STUD_DARK_MODE=1/0 overrides it,
    // which is the only way to test the other branch on a machine that
    // is set one way.
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    payload.system_dark_mode =
        QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
    // Qt gained a direct answer in 6.5. Before that, the palette is what
    // there is: a theme is dark when its window text is lighter than the
    // window behind it. That is what Qt's own pre-6.5 code did, and it
    // agrees with the later answer on every ordinary theme.
    {
        const QPalette palette = QGuiApplication::palette();
        payload.system_dark_mode = palette.color(QPalette::WindowText).lightness() >
                                    palette.color(QPalette::Window).lightness();
    }
#endif
    if (const char* forced = std::getenv("STUD_DARK_MODE")) {
        payload.system_dark_mode = forced[0] == '1';
    }
    std::printf("stud: desktop colour scheme: %s\n",
                payload.system_dark_mode ? "dark" : "light");
    if (launch_uri) {
        payload.game_info = launch_uri->game_info;
        payload.place_launcher_url = launch_uri->place_launcher_url;
        payload.deep_link_place_id = launch_uri->place_id;
        payload.deep_link_join_attempt_id = launch_uri->join_attempt_id;
        payload.deep_link_referred_by_player_id = launch_uri->referred_by_player_id;
        payload.deep_link_join_attempt_origin = launch_uri->join_attempt_origin;
        payload.deep_link_game_instance_id = launch_uri->game_instance_id;
    }
    fetch_client_settings(payload);
    fetch_authenticated_user(payload);
    fetch_place_launcher_info(payload);

    std::string socket_path = stud::ipc::default_socket_path();

    // Serves in the background so Process B (started right after, further
    // down this function) can connect without this call blocking on a
    // connection that can't exist yet -- serve_launch_payload_once()
    // itself already has a real, bounded timeout (see stud-ipc/include/
    // stud/ipc.h), so this thread is guaranteed to finish, not leaked
    // indefinitely.
    //
    // Real, live-caught regression fixed: this used to be .detach()ed,
    // on the theory that its own bounded timeout made that safe. It
    // isn't -- a detached thread dies with the whole process, and
    // proceed() (this function's only real caller) quits the process
    // right after this function returns. Once the OTHER real hang bug
    // in proceed() got fixed (QApplication::quit() no longer silently
    // swallowed), stud-ui started exiting fast enough to routinely beat
    // this thread to its own accept()+write() -- confirmed live:
    // Process B's own log showed "malformed launch payload: ...
    // attempting to parse an empty input" almost every run, immediately
    // followed by a real RBXCRASH ("Can't initialize the TaskScheduler
    // before flags have been loaded"). Joined now, at the end of this
    // function, after Process B has actually been spawned -- so the
    // real concurrency this was introduced for (server listening while
    // the rest of this function does its own, slower setup work) is
    // preserved, but launch_game() -- and so proceed()'s subsequent
    // quit() -- can no longer return before the handoff genuinely
    // finishes or times out.
    std::thread payload_thread([socket_path, payload]() {
        try {
            stud::ipc::serve_launch_payload_once(socket_path, payload);
        } catch (const stud::ipc::IpcError&) {
            // Best-effort handoff -- if Process B never connects (e.g.
            // it failed to start), there's nothing further to report
            // here; its own stderr already carries the failure if it
            // did start.
        }
    });

    // Real host paths Process B needs visible inside its sandbox, at the
    // identical absolute path outside it (bionic_runtime::launch_process_b()'s
    // own transparent-passthrough convention): the extracted libroblox.so/
    // assets/cache root (writable -- Process B's own cache_subdir() writes
    // there), $XDG_RUNTIME_DIR (writable -- covers both stud-render-host's
    // socket and stud-ipc's own launch socket, both live under
    // $XDG_RUNTIME_DIR/stud/), and the configured APK's own containing
    // directory (read-only -- only needed for the one-shot asset
    // extraction, already read-only by nature).
    std::vector<stud::bionic_runtime::HostBind> extra_binds;
    extra_binds.push_back(
        {std::filesystem::path(so_path).parent_path().string(), /*writable=*/true});
    if (const char* xdg_runtime_dir = std::getenv("XDG_RUNTIME_DIR")) {
        extra_binds.push_back({xdg_runtime_dir, /*writable=*/true});
    }
    {
        std::string apk_dir = std::filesystem::path(settings.apk_path).parent_path().string();
        if (!apk_dir.empty()) {
            extra_binds.push_back({apk_dir, /*writable=*/false});
        }
    }
    // Writable bind for Stud's own data directory, where the runtime keeps
    // the persistent local storage the engine logs in through. Without it
    // Process B's sandbox (`--tmpfs /`) has no such path at all, the store
    // silently never gets written, and every login is forgotten on exit --
    // live-caught exactly that way. Created here, by the process that has
    // the real HOME, so the bind always has something to point at.
    {
        std::string data_home;
        if (const char* xdg_data_home = std::getenv("XDG_DATA_HOME");
            xdg_data_home != nullptr && *xdg_data_home != '\0') {
            data_home = xdg_data_home;
        } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
            data_home = std::string(home) + "/.local/share";
        }
        if (!data_home.empty()) {
            const std::string dir = data_home + "/stud";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                          std::filesystem::perm_options::replace, ec);
            if (!ec) extra_binds.push_back({dir, /*writable=*/true});
        }
    }

    // The session log, into the sandbox: Process B writes to the same
    // file as the other two, so the directory has to exist inside its
    // own mount namespace and the path has to reach it as an env var.
    {
        const std::string log_dir = stud::paths::log_dir();
        const QByteArray log_file = qgetenv("STUD_LOG_FILE");
        if (!log_file.isEmpty()) {
            extra_binds.push_back({log_dir, /*writable=*/true});
        }
    }

    stud::bionic_runtime::ProcessBConfig config;
    config.executable_path = process_b_binary.toStdString();
    config.args = {so_path, "--apk", settings.apk_path, "--ipc-connect", socket_path};

    // Real, previously-missing wiring: process-b/src/main.cpp already
    // reads a "--flag-overrides <path>" arg and threads it all the way
    // through to the real nativePreloadFlagOverrides call (see
    // bootstrap.cpp) -- but launch_game() never actually passed one, so
    // the whole real FlagOverrides mechanism sat unreachable from any
    // real launch. Per the locked decision (flag_overrides.h's own doc
    // comment): this is a raw, hand-edited JSON file, never generated
    // from a Settings UI toggle (settings.graphics_mode is a separate,
    // real Stud-level knob and deliberately isn't read here) -- a
    // missing file is the normal case and silently skipped, same as
    // FlagOverrides::load_from_file()'s own contract.
    std::string flag_overrides_path = stud::config::default_flag_overrides_path();
    if (QFile::exists(QString::fromStdString(flag_overrides_path))) {
        config.args.push_back("--flag-overrides");
        config.args.push_back(flag_overrides_path);
        extra_binds.push_back(
            {std::filesystem::path(flag_overrides_path).parent_path().string(), /*writable=*/false});
    }
    // The layout scale is Process B's to apply, and it needs both
    // halves: following the display only means anything while the buffer
    // is being scaled, because with HiDPI off the buffer is already the
    // window's logical size.
    // The same choice, to the process that can actually make the ENGINE
    // honour it. render-host only selects which backend it hands over;
    // which renderer the engine picks is decided by its own flags, which
    // Process B merges into ClientSettings.
    config.args.push_back("--graphics-mode");
    config.args.push_back(graphics_mode_arg.toStdString());
    config.args.push_back("--follow-dpi");
    config.args.push_back(settings.follow_dpi ? "on" : "off");
    config.args.push_back("--hidpi");
    config.args.push_back(settings.hidpi ? "on" : "off");
    config.args.push_back("--notify-region");
    config.args.push_back(settings.server_region_notification ? "on" : "off");
    config.args.push_back("--close-on-leave");
    config.args.push_back(settings.close_on_leave ? "on" : "off");
    config.args.push_back("--discord-presence");
    config.args.push_back(settings.discord_rich_presence ? "on" : "off");
    config.args.push_back("--smooth-zoom");
    config.args.push_back(settings.smooth_zoom ? "on" : "off");

    if (const QByteArray log_file = qgetenv("STUD_LOG_FILE"); !log_file.isEmpty()) {
        config.extra_env.emplace_back("STUD_LOG_FILE", log_file.toStdString());
    }
    // Test levers that only Process B reads, passed through so trying one
    // is a relaunch rather than a rebuild.
    for (const char* name : {"STUD_FORCE_THEME", "STUD_TLS_TRACE", "STUD_VIDEO_CODECS"}) {
        if (const QByteArray value = qgetenv(name); !value.isEmpty()) {
            config.extra_env.emplace_back(name, value.toStdString());
        }
    }
    config.stdout_fd = stud::logging::passthrough_stdout_fd();
    config.extra_binds = std::move(extra_binds);
    // Real, writable, already-bound (see extra_binds above) cwd for
    // Process B -- see ProcessBConfig::working_directory's own doc
    // comment for the real EROFS hazard this avoids.
    config.working_directory = std::filesystem::path(so_path).parent_path().string();
    // Real diagnostic, always on: before this, launch_process_b() had
    // no way to set any env var at all, so this trace (and every other
    // env-gated diagnostic this project has built) stayed permanently
    // off outside a hand-run bwrap test script -- meaning there was
    // never any real evidence, from a real production launch, of
    // whether libroblox.so ever even attempts
    // dlopen("libvulkan.so.1") at all. Cheap when it never fires
    // (which is the expectation until Phase 6 native-Vulkan work
    // forces it) -- just an env-gated stderr print in the stub, see
    // process-b/render-client/src/vulkan_stub.cpp.
    // Deliberately NOT setting STUD_VULKAN_CALL_TRACE here any more. It
    // was added when the open question was whether libroblox.so ever even
    // dlopen()s libvulkan.so.1, and the comment above said it was "cheap
    // when it never fires (which is the expectation until Phase 6)".
    // Phase 6 happened: Vulkan is the render path, so it fires constantly,
    // and every call pays a getenv, a string compare and a flushed stderr
    // write. Leaving diagnostics armed in a production launch is the
    // mistake this file records twice already (the planted breakpoints, the
    // mid-frame FBO read-back). Set it in the environment when it is
    // actually wanted.

    try {
        stud::bionic_runtime::launch_process_b(config);
    } catch (const std::exception& e) {
        QMessageBox::critical(nullptr, "Stud",
                               QString("Failed to start the real bionic runtime: %1").arg(e.what()));
    }

    // See payload_thread's own doc comment above -- must join, not
    // detach, so this function (and so proceed()'s subsequent quit())
    // can't return before the real handoff actually finishes or its own
    // bounded timeout elapses.
    payload_thread.join();
}

}  // namespace

namespace stud::ui {

// Ends a running session -- the same scan the launcher uses to clear
// leftovers, which is exactly what the tray's "Exit Stud" has to do.
// Outside the anonymous namespace above so the tray can link against it.
void terminate_stud_session() { terminate_stale_processes(); }

}  // namespace stud::ui

int main(int argc, char** argv) {
    // Before Qt, and before anything can make a Vulkan call: see the
    // function's own comment for what loading MangoHud into this process
    // actually does.
    keep_mangohud_out_of_this_process();

    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(":/stud-logo-color.png"));
    // Wayland has no window-icon protocol: a compositor finds an app's icon by
    // matching the surface's app_id against a .desktop file. Qt derives that
    // app_id from the desktop file name, and without this it uses the
    // executable name ("stud-ui"), which matches no entry -- so the taskbar
    // showed a generic placeholder no matter what setWindowIcon() said. The
    // game window already reports the same id via xdg_toplevel_set_app_id().
    QGuiApplication::setDesktopFileName(QStringLiteral(STUD_APP_ID));
    QCoreApplication::setApplicationName(QStringLiteral("Stud"));

    // "--settings" is the real entry point packaging/stud.desktop's
    // "Settings" Desktop Action invokes (right-click the app icon ->
    // Settings, same discoverability convention Sober itself uses --
    // see the engineering notes' M8 writeup). Deliberately bypasses the login
    // gate entirely, not just "skip login if already logged in": GPU
    // selection, the APK path, and the FFlags file are all local
    // configuration with zero dependency on a Roblox session, so
    // requiring auth first was a real bug (caught live) -- a user
    // without a stored session yet still needs to be able to pick their
    // APK before ever logging in.
    // Persist a session cookie the engine produced, then exit.
    //
    // Stud's login happens inside the real Roblox app, so the cookie that
    // proves it is created by the engine -- not by anything Process A
    // does. Nothing ever wrote it back, so a fresh in-app login lived
    // only in that process's memory and every restart fell back to
    // whatever was already in the keyring. That is why logging out
    // anywhere (a web-view panel invalidates the same server-side
    // session) left Stud logged out on every subsequent launch.
    //
    // The value arrives on stdin, never argv: /proc/<pid>/cmdline is
    // readable by anything running as this user and this is a real
    // credential. Only the keyring ever holds it -- never Stud's config,
    // cache, or a log line.
    // Safe-storage helpers, run one-shot by render-host (the only
    // unsandboxed process still alive once a login happens). The secret's
    // NAME is the argument; the VALUE only ever travels on stdin/stdout,
    // because /proc/<pid>/cmdline is readable by anything running as this
    // user. Nothing here logs a value or a prefix of one.
    // One-shot notification helper. The processes that know when
    // something notification-worthy happened -- render-host and Process B
    // -- are not Qt applications and Process B cannot reach the session
    // bus at all from inside its sandbox, so they invoke this the same
    // way render-host already invokes the keyring helper.
    //
    // Always transient and silent: a note about the server you just
    // joined is information for the moment it appears, and a sound plus a
    // permanent entry for every join is noise.
    //
    // No QCoreApplication of its own: main() has already constructed one,
    // and a second instance is a crash. This project has hit that exact
    // trap before in these one-shot modes.
    // Names the country a game server is in, for the join notification.
    //
    // The address comes from Process B, which reads it off the engine's
    // own UDP socket -- the engine's join line names a 10.x UDMUX address
    // that locates nothing. Turning it into a country needs a lookup, and
    // this is the one outbound request Stud makes on a join: what leaves
    // the machine is the Roblox datacenter's own IP, never the user's.
    //
    // A failed or slow lookup produces no notification rather than a
    // wrong one. Bounded, because a join must never wait on this.
    // Game metadata for the Discord presence: name, creator, square
    // thumbnail, join link. Roblox's own public APIs, in three steps
    // because that is how they are shaped -- a place id names a universe,
    // a universe carries the name and creator, and thumbnails are a
    // separate service.
    //
    // Output is one field per line rather than JSON, so render-host needs
    // no parser: name, creator, thumbnail url, join url.
    if (argc > 2 && std::string(argv[1]) == "--game-info") {
        const QString body = QString::fromUtf8(argv[2]);
        const QStringList parts = body.split(' ', Qt::SkipEmptyParts);
        if (parts.isEmpty()) return 1;
        const QString place_id = parts.at(0);
        const QString job_id = parts.size() > 1 ? parts.at(1) : QString();

        QNetworkAccessManager net;
        auto fetch = [&net](const QString& url) -> QJsonDocument {
            QNetworkRequest request{QUrl(url)};
            request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Stud"));
            QNetworkReply* reply = net.get(request);
            QEventLoop loop;
            QTimer::singleShot(5000, &loop, &QEventLoop::quit);
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            loop.exec();
            if (reply->error() != QNetworkReply::NoError) return {};
            return QJsonDocument::fromJson(reply->readAll());
        };

        const QJsonDocument universe_doc =
            fetch("https://apis.roblox.com/universes/v1/places/" + place_id + "/universe");
        const QJsonValue universe_id = universe_doc.object().value("universeId");
        if (universe_id.isUndefined() || universe_id.isNull()) return 1;
        const QString universe = QString::number(universe_id.toVariant().toLongLong());

        const QJsonDocument games_doc =
            fetch("https://games.roblox.com/v1/games?universeIds=" + universe);
        const QJsonArray games = games_doc.object().value("data").toArray();
        if (games.isEmpty()) return 1;
        const QJsonObject game = games.at(0).toObject();
        const QString name = game.value("name").toString();
        const QString creator = game.value("creator").toObject().value("name").toString();

        // The square thumbnail. 512x512 is what Discord wants for a large
        // image, and the API answers with a CDN URL Discord fetches itself
        // -- so nothing has to be uploaded to the application.
        QString thumbnail;
        const QJsonDocument thumb_doc = fetch(
            "https://thumbnails.roblox.com/v1/games/icons?universeIds=" + universe +
            "&size=512x512&format=Png&isCircular=false");
        const QJsonArray thumbs = thumb_doc.object().value("data").toArray();
        if (!thumbs.isEmpty()) {
            thumbnail = thumbs.at(0).toObject().value("imageUrl").toString();
        }

        QString join_url = "roblox://experiences/start?placeId=" + place_id;
        if (!job_id.isEmpty()) join_url += "&gameInstanceId=" + job_id;

        std::printf("%s\n%s\n%s\n%s\n", name.toUtf8().constData(),
                    creator.toUtf8().constData(), thumbnail.toUtf8().constData(),
                    join_url.toUtf8().constData());
        std::fflush(stdout);
        return 0;
    }
    if (argc > 2 && std::string(argv[1]) == "--notify-region") {
        const QString ip = QString::fromUtf8(argv[2]);
        QNetworkAccessManager net;
        QNetworkRequest request(QUrl(
            "http://ip-api.com/json/" + ip + "?fields=status,country,countryCode,city"));
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Stud"));
        QNetworkReply* reply = net.get(request);
        QEventLoop loop;
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        if (reply->error() != QNetworkReply::NoError || !reply->isFinished()) {
            std::fprintf(stderr, "stud: server region lookup failed\n");
            return 1;
        }
        const QJsonObject json = QJsonDocument::fromJson(reply->readAll()).object();
        if (json.value("status").toString() != "success") return 1;
        const QString country = json.value("country").toString();
        const QString code = json.value("countryCode").toString();
        const QString city = json.value("city").toString();
        // A flag emoji is the country's two letters as regional indicator
        // symbols -- U+1F1E6 is 'A' -- so any ISO code becomes one with no
        // table to keep up to date.
        QString flag;
        if (code.size() == 2) {
            for (const QChar c : code.toUpper()) {
                flag.append(QChar::fromUcs4(0x1F1E6 + (c.unicode() - u'A')));
            }
        }
        const QString where = city.isEmpty() ? country : city + ", " + country;
        const bool ok = stud::ui::send_notification(
            QStringLiteral("Server region"),
            flag.isEmpty() ? where : flag + " " + where, QString(),
            {/*transient=*/true, /*suppress_sound=*/true});
        return ok ? 0 : 1;
    }
    if (argc > 3 && std::string(argv[1]) == "--notify") {
        const bool ok = stud::ui::send_notification(QString::fromUtf8(argv[2]),
                                                     QString::fromUtf8(argv[3]), QString(),
                                                     {/*transient=*/true, /*suppress_sound=*/true});
        return ok ? 0 : 1;
    }
    if (argc > 2 && std::string(argv[1]) == "--store-secret") {
        const QString name = QString::fromUtf8(argv[2]);
        std::string value;
        std::getline(std::cin, value);
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
        if (value.empty()) {
            std::fprintf(stderr, "stud: --store-secret: nothing on stdin\n");
            return 2;
        }
        QString error;
        const bool ok = stud::ui::store_secret(name, QString::fromStdString(value), &error);
        std::printf("stud: secret \"%s\" %s (%zu bytes)\n", argv[2],
                    ok ? "stored (encrypted, key in the keyring)" : "NOT stored", value.size());
        if (!ok) std::fprintf(stderr, "stud: keyring write failed: %s\n", error.toUtf8().constData());
        std::fflush(stdout);
        return ok ? 0 : 1;
    }

    if (argc > 2 && std::string(argv[1]) == "--load-secret") {
        const auto value = stud::ui::load_secret(QString::fromUtf8(argv[2]));
        if (!value) {
            std::fprintf(stderr, "stud: --load-secret: no stored secret named \"%s\"\n", argv[2]);
            return 1;
        }
        // stdout is the channel, so this is the one place a value is
        // written -- to a pipe render-host owns, never to a log.
        const QByteArray bytes = value->toUtf8();
        std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
        std::fflush(stdout);
        return 0;
    }

    // Registers this AppImage (or this build) in the user's own
    // ~/.local/share tree, which is what gives its windows an icon and
    // makes roblox:// links from a browser open it. A package installs
    // its own entry system-wide and needs none of this.
    if (argc > 1 && std::string(argv[1]) == "--install-desktop-entry") {
        return stud::ui::install_desktop_entry();
    }

    // An AppImage installs nothing, so nothing ties its windows to an
    // icon and no browser knows it handles roblox:// links -- and there
    // is no obvious place for a user to find that out. Said once, at the
    // one moment it is relevant, rather than made to happen behind their
    // back: writing to someone's desktop tree is their decision.
    if (!qEnvironmentVariable("APPIMAGE").isEmpty()) {
        const QString entry =
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
            QStringLiteral("/applications/") + QStringLiteral(STUD_APP_ID) +
            QStringLiteral(".desktop");
        if (!QFileInfo::exists(entry)) {
            std::printf(
                "stud: no desktop entry yet -- run `%s --install-desktop-entry` for the "
                "taskbar icon and roblox:// links\n",
                qPrintable(qEnvironmentVariable("APPIMAGE")));
            std::fflush(stdout);
        }
    }

    bool settings_only = argc > 1 && std::string(argv[1]) == "--settings";

    std::optional<stud::ui::LaunchUri> launch_uri;
    if (!settings_only && argc > 1) {
        launch_uri = stud::ui::parse_launch_uri(argv[1]);
        // What the browser actually handed over, and what came out of it.
        // A deep link that parses to nothing is indistinguishable from no
        // deep link at all once it reaches the engine -- both open the
        // home screen -- so this is the only place the difference is
        // visible.
        //
        // Key NAMES and byte counts only. The gameinfo field is a
        // single-use join ticket and the launcher URL's query carries the
        // same; neither belongs in a log file.
        const std::string raw = argv[1];
        std::string keys;
        for (size_t i = 0; i < raw.size();) {
            const size_t plus = raw.find('+', i);
            const std::string seg = raw.substr(i, plus == std::string::npos ? plus : plus - i);
            const size_t colon = seg.find(':');
            if (colon != std::string::npos) {
                if (!keys.empty()) keys += ", ";
                keys += seg.substr(0, colon) + "(" + std::to_string(seg.size() - colon - 1) + "B)";
            }
            if (plus == std::string::npos) break;
            i = plus + 1;
        }
        std::printf("stud: launch argument: %zu bytes, keys=[%s]\n", raw.size(), keys.c_str());
        if (launch_uri) {
            std::printf("stud: parsed deep link: placeId=%lld launchMode=\"%s\" gameinfo=%zuB "
                        "placeLauncherUrl=%zuB joinAttemptId=%s\n",
                        static_cast<long long>(launch_uri->place_id),
                        launch_uri->launch_mode.c_str(), launch_uri->game_info.size(),
                        launch_uri->place_launcher_url.size(),
                        launch_uri->join_attempt_id.empty() ? "absent" : "present");
        } else {
            std::printf("stud: the launch argument is not a Roblox deep link\n");
        }
        std::fflush(stdout);
    }

    auto showSettings = std::make_shared<std::unique_ptr<stud::ui::SettingsWindow>>();
    if (settings_only) {
        *showSettings = std::make_unique<stud::ui::SettingsWindow>();
        (*showSettings)->show();
        return app.exec();
    }

    // Nothing to launch yet: open Settings instead of refusing.
    //
    // Stud does not ship Roblox -- the user supplies the APK -- so on a
    // first run there is nothing for a launch to do. It used to answer a
    // plain click with a dialog saying to "open Stud normally and select
    // one in Settings", which is precisely what had just been done, and
    // from a freshly downloaded AppImage there was no other way in at
    // all: no desktop entry yet, so no Settings action, and the tray only
    // appears once a launch has already succeeded.
    //
    // This is not the bare-launch fallback the comment below rules out.
    // That one is about a launch that CAN happen going to the home screen
    // rather than to Settings, which is still exactly what it does.
    {
        bool have_apk = false;
        try {
            const auto configured = stud::config::load_settings(stud::config::default_config_path());
            have_apk = !configured.apk_path.empty() &&
                       QFileInfo::exists(QString::fromStdString(configured.apk_path));
        } catch (const stud::config::SettingsError&) {
            // No readable config at all is the same answer: nothing configured.
        }
        if (!have_apk) {
            *showSettings = std::make_unique<stud::ui::SettingsWindow>();
            (*showSettings)->setStatusMessage(
                QStringLiteral("Select your Roblox APK to finish setting Stud up. "
                               "Stud does not download or include Roblox itself."));
            (*showSettings)->show();
            return app.exec();
        }
    }

    // Real Android Roblox behavior (user correction: Roblox is a real
    // mobile app with its own real UI, not a URI-activated launcher --
    // opening it plain, with no deep link, opens straight into its own
    // real home/game-picker screen, rendered by libroblox.so itself the
    // same as any other screen; a deep link just pre-fills which
    // activity/game it jumps to, the same as any real Android intent
    // extra). So a bare Stud launch (icon click, no args) boots the
    // real engine exactly like a deep-link launch does -- `launch_uri`
    // being absent just means Roblox's own UI opens on its own default
    // screen instead of a specific game. SettingsWindow is reached ONLY
    // via the desktop file's separate "Settings" action (--settings),
    // never as a bare-launch fallback.
    const bool tray_wanted = [] {
        try {
            return stud::config::load_settings(stud::config::default_config_path()).system_tray;
        } catch (const std::exception&) {
            return true;
        }
    }();
    auto proceed = [launch_uri, tray_wanted]() {
        launch_game(launch_uri);
        // Real magnet-link behavior: hand off and get out of the way,
        // same as qBittorrent doesn't need to stay open once a
        // torrent's been added -- the actual game runs in the
        // separate runtime process.
        //
        // Real, live-caught bug fixed: a direct QApplication::quit()
        // call here is a documented Qt no-op if there's no event loop
        // running yet to quit -- which is exactly the case on the
        // already-logged-in path below (proceed() runs synchronously in
        // main(), before app.exec() is ever reached). The quit request
        // was silently swallowed, then app.exec() started a real loop
        // with nothing left to ever stop it -- confirmed live: stud-ui
        // stayed running indefinitely after a bare, already-logged-in
        // launch. QueuedConnection posts a real event that's delivered
        // once the loop actually starts, whether that's before or after
        // this call.
        // ... unless there is a tray to hold. A tray icon has to belong to
        // a process that lives as long as the session, and of the three
        // only this one is a Qt application: render-host owns the window
        // and Process B is sandboxed. With the tray on, this process stays
        // for the session doing nothing else; with it off, the hand-off
        // behaviour above is unchanged.
        //
        // Closing the window still quits Stud either way. The tray is a
        // menu, not somewhere to hide a running game.
        if (tray_wanted) {
            auto* tray = new stud::ui::Tray(qApp);
            if (tray->show()) return;
            std::fprintf(stderr, "stud: no system tray available -- exiting after launch\n");
        }
        QMetaObject::invokeMethod(qApp, &QApplication::quit, Qt::QueuedConnection);
    };

    // No login gate. Stud logs in the way Sober does: inside the real
    // Roblox app itself, on its own login screen, rendered by the real
    // engine -- not through a separate QtWebEngine window that scrapes a
    // session cookie out of the web site. The engine persists that
    // session through Stud's own local-storage platform protocol
    // (runtime: LocalStoragePlatformStub, backed by a real 0600 file
    // under the user's data directory), so a login made once survives
    // restarts exactly as it does on a real device.
    //
    // A cookie stored by an older build is still honoured if present, so
    // upgrading does not silently sign the user out -- but nothing here
    // ever asks for one again.
    proceed();

    return app.exec();
}
