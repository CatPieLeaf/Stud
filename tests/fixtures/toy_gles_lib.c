// Stands in for a real ANGLE libGLESv2.so; see toy_egl_lib.c.

unsigned int glGetError(void) { return 0; /* GL_NO_ERROR */ }

int glStudToyMarker(void) { return 222; }
