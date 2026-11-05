// M8 test: stud-config's general settings storage (GPU selection, HiDPI,
// graphics mode). See stud-config/include/stud/settings.h.

#include "stud/settings.h"
#include "check.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 1;
    }
    std::string dir = argv[1];
    fresh_dir(dir);

    // Missing file -> real defaults (hidpi off, upscaling on, Vulkan), not an error.
    auto defaults = stud::config::load_settings(dir + "/does_not_exist.json");
    check(defaults.hidpi == false, "missing config file defaults to hidpi off");
    check(defaults.upscaling == true, "missing config file defaults to upscaling on");
    check(defaults.upscaler_choice == "ravu-ar", "missing config file defaults to RAVU");
    check(defaults.follow_dpi == false,
          "missing config file defaults to not following the display, which keeps SSAO");
    check(defaults.graphics_mode == stud::config::GraphicsMode::kVulkan,
          "missing config file defaults to Vulkan (locked default-on-first-launch decision)");
    check(defaults.gpu.device_index == 0, "missing config file defaults gpu.deviceIndex to 0");
    check(defaults.background_fps == 30, "missing config file defaults the background frame rate to 30");
    check(defaults.windows_materials, "missing config file defaults Windows Graphical Parity to on");
    check(!defaults.hq_textures, "missing config file defaults HQ Textures to off");
    check(defaults.web_login, "missing config file defaults web login to on");

    std::string path = dir + "/config.json";

    // Real save/load round-trip.
    stud::config::StudSettings settings;
    settings.gpu.device_index = 2;
    settings.gpu.device_name = "Test GPU 9000";
    settings.gpu.device_uuid = "00112233445566778899aabbccddeeff";
    // Every field away from its default: one left there proves nothing, since
    // losing it on the way through would read back the same.
    settings.hidpi = true;
    settings.follow_dpi = true;
    settings.upscaling = false;
    settings.upscaler_choice = "fsr";
    settings.smooth_zoom = false;
    settings.text_overlay_font_ratio = 0.5f;
    settings.background_fps = 120;
    settings.mangohud = true;
    settings.gamemode = true;
    settings.discord_rich_presence = false;
    settings.discord_join_button = true;
    settings.system_tray = false;
    settings.close_on_leave = true;
    settings.server_region_notification = true;
    settings.texture_cache = false;
    settings.texture_cache_mb = 1234;
    settings.graphics_mode = stud::config::GraphicsMode::kOpenGL;
    settings.windows_materials = false;
    settings.web_login = false;
    settings.hq_textures = true;
    stud::config::save_settings(path, settings);

    auto loaded = stud::config::load_settings(path);
    check(loaded.gpu.device_index == 2, "gpu.deviceIndex round-trips");
    check(loaded.gpu.device_name == "Test GPU 9000", "gpu.deviceName round-trips");
    check(loaded.gpu.device_uuid == "00112233445566778899aabbccddeeff", "gpu.deviceUuid round-trips");
    check(loaded.hidpi, "hidpi round-trips");
    check(loaded.follow_dpi, "followDpi round-trips");
    check(!loaded.upscaling, "upscaling round-trips");
    check(loaded.upscaler_choice == "fsr", "the upscaler choice round-trips");
    check(!loaded.smooth_zoom, "smooth zoom round-trips");
    check(loaded.text_overlay_font_ratio == 0.5f, "the text overlay's font ratio round-trips");
    check(loaded.background_fps == 120, "the background frame rate round-trips");
    check(loaded.mangohud, "mangohud round-trips");
    check(loaded.gamemode, "gamemode round-trips");
    check(!loaded.discord_rich_presence, "Discord presence round-trips");
    check(loaded.discord_join_button, "Discord's join button round-trips");
    check(!loaded.system_tray, "the system tray round-trips");
    check(loaded.close_on_leave, "close on leave round-trips");
    check(loaded.server_region_notification, "the server region notification round-trips");
    check(!loaded.texture_cache, "the texture cache switch round-trips");
    check(loaded.texture_cache_mb == 1234, "the texture cache size round-trips");
    check(!loaded.windows_materials, "Windows Graphical Parity round-trips");
    check(!loaded.web_login, "web login round-trips");
    check(loaded.hq_textures, "HQ Textures round-trips");
    check(loaded.graphics_mode == stud::config::GraphicsMode::kOpenGL, "graphicsMode round-trips as opengl");
    // Retired keys are removed on save rather than carried forward: a
    // file that still lists them reads as if they do something.
    {
        std::ofstream stale(path, std::ios::trunc);
        stale << R"({"apkPath":"/home/user/Downloads/roblox.apk","apkName":"roblox.apk",)"
                 R"("uiScale":1.25,"needsBionicFallback":false,"backgroundFps":241})";
    }
    auto stale_loaded = stud::config::load_settings(path);
    check(stale_loaded.background_fps == stud::config::kBackgroundFpsNoLimit,
          "an out-of-range backgroundFps (the 241 older builds wrote) reads as unlimited");
    stud::config::save_settings(path, stale_loaded);
    {
        std::ifstream f(path);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        check(content.find("apkPath") == std::string::npos, "apkPath is not written any more");
        check(content.find("apkName") == std::string::npos, "apkName is not written any more");
        check(content.find("uiScale") == std::string::npos, "uiScale is not written any more");
        check(content.find("needsBionicFallback") == std::string::npos,
              "needsBionicFallback is not written any more");
        check(content.find("\"backgroundFps\": 0") != std::string::npos,
              "unlimited is stored as 0, not as one past the top of a slider");
    }
    // Back to the round-trip file for the checks below.
    stud::config::save_settings(path, settings);

    // Real content check, keys are actually named as the schema promises.
    {
        std::ifstream f(path);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        check(content.find("\"deviceIndex\": 2") != std::string::npos, "deviceIndex key present as written");
        check(content.find("\"graphicsMode\": \"opengl\"") != std::string::npos,
              "graphicsMode serializes as the lowercase string \"opengl\", not an enum int");
    }

    // save_settings() preserves an unrelated pre-existing top-level key
    // instead of clobbering it. This file is shared, not exclusively owned.
    std::string shared_path = dir + "/shared_config.json";
    {
        std::ofstream out(shared_path);
        out << R"({"someOtherKey": {"mode": "x"}})";
    }
    stud::config::save_settings(shared_path, settings);
    {
        std::ifstream f(shared_path);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        check(content.find("someOtherKey") != std::string::npos,
              "save_settings preserves an unrelated pre-existing top-level key");
        check(content.find("\"hidpi\"") != std::string::npos,
              "save_settings still writes its own keys into the same shared file");
    }

    // Malformed file -> throws clearly.
    std::string bad_path = dir + "/bad.json";
    {
        std::ofstream out(bad_path);
        out << "not valid json {{{";
    }
    bool threw = false;
    try {
        stud::config::load_settings(bad_path);
    } catch (const stud::config::SettingsError&) {
        threw = true;
    }
    check(threw, "malformed config file throws instead of silently misbehaving");

    // Invalid graphicsMode value -> throws clearly.
    std::string bad_mode_path = dir + "/bad_mode.json";
    {
        std::ofstream out(bad_mode_path);
        out << R"({"graphicsMode": "direct3d12"})";
    }
    threw = false;
    try {
        stud::config::load_settings(bad_mode_path);
    } catch (const stud::config::SettingsError&) {
        threw = true;
    }
    check(threw, "unrecognized graphicsMode value throws instead of silently misbehaving");

    std::printf("all settings checks passed\n");
    return 0;
}
