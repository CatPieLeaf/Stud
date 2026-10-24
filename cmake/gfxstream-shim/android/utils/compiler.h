#pragma once

// The one thing gfxstream's ring_buffer.h takes from the emulator's own
// headers: C linkage around its declarations. See StudGfxstreamRing.cmake.
#ifdef __cplusplus
#define ANDROID_BEGIN_HEADER extern "C" {
#define ANDROID_END_HEADER }
#else
#define ANDROID_BEGIN_HEADER
#define ANDROID_END_HEADER
#endif
