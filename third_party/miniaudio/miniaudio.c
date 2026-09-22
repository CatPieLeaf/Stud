// miniaudio is a single header that carries its own implementation, and
// this is the one translation unit that asks for it. Stud's own code
// includes miniaudio.h without this define and so gets declarations only.
//
// It is compiled as C rather than C++ deliberately: that is the language
// upstream tests, and 95,000 lines of it through a C++ front end buys
// nothing but compile time and warnings about constructs that are legal
// where they are written.
//
// The MA_NO_* switches that decide which backends and subsystems exist
// are set on the render-host target in CMake, not here, because they
// change miniaudio's declarations as well as its definitions: setting
// them in one file and not the other is how a struct comes out a
// different size in two translation units.
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
