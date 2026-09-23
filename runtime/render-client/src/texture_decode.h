#pragma once

// Software ETC1/ETC2/EAC and PVRTC support for GPUs that have none.
//
// Roblox is an Android application, and its texture pipeline transcodes
// into whatever the device says it can sample. On this desktop the answer
// differs sharply by GPU, and it is visible: measured directly, the same
// engine build on the same machine reports
//
//   Intel Iris Xe          Caps: Texture: DXT 1 PVR 0 ETC1 1 ETC2 1
//   NVIDIA RTX 3050        Caps: Texture: DXT 1 PVR 0 ETC1 0 ETC2 0
//
// and the textures are noticeably better in the first case, confirmed
// side by side by running Stud on each. NVIDIA genuinely has no ETC2 at
// all (probed directly: textureCompressionETC2 is false and every
// ETC2/EAC format reports optimalTilingFeatures 0x0), so the engine falls
// back to a lower-quality path there.
//
// Rather than accept a worse picture on the GPU most likely to be
// running Stud, the formats are supported here: the client reports them
// as available, substitutes an uncompressed image at creation, and
// decodes each upload on the way through. The decode costs memory (RGBA8
// is 8x an ETC2 RGB block) and a little CPU per texture upload, which is
// a good trade for textures that are actually right.
//
// The block decoders themselves are vendored, not written here; see
// runtime/CMakeLists.txt. A hand-rolled block decoder that is subtly
// wrong yields textures that look plausible and are quietly corrupt,
// which is worse than not supporting the format.

#include <cstdint>

#include <vulkan/vulkan.h>

namespace stud::texture_decode {

// True for the compressed formats Stud decodes itself. Everything else,
// including BC/DXT, which desktop GPUs support natively, is left alone.
bool is_emulated(VkFormat format);

// True for the PVRTC formats. They cannot be decoded a band at a time,
// a PVRTC texel is interpolated from neighbouring blocks, so a partial
// decode would seam at every band edge.
bool is_pvrtc(VkFormat format);

// The uncompressed format an emulated one is stored as. Chosen to preserve
// what the source actually carries: colour formats become RGBA8 (UNORM or
// SRGB to match), and the EAC single/dual channel formats become 16-bit
// so their 11 bits of precision survive. Returns `format` unchanged when
// it is not emulated.
VkFormat substitute(VkFormat format);

// Bytes one decoded mip level occupies, tightly packed.
uint64_t decoded_size(VkFormat format, uint32_t width, uint32_t height);

// Bytes one compressed mip level occupies, tightly packed, the size the
// engine itself wrote into its staging buffer.
uint64_t encoded_size(VkFormat format, uint32_t width, uint32_t height);

// Decodes one mip level of one layer. `src` must hold at least
// encoded_size() bytes and `dst` at least decoded_size(). Returns false if
// the format is not one of the emulated ones, in which case nothing is
// written.
//
// `src_row_pitch` is the distance in bytes between vertically adjacent
// rows of blocks, for a source whose rows are not tightly packed (a copy
// with a bufferRowLength larger than the extent). Zero means tight, which
// is the ordinary case. PVRTC cannot be decoded from a padded source at
// all. Its texels interpolate across neighbouring blocks, so a
// non-tight pitch is refused rather than silently mis-decoded.
bool decode(VkFormat format, const void* src, uint32_t width, uint32_t height, void* dst,
            uint64_t src_row_pitch = 0);

// Where row `y` of a decoded level starts, in bytes from the start of
// that level.
uint64_t decoded_row_offset(VkFormat format, uint32_t width, uint32_t y);

// Distance in bytes between vertically adjacent rows of blocks when a copy
// gives an explicit row length in texels. Zero if the format is not
// emulated.
uint64_t row_pitch_for_texels(VkFormat format, uint32_t row_texels);

}  // namespace stud::texture_decode
