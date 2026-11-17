// Stud's launcher: the one Qt process. Stud 2.0 runs Roblox's macOS client on
// Rosemary, as a second process this one starts and stays with for the whole
// session, so there is always a process left to report a crash.
//
// Two entry points into this binary:
//  - a plain launch (desktop icon, no arguments) opens Roblox on its own home
//    screen;
//  - a deep link (`stud roblox-player:...` / `roblox://...`), which the browser
//    hands over when Play is clicked on roblox.com, goes to the client as its
//    own -protocolString. A link clicked while Roblox is already running is
//    handed to the running client by Rosemary itself.
//
// The client is never modified. The only file Stud writes inside it is
// ClientAppSettings.json (mac_client.h).

#include "desktop_entry.h"
#include "chooser.h"
#include "cube_gl.h"
#include "cpu_cores.h"
#include "diagnose.h"
#include "flatpak_permissions.h"
#include "game_activity.h"
#include "gpu_enum.h"
#include "launch_uri.h"
#include "loading_screen.h"
#include "log_scrub.h"
#include "mac_client.h"
#include "customization.h"
#include "notifications.h"
#include "settings_window.h"
#include "stud1_cleanup.h"
#include "tray.h"
#include "webview_host.h"
#include "stud/session_log.h"
#include "stud/settings.h"
#include "stud/stud_paths.h"

#include <QApplication>
#include <QClipboard>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QUrl>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMessageBox>
#include <QPixmap>
#include <QProcess>
#include <QSaveFile>
#include <QRegularExpression>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <sched.h>
#include <sys/stat.h>
#include <grp.h>
#include <unistd.h>

namespace stud::ui {
void set_tray_is_holding(bool holding);
bool crash_dialog_is_open();
bool session_ended_cleanly();
void terminate_stud_session();
void show_crash_dialog(const QString& which_process);
}  // namespace stud::ui

namespace {

// The client, while it runs. Owned for the life of this process: a QProcess
// that is destroyed kills its child, and this process outlives the session
// anyway.
QProcess* g_client = nullptr;
// The session was ended on purpose (the tray's Exit, Settings putting it
// back, the loading screen's Cancel): its exit status is no crash.
bool g_stop_requested = false;

enum class SessionEnd { Running, Clean, Crashed };
SessionEnd g_session_end = SessionEnd::Running;
bool g_tray_is_holding = false;
bool g_crash_dialog_open = false;
bool g_relaunch_pending = false;

// The client's output on its way to the session log, a line at a time. The
// deep link is a single-use join ticket and the log is made to be shared, so
// neither it nor a ticket the client prints gets written.
QByteArray g_deep_link;
// Discord presence and the server region notification, fed the lines they
// need.
stud::ui::GameActivity* g_activity = nullptr;
// What this session runs, so a restart opens the same one.
stud::ui::client::Product g_product = stud::ui::client::Product::Player;
// The write end of the client's output pipe, handed to it as stdout and
// stderr; closed here once it has started.
int g_output_write = -1;
// The Player got as far as starting its app. One that exits, even with 0,
// before it did never opened its window: that is a failed start, not a quit.
std::atomic<bool> g_client_app_started{false};

void forward_line(QByteArray line) {
    if (!g_client_app_started.load(std::memory_order_relaxed) &&
        line.contains("[FLog::SingleSurfaceApp] initializeWithAppStarter")) {
        g_client_app_started.store(true, std::memory_order_relaxed);
    }
    // Studio's presence follows its log files (GameActivity::Options::studio_logs).
    if (g_activity != nullptr && g_product == stud::ui::client::Product::Player &&
        stud::ui::GameActivity::wants(line)) {
        QMetaObject::invokeMethod(qApp, [line] {
            if (g_activity != nullptr) g_activity->line(line);
        }, Qt::QueuedConnection);
    }
    if (!g_deep_link.isEmpty()) line.replace(g_deep_link, "<deep link>");
    const std::string scrubbed =
        stud::ui::scrub_client_log_line(std::string_view(line.constData(), line.size()));
    std::fwrite(scrubbed.data(), 1, scrubbed.size(), stdout);
    std::fputc('\n', stdout);
}

// The client writes its log as it runs, from whichever of its threads has
// something to say, and a write into a full pipe waits. So the pipe is as
// large as the system allows, and a thread of its own drains it: the
// interface thread being busy never holds up a frame of the client's.
// False when the pipe cannot be made; the client then writes to this
// process's own stdout, unscrubbed by nothing but that.
bool start_output_reader() {
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) return false;
    if (std::ifstream max_size{"/proc/sys/fs/pipe-max-size"}) {
        int bytes = 0;
        if (max_size >> bytes && bytes > 0) ::fcntl(fds[0], F_SETPIPE_SZ, bytes);
    }
    g_output_write = fds[1];
    std::thread([read_fd = fds[0]] {
        QByteArray pending;
        char buffer[1 << 16];
        for (;;) {
            const ssize_t got = ::read(read_fd, buffer, sizeof(buffer));
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) break;
            pending.append(buffer, got);
            qsizetype start = 0;
            for (qsizetype end; (end = pending.indexOf('\n', start)) >= 0; start = end + 1) {
                forward_line(pending.mid(start, end - start));
            }
            pending.remove(0, start);
            std::fflush(stdout);
        }
        if (!pending.isEmpty()) forward_line(pending);
        std::fflush(stdout);
        ::close(read_fd);
    }).detach();
    return true;
}

// One Stud at a time, by a lock this process holds for the whole session.
// The kernel drops it when the holder dies however it dies, so it is never
// stale. O_CLOEXEC keeps it out of the client: a process Rosemary leaves
// behind for a while (its idle process server) must not keep it.
int g_lock_fd = -1;

std::string lock_path() { return stud::paths::runtime_dir() + "/stud.lock"; }

