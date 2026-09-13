// M4 test: FFlag overrides, including the exact graphics-API flags found
// in libroblox.so's own strings (DebugGraphicsDisableVulkan,
// DebugGraphicsPreferVulkan, GraphicsMode, GraphicsQualityLevel) -- proves
// Stud's flag system can carry Vulkan/OpenGL mode overrides end-to-end from
// a user-editable JSON file to the wire format nativePreloadFlagOverrides
// expects. See the engineering notes, milestone M4 and the "Nvidia stability"
// section for why this matters.

#include "stud/flag_overrides.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace {

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
    std::printf("ok: %s\n", what);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 1;
    }
    std::string dir = argv[1];

    // Missing file -> empty overrides, not an error (no override file is
    // the common case).
    auto missing = stud::jni_bridge::FlagOverrides::load_from_file(dir + "/does_not_exist.json");
    check(missing.size() == 0, "missing override file loads as empty, not an error");

    std::string path = dir + "/client_app_settings.json";
    {
        std::ofstream out(path);
        out << R"({
            "DebugGraphicsDisableVulkan": false,
            "DebugGraphicsPreferVulkan": true,
            "GraphicsMode": "Vulkan",
            "GraphicsQualityLevel": 21,
            "FFlagSomeBoolFlag": true
        })";
    }

    auto overrides = stud::jni_bridge::FlagOverrides::load_from_file(path);
    check(overrides.size() == 5, "loaded all 5 overrides from the file");
    check(overrides.has("DebugGraphicsPreferVulkan"), "DebugGraphicsPreferVulkan present after load");
    check(overrides.has("GraphicsQualityLevel"), "GraphicsQualityLevel present after load");

    std::string wire = overrides.to_wire_format();
    auto parsed = nlohmann::json::parse(wire);

    check(parsed["DebugGraphicsDisableVulkan"] == "False",
          "DebugGraphicsDisableVulkan serializes as \"False\" in wire format");
    check(parsed["DebugGraphicsPreferVulkan"] == "True",
          "DebugGraphicsPreferVulkan serializes as \"True\" -- the actual override that forces "
          "Roblox's engine toward Vulkan");
    check(parsed["GraphicsMode"] == "Vulkan", "GraphicsMode round-trips as a string");
    check(parsed["GraphicsQualityLevel"] == "21", "GraphicsQualityLevel serializes as a string, not a number");
    check(parsed["FFlagSomeBoolFlag"] == "True", "arbitrary FFlag-prefixed name round-trips too");

    // Malformed file -> throws clearly, doesn't silently return garbage.
    std::string bad_path = dir + "/bad.json";
    {
        std::ofstream out(bad_path);
        out << "not valid json {{{";
    }
    bool threw = false;
    try {
        stud::jni_bridge::FlagOverrides::load_from_file(bad_path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "malformed override file throws instead of silently misbehaving");

    std::printf("all flag-overrides checks passed\n");
    return 0;
}
