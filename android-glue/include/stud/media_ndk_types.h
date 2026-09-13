#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>

// Real Android NDK media API declarations (NdkMediaCodec.h, NdkMediaFormat.h)
// -- signatures as documented/stable in the actual NDK, same "reconstructed
// from documented behavior, not copied" approach as ndk_types.h.
//
// AMediaCodec itself is a deliberate stub (see media_codec.cpp) -- real
// hardware/software video decode is out of scope for the prototype (see
// the engineering notes, "AMediaCodec (video): stub for prototype, real shim
// later"). AMediaFormat is a real, working implementation -- it's just a
// key/value property bag, no actual codec capability needed.

extern "C" {

struct AMediaCodec;
struct AMediaFormat;

using media_status_t = int;
constexpr media_status_t AMEDIA_OK = 0;
constexpr media_status_t AMEDIA_ERROR_UNSUPPORTED = -10004;

// --- NdkMediaCodec.h (stub -- see media_codec.cpp) ----------------------

AMediaCodec* AMediaCodec_createDecoderByType(const char* mime_type);
AMediaCodec* AMediaCodec_createEncoderByType(const char* mime_type);
media_status_t AMediaCodec_delete(AMediaCodec* codec);
media_status_t AMediaCodec_configure(AMediaCodec* codec, const AMediaFormat* format, void* surface,
                                      void* crypto, uint32_t flags);
media_status_t AMediaCodec_start(AMediaCodec* codec);
media_status_t AMediaCodec_stop(AMediaCodec* codec);
media_status_t AMediaCodec_flush(AMediaCodec* codec);
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec* codec, int64_t timeoutUs);
uint8_t* AMediaCodec_getInputBuffer(AMediaCodec* codec, size_t idx, size_t* out_size);
uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec* codec, size_t idx, size_t* out_size);
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec* codec, size_t idx, off_t offset, size_t size,
                                             uint64_t time, uint32_t flags);
// AMediaCodecBufferInfo's real layout isn't needed -- the stub never
// writes through this pointer, and pointer-passing ABI doesn't depend on
// the pointee type.
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec* codec, void* info, int64_t timeoutUs);
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec* codec, size_t idx, bool render);
AMediaFormat* AMediaCodec_getOutputFormat(AMediaCodec* codec);

// --- NdkMediaFormat.h key-name constants ---------------------------------
//
// Real, publicly documented, stable AOSP NDK ABI string constants (not
// Roblox-specific unknowns -- see frameworks/av's NdkMediaFormat.cpp,
// mirrored across every NDK release). Found via a real load: libroblox.so
// imports these as OBJECT (data) symbols -- some statically-linked
// component reads the key *names* through these exported pointers rather
// than hardcoding string literals, same pattern as __sF (see
// libc_shim.cpp). AMediaCodec being a stub doesn't make these values
// unimportant: whatever component calls AMediaFormat_setString/getString
// needs the same key string AMediaFormat's internal property-bag map
// would key on.

extern const char* AMEDIAFORMAT_KEY_MIME;
extern const char* AMEDIAFORMAT_KEY_WIDTH;
extern const char* AMEDIAFORMAT_KEY_HEIGHT;
extern const char* AMEDIAFORMAT_KEY_BIT_RATE;
extern const char* AMEDIAFORMAT_KEY_CHANNEL_COUNT;
extern const char* AMEDIAFORMAT_KEY_COLOR_FORMAT;
extern const char* AMEDIAFORMAT_KEY_FRAME_RATE;
extern const char* AMEDIAFORMAT_KEY_I_FRAME_INTERVAL;
extern const char* AMEDIAFORMAT_KEY_SAMPLE_RATE;
extern const char* AMEDIAFORMAT_KEY_STRIDE;

// --- NdkMediaFormat.h (real implementation) -------------------------------

AMediaFormat* AMediaFormat_new();
media_status_t AMediaFormat_delete(AMediaFormat* format);
const char* AMediaFormat_toString(AMediaFormat* format);
bool AMediaFormat_getInt32(AMediaFormat* format, const char* name, int32_t* out);
void AMediaFormat_setInt32(AMediaFormat* format, const char* name, int32_t value);
void AMediaFormat_setFloat(AMediaFormat* format, const char* name, float value);
void AMediaFormat_setString(AMediaFormat* format, const char* name, const char* value);
bool AMediaFormat_getBuffer(AMediaFormat* format, const char* name, void** data, size_t* size);
void AMediaFormat_setBuffer(AMediaFormat* format, const char* name, void* data, size_t size);

}  // extern "C"
