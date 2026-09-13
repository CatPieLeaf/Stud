#include "texture_decode.h"

#include <cstring>
#include <vector>

// detex decodes every ETC/EAC format Stud emulates. It replaced
// etcdec.h, which is a reference decoder written for clarity: measured on
// this machine the two agree byte for byte on every format here, and
// detex is 2.3x faster on the two colour formats a texture burst is
// actually made of. See runtime/CMakeLists.txt for how it is vendored.
extern "C" {
#include <detex.h>
}

#include <PVRTDecompress.h>

#include "texture_encode.h"

namespace stud::texture_decode {
namespace {

// How a format is laid out and what it decodes into. Everything the
// decoders need is here, so the switch below stays a table rather than a
// pile of special cases.
struct Layout {
    VkFormat substitute = VK_FORMAT_UNDEFINED;
    uint32_t block_w = 4;
    uint32_t block_h = 4;
    uint32_t block_bytes = 8;
    uint32_t texel_bytes = 4;
    enum Codec { kEtcRgb, kEtcRgbA1, kEacRgba, kEacR11, kEacRg11, kPvrtc4, kPvrtc2 } codec = kEtcRgb;
    // The BC format of the same size, and its block size. Undefined for
    // the PVRTC modes, whose 2bpp variant has no BC equivalent and which
    // no GPU on this machine reports anyway.
    VkFormat bc = VK_FORMAT_UNDEFINED;
    uint32_t bc_block_bytes = 0;
    // The higher-quality target for the colour formats: BC7 holds one
    // subset of RGBA endpoints at 7 bits with 4-bit indices, against
    // BC1's 5:6:5 and 2 bits. Exact parity for ETC2 RGBA8 (both 16 bytes
    // a block) and twice the size for ETC2 RGB8, which is the price of
    // normal maps that are not blocky.
    VkFormat bc7 = VK_FORMAT_UNDEFINED;
};

bool& bc7_enabled() {
    static bool on = true;
    return on;
}

bool& bc_enabled() {
    static bool on = false;
    return on;
}

// True when this format is stored as BC rather than uncompressed.
bool transcoding(const Layout& l) { return bc_enabled() && l.bc != VK_FORMAT_UNDEFINED; }

// Which compressed format this one actually becomes, and its block size.
struct Target {
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t block_bytes = 0;
};

Target target_for(const Layout& l) {
    if (!transcoding(l)) return {};
    if (bc7_enabled() && l.bc7 != VK_FORMAT_UNDEFINED) return {l.bc7, 16};
    return {l.bc, l.bc_block_bytes};
}

bool layout_for(VkFormat f, Layout& out) {
    switch (f) {
        // ETC1 has no Vulkan format of its own: it is a strict subset of
        // ETC2 RGB, and a decoder for the latter decodes the former.
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
            out = {VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 8, 4, Layout::kEtcRgb,
                   VK_FORMAT_BC1_RGB_UNORM_BLOCK, 8, VK_FORMAT_BC7_UNORM_BLOCK};
            return true;
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
            out = {VK_FORMAT_R8G8B8A8_SRGB, 4, 4, 8, 4, Layout::kEtcRgb,
                   VK_FORMAT_BC1_RGB_SRGB_BLOCK, 8, VK_FORMAT_BC7_SRGB_BLOCK};
            return true;
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
            out = {VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 8, 4, Layout::kEtcRgbA1,
                   VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, VK_FORMAT_BC7_UNORM_BLOCK};
            return true;
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
            out = {VK_FORMAT_R8G8B8A8_SRGB, 4, 4, 8, 4, Layout::kEtcRgbA1,
                   VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 8, VK_FORMAT_BC7_SRGB_BLOCK};
            return true;
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
            out = {VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 16, 4, Layout::kEacRgba,
                   VK_FORMAT_BC3_UNORM_BLOCK, 16, VK_FORMAT_BC7_UNORM_BLOCK};
            return true;
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
            out = {VK_FORMAT_R8G8B8A8_SRGB, 4, 4, 16, 4, Layout::kEacRgba,
                   VK_FORMAT_BC3_SRGB_BLOCK, 16, VK_FORMAT_BC7_SRGB_BLOCK};
            return true;
        // EAC carries 11 bits per channel, so it decodes to 16-bit rather
        // than 8 -- rounding it to a byte would throw away precision the
        // source actually has. The signed variants are deliberately absent:
        // this decoder produces unsigned data, and claiming support for a
        // format whose sign it would silently lose is worse than saying no.
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
            out = {VK_FORMAT_R16_UNORM, 4, 4, 8, 2, Layout::kEacR11,
                   VK_FORMAT_BC4_UNORM_BLOCK, 8};
            return true;
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
            out = {VK_FORMAT_R16G16_UNORM, 4, 4, 16, 4, Layout::kEacRg11,
                   VK_FORMAT_BC5_UNORM_BLOCK, 16};
            return true;
        // PVRTC. The 4bpp modes cover 4x4 texels per 8-byte block and the
        // 2bpp modes 8x4, but neither decodes block-at-a-time: a PVRTC
        // texel is interpolated between neighbouring blocks, so the whole
        // level goes through the decoder at once.
        case VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG:
            out = {VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 8, 4, Layout::kPvrtc4};
            return true;
        case VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG:
            out = {VK_FORMAT_R8G8B8A8_SRGB, 4, 4, 8, 4, Layout::kPvrtc4};
            return true;
        case VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG:
            out = {VK_FORMAT_R8G8B8A8_UNORM, 8, 4, 8, 4, Layout::kPvrtc2};
            return true;
        case VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG:
            out = {VK_FORMAT_R8G8B8A8_SRGB, 8, 4, 8, 4, Layout::kPvrtc2};
            return true;
        default:
            return false;
    }
}

uint32_t blocks(uint32_t extent, uint32_t block) { return (extent + block - 1) / block; }

}  // namespace

bool is_emulated(VkFormat format) {
    Layout l;
    return layout_for(format, l);
}

bool is_pvrtc(VkFormat format) {
    Layout l;
    if (!layout_for(format, l)) return false;
    return l.codec == Layout::kPvrtc4 || l.codec == Layout::kPvrtc2;
}

void set_bc_available(bool available) { bc_enabled() = available; }
bool bc_available() { return bc_enabled(); }
void set_bc7_available(bool available) { bc7_enabled() = available; }

VkFormat substitute(VkFormat format) {
    Layout l;
    if (!layout_for(format, l)) return format;
    const Target t = target_for(l);
    return t.format != VK_FORMAT_UNDEFINED ? t.format : l.substitute;
}

uint64_t decoded_size(VkFormat format, uint32_t width, uint32_t height) {
    Layout l;
    if (!layout_for(format, l)) return 0;
    if (transcoding(l)) {
        // Blocks, not texels: what comes out is compressed too, and a
        // level narrower than one block still occupies a whole one.
        return static_cast<uint64_t>(blocks(width, 4)) * blocks(height, 4) *
               target_for(l).block_bytes;
    }
    return static_cast<uint64_t>(width) * height * l.texel_bytes;
}

uint64_t encoded_size(VkFormat format, uint32_t width, uint32_t height) {
    Layout l;
    if (!layout_for(format, l)) return 0;
    return static_cast<uint64_t>(blocks(width, l.block_w)) * blocks(height, l.block_h) *
           l.block_bytes;
}

uint64_t decoded_row_offset(VkFormat format, uint32_t width, uint32_t y) {
    Layout l;
    if (!layout_for(format, l)) return 0;
    if (transcoding(l)) {
        return static_cast<uint64_t>(y / 4) * blocks(width, 4) * target_for(l).block_bytes;
    }
    return static_cast<uint64_t>(y) * width * l.texel_bytes;
}

uint64_t row_pitch_for_texels(VkFormat format, uint32_t row_texels) {
    Layout l;
    if (!layout_for(format, l)) return 0;
    return static_cast<uint64_t>(blocks(row_texels, l.block_w)) * l.block_bytes;
}

bool decode(VkFormat format, const void* src, uint32_t width, uint32_t height, void* dst,
            uint64_t src_row_pitch) {
    Layout l;
    if (!layout_for(format, l) || src == nullptr || dst == nullptr || width == 0 || height == 0) {
        return false;
    }
    auto* out = static_cast<uint8_t*>(dst);
    const auto* in = static_cast<const uint8_t*>(src);

    if (l.codec == Layout::kPvrtc4 || l.codec == Layout::kPvrtc2) {
        // PVRTC interpolates across block boundaries, so the decoder takes
        // the whole level at once and always writes RGBA8. It has no way
        // to skip padding between rows, so a padded source is refused.
        if (src_row_pitch != 0 &&
            src_row_pitch != static_cast<uint64_t>(blocks(width, l.block_w)) * l.block_bytes) {
            return false;
        }
        pvr::PVRTDecompressPVRTC(in, l.codec == Layout::kPvrtc2 ? 1u : 0u, width, height, out);
        return true;
    }

    const uint32_t bw = blocks(width, l.block_w);
    const uint32_t bh = blocks(height, l.block_h);
    const uint32_t row_bytes = width * l.texel_bytes;
    const bool transcode = transcoding(l);
    const Target target = target_for(l);
    const bool use_bc7 = target.format == VK_FORMAT_BC7_UNORM_BLOCK ||
                         target.format == VK_FORMAT_BC7_SRGB_BLOCK;
    // Rows of blocks are tightly packed unless the copy said otherwise.
    const uint64_t block_row_pitch =
        src_row_pitch != 0 ? src_row_pitch : static_cast<uint64_t>(bw) * l.block_bytes;
    // Every block decodes into scratch and is copied out, so a block on
    // the right or bottom edge of a non-multiple-of-four level writes only
    // the part that exists. 4x4 texels at 4 bytes is the largest case any
    // of these codecs produces.
    uint8_t scratch[4 * 4 * 4];

    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* block =
                in + static_cast<uint64_t>(by) * block_row_pitch + static_cast<uint64_t>(bx) * l.block_bytes;
            const uint32_t x0 = bx * l.block_w;
            const uint32_t y0 = by * l.block_h;
            const uint32_t w = (x0 + l.block_w <= width) ? l.block_w : width - x0;
            const uint32_t h = (y0 + l.block_h <= height) ? l.block_h : height - y0;
            // detex always writes a tight 4x4 block, so a block that
            // lands whole inside the level is decoded into the scratch
            // and copied out in four rows -- cheap next to the decode,
            // and it keeps one code path for both cases.
            bool ok = false;
            switch (l.codec) {
                case Layout::kEtcRgb:
                    ok = detexDecompressBlockETC2(block, DETEX_MODE_MASK_ALL, 0, scratch);
                    break;
                case Layout::kEtcRgbA1:
                    ok = detexDecompressBlockETC2_PUNCHTHROUGH(block, DETEX_MODE_MASK_ALL, 0,
                                                               scratch);
                    break;
                case Layout::kEacRgba:
                    ok = detexDecompressBlockETC2_EAC(block, DETEX_MODE_MASK_ALL, 0, scratch);
                    break;
                case Layout::kEacR11:
                    ok = detexDecompressBlockEAC_R11(block, DETEX_MODE_MASK_ALL, 0, scratch);
                    break;
                case Layout::kEacRg11:
                    ok = detexDecompressBlockEAC_RG11(block, DETEX_MODE_MASK_ALL, 0, scratch);
                    break;
                default: return false;
            }
            // A block detex refuses is a block the engine should not have
            // produced. Zero is visibly wrong rather than random, which is
            // the honest answer for data this decoder cannot read.
            if (!ok) std::memset(scratch, 0, sizeof(scratch));

            if (transcode) {
                // Straight from the decoded block into the BC block of the
                // same size. Partial edge blocks need no special case: BC
                // is 4x4 too, so the whole block is written and the texels
                // past the level's edge are simply never sampled.
                uint8_t* dst_block =
                    out + (static_cast<uint64_t>(by) * bw + bx) * target.block_bytes;
                if (use_bc7) {
                    // One encoder for every colour format: BC7 mode 6
                    // carries alpha, so the punch-through and full-alpha
                    // cases need no separate path.
                    stud::texture_encode::bc7_mode6_block(scratch, dst_block);
                    continue;
                }
                switch (l.codec) {
                    case Layout::kEtcRgb:
                        stud::texture_encode::bc1_block(scratch, dst_block);
                        break;
                    case Layout::kEtcRgbA1:
                        stud::texture_encode::bc1_block_punchthrough(scratch, dst_block);
                        break;
                    case Layout::kEacRgba:
                        stud::texture_encode::bc3_block(scratch, dst_block);
                        break;
                    case Layout::kEacR11:
                        stud::texture_encode::bc4_block_from_r16(
                            reinterpret_cast<const uint16_t*>(scratch), dst_block);
                        break;
                    case Layout::kEacRg11:
                        stud::texture_encode::bc5_block_from_rg16(
                            reinterpret_cast<const uint16_t*>(scratch), dst_block);
                        break;
                    default: return false;
                }
                continue;
            }

            for (uint32_t y = 0; y < h; ++y) {
                std::memcpy(out + static_cast<uint64_t>(y0 + y) * row_bytes +
                                static_cast<uint64_t>(x0) * l.texel_bytes,
                            scratch + static_cast<uint64_t>(y) * l.block_w * l.texel_bytes,
                            static_cast<size_t>(w) * l.texel_bytes);
            }
        }
    }
    return true;
}

}  // namespace stud::texture_decode
