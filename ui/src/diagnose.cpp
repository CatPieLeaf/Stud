#include "diagnose.h"

#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QString>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <utility>
#include <string>
#include <vector>

#include <dirent.h>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#include <grp.h>
#include <fcntl.h>
#include <gnu/libc-version.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "mac_client.h"
#include "stud_commit.h"
#include "gpu_enum.h"
#include "stud/settings.h"
#include "stud/stud_paths.h"

namespace stud::ui {

namespace {

// What is being written: the log's header, or the text Settings copies.
std::string g_text;

void out(const char* format, ...) __attribute__((format(printf, 1, 2)));
void out(const char* format, ...) {
    va_list args;
    va_start(args, format);
    va_list again;
    va_copy(again, args);
    const int size = std::vsnprintf(nullptr, 0, format, args);
    va_end(args);
    if (size > 0) {
        std::string piece(static_cast<size_t>(size) + 1, '\0');
        std::vsnprintf(piece.data(), piece.size(), format, again);
        piece.resize(static_cast<size_t>(size));
        g_text += piece;
    }
    va_end(again);
}

void line(const char* label, const std::string& value) {
    out("  %-22s %s\n", label, value.empty() ? "(none)" : value.c_str());
}

std::string yes_no(bool value) { return value ? "yes" : "no"; }

std::string exists_size(const std::string& path) {
    QFileInfo info(QString::fromStdString(path));
    if (!info.exists()) return "missing";
    if (info.isDir()) return "present (directory)";
    return "present (" + std::to_string(info.size()) + " bytes)";
}

// Controllers as the kernel lists them, and whether this user can read
// them.
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
            out("  %-22s %s (%s, rumble %s)\n", found == 0 ? "controllers" : "",
                        name[0] != '\0' ? name : "unnamed", path.c_str(), yes_no(rumble).c_str());
            ++found;
        }
        ::close(fd);
    }
    ::closedir(dir);
    if (found == 0) {
        line("controllers", unreadable > 0
                                ? "none (" + std::to_string(unreadable) +
                                      " device(s) unreadable. This user may need the `input` group)"
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
    // a wider path, the raw flag line is over 200 entries and says
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
        // Reported in kB; megabytes is what every other memory number in
        // the header is in.
        try {
            line("memory", std::to_string(std::stoll(mem) / 1024) + " MB");
        } catch (const std::exception&) {
            line("memory", mem);
        }
    }
}

// Every GPU the real Vulkan loader reports, not just the selected one,
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
                                   ", NOT FOUND now; the loader reports " +
                                   std::to_string(gpus.size()) + " device(s)");
        }
        bool first = true;
        for (const auto& gpu : gpus) {
            if (gpu.device_index == settings.gpu.device_index) continue;  // named above
            out("  %-22s [%u] %s\n", first ? "also present" : "", gpu.device_index,
                        gpu.name.c_str());
            first = false;
        }
    }
    // The kernel's own view, which is what says whether a driver is
    // even loaded for a device Vulkan did not report.
    std::string nodes;
    if (DIR* dir = ::opendir("/dev/dri")) {
        while (const dirent* entry = ::readdir(dir)) {
            const std::string name = entry->d_name;
            if (name.rfind("render", 0) != 0 && name.rfind("card", 0) != 0) continue;
            if (!nodes.empty()) nodes += ", ";
            nodes += name;
            // A render node that exists is no use if this account cannot open
            // it: Mesa then falls back to drawing in software.
            if (name.rfind("render", 0) == 0) {
                const std::string path = "/dev/dri/" + name;
                const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
                if (fd >= 0) {
                    ::close(fd);
                } else {
                    const int why = errno;
                    struct stat st {};
                    const group* owner = ::stat(path.c_str(), &st) == 0 ? ::getgrgid(st.st_gid) : nullptr;
                    nodes += std::string(" (cannot open: ") + std::strerror(why) +
                             (owner ? std::string(", group ") + owner->gr_name : std::string()) + ")";
                }
            }
        }
        ::closedir(dir);
    }
    line("drm nodes", nodes);
}

}  // namespace

