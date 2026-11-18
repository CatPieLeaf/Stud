#pragma once

// Separate because spinning_cube.h's GLES2 headers clash with Qt's GL ones.

// Per view, tied to the context it was made in.
struct CubeGlState;

// Null if an entry point is missing or a shader fails.
CubeGlState* cube_gl_create(void* (*resolve)(const char* name, void* data), void* data);

// With the state's context current.
void cube_gl_draw(CubeGlState* state, float seconds, int width, int height);

// GL objects go with their context.
void cube_gl_destroy(CubeGlState* state);

// Call after a client download: every cube reloads the shared stud on its next frame.
void cube_gl_client_changed();