bool another_instance_is_running() {
    if (g_lock_fd >= 0) return false;  // this process holds it
    std::error_code ec;
    std::filesystem::create_directories(stud::paths::runtime_dir(), ec);
    const int fd = ::open(lock_path().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return false;  // cannot tell: let the launch proceed
    const bool free_to_take = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
    if (free_to_take) ::flock(fd, LOCK_UN);
    ::close(fd);
    return !free_to_take;
}

bool take_instance_lock() {
    const int fd = ::open(lock_path().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return true;  // nothing to hold, nothing to refuse
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    g_lock_fd = fd;
    return true;
}

// MangoHud belongs in the client, not here: this process uses Vulkan only to
// list the GPUs, and an inherited MANGOHUD=1 loaded the layer into a Qt
// application with no frames (it crashed in MangoHud's NVIDIA sampler). Turned
// off by name for this process only; the user's own value goes back to the
// client untouched.
QString g_host_layers_disable;
bool g_layers_disable_was_set = false;

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

// Variables that make a process load code it did not ask for. None of them
// reaches the client unless Stud sets it itself: Stud never puts anything
// into Roblox (the same rule as never modifying it).
bool loads_code(const QString& name) {
    static const QStringList kNames = {
        QStringLiteral("VK_LAYER_PATH"), QStringLiteral("VK_ADD_LAYER_PATH"),
        QStringLiteral("VK_IMPLICIT_LAYER_PATH"), QStringLiteral("VK_ADD_IMPLICIT_LAYER_PATH"),
        QStringLiteral("VK_INSTANCE_LAYERS"), QStringLiteral("VK_LOADER_LAYERS_ENABLE"),
        QStringLiteral("VK_ICD_FILENAMES"), QStringLiteral("VK_DRIVER_FILES"),
        QStringLiteral("VK_ADD_DRIVER_FILES"), QStringLiteral("LIBGL_DRIVERS_PATH"),
        QStringLiteral("__EGL_VENDOR_LIBRARY_FILENAMES"),
        QStringLiteral("__EGL_VENDOR_LIBRARY_DIRS"), QStringLiteral("SDL_DYNAMIC_API"),
    };
    return name.startsWith(QStringLiteral("DYLD_")) || name.startsWith(QStringLiteral("LD_")) ||
           kNames.contains(name);
}

// NVIDIA's Vulkan driver manifest, where its packages install it.
QString nvidia_vulkan_driver() {
    for (const char* path : {"/usr/share/vulkan/icd.d/nvidia_icd.x86_64.json",
                             "/usr/share/vulkan/icd.d/nvidia_icd.json",
                             "/etc/vulkan/icd.d/nvidia_icd.json"}) {
        if (QFileInfo(QString::fromLatin1(path)).isFile()) return QString::fromLatin1(path);
    }
    return {};
}

// Whether the machine has an NVIDIA graphics card, as the kernel lists PCI
// devices: vendor 0x10de, display class 0x03.
bool has_nvidia_card() {
    const QDir devices(QStringLiteral("/sys/bus/pci/devices"));
    for (const QString& device : devices.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile vendor(devices.filePath(device) + QStringLiteral("/vendor"));
        QFile cls(devices.filePath(device) + QStringLiteral("/class"));
        if (vendor.open(QIODevice::ReadOnly) && cls.open(QIODevice::ReadOnly) &&
            vendor.readAll().trimmed() == "0x10de" && cls.readAll().trimmed().startsWith("0x03")) {
            return true;
        }
    }
    return false;
}

// Roblox's own frame rate cap, in its settings file in the prefix: its menu
// stops at 240, so with VSync off it is lifted to the value it reads as no
// cap. Only an element already there is changed; Roblox writes the file.
void lift_roblox_frame_cap() {
    const QDir users(stud::ui::client::prefix_dir() + QStringLiteral("/root/Users"));
    static const QRegularExpression cap(
        QStringLiteral(R"((?<=<int name="FramerateCap">)[^<]*(?=</int>))"));
    for (const QString& user : users.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QDir roblox(users.filePath(user) + QStringLiteral("/Library/Roblox"));
        for (const QString& name :
             roblox.entryList({QStringLiteral("GlobalBasicSettings_*.xml")}, QDir::Files)) {
            QFile in(roblox.filePath(name));
            if (!in.open(QIODevice::ReadOnly)) continue;
            QString text = QString::fromUtf8(in.readAll());
            in.close();
            const QString lifted = QString(text).replace(cap, QStringLiteral("9999"));
            if (lifted == text) continue;
            QSaveFile out(roblox.filePath(name));
            if (out.open(QIODevice::WriteOnly)) {
                out.write(lifted.toUtf8());
                out.commit();
            }
        }
    }
}

// The chosen GPU, for a laptop with two: NVIDIA's PRIME offload for their
// driver, DRI_PRIME for Mesa. Both are the documented way and both are
// ignored on a machine with one GPU.
void apply_gpu_selection(QProcessEnvironment& env, const stud::config::StudSettings& settings) {
    const auto gpus = stud::ui::enumerate_gpus();
    const stud::ui::GpuInfo* chosen = nullptr;
    for (const auto& gpu : gpus) {
        if (gpu.device_index == settings.gpu.device_index) chosen = &gpu;
    }
    if (chosen == nullptr || !chosen->discrete) return;
    constexpr uint32_t kVendorNvidia = 0x10de;
    if (chosen->vendor_id == kVendorNvidia) {
        env.insert(QStringLiteral("__NV_PRIME_RENDER_OFFLOAD"), QStringLiteral("1"));
        env.insert(QStringLiteral("__GLX_VENDOR_LIBRARY_NAME"), QStringLiteral("nvidia"));
        env.insert(QStringLiteral("__VK_LAYER_NV_optimus"), QStringLiteral("NVIDIA_only"));
        // Only NVIDIA's Vulkan driver, so Mesa's are not loaded beside it, and
        // its shader cache kept whole: a pipeline compiled once stays compiled.
        if (const QString icd = nvidia_vulkan_driver(); !icd.isEmpty()) {
            env.insert(QStringLiteral("VK_DRIVER_FILES"), icd);
        }
        env.insert(QStringLiteral("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP"), QStringLiteral("1"));
    } else {
        env.insert(QStringLiteral("DRI_PRIME"), QStringLiteral("1"));
    }
    std::printf("stud: running on \"%s\"\n", chosen->name.c_str());
}

void apply_mangohud(QProcessEnvironment& env, bool enabled) {
    // Undo what keep_mangohud_out_of_this_process() did: that was for Stud's
    // own process.
    if (g_layers_disable_was_set) {
        env.insert(QStringLiteral("VK_LOADER_LAYERS_DISABLE"), g_host_layers_disable);
    } else {
        env.remove(QStringLiteral("VK_LOADER_LAYERS_DISABLE"));
    }
    // MangoHud's picmip rewrites every sampler's LOD bias; -17 is its own
    // "leave the sampler alone", so a config file made for another game does
    // not change how Roblox's textures look. read_cfg keeps that file read.
    if (enabled || env.contains(QStringLiteral("MANGOHUD"))) {
        const QString existing = env.value(QStringLiteral("MANGOHUD_CONFIG"));
        env.insert(QStringLiteral("MANGOHUD_CONFIG"),
                   existing.isEmpty() ? QStringLiteral("read_cfg,picmip=-17")
                                      : existing + QStringLiteral(",picmip=-17"));
        // MangoHud draws over every swapchain it sees, and Studio's panels and
        // dialogs are windows of their own: only the main window gets one.
        env.insert(QStringLiteral("ROSEMARY_SWAPCHAIN_MAIN_WINDOW_ONLY"), QStringLiteral("1"));
    }
    // The client presents with Vulkan, where MangoHud's implicit layer needs
    // nothing more than this.
    if (enabled) env.insert(QStringLiteral("MANGOHUD"), QStringLiteral("1"));
}

// Stud's icon for `product`'s window, as a file Rosemary can read, written
// beside Stud's other runtime files: the red stud for Roblox, the blue one for
// Studio. Empty when it cannot be written.
QString window_icon_file(stud::ui::client::Product product) {
    const bool studio = product == stud::ui::client::Product::Studio;
    const QString path = QString::fromStdString(stud::paths::runtime_dir()) +
                         (studio ? QStringLiteral("/window-icon-studio.png")
                                 : QStringLiteral("/window-icon.png"));
    QFile::remove(path);
    return QFile::copy(studio ? QStringLiteral(":/studblu.png") : QStringLiteral(":/studred.png"), path)
               ? path
               : QString();
}

// Everything the client is started with.
QProcessEnvironment client_environment(const stud::config::StudSettings& settings,
                                       stud::ui::client::Product product) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // The variables Settings decides are Settings' alone; Rosemary's others
    // (ROSEMARY_METAL_STATS and the like) pass through, for investigating.
    static const QStringList kFromSettings = {
        QStringLiteral("ROSEMARY_PREFIX"), QStringLiteral("ROSEMARY_PREFIX_SETUP"),
        QStringLiteral("ROSEMARY_DECORATIONS"), QStringLiteral("ROSEMARY_MENU"),
        QStringLiteral("ROSEMARY_CONTROL_EDIT_KEYS"), QStringLiteral("ROSEMARY_COMMAND_KEY"),
        QStringLiteral("ROSEMARY_SWAP_INTERVAL"),
        QStringLiteral("ROSEMARY_SCREEN_REFRESH_RATE"),
        QStringLiteral("ROSEMARY_METAL_PRESENTATION_MODE"), QStringLiteral("ROSEMARY_WINDOW_TITLE"),
        QStringLiteral("ROSEMARY_WINDOW_ICON"), QStringLiteral("ROSEMARY_STATUS_ITEMS"),
        QStringLiteral("ROSEMARY_BACKGROUND_FPS"), QStringLiteral("ROSEMARY_RENDER_SCALE"),
        QStringLiteral("ROSEMARY_UPSCALER"), QStringLiteral("ROSEMARY_WINDOW_STATE"),
        QStringLiteral("ROSEMARY_SWAPCHAIN_MAIN_WINDOW_ONLY"), QStringLiteral("ROSEMARY_PROC_TRANSLATED"),
        QStringLiteral("ROSEMARY_REFUSED_PROGRAMS"),
    };
    for (const QString& name : env.keys()) {
        if (loads_code(name) || kFromSettings.contains(name)) env.remove(name);
    }
    env.remove(QStringLiteral("STUD_LOG_FILE"));

    env.insert(QStringLiteral("ROSEMARY_PREFIX"), stud::ui::client::prefix_dir());
    env.insert(QStringLiteral("ROSEMARY_PREFIX_SETUP"), QStringLiteral("download"));

    // The window: an application id of its own under Stud's, <id>.Player or
    // <id>.Studio, whose hidden desktop entry gives the taskbar the red stud
    // for Roblox and the blue one for Studio. A Flatpak's compositor accepts
    // it, being an extension of the sandbox's own id.
    const bool studio = product == stud::ui::client::Product::Studio;
    env.insert(QStringLiteral("SDL_APP_ID"),
               QStringLiteral(STUD_APP_ID) + (studio ? QStringLiteral(".Studio") : QStringLiteral(".Player")));
    // The sound, in a mixer: Stud with Stud's icon. Plasma's applet leaves out
    // a stream name with "audio" in it, so it reads just Stud; its settings
    // page, "Stud: Roblox audio". On either of SDL's sound servers; Rosemary
    // takes the name over the bundle's.
    env.insert(QStringLiteral("SDL_APP_NAME"), QStringLiteral("Stud"));
    // An Intel Mac, which is what the client sees of the CPU: not a translated
    // process on Apple silicon. Rosemary says Rosetta for Roblox's installer,
    // which picks its download by it; Stud downloads the client itself.
    env.insert(QStringLiteral("ROSEMARY_PROC_TRANSLATED"), QStringLiteral("0"));
    // Stud installs and updates the client; Roblox's own installer never runs
    // in the prefix. Started by the client or its menu bar helper, it would
    // download and unpack another build (a test channel's) over the one Stud
    // checked. Rosemary refuses it, and the client sees its update fail.
    env.insert(QStringLiteral("ROSEMARY_REFUSED_PROGRAMS"),
               QStringLiteral("RobloxPlayerInstaller:RobloxStudioInstaller"));
    // A smaller render, and what scales it back up as it is presented. Metal
    // only: an OpenGL surface stays the window's size whatever the guest is
    // told, so it would draw into a corner of it.
    if (settings.render_scale < 100 && settings.renderer == "vulkan") {
        env.insert(QStringLiteral("ROSEMARY_RENDER_SCALE"), QString::number(settings.render_scale / 100.0));
        env.insert(QStringLiteral("ROSEMARY_UPSCALER"), QString::fromStdString(settings.upscaler));
    }
    env.insert(QStringLiteral("SDL_AUDIO_DEVICE_STREAM_NAME"), QStringLiteral("Roblox audio"));
    // The mixer shows the app's own stud: the red for Roblox, the blue for
    // Studio, the icons Stud's entry already installs for its two actions.
    env.insert(QStringLiteral("SDL_AUDIO_DEVICE_APP_ICON_NAME"),
               QStringLiteral(STUD_APP_ID) + (studio ? QStringLiteral("-studio") : QStringLiteral("-player")));
    // Wayland wherever there is one. X11 through XWayland only when asked
    // for, and only where XWayland is there to ask.
    if (!env.value(QStringLiteral("WAYLAND_DISPLAY")).isEmpty()) {
        const bool xwayland = !env.value(QStringLiteral("DISPLAY")).isEmpty();
        env.insert(QStringLiteral("SDL_VIDEO_DRIVER"),
                   xwayland && settings.display_backend == "x11" ? QStringLiteral("x11")
                                                                 : QStringLiteral("wayland"));
    }
    // Rosemary draws the macOS title bar itself ("client"), or leaves the
    // window to the desktop's own ("server").
    const bool native_title_bar = settings.title_bar == "native";
    env.insert(QStringLiteral("ROSEMARY_DECORATIONS"),
               native_title_bar ? QStringLiteral("server") : QStringLiteral("client"));
    env.insert(QStringLiteral("ROSEMARY_MENU"), settings.title_bar == "macos"
                                                    ? QStringLiteral("titlebar")
                                                    : QStringLiteral("none"));
    // Stud's name and icon on the window, and no tray icons of the client's
    // own beside Stud's (the switches packaging/rosemary/stud-launcher.patch
    // adds to Rosemary).
    // Studio keeps its own titles: its windows are documents, named by what
    // is open.
    if (product == stud::ui::client::Product::Player) {
        env.insert(QStringLiteral("ROSEMARY_WINDOW_TITLE"), QStringLiteral("Stud"));
    }
    if (const QString icon = window_icon_file(product); !icon.isEmpty()) {
        env.insert(QStringLiteral("ROSEMARY_WINDOW_ICON"), icon);
    }
    env.insert(QStringLiteral("ROSEMARY_STATUS_ITEMS"), QStringLiteral("0"));
    // The window opens as it was left: its size, and maximized or fullscreen.
    {
        const QString state = QString::fromStdString(stud::paths::state_dir());
        QDir().mkpath(state);
        env.insert(QStringLiteral("ROSEMARY_WINDOW_STATE"),
                   state + (product == stud::ui::client::Product::Studio ? QStringLiteral("/window-studio")
                                                                         : QStringLiteral("/window")));
    }
    // The client's web views are QtWebEngine pages drawn inside its own
    // window: Rosemary starts this program again as the web view helper, in
    // a compositor of its own. STUD_WEBVIEW_WINDOWS=1 shows them as windows of
    // this process on the desktop instead, as before.
    if (env.value(QStringLiteral("STUD_WEBVIEW_WINDOWS")) == QLatin1String("1")) {
        if (const QString socket = stud::ui::webview::listen(); !socket.isEmpty()) {
            env.insert(QStringLiteral("STUD_WEBVIEW_SOCKET"), socket);
        }
    } else {
        // In the AppImage, its launcher: the bare binary finds none of the
        // libraries the AppImage carries.
        const QString appdir = qEnvironmentVariable("APPDIR");
        const QString launcher = appdir + QStringLiteral("/bin/stud");
        env.insert(QStringLiteral("STUD_WEBVIEW_HELPER"),
                   !appdir.isEmpty() && QFileInfo(launcher).isExecutable() ? launcher
                                                                          : QCoreApplication::applicationFilePath());
        env.remove(QStringLiteral("STUD_WEBVIEW_SOCKET"));
    }
    if (settings.background_fps != stud::config::kBackgroundFpsNoLimit) {
        env.insert(QStringLiteral("ROSEMARY_BACKGROUND_FPS"),
                   QString::number(settings.background_fps));
    }

    // Ctrl where the macOS client expects Command, which Linux desktops keep
    // for themselves. The Player only for copy, cut, paste and select-all, so
    // Ctrl stays Ctrl in games; Studio in every shortcut, as Studio on Windows
    // has it (Ctrl+S saves, Ctrl+1 to 4 pick tools, Ctrl-click adds to a
    // selection), with Super taking Control's place.
    if (product == stud::ui::client::Product::Studio) {
        env.insert(QStringLiteral("ROSEMARY_COMMAND_KEY"), QStringLiteral("ctrl"));
    } else {
        env.insert(QStringLiteral("ROSEMARY_CONTROL_EDIT_KEYS"), QStringLiteral("1"));
    }

    // Presentation: on Vulkan the presentation mode, on OpenGL the swap
    // interval (mailbox has no OpenGL equivalent, and is immediate there).
    // Not waiting for the display means uncapped: the client paces itself to
    // the refresh rate it is told, so it is told one no display has.
    if (!stud::config::waits_for_refresh(settings)) {
        env.insert(QStringLiteral("ROSEMARY_SCREEN_REFRESH_RATE"), QStringLiteral("9999"));
    }
    if (settings.renderer == "opengl") {
        if (settings.presentation == "adaptive") {
            env.insert(QStringLiteral("ROSEMARY_SWAP_INTERVAL"), QStringLiteral("-1"));
        } else if (!stud::config::waits_for_refresh(settings)) {
            env.insert(QStringLiteral("ROSEMARY_SWAP_INTERVAL"), QStringLiteral("0"));
        }
    } else {
        const QString mode = settings.presentation == "vsync"      ? QStringLiteral("fifo")
                             : settings.presentation == "adaptive" ? QStringLiteral("fifo_relaxed")
                                                                   : QString::fromStdString(settings.presentation);
        env.insert(QStringLiteral("ROSEMARY_METAL_PRESENTATION_MODE"), mode);
    }

    apply_gpu_selection(env, settings);
    apply_mangohud(env, settings.mangohud);
    // Settings' own variables, last, so each wins over Stud's.
    for (const QString& line : QString::fromStdString(settings.environment).split(QLatin1Char('\n'))) {
        const QString entry = line.trimmed();
        const qsizetype equals = entry.indexOf(QLatin1Char('='));
        if (entry.isEmpty() || entry.startsWith(QLatin1Char('#')) || equals <= 0) continue;
        env.insert(entry.left(equals).trimmed(), entry.mid(equals + 1));
    }
    return env;
}

// The FastFlags of Settings' FFlags tab (flags.json), written as the
// client's ClientAppSettings.json. A malformed flags.json is reported and
// left out rather than stopping the launch.
QJsonObject client_flags(const stud::config::StudSettings& settings,
                         stud::ui::client::Product product = stud::ui::client::Product::Player) {
    QJsonObject flags;
    // The Player's from the FFlags tab, Studio's from the Studio tab.
    QFile file(QString::fromStdString(product == stud::ui::client::Product::Studio
                                          ? stud::config::default_studio_flags_path()
                                          : stud::config::default_flag_overrides_path()));
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray text = file.readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(text);
        if (doc.isObject()) {
            flags = doc.object();
        } else if (!text.trimmed().isEmpty()) {  // an empty file is no flags
            std::fprintf(stderr, "stud: %s is not a JSON object; its flags are not used\n",
                         qPrintable(file.fileName()));
        }
    }
    // The renderer is Settings' choice, over anything flags.json says about it.
    flags.remove(QStringLiteral("FFlagDebugGraphicsPreferOpenGL"));
    if (settings.renderer == "opengl") {
        flags.insert(QStringLiteral("FFlagDebugGraphicsPreferOpenGL"), true);
    }
    return flags;
}

