#include "cube_gl.h"

#include <type_traits>

#include "spinning_cube.h"

namespace {

stud::tools::CubeGl g_gl{};
stud::tools::SpinningCube g_cube;

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
    return all && g_cube.init(g_gl);
}

void cube_gl_draw(float seconds, int width, int height) {
    // 2.1: the closest the camera can sit with no corner ever leaving the
    // drawable, measured over a full turn (90% of the half-width at most).
    g_cube.draw(g_gl, seconds, width, height, 2.1f, /*transparent=*/true);
}
