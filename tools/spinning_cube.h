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
    // Only for a textured cube; a caller that leaves them null gets the
    // original's plain colours.
    void (*GenTextures)(GLsizei, GLuint*);
    void (*BindTexture)(GLenum, GLuint);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                       const void*);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*Uniform1i)(GLint, GLint);
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
    // `face`, when given, is a `face_size` x `face_size` luminance image put
    // on every side: a shading map, mid-grey where it leaves the colour as
    // it is, which is how the stud texture Roblox ships works.
    bool init(const CubeGl& gl, const unsigned char* face = nullptr, int face_size = 0) {
        static const char* kVertex =
            "attribute vec3 pos;\n"
            "attribute vec3 vertex_color;\n"
            "attribute vec2 vertex_uv;\n"
            "uniform mat4 transform;\n"
            "varying vec3 color;\n"
            "varying vec2 uv;\n"
            "void main() {\n"
            "  gl_Position = transform * vec4(pos, 1.0);\n"
            "  color = vertex_color;\n"
            "  uv = vertex_uv;\n"
            "}\n";
        static const char* kFragment =
            // GLES requires a default float precision and desktop GL's
            // older GLSL rejects the statement, so it is given only where
            // GL_ES says the compiler is a GLES one.
            "#ifdef GL_ES\n"
            "precision mediump float;\n"
            "#endif\n"
            "varying vec3 color;\n"
            "varying vec2 uv;\n"
            "uniform sampler2D face;\n"
            "uniform bool textured;\n"
            "void main() {\n"
            "  float shade = textured ? 2.0 * texture2D(face, uv).r : 1.0;\n"
            "  gl_FragColor = vec4(color * shade, 1.0);\n"
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
        attr_uv_ = gl.GetAttribLocation(program_, "vertex_uv");
        uniform_transform_ = gl.GetUniformLocation(program_, "transform");
        uniform_textured_ = gl.GetUniformLocation(program_, "textured");

        // The original's own cube: eight corners, one colour each, and
        // twelve triangles indexing them.
        static const float kCorners[] = {
            0.5f,  0.5f,  0.5f,  -0.5f, 0.5f,  0.5f,  -0.5f, -0.5f, 0.5f,  0.5f,  -0.5f, 0.5f,
            0.5f,  0.5f,  -0.5f, -0.5f, 0.5f,  -0.5f, -0.5f, -0.5f, -0.5f, 0.5f,  -0.5f, -0.5f,
        };
        static const float kColors[] = {
            1.0f, 0.4f, 0.6f, 1.0f, 0.9f, 0.2f, 0.7f, 0.3f, 0.8f, 0.5f, 0.3f, 1.0f,
            0.2f, 0.6f, 1.0f, 0.6f, 1.0f, 0.4f, 0.6f, 0.8f, 0.8f, 0.4f, 0.8f, 0.8f,
        };
        // The same twelve triangles, as six quads of corner indices. A face
        // needs corners of its own to carry its own texture coordinates,
        // so each quad becomes four vertices, keeping the corners' colours.
        static const int kFaces[6][4] = {
            {0, 1, 2, 3},  // front
            {0, 3, 7, 4},  // right
            {2, 6, 7, 3},  // bottom
            {1, 5, 6, 2},  // left
            {4, 7, 6, 5},  // back
            {5, 1, 0, 4},  // top
        };
        // Image row 0 is the top of the stud, and GL puts it at t = 0.
        static const float kUv[4][2] = {{1, 0}, {0, 0}, {0, 1}, {1, 1}};
        float vertices[24 * 3];
        float colors[24 * 3];
        float uvs[24 * 2];
        GLushort indices[36];
        for (int f = 0; f < 6; ++f) {
            for (int c = 0; c < 4; ++c) {
                const int v = f * 4 + c;
                for (int k = 0; k < 3; ++k) {
                    vertices[v * 3 + k] = kCorners[kFaces[f][c] * 3 + k];
                    colors[v * 3 + k] = kColors[kFaces[f][c] * 3 + k];
                }
                uvs[v * 2] = kUv[c][0];
                uvs[v * 2 + 1] = kUv[c][1];
            }
            static const int kQuad[6] = {0, 1, 2, 2, 3, 0};
            for (int i = 0; i < 6; ++i) indices[f * 6 + i] = static_cast<GLushort>(f * 4 + kQuad[i]);
        }

        gl.GenBuffers(1, &vbo_);
        gl.BindBuffer(GL_ARRAY_BUFFER, vbo_);
        gl.BufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);

        gl.GenBuffers(1, &colors_);
        gl.BindBuffer(GL_ARRAY_BUFFER, colors_);
        gl.BufferData(GL_ARRAY_BUFFER, sizeof colors, colors, GL_STATIC_DRAW);

        gl.GenBuffers(1, &uvs_);
        gl.BindBuffer(GL_ARRAY_BUFFER, uvs_);
        gl.BufferData(GL_ARRAY_BUFFER, sizeof uvs, uvs, GL_STATIC_DRAW);

        gl.GenBuffers(1, &ebo_);
        gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
        gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);

        textured_ = face != nullptr && face_size > 0 && gl.GenTextures != nullptr &&
                    gl.BindTexture != nullptr && gl.TexImage2D != nullptr &&
                    gl.TexParameteri != nullptr && gl.Uniform1i != nullptr;
        if (textured_) {
            gl.GenTextures(1, &texture_);
            gl.BindTexture(GL_TEXTURE_2D, texture_);
            gl.TexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, face_size, face_size, 0, GL_LUMINANCE,
                          GL_UNSIGNED_BYTE, face);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl.UseProgram(program_);
            gl.Uniform1i(gl.GetUniformLocation(program_, "face"), 0);
            gl.Uniform1i(uniform_textured_, 1);
        }

        gl.Enable(GL_DEPTH_TEST);
        return true;
    }

    // seconds drives the spin; one turn every four seconds, as in the
    // original. `distance` is how far back the camera sits, the
    // original's 3 by default; closer fills more of the drawable. A
    // transparent clear leaves only the cube, for drawing over a window.
    void draw(const CubeGl& gl, float seconds, int width, int height, float distance = 3.0f,
              bool transparent = false) {
        const float pi = 3.141593f;
        gl.Viewport(0, 0, width, height);
        if (transparent) {
            gl.ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        } else {
            gl.ClearColor(0.1f, 0.12f, 0.2f, 1.0f);
        }
        gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        gl.UseProgram(program_);

        const float aspect =
            height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
        Mat4 transform = mat4_identity();
        transform = mat4_multiply(transform, mat4_perspective(aspect));
        transform = mat4_multiply(transform, mat4_translation(0, 0, -distance));
        transform = mat4_multiply(transform, mat4_rotate_x(0.15f * pi));
        transform = mat4_multiply(transform, mat4_rotate_y(2 * pi * (seconds / 4.0f)));
        gl.UniformMatrix4fv(uniform_transform_, 1, GL_FALSE, transform.m);

        gl.BindBuffer(GL_ARRAY_BUFFER, vbo_);
        gl.VertexAttribPointer(static_cast<GLuint>(attr_pos_), 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        gl.EnableVertexAttribArray(static_cast<GLuint>(attr_pos_));
        gl.BindBuffer(GL_ARRAY_BUFFER, colors_);
        gl.VertexAttribPointer(static_cast<GLuint>(attr_color_), 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        gl.EnableVertexAttribArray(static_cast<GLuint>(attr_color_));
        if (attr_uv_ >= 0) {
            gl.BindBuffer(GL_ARRAY_BUFFER, uvs_);
            gl.VertexAttribPointer(static_cast<GLuint>(attr_uv_), 2, GL_FLOAT, GL_FALSE, 0,
                                   nullptr);
            gl.EnableVertexAttribArray(static_cast<GLuint>(attr_uv_));
        }
        if (textured_) gl.BindTexture(GL_TEXTURE_2D, texture_);
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
    GLuint uvs_ = 0;
    GLuint ebo_ = 0;
    GLuint texture_ = 0;
    bool textured_ = false;
    GLint attr_pos_ = 0;
    GLint attr_color_ = 0;
    GLint attr_uv_ = -1;
    GLint uniform_transform_ = -1;
    GLint uniform_textured_ = -1;
};

}  // namespace stud::tools