// GameMode raises the CPU governor through a helper polkit lets only members
// of the "gamemode" group run. Without that, GameMode still says it is on and
// the CPU stays in its power-saving profile: on a laptop that halved Roblox's
// frame rate. Ask polkit, without prompting, whether this user may run the
// helper, and say so once per run if not.
void warn_if_gamemode_cannot_raise_cpu() {
    static bool warned = false;
    if (warned) return;
    auto* check = new QProcess(qApp);
    QObject::connect(check, &QProcess::finished, check, [check](int code, QProcess::ExitStatus status) {
        check->deleteLater();
        // pkcheck: 0 authorized, 1 not authorized, 2 authorization needs a
        // password. Anything else, or a crash, says nothing about GameMode.
        if (status != QProcess::NormalExit || (code != 1 && code != 2) || warned) return;
        warned = true;
        std::printf("stud: GameMode cannot change the CPU governor (not in the gamemode group)\n");
        stud::ui::send_notification(
            QStringLiteral("GameMode can't speed up the CPU"),
            QStringLiteral("You're not in the gamemode group, so the CPU stays in power-saving mode "
                           "and Roblox runs slower. Run \"sudo usermod -aG gamemode $USER\", "
                           "then log out and back in."));
    });
    QObject::connect(check, &QProcess::errorOccurred, check, [check](QProcess::ProcessError error) {
        // No pkcheck: no polkit to ask, and nothing to warn about.
        if (error == QProcess::FailedToStart) check->deleteLater();
    });
    check->start(QStringLiteral("pkcheck"),
                 {QStringLiteral("--action-id"),
                  QStringLiteral("com.feralinteractive.GameMode.governor-helper"),
                  QStringLiteral("--process"), QString::number(QCoreApplication::applicationPid())});
}

