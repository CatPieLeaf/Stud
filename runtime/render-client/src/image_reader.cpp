// NdkImageReader.h and the two hardware-buffer reference calls, answered
// honestly: Stud has no image reader.
//
// Roblox 2.744 started importing these. An image reader is how Android
// hands decoded video frames to a renderer as GPU buffers; Stud's
// AMediaCodec (media_codec_forward.cpp) decodes into plain output buffers
// instead, which is the path an app takes on a device without one. So a
// reader cannot be made, every call on one that does not exist says so,
// and nothing here ever hands out an AHardwareBuffer.

#include "stud/media_ndk_types.h"

#include <cstdint>
#include <cstdio>

extern "C" {

struct AImageReader;
struct AImage;
struct AHardwareBuffer;
struct ANativeWindow;

media_status_t AImageReader_newWithUsage(int32_t width, int32_t height, int32_t format,
                                         uint64_t usage, int32_t max_images,
                                         AImageReader** reader) {
    if (reader != nullptr) *reader = nullptr;
    static bool said = false;
    if (!said) {
        said = true;
        std::fprintf(stderr,
                     "stud: libmediandk: the engine asked for an image reader (%dx%d format %d); "
                     "Stud has none, so it decodes video the plain-buffer way\n",
                     width, height, format);
    }
    (void)usage;
    (void)max_images;
    return AMEDIA_ERROR_UNSUPPORTED;
}

media_status_t AImageReader_getWindow(AImageReader*, ANativeWindow** window) {
    if (window != nullptr) *window = nullptr;
    return AMEDIA_ERROR_INVALID_PARAMETER;
}

media_status_t AImageReader_acquireNextImage(AImageReader*, AImage** image) {
    if (image != nullptr) *image = nullptr;
    return AMEDIA_ERROR_INVALID_PARAMETER;
}

void AImageReader_delete(AImageReader*) {}

void AImage_delete(AImage*) {}

media_status_t AImage_getHardwareBuffer(const AImage*, AHardwareBuffer** buffer) {
    if (buffer != nullptr) *buffer = nullptr;
    return AMEDIA_ERROR_INVALID_PARAMETER;
}

media_status_t AImage_getTimestamp(const AImage*, int64_t* timestamp_ns) {
    if (timestamp_ns != nullptr) *timestamp_ns = 0;
    return AMEDIA_ERROR_INVALID_PARAMETER;
}

// No AHardwareBuffer is ever handed out, so there is nothing to count.
void AHardwareBuffer_acquire(AHardwareBuffer*) {}
void AHardwareBuffer_release(AHardwareBuffer*) {}

}  // extern "C"
