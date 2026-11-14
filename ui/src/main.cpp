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
#include "diagnose.h"
#include "game_activity.h"
#include "gpu_enum.h"
#include "launch_uri.h"
#include "loading_screen.h"
#include "log_scrub.h"
#include "mac_client.h"
#include "mods.h"
#include "notifications.h"
#include "settings_window.h"
#include "tray.h"
#include "stud/session_log.h"
#include "stud/settings.h"
#include "stud/stud_paths.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPixmap>
#include <QProcess>
#include <QSaveFile>
#include <QRegularExpression>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QStandardPaths>
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

void forward_line(QByteArray line) {
    if (g_activity != nullptr && stud::ui::GameActivity::wants(line)) {
        QMetaObject::invokeMethod(qApp, [line] {
            if (g_activity != nullptr) g_activity->line(line);
        }, Qt::QueuedConnection);
    }
    if (!g_deep_link.isEmpty()) line.replace(g_deep_link, "<deep link>");
    const std::string scrubbed =
        stud::ui::scrub_engine_log_line(std::string_view(line.constData(), line.size()));
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
    }
    // The client presents with Vulkan, where MangoHud's implicit layer needs
    // nothing more than this.
    if (enabled) env.insert(QStringLiteral("MANGOHUD"), QStringLiteral("1"));
}

// Stud's icon as a file Rosemary can read, written beside Stud's other
// runtime files. Empty when it cannot be written.
QString window_icon_file() {
    const QString path = QString::fromStdString(stud::paths::runtime_dir()) +
                         QStringLiteral("/window-icon.png");
    QFile::remove(path);
    return QFile::copy(QStringLiteral(":/stud-logo-color.png"), path) ? path : QString();
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
        QStringLiteral("ROSEMARY_CONTROL_EDIT_KEYS"), QStringLiteral("ROSEMARY_SWAP_INTERVAL"),
        QStringLiteral("ROSEMARY_SCREEN_REFRESH_RATE"),
        QStringLiteral("ROSEMARY_METAL_PRESENTATION_MODE"), QStringLiteral("ROSEMARY_WINDOW_TITLE"),
        QStringLiteral("ROSEMARY_WINDOW_ICON"), QStringLiteral("ROSEMARY_STATUS_ITEMS"),
        QStringLiteral("ROSEMARY_BACKGROUND_FPS"),
    };
    for (const QString& name : env.keys()) {
        if (loads_code(name) || kFromSettings.contains(name)) env.remove(name);
    }
    env.remove(QStringLiteral("STUD_LOG_FILE"));

    env.insert(QStringLiteral("ROSEMARY_PREFIX"), stud::ui::client::prefix_dir());
    env.insert(QStringLiteral("ROSEMARY_PREFIX_SETUP"), QStringLiteral("download"));

    // The window: Stud's own application id, so the compositor gives it
    // Stud's icon and puts it under Stud's taskbar entry, and the backend.
    env.insert(QStringLiteral("SDL_APP_ID"), QStringLiteral(STUD_APP_ID));
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
    // Studio keeps its own: its windows are documents, named by what is open.
    if (product == stud::ui::client::Product::Player) {
        env.insert(QStringLiteral("ROSEMARY_WINDOW_TITLE"), QStringLiteral("Stud"));
        if (const QString icon = window_icon_file(); !icon.isEmpty()) {
            env.insert(QStringLiteral("ROSEMARY_WINDOW_ICON"), icon);
        }
    }
    env.insert(QStringLiteral("ROSEMARY_STATUS_ITEMS"), QStringLiteral("0"));
    if (settings.background_fps != stud::config::kBackgroundFpsNoLimit) {
        env.insert(QStringLiteral("ROSEMARY_BACKGROUND_FPS"),
                   QString::number(settings.background_fps));
    }

    // Ctrl for copy, cut, paste and select-all, where the macOS client
    // expects Command (which Linux desktops keep for themselves).
    env.insert(QStringLiteral("ROSEMARY_CONTROL_EDIT_KEYS"), QStringLiteral("1"));

    // VSync: on Vulkan the presentation mode, FIFO waiting for the display
    // and Rosemary's default (MAILBOX) not; on OpenGL the swap interval.
    // Without VSync, uncapped: the client paces itself to the refresh rate it
    // is told, so it is told one no display has.
    if (!settings.vsync) {
        env.insert(QStringLiteral("ROSEMARY_SCREEN_REFRESH_RATE"), QStringLiteral("9999"));
    }
    if (settings.renderer == "opengl") {
        if (!settings.vsync) env.insert(QStringLiteral("ROSEMARY_SWAP_INTERVAL"), QStringLiteral("0"));
    } else if (settings.vsync) {
        env.insert(QStringLiteral("ROSEMARY_METAL_PRESENTATION_MODE"), QStringLiteral("fifo"));
    }

    apply_gpu_selection(env, settings);
    apply_mangohud(env, settings.mangohud);
    return env;
}

