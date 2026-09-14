// M6 test: the prototype-only dev render-backend config toggle (prebuilt
// ANGLE / Stud's own patched ANGLE / Zink). See
// render/include/stud/dev_backend_config.h and the engineering notes.

#include "stud/dev_backend_config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

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

    // Missing file -> nullopt, not an error (no dev override is the
    // common/default case).
    auto missing = stud::render::load_dev_render_backend_config(dir + "/does_not_exist.json");
    check(!missing.has_value(), "missing config file loads as nullopt, not an error");

    // File exists but has no "devRenderBackend" key -> also nullopt.
    std::string no_key_path = dir + "/no_key.json";
    {
        std::ofstream out(no_key_path);
        out << R"({"someOtherSetting": true})";
    }
    auto no_key = stud::render::load_dev_render_backend_config(no_key_path);
    check(!no_key.has_value(), "config without a devRenderBackend key loads as nullopt");

    // Real "angle" mode config.
    std::string angle_path = dir + "/angle.json";
    {
        std::ofstream out(angle_path);
        out << R"({
            "devRenderBackend": {
                "mode": "angle",
                "eglPath": "/some/path/libEGL.so",
                "glesPath": "/some/path/libGLESv2.so"
            }
        })";
    }
    auto angle_cfg = stud::render::load_dev_render_backend_config(angle_path);
    check(angle_cfg.has_value(), "angle-mode config loads");
    check(angle_cfg->mode == stud::render::DevRenderBackendMode::kAngleVulkan, "mode is kAngleVulkan");
    // Library paths in the file are ignored: Stud always uses its own
    // ANGLE, found next to the running binary. An older build wrote them,
    // and since every install shares one config file, a single AppImage
    // run left every later launch pointing into a vanished mount.

    // A bare mode is the normal shape of this setting.
    std::string bare_angle_path = dir + "/bare_angle.json";
    {
        std::ofstream out(bare_angle_path);
        out << R"({"devRenderBackend": {"mode": "angle"}})";
    }
    auto bare_angle = stud::render::load_dev_render_backend_config(bare_angle_path);
    check(bare_angle.has_value(), "angle mode with no paths loads");

    // The two other real ANGLE backends.
    std::string gl_path = dir + "/angle_gl.json";
    {
        std::ofstream out(gl_path);
        out << R"({"devRenderBackend": {"mode": "angle-gl"}})";
    }
    auto gl_cfg = stud::render::load_dev_render_backend_config(gl_path);
    check(gl_cfg.has_value() && gl_cfg->mode == stud::render::DevRenderBackendMode::kAngleDesktopGL,
          "angle-gl selects the desktop GL backend");

    std::string sw_path = dir + "/angle_sw.json";
    {
        std::ofstream out(sw_path);
        out << R"({"devRenderBackend": {"mode": "angle-swiftshader"}})";
    }
    auto sw_cfg = stud::render::load_dev_render_backend_config(sw_path);
    check(sw_cfg.has_value() &&
              sw_cfg->mode == stud::render::DevRenderBackendMode::kAngleSwiftShader,
          "angle-swiftshader selects the software backend");

    // A config still naming the removed Zink backend is not an error.
    // It means the default now, so an existing install keeps working
    // instead of refusing to open its own settings.
    std::string zink_path = dir + "/zink.json";
    {
        std::ofstream out(zink_path);
        out << R"({"devRenderBackend": {"mode": "zink"}})";
    }
    auto zink_cfg = stud::render::load_dev_render_backend_config(zink_path);
    check(zink_cfg.has_value(), "a leftover zink config still loads");
    check(zink_cfg->mode == stud::render::DevRenderBackendMode::kAngleVulkan,
          "a leftover zink config falls back to the ANGLE default");

    // Unrecognized mode -> throws clearly.
    std::string bad_mode_path = dir + "/bad_mode.json";
    {
        std::ofstream out(bad_mode_path);
        out << R"({"devRenderBackend": {"mode": "d3d11"}})";
    }
    bool threw = false;
    try {
        stud::render::load_dev_render_backend_config(bad_mode_path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "unrecognized devRenderBackend mode throws instead of silently misbehaving");

    // Malformed JSON -> throws clearly.
    std::string malformed_path = dir + "/malformed.json";
    {
        std::ofstream out(malformed_path);
        out << "not valid json {{{";
    }
    threw = false;
    try {
        stud::render::load_dev_render_backend_config(malformed_path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "malformed config file throws instead of silently misbehaving");


    std::printf("all dev-backend-config checks passed\n");
    return 0;
}