// GameMode as the Flatpak sandbox reaches it: through the desktop portal,
// which needs no permission and hands the request to GameMode on the host.
// Calling GameMode's own name from inside the sandbox fails (ServiceUnknown),
// and the session went without it. The process is named by pidfds, which
// stay right across the sandbox's pid namespace; RegisterGame with a bare pid
// only where pidfds are unavailable.
QDBusReply<int> register_through_portal(qint64 pid) {
    QDBusInterface portal("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                          "org.freedesktop.portal.GameMode", QDBusConnection::sessionBus());
    const int target = static_cast<int>(::syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0));
    const int self = static_cast<int>(::syscall(SYS_pidfd_open, ::getpid(), 0));
    QDBusReply<int> reply;
    if (target >= 0 && self >= 0) {
        // QDBusUnixFileDescriptor keeps a duplicate, so these are closed here.
        reply = portal.call("RegisterGameByPIDFd", QVariant::fromValue(QDBusUnixFileDescriptor(target)),
                            QVariant::fromValue(QDBusUnixFileDescriptor(self)));
    } else {
        reply = portal.call("RegisterGame", static_cast<int>(pid));
    }
    if (target >= 0) ::close(target);
    if (self >= 0) ::close(self);
    return reply;
}

// The client's main thread, raised as far as RealtimeKit lets a user process:
// it runs Roblox's frame and sets the frame rate, while the same process's
// workers and Rosemary's own threads would otherwise get an equal share of a
// busy core. RealtimeKit (on the system bus), or in the Flatpak the desktop
// portal's Realtime interface, does it without root; the level is the one
// they report, MinNiceLevel. The main thread is the process's first, so its
// thread id is the process id.
void raise_main_thread_priority(qint64 pid) {
    const bool sandboxed = QFileInfo::exists(QStringLiteral("/.flatpak-info"));
    QDBusInterface realtime(
        sandboxed ? QStringLiteral("org.freedesktop.portal.Desktop") : QStringLiteral("org.freedesktop.RealtimeKit1"),
        sandboxed ? QStringLiteral("/org/freedesktop/portal/desktop") : QStringLiteral("/org/freedesktop/RealtimeKit1"),
        sandboxed ? QStringLiteral("org.freedesktop.portal.Realtime") : QStringLiteral("org.freedesktop.RealtimeKit1"),
        sandboxed ? QDBusConnection::sessionBus() : QDBusConnection::systemBus());
    const QVariant floor = realtime.property("MinNiceLevel");
    if (!floor.isValid()) {
        std::printf("stud: RealtimeKit is not available; Roblox's main thread keeps its priority\n");
        return;
    }
    const QDBusMessage reply = realtime.call(QStringLiteral("MakeThreadHighPriorityWithPID"),
                                             static_cast<qulonglong>(pid), static_cast<qulonglong>(pid),
                                             floor.toInt());
    if (reply.type() == QDBusMessage::ErrorMessage) {
        std::printf("stud: RealtimeKit refused Roblox's main thread: %s\n",
                    reply.errorMessage().toLocal8Bit().constData());
    } else {
        std::printf("stud: Roblox's main thread runs at priority %d\n", floor.toInt());
    }
}

// What the kernel offers this account to draw with.
struct KernelGpu {
    bool usable = false;  // a render node it can open, or NVIDIA's own device
    QString blocked;      // otherwise, a render node it cannot open
    QString group;        // and the group that owns it
};

