// Real libGLESv2.so, bionic-compiled, placed at /system/lib64/
// libGLESv2.so -- see egl_stub.cpp's own doc comment for the full real
// mechanism (identical: real symbol names/signatures, forwarding over
// the real Unix-socket protocol proven end-to-end this session).

#include "render_client_common.h"

#include <GLES2/gl2.h>
#include <GLES3/gl3.h>

#include <cstdio>
#include <cstdlib>

#include <cstring>
#include <string>
#include <map>
#include <vector>

using stud::render_host::CallId;
using stud::render_client::connection;

namespace {
uint64_t pack_float(GLfloat f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}
// Every caller of this ignores the return value, so the request is pipelined
// rather than round-tripped. This is where nearly all of Stud's per-frame IPC
// cost lived: ~1840 blocking round-trips per frame, 16-100ms of pure wait
// (measure it again any time with STUD_IPC_STATS=1). Ordering is unchanged --
// one stream socket, and anything that needs an answer flushes the queue
// first.
void call0(CallId id, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0,
           uint64_t a4 = 0, uint64_t a5 = 0, uint64_t a6 = 0, uint64_t a7 = 0) {
    uint64_t a[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
    connection().call_void(id, a);
}
uint64_t call1(CallId id, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0,
               uint64_t a4 = 0, uint64_t a5 = 0, uint64_t a6 = 0, uint64_t a7 = 0) {
    uint64_t a[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
    return connection().call(id, a, nullptr, 0, nullptr, 0, nullptr);
}
// Real GL_PIXEL_UNPACK_BUFFER/GL_PIXEL_PACK_BUFFER tracking. When one is
// bound, every real texture upload/download's `pixels` argument is a byte
// OFFSET into that buffer, never a client-side pointer -- dereferencing it
// is a hard error (live-caught: a real 1MB compressed-texture upload read
// from a raw offset value and killed the render connection with EFAULT).
GLuint g_pixel_unpack_buffer = 0;
GLuint g_pixel_pack_buffer = 0;

// Real question this answers (the engineering notes, cursor investigation): the
// cursor is drawn last every frame with a correct texture and healthy draw
// state, yet never appears -- and the one documented hole in this forwarding
// layer is that glVertexAttribPointer's `p` is treated as a VBO-relative
// offset, which is garbage if the caller meant a genuine client-side array.
// Counting is enough to decide whether that hole is actually being hit;
// reported once at exit under STUD_TRACE_CLIENT_ARRAYS=1.
GLuint g_bound_element_buffer = 0;
GLuint g_bound_vertex_array = 0;
std::map<GLuint, GLuint>& element_buffer_by_vao() {
    static std::map<GLuint, GLuint> m;
    return m;
}
std::map<GLenum, GLuint>& bound_buffer_by_target() {
    static std::map<GLenum, GLuint> m;
    return m;
}

GLuint g_array_buffer = 0;
GLuint g_element_array_buffer = 0;
unsigned long g_client_side_attrib_pointers = 0;
unsigned long g_client_side_element_draws = 0;

bool client_array_trace_enabled() {
    static const bool on = std::getenv("STUD_TRACE_CLIENT_ARRAYS") != nullptr;
    return on;
}

void report_client_array_use(const char* what, unsigned long& counter) {
    ++counter;
    if (!client_array_trace_enabled()) return;
    if (counter == 1 || counter % 500 == 0) {
        std::fprintf(stderr, "stud: CLIENTARRAY %s used %lu time(s)\n", what, counter);
        std::fflush(stderr);
    }
}

// Encodes `pixels` for the wire: 0 means "real client pointer, data rides in
// the in-buffer", anything else is the real PBO offset plus one.
uint64_t pbo_tag(const void* pixels) {
    if (g_pixel_unpack_buffer == 0) return 0;
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pixels)) + 1u;
}

// Real GLES3 bytes-per-texel. The old version assumed 4 components for
// anything it did not recognise, which silently over-read the caller's
// buffer by 4x for a real GL_RED upload -- live-caught as a fatal
// EFAULT on the render socket (the engine uploads 128x2048 GL_RED
// glyph/mask atlases during real UI bring-up), which killed the render
// connection permanently and froze the window mid-frame.
uint32_t gl_pixel_size(GLenum format, GLenum type) {
    uint32_t components = 4;
    switch (format) {
        case GL_RED:
        case GL_RED_INTEGER:
        case GL_ALPHA:
        case GL_LUMINANCE:
        case GL_DEPTH_COMPONENT:
        case GL_STENCIL_INDEX8:
            components = 1;
            break;
        case GL_RG:
        case GL_RG_INTEGER:
        case GL_LUMINANCE_ALPHA:
        case GL_DEPTH_STENCIL:
            components = 2;
            break;
        case GL_RGB:
        case GL_RGB_INTEGER:
            components = 3;
            break;
        case GL_RGBA:
        case GL_RGBA_INTEGER:
        default:
            components = 4;
            break;
    }
    switch (type) {
        // Packed types carry the whole texel in one unit.
        case GL_UNSIGNED_SHORT_5_6_5:
        case GL_UNSIGNED_SHORT_4_4_4_4:
        case GL_UNSIGNED_SHORT_5_5_5_1:
            return 2;
        case GL_UNSIGNED_INT_2_10_10_10_REV:
        case GL_UNSIGNED_INT_10F_11F_11F_REV:
        case GL_UNSIGNED_INT_5_9_9_9_REV:
        case GL_UNSIGNED_INT_24_8:
            return 4;
        case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
            return 8;
        // Unpacked types: one unit per component.
        case GL_BYTE:
        case GL_UNSIGNED_BYTE:
            return components;
        case GL_SHORT:
        case GL_UNSIGNED_SHORT:
        case GL_HALF_FLOAT:
            return components * 2u;
        case GL_INT:
        case GL_UNSIGNED_INT:
        case GL_FLOAT:
            return components * 4u;
        default:
            return components;
    }
}
}  // namespace

