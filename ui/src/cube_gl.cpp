#include "cube_gl.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <type_traits>
#include <vector>

#include "spinning_cube.h"
#include "stud/stud_paths.h"

namespace {

stud::tools::CubeGl g_gl{};
stud::tools::SpinningCube g_cube;

// One Roblox stud, out of the studs texture the engine ships, read from
// the copy Stud extracted from the APK rather than carried in Stud.
//
// The file is a strip of square tiles, one per surface type, Studs first,
// and each tile holds two studs by two: so the top-left quarter of the
// first tile is exactly one stud. Uncompressed 8-bit luminance; anything
// else, or no file yet (before the first launch extracts it), and the
// cube keeps its plain colours.
std::vector<unsigned char> load_one_stud(int& size) {
    std::ifstream in(stud::paths::cache_dir() + "/assets/android/textures/studs.dds",
                     std::ios::binary);
    const std::vector<unsigned char> d((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
    auto u32 = [&d](size_t at) {
        uint32_t v = 0;
        std::memcpy(&v, d.data() + at, sizeof v);
        return v;
    };
    constexpr size_t kHeader = 128;
    constexpr uint32_t kLuminance = 0x20000;
    if (d.size() < kHeader || std::memcmp(d.data(), "DDS ", 4) != 0) return {};
    const uint32_t height = u32(12);
    const uint32_t width = u32(16);
    if ((u32(80) & kLuminance) == 0 || u32(88) != 8 || width < 2 || height < width ||
        d.size() < kHeader + static_cast<size_t>(width) * width) {
        return {};
    }
    size = static_cast<int>(width / 2);
    std::vector<unsigned char> stud(static_cast<size_t>(size) * size);
    for (int row = 0; row < size; ++row) {
        std::memcpy(stud.data() + static_cast<size_t>(row) * size,
                    d.data() + kHeader + static_cast<size_t>(row) * width, size);
    }
    return stud;
}

}  // namespace

bool cube_gl_init(void* (*resolve)(const char* name, void* data), void* data) {
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
    if (!all) return false;
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
    return g_cube.init(g_gl, stud.empty() ? nullptr : stud.data(), size);
}

void cube_gl_draw(float seconds, int width, int height) {
    // 2.1: the closest the camera can sit with no corner ever leaving the
    // drawable, measured over a full turn (90% of the half-width at most).
    g_cube.draw(g_gl, seconds, width, height, 2.1f, /*transparent=*/true);
}
