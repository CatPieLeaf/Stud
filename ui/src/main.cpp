// Stud's launcher. It starts Roblox's macOS client on Rosemary as a second
// process and stays for the session, so a crash can always be reported.
// A plain launch opens Roblox's home screen; a deep link (roblox://...) is
// passed to the client as its -protocolString. Stud never modifies the client
// apart from writing ClientAppSettings.json (mac_client.h).

#include "desktop_entry.h"
#include "chooser.h"
#include "cube.h"
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
#include "login_keyring.h"
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
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <atomic>
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
#include <unistd.h>

namespace stud::ui {
void terminate_stud_session();
void show_crash_dialog(const QString& which_process);
}  // namespace stud::ui

namespace {

// The running client. Owned for the life of this process.
QProcess* g_client = nullptr;
// Set when the session was ended on purpose, so its exit is not a crash.
bool g_stop_requested = false;
bool g_relaunch_pending = false;

// The deep link holds a single-use join ticket, so it is kept out of the log.
QByteArray g_deep_link;
stud::ui::GameActivity* g_activity = nullptr;
stud::ui::client::Product g_product = stud::ui::client::Product::Player;
// The client's stdout/stderr pipe; closed here once it has started.
int g_output_write = -1;
// A Player that exits before its app starts failed to start, whatever its code.
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

// The client logs from any thread and a full pipe blocks it, so the pipe is as
// large as allowed and a thread of its own drains it. False when the pipe
// cannot be made; the client then writes to this process's stdout.
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

// One Stud at a time. The kernel drops the lock when its holder dies.
// O_CLOEXEC keeps it out of Rosemary's long-lived helper processes.
int g_lock_fd = -1;

std::string lock_path() { return stud::paths::runtime_dir() + "/stud.lock"; }

bool another_instance_is_running() {
    if (g_lock_fd >= 0) return false;
    std::error_code ec;
    std::filesystem::create_directories(stud::paths::runtime_dir(), ec);
    const int fd = ::open(lock_path().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const bool free_to_take = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
    if (free_to_take) ::flock(fd, LOCK_UN);
    ::close(fd);
    return !free_to_take;
}

bool take_instance_lock() {
    std::error_code ec;
    std::filesystem::create_directories(stud::paths::runtime_dir(), ec);
    const int fd = ::open(lock_path().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return true;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    g_lock_fd = fd;
    return true;
}

// An inherited MANGOHUD=1 loaded the layer into this Qt process, which uses
// Vulkan only to list GPUs, and crashed it. Disabled here; the client still
// gets the user's value.
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

// Variables that inject code into a process. Stud never lets them reach Roblox.
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

QString nvidia_vulkan_driver() {
    for (const char* path : {"/usr/share/vulkan/icd.d/nvidia_icd.x86_64.json",
                             "/usr/share/vulkan/icd.d/nvidia_icd.json",
                             "/etc/vulkan/icd.d/nvidia_icd.json"}) {
        if (QFileInfo(QString::fromLatin1(path)).isFile()) return QString::fromLatin1(path);
    }
    return {};
}

// PCI vendor 0x10de, display class 0x03.
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

// Roblox's menu caps the frame rate at 240, so with VSync off its settings
// file gets the value it reads as uncapped. Only an existing entry is changed.
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

// The chosen GPU on a laptop with two: PRIME offload for NVIDIA, DRI_PRIME for Mesa.
void apply_gpu_selection(QProcessEnvironment& env, const stud::config::StudSettings& settings) {
    const auto gpus = stud::ui::enumerate_gpus();
    const stud::ui::GpuInfo* chosen = nullptr;
    for (const auto& gpu : gpus) {
        if (gpu.device_index == settings.gpu.device_index) chosen = &gpu;
    }
    if (chosen == nullptr || !chosen->discrete) return;
    if (chosen->vendor_id == stud::ui::kVendorNvidia) {
        env.insert(QStringLiteral("__NV_PRIME_RENDER_OFFLOAD"), QStringLiteral("1"));
        env.insert(QStringLiteral("__GLX_VENDOR_LIBRARY_NAME"), QStringLiteral("nvidia"));
        env.insert(QStringLiteral("__VK_LAYER_NV_optimus"), QStringLiteral("NVIDIA_only"));
        // Only NVIDIA's ICD, so Mesa's aren't loaded too and its shader cache stays whole.
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
    // Undo keep_mangohud_out_of_this_process(), which was for Stud's own process.
    if (g_layers_disable_was_set) {
        env.insert(QStringLiteral("VK_LOADER_LAYERS_DISABLE"), g_host_layers_disable);
    } else {
        env.remove(QStringLiteral("VK_LOADER_LAYERS_DISABLE"));
    }
    // picmip -17 leaves sampler LOD bias alone, so a config made for another game
    // doesn't change Roblox's textures.
    if (enabled || env.contains(QStringLiteral("MANGOHUD"))) {
        const QString existing = env.value(QStringLiteral("MANGOHUD_CONFIG"));
        env.insert(QStringLiteral("MANGOHUD_CONFIG"),
                   existing.isEmpty() ? QStringLiteral("read_cfg,picmip=-17")
                                      : existing + QStringLiteral(",picmip=-17"));
        // Studio's panels are windows of their own; only the main one gets the overlay.
        env.insert(QStringLiteral("ROSEMARY_SWAPCHAIN_MAIN_WINDOW_ONLY"), QStringLiteral("1"));
    }
    if (enabled) env.insert(QStringLiteral("MANGOHUD"), QStringLiteral("1"));
}

// Stud's window icon for `product` as a file Rosemary can read. Empty on failure.
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

QProcessEnvironment client_environment(const stud::config::StudSettings& settings,
                                       stud::ui::client::Product product) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // Settings owns these; Rosemary's other variables pass through for debugging.
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

    // <id>.Player or <id>.Studio, whose hidden desktop entries give the taskbar
    // the right icon. Flatpak accepts them as extensions of the sandbox id.
    const bool studio = product == stud::ui::client::Product::Studio;
    env.insert(QStringLiteral("SDL_APP_ID"),
               QStringLiteral(STUD_APP_ID) + (studio ? QStringLiteral(".Studio") : QStringLiteral(".Player")));
    // Plasma's mixer drops stream names containing "audio", so it shows just Stud.
    env.insert(QStringLiteral("SDL_APP_NAME"), QStringLiteral("Stud"));
    // An Intel Mac, not Rosetta: Rosemary reports Rosetta for Roblox's installer,
    // and Stud downloads the client itself.
    env.insert(QStringLiteral("ROSEMARY_PROC_TRANSLATED"), QStringLiteral("0"));
    // Stud installs the client; Roblox's installer would replace the checked
    // build with another channel's, so Rosemary refuses to run it.
    env.insert(QStringLiteral("ROSEMARY_REFUSED_PROGRAMS"),
               QStringLiteral("RobloxPlayerInstaller:RobloxStudioInstaller"));
    // Metal only: an OpenGL surface stays window-sized and would draw into a corner.
    if (settings.render_scale < 100 && settings.renderer == "vulkan") {
        env.insert(QStringLiteral("ROSEMARY_RENDER_SCALE"), QString::number(settings.render_scale / 100.0));
        env.insert(QStringLiteral("ROSEMARY_UPSCALER"), QString::fromStdString(settings.upscaler));
    }
    env.insert(QStringLiteral("SDL_AUDIO_DEVICE_STREAM_NAME"), QStringLiteral("Roblox audio"));
    env.insert(QStringLiteral("SDL_AUDIO_DEVICE_APP_ICON_NAME"),
               QStringLiteral(STUD_APP_ID) + (studio ? QStringLiteral("-studio") : QStringLiteral("-player")));
    // Wayland when available; XWayland only when chosen in Settings.
    if (!env.value(QStringLiteral("WAYLAND_DISPLAY")).isEmpty()) {
        const bool xwayland = !env.value(QStringLiteral("DISPLAY")).isEmpty();
        env.insert(QStringLiteral("SDL_VIDEO_DRIVER"),
                   xwayland && settings.display_backend == "x11" ? QStringLiteral("x11")
                                                                 : QStringLiteral("wayland"));
    }
    // "client": Rosemary draws the macOS title bar; "server": the desktop does.
    const bool native_title_bar = settings.title_bar == "native";
    env.insert(QStringLiteral("ROSEMARY_DECORATIONS"),
               native_title_bar ? QStringLiteral("server") : QStringLiteral("client"));
    env.insert(QStringLiteral("ROSEMARY_MENU"), settings.title_bar == "macos"
                                                    ? QStringLiteral("titlebar")
                                                    : QStringLiteral("none"));
    // Stud's name and icon, and no tray icons of the client's own. Studio keeps
    // its titles, which name the open document.
    if (product == stud::ui::client::Product::Player) {
        env.insert(QStringLiteral("ROSEMARY_WINDOW_TITLE"), QStringLiteral("Stud"));
    }
    if (const QString icon = window_icon_file(product); !icon.isEmpty()) {
        env.insert(QStringLiteral("ROSEMARY_WINDOW_ICON"), icon);
    }
    env.insert(QStringLiteral("ROSEMARY_STATUS_ITEMS"), QStringLiteral("0"));
    {
        const QString state = QString::fromStdString(stud::paths::state_dir());
        QDir().mkpath(state);
        env.insert(QStringLiteral("ROSEMARY_WINDOW_STATE"),
                   state + (product == stud::ui::client::Product::Studio ? QStringLiteral("/window-studio")
                                                                         : QStringLiteral("/window")));
    }
    if (settings.background_fps != stud::config::kBackgroundFpsNoLimit) {
        env.insert(QStringLiteral("ROSEMARY_BACKGROUND_FPS"),
                   QString::number(settings.background_fps));
    }

    // Ctrl for Command. The Player only for clipboard shortcuts, so games keep
    // Ctrl; Studio for every shortcut, as on Windows, with Super for Control.
    if (product == stud::ui::client::Product::Studio) {
        env.insert(QStringLiteral("ROSEMARY_COMMAND_KEY"), QStringLiteral("ctrl"));
    } else {
        env.insert(QStringLiteral("ROSEMARY_CONTROL_EDIT_KEYS"), QStringLiteral("1"));
    }

    // VSync off means uncapped: the client paces itself to the refresh rate it
    // is told, so it gets one no display has. OpenGL has no mailbox mode.
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
    // Last, so Settings' own variables win.
    for (const QString& line : QString::fromStdString(settings.environment).split(QLatin1Char('\n'))) {
        const QString entry = line.trimmed();
        const qsizetype equals = entry.indexOf(QLatin1Char('='));
        if (entry.isEmpty() || entry.startsWith(QLatin1Char('#')) || equals <= 0) continue;
        env.insert(entry.left(equals).trimmed(), entry.mid(equals + 1));
    }
    return env;
}

// flags.json as ClientAppSettings.json. A malformed file is reported and skipped.
QJsonObject client_flags(const stud::config::StudSettings& settings,
                         stud::ui::client::Product product = stud::ui::client::Product::Player) {
    QJsonObject flags;
    QFile file(QString::fromStdString(product == stud::ui::client::Product::Studio
                                          ? stud::config::default_studio_flags_path()
                                          : stud::config::default_flag_overrides_path()));
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray text = file.readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(text);
        if (doc.isObject()) {
            flags = doc.object();
        } else if (!text.trimmed().isEmpty()) {
            std::fprintf(stderr, "stud: %s is not a JSON object; its flags are not used\n",
                         qPrintable(file.fileName()));
        }
    }
    flags.remove(QStringLiteral("FFlagDebugGraphicsPreferOpenGL"));
    if (settings.renderer == "opengl") {
        flags.insert(QStringLiteral("FFlagDebugGraphicsPreferOpenGL"), true);
    }
    return flags;
}

// GameMode raises the CPU governor through a helper only the "gamemode" group
// may run; otherwise it claims to be on while the CPU stays in power-saving
// mode (half the frame rate on a laptop). Warn once if polkit says no.
void warn_if_gamemode_cannot_raise_cpu() {
    static bool warned = false;
    if (warned) return;
    auto* check = new QProcess(qApp);
    QObject::connect(check, &QProcess::finished, check, [check](int code, QProcess::ExitStatus status) {
        check->deleteLater();
        // pkcheck: 0 authorized, 1 not, 2 needs a password.
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
        if (error == QProcess::FailedToStart) check->deleteLater();
    });
    check->start(QStringLiteral("pkcheck"),
                 {QStringLiteral("--action-id"),
                  QStringLiteral("com.feralinteractive.GameMode.governor-helper"),
                  QStringLiteral("--process"), QString::number(QCoreApplication::applicationPid())});
}

// GameMode inside Flatpak goes through the desktop portal; GameMode's own bus
// name isn't reachable from the sandbox. pidfds survive the PID namespace;
// a bare pid is the fallback.
QDBusReply<int> register_through_portal(qint64 pid) {
    QDBusInterface portal("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                          "org.freedesktop.portal.GameMode", QDBusConnection::sessionBus());
    const int target = static_cast<int>(::syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0));
    const int self = static_cast<int>(::syscall(SYS_pidfd_open, ::getpid(), 0));
    QDBusReply<int> reply;
    if (target >= 0 && self >= 0) {
        reply = portal.call("RegisterGameByPIDFd", QVariant::fromValue(QDBusUnixFileDescriptor(target)),
                            QVariant::fromValue(QDBusUnixFileDescriptor(self)));
    } else {
        reply = portal.call("RegisterGame", static_cast<int>(pid));
    }
    if (target >= 0) ::close(target);
    if (self >= 0) ::close(self);
    return reply;
}

// Raise the client's main thread (Roblox's frame loop) as far as RealtimeKit,
// or the portal's Realtime interface in Flatpak, allows. Its thread id is the pid.
void raise_main_thread_priority(qint64 pid) {
    const bool sandboxed = stud::config::in_flatpak();
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

struct KernelGpu {
    bool usable = false;  // a render node it can open, or NVIDIA's device
    QString blocked;  // otherwise, the render node it can't open
    QString group;
};

KernelGpu kernel_gpu() {
    KernelGpu gpu;
    if (QFileInfo::exists(QStringLiteral("/dev/nvidiactl"))) {
        gpu.usable = true;
        return gpu;
    }
    for (const stud::ui::RenderNode& node : stud::ui::render_nodes()) {
        if (node.error == 0) {
            gpu.usable = true;
            return gpu;
        }
        if (node.error == EACCES && gpu.blocked.isEmpty()) {
            gpu.blocked = QString::fromStdString(node.path);
            gpu.group = QString::fromStdString(node.group);
        }
    }
    return gpu;
}

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

QString no_gpu_reason() {
    QString reason = QStringLiteral(
        "Roblox needs a graphics card, and this Linux system has none it can use: Vulkan finds no "
        "device, and there is nothing in /dev/dri. Without one, Roblox only shows an empty window.");
    // ChromeOS's Linux container gets the GPU only when enabled.
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

void register_with_gamemode(qint64 pid) {
    const bool sandboxed = stud::config::in_flatpak();
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
        // pkcheck can't reach the host's polkit from the sandbox.
        if (!sandboxed) warn_if_gamemode_cannot_raise_cpu();
    }
}

// One log per session under ~/.local/state/stud/logs; the last ten are kept.
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

// Hands `link` to the running client, as macOS would. False when none is running.
bool hand_link_to_client(stud::ui::client::Product product, const QString& link,
                         const stud::config::StudSettings& settings) {
    const auto build = stud::ui::client::installed(product);
    const QString rosemary = stud::ui::client::find_rosemary();
    if (!build || rosemary.isEmpty()) return false;
    QProcess handoff;
    handoff.setProgram(rosemary);
    // A place file goes as a file:// URL, which Rosemary opens as a document.
    handoff.setArguments({QStringLiteral("run"), build->app,
                          link.startsWith(QLatin1Char('/')) ? QUrl::fromLocalFile(link).toString() : link});
    handoff.setProcessEnvironment(client_environment(settings, product));
    handoff.setStandardOutputFile(QProcess::nullDevice());
    handoff.setStandardErrorFile(QProcess::nullDevice());
    return handoff.startDetached();
}

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

// A .rbxl/.rbxlx path or file:// URL, as the path Studio's guest sees it.
std::optional<QString> place_file(const char* argument) {
    const QString given = QString::fromLocal8Bit(argument);
    const QFileInfo info(given.startsWith(QLatin1String("file:")) ? QUrl(given).toLocalFile() : given);
    const QString suffix = info.suffix().toLower();
    if ((suffix != QLatin1String("rbxl") && suffix != QLatin1String("rbxlx")) || !info.isFile()) {
        return std::nullopt;
    }
    return QStringLiteral("/Volumes/Host") + info.absoluteFilePath();
}

// Names Flatpak permissions that were removed and the host command that restores
// them; the sandbox can't widen itself.
void check_flatpak_permissions() {
    QFile info(QStringLiteral("/.flatpak-info"));
    if (!info.open(QIODevice::ReadOnly)) return;
    const QStringList missing = stud::ui::missing_flatpak_permissions(QString::fromUtf8(info.readAll()));
    if (missing.isEmpty()) return;
    const QString command = stud::ui::flatpak_override_command(missing);
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

// False when the client couldn't start; the reason has been shown.
bool launch_game(stud::ui::client::Product product, const std::optional<QString>& deep_link,
                 stud::config::StudSettings settings) {
    g_product = product;
    const bool player = product == stud::ui::client::Product::Player;
    settings.gpu.device_index = stud::ui::chosen_gpu_index(settings.gpu);

    if (!take_instance_lock()) {
        if (deep_link && hand_link_to_client(product, *deep_link, settings)) {
            QMetaObject::invokeMethod(qApp, &QApplication::quit, Qt::QueuedConnection);
            return false;
        }
        QMessageBox::information(nullptr, "Stud",
                                 "Stud is already running.\n\nClose the existing window before "
                                 "starting it again.");
        return false;
    }

    start_session_log();
    stud::ui::write_diagnostics();

    // With no Vulkan device Roblox's Metal renderer can't start, and Mesa's
    // software fallback can't compile its shaders, leaving an empty window.
    if (!stud::ui::vulkan_available()) {
        if (const KernelGpu gpu = kernel_gpu(); !gpu.usable) {
            open_settings_for_launch_failure(gpu.blocked.isEmpty() ? no_gpu_reason() : no_gpu_access_reason(gpu));
            return false;
        }
        if (settings.renderer != "opengl") {
            settings.renderer = "opengl";
            std::printf("stud: no Vulkan device; Roblox runs on OpenGL this time\n");
        }
    }

    stud::ui::show_loading_lines([]() {
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
    // The loading cubes use the installed Player's stud texture.
    const auto has = stud::ui::client::installed(stud::ui::client::Product::Player);
    if (has && (!had || had->upload != has->upload)) cube_client_changed();
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
    if (!stud::ui::customization::apply(*build, product, &error)) {
        std::fprintf(stderr, "stud: customizations were not applied: %s\n", qPrintable(error));
    }
    // New customizations are on trial until this run ends (crash guard).
    stud::ui::customization::begin_run(product);
    stud::ui::login::restore(product);
    if (!stud::ui::client::write_app_settings(*build, client_flags(settings, product), &error)) {
        std::fprintf(stderr, "stud: %s\n", qPrintable(error));
    }
    if (stud::ui::loading_cancelled()) return false;
    stud::ui::clear_loading_text();

    // The executable, not the bundle: a bundle would be opened by Rosemary's
    // services instead of as this process's child, so we'd lose its exit.
    const QString program = stud::ui::client::executable(*build);
    if (program.isEmpty()) {
        open_settings_for_launch_failure(
            QStringLiteral("The Roblox client in %1 has no executable.").arg(build->app));
        return false;
    }
    QStringList args{QStringLiteral("run"), program};
    if (deep_link && player) args << QStringLiteral("-protocolString") << *deep_link;
    // As Studio's edit task: opened as a document, a second Studio would start
    // beside it on the start page.
    if (deep_link && !player && deep_link->startsWith(QLatin1Char('/'))) {
        args << QStringLiteral("-task") << QStringLiteral("EditFile") << QStringLiteral("-localPlaceFile")
             << *deep_link;
    } else if (deep_link && !player) {
        args << *deep_link;
    }

    stud::ui::GameActivity::Options activity;
    activity.discord = settings.discord_rich_presence;
    activity.join_button = settings.discord_join_button;
    activity.region = settings.server_region_notification;
    activity.studio = !player;
    if (!player) {
        if (const QString home = stud::ui::client::prefix_home(); !home.isEmpty()) {
            activity.studio_logs = home + QStringLiteral("/Library/Logs/Roblox");
        }
    }
    activity.history_games = settings.record_game_history;
    activity.history_servers = settings.record_server_history;
    activity.close_on_leave = settings.close_on_leave;
    g_activity = new stud::ui::GameActivity(activity, qApp);
    QObject::connect(g_activity, &stud::ui::GameActivity::leftGame, qApp,
                     [] { stud::ui::terminate_stud_session(); }, Qt::QueuedConnection);
    g_client = new QProcess(qApp);
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
    if (deep_link) g_deep_link = deep_link->toUtf8();
    g_client->setProcessChannelMode(QProcess::ForwardedChannels);
    // On machines with mixed cores, the client goes on the fastest ones; the
    // scheduler would otherwise use efficiency cores. Prepared before the fork.
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
        const bool never_started = product == stud::ui::client::Product::Player &&
                                   !g_client_app_started.load(std::memory_order_relaxed);
        const bool crashed =
            !g_stop_requested && (status == QProcess::CrashExit || code != 0 || never_started);
        if (crashed && never_started && status != QProcess::CrashExit && code == 0) {
            std::printf("stud: Roblox closed before its window opened\n");
        }
        std::printf("stud: Roblox exited (%s %d)\n",
                    status == QProcess::CrashExit ? "signal" : "code", code);
        if (g_activity != nullptr) g_activity->left();
        stud::ui::login::seal(product);
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
    // Close our copy so the reader sees EOF when the client and its children exit.
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
        // The started process is the wrapper, not the client's main thread.
        std::printf("stud: Roblox starts through a wrapper; its main thread keeps its priority\n");
    }
    QTimer::singleShot(3000, qApp, [] { stud::ui::close_loading_screen(); });
    return true;
}

// Every Rosemary process of Stud's prefix (rosemary with Stud's ROSEMARY_PREFIX):
// the client plus launchd, daemons and the process server.
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

void terminate_stud_session() {
    if (g_client == nullptr) return;
    g_stop_requested = true;
    // Rosemary's services outlive the client and would serve the next session
    // stale state, so every process of the prefix is stopped with it.
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
    // Looked up again: a pid may have been reused while we waited.
    for (const pid_t pid : client_process_ids()) ::kill(pid, SIGKILL);
    if (g_client->state() != QProcess::NotRunning) {
        g_client->kill();
        g_client->waitForFinished(2000);
    }
}

bool stud_session_is_running() {
    if (g_client != nullptr) return g_client->state() != QProcess::NotRunning;
    return another_instance_is_running();
}

void relaunch_stud() {
    g_relaunch_pending = true;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("STUD_LOG_FILE"));
    if (g_layers_disable_was_set) {
        env.insert(QStringLiteral("VK_LOADER_LAYERS_DISABLE"), g_host_layers_disable);
    } else if (::getenv("MANGOHUD") != nullptr || ::getenv("MANGOHUD_CONFIG") != nullptr) {
        env.remove(QStringLiteral("VK_LOADER_LAYERS_DISABLE"));
    }
    const QString program = stud::ui::launcher_command();
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp, [program, env]() {
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

void show_crash_dialog(const QString& which_process) {
    const QString log = QString::fromUtf8(qgetenv("STUD_LOG_FILE"));
    qApp->setQuitOnLastWindowClosed(false);

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
        QMetaObject::invokeMethod(qApp, &QApplication::quit, Qt::QueuedConnection);
    });
    box->exec();
}

}  // namespace stud::ui

int main(int argc, char** argv) {
    keep_mangohud_out_of_this_process();

    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(":/stud-logo-color.png"));
    // Wayland finds the icon through app_id; the client reports the same id.
    QGuiApplication::setDesktopFileName(QStringLiteral(STUD_APP_ID));
    QCoreApplication::setApplicationName(QStringLiteral("Stud"));

    // Remove Stud 1.x's Android engine, APK and caches; its settings, flags,
    // customizations and logs stay.
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
    // QtWebEngine's profile, unused since Rosemary shows web views with WPE WebKit.
    for (const std::string& dir : {stud::paths::data_dir(), stud::paths::cache_dir()}) {
        QDir(QString::fromStdString(dir) + QStringLiteral("/webview")).removeRecursively();
    }

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

    // A running Stud owns the Settings window; this hands the request over.
    if (argc > 1 && std::string(argv[1]) == "--settings") {
        if (stud::ui::SettingsWindow::handOffToRunningInstance()) return 0;
        stud::ui::SettingsWindow::listenForOpenRequests();
        stud::ui::SettingsWindow::showSingleton();
        return app.exec();
    }

    // Only the link's key names and sizes are logged: it holds a join ticket.
    // --roblox and --studio skip the welcome page's choice.
    std::optional<QString> deep_link;
    std::optional<stud::ui::client::Product> product;
    if (argc > 1 && std::string(argv[1]) == "--roblox") product = stud::ui::client::Product::Player;
    if (argc > 1 && std::string(argv[1]) == "--studio") product = stud::ui::client::Product::Studio;
    const std::optional<QString> place = argc > 1 ? place_file(argv[1]) : std::nullopt;
    if (place) {
        product = stud::ui::client::Product::Studio;
        deep_link = place;
        std::printf("stud: opening a place file in Studio\n");
    } else if (argc > 1 && std::string(argv[1]).rfind("roblox-studio", 0) == 0) {
        product = stud::ui::client::Product::Studio;
        deep_link = QString::fromUtf8(argv[1]);
        std::printf("stud: Studio link of %zu bytes\n", std::strlen(argv[1]));
    } else if (argc > 1 && argv[1][0] != '-') {
        product = stud::ui::client::Product::Player;
        if (stud::ui::is_roblox_link(argv[1])) {
            deep_link = QString::fromUtf8(argv[1]);
            std::printf("stud: deep link of %zu bytes\n", std::strlen(argv[1]));
        } else {
            std::printf("stud: the launch argument is not a Roblox deep link\n");
        }
    }

    stud::config::StudSettings settings;
    try {
        settings = stud::config::load_settings(stud::config::default_config_path());
    } catch (const stud::config::SettingsError& e) {
        std::fprintf(stderr, "%s; launching with default settings\n", e.what());
    }

    const bool welcome_page = !product && !settings.open_player_directly;
    if (!product) {
        product = welcome_page ? stud::ui::choose_product() : std::optional(stud::ui::client::Product::Player);
    }
    if (!product) {
        // Closed without a choice. An open Settings stays until it is closed.
        const QWidgetList windows = QApplication::topLevelWidgets();
        if (std::any_of(windows.begin(), windows.end(), [](const QWidget* w) { return w->isVisible(); })) {
            return app.exec();
        }
        return 0;
    }

    // The welcome page's Settings button may have changed them.
    if (welcome_page) {
        try {
            settings = stud::config::load_settings(stud::config::default_config_path());
        } catch (const stud::config::SettingsError&) {
        }
    }
    if (!launch_game(*product, deep_link, settings)) {
        const QWidgetList windows = QApplication::topLevelWidgets();
        if (std::none_of(windows.begin(), windows.end(),
                         [](const QWidget* w) { return w->isVisible(); })) {
            QMetaObject::invokeMethod(qApp, &QApplication::quit, Qt::QueuedConnection);
        }
        return app.exec();
    }

    // This process stays as the client's parent so it can report a crash.
    stud::ui::SettingsWindow::listenForOpenRequests();
    if (settings.system_tray) {
        auto* tray = new stud::ui::Tray(qApp);
        if (!tray->show()) std::fprintf(stderr, "stud: no system tray available\n");
    }
    app.setQuitOnLastWindowClosed(false);
    return app.exec();
}
