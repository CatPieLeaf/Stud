#pragma once

#include <optional>
#include <stdexcept>
#include <string>

// Prototype-only developer setting: lets Stud's runtime switch which
// render backend render/ actually dlopens, without rebuilding. See
// the engineering notes ("M6 resumed: real ANGLE build started...") for the
// full reasoning: real, load-bearing logic for M6/M7 development,
// explicitly NOT meant to survive into a public release (same
// "prototype-phase flexibility, revisit before release" category as
// FFlag-JSON-only and the Zink-prototype-only decision already locked
// in).
//
// Backed by the same JSON config file every other Stud setting will
// eventually live in (~/.config/stud/config.json, per the locked
// "Settings storage" decision) under a "devRenderBackend" key,
// deliberately, so M8's Qt settings UI can wire a dropdown directly to
// this same key later instead of inventing a second config path. See
// the "For M8" note in dev_backend_config.cpp for exactly what that
// wiring should look like.
//
// Only compiled when STUD_ENABLE_DEV_RENDER_TOGGLE is on (top-level
// CMakeLists.txt option, default ON during the prototype phase),
// absent entirely from a build with that option off, not just inert at
// runtime.

namespace stud::render {

enum class DevRenderBackendMode {
    // ANGLE translating to Vulkan. The default, and the fastest of these
    // wherever a working Vulkan driver exists.
    kAngleVulkan,
    // ANGLE translating to the host's desktop OpenGL. This is the entry
    // that exists for hardware too old for Vulkan: nothing else here can
    // run at all on such a machine. Measured working; see the frame
    // dump result recorded in the engineering notes.
    kAngleDesktopGL,
    // ANGLE on SwiftShader, its own bundled CPU Vulkan implementation.
    // No GPU or driver involved at all, so it is the last resort when
    // neither of the above initialises. Slow by construction, not by
    // defect.
    kAngleSwiftShader,
};

// What is remembered: the mode, and only the mode.
//
// It deliberately carries no library paths. Where ANGLE is depends on how
// Stud is being run, next to the binary for a package, inside a
// temporary mount for an AppImage, in third_party/ for a build tree,
// which makes it a thing to look up at startup, never a thing to write
// down. An earlier version did write it down, and since all three
// installs share one config file, a single AppImage run left every later
// launch loading a library from a mount that no longer existed.
//
// Stud always uses its own ANGLE; there is deliberately no way to point
// it at another one.
struct DevRenderBackendConfig {
    DevRenderBackendMode mode = DevRenderBackendMode::kAngleVulkan;
};

// Loads the "devRenderBackend" key from a Stud config JSON file. Missing
// file, or a file with no "devRenderBackend" key, returns std::nullopt:
// this setting is an override, not a requirement, so the caller falls
// back to whatever default/CLI-provided paths it already has. Throws
// std::runtime_error on a malformed file or an unrecognized mode. Any
// eglPath/glesPath a older build left in the file is ignored, and
// removed the next time this key is written.
std::optional<DevRenderBackendConfig> load_dev_render_backend_config(const std::string& path);

// Writes the "devRenderBackend" key into a Stud config JSON file,
// preserving every other top-level key already there (same
// read-merge-write pattern stud-config's own save_settings() uses.
// This file is shared, not exclusively owned by this module). Creates
// the file (and parent directories) if it doesn't exist yet. This is
// the real M8 wiring point named in the "For M8" note in
// dev_backend_config.cpp: the Settings window's dev-only backend
// dropdown calls this on Save.
void save_dev_render_backend_config(const std::string& path, const DevRenderBackendConfig& config);

// Real path pair for Stud's own vendored ANGLE build (see the engineering notes,
// "M6 resumed: real ANGLE build started", the actual build this project
// produced from source, `third_party/angle/`, put there by tools/setup.sh). No AppImage/M11
// packaging layout exists yet, so this is the honest current answer to
// "where is Stud's shipped ANGLE," not a placeholder, revisit once M11
// defines a real install layout (check next to the running binary first,
// same pattern find_runtime_binary() in ui/src/main.cpp already uses,
// falling back to this path only on a dev checkout). Settings UI's
// backend chooser (ANGLE/Zink only, no path picker, deliberately, so
// there's nothing for a user to get wrong here) calls this directly
// instead of asking the user to browse to a .so by hand.
struct ShippedAngleNotFound : std::runtime_error {
    ShippedAngleNotFound()
        : std::runtime_error("stud: could not find Stud's own built ANGLE (libEGL.so/libGLESv2.so), "
                              "build it first (see the engineering notes' ANGLE build notes)") {}
};
struct ShippedAnglePaths {
    std::string egl_path;
    std::string gles_path;
};

// Where this particular run finds Stud's own ANGLE: next to the running
// binary, then the build tree's third_party/. Answered at startup, every
// startup; see DevRenderBackendConfig above for why the answer is
// never saved, and shipped_angle_paths() for why there is no override.
ShippedAnglePaths shipped_angle_paths();

}  // namespace stud::render
