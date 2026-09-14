#pragma once

// The spinning cube the render smoke test draws.
//
// Ported from c2d7fa's opengl-cube (https://github.com/c2d7fa/opengl-cube),
// which is dedicated to the public domain under CC0-1.0. The geometry,
// the vertex colours, the column-major matrix helpers and the projection
// are that project's; the credit is kept because it is the decent thing
// to do, not because CC0 asks for it.
//
// Two real changes were needed. The original targets desktop OpenGL 4.5
// through GLFW and GLEW; Stud's render path is GLES2 on EGL, which this
// tool already sets up, so the shaders are GLSL ES 1.00 (attributes and
// varyings rather than layout-qualified in/out) and there is no vertex
// array object, GLES2 has none, so the attribute pointers are bound
// per frame instead.
//
// Why a cube rather than the colour-cycling clear this replaces: a clear
// proves the surface presents, and nothing more. A cube with depth
// testing, an index buffer, a shader program and a per-frame uniform
// exercises the parts of the pipeline that actually break, which is
// the whole point of a smoke test that exists to answer "is it the
// machine, the driver or Stud?".

#include <GLES2/gl2.h>

#include <cmath>
#include <cstdio>

namespace stud::tools {

// The GLES2 entry points the cube needs, resolved by the caller the same
// way it resolves every other one.
struct CubeGl {
    void (*Enable)(GLenum);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*Clear)(GLbitfield);
    GLuint (*CreateShader)(GLenum);
    void (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
    void (*CompileShader)(GLuint);
    void (*GetShaderiv)(GLuint, GLenum, GLint*);
    void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    GLuint (*CreateProgram)();
    void (*AttachShader)(GLuint, GLuint);
    void (*LinkProgram)(GLuint);
    void (*GetProgramiv)(GLuint, GLenum, GLint*);
    void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    void (*UseProgram)(GLuint);
    void (*GenBuffers)(GLsizei, GLuint*);
    void (*BindBuffer)(GLenum, GLuint);
    void (*BufferData)(GLenum, GLsizeiptr, const void*, GLenum);
    GLint (*GetAttribLocation)(GLuint, const GLchar*);
    GLint (*GetUniformLocation)(GLuint, const GLchar*);
    void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
    void (*EnableVertexAttribArray)(GLuint);
    void (*UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*);
    void (*DrawElements)(GLenum, GLsizei, GLenum, const void*);
};

// Column-major, so it is handed to glUniformMatrix4fv as-is.
struct Mat4 {
    float m[16];
};

inline Mat4 mat4_identity() {
    return Mat4{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
}

inline Mat4 mat4_multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            r.m[col * 4 + row] = sum;
        }
    }
    return r;
}

inline Mat4 mat4_translation(float x, float y, float z) {
    return Mat4{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1}};
}

inline Mat4 mat4_rotate_x(float t) {
    return Mat4{{1, 0, 0, 0, 0, std::cos(t), std::sin(t), 0, 0, -std::sin(t), std::cos(t), 0, 0, 0,
                  0, 1}};
}

inline Mat4 mat4_rotate_y(float t) {
    return Mat4{{std::cos(t), 0, -std::sin(t), 0, 0, 1, 0, 0, std::sin(t), 0, std::cos(t), 0, 0, 0,
                  0, 1}};
}

// The original's own projection, with the aspect ratio threaded through
// so the cube is not stretched by a non-square window (the original is
// always square).
inline Mat4 mat4_perspective(float aspect) {
    const float r = 0.5f * (aspect > 0.0f ? aspect : 1.0f);
    const float t = 0.5f;
    const float n = 1.0f;
    const float f = 5.0f;
    return Mat4{{n / r, 0, 0, 0, 0, n / t, 0, 0, 0, 0, (-f - n) / (f - n), -1, 0, 0,
                  (2 * f * n) / (n - f), 0}};
}