extern "C" {

void glActiveTexture(GLenum t) { call0(CallId::GlActiveTexture, t); }
void glAttachShader(GLuint p, GLuint s) { call0(CallId::GlAttachShader, p, s); }
void glBindBuffer(GLenum t, GLuint b) {
    if (t == GL_PIXEL_UNPACK_BUFFER) g_pixel_unpack_buffer = b;
    if (t == GL_PIXEL_PACK_BUFFER) g_pixel_pack_buffer = b;
    if (t == GL_ARRAY_BUFFER) g_array_buffer = b;
    if (t == GL_ELEMENT_ARRAY_BUFFER) {
        g_element_array_buffer = b;
        // Really is VAO state: binding an element buffer records it on the
        // currently bound vertex array.
        g_bound_element_buffer = b;
        element_buffer_by_vao()[g_bound_vertex_array] = b;
    } else {
        bound_buffer_by_target()[t] = b;
    }
    call0(CallId::GlBindBuffer, t, b);
}
void glBindFramebuffer(GLenum t, GLuint f) { call0(CallId::GlBindFramebuffer, t, f); }
void glBindRenderbuffer(GLenum t, GLuint r) { call0(CallId::GlBindRenderbuffer, t, r); }
void glBindTexture(GLenum t, GLuint x) { call0(CallId::GlBindTexture, t, x); }
void glBlendFunc(GLenum s, GLenum d) { call0(CallId::GlBlendFunc, s, d); }
void glBlendFuncSeparate(GLenum sc, GLenum dc, GLenum sa, GLenum da) { call0(CallId::GlBlendFuncSeparate, sc, dc, sa, da); }
GLenum glCheckFramebufferStatus(GLenum t) { return static_cast<GLenum>(call1(CallId::GlCheckFramebufferStatus, t)); }
void glClear(GLbitfield m) { call0(CallId::GlClear, m); }
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { call0(CallId::GlClearColor, pack_float(r), pack_float(g), pack_float(b), pack_float(a)); }
void glClearDepthf(GLfloat d) { call0(CallId::GlClearDepthf, pack_float(d)); }
void glClearStencil(GLint s) { call0(CallId::GlClearStencil, static_cast<uint64_t>(s)); }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { call0(CallId::GlColorMask, r, g, b, a); }
void glCompileShader(GLuint s) { call0(CallId::GlCompileShader, s); }
void glCopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) {
    (void)w; (void)h;
    call0(CallId::GlCopyTexSubImage2D, t, static_cast<uint64_t>(l), static_cast<uint64_t>(xo), static_cast<uint64_t>(yo), static_cast<uint64_t>(x), static_cast<uint64_t>(y));
}
GLuint glCreateProgram() { return static_cast<GLuint>(call1(CallId::GlCreateProgram)); }
GLuint glCreateShader(GLenum t) { return static_cast<GLuint>(call1(CallId::GlCreateShader, t)); }
void glCullFace(GLenum m) { call0(CallId::GlCullFace, m); }
void glDeleteProgram(GLuint p) { call0(CallId::GlDeleteProgram, p); }
void glDeleteShader(GLuint s) { call0(CallId::GlDeleteShader, s); }
void glDepthFunc(GLenum f) { call0(CallId::GlDepthFunc, f); }
void glDepthMask(GLboolean m) { call0(CallId::GlDepthMask, m); }
void glDisable(GLenum c) { call0(CallId::GlDisable, c); }
void glDisableVertexAttribArray(GLuint i) { call0(CallId::GlDisableVertexAttribArray, i); }
void glDrawArrays(GLenum m, GLint f, GLsizei c) { call0(CallId::GlDrawArrays, m, static_cast<uint64_t>(f), static_cast<uint64_t>(c)); }
void glDrawElements(GLenum m, GLsizei c, GLenum t, const void* i) {
    if (g_element_array_buffer == 0) {
        report_client_array_use("glDrawElements with no GL_ELEMENT_ARRAY_BUFFER bound",
                                g_client_side_element_draws);
    }
    call0(CallId::GlDrawElements, m, static_cast<uint64_t>(c), t, reinterpret_cast<uint64_t>(i));
}
void glDrawArraysInstanced(GLenum m, GLint f, GLsizei c, GLsizei n) {
    call0(CallId::GlDrawArraysInstanced, m, static_cast<uint64_t>(f), static_cast<uint64_t>(c),
          static_cast<uint64_t>(n));
}
void glDrawElementsInstanced(GLenum m, GLsizei c, GLenum t, const void* i, GLsizei n) {
    if (g_element_array_buffer == 0) {
        report_client_array_use("glDrawElementsInstanced with no GL_ELEMENT_ARRAY_BUFFER bound",
                                g_client_side_element_draws);
    }
    call0(CallId::GlDrawElementsInstanced, m, static_cast<uint64_t>(c), t,
          reinterpret_cast<uint64_t>(i), static_cast<uint64_t>(n));
}
void glVertexAttribDivisor(GLuint index, GLuint divisor) {
    call0(CallId::GlVertexAttribDivisor, index, divisor);
}
void glEnable(GLenum c) { call0(CallId::GlEnable, c); }
void glEnableVertexAttribArray(GLuint i) { call0(CallId::GlEnableVertexAttribArray, i); }
void glFramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint r) { call0(CallId::GlFramebufferRenderbuffer, t, a, rt, r); }
void glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint x, GLint l) { call0(CallId::GlFramebufferTexture2D, t, a, tt, x, static_cast<uint64_t>(l)); }
void glGenerateMipmap(GLenum t) { call0(CallId::GlGenerateMipmap, t); }
// glGetError is by far the most expensive call Stud forwards: libroblox checks
// it after essentially every GL operation, measured at 856 blocking round-trips
// in a single frame -- more than every other call combined, and the dominant
// term in Stud's frame time.
//
// It cannot be made reply-free (the caller uses the value), so it is serviced
// from a cache that the swap path refreshes with one real query per frame. That
// is the same trade every remoting GL implementation makes: errors are reported
// a frame late rather than costing a synchronous round-trip each time. Nothing
// is swallowed -- a real error still surfaces, just on the next check -- and
// STUD_SYNC_GL_ERRORS=1 restores strict per-call querying for debugging.
GLenum g_cached_gl_error = GL_NO_ERROR;

bool sync_gl_errors() {
    static const bool on = std::getenv("STUD_SYNC_GL_ERRORS") != nullptr;
    return on;
}

GLenum glGetError() {
    if (sync_gl_errors()) return static_cast<GLenum>(call1(CallId::GlGetError));
    // Answered entirely from the cache, which the swap path refreshes with
    // ONE real query per frame (refresh_gl_error_cache, below).
    //
    // This used to poll every 64 checks, which sounds cheap and is not: the
    // engine makes about 14,500 GL calls in a frame on this path and checks
    // the error after nearly all of them, so one poll per 64 still came to
    // 226 blocking round-trips a frame -- measured at 25ms of pure waiting,
    // against 2ms for the other 14,300 calls put together. It WAS the frame
    // rate of the OpenGL path.
    //
    // Nothing is swallowed: a real error still surfaces, on the next frame
    // instead of within 64 calls, and STUD_SYNC_GL_ERRORS=1 restores strict
    // per-call querying when that difference matters.
    GLenum e = g_cached_gl_error;
    g_cached_gl_error = GL_NO_ERROR;
    return e;
}

// Not a GL entry point -- Stud's own, called by the swap path. Inside
// the extern "C" block so it keeps a plain, unmangled name, the same way
// the rest of this file's symbols are resolved.
void stud_refresh_gl_error_cache() {
    if (sync_gl_errors()) return;
    // Only when nothing is already pending, so a cached error is not lost
    // before the caller has read it.
    if (g_cached_gl_error == GL_NO_ERROR) {
        g_cached_gl_error = static_cast<GLenum>(call1(CallId::GlGetError));
    }
}