// The FastFlags of Settings' FFlags tab (flags.json), written as the
// client's ClientAppSettings.json. A malformed flags.json is reported and
// left out rather than stopping the launch.
QJsonObject client_flags(const stud::config::StudSettings& settings) {
    QJsonObject flags;
    QFile file(QString::fromStdString(stud::config::default_flag_overrides_path()));
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) {
            flags = doc.object();
        } else {
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

// Hands the client to Feral GameMode, which lets go when it exits.
void register_with_gamemode(qint64 pid) {
    QDBusInterface gamemode("com.feralinteractive.GameMode", "/com/feralinteractive/GameMode",
                            "com.feralinteractive.GameMode", QDBusConnection::sessionBus());
    const QDBusReply<int> reply = gamemode.call("RegisterGame", static_cast<int>(pid));
    if (!reply.isValid()) {
        std::printf("stud: GameMode is not available: %s\n",
                    reply.error().message().toLocal8Bit().constData());
    } else if (reply.value() != 0) {
        std::printf("stud: GameMode refused the session (%d)\n", reply.value());
    } else {
        std::printf("stud: GameMode is on for this session\n");
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

    // A Studio link while Stud runs (Studio's sign-in comes back this way)
    // goes to the running Studio: Rosemary hands a URL given with the bundle
    // to the instance already open.
    if (!player && deep_link && another_instance_is_running()) {
        if (const auto studio = stud::ui::client::installed(product)) {
            const QString rosemary = stud::ui::client::find_rosemary();
            QProcess handoff;
            handoff.setProgram(rosemary);
            handoff.setArguments({QStringLiteral("run"), studio->app, *deep_link});
            handoff.setProcessEnvironment(client_environment(settings, product));
            handoff.setStandardOutputFile(QProcess::nullDevice());
            handoff.setStandardErrorFile(QProcess::nullDevice());
            if (!rosemary.isEmpty() && handoff.startDetached()) {
                QMetaObject::invokeMethod(qApp, &QApplication::quit, Qt::QueuedConnection);
                return false;
            }
        }
    }
    if (another_instance_is_running() || !take_instance_lock()) {
        QMessageBox::information(nullptr, "Stud",
                                 "Stud is already running.\n\nClose the existing window before "
                                 "starting it again.");
        return false;
    }

    start_session_log();
    stud::ui::write_diagnostics();

    // Cancel stops opening Stud.
    stud::ui::show_loading_lines([]() {
        stud::ui::terminate_stud_session();
        QMetaObject::invokeMethod(qApp, &QApplication::quit, Qt::QueuedConnection);
    });
    const auto progress = [](const QString& text, int percent) {
        stud::ui::show_loading_text(percent >= 0 ? QStringLiteral("%1 (%2%)").arg(text).arg(percent)
                                                 : text);
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
    const auto build = stud::ui::client::ensure_current(product, progress, &error);
    if (stud::ui::loading_cancelled()) return false;
    if (!build) {
        open_settings_for_launch_failure(error);
        return false;
    }
    if (!stud::ui::client::ensure_prefix(rosemary, progress, &error)) {
        open_settings_for_launch_failure(error);
        return false;
    }
    if (player && !settings.vsync) lift_roblox_frame_cap();
    if (has_nvidia_card() && nvidia_vulkan_driver().isEmpty()) {
        std::printf("stud: an NVIDIA card without NVIDIA's Vulkan driver\n");
        stud::ui::send_notification(
            QStringLiteral("NVIDIA's Vulkan driver is missing"),
            QStringLiteral("Roblox runs on another GPU until NVIDIA's Vulkan driver is installed."));
    }
    // Mods, over a build put back as it came.
    if (!stud::ui::mods::apply(*build, product, &error)) {
        std::fprintf(stderr, "stud: mods were not applied: %s\n", qPrintable(error));
    }
    // The FFlags tab's flags are the player's.
    if (player && !stud::ui::client::write_app_settings(*build, client_flags(settings), &error)) {
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

    if (player) {
        g_activity = new stud::ui::GameActivity(settings.discord_rich_presence,
                                                settings.discord_join_button,
                                                settings.server_region_notification, qApp);
    }
    g_client = new QProcess(qApp);
    g_client->setProgram(rosemary);
    g_client->setArguments(args);
    g_client->setProcessEnvironment(client_environment(settings, product));
    g_client->setStandardInputFile(QProcess::nullDevice());
    // Through this process, scrubbed, into the session log and terminal.
    if (deep_link) g_deep_link = deep_link->toUtf8();
    g_client->setProcessChannelMode(QProcess::ForwardedChannels);
    if (start_output_reader()) {
        g_client->setChildProcessModifier([fd = g_output_write] {
            ::dup2(fd, STDOUT_FILENO);
            ::dup2(fd, STDERR_FILENO);
        });
    }
    QObject::connect(g_client, &QProcess::finished, qApp, [](int code, QProcess::ExitStatus status) {
        const bool crashed =
            !g_stop_requested && (status == QProcess::CrashExit || code != 0);
        std::printf("stud: Roblox exited (%s %d)\n",
                    status == QProcess::CrashExit ? "signal" : "code", code);
        g_session_end = crashed ? SessionEnd::Crashed : SessionEnd::Clean;
        stud::ui::close_loading_screen();
        if (crashed) {
            stud::ui::show_crash_dialog(QStringLiteral("Roblox"));
        } else if (!g_relaunch_pending) {
            QMetaObject::invokeMethod(qApp, &QApplication::quit, Qt::QueuedConnection);
        }
    });
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
    // The client puts up its own window within seconds; the loading screen has
    // done its part.
    QTimer::singleShot(3000, qApp, [] { stud::ui::close_loading_screen(); });
    return true;
}

// Every Rosemary process running Stud's copy of the client, found through
// /proc: an executable named rosemary with an argument inside the session's
// product's directory.
std::vector<pid_t> client_process_ids() {
    std::vector<pid_t> pids;
    const std::string clients = stud::ui::client::clients_dir(g_product).toStdString() + "/";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit)) continue;
        std::ifstream file(entry.path() / "cmdline", std::ios::binary);
        const std::string cmdline((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
        const std::string program = cmdline.substr(0, cmdline.find('\0'));
        if (std::filesystem::path(program).filename() != "rosemary") continue;
        if (cmdline.find(clients) == std::string::npos) continue;
        pids.push_back(static_cast<pid_t>(std::stol(name)));
    }
    return pids;
}

}  // namespace

namespace stud::ui {

// Ends a running session: the tray's Exit, and Settings before it starts
// Roblox again.
void terminate_stud_session() {
    if (g_client == nullptr) return;
    g_stop_requested = true;
    // `rosemary run` only asks Rosemary's own services to open the app; the
    // client itself runs under them, not under this process. So the client
    // is found by what it runs, Stud's copy of Roblox, and stopped with the
    // process this one started.
    std::vector<pid_t> pids = client_process_ids();
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

    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(":/stud-logo-color.png"));
    // Wayland finds a window's icon through its app_id and a matching desktop
    // entry; the client reports the same id (SDL_APP_ID).
    QGuiApplication::setDesktopFileName(QStringLiteral(STUD_APP_ID));
    QCoreApplication::setApplicationName(QStringLiteral("Stud"));

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
    if (argc > 1 && std::string(argv[1]).rfind("roblox-studio", 0) == 0) {
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

    if (!product) product = stud::ui::choose_product();
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