KernelGpu kernel_gpu() {
    KernelGpu gpu;
    if (QFileInfo::exists(QStringLiteral("/dev/nvidiactl"))) {
        gpu.usable = true;
        return gpu;
    }
    const QDir dri(QStringLiteral("/dev/dri"));
    for (const QString& name : dri.entryList({QStringLiteral("renderD*")}, QDir::System)) {
        const QByteArray path = QFile::encodeName(dri.filePath(name));
        const int fd = ::open(path.constData(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            ::close(fd);
            gpu.usable = true;
            return gpu;
        }
        if (errno == EACCES && gpu.blocked.isEmpty()) {
            gpu.blocked = dri.filePath(name);
            struct stat st {};
            if (::stat(path.constData(), &st) == 0) {
                if (const group* owner = ::getgrgid(st.st_gid)) gpu.group = QString::fromLocal8Bit(owner->gr_name);
            }
        }
    }
    return gpu;
}

// What to tell someone whose account cannot open the GPU it has.
QString no_gpu_access_reason(const KernelGpu& gpu) {
    QString reason = QStringLiteral("Roblox needs the graphics card at %1, and your account cannot open it")
                         .arg(gpu.blocked);
    if (gpu.group.isEmpty()) return reason + QStringLiteral(". Without it, Roblox only shows an empty window.");
    reason += QStringLiteral(": it belongs to the group \"%1\", which you are not in. Without it, Roblox only "
                             "shows an empty window.\n\nJoin the group with\n\n    sudo usermod -aG %1 $USER\n\n"
                             "then sign out and back in")
                  .arg(gpu.group);
    if (QFileInfo::exists(QStringLiteral("/opt/google/cros-containers"))) {
        reason += QStringLiteral(" (on a Chromebook, restart Linux: right-click the Terminal app and choose "
                                 "Shut down Linux)");
    }
    return reason + QStringLiteral(", and start Stud again.");
}

// What to tell someone whose system has no GPU for Roblox to draw with.
QString no_gpu_reason() {
    QString reason = QStringLiteral(
        "Roblox needs a graphics card, and this Linux system has none it can use: Vulkan finds no "
        "device, and there is nothing in /dev/dri. Without one, Roblox only shows an empty window.");
    // ChromeOS's Linux container, which gets the GPU only when asked to.
    if (QFileInfo::exists(QStringLiteral("/opt/google/cros-containers"))) {
        reason += QStringLiteral(
            "\n\nOn a Chromebook, turn on GPU acceleration for Linux: open "
            "chrome://flags/#crostini-gpu-support in Chrome, enable it, restart the Chromebook, "
            "and start Stud again.");
    } else {
        reason += QStringLiteral(
            "\n\nCheck that your graphics driver is installed. In a virtual machine, turn on its 3D "
            "acceleration.");
    }
    return reason;
}

// Hands the client to Feral GameMode, which lets go when it exits.
void register_with_gamemode(qint64 pid) {
    const bool sandboxed = QFileInfo::exists(QStringLiteral("/.flatpak-info"));
    QDBusReply<int> reply;
    if (sandboxed) {
        reply = register_through_portal(pid);
    } else {
        QDBusInterface gamemode("com.feralinteractive.GameMode", "/com/feralinteractive/GameMode",
                                "com.feralinteractive.GameMode", QDBusConnection::sessionBus());
        reply = gamemode.call("RegisterGame", static_cast<int>(pid));
    }
    if (!reply.isValid()) {
        std::printf("stud: GameMode is not available: %s\n",
                    reply.error().message().toLocal8Bit().constData());
    } else if (reply.value() != 0) {
        std::printf("stud: GameMode refused the session (%d)\n", reply.value());
    } else {
        std::printf("stud: GameMode is on for this session%s\n", sandboxed ? " (through the portal)" : "");
        // Polkit is the host's: pkcheck cannot ask it from inside the sandbox.
        if (!sandboxed) warn_if_gamemode_cannot_raise_cpu();
    }
}

// One log file per session under ~/.local/state/stud/logs, holding this
// process's output and the client's (it inherits the teed stdout). The last
// ten sessions are kept.
void start_session_log() {
    if (!qEnvironmentVariable("STUD_LOG_FILE").isEmpty()) return;
    QDir log_dir(QString::fromStdString(stud::paths::log_dir()));
    log_dir.mkpath(QStringLiteral("."));
    constexpr int kSessionsKept = 10;
    const QStringList old =
        log_dir.entryList({QStringLiteral("session-*.log")}, QDir::Files, QDir::Name);
    for (int i = 0; i < old.size() - (kSessionsKept - 1); ++i) {
        QFile::remove(log_dir.filePath(old.at(i)));
    }
    const QString path = log_dir.filePath(
        QStringLiteral("session-%1.log")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"))));
    qputenv("STUD_LOG_FILE", path.toUtf8());
    stud::logging::start_session_log(path.toStdString());
    std::printf("stud: session log %s\n", qPrintable(path));
}

void open_settings_for_launch_failure(const QString& reason) {
    stud::ui::close_loading_screen();
    stud::ui::SettingsWindow::listenForOpenRequests();
    stud::ui::SettingsWindow::showSingleton(reason);
}

std::vector<pid_t> client_process_ids();

// Hands `link` to the running client of `product`: Rosemary gives a URL
// named with the bundle to the instance already open, as macOS does. False
// when there is no client to hand it to.
bool hand_link_to_client(stud::ui::client::Product product, const QString& link,
                         const stud::config::StudSettings& settings) {
    const auto build = stud::ui::client::installed(product);
    const QString rosemary = stud::ui::client::find_rosemary();
    if (!build || rosemary.isEmpty()) return false;
    QProcess handoff;
    handoff.setProgram(rosemary);
    // A place file goes as a file:// URL, which Rosemary delivers as a
    // document to open.
    handoff.setArguments({QStringLiteral("run"), build->app,
                          link.startsWith(QLatin1Char('/')) ? QUrl::fromLocalFile(link).toString() : link});
    handoff.setProcessEnvironment(client_environment(settings, product));
    handoff.setStandardOutputFile(QProcess::nullDevice());
    handoff.setStandardErrorFile(QProcess::nullDevice());
    return handoff.startDetached();
}

// Deletes files older than `days` from Roblox's logs and caches in the
// prefix and from Stud's own logs.
void clean_old_files(int days) {
    if (days <= 0) return;
    const QDateTime before = QDateTime::currentDateTime().addDays(-days);
    QStringList dirs{QString::fromStdString(stud::paths::log_dir()),
                     stud::ui::client::prefix_dir() + QStringLiteral("/root/tmp/Roblox")};
    const QDir users(stud::ui::client::prefix_dir() + QStringLiteral("/root/Users"));
    for (const QString& user : users.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        dirs << users.filePath(user) + QStringLiteral("/Library/Logs/Roblox")
             << users.filePath(user) + QStringLiteral("/Library/Caches/com.roblox.RobloxPlayer");
    }
    int removed = 0;
    for (const QString& dir : dirs) {
        QDirIterator it(dir, QDir::Files | QDir::NoSymLinks | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString file = it.next();
            if (QFileInfo(file).lastModified() < before && QFile::remove(file)) ++removed;
        }
    }
    if (removed > 0) std::printf("stud: cleaner removed %d old file(s)\n", removed);
}

// A place file (.rbxl, .rbxlx) opened from the file manager, as a path or a
// file:// URL, as the path Studio's guest sees it by. Nothing for anything
// else.
std::optional<QString> place_file(const char* argument) {
    const QString given = QString::fromLocal8Bit(argument);
    const QFileInfo info(given.startsWith(QLatin1String("file:")) ? QUrl(given).toLocalFile() : given);
    const QString suffix = info.suffix().toLower();
    if ((suffix != QLatin1String("rbxl") && suffix != QLatin1String("rbxlx")) || !info.isFile()) {
        return std::nullopt;
    }
    return QStringLiteral("/Volumes/Host") + info.absoluteFilePath();
}

// In a Flatpak whose permissions were taken away (Flatseal, flatpak
// override), names what is missing and the command that grants it back,
// which runs on the host: nothing inside the sandbox can widen it.
void check_flatpak_permissions() {
    QFile info(QStringLiteral("/.flatpak-info"));
    if (!info.open(QIODevice::ReadOnly)) return;
    const QStringList missing = stud::ui::missing_flatpak_permissions(QString::fromUtf8(info.readAll()));
    if (missing.isEmpty()) return;
    const QString command = QStringLiteral("flatpak override --user %1 %2")
                                .arg(missing.join(QLatin1Char(' ')), qEnvironmentVariable("FLATPAK_ID"));
    std::fprintf(stderr, "stud: the Flatpak is missing %s\n", qPrintable(missing.join(QLatin1Char(' '))));
    QMessageBox box(QMessageBox::Warning, QStringLiteral("Stud"),
                    QStringLiteral("Stud's Flatpak is missing permissions Roblox needs: %1.")
                        .arg(missing.join(QStringLiteral(", "))));
    box.setInformativeText(QStringLiteral("Run this in a terminal, then start Stud again:\n\n") + command);
    box.setTextInteractionFlags(Qt::TextSelectableByMouse);
    QPushButton* copy = box.addButton(QStringLiteral("Copy command"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Close);
    box.exec();
    if (box.clickedButton() == copy) QGuiApplication::clipboard()->setText(command);
}

// Starts the client. False when it could not start; the reason has been put
// on screen.
bool launch_game(stud::ui::client::Product product, const std::optional<QString>& deep_link) {
    g_product = product;
    const bool player = product == stud::ui::client::Product::Player;
    stud::config::StudSettings settings;
    try {
        settings = stud::config::load_settings(stud::config::default_config_path());
    } catch (const stud::config::SettingsError& e) {
        std::fprintf(stderr, "%s; launching with default settings\n", e.what());
    }
    settings.gpu.device_index = stud::ui::chosen_gpu_index(settings.gpu);

    // A link while Stud runs (a game clicked on roblox.com, Studio's sign-in
    // coming back) goes to the client already open.
    if (deep_link && another_instance_is_running() && hand_link_to_client(product, *deep_link, settings)) {
        QMetaObject::invokeMethod(qApp, &QApplication::quit, Qt::QueuedConnection);
        return false;
    }
    if (another_instance_is_running() || !take_instance_lock()) {
        QMessageBox::information(nullptr, "Stud",
                                 "Stud is already running.\n\nClose the existing window before "
                                 "starting it again.");
        return false;
    }

    start_session_log();
    stud::ui::write_diagnostics();

    // Roblox draws on the GPU. With no Vulkan device its Metal renderer cannot
    // start; with no GPU at all, Mesa's software fallback cannot compile
    // Roblox's shaders, and its window stays empty with nothing to say why.
    if (stud::ui::enumerate_gpus().empty()) {
        if (const KernelGpu gpu = kernel_gpu(); !gpu.usable) {
            open_settings_for_launch_failure(gpu.blocked.isEmpty() ? no_gpu_reason() : no_gpu_access_reason(gpu));
            return false;
        }
        if (settings.renderer != "opengl") {
            settings.renderer = "opengl";
            std::printf("stud: no Vulkan device; Roblox runs on OpenGL this time\n");
        }
    }

    // Cancel stops opening Stud.
    stud::ui::show_loading_lines([]() {
        // Stops a download, unpacking or prefix setup under way, and clears
        // what it left.
        stud::ui::client::cancel();
        stud::ui::terminate_stud_session();
        QMetaObject::invokeMethod(qApp, &QApplication::quit, Qt::QueuedConnection);
    });
    const auto progress = [](const QString& text, int percent) {
        if (stud::ui::loading_cancelled()) return;
        stud::ui::show_loading_text(text);
        stud::ui::set_loading_progress(percent);
        QCoreApplication::processEvents();
    };

    const QString rosemary = stud::ui::client::find_rosemary();
    if (rosemary.isEmpty()) {
        open_settings_for_launch_failure(
            QStringLiteral("Rosemary, which runs Roblox's macOS client, was not found. Install it, "
                           "or set STUD_ROSEMARY to its path."));
        return false;
    }
    std::printf("stud: rosemary %s\n", qPrintable(rosemary));

    QString error;
    const auto had = stud::ui::client::installed(stud::ui::client::Product::Player);
    const auto build = stud::ui::client::ensure_current(product, progress, &error);
    if (stud::ui::loading_cancelled()) return false;
    if (!build) {
        open_settings_for_launch_failure(error);
        return false;
    }
    // The cubes' stud comes from the installed Player: a new one means
    // every cube on screen takes it again.
    const auto has = stud::ui::client::installed(stud::ui::client::Product::Player);
    if (has && (!had || had->upload != has->upload)) cube_gl_client_changed();
    if (!stud::ui::client::ensure_prefix(rosemary, progress, &error)) {
        if (stud::ui::loading_cancelled()) return false;
        open_settings_for_launch_failure(error);
        return false;
    }
    for (const QString& leftover : stud::ui::client::remove_installer_leftovers(
             stud::ui::client::prefix_dir() + QStringLiteral("/root"))) {
        std::printf("stud: removed what Roblox's own installer left: %s\n", qPrintable(leftover));
    }
    if (player && !stud::config::waits_for_refresh(settings)) lift_roblox_frame_cap();
    if (has_nvidia_card() && nvidia_vulkan_driver().isEmpty()) {
        std::printf("stud: an NVIDIA card without NVIDIA's Vulkan driver\n");
        stud::ui::send_notification(
            QStringLiteral("NVIDIA's Vulkan driver is missing"),
            QStringLiteral("Roblox runs on another GPU until NVIDIA's Vulkan driver is installed."));
    }
    clean_old_files(settings.cleaner_days);
    // Customizations, over a build put back as it came.
    if (!stud::ui::customization::apply(*build, product, &error)) {
        std::fprintf(stderr, "stud: customizations were not applied: %s\n", qPrintable(error));
    }
    // A changed set of customizations is on trial until this run ends.
    stud::ui::customization::begin_run(product);
    // The FFlags tab's flags are the player's.
    if (!stud::ui::client::write_app_settings(*build, client_flags(settings, product), &error)) {
        std::fprintf(stderr, "stud: %s\n", qPrintable(error));
    }
    if (stud::ui::loading_cancelled()) return false;
    stud::ui::clear_loading_text();

    // The executable, not the bundle: Rosemary opens a bundle the way
    // LaunchServices does, under its own services and handed to any instance
    // it believes is running, while an executable runs here, as this
    // process's child, so its exit is Roblox's.
    const QString program = stud::ui::client::executable(*build);
    if (program.isEmpty()) {
        open_settings_for_launch_failure(
            QStringLiteral("The Roblox client in %1 has no executable.").arg(build->app));
        return false;
    }
    QStringList args{QStringLiteral("run"), program};
    if (deep_link && player) args << QStringLiteral("-protocolString") << *deep_link;
    if (deep_link && !player) args << *deep_link;

    stud::ui::GameActivity::Options activity;
    activity.discord = settings.discord_rich_presence;
    activity.join_button = settings.discord_join_button;
    activity.region = settings.server_region_notification;
    activity.studio = !player;
    if (!player) {
        const QDir users(stud::ui::client::prefix_dir() + QStringLiteral("/root/Users"));
        const QString self = qEnvironmentVariable("USER");
        const QString user = !self.isEmpty() && users.exists(self) ? self : QString();
        if (!user.isEmpty()) activity.studio_logs = users.filePath(user) + QStringLiteral("/Library/Logs/Roblox");
    }
    activity.history_games = settings.record_game_history;
    activity.history_servers = settings.record_server_history;
    g_activity = new stud::ui::GameActivity(activity, qApp);
    g_client = new QProcess(qApp);
    // Through the wrapper Settings names, if any: it runs Rosemary in turn.
    QStringList wrapper = QProcess::splitCommand(QString::fromStdString(settings.launch_wrapper));
    if (wrapper.isEmpty()) {
        g_client->setProgram(rosemary);
        g_client->setArguments(args);
    } else {
        std::printf("stud: Roblox starts through %s\n", qPrintable(wrapper.join(QLatin1Char(' '))));
        g_client->setProgram(wrapper.takeFirst());
        g_client->setArguments(wrapper + QStringList{rosemary} + args);
    }
    g_client->setProcessEnvironment(client_environment(settings, product));
    g_client->setStandardInputFile(QProcess::nullDevice());
    // Through this process, scrubbed, into the session log and terminal.
    if (deep_link) g_deep_link = deep_link->toUtf8();
    g_client->setProcessChannelMode(QProcess::ForwardedChannels);
    // The client on the machine's fastest cores, where it has more than one
    // kind: on a Mac its main thread's priority keeps it on a fast core, and
    // here the scheduler would as soon run it on an efficiency core. Set up
    // before the fork; the child only hands it to the kernel.
    const std::vector<int> fastest = stud::ui::cpu::fastest();
    cpu_set_t cores;
    CPU_ZERO(&cores);
    for (const int cpu : fastest) CPU_SET(cpu, &cores);
    if (!fastest.empty()) {
        std::printf("stud: the client runs on its %d fastest processors (%d cores)\n",
                    stud::ui::cpu::logical_cpus(fastest), stud::ui::cpu::physical_cores(fastest));
    }
    const int output = start_output_reader() ? g_output_write : -1;
    g_client->setChildProcessModifier([output, cores, restrict = !fastest.empty()] {
        if (output >= 0) {
            ::dup2(output, STDOUT_FILENO);
            ::dup2(output, STDERR_FILENO);
        }
        if (restrict) ::sched_setaffinity(0, sizeof(cores), &cores);
    });
    QObject::connect(g_client, &QProcess::finished, qApp, [product](int code, QProcess::ExitStatus status) {
        // The Player gone before its app started closed without its window
        // ever showing, whatever code it gave.
        const bool never_started = product == stud::ui::client::Product::Player &&
                                   !g_client_app_started.load(std::memory_order_relaxed);
        const bool crashed =
            !g_stop_requested && (status == QProcess::CrashExit || code != 0 || never_started);
        if (crashed && never_started && status != QProcess::CrashExit && code == 0) {
            std::printf("stud: Roblox closed before its window opened\n");
        }
        std::printf("stud: Roblox exited (%s %d)\n",
                    status == QProcess::CrashExit ? "signal" : "code", code);
        const QStringList set_aside = stud::ui::customization::end_run(product, crashed);
        if (!set_aside.isEmpty()) {
            std::printf("stud: Roblox crashed on its first run with changed customizations; "
                        "%lld set aside\n", static_cast<long long>(set_aside.size()));
            stud::ui::send_notification(
                QStringLiteral("Customizations turned off"),
                QStringLiteral("Roblox crashed the first time it ran with your new customizations, "
                               "so Stud put back the last set that worked. The new ones are in %1.")
                    .arg(stud::ui::customization::disabled_folder(product)));
        }
        g_session_end = crashed ? SessionEnd::Crashed : SessionEnd::Clean;
        stud::ui::close_loading_screen();
        if (crashed) {
            stud::ui::show_crash_dialog(QStringLiteral("Roblox"));
        } else if (!g_relaunch_pending) {
            QMetaObject::invokeMethod(qApp, &QApplication::quit, Qt::QueuedConnection);
        }
    });
    g_client_app_started.store(false, std::memory_order_relaxed);
    std::printf("stud: starting Roblox %s (%s)%s\n", qPrintable(build->version),
                qPrintable(build->upload), deep_link ? " with a deep link" : "");
    std::fflush(stdout);
    g_client->start();
    // The client holds the pipe's write end now; with this copy closed, the
    // reader sees the end of its output when it and its children are gone.
    if (g_output_write >= 0) {
        ::close(g_output_write);
        g_output_write = -1;
    }
    if (!g_client->waitForStarted(10000)) {
        open_settings_for_launch_failure(
            QStringLiteral("Roblox didn't start: %1").arg(g_client->errorString()));
        return false;
    }
    if (settings.gamemode) register_with_gamemode(g_client->processId());
    if (QProcess::splitCommand(QString::fromStdString(settings.launch_wrapper)).isEmpty()) {
        raise_main_thread_priority(g_client->processId());
    } else {
        // The process started is the wrapper, not Roblox: its first thread is
        // not the client's main thread.
        std::printf("stud: Roblox starts through a wrapper; its main thread keeps its priority\n");
    }
    // The client puts up its own window within seconds; the loading screen has
    // done its part.
    QTimer::singleShot(3000, qApp, [] { stud::ui::close_loading_screen(); });
    return true;
}

// Every Rosemary process of Stud's prefix, found through /proc: an executable
// named rosemary started with Stud's ROSEMARY_PREFIX. That is the client and
// what runs beside it, and the prefix's own services, launchd, its daemons and
// the process server, none of which name the client in their arguments.
std::vector<pid_t> client_process_ids() {
    std::vector<pid_t> pids;
    const std::string prefix = "ROSEMARY_PREFIX=" + stud::ui::client::prefix_dir().toStdString();
    const auto read = [](const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    };
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit)) continue;
        const std::string cmdline = read(entry.path() / "cmdline");
        const std::string program = cmdline.substr(0, cmdline.find('\0'));
        if (std::filesystem::path(program).filename() != "rosemary") continue;
        const std::string environ = '\0' + read(entry.path() / "environ") + '\0';
        if (environ.find('\0' + prefix + '\0') == std::string::npos) continue;
        pids.push_back(static_cast<pid_t>(std::stol(name)));
    }
    return pids;
}

}  // namespace