void glLinkProgram(GLuint p) { call0(CallId::GlLinkProgram, p); }
void glPixelStorei(GLenum p, GLint v) { call0(CallId::GlPixelStorei, p, static_cast<uint64_t>(v)); }
void glPolygonOffset(GLfloat f, GLfloat u) { call0(CallId::GlPolygonOffset, pack_float(f), pack_float(u)); }
void glReleaseShaderCompiler() { call0(CallId::GlReleaseShaderCompiler); }
void glRenderbufferStorage(GLenum t, GLenum i, GLsizei w, GLsizei h) { call0(CallId::GlRenderbufferStorage, t, i, static_cast<uint64_t>(w), static_cast<uint64_t>(h)); }
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { call0(CallId::GlScissor, static_cast<uint64_t>(x), static_cast<uint64_t>(y), static_cast<uint64_t>(w), static_cast<uint64_t>(h)); }
void glStencilFunc(GLenum f, GLint r, GLuint m) { call0(CallId::GlStencilFunc, f, static_cast<uint64_t>(r), m); }
void glStencilMask(GLuint m) { call0(CallId::GlStencilMask, m); }
void glStencilOp(GLenum f, GLenum zf, GLenum zp) { call0(CallId::GlStencilOp, f, zf, zp); }
void glTexParameterf(GLenum t, GLenum p, GLfloat v) { call0(CallId::GlTexParameterf, t, p, pack_float(v)); }
void glTexParameteri(GLenum t, GLenum p, GLint v) { call0(CallId::GlTexParameteri, t, p, static_cast<uint64_t>(v)); }
void glUniform1i(GLint l, GLint v) { call0(CallId::GlUniform1i, static_cast<uint64_t>(l), static_cast<uint64_t>(v)); }
void glUseProgram(GLuint p) { call0(CallId::GlUseProgram, p); }
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { call0(CallId::GlViewport, static_cast<uint64_t>(x), static_cast<uint64_t>(y), static_cast<uint64_t>(w), static_cast<uint64_t>(h)); }
void glVertexAttribIPointer(GLuint i, GLint s, GLenum t, GLsizei st, const void* p) {
    // Same VBO-offset reasoning as glVertexAttribPointer just below.
    if (g_array_buffer == 0 && p != nullptr) {
        report_client_array_use("glVertexAttribIPointer with no GL_ARRAY_BUFFER bound",
                                g_client_side_attrib_pointers);
    }
    call0(CallId::GlVertexAttribIPointer, i, static_cast<uint64_t>(s), t,
          static_cast<uint64_t>(st), reinterpret_cast<uint64_t>(p));
}
void glVertexAttribPointer(GLuint i, GLint s, GLenum t, GLboolean n, GLsizei st, const void* p) {
    // `p` forwarded as a raw 64-bit value -- correct when it's a VBO-
    // relative byte offset (the common, modern-GLES real case, a
    // GL_ARRAY_BUFFER bound via glBindBuffer beforehand); see
    // render-host/src/main.cpp's own matching doc comment for the
    // documented, not-yet-supported genuine-client-side-pointer case.
    if (g_array_buffer == 0 && p != nullptr) {
        report_client_array_use("glVertexAttribPointer with no GL_ARRAY_BUFFER bound",
                                g_client_side_attrib_pointers);
    }
    call0(CallId::GlVertexAttribPointer, i, static_cast<uint64_t>(s), t, n, static_cast<uint64_t>(st), reinterpret_cast<uint64_t>(p));
}

const GLubyte* glGetString(GLenum name) {
    // Big enough for the whole GL_EXTENSIONS string.
    //
    // This was 512 bytes, and ANGLE's extension list is several kilobytes
    // -- so the engine received the first 511 characters of it and nothing
    // else. Alphabetically that is the GL_AMD_* and GL_ANGLE_* entries and
    // stops there, which cut off every GL_EXT_texture_compression_*
    // (s3tc, dxt1, rgtc, bptc) the driver really does support.
    //
    // The engine believed the GPU had no block compression at all (its own
    // capability line read `Caps: Texture: DXT 0`), so textures with alpha
    // were stored uncompressed against its 64MB video-memory budget and
    // its streamer kept them at a low mip -- transparent textures staying
    // blurry while opaque ones, which still had ETC2, were sharp.
    //
    // Truncation is still possible in principle, so it is reported rather
    // than left to be discovered the same way twice.
    // ... and yet the FULL list is not what gets reported, by default.
    //
    // Handing the engine everything ANGLE exposes changes which shader
    // permutations it asks its own pack for, and the pack shipped in the
    // APK is an Android GLES one that does not contain them -- measured
    // as `Error: shader DefaultUnifiedFlatOpaqueVS80000006 is not
    // available` for every shader, and a black window. It also made the
    // engine start calling entry points it had never used (glBufferStorage
    // sits past the old cutoff), which is real and is now implemented,
    // but the shader pack is not something Stud can supply.
    //
    // So the reported list is capped at the length it has always had.
    // That is not a fix, it is the status quo held deliberately: the
    // engine gets exactly the capabilities it got before, and nothing it
    // has no shaders for. STUD_GL_FULL_EXTENSIONS=1 reports the whole
    // string for anyone investigating what the extra capabilities would
    // buy -- expect a black window until the shader-pack question is
    // answered.
    static thread_local char buf[16384];
    static const bool full = std::getenv("STUD_GL_FULL_EXTENSIONS") != nullptr;
    uint64_t a[8] = {name};
    uint32_t written = 0;
    connection().call(CallId::GlGetString, a, nullptr, 0, buf,
                      static_cast<uint32_t>(sizeof(buf) - 1), &written);
    if (written >= sizeof(buf) - 1) written = sizeof(buf) - 1;
    buf[written] = '\0';
    if (full) return reinterpret_cast<const GLubyte*>(buf);

    // Capped at the length it has always had.
    //
    // Reporting more changes which shader permutations the engine asks
    // its own APK-shipped pack for, and that pack does not contain them:
    // a black window, live-confirmed twice -- once with the whole list,
    // and again with just two extensions appended
    // (GL_EXT_disjoint_timer_query, GL_EXT_buffer_storage). Two names
    // were enough to move the permutation mask, so this is not about
    // volume and there is no safe subset to sneak through.
    //
    // The cost is real and worth stating: the engine cannot use what it
    // cannot see, so its GPU timing reads 0.00ms even though Stud now
    // implements the timer queries underneath. Making that visible needs
    // the shader-pack question answered first.
    if (written > 511) {
        buf[511] = '\0';
        // Never leave a half-written name at the end.
        if (char* space = std::strrchr(buf, ' ')) *space = '\0';
    }
    return reinterpret_cast<const GLubyte*>(buf);
}
GLint glGetUniformLocation(GLuint program, const GLchar* name) {
    uint64_t a[8] = {program};
    uint32_t len = name != nullptr ? static_cast<uint32_t>(std::strlen(name)) : 0;
    return static_cast<GLint>(connection().call(CallId::GlGetUniformLocation, a, name, len, nullptr, 0, nullptr));
}
void glBindAttribLocation(GLuint program, GLuint index, const GLchar* name) {
    uint64_t a[8] = {program, index};
    uint32_t len = name != nullptr ? static_cast<uint32_t>(std::strlen(name)) : 0;
    connection().call(CallId::GlBindAttribLocation, a, name, len, nullptr, 0, nullptr);
}
void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* length) {
    // Real, documented scope: concatenates into one buffer and sends as
    // a single logical source string (count treated as 1 server-side --
    // see render-host/src/main.cpp's own matching doc comment). Correct
    // for the real, single-string call shape confirmed used elsewhere in
    // this project's own shader-compile probes; a genuine multi-string
    // `count > 1` call from Roblox would still compile correctly here
    // (concatenation preserves the token stream GLSL's own preprocessor
    // sees), just isn't length-array-validated per-fragment.
    std::string src;
    for (GLsizei i = 0; i < count; ++i) {
        if (strings[i] == nullptr) continue;
        size_t len = (length != nullptr && length[i] >= 0) ? static_cast<size_t>(length[i])
                                                             : std::strlen(strings[i]);
        src.append(strings[i], len);
    }
    uint64_t a[8] = {shader};
    connection().call(CallId::GlShaderSource, a, src.data(), static_cast<uint32_t>(src.size()), nullptr, 0, nullptr);
}
void glGetProgramInfoLog(GLuint program, GLsizei bufsize, GLsizei* length, GLchar* infolog) {
    uint64_t a[8] = {program};
    uint32_t written = 0;
    connection().call(CallId::GlGetProgramInfoLog, a, nullptr, 0, infolog,
                       bufsize > 0 ? static_cast<uint32_t>(bufsize) : 0, &written);
    if (length != nullptr) *length = static_cast<GLsizei>(written);
}
void glGetShaderInfoLog(GLuint shader, GLsizei bufsize, GLsizei* length, GLchar* infolog) {
    uint64_t a[8] = {shader};
    uint32_t written = 0;
    connection().call(CallId::GlGetShaderInfoLog, a, nullptr, 0, infolog,
                       bufsize > 0 ? static_cast<uint32_t>(bufsize) : 0, &written);
    if (length != nullptr) *length = static_cast<GLsizei>(written);
}
void glGetActiveUniform(GLuint program, GLuint index, GLsizei bufsize, GLsizei* length, GLint* size,
                         GLenum* type, GLchar* name) {
    uint64_t a[8] = {program, index};
    std::vector<uint8_t> out(sizeof(GLint) + sizeof(GLenum) + (bufsize > 0 ? static_cast<size_t>(bufsize) : 0));
    uint32_t written = 0;
    connection().call(CallId::GlGetActiveUniform, a, nullptr, 0, out.data(),
                       static_cast<uint32_t>(out.size()), &written);
    if (written < sizeof(GLint) + sizeof(GLenum)) return;
    GLint real_size;
    GLenum real_type;
    std::memcpy(&real_size, out.data(), sizeof(GLint));
    std::memcpy(&real_type, out.data() + sizeof(GLint), sizeof(GLenum));
    if (size != nullptr) *size = real_size;
    if (type != nullptr) *type = real_type;
    uint32_t name_len = written - static_cast<uint32_t>(sizeof(GLint) + sizeof(GLenum));
    if (name != nullptr && bufsize > 0) {
        uint32_t to_copy = name_len < static_cast<uint32_t>(bufsize) - 1 ? name_len : static_cast<uint32_t>(bufsize) - 1;
        std::memcpy(name, out.data() + sizeof(GLint) + sizeof(GLenum), to_copy);
        name[to_copy] = '\0';
        if (length != nullptr) *length = static_cast<GLsizei>(to_copy);
    }
}

