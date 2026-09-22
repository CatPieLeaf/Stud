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
// Pinned to upstream commit dc3b628ddf25dbf7bdc6d39035240d9d933617af
// (the dev branch, which calls itself v0.11.26), not to the v0.11.25
// release, for one reason: the release's PulseAudio backend ignores the
// format the application asked for and copies the sink's own instead.
// Where that sink is an integer format -- s32 on the machine this was
// found on -- every float sample is converted down to it on the way out,
// and that conversion clamps at full scale, so any peak above 1.0 is
// destroyed before the sound server sees it. Audible as hard clipping on
// loud, bassy passages.
//
// Upstream fixed it on dev (`ss.format = ma_format_to_pulse(...)`), so
// this tracks upstream rather than carrying a local patch. Move to
// v0.11.26 proper once it is released; do not go back to v0.11.25.
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
