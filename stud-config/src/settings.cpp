#include "stud/settings.h"

#include <nlohmann/json.hpp>

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>

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

bool waits_for_refresh(const StudSettings& settings) {
    return settings.presentation == "vsync" || settings.presentation == "adaptive";
}

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
        if (gpu.contains("deviceUuid") && gpu.at("deviceUuid").is_string()) {
            result.gpu.device_uuid = gpu.at("deviceUuid").get<std::string>();
        }
    }

    // A key with the wrong type is an error, said out loud; a missing key
    // keeps its default.
    const auto read_bool = [&](const char* key, bool& out) {
        if (!doc.contains(key)) return;
        if (!doc.at(key).is_boolean()) {
            throw SettingsError("stud: config '" + path + "' has a non-boolean \"" + key + "\"");
        }
        out = doc.at(key).get<bool>();
    };
    const auto read_text = [&](const char* key, std::string& out) {
        if (!doc.contains(key)) return;
        if (!doc.at(key).is_string()) {
            throw SettingsError("stud: config '" + path + "' has a non-string \"" + key + "\"");
        }
        out = doc.at(key).get<std::string>();
    };
    const auto read_string = [&](const char* key, std::string& out,
                                 std::initializer_list<const char*> allowed) {
        if (!doc.contains(key)) return;
        if (!doc.at(key).is_string()) {
            throw SettingsError("stud: config '" + path + "' has a non-string \"" + key + "\"");
        }
        const std::string value = doc.at(key).get<std::string>();
        for (const char* option : allowed) {
            if (value == option) {
                out = value;
                return;
            }
        }
        throw SettingsError("stud: config '" + path + "' has an unrecognized \"" + key +
                            "\" \"" + value + "\"");
    };

    // Stud 1.x's names for the two settings that carried over to 2.0: its
    // graphics mode is the renderer, and its upscaler choice the filter. Taken
    // only where the file has no 2.0 value of its own, and only a value 2.0
    // knows; the next save drops the old keys.
    if (!doc.contains("renderer") && doc.contains("graphicsMode") && doc.at("graphicsMode").is_string()) {
        const std::string mode = doc.at("graphicsMode").get<std::string>();
        if (mode == "vulkan" || mode == "opengl") doc["renderer"] = mode;
    }
    if (!doc.contains("upscaler") && doc.contains("upscalerChoice") && doc.at("upscalerChoice").is_string()) {
        const std::string choice = doc.at("upscalerChoice").get<std::string>();
        for (const char* known : {"ravu-ar", "sgsr-ed", "ravu", "sgsr", "linear"}) {
            if (choice == known) doc["upscaler"] = choice;
        }
    }
    read_string("renderer", result.renderer, {"vulkan", "opengl"});
    // Before 2.0.2, VSync was on or off; off was Rosemary's mailbox.
    if (!doc.contains("presentation") && doc.contains("vsync")) {
        bool vsync = true;
        read_bool("vsync", vsync);
        result.presentation = vsync ? "vsync" : "mailbox";
    }
    read_string("presentation", result.presentation, {"vsync", "adaptive", "mailbox", "immediate"});
    if (doc.contains("renderResolution")) {
        const auto& value = doc.at("renderResolution");
        if (!value.is_number_integer() || value.get<int>() < 1 || value.get<int>() > 100) {
            throw SettingsError("stud: config '" + path +
                                "' has a \"renderResolution\" that is not a percentage from 1 to 100");
        }
        result.render_scale = value.get<int>();
    }
    // The names a build before these were the two of Stud 1.x wrote.
    if (doc.contains("upscaler") && doc.at("upscaler").is_string()) {
        const std::string old = doc.at("upscaler").get<std::string>();
        if (old == "ravu" || old == "linear") doc["upscaler"] = "ravu-ar";
        if (old == "sgsr") doc["upscaler"] = "sgsr-ed";
    }
    read_string("upscaler", result.upscaler, {"ravu-ar", "sgsr-ed"});
    read_string("titleBar", result.title_bar, {"macos", "macos-no-menu", "native"});
    read_string("displayBackend", result.display_backend, {"auto", "wayland", "x11"});
    if (doc.contains("backgroundFps")) {
        if (!doc.at("backgroundFps").is_number_integer()) {
            throw SettingsError("stud: config '" + path + "' has a non-integer \"backgroundFps\"");
        }
        // 0 means no limit, and so does anything past the top of the range,
        // including the 241 older builds wrote.
        const int value = doc.at("backgroundFps").get<int>();
        result.background_fps =
            (value <= 0 || value > kBackgroundFpsUnlimited) ? kBackgroundFpsNoLimit : value;
    }
    read_bool("mangohud", result.mangohud);
    read_text("launchWrapper", result.launch_wrapper);
    read_text("environment", result.environment);
    read_bool("discordRichPresence", result.discord_rich_presence);
    read_bool("discordJoinButton", result.discord_join_button);
    read_bool("serverRegionNotification", result.server_region_notification);
    read_bool("recordGameHistory", result.record_game_history);
    read_bool("recordServerHistory", result.record_server_history);
    if (doc.contains("cleanerDays")) {
        if (!doc.at("cleanerDays").is_number_integer() || doc.at("cleanerDays").get<int>() < 0) {
            throw SettingsError("stud: config '" + path +
                                "' has a \"cleanerDays\" that is not a non-negative integer");
        }
        result.cleaner_days = doc.at("cleanerDays").get<int>();
    }
    read_bool("gamemode", result.gamemode);
    read_bool("systemTray", result.system_tray);
    read_bool("openPlayerDirectly", result.open_player_directly);

    return result;
}