void glDeleteBuffers(GLsizei n, const GLuint* b) {
    uint64_t a[8] = {static_cast<uint64_t>(n)};
    connection().call(CallId::GlDeleteBuffers, a, b, static_cast<uint32_t>(n) * sizeof(GLuint), nullptr, 0, nullptr);
}
void glDeleteFramebuffers(GLsizei n, const GLuint* b) {
    uint64_t a[8] = {static_cast<uint64_t>(n)};
    connection().call(CallId::GlDeleteFramebuffers, a, b, static_cast<uint32_t>(n) * sizeof(GLuint), nullptr, 0, nullptr);
}
void glDeleteRenderbuffers(GLsizei n, const GLuint* b) {
    uint64_t a[8] = {static_cast<uint64_t>(n)};
    connection().call(CallId::GlDeleteRenderbuffers, a, b, static_cast<uint32_t>(n) * sizeof(GLuint), nullptr, 0, nullptr);
}
void glDeleteTextures(GLsizei n, const GLuint* b) {
    uint64_t a[8] = {static_cast<uint64_t>(n)};
    connection().call(CallId::GlDeleteTextures, a, b, static_cast<uint32_t>(n) * sizeof(GLuint), nullptr, 0, nullptr);
}
void glGenBuffers(GLsizei n, GLuint* out) {
    uint64_t a[8] = {static_cast<uint64_t>(n)};
    connection().call(CallId::GlGenBuffers, a, nullptr, 0, out, static_cast<uint32_t>(n) * sizeof(GLuint), nullptr);
}

// Real VAO entry points. libroblox resolves these through
// eglGetProcAddress() into its own dispatch table up front; before
// egl_stub.cpp's eglGetProcAddress() was implemented they came back
// NULL and the first glGenVertexArrays(1, &vao) call jumped to address
// zero, killing the engine's designated internal "main" thread (see
// the engineering notes). Same forwarding shape as the glGen*/glBind*/
// glDelete* families above.
void glGenVertexArrays(GLsizei n, GLuint* out) {
    uint64_t a[8] = {static_cast<uint64_t>(n)};
    connection().call(CallId::GlGenVertexArrays, a, nullptr, 0, out, static_cast<uint32_t>(n) * sizeof(GLuint), nullptr);
}
void glBindVertexArray(GLuint array) {
    // Swap in that VAO's own element-buffer binding, mirroring real GL.
    g_bound_vertex_array = array;
    auto it = element_buffer_by_vao().find(array);
    g_bound_element_buffer = it == element_buffer_by_vao().end() ? 0 : it->second;
    g_element_array_buffer = g_bound_element_buffer;
    call0(CallId::GlBindVertexArray, array);
}

// Real GLES3 render-setup entry points the engine reaches once the full
// app-bring-up sequence succeeds (each was previously a named no-op and
// reported itself as called -- see the engineering notes). Uniform-buffer
// binding, per-buffer clears and MRT draw-buffer selection are all
// load-bearing for real rendering, so no-oping them cannot produce a
// correct frame.
void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset,
                        GLsizeiptr size) {
    uint64_t a[8] = {target, index, buffer, static_cast<uint64_t>(offset),
                      static_cast<uint64_t>(size)};
    connection().call_void(CallId::GlBindBufferRange, a);
}
void glBindBufferBase(GLenum target, GLuint index, GLuint buffer) {
    call0(CallId::GlBindBufferBase, target, index, buffer);
}
void glClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value) {
    uint64_t a[8] = {buffer, static_cast<uint64_t>(drawbuffer)};
    // Always four floats -- the widest real case (a colour buffer);
    // depth/stencil clears read only the first, so sending four is
    // safe and keeps the wire format fixed.
    GLfloat local[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (value != nullptr) {
        for (int i = 0; i < 4; ++i) local[i] = value[i];
    }
    connection().call(CallId::GlClearBufferfv, a, local, sizeof(local), nullptr, 0, nullptr);
}
void glDrawBuffers(GLsizei n, const GLenum* bufs) {
    uint64_t a[8] = {static_cast<uint64_t>(n)};
    connection().call(CallId::GlDrawBuffers, a, bufs,
                       static_cast<uint32_t>(n) * sizeof(GLenum), nullptr, 0, nullptr);
}

// The engine's main colour buffer is a multisampled renderbuffer. While
// this resolved to a no-op the renderbuffer never got storage, so every
// framebuffer it was attached to reported
// GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT and the engine abandoned its main
// render target -- only on the OpenGL path, which is why it survived
// unnoticed for as long as everything ran on Vulkan.
void glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat,
                                       GLsizei width, GLsizei height) {
    call0(CallId::GlRenderbufferStorageMultisample, target, static_cast<uint64_t>(samples),
          internalformat, static_cast<uint64_t>(width), static_cast<uint64_t>(height));
}
void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0,
                       GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
    // The header holds eight arguments and this call has ten, so the last
    // two travel in the buffer -- same arrangement as glTexSubImage3D.
    uint64_t a[8] = {static_cast<uint64_t>(static_cast<int64_t>(srcX0)),
                     static_cast<uint64_t>(static_cast<int64_t>(srcY0)),
                     static_cast<uint64_t>(static_cast<int64_t>(srcX1)),
                     static_cast<uint64_t>(static_cast<int64_t>(srcY1)),
                     static_cast<uint64_t>(static_cast<int64_t>(dstX0)),
                     static_cast<uint64_t>(static_cast<int64_t>(dstY0)),
                     static_cast<uint64_t>(static_cast<int64_t>(dstX1)),
                     static_cast<uint64_t>(static_cast<int64_t>(dstY1))};
    const uint32_t tail[2] = {static_cast<uint32_t>(mask), static_cast<uint32_t>(filter)};
    connection().call(CallId::GlBlitFramebuffer, a, tail, sizeof(tail), nullptr, 0, nullptr);
}
void glInvalidateFramebuffer(GLenum target, GLsizei numAttachments, const GLenum* attachments) {
    uint64_t a[8] = {target, static_cast<uint64_t>(numAttachments)};
    connection().call(CallId::GlInvalidateFramebuffer, a, attachments,
                       static_cast<uint32_t>(numAttachments) * sizeof(GLenum), nullptr, 0, nullptr);
}

