#include "stud/dev_backend_config.h"

#include <nlohmann/json.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <vector>

// For M8: the Qt settings UI's graphics-backend dropdown (prototype-only,
// alongside the real "OpenGL / Vulkan" mode picker, NOT the same
// control) should read/write exactly the "devRenderBackend" JSON key this
// file loads, in Stud's normal ~/.config/stud/config.json, three options
// ("Prebuilt ANGLE", "Stud's own build", "Zink (experimental)") map onto
// {"mode":"angle","eglPath":...,"glesPath":...} for the first two (the UI
// just changes which path pair it writes) and {"mode":"zink"} for the
// third. Delete this whole dropdown (and this module) before any public
// release build, same removal marker as the Zink decision itself in
// the engineering notes.

namespace stud::render {

std::optional<DevRenderBackendConfig> load_dev_render_backend_config(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return std::nullopt;
    }

    nlohmann::json doc;
    try {
        file >> doc;
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error("stud: malformed config file '" + path + "': " + e.what());
    }

    if (!doc.is_object() || !doc.contains("devRenderBackend")) {
        return std::nullopt;
    }

    const nlohmann::json& node = doc.at("devRenderBackend");
    if (!node.is_object() || !node.contains("mode") || !node.at("mode").is_string()) {
        throw std::runtime_error("stud: config '" + path +
                                  "' has an invalid \"devRenderBackend\" (expected an object with "
                                  "a string \"mode\")");
    }

    DevRenderBackendConfig result;
    const std::string mode = node.at("mode").get<std::string>();
    if (mode == "angle" || mode == "angle-gl" || mode == "angle-swiftshader") {
        result.mode = mode == "angle-gl"            ? DevRenderBackendMode::kAngleDesktopGL
                      : mode == "angle-swiftshader" ? DevRenderBackendMode::kAngleSwiftShader
                                                     : DevRenderBackendMode::kAngleVulkan;
        // The paths are optional: they only say which ANGLE build to load,
        // and the shipped one is the answer whenever they are absent.
        // eglPath/glesPath are read by nothing any more; an older build
        // may still have written them. See the header for why.
    } else if (mode == "zink") {
        // Zink was removed: it translates GL to Vulkan, so it cannot run at
        // all on the hardware this setting exists for, and where Vulkan
        // does work ANGLE was measured to do the same frame at a third of
        // the CPU. An existing config that still names it is not an error
        // it just means the default now.
        result.mode = DevRenderBackendMode::kAngleVulkan;
    } else {
        throw std::runtime_error("stud: config '" + path +
                                  "' has an unrecognized \"devRenderBackend\".mode \"" + mode +
                                  "\" (expected \"angle\", \"angle-gl\" or \"angle-swiftshader\")");
    }

    return result;
}

namespace {
// Mirrors stud-config's own make_directories(); this module doesn't
// link stud-config (would be a real, unwanted cross-module dependency
// just for one helper), so a small local copy is the cleaner call than
// introducing that link for four lines of mkdir -p logic.
void make_directories(const std::string& dir) {
    if (dir.empty()) {
        return;
    }
    std::string partial;
    for (size_t pos = 0; pos <= dir.size(); ++pos) {
        if (pos == dir.size() || dir[pos] == '/') {
            if (!partial.empty()) {
                ::mkdir(partial.c_str(), 0755);
            }
        }
        if (pos < dir.size()) {
            partial += dir[pos];
        }
    }
}
}  // namespace

void save_dev_render_backend_config(const std::string& path, const DevRenderBackendConfig& config) {
    nlohmann::json doc = nlohmann::json::object();
    {
        std::ifstream existing(path);
        if (existing.is_open()) {
            try {
                nlohmann::json existing_doc;
                existing >> existing_doc;
                if (existing_doc.is_object()) {
                    doc = existing_doc;
                }
            } catch (const nlohmann::json::parse_error&) {
                // Fall through with a fresh object, an unrelated malformed
                // file shouldn't block saving this setting.
            }
        }
    }

    const char* mode_name = config.mode == DevRenderBackendMode::kAngleDesktopGL ? "angle-gl"
                            : config.mode == DevRenderBackendMode::kAngleSwiftShader
                                ? "angle-swiftshader"
                                : "angle";
    // The mode, and nothing else. Any eglPath/glesPath an older build
    // left behind is dropped here rather than carried forward; that is
    // what repairs a config poisoned by an AppImage's own temporary
    // mount path, without asking the user to know any of this.
    doc["devRenderBackend"] = {{"mode", mode_name}};

    auto slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        make_directories(path.substr(0, slash));
    }

    std::ofstream out(path);
    if (!out.is_open()) {
        throw std::runtime_error("stud: failed to write config file '" + path + "'");
    }
    out << doc.dump(2);
}

ShippedAnglePaths shipped_angle_paths() {
    // Stud's own ANGLE, always, the one it was built and tested with,
    // shipped beside its binaries. There is deliberately no way to point
    // it at another: the render path is Stud's, and a mismatched ANGLE is
    // a class of bug reported as Stud's without being Stud's.
    //
    // Checked with access(), not assumed: a checkout that never ran
    // tools/setup.sh gets a clear error here rather than a dlopen failure
    // much later inside render::set_angle_library_paths().
    // Relative to the running binary, never absolute: an AppImage is
    // mounted somewhere different every run, and a package, a build tree
    // and that mount are three different layouts of the same thing.
    std::vector<std::string> bases;
    {
        char buf[4096];
        const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            std::string exe(buf);
            const auto slash = exe.find_last_of('/');
            if (slash != std::string::npos) {
                const std::string dir = exe.substr(0, slash);
                bases.emplace_back(dir + "/../lib/stud/angle/");
                bases.emplace_back(dir + "/../../lib/stud/angle/");
            }
        }
    }
#ifdef STUD_THIRD_PARTY_DIR
    bases.emplace_back(std::string(STUD_THIRD_PARTY_DIR) + "/angle/");
#endif
    for (const std::string& base : bases) {
        std::string egl = base + "libEGL.so";
        std::string gles = base + "libGLESv2.so";
        if (::access(egl.c_str(), R_OK) == 0 && ::access(gles.c_str(), R_OK) == 0) {
            return ShippedAnglePaths{egl, gles};
        }
    }
    throw ShippedAngleNotFound();
}

}  // namespace stud::render
