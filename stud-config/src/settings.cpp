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

    if (doc.contains("frameCap")) {
        if (!doc.at("frameCap").is_number_integer() || doc.at("frameCap").get<int>() < 0) {
            throw SettingsError("stud: config '" + path +
                                "' has a \"frameCap\" that is not a non-negative integer");
        }
        result.frame_cap = doc.at("frameCap").get<int>();
    }
    read_bool("vsync", result.vsync);
    read_string("presentationMode", result.presentation_mode,
                {"", "fifo", "fifo_relaxed", "mailbox", "immediate"});
    read_string("titleBar", result.title_bar, {"macos", "macos-no-menu", "native"});
    read_string("displayBackend", result.display_backend, {"auto", "wayland", "x11"});
    read_bool("controlEditKeys", result.control_edit_keys);
    read_bool("mangohud", result.mangohud);
    read_bool("gamemode", result.gamemode);
    read_bool("systemTray", result.system_tray);

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
    doc["frameCap"] = settings.frame_cap;
    doc["vsync"] = settings.vsync;
    doc["presentationMode"] = settings.presentation_mode;
    doc["titleBar"] = settings.title_bar;
    doc["displayBackend"] = settings.display_backend;
    doc["controlEditKeys"] = settings.control_edit_keys;
    doc["mangohud"] = settings.mangohud;
    doc["gamemode"] = settings.gamemode;
    doc["systemTray"] = settings.system_tray;
    // Settings of Stud 1.x's Android engine, which Stud 2.0 does not have.
    // Erased rather than kept, so the file never lists something that does
    // nothing.
    for (const char* gone :
         {"hidpi", "followDpi", "renderScale", "upscaling", "upscaleOutputPercent",
          "upscaleQualityPercent", "upscaleTargetPercent", "upscalerChoice",
          "textOverlayFontRatio", "smoothZoom", "windowsMaterials", "webLogin", "hqTextures",
          "backgroundFps", "discordRichPresence", "discordJoinButton", "closeOnLeave",
          "serverRegionNotification", "textureCache", "textureCacheMB", "graphicsMode",
          "devRenderBackend", "apkPath", "apkName", "uiScale", "needsBionicFallback",
          "menuBar"}) {
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

}  // namespace stud::config
