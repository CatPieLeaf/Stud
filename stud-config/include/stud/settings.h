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

struct GpuSelection {
    // Index into vkEnumeratePhysicalDevices' order when it was chosen. The
    // name and UUID find the same device again if that order changes.
    uint32_t device_index = 0;
    std::string device_name;
    std::string device_uuid;
};

struct StudSettings {
    GpuSelection gpu;

    // A frame-rate limit Stud asks Roblox for (DFIntTaskSchedulerTargetFps),
    // 0 to leave it to Roblox's own Maximum Frame Rate setting.
    int frame_cap = 0;

    // Whether frames wait for the display's refresh. Off, the client is told
    // a refresh rate it never reaches (it paces itself to the rate it is
    // told) and presents without waiting.
    bool vsync = false;

    // How frames are handed to the display: "" for Rosemary's choice, or
    // "fifo", "fifo_relaxed", "mailbox", "immediate".
    std::string presentation_mode;

    // The window's title bar: "macos" (the macOS title bar with the menu bar
    // in it), "macos-no-menu" (the macOS title bar alone) or "native" (the
    // desktop's own title bar, no menu bar).
    std::string title_bar = "native";

    // Where the window goes: "auto" (Wayland when there is one), "wayland"
    // or "x11".
    std::string display_backend = "auto";

    // Ctrl for copy, cut, paste and select-all, where the macOS client
    // expects Command (which most Linux desktops keep for themselves).
    bool control_edit_keys = true;

    bool mangohud = false;
    bool gamemode = false;

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
