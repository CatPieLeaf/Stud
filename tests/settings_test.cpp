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
    check(defaults.frame_cap == 0, "the frame cap defaults to Roblox's own");
    check(!defaults.vsync, "vsync defaults to off");
    check(defaults.presentation_mode.empty(), "the presentation mode defaults to Rosemary's");
    check(defaults.title_bar == "native", "the title bar defaults to the desktop's own");
    check(defaults.display_backend == "auto", "the display backend defaults to automatic");
    check(defaults.control_edit_keys, "Ctrl edit keys default to on");
    check(defaults.system_tray, "the tray defaults to on");

    const std::string path = dir + "/config.json";

    // Every field away from its default: one left there proves nothing, since
    // losing it on the way through would read back the same.
    stud::config::StudSettings settings;
    settings.gpu.device_index = 2;
    settings.gpu.device_name = "Test GPU 9000";
    settings.gpu.device_uuid = "00112233445566778899aabbccddeeff";
    settings.frame_cap = 144;
    settings.vsync = true;
    settings.presentation_mode = "mailbox";
    settings.title_bar = "macos-no-menu";
    settings.display_backend = "x11";
    settings.control_edit_keys = false;
    settings.mangohud = true;
    settings.gamemode = true;
    settings.system_tray = false;
    stud::config::save_settings(path, settings);

    auto loaded = stud::config::load_settings(path);
    check(loaded.gpu.device_index == 2, "gpu.deviceIndex round-trips");
    check(loaded.gpu.device_name == "Test GPU 9000", "gpu.deviceName round-trips");
    check(loaded.gpu.device_uuid == "00112233445566778899aabbccddeeff", "gpu.deviceUuid round-trips");
    check(loaded.frame_cap == 144, "the frame cap round-trips");
    check(loaded.vsync, "vsync round-trips");
    check(loaded.presentation_mode == "mailbox", "the presentation mode round-trips");
    check(loaded.title_bar == "macos-no-menu", "the title bar round-trips");
    check(loaded.display_backend == "x11", "the display backend round-trips");
    check(!loaded.control_edit_keys, "Ctrl edit keys round-trip");
    check(loaded.mangohud, "mangohud round-trips");
    check(loaded.gamemode, "gamemode round-trips");
    check(!loaded.system_tray, "the system tray round-trips");

    // Stud 1.x's keys are removed on save rather than carried forward: a file
    // that still lists them reads as if they do something.
    std::ofstream(path, std::ios::trunc)
        << R"({"apkPath":"/home/user/Downloads/roblox.apk","hidpi":true,)"
           R"("graphicsMode":"opengl","backgroundFps":30,"menuBar":"strip",)"
           R"("someOtherKey":{"mode":"x"}})";
    stud::config::save_settings(path, stud::config::load_settings(path));
    {
        const std::string content = read_file(path);
        check(content.find("apkPath") == std::string::npos, "apkPath is not written any more");
        check(content.find("hidpi") == std::string::npos, "hidpi is not written any more");
        check(content.find("graphicsMode") == std::string::npos,
              "graphicsMode is not written any more");
        check(content.find("backgroundFps") == std::string::npos,
              "backgroundFps is not written any more");
        check(content.find("someOtherKey") != std::string::npos,
              "save_settings preserves a key that is not Stud's");
        check(content.find("menuBar") == std::string::npos, "menuBar is not written any more");
        check(content.find("\"titleBar\": \"native\"") != std::string::npos,
              "save_settings writes its own keys into the same file");
    }

    // A wrong value is an error, not a silent default.
    check(throws_on(dir + "/bad.json", "not valid json {{{"), "malformed JSON throws");
    check(throws_on(dir + "/bad_title.json", R"({"titleBar": "dock"})"),
          "an unknown title bar value throws");
    check(throws_on(dir + "/bad_backend.json", R"({"displayBackend": "mir"})"),
          "an unknown display backend throws");
    check(throws_on(dir + "/bad_cap.json", R"({"frameCap": -1})"), "a negative frame cap throws");
    check(throws_on(dir + "/bad_vsync.json", R"({"vsync": "yes"})"), "a non-boolean vsync throws");

    std::printf("all settings checks passed\n");
    return 0;
}
