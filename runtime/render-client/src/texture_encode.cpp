#include "texture_encode.h"

#include <cstdint>

#include "bc7enc.h"
#include "rgbcx.h"

namespace stud::texture_encode {
namespace {

// Both libraries build lookup tables once before anything may encode.
//
// A function-local static is initialised exactly once even when the
// decode worker pool calls in from several threads at the same time, and
// every thread that arrives during that initialisation waits for it.
// Compressing a block afterwards only reads those tables.
struct Tables {
    Tables() {
        bc7enc_compress_block_init();
        rgbcx::init();
    }
};
const Tables& tables() {
    static const Tables t;
    return t;
}

// BC7 settings, chosen by measurement rather than by reputation.
//
// Cost per 1024x1024 level, 65536 blocks, against the hand-written
// encoder this replaces at 30.6 ms:
//
//   mode 6 only, no least squares            56.6 ms
//   modes 5+6                                93.6 ms
//   modes 1+5+6, 4 partitions               134.4 ms   <- this
//   every mode, 16 partitions               282.4 ms
//
// Mode 1 is the one that matters: it is a two-subset mode, so a 4x4
// block holding two distinct colours is no longer forced onto a single
// line. That is what a visible square on a normal map is. Mode 5 carries
// alpha in its own index set, for blocks where alpha does not follow
// colour. Mode 6 handles everything smooth, which is most blocks.
//
// Partitions stay at 4. Measured on real dumped textures, raising the
// search past 16 changed nothing (53.24 dB against 53.27 dB), so the
// budget goes to having the right modes rather than to searching more
// layouts of one of them.
//
// This is not free: it is roughly four times the cost of the encoder it
// replaces, paid once per texture and then served from the texture
// cache. An earlier attempt shipped 282 ms a level and made loading
// unplayable, which is why every number above is measured.
const bc7enc_compress_block_params& bc7_params() {
    static const bc7enc_compress_block_params p = [] {
        bc7enc_compress_block_params v;
        bc7enc_compress_block_params_init(&v);
        v.m_mode_mask = (1u << 1) | (1u << 5) | (1u << 6);
        v.m_max_partitions = 4;
        v.m_uber_level = 0;
        return v;
    }();
    return p;
}

// rgbcx's quality level for the BC1 and BC3 colour halves.
//
// Level 5 costs 13.7 ms a level against the hand-written encoder's
// 11.7 ms. Level 0 is 9.6 ms and cheaper than what it replaces; level 10
// jumps to 112.9 ms and level 18 to 396.2 ms, which is the shape of
// rgbcx's ordering search and not worth it here.
constexpr uint32_t kBc1Level = 5;

// 16-bit to 8-bit, rounded rather than truncated.
//
// detex hands EAC's 11 bits back replicated across all 16 bits
// ((value << 5) | (value >> 6)), so a plain `v >> 8` is a systematic
// downward bias of up to a whole 8-bit step, landing on every texel of a
// smooth gradient at once and moving where the eight BC4 steps fall. On
// a gentle ramp that bias was the entire error.
inline uint8_t narrow16(uint16_t v) {
    return static_cast<uint8_t>((static_cast<uint32_t>(v) * 255u + 32767u) / 65535u);
}

}  // namespace

void bc1_block(const uint8_t* rgba, uint8_t* out) {
    tables();
    // Opaque: no three-colour mode, so every texel gets one of four.
    rgbcx::encode_bc1(kBc1Level, out, rgba, false, false);
}

void bc1_block_punchthrough(const uint8_t* rgba, uint8_t* out) {
    tables();
    // ETC2's punch-through alpha maps onto BC1's own three-colour mode,
    // where the fourth index is transparent black.
    rgbcx::encode_bc1(kBc1Level, out, rgba, true, true);
}

void bc3_block(const uint8_t* rgba, uint8_t* out) {
    tables();
    rgbcx::encode_bc3(kBc1Level, out, rgba);
}

void bc4_block_from_r16(const uint16_t* r16, uint8_t* out) {
    tables();
    uint8_t v[16];
    for (int i = 0; i < 16; ++i) v[i] = narrow16(r16[i]);
    rgbcx::encode_bc4(out, v, 1);
}

void bc5_block_from_rg16(const uint16_t* rg16, uint8_t* out) {
    tables();
    uint8_t v[32];
    for (int i = 0; i < 16; ++i) {
        v[i * 2 + 0] = narrow16(rg16[i * 2 + 0]);
        v[i * 2 + 1] = narrow16(rg16[i * 2 + 1]);
    }
    rgbcx::encode_bc5(out, v, 0, 1, 2);
}

void bc7_block(const uint8_t* rgba, uint8_t* out) {
    tables();
    bc7enc_compress_block(out, rgba, &bc7_params());
}

}  // namespace stud::texture_encode
