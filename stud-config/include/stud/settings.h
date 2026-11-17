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

// Whether frames wait for the display's refresh, so the client's own frame
// rate follows it.
struct StudSettings;
bool waits_for_refresh(const StudSettings& settings);

struct StudSettings {
    GpuSelection gpu;

    // The client's renderer: "vulkan" (its Metal renderer, which Rosemary
    // runs on Vulkan) or "opengl" (its OpenGL renderer, chosen with the
    // allowlisted FFlagDebugGraphicsPreferOpenGL).
    std::string renderer = "vulkan";

    // How frames reach the display:
    //   "vsync"     each waits for a refresh (Vulkan FIFO, OpenGL interval 1)
    //   "adaptive"  waits, unless it is already late, so a slow frame tears
    //               instead of halving the rate (FIFO relaxed, interval -1)
    //   "mailbox"   no wait and no tearing: the newest frame is shown at each
    //               refresh (Rosemary's own default; on OpenGL, "immediate")
    //   "immediate" no wait, and frames may tear (IMMEDIATE, interval 0)
    std::string presentation = "vsync";

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
    std::string title_bar = "macos-no-menu";

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

    // A command Roblox starts through (gamescope -f --, prime-run, ...), split
    // the way a shell splits a line; empty for none.
    std::string launch_wrapper;
    // Extra environment for the client, one NAME=value a line. They are set
    // last, so one of them wins over Stud's own value for the same name.
    std::string environment;

    // Discord Rich Presence for the game being played, and whether it offers
    // a button to join the same server.
    bool discord_rich_presence = true;
    bool discord_join_button = false;

    // A notification naming the country of the game server on each join.
    bool server_region_notification = false;

    // The History tab's records: the games played, and the servers joined in
    // each (with where they are). No servers are kept without games.
    bool record_game_history = true;
    bool record_server_history = true;

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

// Studio's FastFlags, as a JSON object: written into Studio's own
// ClientAppSettings.json at every launch.
std::string default_studio_flags_path();

}  // namespace stud::config
