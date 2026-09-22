#pragma once

// BC block encoders, so an emulated texture costs what the engine thinks
// it costs.
//
// Why this exists, measured rather than assumed. NVIDIA has no ETC2, so
// Stud decodes every upload, and stored the result as RGBA8, which is
// 8x an ETC2 RGB block. The engine budgets texture memory against a
// compiled-in `caps.videoMemory` of 64MB (a bare immediate, with nothing feeding it), so 8x inflation leaves it an
// eighth of the room it expects. What that looks like on screen: joining
// a game, textures flick between full resolution and a low mip for a
// while and then stay low, because the streaming system loads a mip,
// runs out of budget, evicts it, and loads it again.
//
// Confirmed by direct comparison rather than reasoning: the same build,
// same account, same game on the Intel iGPU, which has ETC2 in
// hardware, so nothing is substituted and a texture costs its real 4
// bits per texel, keeps its full-resolution textures and never
// flickers, on a GPU that is far slower. Slower hardware ruling out the
// frame-rate explanation is what makes that test decisive.
//
// So the decoded block is re-encoded into the BC format of the same
// size, which every desktop GPU samples natively:
//
//     ETC2 RGB8   4 bpp -> BC1  4 bpp
//     ETC2 RGB8A1 4 bpp -> BC1  4 bpp (its own punch-through mode)
//     ETC2 RGBA8  8 bpp -> BC3  8 bpp
//     EAC R11     4 bpp -> BC4  4 bpp
//     EAC RG11    8 bpp -> BC5  8 bpp
//
// Byte-for-byte the same size as what the engine handed over, so its
// accounting is right again.
//
// The encoders themselves are bc7enc_rdo's, vendored at
// third_party/bc7enc: rgbcx for BC1/BC3/BC4/BC5 and bc7enc for BC7. Stud
// wrote its own once and they were measurably worse where it mattered:
// the BC7 one had no partitioned mode, so a 4x4 block holding two
// distinct colours was forced onto a single line and came out as a
// visible square on a normal map.
//
// Measured per 1024x1024 level, replaced -> replacement:
//
//   BC4   10.5 ms ->   2.4 ms
//   BC5   13.8 ms ->   4.8 ms
//   BC1   11.7 ms ->  13.7 ms
//   BC7   30.6 ms ->  51.5 ms
//
// Three of the four are cheaper. BC7 is not, and that is deliberate: the
// extra time buys the partitioned mode. See bc7_params() for the full
// settings table and why each one was picked.
//
// Transcoding one lossy format to another does lose something; a texture
// that stays at full resolution is worth more than one that is marginally
// better encoded and never loads.

#include <cstdint>

namespace stud::texture_encode {

// Bump whenever any encoder below changes what it writes.
//
// The texture cache keys on the source bytes and the formats, so without
// this a block encoded by an older, worse encoder would be served from
// disk forever and the change would never reach anything already played.
inline constexpr uint32_t kEncoderVersion = 10;

// Each takes one decoded 4x4 block, tightly packed, and writes one
// compressed block. `rgba` is 16 texels of 4 bytes; `rg16`/`r16` are 16
// texels of 16-bit channels.
void bc1_block(const uint8_t* rgba, uint8_t* out);           // 8 bytes
void bc1_block_punchthrough(const uint8_t* rgba, uint8_t* out);  // 8 bytes
void bc3_block(const uint8_t* rgba, uint8_t* out);           // 16 bytes
void bc4_block_from_r16(const uint16_t* r16, uint8_t* out);  // 8 bytes
void bc5_block_from_rg16(const uint16_t* rg16, uint8_t* out);  // 16 bytes

// BC7, whichever of mode 6 and mode 5 reconstructs the block closer.
//
// Why a second colour format at all: BC1 is 4 bits per texel and holds
// four colours per 4x4 block, which is the textbook worst case for a
// normal map. Smooth surface normals come out as blocky per-block bands,
// which is what "the normal maps look like pixelated noise" is.
//
// Mode 6 is one subset, RGBA endpoints at 7 bits plus a shared p-bit,
// and 4-bit indices: sixteen steps along the line where BC1 has four, at
// eight times its endpoint precision. It is the right mode whenever a
// block's four channels move together, which is most of them.
//
// Mode 5 is one subset with RGB at 7 bits and alpha at 8, each with its
// own 2-bit index set. Coarser steps, but the alpha indices are separate.
// Mode 6's single index per texel is shared by all four channels, so a
// texel cannot sit at one point along the colour line and a different
// point along alpha, and where alpha varies independently of colour that
// is unrepresentable at any endpoint precision. Measured on a block whose
// alpha and colour both vary non-linearly and independently, mode 6
// manages 12.53 dB with a worst-case error of 124 of 255. Soft-edged
// decals and cut-out masks are that shape.
void bc7_block(const uint8_t* rgba, uint8_t* out);  // 16 bytes

}  // namespace stud::texture_encode
