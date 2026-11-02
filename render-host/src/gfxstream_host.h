#pragma once

// OpenGL mode through gfxstream: the engine's GLES runs in Process B on
// gfxstream's guest encoder, and is decoded here on the system's own EGL and
// GLES driver (EGL-on-EGL), on render-host's display. Finished frames arrive
// as colour buffers this process draws into the window.
//
// Built only when CMake is given a gfxstream build (STUD_GFXSTREAM_DIR);
// otherwise available() is false and OpenGL mode forwards as before.

#include <cstdint>
#include <string>

namespace stud::render_host::gfxstream_host {

bool available();

// Starts the renderer on `native_display` (an EGL platform and its native
// display, render-host's own) and serves the guest's connections on
// `socket_path`. False if either fails; nothing is left running then.
bool start(unsigned int egl_platform, void* native_display, const std::string& socket_path);

// After start(): a host driver context sharing the guest's colour buffers,
// to create the presenting context against.
void* shared_context();

// The host driver's texture holding colour buffer `handle`, and its size;
// 0 if there is none.
unsigned int color_buffer_texture(uint32_t handle, uint32_t* width, uint32_t* height);

// Puts `texture` (a host driver texture, 0 to clear) on texture unit `unit`
// whatever the guest binds on that unit's 2D binding.
void set_unit_texture_override(unsigned int unit, unsigned int texture);

// Makes texture unit `unit` always show the cube map the guest binds on
// unit `source`.
void set_cube_unit_mirror(unsigned int unit, unsigned int source);

// The presenter's side of a frame, its context current. wait_frame: until
// the guest's frame into `color_buffer` has reached the host, then the GPU
// waits for it, so drawing from the colour buffer needs no glFinish
// anywhere. frame_shown: after drawing from it, so the guest's next frame
// into it waits on the GPU for that draw.
void wait_frame(uint32_t color_buffer);
void frame_shown(uint32_t color_buffer);

}  // namespace stud::render_host::gfxstream_host
