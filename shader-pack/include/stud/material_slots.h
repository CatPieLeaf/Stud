#pragma once

#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace stud::shader_pack {

// Where the Windows client's material shaders expect the textures Stud
// fills or rewrites: one number each, a GL texture unit and the Vulkan
// binding of the material set alike. Read from the Windows GL pack of the
// build in use, which states them (see material_slots_from_gl_pack), and
// left beside the combined packs for render-host. Fixed numbers here would
// put the wrong texture on every material the day Roblox moved one.
struct MaterialSlots {
    int normal = -1;        // NormalMapTexture
    int detail_normal = -1; // NormalDetailMapTexture
    int blend_target = -1;  // PrefilteredEnvBlendTargetTexture
    int brdf = -1;          // PrecomputedBRDFTexture
    int indoor_env = -1;    // PrefilteredEnvIndoorTexture
    int env = -1;           // PrefilteredEnvTexture

    bool complete() const {
        return normal >= 0 && detail_normal >= 0 && blend_target >= 0 && brdf >= 0 &&
               indoor_env >= 0 && env >= 0;
    }
};

inline constexpr const char* kMaterialSlotsFile = "material_slots";

inline std::string material_slots_text(const MaterialSlots& s) {
    std::ostringstream out;
    out << s.normal << ' ' << s.detail_normal << ' ' << s.blend_target << ' ' << s.brdf << ' '
        << s.indoor_env << ' ' << s.env << '\n';
    return out.str();
}

// Empty when the file is missing or incomplete, and the caller then leaves
// the textures alone rather than guessing.
inline std::optional<MaterialSlots> load_material_slots(const std::string& dir) {
    std::ifstream in(dir + "/" + kMaterialSlotsFile);
    MaterialSlots s;
    in >> s.normal >> s.detail_normal >> s.blend_target >> s.brdf >> s.indoor_env >> s.env;
    if (!in || !s.complete()) return std::nullopt;
    return s;
}

}  // namespace stud::shader_pack
