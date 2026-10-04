// Stands in for a real libEGL.so, just enough exported symbols
// (real eglGetError plus a made-up one) to prove stud::render::resolve()'s
// dispatch mechanism (prefix routing + dlopen/dlsym) works, without
// depending on a real EGL/GLES driver being present on the build machine.

int eglGetError(void) { return 0x3000; /* EGL_SUCCESS */ }

int eglStudToyMarker(void) { return 111; }
