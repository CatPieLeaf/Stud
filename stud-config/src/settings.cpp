#include "stud/settings.h"
#include "stud/stud_paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <cmath>
#include <initializer_list>

namespace {

bool is_integer(const QJsonValue& v) {
    return v.isDouble() && std::floor(v.toDouble()) == v.toDouble();
}

QString key_of(const char* key) { return QString::fromLatin1(key); }

QString text(const std::string& s) { return QString::fromStdString(s); }

}  // namespace

namespace stud::config {

bool waits_for_refresh(const StudSettings& settings) {
    return settings.presentation == "vsync" || settings.presentation == "adaptive";
}

StudSettings load_settings(const std::string& path) {
    StudSettings result;

    QFile file(text(path));
    if (!file.open(QIODevice::ReadOnly)) {
        return result;
    }

    QJsonParseError error;
    const QJsonDocument parsed = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError) {
        throw SettingsError("stud: malformed config file '" + path + "': " + error.errorString().toStdString());
    }
    if (!parsed.isObject()) {
        throw SettingsError("stud: config file '" + path + "' must be a JSON object");
    }
    QJsonObject doc = parsed.object();
    const auto has = [&](const char* key) { return doc.contains(key_of(key)); };
    const auto at = [&](const char* key) { return doc.value(key_of(key)); };

    if (has("gpu")) {
        const QJsonObject gpu = at("gpu").toObject();
        const QJsonValue index = gpu.value(QStringLiteral("deviceIndex"));
        if (!at("gpu").isObject() || !is_integer(index) || index.toDouble() < 0) {
            throw SettingsError("stud: config '" + path +
                                "' has an invalid \"gpu\" (expected an object with an unsigned "
                                "\"deviceIndex\")");
        }
        result.gpu.device_index = static_cast<uint32_t>(index.toDouble());
        if (gpu.value(QStringLiteral("deviceName")).isString()) {
            result.gpu.device_name = gpu.value(QStringLiteral("deviceName")).toString().toStdString();
        }
        if (gpu.value(QStringLiteral("deviceUuid")).isString()) {
            result.gpu.device_uuid = gpu.value(QStringLiteral("deviceUuid")).toString().toStdString();
        }
    }

    // A wrongly typed key is an error; a missing one keeps its default.
    const auto read_bool = [&](const char* key, bool& out) {
        if (!has(key)) return;
        if (!at(key).isBool()) {
            throw SettingsError("stud: config '" + path + "' has a non-boolean \"" + key + "\"");
        }
        out = at(key).toBool();
    };
    const auto read_text = [&](const char* key, std::string& out) {
        if (!has(key)) return false;
        if (!at(key).isString()) {
            throw SettingsError("stud: config '" + path + "' has a non-string \"" + key + "\"");
        }
        out = at(key).toString().toStdString();
        return true;
    };
    const auto read_string = [&](const char* key, std::string& out,
                                 std::initializer_list<const char*> allowed) {
        std::string value;
        if (!read_text(key, value)) return;
        for (const char* option : allowed) {
            if (value == option) {
                out = value;
                return;
            }
        }
        throw SettingsError("stud: config '" + path + "' has an unrecognized \"" + key +
                            "\" \"" + value + "\"");
    };
    const auto read_int = [&](const char* key, int& out, const char* what) {
        if (!has(key)) return false;
        if (!is_integer(at(key))) {
            throw SettingsError("stud: config '" + path + "' has a \"" + key + "\" that is not " + what);
        }
        out = at(key).toInt();
        return true;
    };

    // Stud 1.x's names for the two settings 2.0 kept, read only when the 2.0 key is
    // missing; the next save drops them.
    if (!has("renderer") && at("graphicsMode").isString()) {
        const QString mode = at("graphicsMode").toString();
        if (mode == QLatin1String("vulkan") || mode == QLatin1String("opengl")) {
            doc.insert(QStringLiteral("renderer"), mode);
        }
    }
    if (!has("upscaler") && at("upscalerChoice").isString()) {
        const QString choice = at("upscalerChoice").toString();
        for (const char* known : {"ravu-ar", "sgsr-ed", "ravu", "sgsr", "linear"}) {
            if (choice == QLatin1String(known)) doc.insert(QStringLiteral("upscaler"), choice);
        }
    }
    read_string("renderer", result.renderer, {"vulkan", "opengl"});
    // Before 2.0.2, VSync was a bool; off meant mailbox.
    if (!has("presentation") && has("vsync")) {
        bool vsync = true;
        read_bool("vsync", vsync);
        result.presentation = vsync ? "vsync" : "mailbox";
    }
    read_string("presentation", result.presentation, {"vsync", "adaptive", "mailbox", "immediate"});
    int scale = 0;
    if (read_int("renderResolution", scale, "a percentage from 1 to 100")) {
        if (scale < 1 || scale > 100) {
            throw SettingsError("stud: config '" + path +
                                "' has a \"renderResolution\" that is not a percentage from 1 to 100");
        }
        result.render_scale = scale;
    }
    if (at("upscaler").isString()) {
        const QString old = at("upscaler").toString();
        if (old == QLatin1String("ravu") || old == QLatin1String("linear")) {
            doc.insert(QStringLiteral("upscaler"), QStringLiteral("ravu-ar"));
        }
        if (old == QLatin1String("sgsr")) doc.insert(QStringLiteral("upscaler"), QStringLiteral("sgsr-ed"));
    }
    read_string("upscaler", result.upscaler, {"ravu-ar", "sgsr-ed"});
    read_string("titleBar", result.title_bar, {"macos", "macos-no-menu", "native"});
    read_string("displayBackend", result.display_backend, {"auto", "wayland", "x11"});
    int fps = 0;
    if (read_int("backgroundFps", fps, "an integer")) {
        // 0 or anything past the range (older builds wrote 241) means no limit.
        result.background_fps = (fps <= 0 || fps > kBackgroundFpsUnlimited) ? kBackgroundFpsNoLimit : fps;
    }
    read_bool("mangohud", result.mangohud);
    read_text("launchWrapper", result.launch_wrapper);
    read_text("environment", result.environment);
    read_bool("discordRichPresence", result.discord_rich_presence);
    read_bool("discordJoinButton", result.discord_join_button);
    read_bool("gameControllers", result.game_controllers);
    read_bool("serverRegionNotification", result.server_region_notification);
    read_bool("recordGameHistory", result.record_game_history);
    read_bool("recordServerHistory", result.record_server_history);
    int days = 0;
    if (read_int("cleanerDays", days, "a non-negative integer")) {
        if (days < 0) {
            throw SettingsError("stud: config '" + path +
                                "' has a \"cleanerDays\" that is not a non-negative integer");
        }
        result.cleaner_days = days;
    }
    read_bool("gamemode", result.gamemode);
    read_bool("systemTray", result.system_tray);
    read_bool("openPlayerDirectly", result.open_player_directly);

