#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

// Stud's settings: ~/.config/stud/config.json (XDG), which the Settings window
// reads and writes. Plain C++, no Qt, so anything can read it.
//
// Stud 2.0 runs Roblox's macOS client on Rosemary, so these are the choices
// that shape how that process is started; Roblox's own settings stay in
// Roblox's own menu.

namespace stud::config {

// The top of the background-frame-rate range. A slider set above this
// reads as unlimited rather than as a number.
inline constexpr int kBackgroundFpsUnlimited = 240;

// ...and how "unlimited" is stored: zero, no limit.
inline constexpr int kBackgroundFpsNoLimit = 0;

struct GpuSelection {
    // Index into vkEnumeratePhysicalDevices' order when it was chosen. The
    // name and UUID find the same device again if that order changes.
    uint32_t device_index = 0;
    std::string device_name;
    std::string device_uuid;
};

struct StudSettings {
    GpuSelection gpu;

    // The client's renderer: "vulkan" (its Metal renderer, which Rosemary
    // runs on Vulkan) or "opengl" (its OpenGL renderer, chosen with the
    // allowlisted FFlagDebugGraphicsPreferOpenGL).
    std::string renderer = "vulkan";

    // Whether frames wait for the display's refresh: FIFO presentation, or
    // Rosemary's own default (MAILBOX) when off.
    bool vsync = true;

    // The game drawn at this percentage of the window's size, and scaled
    // back up as it is presented. 100 draws it at full size.
    int render_scale = 100;
    // What scales it back up, as in Stud 1.x: "ravu-ar" (RAVU-Zoom r2 with
    // anti-ringing, then RCAS sharpening) or "sgsr-ed" (Snapdragon Game Super
    // Resolution 1, edge-direction variant, which sharpens as it scales).
    std::string upscaler = "ravu-ar";

    // The window's title bar: "macos" (the macOS title bar with the menu bar
    // in it), "macos-no-menu" (the macOS title bar alone) or "native" (the
    // desktop's own title bar, no menu bar).
    std::string title_bar = "native";

    // Where the window goes, on a Wayland desktop that also runs XWayland:
    // "auto" or "wayland" (Wayland), or "x11" (through XWayland). Anywhere
    // else there is only one choice and this is not read.
    std::string display_backend = "auto";

    // Frames per second while no Roblox window has focus (minimised, on
    // another workspace, covered or just alt-tabbed away), or
    // kBackgroundFpsNoLimit for no limit.
    int background_fps = 30;

    bool mangohud = false;
    bool gamemode = true;

    // Discord Rich Presence for the game being played, and whether it offers
    // a button to join the same server.
    bool discord_rich_presence = true;
    bool discord_join_button = false;

    // A notification naming the country of the game server on each join.
    bool server_region_notification = false;

    // Roblox's logs and caches, and Stud's own logs, older than this many
    // days are deleted at launch; 0 keeps them.
    int cleaner_days = 0;


    // Opened on its own, Stud starts Roblox Player rather than showing the
    // welcome page.
    bool open_player_directly = false;

    // A tray icon for the session: Settings, and stopping Roblox.
    bool system_tray = true;
};

class SettingsError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Defaults for anything the file does not have, including no file at all.
// Throws SettingsError when the file is not valid JSON or a value has the
// wrong type, rather than silently running on defaults.
StudSettings load_settings(const std::string& path);

// Writes `settings`, keeping any other keys in the file.
void save_settings(const std::string& path, const StudSettings& settings);

std::string default_config_path();

// Hand-edited FastFlags, merged into ClientAppSettings.json at launch.
std::string default_flag_overrides_path();

}  // namespace stud::config