class SpinningCube {
  public:
    bool init(const CubeGl& gl) {
        static const char* kVertex =
            "attribute vec3 pos;\n"
            "attribute vec3 vertex_color;\n"
            "uniform mat4 transform;\n"
            "varying vec3 color;\n"
            "void main() {\n"
            "  gl_Position = transform * vec4(pos, 1.0);\n"
            "  color = vertex_color;\n"
            "}\n";
        static const char* kFragment =
            "precision mediump float;\n"
            "varying vec3 color;\n"
            "void main() {\n"
            "  gl_FragColor = vec4(color, 1.0);\n"
            "}\n";

        const GLuint vs = compile(gl, GL_VERTEX_SHADER, kVertex);
        const GLuint fs = compile(gl, GL_FRAGMENT_SHADER, kFragment);
        if (vs == 0 || fs == 0) return false;

        program_ = gl.CreateProgram();
        gl.AttachShader(program_, vs);
        gl.AttachShader(program_, fs);
        gl.LinkProgram(program_);
        GLint linked = 0;
        gl.GetProgramiv(program_, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            char log[1024] = {};
            gl.GetProgramInfoLog(program_, sizeof log - 1, nullptr, log);
            std::fprintf(stderr, "stud: cube program link failed: %s\n", log);
            return false;
        }

        attr_pos_ = gl.GetAttribLocation(program_, "pos");
        attr_color_ = gl.GetAttribLocation(program_, "vertex_color");
        uniform_transform_ = gl.GetUniformLocation(program_, "transform");

        // The original's own cube: eight corners, one colour each, and
        // twelve triangles indexing them.
        static const float kVertices[] = {
            0.5f,  0.5f,  0.5f,  -0.5f, 0.5f,  0.5f,  -0.5f, -0.5f, 0.5f,  0.5f,  -0.5f, 0.5f,
            0.5f,  0.5f,  -0.5f, -0.5f, 0.5f,  -0.5f, -0.5f, -0.5f, -0.5f, 0.5f,  -0.5f, -0.5f,
        };
        static const float kColors[] = {
            1.0f, 0.4f, 0.6f, 1.0f, 0.9f, 0.2f, 0.7f, 0.3f, 0.8f, 0.5f, 0.3f, 1.0f,
            0.2f, 0.6f, 1.0f, 0.6f, 1.0f, 0.4f, 0.6f, 0.8f, 0.8f, 0.4f, 0.8f, 0.8f,
        };
        static const GLushort kIndices[] = {
            0, 1, 2, 2, 3, 0,  // front
            0, 3, 7, 7, 4, 0,  // right
            2, 6, 7, 7, 3, 2,  // bottom
            1, 5, 6, 6, 2, 1,  // left
            4, 7, 6, 6, 5, 4,  // back
            5, 1, 0, 0, 4, 5,  // top
        };

        gl.GenBuffers(1, &vbo_);
        gl.BindBuffer(GL_ARRAY_BUFFER, vbo_);
        gl.BufferData(GL_ARRAY_BUFFER, sizeof kVertices, kVertices, GL_STATIC_DRAW);

        gl.GenBuffers(1, &colors_);
        gl.BindBuffer(GL_ARRAY_BUFFER, colors_);
        gl.BufferData(GL_ARRAY_BUFFER, sizeof kColors, kColors, GL_STATIC_DRAW);

        gl.GenBuffers(1, &ebo_);
        gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
        gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof kIndices, kIndices, GL_STATIC_DRAW);

        gl.Enable(GL_DEPTH_TEST);
        return true;
    }

    // seconds drives the spin; one turn every four seconds, as in the
    // original.
    void draw(const CubeGl& gl, float seconds, int width, int height) {
        const float pi = 3.141593f;
        gl.Viewport(0, 0, width, height);
        gl.ClearColor(0.1f, 0.12f, 0.2f, 1.0f);
        gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        gl.UseProgram(program_);

        const float aspect =
            height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
        Mat4 transform = mat4_identity();
        transform = mat4_multiply(transform, mat4_perspective(aspect));
        transform = mat4_multiply(transform, mat4_translation(0, 0, -3));
        transform = mat4_multiply(transform, mat4_rotate_x(0.15f * pi));
        transform = mat4_multiply(transform, mat4_rotate_y(2 * pi * (seconds / 4.0f)));
        gl.UniformMatrix4fv(uniform_transform_, 1, GL_FALSE, transform.m);

        gl.BindBuffer(GL_ARRAY_BUFFER, vbo_);
        gl.VertexAttribPointer(static_cast<GLuint>(attr_pos_), 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        gl.EnableVertexAttribArray(static_cast<GLuint>(attr_pos_));
        gl.BindBuffer(GL_ARRAY_BUFFER, colors_);
        gl.VertexAttribPointer(static_cast<GLuint>(attr_color_), 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        gl.EnableVertexAttribArray(static_cast<GLuint>(attr_color_));
        gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
        gl.DrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
    }

  private:
    static GLuint compile(const CubeGl& gl, GLenum type, const char* source) {
        const GLuint shader = gl.CreateShader(type);
        gl.ShaderSource(shader, 1, &source, nullptr);
        gl.CompileShader(shader);
        GLint ok = 0;
        gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (ok != GL_TRUE) {
            char log[1024] = {};
            gl.GetShaderInfoLog(shader, sizeof log - 1, nullptr, log);
            std::fprintf(stderr, "stud: cube shader compile failed: %s\n", log);
            return 0;
        }
        return shader;
    }

    GLuint program_ = 0;
    GLuint vbo_ = 0;
    GLuint colors_ = 0;
    GLuint ebo_ = 0;
    GLint attr_pos_ = 0;
    GLint attr_color_ = 0;
    GLint uniform_transform_ = -1;
};

}  // namespace stud::tools
