#pragma once

// The cube's GL half, in a file of its own: tools/spinning_cube.h speaks
// GLES2 through its own headers, which do not mix with Qt's GL ones.

// Resolves the cube's entry points through `resolve` on the current
// context and builds it. False if any is missing or a shader fails.
bool cube_gl_init(void* (*resolve)(const char* name, void* data), void* data);

// One frame, `seconds` into the spin, into a `width` x `height` drawable.
void cube_gl_draw(float seconds, int width, int height);
