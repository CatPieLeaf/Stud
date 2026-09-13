#include "diagnose.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QString>

#include <array>
#include <cstdio>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <gnu/libc-version.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "stud/android_glue.h"
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

}  // namespace

int run_diagnose() {
    std::printf("Stud %s\n\n", STUD_VERSION);

    utsname uts{};
    ::uname(&uts);

    std::printf("system\n");
    line("kernel", std::string(uts.sysname) + " " + uts.release + " " + uts.machine);
    line("glibc", ::gnu_get_libc_version());
    line("session", qEnvironmentVariable("XDG_SESSION_TYPE").toStdString());
    line("desktop", qEnvironmentVariable("XDG_CURRENT_DESKTOP").toStdString());
    line("wayland display", qEnvironmentVariable("WAYLAND_DISPLAY").toStdString());
    line("x11 display", qEnvironmentVariable("DISPLAY").toStdString());
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
    return 0;
}

}  // namespace stud::ui
