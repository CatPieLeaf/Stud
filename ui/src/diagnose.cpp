#include "diagnose.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QString>

#include <array>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <cstdlib>
#include <dlfcn.h>
#include <fcntl.h>
#include <gnu/libc-version.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "stud/android_glue.h"
#include "gpu_enum.h"
#include "stud/settings.h"
#include "stud/stud_paths.h"

namespace stud::ui {

namespace {

void line(const char* label, const std::string& value) {
    std::printf("  %-22s %s\n", label, value.empty() ? "(none)" : value.c_str());
}

std::string yes_no(bool value) { return value ? "yes" : "no"; }

std::string exists_size(const std::string& path) {
    QFileInfo info(QString::fromStdString(path));
    if (!info.exists()) return "missing";
    if (info.isDir()) return "present (directory)";
    return "present (" + std::to_string(info.size()) + " bytes)";
}

// The allocator a distribution preloads matters here in a way it does
// not for an ordinary application: Process B runs real bionic code with
// its own allocator, and libroblox links mimalloc statically. A
// replacement allocator forced in through LD_PRELOAD applies to the
// glibc processes only, which is a real difference between two machines
// running the same build and is worth stating rather than guessing at.
std::string preloaded_allocators() {
    const QString preload = qEnvironmentVariable("LD_PRELOAD");
    if (preload.isEmpty()) return {};
    QString found;
    for (const QString& part : preload.split(QLatin1Char(':'), Qt::SkipEmptyParts)) {
        const QString base = QFileInfo(part).fileName();
        if (base.contains(QLatin1String("malloc")) || base.contains(QLatin1String("jemalloc")) ||
            base.contains(QLatin1String("tcmalloc")) || base.contains(QLatin1String("mimalloc"))) {
            if (!found.isEmpty()) found += QLatin1String(", ");
            found += base;
        }
    }
    return found.toStdString();
}

// Controllers, read the same way render-host reads them, so the report
// says what Stud itself would find rather than what the desktop shows.
void report_controllers() {
    DIR* dir = ::opendir("/dev/input");
    if (dir == nullptr) {
        line("controllers", "/dev/input unreadable");
        return;
    }
    int unreadable = 0;
    int found = 0;
    while (dirent* entry = ::readdir(dir)) {
        if (std::string(entry->d_name).rfind("event", 0) != 0) continue;
        const std::string path = std::string("/dev/input/") + entry->d_name;
        const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (errno == EACCES) ++unreadable;
            continue;
        }
        std::array<unsigned long, (KEY_MAX / (8 * sizeof(unsigned long))) + 1> keys{};
        std::array<unsigned long, (ABS_MAX / (8 * sizeof(unsigned long))) + 1> abs{};
        std::array<unsigned long, (FF_MAX / (8 * sizeof(unsigned long))) + 1> ff{};
        const auto bit = [](const auto& bits, int b) {
            return (bits[b / (8 * sizeof(unsigned long))] >> (b % (8 * sizeof(unsigned long)))) & 1ul;
        };
        if (::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys.data()) >= 0 &&
            ::ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs.data()) >= 0 && bit(keys, BTN_SOUTH) &&
            bit(abs, ABS_X) && bit(abs, ABS_Y)) {
            char name[256] = {0};
            ::ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
            const bool rumble = ::ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff)), ff.data()) >= 0 &&
                                bit(ff, FF_RUMBLE);
            std::printf("  %-22s %s (%s, rumble %s)\n", found == 0 ? "controllers" : "",
                        name[0] != '\0' ? name : "unnamed", path.c_str(), yes_no(rumble).c_str());
            ++found;
        }
        ::close(fd);
    }
    ::closedir(dir);
    if (found == 0) {
        line("controllers", unreadable > 0
                                ? "none (" + std::to_string(unreadable) +
                                      " device(s) unreadable -- this user may need the `input` group)"
                                : "none");
    }
}


// One field out of a "key: value" file, which is the shape of both
// /proc/cpuinfo and /proc/meminfo.
std::string proc_field(const char* path, const char* key) {
    std::ifstream file(path);
    std::string text;
    const std::string needle(key);
    while (std::getline(file, text)) {
        if (text.rfind(needle, 0) != 0) continue;
        const auto colon = text.find(':');
        if (colon == std::string::npos) continue;
        std::string value = text.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\n')) value.pop_back();
        return value;
    }
    return {};
}

