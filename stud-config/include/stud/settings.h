#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

// Stud's settings in ~/.config/stud/config.json. Plain C++, no Qt. Roblox's own
// settings stay in Roblox's menu.

namespace stud::config {

// Slider values above this mean unlimited.
inline constexpr int kBackgroundFpsUnlimited = 240;

// How unlimited is stored.
inline constexpr int kBackgroundFpsNoLimit = 0;

struct GpuSelection {
    // vkEnumeratePhysicalDevices order when chosen; name and UUID find it again if that changes.
    uint32_t device_index = 0;
    std::string device_name;
    std::string device_uuid;
};

struct StudSettings;
bool waits_for_refresh(const StudSettings& settings);

struct StudSettings {
    GpuSelection gpu;

    // "vulkan" (the Metal renderer, on Vulkan through Rosemary) or "opengl"
    // (via FFlagDebugGraphicsPreferOpenGL).
    std::string renderer = "vulkan";

    //   "vsync"     wait for each refresh (FIFO, interval 1)
    //   "adaptive"  wait unless late, then tear (FIFO relaxed, interval -1)
    //   "mailbox"   no wait, no tearing (on OpenGL, same as immediate)
    //   "immediate" no wait, may tear (IMMEDIATE, interval 0)
    std::string presentation = "vsync";

    // Percent of the window size the game renders at; 100 is full size.
    int render_scale = 100;
    // "ravu-ar" (RAVU-Zoom r2 with anti-ringing, then RCAS) or "sgsr-ed"
    // (Snapdragon GSR 1, edge-direction variant).
    std::string upscaler = "ravu-ar";

    // "macos" (with menu bar), "macos-no-menu", or "native" (the desktop's).
    std::string title_bar = "macos-no-menu";

    // "auto"/"wayland" or "x11" (XWayland). Only read where both are available.
    std::string display_backend = "auto";

    // FPS while no Roblox window has focus; kBackgroundFpsNoLimit for none.
    int background_fps = 30;

    bool mangohud = false;
    bool gamemode = true;

    // e.g. gamescope -f --, prime-run; split as a shell would. Empty for none.
    std::string launch_wrapper;
    // One NAME=value per line, set last so they win.
    std::string environment;

    bool discord_rich_presence = true;
    bool discord_join_button = false;

    bool server_region_notification = false;

    // History tab records. Servers are only kept with their games.
    bool record_game_history = true;
    bool record_server_history = true;

    // Roblox logs and caches and Stud's logs older than this are deleted at launch; 0 keeps them.
    int cleaner_days = 0;


    // Skip the welcome page and start Roblox Player.
    bool open_player_directly = false;

    bool system_tray = true;
};

class SettingsError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Defaults for anything missing, including the whole file. Throws SettingsError on
// invalid JSON or a wrongly typed value instead of silently using defaults.
StudSettings load_settings(const std::string& path);

// Keeps keys in the file that StudSettings doesn't know.
void save_settings(const std::string& path, const StudSettings& settings);

std::string default_config_path();

std::string default_flag_overrides_path();

std::string default_studio_flags_path();

}  // namespace stud::config
