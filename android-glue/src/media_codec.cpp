#include <cstdio>
#include "stud/media_ndk_types.h"

#include <cstring>
#include <map>
#include <string>
#include <variant>
#include <vector>

// AMediaFormat: a real, working key/value property bag -- no actual codec
// capability needed for this half of the API.
struct AMediaFormat {
    using Value = std::variant<int32_t, float, std::string, std::pair<std::vector<uint8_t>, size_t>>;
    std::map<std::string, Value> values;
    std::string to_string_cache;
};

// Real AOSP NdkMediaFormat.h key-name string values (stable public NDK ABI,
// same string content on every Android version). See media_ndk_types.h for
// why these need to be real, not just present.
extern "C" {
const char* AMEDIAFORMAT_KEY_MIME = "mime";
const char* AMEDIAFORMAT_KEY_WIDTH = "width";
const char* AMEDIAFORMAT_KEY_HEIGHT = "height";
const char* AMEDIAFORMAT_KEY_BIT_RATE = "bitrate";
const char* AMEDIAFORMAT_KEY_CHANNEL_COUNT = "channel-count";
const char* AMEDIAFORMAT_KEY_COLOR_FORMAT = "color-format";
const char* AMEDIAFORMAT_KEY_FRAME_RATE = "frame-rate";
const char* AMEDIAFORMAT_KEY_I_FRAME_INTERVAL = "i-frame-interval";
const char* AMEDIAFORMAT_KEY_SAMPLE_RATE = "sample-rate";
const char* AMEDIAFORMAT_KEY_STRIDE = "stride";
}

// AMediaCodec: deliberate stub. Real hardware/software video decode is out
// of scope for the prototype (see the engineering notes). Creation always
// returns nullptr ("codec unavailable for this MIME type"), matching a
// real device's behavior when no matching codec exists -- callers are
// already expected to null-check per the real API's own documented
// contract, so this doesn't introduce a new failure mode.
extern "C" {

AMediaCodec* AMediaCodec_createDecoderByType(const char* mime_type) {
    // Which MIME the engine asks for is the first thing any real
    // implementation needs to know, and nothing recorded it before.
    std::fprintf(stderr, "stud: AMediaCodec_createDecoderByType(\"%s\") -- no decoder\n",
                 mime_type != nullptr ? mime_type : "(null)");
    return nullptr;
}
AMediaCodec* AMediaCodec_createEncoderByType(const char* mime_type) {
    std::fprintf(stderr, "stud: AMediaCodec_createEncoderByType(\"%s\") -- no encoder\n",
                 mime_type != nullptr ? mime_type : "(null)");
    return nullptr;
}
media_status_t AMediaCodec_delete(AMediaCodec* /*codec*/) { return AMEDIA_OK; }
media_status_t AMediaCodec_configure(AMediaCodec* /*codec*/, const AMediaFormat* /*format*/,
                                      void* /*surface*/, void* /*crypto*/, uint32_t /*flags*/) {
    return AMEDIA_ERROR_UNSUPPORTED;
}
media_status_t AMediaCodec_start(AMediaCodec* /*codec*/) { return AMEDIA_ERROR_UNSUPPORTED; }
media_status_t AMediaCodec_stop(AMediaCodec* /*codec*/) { return AMEDIA_OK; }
media_status_t AMediaCodec_flush(AMediaCodec* /*codec*/) { return AMEDIA_OK; }
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec* /*codec*/, int64_t /*timeoutUs*/) { return -1; }
uint8_t* AMediaCodec_getInputBuffer(AMediaCodec* /*codec*/, size_t /*idx*/, size_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    return nullptr;
}
uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec* /*codec*/, size_t /*idx*/, size_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    return nullptr;
}
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec* /*codec*/, size_t /*idx*/, off_t /*offset*/,
                                             size_t /*size*/, uint64_t /*time*/, uint32_t /*flags*/) {
    return AMEDIA_ERROR_UNSUPPORTED;
}
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec* /*codec*/, void* /*info*/, int64_t /*timeoutUs*/) {
    return -1;
}
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec* /*codec*/, size_t /*idx*/, bool /*render*/) {
    return AMEDIA_OK;
}
AMediaFormat* AMediaCodec_getOutputFormat(AMediaCodec* /*codec*/) { return nullptr; }

AMediaFormat* AMediaFormat_new() { return new AMediaFormat(); }

media_status_t AMediaFormat_delete(AMediaFormat* format) {
    delete format;
    return AMEDIA_OK;
}

const char* AMediaFormat_toString(AMediaFormat* format) {
    std::string out = "AMediaFormat{";
    bool first = true;
    for (auto& [key, value] : format->values) {
        if (!first) out += ", ";
        first = false;
        out += key + "=";
        std::visit(
            [&](auto&& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, int32_t>) {
                    out += std::to_string(v);
                } else if constexpr (std::is_same_v<T, float>) {
                    out += std::to_string(v);
                } else if constexpr (std::is_same_v<T, std::string>) {
                    out += v;
                } else {
                    out += "<buffer>";
                }
            },
            value);
    }
    out += "}";
    format->to_string_cache = std::move(out);
    return format->to_string_cache.c_str();
}

bool AMediaFormat_getInt32(AMediaFormat* format, const char* name, int32_t* out) {
    auto it = format->values.find(name);
    if (it == format->values.end() || !std::holds_alternative<int32_t>(it->second)) return false;
    *out = std::get<int32_t>(it->second);
    return true;
}

void AMediaFormat_setInt32(AMediaFormat* format, const char* name, int32_t value) {
    format->values[name] = value;
}

void AMediaFormat_setFloat(AMediaFormat* format, const char* name, float value) {
    format->values[name] = value;
}

void AMediaFormat_setString(AMediaFormat* format, const char* name, const char* value) {
    format->values[name] = std::string(value);
}

bool AMediaFormat_getBuffer(AMediaFormat* format, const char* name, void** data, size_t* size) {
    auto it = format->values.find(name);
    using BufferType = std::pair<std::vector<uint8_t>, size_t>;
    if (it == format->values.end() || !std::holds_alternative<BufferType>(it->second)) return false;
    auto& buf = std::get<BufferType>(it->second);
    *data = buf.first.data();
    *size = buf.second;
    return true;
}

void AMediaFormat_setBuffer(AMediaFormat* format, const char* name, void* data, size_t size) {
    auto* bytes = static_cast<uint8_t*>(data);
    format->values[name] = std::make_pair(std::vector<uint8_t>(bytes, bytes + size), size);
}

}  // extern "C"