    return result;
}

void save_settings(const std::string& path, const StudSettings& settings) {
    const QString file_path = text(path);

    // Keep the file's other keys; a missing or malformed file starts fresh.
    QJsonObject doc;
    QFile existing(file_path);
    if (existing.open(QIODevice::ReadOnly)) {
        doc = QJsonDocument::fromJson(existing.readAll()).object();
        existing.close();
    }

    doc.insert(QStringLiteral("gpu"),
               QJsonObject{{QStringLiteral("deviceIndex"), static_cast<qint64>(settings.gpu.device_index)},
                           {QStringLiteral("deviceName"), text(settings.gpu.device_name)},
                           {QStringLiteral("deviceUuid"), text(settings.gpu.device_uuid)}});
    doc.insert(QStringLiteral("renderer"), text(settings.renderer));
    doc.insert(QStringLiteral("presentation"), text(settings.presentation));
    doc.insert(QStringLiteral("renderResolution"), settings.render_scale);
    doc.insert(QStringLiteral("upscaler"), text(settings.upscaler));
    doc.insert(QStringLiteral("titleBar"), text(settings.title_bar));
    doc.insert(QStringLiteral("displayBackend"), text(settings.display_backend));
    doc.insert(QStringLiteral("backgroundFps"), settings.background_fps);
    doc.insert(QStringLiteral("mangohud"), settings.mangohud);
    doc.insert(QStringLiteral("launchWrapper"), text(settings.launch_wrapper));
    doc.insert(QStringLiteral("environment"), text(settings.environment));
    doc.insert(QStringLiteral("discordRichPresence"), settings.discord_rich_presence);
    doc.insert(QStringLiteral("discordJoinButton"), settings.discord_join_button);
    doc.insert(QStringLiteral("gameControllers"), settings.game_controllers);
    doc.insert(QStringLiteral("serverRegionNotification"), settings.server_region_notification);
    doc.insert(QStringLiteral("recordGameHistory"), settings.record_game_history);
    doc.insert(QStringLiteral("recordServerHistory"), settings.record_server_history);
    doc.insert(QStringLiteral("cleanerDays"), settings.cleaner_days);
    doc.insert(QStringLiteral("gamemode"), settings.gamemode);
    doc.insert(QStringLiteral("systemTray"), settings.system_tray);
    doc.insert(QStringLiteral("openPlayerDirectly"), settings.open_player_directly);
    // Settings Stud no longer has (1.x's engine and some 2.0 ones), so the file only lists live ones.
    for (const char* gone :
         {"vsync", "hidpi", "followDpi", "renderScale", "upscaling", "upscaleOutputPercent",
          "upscaleQualityPercent", "upscaleTargetPercent", "upscalerChoice",
          "textOverlayFontRatio", "smoothZoom", "windowsMaterials", "webLogin", "hqTextures",
          "closeOnLeave", "textureCache", "textureCacheMB", "graphicsMode",
          "devRenderBackend", "apkPath", "apkName", "uiScale", "needsBionicFallback",
          "menuBar", "frameCap", "presentationMode", "controlEditKeys", "launcherStyle"}) {
        doc.remove(key_of(gone));
    }

    QDir().mkpath(QFileInfo(file_path).absolutePath());

    // Written beside and renamed over, so a crash or full disk can't leave it half-written.
    QSaveFile out(file_path);
    if (!out.open(QIODevice::WriteOnly)) {
        throw SettingsError("stud: failed to open '" + path + "' for writing");
    }
    out.write(QJsonDocument(doc).toJson(QJsonDocument::Indented));
    if (!out.commit()) {
        throw SettingsError("stud: failed to write '" + path + "'");
    }
}

std::string default_config_path() { return stud::paths::config_dir() + "/config.json"; }

std::string default_flag_overrides_path() { return stud::paths::config_dir() + "/flags.json"; }

std::string default_studio_flags_path() { return stud::paths::config_dir() + "/studio-flags.json"; }

}  // namespace stud::config