namespace stud::ui {

// Ends a running session: the tray's Exit, and Settings before it starts
// Roblox again.
bool stud_session_running() { return g_lock_fd >= 0 || another_instance_is_running(); }

void terminate_stud_session() {
    if (g_client == nullptr) return;
    g_stop_requested = true;
    // `rosemary run` only asks Rosemary's own services to open the app; the
    // client itself runs under them, not under this process, and the services
    // outlive it and would serve the next session from this one's state. So
    // the prefix's process server is told to end everything it runs, and every
    // process of the prefix is stopped with the one this process started.
    std::vector<pid_t> pids = client_process_ids();
    QProcess stop;
    stop.setProgram(g_client->program());
    stop.setArguments({QStringLiteral("server"), QStringLiteral("--stop")});
    stop.setProcessEnvironment(g_client->processEnvironment());
    stop.start();
    stop.waitForFinished(2000);
    for (const pid_t pid : pids) ::kill(pid, SIGTERM);
    g_client->terminate();
    const auto all_gone = [&pids] {
        return std::none_of(pids.begin(), pids.end(), [](pid_t pid) { return ::kill(pid, 0) == 0; });
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline &&
           (g_client->state() != QProcess::NotRunning || !all_gone())) {
        g_client->waitForFinished(100);
    }
    for (const pid_t pid : pids) ::kill(pid, SIGKILL);
    if (g_client->state() != QProcess::NotRunning) {
        g_client->kill();
        g_client->waitForFinished(2000);
    }
}

bool stud_session_is_running() {
    if (g_client != nullptr) return g_client->state() != QProcess::NotRunning;
    return another_instance_is_running();
}

void hold_for_relaunch() { g_relaunch_pending = true; }

bool relaunch_is_pending() { return g_relaunch_pending; }

void set_tray_is_holding(bool holding) { g_tray_is_holding = holding; }

bool crash_dialog_is_open() { return g_crash_dialog_open; }

bool session_ended_cleanly() { return g_session_end != SessionEnd::Crashed; }

// Starts Stud again as a new process and lets this one go, which is what
// closing and reopening it does.
void relaunch_stud() {
    hold_for_relaunch();
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("STUD_LOG_FILE"));
    if (g_layers_disable_was_set) {
        env.insert(QStringLiteral("VK_LOADER_LAYERS_DISABLE"), g_host_layers_disable);
    } else if (::getenv("MANGOHUD") != nullptr || ::getenv("MANGOHUD_CONFIG") != nullptr) {
        env.remove(QStringLiteral("VK_LOADER_LAYERS_DISABLE"));
    }
    // The AppImage file itself, not the mount this process runs from.
    const QString appimage = qEnvironmentVariable("APPIMAGE");
    const QString program = !appimage.isEmpty() && QFileInfo::exists(appimage)
                                ? QFileInfo(appimage).absoluteFilePath()
                                : QCoreApplication::applicationFilePath();
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp, [program, env]() {
        // The lock goes with this process; the new one takes it.
        if (g_lock_fd >= 0) {
            ::close(g_lock_fd);
            g_lock_fd = -1;
        }
        QProcess next;
        next.setProgram(program);
        next.setArguments({g_product == stud::ui::client::Product::Studio
                               ? QStringLiteral("--studio")
                               : QStringLiteral("--roblox")});
        next.setProcessEnvironment(env);
        if (const int passthrough = stud::logging::passthrough_stdout_fd(); passthrough >= 0) {
            next.setChildProcessModifier([passthrough] {
                ::dup2(passthrough, STDOUT_FILENO);
                ::dup2(passthrough, STDERR_FILENO);
            });
        }
        if (!next.startDetached()) {
            std::fprintf(stderr, "stud: could not start Stud again (%s)\n", qPrintable(program));
        }
    });
    QApplication::quit();
}