// Real GLES3 sync objects. A GLsync is an opaque handle the caller
// never dereferences, so the host's own real GLsync pointer value is
// forwarded straight back as the client's GLsync -- no client-side
// table needed, and it round-trips unchanged on every later call.
GLsync glFenceSync(GLenum condition, GLbitfield flags) {
    return reinterpret_cast<GLsync>(
        static_cast<uintptr_t>(call1(CallId::GlFenceSync, condition, flags)));
}
GLenum glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
    return static_cast<GLenum>(call1(CallId::GlClientWaitSync,
                                     static_cast<uint64_t>(reinterpret_cast<uintptr_t>(sync)), flags,
                                     timeout));
}
void glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
    call0(CallId::GlWaitSync, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(sync)), flags,
          timeout);
}
void glDeleteSync(GLsync sync) {
    call0(CallId::GlDeleteSync, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(sync)));
}
GLboolean glIsSync(GLsync sync) {
    return call1(CallId::GlIsSync, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(sync)))
               ? GL_TRUE
               : GL_FALSE;
}
void glGetSynciv(GLsync sync, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* values) {
    uint64_t a[8] = {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(sync)), pname,
                     static_cast<uint64_t>(bufSize)};
    uint32_t written = 0;
    connection().call(CallId::GlGetSynciv, a, nullptr, 0, values,
                      static_cast<uint32_t>(bufSize) * sizeof(GLint), &written);
    if (length != nullptr) *length = static_cast<GLsizei>(written / sizeof(GLint));
}

// glCopyImageSubData takes 15 real int arguments -- the wire header
// only carries 8, so all 15 ride in the in-buffer as a plain int32
// array instead (same technique glTexSubImage3D already uses for its
// two overflow arguments).
void glCopyImageSubData(GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY,
                        GLint srcZ, GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX,
                        GLint dstY, GLint dstZ, GLsizei srcWidth, GLsizei srcHeight,
                        GLsizei srcDepth) {
    const int32_t args[15] = {static_cast<int32_t>(srcName), static_cast<int32_t>(srcTarget),
                              srcLevel,                      srcX,
                              srcY,                          srcZ,
                              static_cast<int32_t>(dstName), static_cast<int32_t>(dstTarget),
                              dstLevel,                      dstX,
                              dstY,                          dstZ,
                              srcWidth,                      srcHeight,
                              srcDepth};
    uint64_t a[8] = {};
    connection().call(CallId::GlCopyImageSubData, a, args, sizeof(args), nullptr, 0, nullptr);
}

// Real GLES3 entry points libroblox actually calls during app bring-up
// (confirmed live -- each one was previously resolved to a no-op stub
// and reported by name; see the engineering notes).
void glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width,
                     GLsizei height) {
    uint64_t a[8] = {target, static_cast<uint64_t>(levels), internalformat,
                      static_cast<uint64_t>(width), static_cast<uint64_t>(height)};
    connection().call(CallId::GlTexStorage2D, a, nullptr, 0, nullptr, 0, nullptr);
}
void glTexStorage3D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width,
                     GLsizei height, GLsizei depth) {
    uint64_t a[8] = {target,
                      static_cast<uint64_t>(levels),
                      internalformat,
                      static_cast<uint64_t>(width),
                      static_cast<uint64_t>(height),
                      static_cast<uint64_t>(depth)};
    connection().call(CallId::GlTexStorage3D, a, nullptr, 0, nullptr, 0, nullptr);
}
void glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                      GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                      const void* pixels) {
    // 10 real int args, but the wire header only carries 8 -- so
    // `format` and `type` ride at the front of the in-buffer, ahead of
    // the real pixel data. The host unpacks them the same way.
    uint64_t a[8] = {target,
                      static_cast<uint64_t>(level),
                      static_cast<uint64_t>(xoffset),
                      static_cast<uint64_t>(yoffset),
                      static_cast<uint64_t>(zoffset),
                      static_cast<uint64_t>(width),
                      static_cast<uint64_t>(height),
                      static_cast<uint64_t>(depth)};
    uint64_t pbo = pbo_tag(pixels);
    size_t pixel_bytes = (pbo == 0 && pixels != nullptr)
                             ? static_cast<size_t>(width) * static_cast<size_t>(height) *
                                   static_cast<size_t>(depth) * gl_pixel_size(format, type)
                             : 0u;
    std::vector<unsigned char> payload(2 * sizeof(uint64_t) + pixel_bytes);
    auto* extra = reinterpret_cast<uint64_t*>(payload.data());
    extra[0] = format;
    extra[1] = type;
    if (pixel_bytes > 0) {
        std::memcpy(payload.data() + 2 * sizeof(uint64_t), pixels, pixel_bytes);
    }
    connection().call(CallId::GlTexSubImage3D, a, payload.data(),
                       static_cast<uint32_t>(payload.size()), nullptr, 0, nullptr, pbo);
}
void glProgramParameteri(GLuint program, GLenum pname, GLint value) {
    uint64_t a[8] = {program, pname, static_cast<uint64_t>(value)};
    connection().call(CallId::GlProgramParameteri, a, nullptr, 0, nullptr, 0, nullptr);
}
GLuint glGetUniformBlockIndex(GLuint program, const GLchar* uniformBlockName) {
    if (uniformBlockName == nullptr) return GL_INVALID_INDEX;
    uint64_t a[8] = {program};
    uint32_t len = static_cast<uint32_t>(std::strlen(uniformBlockName)) + 1u;
    return static_cast<GLuint>(
        connection().call(CallId::GlGetUniformBlockIndex, a, uniformBlockName, len, nullptr, 0,
                          nullptr));
}
void glUniformBlockBinding(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding) {
    uint64_t a[8] = {program, uniformBlockIndex, uniformBlockBinding};
    connection().call_void(CallId::GlUniformBlockBinding, a);
}
void glGetActiveUniformBlockiv(GLuint program, GLuint uniformBlockIndex, GLenum pname,
                                GLint* params) {
    if (params == nullptr) return;
    uint64_t a[8] = {program, uniformBlockIndex, pname};
    // The host sizes the real reply itself (one int for most pnames,
    // one per active uniform for ACTIVE_UNIFORM_INDICES), so give it a
    // generous capacity and copy back exactly what it wrote.
    std::vector<GLint> scratch(256, 0);
    uint32_t written = 0;
    connection().call(CallId::GlGetActiveUniformBlockiv, a, nullptr, 0, scratch.data(),
                       static_cast<uint32_t>(scratch.size() * sizeof(GLint)), &written);
    // Never write more than the pname itself defines, whatever the host
    // reports: params is sized by the caller from the pname, so trusting
    // a larger reply would run off the end of it (the same class of bug
    // as the glGetIntegerv stack smash above).
    size_t count = written / sizeof(GLint);
    size_t max_for_pname = 1;
    if (pname == GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES) {
        GLint active = 0;
        glGetActiveUniformBlockiv(program, uniformBlockIndex,
                                   GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &active);
        max_for_pname = active > 0 ? static_cast<size_t>(active) : 0;
    }
    if (count > max_for_pname) count = max_for_pname;
    for (size_t i = 0; i < count; ++i) params[i] = scratch[i];
}
void glDeleteVertexArrays(GLsizei n, const GLuint* arrays) {
    uint64_t a[8] = {static_cast<uint64_t>(n)};
    connection().call(CallId::GlDeleteVertexArrays, a, arrays, static_cast<uint32_t>(n) * sizeof(GLuint), nullptr, 0, nullptr);
}

