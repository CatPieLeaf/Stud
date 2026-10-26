#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// Windows Graphical Parity: the APK's own shader pack, with the Windows
// client's builds of the material shaders put in where they fit the
// Android engine.
//
// Both packs are Roblox's, and neither ships with Stud. The mobile one
// comes out of the user's own APK; the desktop one is downloaded, at run
// time, from Roblox's own CDN (see ui/src/desktop_materials.cpp). This
// library only combines them, on the user's machine.
//
// The container ("RBXS"): an 0x54-byte header (u16 name count at 12, u16
// permutation count at 14), the shader-name table (68-byte records, the
// name in the first 64), two tables this code copies through unchanged,
// the permutation table (64-byte records: u32 blob offset at 16, u32 blob
// size at 20, u32 variant key at 24, u16 name index at 30), then the
// blobs, each a 24-byte header and a SPIR-V module. The engine builds a
// pipeline's descriptor layout from the binding masks in that header, so a
// blob carries its own layout with it.
namespace stud::shader_pack {

class PackError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// What a build did, for the log.
struct BuildReport {
    size_t substituted = 0;       // permutations now running the desktop blob
    size_t refused_capability = 0;
    size_t refused_image = 0;     // a binding both declare, as a different kind of image
    size_t refused_vertex_io = 0; // vertex shaders whose stage interface differs
    size_t grafted = 0;           // desktop-only variants added
    size_t aliases = 0;           // two-sided lit keys answered with the one-sided blob
    size_t normal_reads = 0;      // .wy normal-map reads rewritten to .xy
    size_t normal_z = 0;          // normal z taken from the map
    size_t detail_dropped = 0;    // Plastic detail-normal terms removed
    // Of `substituted`: those with the very same bindings as the mobile
    // build, taken because their stage interface matches as well, by
    // shader name.
    size_t drop_in = 0;
    std::map<std::string, size_t> drop_in_by_shader;
    size_t refused_interface = 0;  // same bindings, different stage interface
};

// The combined pack. Throws PackError when either input is not a pack this
// code understands, or when the result fails its own check that every
// permutation points at a SPIR-V module.
std::vector<uint8_t> build_desktop_materials(const std::vector<uint8_t>& mobile,
                                             const std::vector<uint8_t>& desktop,
                                             BuildReport* report = nullptr);

// The same for the GL path: the APK's GLSL ES pack (shaders_glsles3.pack)
// with every shader the Windows GLSL 150 pack (shaders_glsl3.pack) builds
// converted to GLSL ES 3.00 and put in its place, Windows-only variants
// added. The APK's build stays only for terrain (the Windows one samples
// texture arrays where the Android engine binds atlases), for permutations
// that read a multisampled depth texture the Android engine never makes,
// and where Windows has no build at all.
struct GlBuildReport {
    size_t substituted = 0;
    size_t grafted = 0;
    size_t kept_terrain = 0;
    size_t kept_unconvertible = 0;
    size_t normal_reads = 0;  // normal-map decodes taken as RGB, z from the map
};
std::vector<uint8_t> build_desktop_materials_gl(const std::vector<uint8_t>& mobile,
                                                const std::vector<uint8_t>& desktop,
                                                GlBuildReport* report = nullptr);

// One Windows GLSL 150 shader as GLSL ES 3.00, adapted to what the Android
// engine binds. Empty when it needs something the engine never provides.
// Exposed for tests.
std::string convert_glsl(const std::string& desktop, size_t* normal_reads = nullptr);

// The SPIR-V rewrites, on one module (no blob header), exposed for tests.
// Each returns how many sites it changed; a module with none comes back
// unchanged.
size_t rewrite_normal_reads(std::vector<uint32_t>& words);
size_t take_normal_z_from_map(std::vector<uint32_t>& words);
size_t drop_detail_normal(std::vector<uint32_t>& words);

}  // namespace stud::shader_pack