void save_settings(const std::string& path, const StudSettings& settings) {
    nlohmann::json doc = nlohmann::json::object();

    // Preserve any other top-level keys already in the file; this file is
    // shared, not exclusively owned by this module. A missing or
    // malformed existing file is not fatal here, start fresh rather
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
                // Fall through with a fresh object, an unrelated
                // malformed file shouldn't block saving real settings.
            }
        }
    }

    doc["gpu"] = {{"deviceIndex", settings.gpu.device_index},
                  {"deviceName", settings.gpu.device_name},
                  {"deviceUuid", settings.gpu.device_uuid}};
    doc["renderer"] = settings.renderer;
    doc["presentation"] = settings.presentation;
    doc.erase("vsync");
    doc["renderResolution"] = settings.render_scale;
    doc["upscaler"] = settings.upscaler;
    doc["titleBar"] = settings.title_bar;
    doc["displayBackend"] = settings.display_backend;
    doc["backgroundFps"] = settings.background_fps;
    doc["mangohud"] = settings.mangohud;
    doc["launchWrapper"] = settings.launch_wrapper;
    doc["environment"] = settings.environment;
    doc["discordRichPresence"] = settings.discord_rich_presence;
    doc["discordJoinButton"] = settings.discord_join_button;
    doc["serverRegionNotification"] = settings.server_region_notification;
    doc["recordGameHistory"] = settings.record_game_history;
    doc["recordServerHistory"] = settings.record_server_history;
    doc["cleanerDays"] = settings.cleaner_days;
    doc["gamemode"] = settings.gamemode;
    doc["systemTray"] = settings.system_tray;
    doc["openPlayerDirectly"] = settings.open_player_directly;
    // Settings Stud no longer has: Stud 1.x's Android engine's, and ones 2.0
    // dropped.
    // Erased rather than kept, so the file never lists something that does
    // nothing.
    for (const char* gone :
         {"hidpi", "followDpi", "renderScale", "upscaling", "upscaleOutputPercent",
          "upscaleQualityPercent", "upscaleTargetPercent", "upscalerChoice",
          "textOverlayFontRatio", "smoothZoom", "windowsMaterials", "webLogin", "hqTextures",
          "closeOnLeave", "textureCache", "textureCacheMB", "graphicsMode",
          "devRenderBackend", "apkPath", "apkName", "uiScale", "needsBionicFallback",
          "menuBar", "frameCap", "presentationMode", "controlEditKeys", "launcherStyle"}) {
        doc.erase(gone);
    }

    make_directories(parent_directory(path));

    // Written beside it and renamed over it: truncating the real file
    // first left it empty or half-written after a crash or a full disk,
    // and the next launch then read it as malformed and ran on defaults.
    const std::string temp = path + ".new";
    {
        std::ofstream out(temp);
        if (!out.is_open()) {
            throw SettingsError("stud: failed to open '" + temp + "' for writing");
        }
        out << doc.dump(2);
        out.close();
        if (out.fail()) {
            std::remove(temp.c_str());
            throw SettingsError("stud: failed to write '" + temp + "'");
        }
    }
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        std::remove(temp.c_str());
        throw SettingsError("stud: failed to replace '" + path + "'");
    }
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

std::string default_studio_flags_path() {
    if (const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME")) {
        return std::string(xdg_config_home) + "/stud/studio-flags.json";
    }
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") + "/.config/stud/studio-flags.json";
}

}  // namespace stud::config
