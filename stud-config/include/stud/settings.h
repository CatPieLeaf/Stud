#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

// General settings storage (M8): the real, user-facing settings from the
// locked decisions table (the engineering notes) -- GPU selection, HiDPI,
// graphics mode (Vulkan/OpenGL). Deliberately plain C++, no Qt dependency,
// so both `ui` (reads/writes via the settings window) and `runtime`
// (reads the saved choice to set env vars before launching, per the
// locked "GPU selection... sets the appropriate env var... before
// launching the runtime process" decision) can use it without runtime
// linking Qt.
//
// Same JSON file every other Stud setting lives in
// (~/.config/stud/config.json, XDG convention) -- coexists with the
// "devRenderBackend" key render/dev_backend_config.h already owns
// (prototype-only, not part of this schema) as a sibling top-level key,
// not a competing config path. "UI is a front-end over the JSON config
// file, not a separate model" (locked decision) -- this struct's fields
// are exactly what the settings UI reads/writes, no separate model.

namespace stud::config {

enum class GraphicsMode {
    kVulkan,  // default on first launch, per the locked decision
    kOpenGL,
};

// The top of the background-frame-rate range. A slider set above this
// reads as unlimited rather than as a number.
inline constexpr int kBackgroundFpsUnlimited = 240;

// ...and how "unlimited" is stored. A number one past the top of a
// slider is a UI detail, not a setting: it wrote `241` into a file meant
// to be read by a person. Zero says the thing itself -- no limit.
inline constexpr int kBackgroundFpsNoLimit = 0;

struct GpuSelection {
    // Index into vkEnumeratePhysicalDevices()'s own result order --
    // that's the real, stable-for-a-given-driver-state identifier the
    // settings window enumerates against; deviceName is stored purely
    // for human-readable display (e.g. showing a saved choice without
    // needing to re-enumerate), never used to re-resolve the index.
    uint32_t device_index = 0;
    std::string device_name;
};

struct StudSettings {
    GpuSelection gpu;
    // On by default: the buffer is the window's logical size multiplied
    // by the display's real scale, so the compositor maps it 1:1 to
    // physical pixels and the image is sharp. The scale comes from the
    // compositor's fractional-scale protocol, never a guess, and is read
    // exactly once so nothing can change it mid-session.
    // Two independent things, deliberately.
    //
    // HiDPI is sharpness: whether the buffer is rendered at the display's
    // own scale (sharp, 1:1 with the panel) or at the window's logical
    // size and upscaled by the compositor (blurry, cheaper). It changes
    // how many pixels are drawn, never how big anything looks.
    //
    // It used to be a checkbox that could not do this, because one
    // variable held both the display's scale and the buffer's -- turning
    // it off erased the monitor's real scale and the engine was told it
    // was on an unscaled display. Those are separate now (see
    // android-glue's g_display_scale_120 / g_render_scale_120), so the
    // checkbox finally means what it says.
    bool hidpi = true;
    // Lay the app out at the display's own scale, so it is the same
    // physical size as the rest of the desktop.
    //
    // Off by default, and that is a real trade rather than a taste: the
    // engine reads this one number to pick its render technique, and
    // above 1.0 it takes the simplified path -- no SSAO, flatter
    // shadows. Measured at 1.00 (SSAO), 1.10 (none) and 1.25 (none), so
    // the boundary is exactly 1.0 and no quality setting overrides it.
    // See runtime/src/main.cpp, where the scale is decided.
    //
    // Only means anything with HiDPI on: with it off the buffer is
    // already the window's logical size, where 1.0 is the size that
    // matches the desktop.
    bool follow_dpi = false;
    // Internal render scale: how many pixels the ENGINE draws, as a
    // percentage of the buffer it draws into. 100 renders every pixel of
    // the output, which is what Stud has always done.
    //
    // Below that, the engine renders fewer pixels and the result is
    // scaled back up to the window -- the resolution scale every PC game
    // has and Roblox's own Android build does not offer. It is the input
    // end of the same pipeline HiDPI sits at the output end of: HiDPI
    // decides how many real pixels the buffer has, this decides how many
    // of them are actually drawn.
    //
    // Only meaningful with HiDPI on. With it off the buffer is already
    // the window's logical size and the compositor stretches it, so
    // scaling down here would be resampled a second time and any
    // sharpening thrown away.
    int render_scale_percent = 100;
    // Smooth zoom: the wheel eases the camera toward the new distance
    // instead of stepping straight to it. Off is the Android build's own
    // behaviour, which is what Sober does.
    bool smooth_zoom = true;
    // Frames per second while nobody can see the window -- minimised, on
    // another workspace, or fully covered.
    //
    // Rendering a game at full rate into a window nobody is looking at is
    // pure waste of GPU and CPU, and on a laptop it is the difference
    // between a warm machine and a hot one. Anything above
    // kBackgroundFpsUnlimited means "do not throttle at all", which is
    // what a recording or a download inside the client wants.
    int background_fps = 30;
    // MangoHud's performance overlay over Stud's own window. It is a
    // Vulkan implicit layer, so it belongs on render-host: that is the
    // process holding the real driver and the real swapchain, on both
    // render paths (the OpenGL one reaches Vulkan through ANGLE).
    bool mangohud = false;
    // Discord rich presence: what game is being played, its thumbnail,
    // and how long for. On by default: it reaches only a Discord client
    // already running on this machine, shows what a Roblox client on any
    // other platform would show, and is off entirely for anyone without
    // Discord open.
    bool discord_rich_presence = true;
    // A "Join server" button carrying the experience's own deep link.
    // Separate, because sharing a joinable link is a bigger step than
    // saying what you are playing.
    bool discord_join_button = false;
    // A tray icon for as long as the session runs. It is why Process A
    // outlives a launch at all -- with this off it hands off and exits, as
    // it always did.
    bool system_tray = true;
    // Whether Roblox's own in-game leave button ends Stud instead of
    // returning to the app's home screen. Off by default: leaving a game
    // to pick another one is the ordinary thing to do, and quitting
    // instead would be a surprise.
    bool close_on_leave = false;
    // A notification naming the game server's region on every join.
    // Sober calls this the server location indicator.
    //
    // Off by default, for the same reason Discord presence is: answering
    // it means one HTTPS request per join to a third-party geolocation
    // service (the datacenter's IP leaves the machine, never the user's).
    // Anything that talks to someone else is asked for, not assumed --
    // and Sober ships this off too.
    bool server_region_notification = false;
    // The transcoded-texture cache: whether to keep one at all, and how
    // much disk it may use.
    //
    // Both are config-file only, deliberately absent from the settings
    // window: this is a disk-space trade, and the honest reason to change
    // it is knowing your own disk rather than judging it by feel.
    //
    // It is turned off automatically on a spinning disk, where a random
    // read with a seek is close enough to the cost of re-encoding not to
    // be worth the space. On an SSD it is a clear win and on an NVMe an
    // enormous one. Setting `textureCache` false, or `textureCacheMB` to
    // 0, disables it anywhere.
    bool texture_cache = true;
    int texture_cache_mb = 500;
    GraphicsMode graphics_mode = GraphicsMode::kVulkan;
    // The APK is deliberately NOT here.
    //
    // Stud keeps its own copy at one fixed path (stud_paths.h's
    // stored_apk_path) and always uses that one, so there is nothing for
    // a config file to point at and nothing for a hand edit to break.
    // The settings window is a picker: it copies what is chosen into
    // that path. The name of the file that was picked is shown in the
    // window while it is open and is not written anywhere.
};

class SettingsError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Missing file -> default-constructed StudSettings (not an error -- no
// settings file yet is the normal first-run state). Malformed file or
// an invalid field value -> throws SettingsError clearly, same
// no-silent-misbehavior policy as FlagOverrides::load_from_file.
StudSettings load_settings(const std::string& path);

// Writes the full settings object as the "gpu"/"hidpi"/"graphicsMode"
// top-level keys, preserving any other top-level keys already in the
// file (e.g. "devRenderBackend") rather than clobbering them -- this
// file is shared, not exclusively owned by this module. Creates the
// file (and its parent directory) if it doesn't exist yet.
void save_settings(const std::string& path, const StudSettings& settings);

// XDG-compliant path every Stud process should use for this shared file
// ($XDG_CONFIG_HOME/stud/config.json, or ~/.config/stud/config.json) --
// the one real config-path helper, shared so `ui` and `runtime` can't
// silently drift onto two different paths for what's meant to be one
// file.
std::string default_config_path();

// XDG-compliant path for the real, raw FFlag-overrides JSON file
// (see jni-bridge/include/stud/flag_overrides.h's own doc comment,
// "the locked decision": FFlag overrides live ONLY here, hand-edited
// by the user, never generated from Settings UI toggles). Sibling to
// default_config_path() ($XDG_CONFIG_HOME/stud/flags.json, or
// ~/.config/stud/flags.json). A missing file is the common case, not
// an error -- FlagOverrides::load_from_file() already treats it that
// way.
std::string default_flag_overrides_path();

}  // namespace stud::config
