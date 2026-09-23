#pragma once

#include <cstdint>
#include <vector>

// Video decoding and encoding for the engine's AMediaCodec, on the system's
// FFmpeg.
//
// The engine plays video (VideoFrame, and whatever Home shows) through
// Android's MediaCodec: compressed packets in, raw YUV frames out, which it
// uploads itself. Process B's libmediandk forwards those calls here.
//
// The codecs are the distribution's: libavcodec is loaded at runtime by the
// exact major version these headers describe, so the structures read here
// always match the library, and a system without it, or with a build that
// leaves a codec out, simply reports that codec as unavailable, which is
// what the engine hears from a device without that decoder. Stud ships no
// codec of its own.
namespace stud::render_host {

// Whether a decoder for this Android MIME type ("video/avc", ...) exists.
bool video_decoder_supported(const char* mime);

// A decoder, or 0 when there is none for this MIME type.
uint64_t video_decoder_create(const char* mime);

// Codec-specific data (csd-0 followed by csd-1, as Android hands them over),
// and the dimensions from the input format. Returns an AMediaCodec status.
int32_t video_decoder_configure(uint64_t decoder, uint32_t width, uint32_t height,
                                const uint8_t* csd, uint32_t csd_size);

// One compressed access unit. `end_of_stream` drains the decoder.
int32_t video_decoder_queue(uint64_t decoder, const uint8_t* data, uint32_t size,
                            int64_t pts_us, bool end_of_stream);

// The next decoded frame, if one is ready, as the wire's VideoFrameHeader
// followed by NV12 data. Returns 1 for a frame, 0 for none yet, 2 at the
// end of the stream, negative on error.
int32_t video_decoder_dequeue(uint64_t decoder, std::vector<uint8_t>& out, uint32_t* out_len);

void video_decoder_flush(uint64_t decoder);
void video_decoder_destroy(uint64_t decoder);

// Encoding, for the engine's recording (its HevcMediaCodecEncoder).
//
// The first of FFmpeg's encoders for the type that opens on this machine:
// NVENC, AMF and Quick Sync, which take frames from system memory, then
// the software encoder. VA-API and Vulkan encoders need their frames on the
// GPU already and are not tried.

struct VideoEncoderSupport {
    bool supported = false;
    bool hardware = false;
};
VideoEncoderSupport video_encoder_supported(const char* mime);

uint64_t video_encoder_create(const char* mime);

// `color_format` is Android's: 21 (NV12), 19 (I420), or 0x7F420888
// (flexible, taken as NV12, Android's most common layout for it). Frames
// arrive tightly packed. Returns an AMediaCodec status.
int32_t video_encoder_configure(uint64_t encoder, uint32_t width, uint32_t height,
                                uint32_t bit_rate, uint32_t frame_rate_milli,
                                uint32_t key_interval_ms, uint32_t color_format);

// One raw frame; `end_of_stream` drains the encoder.
int32_t video_encoder_queue(uint64_t encoder, const uint8_t* data, uint32_t size, int64_t pts_us,
                            bool end_of_stream);

// The next packet, as the wire's EncodedPacketHeader followed by its bytes.
// The first is the codec configuration (flag 2). Returns 1 for a packet, 0
// for none yet, 2 at the end of the stream, negative on error.
int32_t video_encoder_dequeue(uint64_t encoder, std::vector<uint8_t>& out, uint32_t* out_len);

// The codec configuration (VPS/SPS/PPS), for the output format's csd-0.
std::vector<uint8_t> video_encoder_config(uint64_t encoder);

void video_encoder_destroy(uint64_t encoder);

}  // namespace stud::render_host
