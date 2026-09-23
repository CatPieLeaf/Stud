#pragma once

#include <cstdint>
#include <vector>

// Video decoding for the engine's AMediaCodec, on the system's FFmpeg.
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

}  // namespace stud::render_host
