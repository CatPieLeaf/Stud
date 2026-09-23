#include "stud/video_decoder.h"

#include "stud/render_host_protocol.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#if defined(STUD_HAVE_FFMPEG)
#include <dlfcn.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
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
#undef STUD_AV
        a.ok = a.find_decoder && a.alloc_context && a.open && a.free_context &&
               a.send_packet && a.receive_frame && a.flush && a.packet_alloc &&
               a.packet_free && a.frame_alloc && a.frame_free && a.frame_unref && a.mallocz;
        std::printf("stud-render-host: video: %s %s\n", codec_name.c_str(),
                    a.ok ? "loaded" : "is missing entry points, no video decoders");
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
// commonly produce (COLOR_FormatYUV420SemiPlanar). 8-bit 4:2:0 only: planar
// (YUV420P, and its full-range JPEG spelling) is interleaved here, NV12 is
// copied as it is. Anything else is reported rather than passed through
// under the wrong label.
int32_t write_nv12(const AVFrame* f, std::vector<uint8_t>& out, uint32_t* out_len) {
    const auto format = static_cast<AVPixelFormat>(f->format);
    const bool planar = format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P;
    const bool nv12 = format == AV_PIX_FMT_NV12;
    if (!planar && !nv12) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::printf("stud-render-host: video: decoder produced pixel format %d, which is not "
                        "8-bit 4:2:0; frame dropped\n",
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
            if (write_nv12(d->frame, frame, &len) == 1) d->ready.push_back(std::move(frame));
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
    const int32_t written = write_nv12(d->frame, out, out_len);
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
    av().frame_free(&d->frame);
    av().packet_free(&d->packet);
    av().free_context(&d->ctx);
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

#endif

}  // namespace stud::render_host