void report_cpu() {
    std::string model = proc_field("/proc/cpuinfo", "model name");
    if (model.empty()) model = proc_field("/proc/cpuinfo", "Model");
    line("cpu", model);
    const long threads = ::sysconf(_SC_NPROCESSORS_ONLN);
    line("cpu threads", threads > 0 ? std::to_string(threads) : std::string());
    const std::string mem = proc_field("/proc/meminfo", "MemTotal");
    if (!mem.empty()) {
        // Reported in kB; megabytes is what every other memory number
        // in this project is in, including the one the engine is told.
        try {
            line("memory", std::to_string(std::stoll(mem) / 1024) + " MB");
        } catch (const std::exception&) {
            line("memory", mem);
        }
    }
}

// Every GPU the real Vulkan loader reports, not just the selected one --
// a hybrid laptop is the common case and which one Stud picked is
// usually the first question.
void report_gpus(const stud::config::StudSettings& settings) {
    const auto gpus = stud::ui::enumerate_gpus();
    if (gpus.empty()) {
        line("vulkan gpus", "none (no Vulkan-capable device or loader)");
    }
    for (const auto& gpu : gpus) {
        const bool selected = gpu.device_index == settings.gpu.device_index;
        std::printf("  %-22s [%u] %s%s\n", gpu.device_index == gpus.front().device_index
                                                ? "vulkan gpus" : "",
                    gpu.device_index, gpu.name.c_str(), selected ? "  <- selected" : "");
    }
    // The kernel's own view, which is what says whether a driver is
    // even loaded for a device Vulkan did not report.
    std::string nodes;
    if (DIR* dir = ::opendir("/dev/dri")) {
        while (dirent* entry = ::readdir(dir)) {
            const std::string name = entry->d_name;
            if (name.rfind("render", 0) != 0 && name.rfind("card", 0) != 0) continue;
            if (!nodes.empty()) nodes += ", ";
            nodes += name;
        }
        ::closedir(dir);
    }
    line("drm nodes", nodes);
}

// Hardware video decoders, asked of VA-API directly.
//
// libva is dlopen'd rather than linked: a machine without it is a
// machine with no VA-API, which is a fact worth reporting rather than a
// reason for Stud not to start. Reported because Stud has no MediaCodec
// and honestly tells the engine so -- when a video in an experience
// fails to load, what the GPU can actually decode is the first thing
// anyone asks.
void report_decoders() {
    // libva prints its own driver-probing chatter to stderr, which
    // lands in the middle of this report. Silenced for the query.
    ::setenv("LIBVA_MESSAGING_LEVEL", "0", 1);
    void* va = ::dlopen("libva.so.2", RTLD_LAZY);
    void* va_drm = ::dlopen("libva-drm.so.2", RTLD_LAZY);
    if (va == nullptr || va_drm == nullptr) {
        line("hw decoders", "VA-API not installed");
        if (va != nullptr) ::dlclose(va);
        if (va_drm != nullptr) ::dlclose(va_drm);
        return;
    }
    using GetDisplayDrmFn = void* (*)(int);
    using InitializeFn = int (*)(void*, int*, int*);
    using MaxProfilesFn = int (*)(void*);
    using QueryProfilesFn = int (*)(void*, int*, int*);
    using ProfileStrFn = const char* (*)(int);
    using TerminateFn = int (*)(void*);
    auto get_display = reinterpret_cast<GetDisplayDrmFn>(::dlsym(va_drm, "vaGetDisplayDRM"));
    auto initialize = reinterpret_cast<InitializeFn>(::dlsym(va, "vaInitialize"));
    auto max_profiles = reinterpret_cast<MaxProfilesFn>(::dlsym(va, "vaMaxNumProfiles"));
    auto query_profiles = reinterpret_cast<QueryProfilesFn>(::dlsym(va, "vaQueryConfigProfiles"));
    auto profile_str = reinterpret_cast<ProfileStrFn>(::dlsym(va, "vaProfileStr"));
    auto terminate = reinterpret_cast<TerminateFn>(::dlsym(va, "vaTerminate"));
    if (get_display == nullptr || initialize == nullptr || query_profiles == nullptr) {
        line("hw decoders", "VA-API present but incomplete");
        ::dlclose(va);
        ::dlclose(va_drm);
        return;
    }

    std::string reported;
    for (int index = 128; index < 132 && reported.empty(); ++index) {
        const std::string node = "/dev/dri/renderD" + std::to_string(index);
        const int fd = ::open(node.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        void* display = get_display(fd);
        int major = 0;
        int minor = 0;
        if (display != nullptr && initialize(display, &major, &minor) == 0) {
            const int capacity = max_profiles != nullptr ? max_profiles(display) : 64;
            std::vector<int> profiles(static_cast<size_t>(capacity > 0 ? capacity : 64));
            int count = 0;
            if (query_profiles(display, profiles.data(), &count) == 0) {
                for (int i = 0; i < count; ++i) {
                    const char* name = profile_str != nullptr ? profile_str(profiles[i]) : nullptr;
                    if (name == nullptr) continue;
                    if (!reported.empty()) reported += ", ";
                    reported += name;
                }
            }
            std::printf("  %-22s %s (libva %d.%d)\n", "vaapi", node.c_str(), major, minor);
            if (terminate != nullptr) terminate(display);
        }
        ::close(fd);
    }
    line("hw decoders", reported.empty() ? "none reported" : reported);
    ::dlclose(va);
    ::dlclose(va_drm);
}

}  // namespace

