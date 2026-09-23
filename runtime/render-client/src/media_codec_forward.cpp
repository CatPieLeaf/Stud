// AMediaCodec for Process B, forwarding video decoding to stud-render-host.
//
// The engine plays video through Android's MediaCodec in ByteBuffer mode:
// it writes compressed access units into input buffers, and reads raw YUV
// back out of output buffers, whose layout it takes from the output format
// (color-format, stride, slice-height and the crop). The decoding itself is
// the system's FFmpeg, in the render host; see video_decoder.h there.
//
// This used to answer every call with "unsupported", so the engine had no
// decoder at all and video in experiences never played.
//
// Encoders are not provided: createEncoderByType still answers null, which
// is a device with no hardware encoder for that type.

#include "render_client_common.h"
#include "stud/media_ndk_types.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using stud::render_host::CallId;
using stud::render_host::VideoFrameHeader;

namespace {

constexpr ssize_t kTryAgainLater = -1;        // AMEDIACODEC_INFO_TRY_AGAIN_LATER
constexpr ssize_t kOutputFormatChanged = -2;  // AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED
constexpr uint32_t kFlagCodecConfig = 2;      // AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG
constexpr uint32_t kFlagEndOfStream = 4;      // AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM
constexpr int32_t kColorFormatNv12 = 21;      // COLOR_FormatYUV420SemiPlanar
constexpr media_status_t kErrorUnknown = -10000;
constexpr size_t kInputBuffers = 4;

// AMediaCodecBufferInfo, as NdkMediaCodec.h lays it out.
struct BufferInfo {
    int32_t offset;
    int32_t size;
    int64_t presentationTimeUs;
    uint32_t flags;
};

struct OutputSlot {
    bool used = false;
    std::vector<uint8_t> data;  // NV12, as the host sent it
    int64_t pts_us = 0;
    uint32_t flags = 0;
};

}  // namespace

struct AMediaCodec {
    uint64_t host = 0;
    std::string mime;
    std::mutex mutex;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> csd;  // csd-0 then csd-1, from configure()
    bool started = false;

    size_t input_capacity = 0;
    std::vector<std::vector<uint8_t>> inputs;
    std::vector<bool> input_free;

    std::vector<OutputSlot> outputs;
    // A frame that arrived with a new format, held back until the engine
    // has seen the format change.
    bool have_pending = false;
    OutputSlot pending;
    VideoFrameHeader format{};
    bool format_reported = false;
    bool ended = false;
};

namespace {

uint64_t host_call(CallId id, uint64_t a0, uint64_t a1, uint64_t a2, const void* in, size_t in_len,
                   void* out = nullptr, uint32_t out_cap = 0, uint32_t* written = nullptr) {
    uint64_t a[8] = {a0, a1, a2};
    return stud::render_client::connection().call(id, a, in, static_cast<uint32_t>(in_len), out,
                                                   out_cap, written);
}

int64_t as_signed(uint64_t v) { return static_cast<int64_t>(v); }

// One frame from the host, or none yet. The largest frame the engine could
// be sent is bounded by the configured size; the buffer grows if a stream
// changes resolution upward.
int32_t fetch_frame(AMediaCodec* c, OutputSlot& slot, VideoFrameHeader& header) {
    const size_t guess =
        sizeof(VideoFrameHeader) +
        static_cast<size_t>((c->width + 15u) & ~15u) * ((c->height + 1u) & ~1u) * 3u / 2u;
    std::vector<uint8_t> buf(std::max<size_t>(guess, 1u << 20));
    uint32_t written = 0;
    const int32_t r = static_cast<int32_t>(as_signed(
        host_call(CallId::VideoDecoderDequeue, c->host, 0, 0, nullptr, 0, buf.data(),
                  static_cast<uint32_t>(buf.size()), &written)));
    if (r != 1) return r;
    if (written < sizeof(VideoFrameHeader)) return -1;
    std::memcpy(&header, buf.data(), sizeof(header));
    if (sizeof(header) + header.data_size > written) {
        // Did not fit, and the host has already handed the frame over:
        // there is no second copy to ask for. Sized for the next one.
        std::fprintf(stderr, "stud: AMediaCodec: a %ux%u frame outgrew the buffer\n",
                     header.width, header.height);
        c->width = header.width;
        c->height = header.height;
        return 0;
    }
    slot.data.assign(buf.begin() + sizeof(header),
                     buf.begin() + static_cast<long>(sizeof(header) + header.data_size));
    slot.pts_us = header.pts_us;
    slot.flags = 0;
    return 1;
}

bool same_layout(const VideoFrameHeader& a, const VideoFrameHeader& b) {
    return a.width == b.width && a.height == b.height && a.stride == b.stride &&
           a.slice_height == b.slice_height;
}

ssize_t deliver(AMediaCodec* c, OutputSlot&& slot, BufferInfo* info) {
    size_t idx = 0;
    while (idx < c->outputs.size() && c->outputs[idx].used) ++idx;
    if (idx == c->outputs.size()) c->outputs.emplace_back();
    OutputSlot& s = c->outputs[idx];
    s = std::move(slot);
    s.used = true;
    if (info != nullptr) {
        info->offset = 0;
        info->size = static_cast<int32_t>(s.data.size());
        info->presentationTimeUs = s.pts_us;
        info->flags = s.flags;
    }
    return static_cast<ssize_t>(idx);
}

}  // namespace

