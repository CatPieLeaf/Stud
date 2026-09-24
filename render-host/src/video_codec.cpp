#include "stud/video_codec.h"

#include "stud/render_host_protocol.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(STUD_HAVE_FFMPEG)
#include <dlfcn.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}
#endif

namespace stud::render_host {

namespace {

// AMediaCodec's own status values, which is what the client hands back.
constexpr int32_t kOk = 0;
constexpr int32_t kErrorUnknown = -10000;
constexpr int32_t kErrorUnsupported = -10004;

#if defined(STUD_HAVE_FFMPEG)

// The few libavcodec/libavutil entry points used, resolved from the exact
// sonames these headers were written for. Loading by the version's own
// number is what makes reading AVFrame and AVCodecContext fields safe: a
// library whose major version differs is a different ABI, and is not
// loaded at all.
struct Av {
    bool ok = false;
    decltype(&avcodec_find_decoder) find_decoder = nullptr;
    decltype(&avcodec_alloc_context3) alloc_context = nullptr;
    decltype(&avcodec_open2) open = nullptr;
    decltype(&avcodec_free_context) free_context = nullptr;
    decltype(&avcodec_send_packet) send_packet = nullptr;
    decltype(&avcodec_receive_frame) receive_frame = nullptr;
    decltype(&avcodec_flush_buffers) flush = nullptr;
    decltype(&av_packet_alloc) packet_alloc = nullptr;
    decltype(&av_packet_free) packet_free = nullptr;
    decltype(&av_frame_alloc) frame_alloc = nullptr;
    decltype(&av_frame_free) frame_free = nullptr;
    decltype(&av_frame_unref) frame_unref = nullptr;
    decltype(&av_mallocz) mallocz = nullptr;
    // Encoding.
    decltype(&avcodec_find_encoder_by_name) find_encoder_by_name = nullptr;
    decltype(&avcodec_send_frame) send_frame = nullptr;
    decltype(&avcodec_receive_packet) receive_packet = nullptr;
    decltype(&avcodec_get_supported_config) supported_config = nullptr;
    decltype(&av_packet_unref) packet_unref = nullptr;
    decltype(&av_frame_get_buffer) frame_get_buffer = nullptr;
    // Hardware frames, for the encoders that only take frames already on
    // the GPU (VA-API, Vulkan): the engine's frame is uploaded into one.
    bool hw_ok = false;
    decltype(&av_hwdevice_ctx_create) hwdevice_create = nullptr;
    decltype(&av_hwframe_ctx_alloc) hwframe_ctx_alloc = nullptr;
    decltype(&av_hwframe_ctx_init) hwframe_ctx_init = nullptr;
    decltype(&av_hwframe_get_buffer) hwframe_get_buffer = nullptr;
    decltype(&av_hwframe_transfer_data) hwframe_transfer = nullptr;
    decltype(&av_buffer_ref) buffer_ref = nullptr;
    decltype(&av_buffer_unref) buffer_unref = nullptr;
    // Pixel-format conversion, from libswscale: formats other than 8-bit
    // 4:2:0 on the way out of a decoder, and whatever an encoder wants on
    // the way in. Without it those are refused, as they were before.
    bool sws_ok = false;
    decltype(&sws_getCachedContext) sws_get = nullptr;
    decltype(&sws_scale) sws_scale_fn = nullptr;
    decltype(&sws_freeContext) sws_free = nullptr;
};

const Av& av() {
    static const Av loaded = [] {
        Av a;
        const std::string codec_name =
            "libavcodec.so." + std::to_string(LIBAVCODEC_VERSION_MAJOR);
        const std::string util_name = "libavutil.so." + std::to_string(LIBAVUTIL_VERSION_MAJOR);
        void* util = ::dlopen(util_name.c_str(), RTLD_NOW | RTLD_LOCAL);
        void* codec = util != nullptr ? ::dlopen(codec_name.c_str(), RTLD_NOW | RTLD_LOCAL)
                                      : nullptr;
        if (codec == nullptr) {
            std::printf("stud-render-host: video: %s not found, the engine gets no video "
                        "decoders\n",
                        util == nullptr ? util_name.c_str() : codec_name.c_str());
            std::fflush(stdout);
            return a;
        }
#define STUD_AV(field, lib, name) \
    a.field = reinterpret_cast<decltype(a.field)>(::dlsym(lib, #name))
        STUD_AV(find_decoder, codec, avcodec_find_decoder);
        STUD_AV(alloc_context, codec, avcodec_alloc_context3);
        STUD_AV(open, codec, avcodec_open2);
        STUD_AV(free_context, codec, avcodec_free_context);
        STUD_AV(send_packet, codec, avcodec_send_packet);
        STUD_AV(receive_frame, codec, avcodec_receive_frame);
        STUD_AV(flush, codec, avcodec_flush_buffers);
        STUD_AV(packet_alloc, codec, av_packet_alloc);
        STUD_AV(packet_free, codec, av_packet_free);
        STUD_AV(frame_alloc, util, av_frame_alloc);
        STUD_AV(frame_free, util, av_frame_free);
        STUD_AV(frame_unref, util, av_frame_unref);
        STUD_AV(mallocz, util, av_mallocz);
        STUD_AV(find_encoder_by_name, codec, avcodec_find_encoder_by_name);
        STUD_AV(send_frame, codec, avcodec_send_frame);
        STUD_AV(receive_packet, codec, avcodec_receive_packet);
        STUD_AV(supported_config, codec, avcodec_get_supported_config);
        STUD_AV(packet_unref, codec, av_packet_unref);
        STUD_AV(frame_get_buffer, util, av_frame_get_buffer);
        STUD_AV(hwdevice_create, util, av_hwdevice_ctx_create);
        STUD_AV(hwframe_ctx_alloc, util, av_hwframe_ctx_alloc);
        STUD_AV(hwframe_ctx_init, util, av_hwframe_ctx_init);
        STUD_AV(hwframe_get_buffer, util, av_hwframe_get_buffer);
        STUD_AV(hwframe_transfer, util, av_hwframe_transfer_data);
        STUD_AV(buffer_ref, util, av_buffer_ref);
        STUD_AV(buffer_unref, util, av_buffer_unref);
        a.hw_ok = a.hwdevice_create && a.hwframe_ctx_alloc && a.hwframe_ctx_init &&
                  a.hwframe_get_buffer && a.hwframe_transfer && a.buffer_ref && a.buffer_unref;
        const std::string sws_name =
            "libswscale.so." + std::to_string(LIBSWSCALE_VERSION_MAJOR);
        if (void* sws = ::dlopen(sws_name.c_str(), RTLD_NOW | RTLD_LOCAL)) {
            STUD_AV(sws_get, sws, sws_getCachedContext);
            STUD_AV(sws_scale_fn, sws, sws_scale);
            STUD_AV(sws_free, sws, sws_freeContext);
            a.sws_ok = a.sws_get && a.sws_scale_fn && a.sws_free;
        }
#undef STUD_AV
        a.ok = a.find_decoder && a.alloc_context && a.open && a.free_context &&
               a.send_packet && a.receive_frame && a.flush && a.packet_alloc &&
               a.packet_free && a.frame_alloc && a.frame_free && a.frame_unref && a.mallocz;
        std::printf("stud-render-host: video: %s %s, %s\n", codec_name.c_str(),
                    a.ok ? "loaded" : "is missing entry points, no video decoders",
                    a.sws_ok ? "libswscale loaded" : "no libswscale (8-bit 4:2:0 only)");
        std::fflush(stdout);
        return a;
    }();
    return loaded;
}

// Android's MIME names for what the engine asks for.
AVCodecID codec_for(const char* mime) {
    if (mime == nullptr) return AV_CODEC_ID_NONE;
    const std::string m(mime);
    if (m == "video/avc") return AV_CODEC_ID_H264;
    if (m == "video/hevc") return AV_CODEC_ID_HEVC;
    if (m == "video/x-vnd.on2.vp8") return AV_CODEC_ID_VP8;
    if (m == "video/x-vnd.on2.vp9") return AV_CODEC_ID_VP9;
    if (m == "video/av01") return AV_CODEC_ID_AV1;
    return AV_CODEC_ID_NONE;
}

struct Decoder {
    AVCodecContext* ctx = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    const AVCodec* codec = nullptr;
    bool opened = false;
    // Frames taken out of FFmpeg to make room for a packet, in order,
    // ahead of anything still inside it.
    std::deque<std::vector<uint8_t>> ready;
    SwsContext* sws = nullptr;  // for anything that is not already 8-bit 4:2:0
    std::mutex mutex;  // one engine thread feeds, another may drain
};

std::mutex& table_mutex() {
    static std::mutex m;
    return m;
}

std::map<uint64_t, std::shared_ptr<Decoder>>& decoders() {
    static std::map<uint64_t, std::shared_ptr<Decoder>> m;
    return m;
}

std::shared_ptr<Decoder> find(uint64_t handle) {
    std::lock_guard<std::mutex> lock(table_mutex());
    auto it = decoders().find(handle);
    return it == decoders().end() ? nullptr : it->second;
}

// Copies a decoded frame out as NV12, the layout Android decoders most
// commonly produce (COLOR_FormatYUV420SemiPlanar). 8-bit 4:2:0 is copied
// directly: planar (YUV420P, and its full-range JPEG spelling) interleaved
// here, NV12 as it is. Anything else (10-bit, 4:2:2, 4:4:4) is converted by
// libswscale into the same layout, through `*sws`, which is kept between
// frames; without libswscale it is refused rather than mislabelled.
int32_t write_nv12(const AVFrame* f, std::vector<uint8_t>& out, uint32_t* out_len,
                   SwsContext** sws) {
    const auto format = static_cast<AVPixelFormat>(f->format);
    const bool planar = format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P;
    const bool nv12 = format == AV_PIX_FMT_NV12;
    if (!planar && !nv12 && !av().sws_ok) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::printf("stud-render-host: video: pixel format %d needs libswscale, which is not "
                        "installed; frame dropped\n",
                        static_cast<int>(format));
            std::fflush(stdout);
        }
        return kErrorUnsupported;
    }
    const uint32_t width = static_cast<uint32_t>(f->width);
    const uint32_t height = static_cast<uint32_t>(f->height);
    const uint32_t stride = (width + 15u) & ~15u;
    const uint32_t slice = (height + 1u) & ~1u;
    const uint32_t y_bytes = stride * slice;
    const uint32_t uv_bytes = stride * (slice / 2);
    VideoFrameHeader h{};
    h.width = width;
    h.height = height;
    h.stride = stride;
    h.slice_height = slice;
    h.crop_left = 0;
    h.crop_top = 0;
    h.crop_right = width - 1;
    h.crop_bottom = height - 1;
    h.pts_us = f->pts;
    h.data_size = y_bytes + uv_bytes;
    out.assign(sizeof(h) + h.data_size, 0);
    std::memcpy(out.data(), &h, sizeof(h));
    uint8_t* y = out.data() + sizeof(h);
    uint8_t* uv = y + y_bytes;
    if (!planar && !nv12) {
        // The same context for every frame of a stream: the cached getter
        // rebuilds it only when the format or size changes.
        *sws = av().sws_get(*sws, f->width, f->height, format, f->width, f->height,
                            AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (*sws == nullptr) return kErrorUnsupported;
        uint8_t* const dst[4] = {y, uv, nullptr, nullptr};
        const int dst_stride[4] = {static_cast<int>(stride), static_cast<int>(stride), 0, 0};
        av().sws_scale_fn(*sws, f->data, f->linesize, 0, f->height, dst, dst_stride);
        *out_len = static_cast<uint32_t>(out.size());
        return 1;
    }
    for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(y + static_cast<size_t>(row) * stride,
                    f->data[0] + static_cast<ptrdiff_t>(row) * f->linesize[0], width);
    }
    const uint32_t chroma_w = (width + 1u) / 2u;
    const uint32_t chroma_h = (height + 1u) / 2u;
    for (uint32_t row = 0; row < chroma_h; ++row) {
        uint8_t* dst = uv + static_cast<size_t>(row) * stride;
        if (nv12) {
            std::memcpy(dst, f->data[1] + static_cast<ptrdiff_t>(row) * f->linesize[1],
                        chroma_w * 2u);
        } else {
            const uint8_t* u = f->data[1] + static_cast<ptrdiff_t>(row) * f->linesize[1];
            const uint8_t* v = f->data[2] + static_cast<ptrdiff_t>(row) * f->linesize[2];
            for (uint32_t x = 0; x < chroma_w; ++x) {
                dst[2 * x] = u[x];
                dst[2 * x + 1] = v[x];
            }
        }
    }
    *out_len = static_cast<uint32_t>(out.size());
    return 1;
}

#endif  // STUD_HAVE_FFMPEG

}  // namespace

#if defined(STUD_HAVE_FFMPEG)

bool video_decoder_supported(const char* mime) {
    const AVCodecID id = codec_for(mime);
    return id != AV_CODEC_ID_NONE && av().ok && av().find_decoder(id) != nullptr;
}

uint64_t video_decoder_create(const char* mime) {
    if (!video_decoder_supported(mime)) return 0;
    auto d = std::make_shared<Decoder>();
    d->codec = av().find_decoder(codec_for(mime));
    d->ctx = av().alloc_context(d->codec);
    d->packet = av().packet_alloc();
    d->frame = av().frame_alloc();
    if (d->ctx == nullptr || d->packet == nullptr || d->frame == nullptr) return 0;
    // All cores: a desktop has them, and a phone's hardware decoder is
    // what the engine's pacing assumes.
    d->ctx->thread_count = 0;
    static uint64_t next = 0;
    std::lock_guard<std::mutex> lock(table_mutex());
    const uint64_t handle = ++next;
    decoders()[handle] = std::move(d);
    std::printf("stud-render-host: video: decoder %llu for %s\n",
                static_cast<unsigned long long>(handle), mime);
    std::fflush(stdout);
    return handle;
}

int32_t video_decoder_configure(uint64_t handle, uint32_t width, uint32_t height,
                                const uint8_t* csd, uint32_t csd_size) {
    auto d = find(handle);
    if (!d) return kErrorUnknown;
    std::lock_guard<std::mutex> lock(d->mutex);
    if (d->opened) return kOk;
    d->ctx->width = static_cast<int>(width);
    d->ctx->height = static_cast<int>(height);
    if (csd != nullptr && csd_size > 0) {
        // Android's csd buffers are the codec's out-of-band configuration
        // (SPS/PPS for H.264 and HEVC, in Annex B), which is what FFmpeg
        // calls extradata. It must be padded and owned by FFmpeg.
        d->ctx->extradata =
            static_cast<uint8_t*>(av().mallocz(csd_size + AV_INPUT_BUFFER_PADDING_SIZE));
        if (d->ctx->extradata == nullptr) return kErrorUnknown;
        std::memcpy(d->ctx->extradata, csd, csd_size);
        d->ctx->extradata_size = static_cast<int>(csd_size);
    }
    if (av().open(d->ctx, d->codec, nullptr) < 0) return kErrorUnknown;
    d->opened = true;
    return kOk;
}

int32_t video_decoder_queue(uint64_t handle, const uint8_t* data, uint32_t size, int64_t pts_us,
                            bool end_of_stream) {
    auto d = find(handle);
    if (!d) return kErrorUnknown;
    std::lock_guard<std::mutex> lock(d->mutex);
    if (!d->opened) return kErrorUnknown;
    if (size > 0 && data != nullptr) {
        // FFmpeg reads past the end of a packet by up to the padding, so it
        // gets its own padded copy rather than the wire's buffer.
        std::vector<uint8_t> padded(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        std::memcpy(padded.data(), data, size);
        d->packet->data = padded.data();
        d->packet->size = static_cast<int>(size);
        d->packet->pts = pts_us;
        // EAGAIN means frames are waiting to be taken first. Android's
        // contract is that a queued buffer was accepted, so the frames are
        // moved to `ready` and the packet sent again, never dropped.
        int r = av().send_packet(d->ctx, d->packet);
        while (r == AVERROR(EAGAIN)) {
            if (av().receive_frame(d->ctx, d->frame) < 0) break;
            std::vector<uint8_t> frame;
            uint32_t len = 0;
            if (write_nv12(d->frame, frame, &len, &d->sws) == 1) d->ready.push_back(std::move(frame));
            av().frame_unref(d->frame);
            r = av().send_packet(d->ctx, d->packet);
        }
        d->packet->data = nullptr;
        d->packet->size = 0;
        if (r < 0) return kErrorUnknown;
    }
    if (end_of_stream) av().send_packet(d->ctx, nullptr);
    return kOk;
}

int32_t video_decoder_dequeue(uint64_t handle, std::vector<uint8_t>& out, uint32_t* out_len) {
    auto d = find(handle);
    if (!d) return -1;
    std::lock_guard<std::mutex> lock(d->mutex);
    if (!d->opened) return 0;
    if (!d->ready.empty()) {
        out = std::move(d->ready.front());
        d->ready.pop_front();
        *out_len = static_cast<uint32_t>(out.size());
        return 1;
    }
    const int r = av().receive_frame(d->ctx, d->frame);
    if (r == AVERROR(EAGAIN)) return 0;
    if (r == AVERROR_EOF) return 2;
    if (r < 0) return -1;
    const int32_t written = write_nv12(d->frame, out, out_len, &d->sws);
    av().frame_unref(d->frame);
    return written;
}

void video_decoder_flush(uint64_t handle) {
    auto d = find(handle);
    if (!d) return;
    std::lock_guard<std::mutex> lock(d->mutex);
    d->ready.clear();
    if (d->opened) av().flush(d->ctx);
}

void video_decoder_destroy(uint64_t handle) {
    std::shared_ptr<Decoder> d;
    {
        std::lock_guard<std::mutex> lock(table_mutex());
        auto it = decoders().find(handle);
        if (it == decoders().end()) return;
        d = it->second;
        decoders().erase(it);
    }
    std::lock_guard<std::mutex> lock(d->mutex);
    if (d->sws != nullptr) av().sws_free(d->sws);
    av().frame_free(&d->frame);
    av().packet_free(&d->packet);
    av().free_context(&d->ctx);
}


// ---- Encoding ----

namespace {

struct Candidate {
    const char* name;
    bool hardware;
    // AV_HWDEVICE_TYPE_NONE for an encoder that takes frames from system
    // memory. Otherwise the kind of device whose frames it wants: the
    // engine's frame is uploaded into one of that device's frames first.
    AVHWDeviceType device = AV_HWDEVICE_TYPE_NONE;
    AVPixelFormat device_format = AV_PIX_FMT_NONE;
    // Which of the device kind's devices, as FFmpeg names them; empty for
    // its default. Filled in by the probe, which may have to look past the
    // default to find one that encodes.
    std::string device_name;
};

// In the order tried, the first that opens wins. NVENC, AMF and Quick
// Sync take system-memory frames directly. VA-API and Vulkan take only
// frames on their own device, so each frame is uploaded (one copy, the
// same one the others make inside their own driver); they are what an
// Intel or AMD machine without AMF or Quick Sync has, and before them
// such a machine fell straight through to the software encoder.
std::vector<Candidate> all_candidates_for(const char* mime);

// STUD_VIDEO_ENCODER=<name> (hevc_vaapi, h264_vulkan, libx264, ...) keeps
// only that encoder, for testing a path the machine would not otherwise
// reach, or for skipping one that opens but misbehaves.
std::vector<Candidate> candidates_for(const char* mime) {
    std::vector<Candidate> all = all_candidates_for(mime);
    const char* only = std::getenv("STUD_VIDEO_ENCODER");
    if (only == nullptr || *only == '\0') return all;
    std::vector<Candidate> kept;
    for (const Candidate& c : all) {
        if (std::strcmp(c.name, only) == 0) kept.push_back(c);
    }
    return kept;
}

std::vector<Candidate> all_candidates_for(const char* mime) {
    const std::string m = mime != nullptr ? mime : "";
    if (m == "video/hevc") {
        return {{"hevc_nvenc", true},
                {"hevc_amf", true},
                {"hevc_qsv", true},
                {"hevc_vaapi", true, AV_HWDEVICE_TYPE_VAAPI, AV_PIX_FMT_VAAPI},
                {"hevc_vulkan", true, AV_HWDEVICE_TYPE_VULKAN, AV_PIX_FMT_VULKAN},
                {"libx265", false}};
    }
    if (m == "video/avc") {
        return {{"h264_nvenc", true},
                {"h264_amf", true},
                {"h264_qsv", true},
                {"h264_vaapi", true, AV_HWDEVICE_TYPE_VAAPI, AV_PIX_FMT_VAAPI},
                {"h264_vulkan", true, AV_HWDEVICE_TYPE_VULKAN, AV_PIX_FMT_VULKAN},
                {"libx264", false}};
    }
    return {};
}

// The pixel format this encoder will be fed: NV12 where it takes it, since
// that is what the engine most often sends, and 8-bit planar otherwise.
AVPixelFormat encoder_pixel_format(const AVCodec* codec, AVCodecContext* ctx) {
    const void* configs = nullptr;
    int count = 0;
    if (av().supported_config(ctx, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &count) < 0 ||
        configs == nullptr) {
        return AV_PIX_FMT_YUV420P;
    }
    const auto* formats = static_cast<const AVPixelFormat*>(configs);
    for (int i = 0; i < count; ++i) {
        if (formats[i] == AV_PIX_FMT_NV12) return AV_PIX_FMT_NV12;
    }
    for (int i = 0; i < count; ++i) {
        if (formats[i] == AV_PIX_FMT_YUV420P) return AV_PIX_FMT_YUV420P;
    }
    return count > 0 ? formats[0] : AV_PIX_FMT_YUV420P;
}

// A pool of `c.device` frames of this size, NV12 underneath, for an
// encoder that takes only device frames. Null where the device cannot be
// opened (no such GPU, no driver), which rules the encoder out.
AVBufferRef* device_frames_for(const Candidate& c, uint32_t width, uint32_t height) {
    if (c.device == AV_HWDEVICE_TYPE_NONE || !av().hw_ok) return nullptr;
    AVBufferRef* device = nullptr;
    if (av().hwdevice_create(&device, c.device,
                             c.device_name.empty() ? nullptr : c.device_name.c_str(), nullptr,
                             0) < 0) {
        return nullptr;
    }
    AVBufferRef* frames = av().hwframe_ctx_alloc(device);
    av().buffer_unref(&device);  // the frames context holds its own reference
    if (frames == nullptr) return nullptr;
    auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
    fc->format = c.device_format;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = static_cast<int>(width);
    fc->height = static_cast<int>(height);
    fc->initial_pool_size = 16;
    if (av().hwframe_ctx_init(frames) < 0) {
        av().buffer_unref(&frames);
        return nullptr;
    }
    return frames;
}

// `device_frames`, when given, is where this encoder's frames live; the
// context takes its own reference.
AVCodecContext* open_encoder(const AVCodec* codec, uint32_t width, uint32_t height,
                             uint32_t bit_rate, uint32_t frame_rate_milli,
                             uint32_t key_interval_ms, AVBufferRef* device_frames = nullptr) {
    AVCodecContext* ctx = av().alloc_context(codec);
    if (ctx == nullptr) return nullptr;
    const uint32_t fps_milli = frame_rate_milli != 0 ? frame_rate_milli : 30000;
    ctx->width = static_cast<int>(width);
    ctx->height = static_cast<int>(height);
    ctx->time_base = AVRational{1, 1000000};  // pts are microseconds, as Android's
    ctx->framerate = AVRational{static_cast<int>(fps_milli), 1000};
    ctx->bit_rate = bit_rate != 0 ? bit_rate : 4000000;
    // Android's i-frame-interval, in seconds: 0 is every frame a key frame.
    ctx->gop_size = key_interval_ms == 0
                        ? 1
                        : std::max(1, static_cast<int>(static_cast<uint64_t>(fps_milli) *
                                                       key_interval_ms / 1000000u));
    // Frames come out in the order they went in, as MediaCodec's do by
    // default, and the configuration is out of band, for csd-0.
    ctx->max_b_frames = 0;
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (device_frames != nullptr) {
        ctx->pix_fmt =
            reinterpret_cast<const AVHWFramesContext*>(device_frames->data)->format;
        ctx->hw_frames_ctx = av().buffer_ref(device_frames);
        if (ctx->hw_frames_ctx == nullptr) {
            av().free_context(&ctx);
            return nullptr;
        }
    } else {
        ctx->pix_fmt = encoder_pixel_format(codec, ctx);
    }
    if (av().open(ctx, codec, nullptr) < 0) {
        av().free_context(&ctx);
        return nullptr;
    }
    return ctx;
}

struct EncoderSupport {
    VideoEncoderSupport support;
    const AVCodec* codec = nullptr;
    Candidate candidate{};
};

// Whether one of this MIME type's encoders opens here, found once: a
// hardware encoder is listed by every FFmpeg build and opens only on the
// hardware it drives.
EncoderSupport probe_encoder(const char* mime) {
    static std::mutex m;
    static std::map<std::string, EncoderSupport> cache;
    std::lock_guard<std::mutex> lock(m);
    const std::string key = mime != nullptr ? mime : "";
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    EncoderSupport found;
    // libswscale is what moves the engine's frame into the encoder's own,
    // so without it there is no encoding at all.
    if (av().ok && av().sws_ok && av().find_encoder_by_name != nullptr) {
        for (const Candidate& listed : candidates_for(mime)) {
            const AVCodec* codec = av().find_encoder_by_name(listed.name);
            if (codec == nullptr) continue;
            // Which devices to try. Vulkan's default is simply the first
            // GPU, and on a hybrid laptop that is the integrated one, which
            // may have no video-encode queue while the other GPU does; the
            // rest are tried by index. VA-API's default is the render node
            // the system picks, and there is nothing to enumerate.
            std::vector<std::string> devices = {""};
            if (listed.device == AV_HWDEVICE_TYPE_VULKAN) {
                devices = {"", "1", "2", "3"};
            }
            bool opened = false;
            Candidate c = listed;
            for (const std::string& device : devices) {
                c.device_name = device;
                AVBufferRef* frames = nullptr;
                if (c.device != AV_HWDEVICE_TYPE_NONE) {
                    frames = device_frames_for(c, 256, 256);
                    if (frames == nullptr) continue;
                }
                AVCodecContext* ctx =
                    open_encoder(codec, 256, 256, 1000000, 30000, 1000, frames);
                if (frames != nullptr) av().buffer_unref(&frames);
                if (ctx == nullptr) continue;
                av().free_context(&ctx);
                opened = true;
                break;
            }
            if (!opened) continue;
            found.support = {true, c.hardware};
            found.codec = codec;
            found.candidate = c;
            std::printf("stud-render-host: video: %s encoder is %s (%s)\n", key.c_str(), c.name,
                        c.hardware ? "hardware" : "software");
            std::fflush(stdout);
            break;
        }
    }
    cache[key] = found;
    return found;
}

struct Encoder {
    const AVCodec* codec = nullptr;
    Candidate candidate{};
    AVCodecContext* ctx = nullptr;
    // For an encoder that takes device frames: the pool, and the frame
    // each upload lands in. `frame` is then the NV12 staging frame.
    AVBufferRef* device_frames = nullptr;
    AVFrame* device_frame = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* sws = nullptr;
    AVPixelFormat input = AV_PIX_FMT_NV12;
    std::vector<uint8_t> config;
    bool config_sent = false;
    bool ended = false;
    std::deque<std::vector<uint8_t>> ready;
    std::mutex mutex;
};

std::map<uint64_t, std::shared_ptr<Encoder>>& encoders() {
    static std::map<uint64_t, std::shared_ptr<Encoder>> m;
    return m;
}

std::shared_ptr<Encoder> find_encoder(uint64_t handle) {
    std::lock_guard<std::mutex> lock(table_mutex());
    auto it = encoders().find(handle);
    return it == encoders().end() ? nullptr : it->second;
}

std::vector<uint8_t> packet_bytes(const AVPacket* p) {
    EncodedPacketHeader h{};
    h.pts_us = p->pts;
    h.flags = (p->flags & AV_PKT_FLAG_KEY) != 0 ? 1u : 0u;
    h.size = static_cast<uint32_t>(p->size);
    std::vector<uint8_t> out(sizeof(h) + h.size);
    std::memcpy(out.data(), &h, sizeof(h));
    std::memcpy(out.data() + sizeof(h), p->data, h.size);
    return out;
}

// Everything the encoder has finished, into `ready`. True at its end.
bool drain_encoder(Encoder& e) {
    for (;;) {
        const int r = av().receive_packet(e.ctx, e.packet);
        if (r == AVERROR(EAGAIN)) return false;
        if (r < 0) return true;  // AVERROR_EOF, or a failure that ends it
        e.ready.push_back(packet_bytes(e.packet));
        av().packet_unref(e.packet);
    }
}

}  // namespace

VideoEncoderSupport video_encoder_supported(const char* mime) {
    return probe_encoder(mime).support;
}

uint64_t video_encoder_create(const char* mime) {
    const EncoderSupport s = probe_encoder(mime);
    if (!s.support.supported) return 0;
    auto e = std::make_shared<Encoder>();
    e->codec = s.codec;
    e->candidate = s.candidate;
    e->frame = av().frame_alloc();
    e->packet = av().packet_alloc();
    if (e->frame == nullptr || e->packet == nullptr) return 0;
    static uint64_t next = 0;
    std::lock_guard<std::mutex> lock(table_mutex());
    const uint64_t handle = ++next;
    encoders()[handle] = std::move(e);
    return handle;
}

int32_t video_encoder_configure(uint64_t handle, uint32_t width, uint32_t height,
                                uint32_t bit_rate, uint32_t frame_rate_milli,
                                uint32_t key_interval_ms, uint32_t color_format) {
    auto e = find_encoder(handle);
    if (!e || width == 0 || height == 0) return kErrorUnknown;
    std::lock_guard<std::mutex> lock(e->mutex);
    if (e->ctx != nullptr) return kOk;
    constexpr uint32_t kNv12 = 21;
    constexpr uint32_t kI420 = 19;
    constexpr uint32_t kFlexible = 0x7F420888;
    if (color_format == kI420) {
        e->input = AV_PIX_FMT_YUV420P;
    } else if (color_format == kNv12 || color_format == kFlexible || color_format == 0) {
        e->input = AV_PIX_FMT_NV12;
    } else {
        std::printf("stud-render-host: video: encoder input colour format %u is not supported\n",
                    color_format);
        std::fflush(stdout);
        return kErrorUnsupported;
    }
    if (e->candidate.device != AV_HWDEVICE_TYPE_NONE) {
        e->device_frames = device_frames_for(e->candidate, width, height);
        e->device_frame = av().frame_alloc();
        if (e->device_frames == nullptr || e->device_frame == nullptr) return kErrorUnknown;
    }
    e->ctx = open_encoder(e->codec, width, height, bit_rate, frame_rate_milli, key_interval_ms,
                          e->device_frames);
    if (e->ctx == nullptr) return kErrorUnknown;
    // The frame the engine's bytes are converted into: the encoder's own
    // format, or NV12 on its way up to a device frame.
    e->frame->format = e->device_frames != nullptr ? AV_PIX_FMT_NV12 : e->ctx->pix_fmt;
    e->frame->width = e->ctx->width;
    e->frame->height = e->ctx->height;
    if (av().frame_get_buffer(e->frame, 0) < 0) return kErrorUnknown;
    if (e->ctx->extradata != nullptr && e->ctx->extradata_size > 0) {
        e->config.assign(e->ctx->extradata, e->ctx->extradata + e->ctx->extradata_size);
    }
    return kOk;
}

int32_t video_encoder_queue(uint64_t handle, const uint8_t* data, uint32_t size, int64_t pts_us,
                            bool end_of_stream) {
    auto e = find_encoder(handle);
    if (!e) return kErrorUnknown;
    std::lock_guard<std::mutex> lock(e->mutex);
    if (e->ctx == nullptr) return kErrorUnknown;
    const int w = e->ctx->width;
    const int h = e->ctx->height;
    const size_t y_bytes = static_cast<size_t>(w) * h;
    const size_t chroma = static_cast<size_t>((w + 1) / 2) * ((h + 1) / 2);
    if (data != nullptr && size >= y_bytes + 2 * chroma) {
        // The engine's frame, tightly packed, as the planes of its format.
        const uint8_t* src[4] = {data, data + y_bytes, nullptr, nullptr};
        int src_stride[4] = {w, 0, 0, 0};
        if (e->input == AV_PIX_FMT_NV12) {
            src_stride[1] = ((w + 1) / 2) * 2;
        } else {
            src[2] = data + y_bytes + chroma;
            src_stride[1] = (w + 1) / 2;
            src_stride[2] = (w + 1) / 2;
        }
        // Into the encoder's own frame, converting where it wants another
        // layout (libswscale copies when the two are the same).
        e->sws = av().sws_get(e->sws, w, h, e->input, w, h,
                              static_cast<AVPixelFormat>(e->frame->format), SWS_BILINEAR,
                              nullptr, nullptr, nullptr);
        if (e->sws == nullptr) return kErrorUnsupported;
        av().sws_scale_fn(e->sws, src, src_stride, 0, h, e->frame->data, e->frame->linesize);
        e->frame->pts = pts_us;
        AVFrame* sent = e->frame;
        if (e->device_frames != nullptr) {
            // Up to the device, into a frame from the encoder's own pool.
            av().frame_unref(e->device_frame);
            if (av().hwframe_get_buffer(e->device_frames, e->device_frame, 0) < 0 ||
                av().hwframe_transfer(e->device_frame, e->frame, 0) < 0) {
                return kErrorUnknown;
            }
            e->device_frame->pts = pts_us;
            sent = e->device_frame;
        }
        int r = av().send_frame(e->ctx, sent);
        while (r == AVERROR(EAGAIN)) {
            drain_encoder(*e);
            r = av().send_frame(e->ctx, sent);
        }
        if (e->device_frames != nullptr) av().frame_unref(e->device_frame);
        if (r < 0) return kErrorUnknown;
        drain_encoder(*e);
    }
    if (end_of_stream) {
        av().send_frame(e->ctx, nullptr);
        e->ended = drain_encoder(*e);
    }
    return kOk;
}

int32_t video_encoder_dequeue(uint64_t handle, std::vector<uint8_t>& out, uint32_t* out_len) {
    auto e = find_encoder(handle);
    if (!e) return -1;
    std::lock_guard<std::mutex> lock(e->mutex);
    if (e->ctx == nullptr) return 0;
    if (!e->config_sent && !e->config.empty()) {
        // Android hands the configuration over first, flagged as such.
        e->config_sent = true;
        EncodedPacketHeader h{0, 2u, static_cast<uint32_t>(e->config.size())};
        out.resize(sizeof(h) + e->config.size());
        std::memcpy(out.data(), &h, sizeof(h));
        std::memcpy(out.data() + sizeof(h), e->config.data(), e->config.size());
        *out_len = static_cast<uint32_t>(out.size());
        return 1;
    }
    if (e->ready.empty() && !e->ended) e->ended = drain_encoder(*e);
    if (!e->ready.empty()) {
        out = std::move(e->ready.front());
        e->ready.pop_front();
        *out_len = static_cast<uint32_t>(out.size());
        return 1;
    }
    return e->ended ? 2 : 0;
}

std::vector<uint8_t> video_encoder_config(uint64_t handle) {
    auto e = find_encoder(handle);
    if (!e) return {};
    std::lock_guard<std::mutex> lock(e->mutex);
    return e->config;
}

void video_encoder_destroy(uint64_t handle) {
    std::shared_ptr<Encoder> e;
    {
        std::lock_guard<std::mutex> lock(table_mutex());
        auto it = encoders().find(handle);
        if (it == encoders().end()) return;
        e = it->second;
        encoders().erase(it);
    }
    std::lock_guard<std::mutex> lock(e->mutex);
    if (e->sws != nullptr) av().sws_free(e->sws);
    av().frame_free(&e->frame);
    if (e->device_frame != nullptr) av().frame_free(&e->device_frame);
    av().packet_free(&e->packet);
    if (e->ctx != nullptr) av().free_context(&e->ctx);
    if (e->device_frames != nullptr) av().buffer_unref(&e->device_frames);
}

#else  // no FFmpeg headers at build time: no decoders, and says so.

bool video_decoder_supported(const char*) { return false; }
uint64_t video_decoder_create(const char*) { return 0; }
int32_t video_decoder_configure(uint64_t, uint32_t, uint32_t, const uint8_t*, uint32_t) {
    return kErrorUnsupported;
}
int32_t video_decoder_queue(uint64_t, const uint8_t*, uint32_t, int64_t, bool) {
    return kErrorUnsupported;
}
int32_t video_decoder_dequeue(uint64_t, std::vector<uint8_t>&, uint32_t*) { return -1; }
void video_decoder_flush(uint64_t) {}
void video_decoder_destroy(uint64_t) {}
VideoEncoderSupport video_encoder_supported(const char*) { return {}; }
uint64_t video_encoder_create(const char*) { return 0; }
int32_t video_encoder_configure(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                uint32_t) {
    return kErrorUnsupported;
}
int32_t video_encoder_queue(uint64_t, const uint8_t*, uint32_t, int64_t, bool) {
    return kErrorUnsupported;
}
int32_t video_encoder_dequeue(uint64_t, std::vector<uint8_t>&, uint32_t*) { return -1; }
std::vector<uint8_t> video_encoder_config(uint64_t) { return {}; }
void video_encoder_destroy(uint64_t) {}

#endif

}  // namespace stud::render_host
