#pragma once

// The cube's GL half, in a file of its own: tools/spinning_cube.h speaks
// GLES2 through its own headers, which do not mix with Qt's GL ones.

// One cube's GL state: its entry points and its buffers, shaders and
// texture, all of the context it was made in. Each view has its own, so two
// cubes on screen at once never draw with each other's.
struct CubeGlState;

// Resolves the cube's entry points through `resolve` on the current
// context and builds it. Null if any is missing or a shader fails.
CubeGlState* cube_gl_create(void* (*resolve)(const char* name, void* data), void* data);

// One frame, `seconds` into the spin, into a `width` x `height` drawable,
// with the context `state` was made in current.
void cube_gl_draw(CubeGlState* state, float seconds, int width, int height);

// Frees the state; the GL objects go with their context.
void cube_gl_destroy(CubeGlState* state);

// Every cube takes its stud from the one place, the installed Roblox Player's
// studs texture, read once and shared. Called when a download has finished:
// every cube on screen picks the stud up again on its next frame.
void cube_gl_client_changed();