// "Oops, Stud has crashed!", with a way to keep the log that says why.
void show_crash_dialog(const QString& which_process) {
    const QString log = QString::fromUtf8(qgetenv("STUD_LOG_FILE"));
    // A dialog that is this process's last window must not quit it on its own.
    qApp->setQuitOnLastWindowClosed(false);
    g_crash_dialog_open = true;

    auto* box = new QMessageBox;
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setIconPixmap(QPixmap(QStringLiteral(":/stud-logo-update.png"))
                           .scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    box->setWindowTitle(QStringLiteral("Stud"));
    box->setText(QStringLiteral("Oops, Stud has crashed!"));
    box->setInformativeText(
        log.isEmpty() ? QStringLiteral("%1 stopped unexpectedly.").arg(which_process)
                      : QStringLiteral("%1 stopped unexpectedly.\n\nThe log for this session has "
                                       "been written and can be exported.")
                            .arg(which_process));
    QPushButton* export_button = nullptr;
    if (!log.isEmpty()) {
        export_button = box->addButton(QStringLiteral("Export logs"), QMessageBox::ActionRole);
    }
    box->addButton(QMessageBox::Close);

    QObject::connect(box, &QMessageBox::finished, qApp, [box, export_button, log]() {
        if (export_button != nullptr && box->clickedButton() == export_button) {
            QFileDialog dialog(nullptr, QStringLiteral("Export Stud log"));
            dialog.setAcceptMode(QFileDialog::AcceptSave);
            dialog.setFileMode(QFileDialog::AnyFile);
            dialog.setNameFilter(QStringLiteral("Log files (*.log);;All files (*)"));
            dialog.setDirectory(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
            dialog.selectFile(QFileInfo(log).fileName());
            const QString target =
                dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty()
                    ? dialog.selectedFiles().first()
                    : QString();
            if (!target.isEmpty()) {
                QFile::remove(target);
                if (!QFile::copy(log, target)) {
                    QMessageBox::warning(nullptr, QStringLiteral("Stud"),
                                         QStringLiteral("Could not write %1.").arg(target));
                }
            }
        }
        g_crash_dialog_open = false;
        QMetaObject::invokeMethod(qApp, &QApplication::quit, Qt::QueuedConnection);
    });
    box->exec();
}

}  // namespace stud::ui

int main(int argc, char** argv) {
    keep_mangohud_out_of_this_process();

    // No window waits for the compositor's go-ahead to draw its next frame.
    // A covered window gets none on Wayland, and every frame it drew then
    // held the whole interface thread for Qt's timeout: Settings over the
    // welcome page froze. The animations pace themselves instead.
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(format);
    // QtWebEngine shares its GL contexts with the widgets that show pages.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    // Rosemary starting the web view helper for the client it runs.
    if (argc > 1 && std::string(argv[1]) == "--webview-helper") {
        return stud::ui::webview::run_helper(argc, argv);
    }

    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(":/stud-logo-color.png"));
    // Wayland finds a window's icon through its app_id and a matching desktop
    // entry; the client reports the same id (SDL_APP_ID).
    QGuiApplication::setDesktopFileName(QStringLiteral(STUD_APP_ID));
    QCoreApplication::setApplicationName(QStringLiteral("Stud"));

    // Stud 1.x's Android engine, APK and caches, gone at the first 2.0 start;
    // its settings, FFlags, customizations and logs are read where they are.
    {
        const auto removed = stud::ui::stud1::remove_remnants(
            QString::fromStdString(stud::paths::data_dir()),
            QString::fromStdString(stud::paths::cache_dir()));
        if (!removed.paths.isEmpty()) {
            for (const QString& path : removed.paths) {
                std::printf("stud: removed Stud 1.x's %s\n", qPrintable(path));
            }
            const QString size = QLocale().formattedDataSize(removed.bytes);
            stud::ui::send_notification(
                QStringLiteral("Welcome to Stud 2.0"),
                QStringLiteral("Stud 1.x's Android files are gone (%1 freed). Your settings, FFlags "
                               "and customizations were kept. Log in to Roblox again in the new client.")
                    .arg(size));
        }
    }

    // Registers this AppImage (or build) in the user's own ~/.local/share,
    // which gives the windows an icon and makes roblox:// links open Stud.
    if (argc > 1 && std::string(argv[1]) == "--install-desktop-entry") {
        return stud::ui::install_desktop_entry();
    }
    if (!qEnvironmentVariable("APPIMAGE").isEmpty()) {
        const QString entry = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                              QStringLiteral("/applications/") + QStringLiteral(STUD_APP_ID) +
                              QStringLiteral(".desktop");
        if (!QFileInfo::exists(entry)) {
            std::printf("stud: no desktop entry yet, run `%s --install-desktop-entry` for the "
                        "taskbar icon and roblox:// links\n",
                        qPrintable(qEnvironmentVariable("APPIMAGE")));
        }
    }

    check_flatpak_permissions();

    // Settings, from the desktop entry's action or the tray. A running Stud
    // owns the window; this hands the request over.
    if (argc > 1 && std::string(argv[1]) == "--settings") {
        if (stud::ui::SettingsWindow::handOffToRunningInstance()) return 0;
        stud::ui::SettingsWindow::listenForOpenRequests();
        stud::ui::SettingsWindow::showSingleton();
        return app.exec();
    }

    // A deep link, passed to the client as it came. Only its key names and
    // sizes are logged: the game info is a single-use join ticket. Started on
    // its own, Stud asks whether to open Roblox or Roblox Studio; --roblox
    // and --studio answer that up front.
    std::optional<QString> deep_link;
    std::optional<stud::ui::client::Product> product;
    if (argc > 1 && std::string(argv[1]) == "--roblox") product = stud::ui::client::Product::Player;
    if (argc > 1 && std::string(argv[1]) == "--studio") product = stud::ui::client::Product::Studio;
    const std::optional<QString> place = argc > 1 ? place_file(argv[1]) : std::nullopt;
    if (place) {
        // Studio opens it, the way a Mac opens a document it was given.
        product = stud::ui::client::Product::Studio;
        deep_link = place;
        std::printf("stud: opening a place file in Studio\n");
    } else if (argc > 1 && std::string(argv[1]).rfind("roblox-studio", 0) == 0) {
        product = stud::ui::client::Product::Studio;
        deep_link = QString::fromUtf8(argv[1]);
        std::printf("stud: Studio link of %zu bytes\n", std::strlen(argv[1]));
    } else if (argc > 1 && argv[1][0] != '-') {
        product = stud::ui::client::Product::Player;
        if (stud::ui::parse_launch_uri(argv[1])) {
            deep_link = QString::fromUtf8(argv[1]);
            std::printf("stud: deep link of %zu bytes\n", std::strlen(argv[1]));
        } else {
            std::printf("stud: the launch argument is not a Roblox deep link\n");
        }
    }

    const bool tray_wanted = [] {
        try {
            return stud::config::load_settings(stud::config::default_config_path()).system_tray;
        } catch (const std::exception&) {
            return true;
        }
    }();

    if (!product) {
        const bool directly = [] {
            try {
                return stud::config::load_settings(stud::config::default_config_path()).open_player_directly;
            } catch (const std::exception&) {
                return false;
            }
        }();
        product = directly ? std::optional(stud::ui::client::Product::Player) : stud::ui::choose_product();
    }
    if (!product) return 0;  // closed without a choice

    if (!launch_game(*product, deep_link)) {
        // Settings saying why keeps Stud open; with nothing on screen, quit.
        const QWidgetList windows = QApplication::topLevelWidgets();
        if (std::none_of(windows.begin(), windows.end(),
                         [](const QWidget* w) { return w->isVisible(); })) {
            QMetaObject::invokeMethod(qApp, &QApplication::quit, Qt::QueuedConnection);
        }
        return app.exec();
    }

    // This process stays for the session: it is the client's parent and the
    // one able to report a crash. With a tray it also offers Settings and
    // Exit; closing Roblox ends the session either way.
    stud::ui::SettingsWindow::listenForOpenRequests();
    if (tray_wanted) {
        auto* tray = new stud::ui::Tray(qApp);
        if (tray->show()) {
            stud::ui::set_tray_is_holding(true);
        } else {
            std::fprintf(stderr, "stud: no system tray available\n");
        }
    }
    // Windows come and go (Settings, a dialog) while Roblox runs; only the
    // client's exit ends Stud.
    app.setQuitOnLastWindowClosed(false);
    return app.exec();
}
