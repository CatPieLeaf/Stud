#include "diagnose.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QString>

#include <array>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <utility>
#include <string>
#include <vector>

#include <dirent.h>
#include <cstdlib>
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
    // Only the sets that decide whether a build can run at all or take
    // a wider path -- the raw flag line is over 200 entries and says
    // nothing useful in a report.
    {
        const std::string flags = " " + proc_field("/proc/cpuinfo", "flags") + " ";
        static const char* const kInteresting[] = {"sse4_2", "avx",  "avx2",     "avx512f",
                                                   "fma",    "aes",  "pclmulqdq", "bmi2",
                                                   "f16c",   "sha_ni"};
        std::string present;
        for (const char* name : kInteresting) {
            if (flags.find(std::string(" ") + name + " ") == std::string::npos) continue;
            if (!present.empty()) present += " ";
            present += name;
        }
        line("cpu instructions", present);
    }
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
        line("gpu in use", "none (no Vulkan-capable device or loader)");
    } else {
        // What Stud is actually running on, first and by itself. The
        // index is what the setting stores, so a saved choice that no
        // longer exists shows up as a mismatch rather than silently
        // becoming device 0.
        const stud::ui::GpuInfo* chosen = nullptr;
        for (const auto& gpu : gpus) {
            if (gpu.device_index == settings.gpu.device_index) chosen = &gpu;
        }
        if (chosen != nullptr) {
            line("gpu in use", "[" + std::to_string(chosen->device_index) + "] " + chosen->name);
        } else {
            line("gpu in use", "[" + std::to_string(settings.gpu.device_index) + "] " +
                                   settings.gpu.device_name +
                                   " -- NOT FOUND now; the loader reports " +
                                   std::to_string(gpus.size()) + " device(s)");
        }
        bool first = true;
        for (const auto& gpu : gpus) {
            if (gpu.device_index == settings.gpu.device_index) continue;  // named above
            std::printf("  %-22s [%u] %s\n", first ? "also present" : "", gpu.device_index,
                        gpu.name.c_str());
            first = false;
        }
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

// Which compressed texture formats this GPU can sample, and what that
// costs when it cannot.
//
// Roblox ships its Android content in ETC2/EAC, which no desktop GPU can
// sample. Stud decodes those blocks on the CPU (detex) and re-encodes
// them into a BC format the device does support, so this line explains
// both a chunk of CPU use and any complaint about texture quality --
// BC7 keeps normal maps clean where BC1 turns them blocky.
void report_texture_formats(const stud::config::StudSettings& settings) {
    const auto support = stud::ui::query_texture_formats(settings.gpu.device_index);
    if (!support.queried) {
        line("texture formats", "could not query the selected device");
        return;
    }
    const auto join = [](std::initializer_list<std::pair<const char*, bool>> items, bool want) {
        std::string out;
        for (const auto& [name, present] : items) {
            if (present != want) continue;
            if (!out.empty()) out += " ";
            out += name;
        }
        return out;
    };
    const std::initializer_list<std::pair<const char*, bool>> all = {
        {"ETC2", support.etc2},   {"EAC", support.eac},   {"ASTC", support.astc_ldr},
        {"PVRTC", support.pvrtc}, {"BC1", support.bc1},   {"BC3", support.bc3},
        {"BC4/BC5", support.bc4_bc5}, {"BC7", support.bc7}};
    line("gpu texture formats", join(all, true));
    line("not supported", join(all, false));
    // What Stud actually does about it, which is the part worth reading.
    const bool content_native = support.etc2 && support.eac;
    if (content_native) {
        line("texture handling", "the APK's own ETC2/EAC formats upload directly");
    } else if (support.bc7) {
        line("texture handling",
             "ETC2/EAC decoded on the CPU, re-encoded to BC7 (colour) and BC4/BC5");
    } else if (support.bc1) {
        line("texture handling",
             "ETC2/EAC decoded on the CPU, re-encoded to BC1/BC3 -- no BC7 on this device");
    } else {
        line("texture handling",
             "ETC2/EAC decoded on the CPU and stored UNCOMPRESSED -- no BC support, expect "
             "high texture memory use");
    }
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
    const std::string allocator = preloaded_allocators();
    line("preloaded allocator", allocator);
    if (!allocator.empty()) {
        // Worth stating rather than leaving to be noticed: a replacement
        // allocator forced in through LD_PRELOAD applies to Stud's glibc
        // processes but not to Process B, which runs real bionic with
        // its own allocator and an engine that links mimalloc
        // statically. Two machines running the same build can behave
        // differently because of this, and hardened_malloc in
        // particular is strict enough to matter.
        std::printf("  %-22s %s\n", "",
                    "note: this replaces the allocator in Stud's glibc processes only -- "
                    "Process B keeps bionic's");
    }
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
    report_texture_formats(settings);
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