// Rosemary's own version and commit, as `rosemary --version` reports them:
// "0.5.0, e5a57fc". Empty when it does not answer.
static std::string rosemary_version(const QString& rosemary) {
    if (rosemary.isEmpty()) return {};
    QProcess process;
    process.start(rosemary, {QStringLiteral("--version")});
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished();
        return {};
    }
    QString version;
    QString commit;
    for (const QString& raw : QString::fromUtf8(process.readAllStandardOutput()).split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("rosemary "))) version = line.mid(9);
        if (line.startsWith(QLatin1String("commit:"))) commit = line.mid(7).trimmed();
    }
    if (version.isEmpty()) return {};
    return (commit.isEmpty() ? version : version + QStringLiteral(", ") + commit).toStdString();
}

void build_diagnostics() {
    // The commit too: every build of a release is called by the release's
    // version, and only the commit says which code ran.
    if (std::string(STUD_COMMIT).empty()) {
        out("=== Stud %s ===\n\n", STUD_VERSION);
    } else {
        out("=== Stud %s (%s) ===\n\n", STUD_VERSION, STUD_COMMIT);
    }

    utsname uts{};
    ::uname(&uts);

    out("system\n");
    line("kernel", std::string(uts.sysname) + " " + uts.release + " " + uts.machine);
    line("glibc", ::gnu_get_libc_version());
    line("session", qEnvironmentVariable("XDG_SESSION_TYPE").toStdString());
    line("desktop", qEnvironmentVariable("XDG_CURRENT_DESKTOP").toStdString());
    line("wayland display", qEnvironmentVariable("WAYLAND_DISPLAY").toStdString());
    line("x11 display", qEnvironmentVariable("DISPLAY").toStdString());
    report_cpu();
    line("appimage", qEnvironmentVariable("APPIMAGE").toStdString());
    out("\n");

    out("paths\n");
    line("config", stud::paths::config_dir());
    line("data", stud::paths::data_dir());
    line("cache", stud::paths::cache_dir());
    line("logs", stud::paths::log_dir());
    line("config file", exists_size(stud::config::default_config_path()));
    line("flag overrides", exists_size(stud::config::default_flag_overrides_path()));
    out("\n");

    out("configuration\n");
    stud::config::StudSettings settings;
    std::string settings_error;
    try {
        settings = stud::config::load_settings(stud::config::default_config_path());
    } catch (const std::exception& e) {
        settings_error = e.what();
    }
    // What the launch will really run on, not what the file says.
    settings.gpu.device_index = stud::ui::chosen_gpu_index(settings.gpu);
    if (!settings_error.empty()) {
        line("settings", "unreadable: " + settings_error);
    } else {
        line("gpu", settings.gpu.device_name);
        line("renderer", settings.renderer);
        line("presentation", settings.presentation);
        line("title bar", settings.title_bar);
        line("display backend", settings.display_backend);
        line("background fps", settings.background_fps == stud::config::kBackgroundFpsNoLimit
                                   ? std::string("unlimited")
                                   : std::to_string(settings.background_fps));
        line("mangohud", yes_no(settings.mangohud));
        line("discord presence", yes_no(settings.discord_rich_presence));
        line("server region", yes_no(settings.server_region_notification));
        line("gamemode", yes_no(settings.gamemode));
        line("system tray", yes_no(settings.system_tray));
    }
    out("\n");

    out("graphics\n");
    report_gpus(settings);
    out("\n");

    out("client\n");
    for (const auto product : {stud::ui::client::Product::Player, stud::ui::client::Product::Studio}) {
        const auto build = stud::ui::client::installed(product);
        line(product == stud::ui::client::Product::Studio ? "studio" : "roblox",
             build ? (build->version + " (" + build->upload + ")").toStdString() : "");
    }
    {
        const QString rosemary = stud::ui::client::find_rosemary();
        const std::string version = rosemary_version(rosemary);
        line("rosemary", version.empty() ? rosemary.toStdString()
                                         : rosemary.toStdString() + " (" + version + ")");
    }
    line("prefix", exists_size(stud::ui::client::prefix_dir().toStdString() + "/root"));
    out("\n");

    out("input\n");
    report_controllers();
    out("\n");

    out("Logs from the last few sessions are in %s, the Settings window's\n"
                "\"Export logs\" entry packs them into a single archive.\n",
                stud::paths::log_dir().c_str());
    out("\n=== end of diagnostics ===\n\n");
    std::fflush(stdout);
}

void write_diagnostics() {
    g_text.clear();
    build_diagnostics();
    std::fputs(g_text.c_str(), stdout);
    std::fflush(stdout);
}

std::string diagnostics_text() {
    g_text.clear();
    build_diagnostics();
    return std::move(g_text);
}

}  // namespace stud::ui