// Real OES aliases -- same functions, the names GLES2-era code looks
// for first (libroblox probes both, confirmed live).
void glGenVertexArraysOES(GLsizei n, GLuint* out) { glGenVertexArrays(n, out); }
void glBindVertexArrayOES(GLuint array) { glBindVertexArray(array); }
void glDeleteVertexArraysOES(GLsizei n, const GLuint* arrays) { glDeleteVertexArrays(n, arrays); }
void glGenFramebuffers(GLsizei n, GLuint* out) {
    uint64_t a[8] = {static_cast<uint64_t>(n)};
    connection().call(CallId::GlGenFramebuffers, a, nullptr, 0, out, static_cast<uint32_t>(n) * sizeof(GLuint), nullptr);
}
void glGenRenderbuffers(GLsizei n, GLuint* out) {
    uint64_t a[8] = {static_cast<uint64_t>(n)};
    connection().call(CallId::GlGenRenderbuffers, a, nullptr, 0, out, static_cast<uint32_t>(n) * sizeof(GLuint), nullptr);
}
void glGenTextures(GLsizei n, GLuint* out) {
    uint64_t a[8] = {static_cast<uint64_t>(n)};
    connection().call(CallId::GlGenTextures, a, nullptr, 0, out, static_cast<uint32_t>(n) * sizeof(GLuint), nullptr);
}

// How many ints glGetIntegerv really writes for a given pname. The
// overwhelming majority of GLES pnames return exactly one, so 1 is the
// default and only the genuinely multi-valued ones are listed.
//
// This is memory safety, not a nicety: the caller's buffer is sized by
// the pname, so writing more than the pname's own count runs off the end
// of whatever it passed -- usually a single stack GLint.
int gl_integerv_count(GLenum pname) {
    switch (pname) {
        case GL_MAX_VIEWPORT_DIMS:
        case GL_ALIASED_POINT_SIZE_RANGE:
        case GL_ALIASED_LINE_WIDTH_RANGE:
        case GL_DEPTH_RANGE:
            return 2;
        case GL_VIEWPORT:
        case GL_SCISSOR_BOX:
        case GL_COLOR_WRITEMASK:
        case GL_COLOR_CLEAR_VALUE:
        case GL_BLEND_COLOR:
            return 4;
        default:
            return 1;
    }
}

void glGetIntegerv(GLenum pname, GLint* params) {
    if (params == nullptr) return;
    uint64_t a[8] = {pname};
    GLint values[16] = {};
    uint32_t written = 0;
    connection().call(CallId::GlGetIntegerv, a, nullptr, 0, values, sizeof(values), &written);
    // Read generously, write exactly the pname's own count.
    //
    // This used to write a fixed 4 ints for every pname. Live-caught as a
    // real stack smash: the engine joined a real game, hit the texture
    // path ("TextureDownloadResource ... falling back on synchronous
    // download"), and libc's own -fstack-protector fired --
    // `stack corruption detected` and an abort on the render thread. That
    // thread died, GL stopped, and the window froze while the process
    // stayed alive. A caller doing `GLint n; glGetIntegerv(
    // GL_MAX_TEXTURE_SIZE, &n);` -- one int, on the stack -- was getting
    // 16 bytes.
    int count = gl_integerv_count(pname);
    uint32_t have = written / static_cast<uint32_t>(sizeof(GLint));
    if (have > 0 && static_cast<uint32_t>(count) > have) count = static_cast<int>(have);
    std::memcpy(params, values, static_cast<size_t>(count) * sizeof(GLint));
}
void glTexParameterfv(GLenum target, GLenum pname, const GLfloat* params) {
    uint64_t a[8] = {target, pname, pack_float(params[0])};
    connection().call(CallId::GlTexParameterfv, a, nullptr, 0, nullptr, 0, nullptr);
}
void glGetProgramiv(GLuint program, GLenum pname, GLint* params) {
    uint64_t a[8] = {program, pname};
    connection().call(CallId::GlGetProgramiv, a, nullptr, 0, params, sizeof(GLint), nullptr);
}
void glGetShaderiv(GLuint shader, GLenum pname, GLint* params) {
    uint64_t a[8] = {shader, pname};
    connection().call(CallId::GlGetShaderiv, a, nullptr, 0, params, sizeof(GLint), nullptr);
}

void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    uint64_t a[8] = {target, static_cast<uint64_t>(size), 0, usage};
    connection().call_void(CallId::GlBufferData, a, data,
                           data != nullptr ? static_cast<uint32_t>(size) : 0);
}
// GL timer queries. The engine times the GPU with these; without them
// its own MicroProfiler reports GPU 0.00ms and its work scheduling has
// nothing to pace against.
//
// Both spellings: GLES3 has them in core, and GL_EXT_disjoint_timer_query
// exports the same entry points with an EXT suffix. The engine resolves
// whichever it believes in, so both are exported and do the same thing.
void glGenQueries(GLsizei n, GLuint* ids) {
    if (n <= 0 || ids == nullptr) return;
    uint64_t a[8] = {static_cast<uint64_t>(n)};
    uint32_t written = 0;
    connection().call(CallId::GlGenQueries, a, nullptr, 0, ids,
                      static_cast<uint32_t>(n) * sizeof(GLuint), &written);
}
void glDeleteQueries(GLsizei n, const GLuint* ids) {
    if (n <= 0 || ids == nullptr) return;
    uint64_t a[8] = {static_cast<uint64_t>(n)};
    connection().call_void(CallId::GlDeleteQueries, a, ids,
                           static_cast<uint32_t>(n) * sizeof(GLuint));
}
void glBeginQuery(GLenum target, GLuint id) { call0(CallId::GlBeginQuery, target, id); }
void glEndQuery(GLenum target) { call0(CallId::GlEndQuery, target); }
void glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params) {
    if (params == nullptr) return;
    uint64_t a[8] = {id, pname};
    uint32_t written = 0;
    connection().call(CallId::GlGetQueryObjectuiv, a, nullptr, 0, params, sizeof(GLuint),
                      &written);
    if (written < sizeof(GLuint)) *params = 0;
}
void glGetQueryObjectui64v(GLuint id, GLenum pname, GLuint64* params) {
    if (params == nullptr) return;
    uint64_t a[8] = {id, pname};
    uint32_t written = 0;
    connection().call(CallId::GlGetQueryObjectui64v, a, nullptr, 0, params, sizeof(GLuint64),
                      &written);
    if (written < sizeof(GLuint64)) *params = 0;
}
void glGenQueriesEXT(GLsizei n, GLuint* ids) { glGenQueries(n, ids); }
void glDeleteQueriesEXT(GLsizei n, const GLuint* ids) { glDeleteQueries(n, ids); }
void glBeginQueryEXT(GLenum target, GLuint id) { glBeginQuery(target, id); }
void glEndQueryEXT(GLenum target) { glEndQuery(target); }
void glGetQueryObjectuivEXT(GLuint id, GLenum pname, GLuint* params) {
    glGetQueryObjectuiv(id, pname, params);
}
void glGetQueryObjectui64vEXT(GLuint id, GLenum pname, GLuint64* params) {
    glGetQueryObjectui64v(id, pname, params);
}

void glBufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
    // Immutable storage: same shape as glBufferData, plus the flags that
    // say how it may be mapped later.
    if (target == GL_PIXEL_UNPACK_BUFFER || target == GL_PIXEL_PACK_BUFFER) {
        // Nothing special to track here -- the bound-buffer shadow that
        // the pixel paths read is maintained by glBindBuffer.
    }
    uint64_t a[8] = {target, static_cast<uint64_t>(size), flags};
    connection().call_void(CallId::GlBufferStorage, a, data,
                           data != nullptr ? static_cast<uint32_t>(size) : 0);
}
void glBufferStorageEXT(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
    glBufferStorage(target, size, data, flags);
}
void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    uint64_t a[8] = {target, static_cast<uint64_t>(offset), static_cast<uint64_t>(size)};
    connection().call_void(CallId::GlBufferSubData, a, data, static_cast<uint32_t>(size));
}

// Real buffer mapping over the render-host IPC boundary. A real
// glMapBufferRange() hands back a pointer into driver-owned memory,
// which cannot cross a process boundary -- so map into a client-side
// staging allocation and upload it on unmap via the existing, real
// glBufferSubData path. This is the standard way to emulate mapping
// across a transport, and it is what streaming vertex/index data
// (the overwhelmingly common real use, and the one libroblox hits
// during app bring-up) actually needs.
//
// The staging allocation is pre-filled with the buffer's real current
// contents (CallId::GlGetBufferSubData) whenever the caller has not told
// GL it may discard them -- i.e. for GL_MAP_READ_BIT, and for a
// GL_MAP_WRITE_BIT map carrying neither GL_MAP_INVALIDATE_RANGE_BIT nor
// GL_MAP_INVALIDATE_BUFFER_BIT, where the spec requires every byte the
// caller does not overwrite to survive. Zeroing instead was a real,
// live-caught bug: libroblox maps its index buffer exactly that way
// (GL_ELEMENT_ARRAY_BUFFER, access=GL_MAP_WRITE_BIT, 18432 bytes), so
// every index it did not rewrite came back 0 and the geometry collapsed
// into degenerate triangles. An invalidating map still skips the
// round-trip, which is the common streaming case and the hot path.
namespace {
struct BufferMapping {
    GLintptr offset = 0;
    GLsizeiptr length = 0;
    GLbitfield access = 0;
    std::vector<unsigned char> staging;
    bool active = false;
};

// Keyed by the real buffer OBJECT, not by the target it happens to be bound
// to. GL's rule is one mapping per buffer object; two different buffers bound
// to the same target can be mapped at the same time, and libroblox really
// does that -- it double-buffers its dynamic UI geometry (live-observed:
// GL_ARRAY_BUFFER alternating between names 9 and 10 across frames).
//
// Keying by target instead was a real, live-caught corruption bug: the second
// map reused and re-assign()ed the one slot, which both dangled the pointer
// the engine was still writing through and left the wrong staging buffer to
// be uploaded on unmap. The observable result was index data uploaded into
// the vertex buffer and vertex data into the index buffer (caught directly:
// an UNMAP of GL_ARRAY_BUFFER whose first values were the quad index pattern
// 0 1 2 0 3 1, and an UNMAP of GL_ELEMENT_ARRAY_BUFFER whose bytes decoded to
// float positions), which is why draws came out fully degenerate -- six
// indices all 0 pointing at a zeroed vertex.
//
// std::map keeps references stable across insertion, which a vector does not.
std::map<GLuint, BufferMapping>& mappings() {
    static std::map<GLuint, BufferMapping> m;
    return m;
}

// The binding is asked of the real GL, not tracked locally: GL_ELEMENT_ARRAY_
// BUFFER's binding is vertex-array-object state, so glBindVertexArray changes
// it without any glBindBuffer for a client-side tracker to see.
GLenum buffer_binding_query_for(GLenum target) {
    switch (target) {
        case GL_ARRAY_BUFFER: return GL_ARRAY_BUFFER_BINDING;
        case GL_ELEMENT_ARRAY_BUFFER: return GL_ELEMENT_ARRAY_BUFFER_BINDING;
        case GL_PIXEL_UNPACK_BUFFER: return GL_PIXEL_UNPACK_BUFFER_BINDING;
        case GL_PIXEL_PACK_BUFFER: return GL_PIXEL_PACK_BUFFER_BINDING;
        case GL_UNIFORM_BUFFER: return GL_UNIFORM_BUFFER_BINDING;
        case GL_COPY_READ_BUFFER: return GL_COPY_READ_BUFFER_BINDING;
        case GL_COPY_WRITE_BUFFER: return GL_COPY_WRITE_BUFFER_BINDING;
        case GL_TRANSFORM_FEEDBACK_BUFFER: return GL_TRANSFORM_FEEDBACK_BUFFER_BINDING;
        default: return 0;
    }
}

// Client-side shadow of the buffer bindings, so resolving one costs nothing.
// Asking the real GL instead was measured at ~124 blocking round-trips per
// frame -- the largest remaining term once the reply-free pipeline landed.
//
// GL_ELEMENT_ARRAY_BUFFER's binding is vertex-array-object state, so it is
// shadowed per VAO and swapped on glBindVertexArray, exactly as GL does; every
// other target's binding is plain context state.
// Never returns 0: an unknown target degrades to a per-target key, which is
// exactly the old behaviour, rather than failing the map outright. Handing the
// engine a null pointer from glMapBufferRange wedges its whole render bring-up
// (live-caught doing exactly that).
GLuint mapping_key_for(GLenum target) {
    GLuint bound = 0;
    if (target == GL_ELEMENT_ARRAY_BUFFER) {
        bound = g_bound_element_buffer;
    } else {
        auto it = bound_buffer_by_target().find(target);
        if (it != bound_buffer_by_target().end()) bound = it->second;
    }
    if (bound != 0) return bound;
    return 0x80000000u | static_cast<GLuint>(target & 0xffffu);
}

}  // namespace

void* glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    if (length <= 0) return nullptr;
    const GLuint buffer = mapping_key_for(target);
    auto& m = mappings()[buffer];
    if (m.active) {
        // Reallocating here would dangle the pointer the caller is still
        // writing through. GL calls this an error; say so rather than corrupt.
        std::fprintf(stderr,
                     "stud: glMapBufferRange: buffer %u is already mapped (target 0x%x)\n", buffer,
                     target);
        std::fflush(stderr);
        return nullptr;
    }

    if (client_array_trace_enabled()) {
        // Which access bits libroblox actually uses decides whether the
        // zeroed-staging shortcut below is honest. A GL_MAP_WRITE_BIT map
        // WITHOUT an invalidate bit is required to preserve whatever the
        // caller does not overwrite -- this emulation cannot, so it would
        // silently zero real geometry.
        static std::vector<GLbitfield> seen;
        bool known = false;
        for (GLbitfield f : seen) known = known || f == access;
        if (!known) {
            seen.push_back(access);
            std::fprintf(stderr,
                         "stud: MAPTRACE glMapBufferRange target=0x%x access=0x%x (read=%d write=%d "
                         "invalidateRange=%d invalidateBuffer=%d flushExplicit=%d unsynchronized=%d) "
                         "length=%ld\n",
                         target, access, (access & GL_MAP_READ_BIT) != 0,
                         (access & GL_MAP_WRITE_BIT) != 0, (access & GL_MAP_INVALIDATE_RANGE_BIT) != 0,
                         (access & GL_MAP_INVALIDATE_BUFFER_BIT) != 0,
                         (access & GL_MAP_FLUSH_EXPLICIT_BIT) != 0,
                         (access & GL_MAP_UNSYNCHRONIZED_BIT) != 0, static_cast<long>(length));
            std::fflush(stderr);
        }
    }
    m.offset = offset;
    m.length = length;
    m.access = access;
    m.staging.assign(static_cast<size_t>(length), 0);
    const bool may_discard =
        (access & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT)) != 0;
    if (!may_discard) {
        uint64_t a[8] = {target, static_cast<uint64_t>(offset), static_cast<uint64_t>(length)};
        uint32_t written = 0;
        connection().call(CallId::GlGetBufferSubData, a, nullptr, 0, m.staging.data(),
                          static_cast<uint32_t>(length), &written);
        if (written != static_cast<uint32_t>(length)) {
            // Honest, visible degrade rather than silent corruption: the
            // caller is about to rely on bytes we could not read back.
            static bool reported = false;
            if (!reported) {
                reported = true;
                std::fprintf(stderr,
                             "stud: glMapBufferRange: could not read back %ld bytes of the real "
                             "buffer (got %u) -- unwritten bytes in this non-invalidating map will "
                             "be zero\n",
                             static_cast<long>(length), written);
                std::fflush(stderr);
            }
        }
    }
    m.active = true;
    return m.staging.data();
}

