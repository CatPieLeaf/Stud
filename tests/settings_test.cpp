// stud-config's settings storage. See stud-config/include/stud/settings.h.

#include "stud/settings.h"
#include "check.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace {

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool throws_on(const std::string& path, const char* json) {
    std::ofstream(path, std::ios::trunc) << json;
    try {
        stud::config::load_settings(path);
    } catch (const stud::config::SettingsError&) {
        return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 1;
    }
    std::string dir = argv[1];
    fresh_dir(dir);

    // Missing file: defaults, not an error.
    auto defaults = stud::config::load_settings(dir + "/does_not_exist.json");
    check(defaults.gpu.device_index == 0, "missing config file defaults gpu.deviceIndex to 0");
    check(defaults.presentation == "vsync" && stud::config::waits_for_refresh(defaults),
          "presentation defaults to VSync");
    check(defaults.render_scale == 100 && defaults.upscaler == "ravu-ar",
          "the game renders at full size, with RAVU-Zoom ready");
    check(defaults.renderer == "vulkan", "the renderer defaults to Vulkan");
    check(defaults.title_bar == "native", "the title bar defaults to the desktop's own");
    check(defaults.display_backend == "wayland", "the display backend defaults to Wayland");
    check(defaults.system_tray, "the tray defaults to on");
    check(defaults.background_fps == 30, "the background frame rate defaults to 30");
    check(defaults.discord_rich_presence, "Discord presence defaults to on");

    const std::string path = dir + "/config.json";

    // Every field off its default, so a field lost in the round trip shows.
    stud::config::StudSettings settings;
    settings.gpu.device_index = 2;
    settings.gpu.device_name = "Test GPU 9000";
    settings.gpu.device_uuid = "00112233445566778899aabbccddeeff";
    settings.presentation = "adaptive";
    settings.render_scale = 67;
    settings.upscaler = "sgsr-ed";
    settings.renderer = "opengl";
    settings.title_bar = "macos";
    settings.display_backend = "x11";
    settings.mangohud = true;
    settings.gamemode = false;
    settings.system_tray = false;
    settings.open_player_directly = true;
    settings.background_fps = 120;
    settings.discord_rich_presence = false;
    settings.discord_join_button = true;
    settings.server_region_notification = true;
    settings.record_game_history = false;
    settings.record_server_history = false;
    settings.launch_wrapper = "gamescope -f --";
    settings.environment = "A=1\nB=two words";
    settings.cleaner_days = 7;
    stud::config::save_settings(path, settings);

    auto loaded = stud::config::load_settings(path);
    check(loaded.gpu.device_index == 2, "gpu.deviceIndex round-trips");
    check(loaded.gpu.device_name == "Test GPU 9000", "gpu.deviceName round-trips");
    check(loaded.gpu.device_uuid == "00112233445566778899aabbccddeeff", "gpu.deviceUuid round-trips");
    check(loaded.presentation == "adaptive" && stud::config::waits_for_refresh(loaded),
          "presentation round-trips");
    check(loaded.render_scale == 67 && loaded.upscaler == "sgsr-ed", "render scale and upscaler round-trip");
    check(loaded.renderer == "opengl", "the renderer round-trips");
    check(loaded.title_bar == "macos", "the title bar round-trips");
    check(loaded.display_backend == "x11", "the display backend round-trips");
    check(loaded.mangohud, "mangohud round-trips");
    check(!loaded.gamemode, "gamemode round-trips");
    check(!loaded.system_tray, "the system tray round-trips");
    check(loaded.open_player_directly, "opening Roblox Player directly round-trips");
    check(loaded.background_fps == 120, "the background frame rate round-trips");
    check(!loaded.discord_rich_presence, "Discord presence round-trips");
    check(loaded.discord_join_button, "Discord's join button round-trips");
    check(loaded.server_region_notification, "the region notification round-trips");
    check(!loaded.record_game_history, "recording games round-trips");
    check(!loaded.record_server_history, "recording servers round-trips");
    check(loaded.launch_wrapper == "gamescope -f --", "the launch wrapper round-trips");
    check(loaded.environment == "A=1\nB=two words", "the environment round-trips");
    check(stud::config::StudSettings{}.record_game_history && stud::config::StudSettings{}.record_server_history,
          "games and servers are recorded by default");
    check(loaded.cleaner_days == 7, "the cleaner's age round-trips");

    // Stud 1.x's keys are dropped on save.
    std::ofstream(path, std::ios::trunc)
        << R"({"apkPath":"/home/user/Downloads/roblox.apk","hidpi":true,)"
           R"("graphicsMode":"opengl","backgroundFps":241,"menuBar":"strip",)"
           R"("someOtherKey":{"mode":"x"}})";
    stud::config::save_settings(path, stud::config::load_settings(path));
    {
        const std::string content = read_file(path);
        check(content.find("apkPath") == std::string::npos, "apkPath is not written any more");
        check(content.find("hidpi") == std::string::npos, "hidpi is not written any more");
        check(content.find("graphicsMode") == std::string::npos,
              "graphicsMode is not written any more");
        check(content.find("\"backgroundFps\": 0") != std::string::npos,
              "the 241 older builds wrote reads back as unlimited, stored as 0");
        check(content.find("someOtherKey") != std::string::npos,
              "save_settings preserves a key that is not Stud's");
        check(content.find("menuBar") == std::string::npos, "menuBar is not written any more");
        check(content.find("\"titleBar\": \"native\"") != std::string::npos,
              "save_settings writes its own keys into the same file");
    }

    // A Stud 1.x config.json: kept settings read under their old names, the rest
    // ignored, old keys gone after a save.
    {
        const std::string v1 = dir + "/v1.json";
        std::ofstream(v1, std::ios::trunc) << R"({"graphicsMode": "opengl", "upscaling": true,
            "upscalerChoice": "sgsr-ed", "hidpi": false, "textureCacheMB": 500,
            "gamemode": false, "backgroundFps": 60})";
        auto loaded = stud::config::load_settings(v1);
        check(loaded.renderer == "opengl", "Stud 1.x's graphics mode becomes the renderer");
        check(loaded.upscaler == "sgsr-ed", "Stud 1.x's upscaler choice becomes the upscaler");
        check(!loaded.gamemode && loaded.background_fps == 60,
              "settings with the same name in 1.x and 2.0 carry over");
        stud::config::save_settings(v1, loaded);
        const std::string content = read_file(v1);
        check(content.find("graphicsMode") == std::string::npos &&
                  content.find("upscalerChoice") == std::string::npos &&
                  content.find("textureCacheMB") == std::string::npos,
              "Stud 1.x's own keys are dropped on the next save");

        std::ofstream(v1, std::ios::trunc) << R"({"graphicsMode": "vulkan", "upscalerChoice": "fsr",
            "renderer": "opengl"})";
        loaded = stud::config::load_settings(v1);
        check(loaded.upscaler == "ravu-ar", "a 1.x upscaler 2.0 does not have keeps the default");
        check(loaded.renderer == "opengl", "a 2.0 value wins over the 1.x name for it");
    }

    check(throws_on(dir + "/bad.json", "not valid json {{{"), "malformed JSON throws");
    check(throws_on(dir + "/bad_title.json", R"({"titleBar": "dock"})"),
          "an unknown title bar value throws");
    check(throws_on(dir + "/bad_backend.json", R"({"displayBackend": "mir"})"),
          "an unknown display backend throws");
    {
        std::ofstream(dir + "/auto_backend.json") << R"({"displayBackend": "auto"})";
        check(stud::config::load_settings(dir + "/auto_backend.json").display_backend == "wayland",
              "an older build's automatic display backend reads as Wayland");
    }
    check(throws_on(dir + "/bad_vsync.json", R"({"vsync": "yes"})"), "a non-boolean vsync throws");
    check(throws_on(dir + "/bad_presentation.json", R"({"presentation": "triple"})"),
          "an unknown presentation throws");
    {
        // A file from before presentation modes: VSync off was mailbox.
        std::ofstream old(dir + "/old_vsync.json");
        old << R"({"vsync": false})";
    }
    const auto migrated = stud::config::load_settings(dir + "/old_vsync.json");
    check(migrated.presentation == "mailbox" && !stud::config::waits_for_refresh(migrated),
          "an old VSync-off setting becomes mailbox");

    std::printf("all settings checks passed\n");
    return 0;
}
