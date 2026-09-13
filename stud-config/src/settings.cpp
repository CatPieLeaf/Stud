#include "stud/settings.h"

#include <nlohmann/json.hpp>

#include <sys/stat.h>

#include <cstdlib>
#include <fstream>

namespace {

std::string parent_directory(const std::string& path) {
    size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

void make_directories(const std::string& path) {
    std::string current;
    for (char c : path) {
        if (c == '/' && !current.empty()) {
            ::mkdir(current.c_str(), 0755);
        }
        current += c;
    }
    if (!current.empty()) {
        ::mkdir(current.c_str(), 0755);
    }
}

}  // namespace

namespace stud::config {

StudSettings load_settings(const std::string& path) {
    StudSettings result;

    std::ifstream file(path);
    if (!file.is_open()) {
        return result;
    }

    nlohmann::json doc;
    try {
        file >> doc;
    } catch (const nlohmann::json::parse_error& e) {
        throw SettingsError("stud: malformed config file '" + path + "': " + e.what());
    }

    if (!doc.is_object()) {
        throw SettingsError("stud: config file '" + path + "' must be a JSON object");
    }

    if (doc.contains("gpu")) {
        const auto& gpu = doc.at("gpu");
        if (!gpu.is_object() || !gpu.contains("deviceIndex") || !gpu.at("deviceIndex").is_number_unsigned()) {
            throw SettingsError("stud: config '" + path +
                                 "' has an invalid \"gpu\" (expected an object with an unsigned "
                                 "\"deviceIndex\")");
        }
        result.gpu.device_index = gpu.at("deviceIndex").get<uint32_t>();
        if (gpu.contains("deviceName") && gpu.at("deviceName").is_string()) {
            result.gpu.device_name = gpu.at("deviceName").get<std::string>();
        }
    }

    if (doc.contains("hidpi")) {
        if (!doc.at("hidpi").is_boolean()) {
            throw SettingsError("stud: config '" + path + "' has a non-boolean \"hidpi\"");
        }
        result.hidpi = doc.at("hidpi").get<bool>();
    }

    if (doc.contains("followDpi")) {
        if (!doc.at("followDpi").is_boolean()) {
            throw SettingsError("stud: config '" + path + "' has a non-boolean \"followDpi\"");
        }
        result.follow_dpi = doc.at("followDpi").get<bool>();
    }
    // "uiScale" is read by nothing now: the density the engine lays out
    // against is fixed at 1.0, because anything above that turns off its
    // full render path (runtime/src/main.cpp says why). An older file
    // that still carries the key simply loses it on the next save.
    if (doc.contains("renderScale") && doc.at("renderScale").is_number()) {
        // Migration: renderScale briefly meant the buffer scale and the
        // layout scale at once. Only the buffer half still exists.
        const double scale = doc.at("renderScale").get<double>();
        result.hidpi = scale <= 0.0 || scale > 1.0;
    }
    if (doc.contains("upscaling")) {
        if (!doc.at("upscaling").is_boolean()) {
            throw SettingsError("stud: config '" + path + "' has a non-boolean \"upscaling\"");
        }
        result.upscaling = doc.at("upscaling").get<bool>();
    }
    if (doc.contains("upscaleQualityPercent")) {
        if (!doc.at("upscaleQualityPercent").is_number_integer()) {
            throw SettingsError("stud: config '" + path +
                                "' has a non-integer \"upscaleQualityPercent\"");
        }
        const int percent = doc.at("upscaleQualityPercent").get<int>();
        // Clamped rather than rejected. 33 is DLSS's own Ultra
        // Performance ratio and the floor below which there is nothing
        // left to reconstruct; 100 means rendering the output size, which
        // is upscaling nothing.
        result.upscale_quality_percent = percent < 33 ? 33 : (percent > 100 ? 100 : percent);
    }
    if (doc.contains("smoothZoom")) {
        if (!doc.at("smoothZoom").is_boolean()) {
            throw SettingsError("stud: config '" + path + "' has a non-boolean \"smoothZoom\"");
        }
        result.smooth_zoom = doc.at("smoothZoom").get<bool>();
    }
    if (doc.contains("backgroundFps")) {
        if (!doc.at("backgroundFps").is_number_integer()) {
            throw SettingsError("stud: config '" + path +
                                "' has a non-integer \"backgroundFps\"");
        }
        // Clamped rather than rejected: a hand-edited file is allowed to
        // be out of range, and the honest reading of 5000 is "unlimited"
        // rather than an error that stops Stud starting.
        const int value = doc.at("backgroundFps").get<int>();
        // 0 means no limit. Anything above the top of the range means
        // the same thing -- including the `241` older builds wrote,
        // which is why that is read back as unlimited rather than
        // clamped to a number.
        result.background_fps =
            (value <= 0 || value > kBackgroundFpsUnlimited) ? kBackgroundFpsNoLimit : value;
    }
    if (doc.contains("mangohud")) {
        if (!doc.at("mangohud").is_boolean()) {
            throw SettingsError("stud: config '" + path + "' has a non-boolean \"mangohud\"");
        }
        result.mangohud = doc.at("mangohud").get<bool>();
    }
    if (doc.contains("discordRichPresence")) {
        if (!doc.at("discordRichPresence").is_boolean()) {
            throw SettingsError("stud: config '" + path +
                                 "' has a non-boolean \"discordRichPresence\"");
        }
        result.discord_rich_presence = doc.at("discordRichPresence").get<bool>();
    }
    if (doc.contains("discordJoinButton")) {
        if (!doc.at("discordJoinButton").is_boolean()) {
            throw SettingsError("stud: config '" + path +
                                 "' has a non-boolean \"discordJoinButton\"");
        }
        result.discord_join_button = doc.at("discordJoinButton").get<bool>();
    }
    if (doc.contains("systemTray")) {
        if (!doc.at("systemTray").is_boolean()) {
            throw SettingsError("stud: config '" + path + "' has a non-boolean \"systemTray\"");
        }
        result.system_tray = doc.at("systemTray").get<bool>();
    }
    if (doc.contains("closeOnLeave")) {
        if (!doc.at("closeOnLeave").is_boolean()) {
            throw SettingsError("stud: config '" + path + "' has a non-boolean \"closeOnLeave\"");
        }
        result.close_on_leave = doc.at("closeOnLeave").get<bool>();
    }
    if (doc.contains("serverRegionNotification")) {
        if (!doc.at("serverRegionNotification").is_boolean()) {
            throw SettingsError("stud: config '" + path +
                                 "' has a non-boolean \"serverRegionNotification\"");
        }
        result.server_region_notification = doc.at("serverRegionNotification").get<bool>();
    }
    if (doc.contains("textureCache")) {
        if (!doc.at("textureCache").is_boolean()) {
            throw SettingsError("stud: config '" + path + "' has a non-boolean \"textureCache\"");
        }
        result.texture_cache = doc.at("textureCache").get<bool>();
    }
    if (doc.contains("textureCacheMB")) {
        if (!doc.at("textureCacheMB").is_number_integer()) {
            throw SettingsError("stud: config '" + path + "' has a non-integer \"textureCacheMB\"");
        }
        const int value = doc.at("textureCacheMB").get<int>();
        result.texture_cache_mb = value < 0 ? 0 : value;
    }
    if (doc.contains("graphicsMode")) {
        if (!doc.at("graphicsMode").is_string()) {
            throw SettingsError("stud: config '" + path + "' has a non-string \"graphicsMode\"");
        }
        std::string mode = doc.at("graphicsMode").get<std::string>();
        if (mode == "vulkan") {
            result.graphics_mode = GraphicsMode::kVulkan;
        } else if (mode == "opengl") {
            result.graphics_mode = GraphicsMode::kOpenGL;
        } else {
            throw SettingsError("stud: config '" + path + "' has an unrecognized \"graphicsMode\" \"" +
                                 mode + "\" (expected \"vulkan\" or \"opengl\")");
        }
    }

    return result;
}

void save_settings(const std::string& path, const StudSettings& settings) {
    nlohmann::json doc = nlohmann::json::object();

    // Preserve any other top-level keys already in the file (e.g.
    // render/dev_backend_config.h's "devRenderBackend") -- this file is
    // shared, not exclusively owned by this module. A missing or
    // malformed existing file is not fatal here -- start fresh rather
    // than blocking a save on an unrelated pre-existing problem.
    {
        std::ifstream existing(path);
        if (existing.is_open()) {
            try {
                nlohmann::json existing_doc;
                existing >> existing_doc;
                if (existing_doc.is_object()) {
                    doc = existing_doc;
                }
            } catch (const nlohmann::json::parse_error&) {
                // Fall through with a fresh object -- an unrelated
                // malformed file shouldn't block saving real settings.
            }
        }
    }

    doc["gpu"] = {{"deviceIndex", settings.gpu.device_index}, {"deviceName", settings.gpu.device_name}};
    doc["hidpi"] = settings.hidpi;

    doc["followDpi"] = settings.follow_dpi;
    // Written as a plain multiplier rather than 120ths: this file is meant
    // to be hand-editable, and "1.25" is what a person means.
    doc.erase("renderScale");
    doc["upscaling"] = settings.upscaling;
    doc.erase("upscaleOutputPercent");
    doc["upscaleQualityPercent"] = settings.upscale_quality_percent;
    doc["smoothZoom"] = settings.smooth_zoom;
    doc["backgroundFps"] = settings.background_fps;
    doc["mangohud"] = settings.mangohud;
    doc["discordRichPresence"] = settings.discord_rich_presence;
    doc["discordJoinButton"] = settings.discord_join_button;
    doc["systemTray"] = settings.system_tray;
    doc["closeOnLeave"] = settings.close_on_leave;
    doc["serverRegionNotification"] = settings.server_region_notification;
    doc["textureCache"] = settings.texture_cache;
    doc["textureCacheMB"] = settings.texture_cache_mb;
    doc["graphicsMode"] = settings.graphics_mode == GraphicsMode::kVulkan ? "vulkan" : "opengl";
    // Keys that are nobody's setting any more. Erased rather than left
    // alone, because a file that still lists them reads as if they do
    // something: apkPath/apkName (Stud keeps one APK of its own, at a
    // fixed path -- see settings.h), uiScale (there is no such control;
    // the layout density is fixed at 1.0), needsBionicFallback (from an
    // architecture that no longer exists), and renderScale (split into
    // hidpi and followDpi).
    doc.erase("apkPath");
    doc.erase("apkName");
    doc.erase("uiScale");
    doc.erase("needsBionicFallback");

    make_directories(parent_directory(path));

    std::ofstream out(path);
    if (!out.is_open()) {
        throw SettingsError("stud: failed to open '" + path + "' for writing");
    }
    out << doc.dump(2);
}

std::string default_config_path() {
    if (const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME")) {
        return std::string(xdg_config_home) + "/stud/config.json";
    }
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") + "/.config/stud/config.json";
}

std::string default_flag_overrides_path() {
    if (const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME")) {
        return std::string(xdg_config_home) + "/stud/flags.json";
    }
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") + "/.config/stud/flags.json";
}

}  // namespace stud::config