GLboolean glUnmapBuffer(GLenum target) {
    // GL's own rule: glUnmapBuffer names a target and unmaps whichever buffer
    // is bound to it right now -- so resolve the key exactly the way the map
    // did. Remembering one key per target instead cannot work, because two
    // different buffers can legitimately be mapped through the same target
    // (live-caught: 2890 "already mapped" failures in one run, because the
    // second map overwrote the first buffer's bookkeeping and it was never
    // unmapped again).
    const GLuint buffer = mapping_key_for(target);
    auto it = mappings().find(buffer);
    if (it == mappings().end() || !it->second.active) return GL_FALSE;
    auto& m = it->second;
    m.active = false;
    if (client_array_trace_enabled() && m.length >= 12) {
        static int shown = 0;
        if (shown < 12) {
            ++shown;
            const unsigned short* u = reinterpret_cast<const unsigned short*>(m.staging.data());
            std::fprintf(stderr,
                         "stud: UNMAP target=0x%x buffer=%u offset=%ld length=%ld "
                         "first6=%u %u %u %u %u %u\n",
                         target, buffer, static_cast<long>(m.offset), static_cast<long>(m.length),
                         u[0], u[1], u[2], u[3], u[4], u[5]);
            std::fflush(stderr);
        }
    }
    // Upload whatever the caller wrote into the staging allocation.
    if (m.length > 0) {
        glBufferSubData(target, m.offset, m.length, m.staging.data());
    }
    m.staging.clear();
    m.staging.shrink_to_fit();
    return GL_TRUE;
}

// Real no-op: the whole mapped range is uploaded on unmap anyway, so an
// explicit flush of a sub-range has nothing extra to do.
void glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {
    // Ship the flushed range now, rather than waiting for unmap.
    //
    // This was a no-op, which is only safe if unmap uploads everything --
    // and with GL_MAP_FLUSH_EXPLICIT_BIT that is not what the caller is
    // promised. The spec guarantees flushed ranges reach the buffer, so
    // an engine may flush a range and carry on writing elsewhere, or
    // rely on the data being visible before it unmaps. Dropping the
    // flush loses exactly the writes the caller took care to publish.
    //
    // Same shape as the Vulkan HOST_COHERENT bug fixed alongside this:
    // a mapping that lives in another process only ever reaches the GPU
    // when this side explicitly sends it.
    if (length <= 0) return;
    const GLuint buffer = mapping_key_for(target);
    auto it = mappings().find(buffer);
    if (it == mappings().end() || !it->second.active) return;
    auto& m = it->second;

    // offset is relative to the start of the mapped range.
    if (offset < 0 || offset >= static_cast<GLintptr>(m.staging.size())) return;
    GLsizeiptr n = length;
    if (offset + n > static_cast<GLsizeiptr>(m.staging.size())) {
        n = static_cast<GLsizeiptr>(m.staging.size()) - offset;
    }
    if (n <= 0) return;
    glBufferSubData(target, m.offset + offset, n, m.staging.data() + offset);
}

// Real OES/EXT aliases for the same entry points.
void* glMapBufferRangeEXT(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    return glMapBufferRange(target, offset, length, access);
}
GLboolean glUnmapBufferOES(GLenum target) { return glUnmapBuffer(target); }
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                   GLint border, GLenum format, GLenum type, const void* pixels) {
    uint64_t a[8] = {target,
                      static_cast<uint64_t>(level),
                      static_cast<uint64_t>(internalformat),
                      static_cast<uint64_t>(width),
                      static_cast<uint64_t>(height),
                      static_cast<uint64_t>(border),
                      format,
                      type};
    uint64_t pbo = pbo_tag(pixels);
    uint32_t bytes = (pbo == 0 && pixels != nullptr) ? static_cast<uint32_t>(width) *
                                                            static_cast<uint32_t>(height) *
                                                            gl_pixel_size(format, type)
                                                     : 0;
    connection().call_void(CallId::GlTexImage2D, a, pbo == 0 ? pixels : nullptr, bytes, pbo);
}
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                      GLsizei height, GLenum format, GLenum type, const void* pixels) {
    uint64_t a[8] = {target,
                      static_cast<uint64_t>(level),
                      static_cast<uint64_t>(xoffset),
                      static_cast<uint64_t>(yoffset),
                      static_cast<uint64_t>(width),
                      static_cast<uint64_t>(height),
                      format,
                      type};
    uint64_t pbo = pbo_tag(pixels);
    uint32_t bytes = pbo == 0 ? static_cast<uint32_t>(width) * static_cast<uint32_t>(height) *
                                    gl_pixel_size(format, type)
                              : 0;
    connection().call_void(CallId::GlTexSubImage2D, a, pbo == 0 ? pixels : nullptr, bytes, pbo);
}
void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                             GLsizei height, GLint border, GLsizei imageSize, const void* data) {
    uint64_t a[8] = {target,
                      static_cast<uint64_t>(level),
                      internalformat,
                      static_cast<uint64_t>(width),
                      static_cast<uint64_t>(height),
                      static_cast<uint64_t>(border),
                      static_cast<uint64_t>(imageSize)};
    uint64_t pbo = pbo_tag(data);
    connection().call_void(CallId::GlCompressedTexImage2D, a, pbo == 0 ? data : nullptr,
                           pbo == 0 ? static_cast<uint32_t>(imageSize) : 0, pbo);
}
void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                GLsizei height, GLenum format, GLsizei imageSize, const void* data) {
    uint64_t a[8] = {target,
                      static_cast<uint64_t>(level),
                      static_cast<uint64_t>(xoffset),
                      static_cast<uint64_t>(yoffset),
                      static_cast<uint64_t>(width),
                      static_cast<uint64_t>(height),
                      format,
                      static_cast<uint64_t>(imageSize)};
    uint64_t pbo = pbo_tag(data);
    connection().call_void(CallId::GlCompressedTexSubImage2D, a, pbo == 0 ? data : nullptr,
                           pbo == 0 ? static_cast<uint32_t>(imageSize) : 0, pbo);
}
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                   void* pixels) {
    uint64_t a[8] = {static_cast<uint64_t>(x), static_cast<uint64_t>(y), static_cast<uint64_t>(width),
                      static_cast<uint64_t>(height), format, type};
    // Same real per-format sizing as the upload path -- a 4-bytes-per-texel
    // assumption here would overrun the caller's own destination buffer.
    uint32_t bytes = static_cast<uint32_t>(width) * static_cast<uint32_t>(height) *
                     gl_pixel_size(format, type);
    connection().call(CallId::GlReadPixels, a, nullptr, 0, pixels, bytes, nullptr);
}

}  // extern "C"