void write_diagnostics() {
    std::printf("=== Stud %s ===\n\n", STUD_VERSION);

    utsname uts{};
    ::uname(&uts);

    std::printf("system\n");
    line("kernel", std::string(uts.sysname) + " " + uts.release + " " + uts.machine);
    line("glibc", ::gnu_get_libc_version());
    line("session", qEnvironmentVariable("XDG_SESSION_TYPE").toStdString());
    line("desktop", qEnvironmentVariable("XDG_CURRENT_DESKTOP").toStdString());
    line("wayland display", qEnvironmentVariable("WAYLAND_DISPLAY").toStdString());
    line("x11 display", qEnvironmentVariable("DISPLAY").toStdString());
    report_cpu();
    line("preloaded allocator", preloaded_allocators());
    line("appimage", qEnvironmentVariable("APPIMAGE").toStdString());
    std::printf("\n");

    std::printf("paths\n");
    line("config", stud::paths::config_dir());
    line("data", stud::paths::data_dir());
    line("cache", stud::paths::cache_dir());
    line("logs", stud::paths::log_dir());
    line("config file", exists_size(stud::config::default_config_path()));
    line("flag overrides", exists_size(stud::config::default_flag_overrides_path()));
    std::printf("\n");

    std::printf("configuration\n");
    stud::config::StudSettings settings;
    std::string settings_error;
    try {
        settings = stud::config::load_settings(stud::config::default_config_path());
    } catch (const std::exception& e) {
        settings_error = e.what();
    }
    if (!settings_error.empty()) {
        line("settings", "unreadable: " + settings_error);
    } else {
        line("render path",
             settings.graphics_mode == stud::config::GraphicsMode::kVulkan ? "vulkan" : "opengl");
        line("gpu", settings.gpu.device_name);
        line("hidpi", yes_no(settings.hidpi));
        line("background fps", std::to_string(settings.background_fps));
        line("mangohud", yes_no(settings.mangohud));
        line("system tray", yes_no(settings.system_tray));
        line("discord presence", yes_no(settings.discord_rich_presence));
        line("apk", settings.apk_path);
        if (!settings.apk_path.empty()) {
            line("apk file", exists_size(settings.apk_path));
            line("apk version", stud::android_glue::apk_version_name(settings.apk_path));
        }
    }
    std::printf("\n");

    std::printf("graphics\n");
    report_gpus(settings);
    report_decoders();
    std::printf("\n");

    std::printf("engine\n");
    line("extracted libroblox", exists_size(stud::paths::cache_dir() + "/libroblox.so"));
    line("engine files", exists_size(stud::paths::engine_files_dir()));
    line("engine cache", exists_size(stud::paths::engine_cache_dir()));
    std::printf("\n");

    std::printf("input\n");
    report_controllers();
    std::printf("\n");

    // The login is reported as present or absent only. The value is a
    // real credential and has no business in a report meant to be
    // pasted into an issue.
    std::printf("Logs from the last few sessions are in %s -- the Settings window's\n"
                "\"Export logs\" button packs them into a single archive.\n",
                stud::paths::log_dir().c_str());
    std::printf("\n=== end of diagnostics ===\n\n");
    std::fflush(stdout);
}

}  // namespace stud::ui