extern "C" {

// For MediaCodecInfoUtils.getVideoCodecs(), which advertises to the engine
// only what the host can really decode.
int stud_video_decoder_supported(const char* mime) {
    if (mime == nullptr) return 0;
    return host_call(CallId::VideoDecoderSupported, 0, 0, 0, mime, std::strlen(mime) + 1) != 0;
}

AMediaCodec* AMediaCodec_createDecoderByType(const char* mime_type) {
    if (mime_type == nullptr) return nullptr;
    const uint64_t host = host_call(CallId::VideoDecoderCreate, 0, 0, 0, mime_type,
                                    std::strlen(mime_type) + 1);
    std::fprintf(stderr, "stud: AMediaCodec_createDecoderByType(\"%s\") -> %s\n", mime_type,
                 host != 0 ? "decoder" : "no decoder on this system");
    if (host == 0) return nullptr;
    auto* c = new AMediaCodec();
    c->host = host;
    c->mime = mime_type;
    return c;
}

AMediaCodec* AMediaCodec_createEncoderByType(const char* mime_type) {
    std::fprintf(stderr, "stud: AMediaCodec_createEncoderByType(\"%s\"); no encoder\n",
                 mime_type != nullptr ? mime_type : "(null)");
    return nullptr;
}

media_status_t AMediaCodec_delete(AMediaCodec* codec) {
    if (codec == nullptr) return AMEDIA_OK;
    host_call(CallId::VideoDecoderDestroy, codec->host, 0, 0, nullptr, 0);
    delete codec;
    return AMEDIA_OK;
}

media_status_t AMediaCodec_configure(AMediaCodec* codec, const AMediaFormat* format, void* surface,
                                     void* /*crypto*/, uint32_t flags) {
    if (codec == nullptr || format == nullptr) return kErrorUnknown;
    // Output to a Surface, or encoding, is not what this provides; saying
    // so is what a device whose decoder cannot do it says.
    if (surface != nullptr || (flags & 1u) != 0) {
        std::fprintf(stderr, "stud: AMediaCodec_configure: %s output is not supported\n",
                     surface != nullptr ? "Surface" : "encoder");
        return AMEDIA_ERROR_UNSUPPORTED;
    }
    std::lock_guard<std::mutex> lock(codec->mutex);
    auto* f = const_cast<AMediaFormat*>(format);
    int32_t v = 0;
    if (AMediaFormat_getInt32(f, "width", &v) && v > 0) codec->width = static_cast<uint32_t>(v);
    if (AMediaFormat_getInt32(f, "height", &v) && v > 0) codec->height = static_cast<uint32_t>(v);
    codec->csd.clear();
    for (const char* key : {"csd-0", "csd-1"}) {
        void* data = nullptr;
        size_t size = 0;
        if (AMediaFormat_getBuffer(f, key, &data, &size) && data != nullptr && size > 0) {
            const auto* p = static_cast<const uint8_t*>(data);
            codec->csd.insert(codec->csd.end(), p, p + size);
        }
    }
    // Big enough for any access unit the engine could write: max-input-size
    // when it says, and otherwise an uncompressed frame, which no
    // compressed one exceeds.
    int32_t max_input = 0;
    AMediaFormat_getInt32(f, "max-input-size", &max_input);
    const size_t frame = static_cast<size_t>(codec->width) * codec->height * 3u / 2u;
    codec->input_capacity =
        std::max<size_t>({static_cast<size_t>(std::max(0, max_input)), frame, 1u << 20});
    codec->inputs.assign(kInputBuffers, std::vector<uint8_t>(codec->input_capacity));
    codec->input_free.assign(kInputBuffers, true);
    return AMEDIA_OK;
}

media_status_t AMediaCodec_start(AMediaCodec* codec) {
    if (codec == nullptr) return kErrorUnknown;
    std::lock_guard<std::mutex> lock(codec->mutex);
    if (codec->started) return AMEDIA_OK;
    const int32_t r = static_cast<int32_t>(as_signed(
        host_call(CallId::VideoDecoderConfigure, codec->host, codec->width, codec->height,
                  codec->csd.data(), codec->csd.size())));
    if (r != AMEDIA_OK) return r;
    codec->started = true;
    return AMEDIA_OK;
}

media_status_t AMediaCodec_stop(AMediaCodec* codec) {
    if (codec == nullptr) return kErrorUnknown;
    return AMediaCodec_flush(codec);
}

media_status_t AMediaCodec_flush(AMediaCodec* codec) {
    if (codec == nullptr) return kErrorUnknown;
    std::lock_guard<std::mutex> lock(codec->mutex);
    host_call(CallId::VideoDecoderFlush, codec->host, 0, 0, nullptr, 0);
    for (auto&& free : codec->input_free) free = true;
    for (auto& o : codec->outputs) o.used = false;
    codec->have_pending = false;
    codec->ended = false;
    return AMEDIA_OK;
}

ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec* codec, int64_t /*timeoutUs*/) {
    if (codec == nullptr) return kTryAgainLater;
    std::lock_guard<std::mutex> lock(codec->mutex);
    // Inputs are sent to the host the moment they are queued, so one is
    // always free again by the next call; the timeout never has to wait.
    for (size_t i = 0; i < codec->input_free.size(); ++i) {
        if (codec->input_free[i]) {
            codec->input_free[i] = false;
            return static_cast<ssize_t>(i);
        }
    }
    return kTryAgainLater;
}

