// The shader-pack rewrites, on hand-written instruction sequences shaped
// like the desktop material shaders' (no Roblox shader is used here).

#include "stud/shader_pack.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
    std::printf("ok: %s\n", what);
}

uint32_t op(uint32_t code, uint32_t len) { return (len << 16) | code; }
uint32_t bits(float f) {
    uint32_t v;
    std::memcpy(&v, &f, sizeof v);
    return v;
}

// %1 float, %4 vec2, %8 vec4, %2 = 2.0, %3 = 1.0, %6 = vec2(1, 1), %20 the
// GLSL.std.450 set. The normal map is %10 at set 1 binding 4.
std::vector<uint32_t> normal_decode() {
    return {
        0x07230203, 0x10000, 0, 100, 0,
        op(71, 4), 10, 34, 1,          // OpDecorate %10 DescriptorSet 1
        op(71, 4), 10, 33, 4,          // OpDecorate %10 Binding 4
        op(22, 3), 1, 32,              // OpTypeFloat %1 32
        op(43, 4), 1, 2, bits(2.0f),   // OpConstant %2 = 2.0
        op(43, 4), 1, 3, bits(1.0f),   // OpConstant %3 = 1.0
        op(44, 5), 4, 6, 3, 3,         // OpConstantComposite %6 = (1, 1)
        op(61, 4), 7, 11, 10,          // %11 = OpLoad %10
        op(87, 5), 8, 12, 11, 13,      // %12 = OpImageSampleImplicitLod %11
        op(79, 7), 4, 14, 12, 12, 3, 1,  // %14 = %12.wy
        op(142, 5), 4, 15, 14, 2,      // %15 = %14 * 2
        op(131, 5), 4, 16, 15, 6,      // %16 = %15 - 1
        op(148, 5), 1, 17, 16, 16,     // %17 = dot(%16, %16)
        op(12, 6), 1, 18, 20, 31, 17,  // %18 = Sqrt(%17)
    };
}

void test_normal_reads_and_z() {
    std::vector<uint32_t> w = normal_decode();
    check(stud::shader_pack::rewrite_normal_reads(w) == 1, "a .wy read of the normal map is found");
    check(w[43] == 0 && w[44] == 1, "it becomes .xy");
    check(stud::shader_pack::rewrite_normal_reads(w) == 0, "a second pass changes nothing");

    const size_t before = w.size();
    check(stud::shader_pack::take_normal_z_from_map(w) == 1, "the rebuilt z is found");
    check(w.size() == before - 6 + 15, "the Sqrt is replaced by three instructions");
    check(w[3] == 102, "the id bound grows by the two new ids");
    const std::vector<uint32_t> tail(w.end() - 15, w.end());
    const std::vector<uint32_t> expect = {
        op(81, 5), 1, 100, 12, 2,  // %100 = %12.z
        op(133, 5), 1, 101, 100, 2,  // %101 = %100 * 2
        op(131, 5), 1, 18, 101, 3,   // %18 = %101 - 1
    };
    check(tail == expect, "z is the map's .z * 2 - 1, under the Sqrt's own result id");
    check(stud::shader_pack::take_normal_z_from_map(w) == 0, "a second pass changes nothing");
}

void test_detail_dropped() {
    // %30 at set 1 binding 8; %31 = 0.0; %40 a computed strength.
    std::vector<uint32_t> w = {
        0x07230203, 0x10000, 0, 100, 0,
        op(71, 4), 30, 34, 1,
        op(71, 4), 30, 33, 8,
        op(22, 3), 1, 32,
        op(43, 4), 1, 2, bits(2.0f),
        op(43, 4), 1, 31, 0,
        op(61, 4), 7, 32, 30,
        op(87, 5), 8, 33, 32, 13,
        op(79, 7), 4, 34, 33, 33, 0, 1,
        op(142, 5), 4, 35, 34, 2,   // decode: * 2, a constant, kept
        op(142, 5), 4, 36, 35, 40,  // strength: dropped
    };
    check(stud::shader_pack::drop_detail_normal(w) == 1, "the detail strength is found");
    check(w[44] == 2 && w[49] == 31, "the decode's constant stays, the strength becomes 0");
    check(stud::shader_pack::drop_detail_normal(w) == 0, "a second pass changes nothing");
}

void test_not_a_pack() {
    bool threw = false;
    try {
        stud::shader_pack::build_desktop_materials({'n', 'o', 'p', 'e'}, {});
    } catch (const stud::shader_pack::PackError&) {
        threw = true;
    }
    check(threw, "something that is not a pack is refused");
}

}  // namespace

// A hand-written GLSL 150 fragment shader shaped like the desktop
// material ones, and a vertex shader.
void test_glsl_conversion() {
    const std::string fs =
        "#version 150\n"
        "uniform vec4 CB0[61];\n"
        "uniform samplerCube PrefilteredEnvBlendTargetTexture;\n"
        "uniform sampler2D NormalMapTexture;\n"
        "layout(location = 2) in vec4 VARYING2;\n"
        "out vec4 _entryPointOutput;\n"
        "void main()\n"
        "{\n"
        "    vec4 _10 = texture(NormalMapTexture, VARYING2.xy);\n"
        "    vec2 _11 = _10.wy * 2.0;\n"
        "    vec2 _12 = _11 - vec2(1.0);\n"
        "    float _13 = sqrt(clamp(1.0 + dot(vec2(1.0) - _11, _12), 0.0, 1.0));\n"
        "    _entryPointOutput = vec4(_12, _13, 1.0) + texture(PrefilteredEnvBlendTargetTexture, VARYING2.xyz);\n"
        "}\n"
        "//$$PrefilteredEnvBlendTargetTexture=s2\n"
        "//$$NormalMapTexture=s4\n";
    size_t reads = 0;
    const std::string out = stud::shader_pack::convert_glsl(fs, &reads);
    auto has = [&](const char* s) { return out.find(s) != std::string::npos; };
    check(has("#version 300 es\nprecision highp float;\nprecision highp int;\n"), "GLSL ES 3.00 header");
    check(has("uniform highp sampler2D NormalMapTexture;"), "samplers get a precision");
    check(has("\nin vec4 VARYING2;"), "a varying loses its location");
    check(has("layout(location = 0) out vec4 _entryPointOutput;"), "the output gets location 0");
    check(has("vec2 _11 = _10.xy * 2.0;"), "the normal map is read as .xy");
    check(has("float _13 = ((_10).z * 2.0 - 1.0);") && reads == 1, "z comes from the map");
    check(!has("BlendTarget") && has("texture(PrefilteredEnvTexture,"), "the second cube becomes the first");

    const std::string vs =
        "#version 150\n"
        "in vec4 POSITION;\n"
        "in vec2 TEXCOORD1;\n"
        "layout(location = 0) out vec2 VARYING0;\n"
        "void main() { gl_Position = POSITION; VARYING0 = TEXCOORD1; }\n";
    const std::string vout = stud::shader_pack::convert_glsl(vs);
    check(vout.find("layout(location = 0) in vec4 POSITION;") != std::string::npos &&
              vout.find("layout(location = 5) in vec2 TEXCOORD1;") != std::string::npos,
          "attributes get the Android engine's locations");
    check(vout.find("\nout vec2 VARYING0;") != std::string::npos, "a vertex output loses its location");
    check(stud::shader_pack::convert_glsl("#version 150\nuniform sampler2DMS DepthAATexture;\n").empty(),
          "a shader needing a multisampled depth texture is refused");
}

int main() {
    test_normal_reads_and_z();
    test_detail_dropped();
    test_not_a_pack();
    test_glsl_conversion();
    return 0;
}
