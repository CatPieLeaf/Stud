// M4/android-glue test: AMediaFormat is a real key/value property bag
// implementation (unlike AMediaCodec, a deliberate stub) -- verifies it
// actually works, not just that it links. See the engineering notes.

#include "stud/media_ndk_types.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
    std::printf("ok: %s\n", what);
}

}  // namespace

int main() {
    AMediaFormat* format = AMediaFormat_new();
    check(format != nullptr, "AMediaFormat_new returns non-null");

    AMediaFormat_setInt32(format, "width", 1920);
    AMediaFormat_setInt32(format, "height", 1080);
    AMediaFormat_setFloat(format, "frame-rate", 60.0f);
    AMediaFormat_setString(format, "mime", "video/avc");

    int32_t width = 0;
    check(AMediaFormat_getInt32(format, "width", &width) && width == 1920,
          "AMediaFormat_getInt32 round-trips a set value correctly");

    int32_t missing = 0;
    check(!AMediaFormat_getInt32(format, "does-not-exist", &missing),
          "AMediaFormat_getInt32 returns false for a key that was never set");

    uint8_t data[] = {1, 2, 3, 4, 5};
    AMediaFormat_setBuffer(format, "csd-0", data, sizeof(data));
    void* buf_ptr = nullptr;
    size_t buf_size = 0;
    check(AMediaFormat_getBuffer(format, "csd-0", &buf_ptr, &buf_size) && buf_size == 5,
          "AMediaFormat_getBuffer round-trips size correctly");
    check(std::memcmp(buf_ptr, data, 5) == 0, "AMediaFormat_getBuffer round-trips bytes correctly");

    const char* str = AMediaFormat_toString(format);
    check(str != nullptr && std::strstr(str, "mime") != nullptr,
          "AMediaFormat_toString produces a non-null string mentioning a real key");

    AMediaFormat_delete(format);

    // AMediaCodec is a deliberate stub -- confirms it fails cleanly
    // (nullptr, matching real Android's own "no codec for this MIME type"
    // behavior) rather than crashing or silently pretending to work.
    AMediaCodec* codec = AMediaCodec_createDecoderByType("video/avc");
    check(codec == nullptr, "AMediaCodec_createDecoderByType returns nullptr (deliberate stub)");

    std::printf("all AMediaFormat/AMediaCodec checks passed\n");
    return 0;
}
