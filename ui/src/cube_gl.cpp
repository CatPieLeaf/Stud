#include "cube_gl.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <type_traits>
#include <vector>

#include "mac_client.h"
#include "spinning_cube.h"

struct CubeGlState {
    stud::tools::CubeGl gl{};
    stud::tools::SpinningCube cube;
};

namespace {

// One Roblox stud, out of the studs texture the client ships, read from the
// installed Roblox Player rather than carried in Stud.
//
// The file is a strip of square tiles, one per surface type, Studs first,
// and each tile holds two studs by two: so the top-left quarter of the
// first tile is exactly one stud. It is DXT1; anything else, or no client
// installed yet, and the cube keeps its plain colours.
std::vector<unsigned char> load_one_stud(int& size) {
    const auto build = stud::ui::client::installed(stud::ui::client::Product::Player);
    if (!build) return {};
    std::ifstream in((build->app + "/Contents/Resources/PlatformContent/pc/textures/studs.dds").toStdString(),
                     std::ios::binary);
    const std::vector<unsigned char> d((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
    auto u32 = [&d](size_t at) {
        uint32_t v = 0;
        std::memcpy(&v, d.data() + at, sizeof v);
        return v;
    };
    constexpr size_t kHeader = 128;
    constexpr uint32_t kFourCc = 0x4;
    constexpr int kBlock = 4;
    constexpr size_t kBlockBytes = 8;
    if (d.size() < kHeader || std::memcmp(d.data(), "DDS ", 4) != 0) return {};
    const uint32_t height = u32(12);
    const uint32_t width = u32(16);
    const size_t blocks_across = width / kBlock;
    if ((u32(80) & kFourCc) == 0 || std::memcmp(d.data() + 84, "DXT1", 4) != 0 || width < 2 * kBlock ||
        height < width || d.size() < kHeader + blocks_across * (width / kBlock) * kBlockBytes) {
        return {};
    }
    size = static_cast<int>(width / 2);
    std::vector<unsigned char> stud(static_cast<size_t>(size) * size);
    // A 5:6:5 colour's luminance, at 8 bits.
    auto luminance = [](uint16_t c, int weight_of_other = 0, uint16_t other = 0, int parts = 1) {
        auto channel = [](uint16_t v, int shift, int bits) { return ((v >> shift) & ((1 << bits) - 1)) * 255 / ((1 << bits) - 1); };
        auto lum = [&](uint16_t v) { return (channel(v, 11, 5) * 77 + channel(v, 5, 6) * 150 + channel(v, 0, 5) * 29) >> 8; };
        return (lum(c) * (parts - weight_of_other) + lum(other) * weight_of_other) / parts;
    };
    for (int by = 0; by < size / kBlock; ++by) {
        for (int bx = 0; bx < size / kBlock; ++bx) {
            const unsigned char* block = d.data() + kHeader + (by * blocks_across + bx) * kBlockBytes;
            uint16_t c0 = 0, c1 = 0;
            uint32_t indices = 0;
            std::memcpy(&c0, block, 2);
            std::memcpy(&c1, block + 2, 2);
            std::memcpy(&indices, block + 4, 4);
            int palette[4];
            palette[0] = luminance(c0);
            palette[1] = luminance(c1);
            if (c0 > c1) {
                palette[2] = luminance(c0, 1, c1, 3);
                palette[3] = luminance(c0, 2, c1, 3);
            } else {
                palette[2] = luminance(c0, 1, c1, 2);
                palette[3] = 0;
            }
            for (int y = 0; y < kBlock; ++y) {
                for (int x = 0; x < kBlock; ++x) {
                    const int index = (indices >> (2 * (y * kBlock + x))) & 0x3;
                    stud[static_cast<size_t>(by * kBlock + y) * size + bx * kBlock + x] =
                        static_cast<unsigned char>(palette[index]);
                }
            }
        }
    }
    return stud;
}

}  // namespace

CubeGlState* cube_gl_create(void* (*resolve)(const char* name, void* data), void* data) {
    auto* state = new CubeGlState();
    stud::tools::CubeGl& g_gl = state->gl;
    bool all = true;
    auto load = [&](auto& field, const char* name) {
        field = reinterpret_cast<std::remove_reference_t<decltype(field)>>(resolve(name, data));
        if (field == nullptr) all = false;
    };
    load(g_gl.Enable, "glEnable");
    load(g_gl.Viewport, "glViewport");
    load(g_gl.ClearColor, "glClearColor");
    load(g_gl.Clear, "glClear");
    load(g_gl.CreateShader, "glCreateShader");
    load(g_gl.ShaderSource, "glShaderSource");
    load(g_gl.CompileShader, "glCompileShader");
    load(g_gl.GetShaderiv, "glGetShaderiv");
    load(g_gl.GetShaderInfoLog, "glGetShaderInfoLog");
    load(g_gl.CreateProgram, "glCreateProgram");
    load(g_gl.AttachShader, "glAttachShader");
    load(g_gl.LinkProgram, "glLinkProgram");
    load(g_gl.GetProgramiv, "glGetProgramiv");
    load(g_gl.GetProgramInfoLog, "glGetProgramInfoLog");
    load(g_gl.UseProgram, "glUseProgram");
    load(g_gl.GenBuffers, "glGenBuffers");
    load(g_gl.BindBuffer, "glBindBuffer");
    load(g_gl.BufferData, "glBufferData");
    load(g_gl.GetAttribLocation, "glGetAttribLocation");
    load(g_gl.GetUniformLocation, "glGetUniformLocation");
    load(g_gl.VertexAttribPointer, "glVertexAttribPointer");
    load(g_gl.EnableVertexAttribArray, "glEnableVertexAttribArray");
    load(g_gl.UniformMatrix4fv, "glUniformMatrix4fv");
    load(g_gl.DrawElements, "glDrawElements");
    if (!all) {
        delete state;
        return nullptr;
    }
    // Optional: without them the cube is just not textured.
    auto optional = [&](auto& field, const char* name) {
        field = reinterpret_cast<std::remove_reference_t<decltype(field)>>(resolve(name, data));
    };
    optional(g_gl.GenTextures, "glGenTextures");
    optional(g_gl.BindTexture, "glBindTexture");
    optional(g_gl.TexImage2D, "glTexImage2D");
    optional(g_gl.TexParameteri, "glTexParameteri");
    optional(g_gl.Uniform1i, "glUniform1i");
    int size = 0;
    const std::vector<unsigned char> stud = load_one_stud(size);
    if (!state->cube.init(g_gl, stud.empty() ? nullptr : stud.data(), size)) {
        delete state;
        return nullptr;
    }
    return state;
}

void cube_gl_draw(CubeGlState* state, float seconds, int width, int height) {
    // 2.1: the closest the camera can sit with no corner ever leaving the
    // drawable, measured over a full turn (90% of the half-width at most).
    state->cube.draw(state->gl, seconds, width, height, 2.1f, /*transparent=*/true);
}

void cube_gl_destroy(CubeGlState* state) { delete state; }
