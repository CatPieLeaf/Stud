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
// The colour encoder: the block's bounding box gives a first pair of
// endpoints, each texel is assigned the nearest step along that line, and
// then one least-squares solve moves the endpoints to where those
// assignments say they belong. No search over candidate endpoint pairs.
//
// What each step was measured to be worth, on the same image, against
// stb_dxt's 32.39 dB:
//
//   bounding box, nearest-palette indices        30.84 dB   2.6x decode
//   principal axis (power iteration) instead     32.07 dB   7.7x decode
//   bounding box + least-squares refinement      32.11 dB   5.2x decode
//
// So the principal axis is the better starting point and is not worth its
// cost: the refinement solves for the endpoints anyway, which is most of
// what the axis was buying. The two hot passes, the bounding box and
// the projection of 16 texels onto the line, are SSE2, since an RGBA
// texel is four bytes and a 16-byte load is four of them.
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
inline constexpr uint32_t kEncoderVersion = 3;

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