uint8_t* AMediaCodec_getInputBuffer(AMediaCodec* codec, size_t idx, size_t* out_size) {
    if (codec == nullptr || idx >= codec->inputs.size()) {
        if (out_size != nullptr) *out_size = 0;
        return nullptr;
    }
    if (out_size != nullptr) *out_size = codec->inputs[idx].size();
    return codec->inputs[idx].data();
}

media_status_t AMediaCodec_queueInputBuffer(AMediaCodec* codec, size_t idx, off_t offset,
                                            size_t size, uint64_t time, uint32_t flags) {
    if (codec == nullptr || idx >= codec->inputs.size()) return kErrorUnknown;
    std::lock_guard<std::mutex> lock(codec->mutex);
    codec->input_free[idx] = true;
    if (offset < 0 || static_cast<size_t>(offset) + size > codec->inputs[idx].size()) {
        return kErrorUnknown;
    }
    // A codec-config buffer is in-band SPS/PPS or the like; FFmpeg takes it
    // as an ordinary packet, the same way it reads it from a stream.
    (void)kFlagCodecConfig;
    const uint8_t* data = codec->inputs[idx].data() + offset;
    const int32_t r = static_cast<int32_t>(as_signed(
        host_call(CallId::VideoDecoderQueue, codec->host, time,
                  (flags & kFlagEndOfStream) != 0 ? 1 : 0, data, size)));
    return r == AMEDIA_OK ? AMEDIA_OK : r;
}

ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec* codec, void* info_ptr, int64_t timeoutUs) {
    if (codec == nullptr) return kTryAgainLater;
    auto* info = static_cast<BufferInfo*>(info_ptr);
    std::unique_lock<std::mutex> lock(codec->mutex);
    if (codec->have_pending) {
        codec->have_pending = false;
        return deliver(codec, std::move(codec->pending), info);
    }
    if (codec->ended) return kTryAgainLater;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::microseconds(timeoutUs < 0 ? 100000 : timeoutUs);
    for (;;) {
        OutputSlot slot;
        VideoFrameHeader header{};
        const int32_t r = fetch_frame(codec, slot, header);
        if (r == 1) {
            if (!codec->format_reported || !same_layout(header, codec->format)) {
                // The engine reads the layout from the output format before
                // the frame, so the frame waits for the next call.
                codec->format = header;
                codec->format_reported = true;
                codec->pending = std::move(slot);
                codec->have_pending = true;
                return kOutputFormatChanged;
            }
            return deliver(codec, std::move(slot), info);
        }
        if (r == 2) {
            codec->ended = true;
            OutputSlot eos;
            eos.flags = kFlagEndOfStream;
            return deliver(codec, std::move(eos), info);
        }
        if (r < 0) return kTryAgainLater;
        if (std::chrono::steady_clock::now() >= deadline) return kTryAgainLater;
        // Not holding the codec while waiting: the engine may be queueing
        // input on another thread, and that input is what makes a frame.
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lock.lock();
    }
}

uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec* codec, size_t idx, size_t* out_size) {
    if (codec == nullptr || idx >= codec->outputs.size() || !codec->outputs[idx].used) {
        if (out_size != nullptr) *out_size = 0;
        return nullptr;
    }
    if (out_size != nullptr) *out_size = codec->outputs[idx].data.size();
    return codec->outputs[idx].data.data();
}

media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec* codec, size_t idx, bool /*render*/) {
    if (codec == nullptr) return kErrorUnknown;
    std::lock_guard<std::mutex> lock(codec->mutex);
    if (idx < codec->outputs.size()) {
        codec->outputs[idx].used = false;
        codec->outputs[idx].data.clear();
    }
    return AMEDIA_OK;
}

AMediaFormat* AMediaCodec_getOutputFormat(AMediaCodec* codec) {
    AMediaFormat* f = AMediaFormat_new();
    if (codec == nullptr) return f;
    std::lock_guard<std::mutex> lock(codec->mutex);
    const VideoFrameHeader& h = codec->format;
    AMediaFormat_setString(f, "mime", "video/raw");
    AMediaFormat_setInt32(f, "width", static_cast<int32_t>(h.width));
    AMediaFormat_setInt32(f, "height", static_cast<int32_t>(h.height));
    AMediaFormat_setInt32(f, "color-format", kColorFormatNv12);
    AMediaFormat_setInt32(f, "stride", static_cast<int32_t>(h.stride));
    AMediaFormat_setInt32(f, "slice-height", static_cast<int32_t>(h.slice_height));
    AMediaFormat_setInt32(f, "crop-left", static_cast<int32_t>(h.crop_left));
    AMediaFormat_setInt32(f, "crop-top", static_cast<int32_t>(h.crop_top));
    AMediaFormat_setInt32(f, "crop-right", static_cast<int32_t>(h.crop_right));
    AMediaFormat_setInt32(f, "crop-bottom", static_cast<int32_t>(h.crop_bottom));
    return f;
}

}  // extern "C"
