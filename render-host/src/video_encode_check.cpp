// stud_video_encode_check: encodes a few seconds of synthetic frames
// through the same entry points the engine's AMediaCodec encoder reaches
// (video_encoder_*), and says which encoder took them and what came out.
// STUD_VIDEO_ENCODER picks one, so a path this machine would not choose on
// its own (VA-API, Vulkan) can be exercised.
//
//   stud_video_encode_check [video/hevc|video/avc] [width] [height]

#include "stud/render_host_protocol.h"
#include "stud/video_codec.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    using namespace stud::render_host;
    const std::string mime = argc > 1 ? argv[1] : "video/hevc";
    const uint32_t w = argc > 2 ? static_cast<uint32_t>(std::atoi(argv[2])) : 1280;
    const uint32_t h = argc > 3 ? static_cast<uint32_t>(std::atoi(argv[3])) : 720;
    const VideoEncoderSupport s = video_encoder_supported(mime.c_str());
    if (!s.supported) {
        std::printf("no %s encoder opens here\n", mime.c_str());
        return 1;
    }
    const uint64_t e = video_encoder_create(mime.c_str());
    if (e == 0 || video_encoder_configure(e, w, h, 8000000, 60000, 1000, 21) != 0) {
        std::printf("the %s encoder would not configure at %ux%u\n", mime.c_str(), w, h);
        return 1;
    }
    // NV12: a moving gradient, so the encoder has something to do.
    std::vector<uint8_t> frame(static_cast<size_t>(w) * h * 3 / 2);
    size_t packets = 0, bytes = 0, keys = 0, config = 0;
    std::vector<uint8_t> out;
    uint32_t len = 0;
    const int frames = 120;
    for (int i = 0; i < frames; ++i) {
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) frame[y * w + x] = static_cast<uint8_t>(x + y + i * 4);
        }
        for (size_t j = static_cast<size_t>(w) * h; j < frame.size(); ++j) {
            frame[j] = static_cast<uint8_t>(128 + (j + i) % 32);
        }
        if (video_encoder_queue(e, frame.data(), static_cast<uint32_t>(frame.size()),
                                static_cast<int64_t>(i) * 16667, i == frames - 1) != 0) {
            std::printf("queueing frame %d failed\n", i);
            return 1;
        }
        int32_t r;
        while ((r = video_encoder_dequeue(e, out, &len)) == 1) {
            const auto* hdr = reinterpret_cast<const EncodedPacketHeader*>(out.data());
            if (hdr->flags & 2u) {
                ++config;
            } else {
                ++packets;
                bytes += hdr->size;
                if (hdr->flags & 1u) ++keys;
            }
        }
    }
    int32_t r;
    while ((r = video_encoder_dequeue(e, out, &len)) == 1) {
        const auto* hdr = reinterpret_cast<const EncodedPacketHeader*>(out.data());
        if (!(hdr->flags & 2u)) {
            ++packets;
            bytes += hdr->size;
            if (hdr->flags & 1u) ++keys;
        }
    }
    video_encoder_destroy(e);
    std::printf("%s %ux%u: %d frames in, %zu packets out (%zu key, %zu config), %zu bytes, %s\n",
                mime.c_str(), w, h, frames, packets, keys, config, bytes,
                s.hardware ? "hardware" : "software");
    return packets > 0 ? 0 : 1;
}
