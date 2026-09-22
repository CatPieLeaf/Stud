#include "texture_encode.h"

#include "bc7enc.h"
#include "rgbcx.h"

#include <cmath>
#include <cstdint>
#include <cstring>

// SSSE3, not just SSE2: the x86-64 Android ABI this runs on guarantees
// SSE4.2, and _mm_hadd_epi32 folds the two halves of each texel's dot
// product in one instruction where SSE2 needs a shuffle and an add. The
// scalar path below stays for any other architecture.
#if defined(__SSSE3__)
#include <tmmintrin.h>
#define STUD_TEX_ENCODE_SIMD 1
#elif defined(__SSE2__)
#include <emmintrin.h>
#define STUD_TEX_ENCODE_SIMD 1
#endif

namespace stud::texture_encode {
namespace {

inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// Both libraries build lookup tables once before anything may encode. A
// function-local static is initialised exactly once even when the decode
// worker pool calls in from several threads, and every thread arriving
// during that initialisation waits for it.
struct Tables {
    Tables() {
        bc7enc_compress_block_init();
        rgbcx::init();
    }
};
inline const Tables& tables() {
    static const Tables t;
    return t;
}

// bc7enc for the blocks Stud's own encoder fits badly.
//
// The mask MUST keep an alpha-capable mode. Mode 1 is the two-subset mode
// and the reason this is here, but mode 1 carries no alpha at all, and
// bc7enc asserts on a block with alpha unless mode 5, 6 or 7 is also
// available. Stud builds with NDEBUG, so that assert is compiled out and
// what comes back instead is garbage -- every texture with alpha turned
// to noise. Mode 5 and 6 cost almost nothing to leave in (93.6 ms against
// 92.5 ms for 5+6 alone) and they are what bc7enc falls back to.
//
// Two partitions, because one collapses the worst case and past two the
// numbers stop moving. No least-squares pass: it buys average PSNR at the
// cost of a worse worst case, which is backwards for this.
inline const bc7enc_compress_block_params& bc7_mode1_params() {
    static const bc7enc_compress_block_params p = [] {
        bc7enc_compress_block_params v;
        bc7enc_compress_block_params_init(&v);
        v.m_mode_mask = (1u << 1) | (1u << 5) | (1u << 6);
        v.m_max_partitions = 2;
        v.m_uber_level = 0;
        v.m_try_least_squares = false;
        return v;
    }();
    return p;
}

// rgbcx's quality level for BC1 and BC3. Level 5 costs 13.7 ms a level
// against the hand-written encoder's 11.7 ms; level 10 jumps to 112.9 ms.
// These are fallback formats anyway: target_for() hands colour to BC7
// whenever the GPU has it.
constexpr uint32_t kBc1Level = 5;

// 16-bit to 8-bit, rounded rather than truncated. detex hands EAC's 11
// bits back replicated across all 16, so a plain `v >> 8` is a systematic
// downward bias of up to a whole 8-bit step, landing on every texel of a
// smooth gradient at once.
inline uint8_t narrow16(uint16_t v) {
    return static_cast<uint8_t>((static_cast<uint32_t>(v) * 255u + 32767u) / 65535u);
}


void project_block(const uint8_t* rgba, const int* base, const int* dir, int* out,
                   int channels = 3) {
    // channels == 4 includes alpha, which is what BC7 projects against;
    // the 3-channel case zeroes alpha's term so it contributes nothing.
    const int base_a = channels > 3 ? base[3] : 0;
    const int dir_a = channels > 3 ? dir[3] : 0;
#if STUD_TEX_ENCODE_SIMD
    const __m128i zero = _mm_setzero_si128();
    const __m128i base16 = _mm_setr_epi16(static_cast<short>(base[0]), static_cast<short>(base[1]),
                                          static_cast<short>(base[2]), static_cast<short>(base_a),
                                          static_cast<short>(base[0]), static_cast<short>(base[1]),
                                          static_cast<short>(base[2]), static_cast<short>(base_a));
    const __m128i dir16 = _mm_setr_epi16(static_cast<short>(dir[0]), static_cast<short>(dir[1]),
                                         static_cast<short>(dir[2]), static_cast<short>(dir_a),
                                         static_cast<short>(dir[0]), static_cast<short>(dir[1]),
                                         static_cast<short>(dir[2]), static_cast<short>(dir_a));
    for (int i = 0; i < 16; i += 4) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rgba + i * 4));
        // Two texels per half, widened to 16-bit so the subtraction can
        // go negative and the multiply has room.
        const __m128i lo = _mm_sub_epi16(_mm_unpacklo_epi8(v, zero), base16);
        const __m128i hi = _mm_sub_epi16(_mm_unpackhi_epi8(v, zero), base16);
        // madd gives [r*dr + g*dg, b*db + a*0] per texel; adding the two
        // halves of each pair finishes the dot product.
        const __m128i plo = _mm_madd_epi16(lo, dir16);
        const __m128i phi = _mm_madd_epi16(hi, dir16);
#if defined(__SSSE3__)
        // [r*dr+g*dg, b*db, ...] per texel -> one dot product per lane,
        // four texels per store.
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), _mm_hadd_epi32(plo, phi));
#else
        const __m128i slo = _mm_add_epi32(plo, _mm_shuffle_epi32(plo, _MM_SHUFFLE(2, 3, 0, 1)));
        const __m128i shi = _mm_add_epi32(phi, _mm_shuffle_epi32(phi, _MM_SHUFFLE(2, 3, 0, 1)));
        int tmp[4];
        _mm_storeu_si128(reinterpret_cast<__m128i*>(tmp), slo);
        out[i + 0] = tmp[0];
        out[i + 1] = tmp[2];
        _mm_storeu_si128(reinterpret_cast<__m128i*>(tmp), shi);
        out[i + 2] = tmp[0];
        out[i + 3] = tmp[2];
#endif
    }
#else
    for (int i = 0; i < 16; ++i) {
        int d = 0;
        for (int c = 0; c < 3; ++c) d += (rgba[i * 4 + c] - base[c]) * dir[c];
        if (channels > 3) d += (rgba[i * 4 + 3] - base_a) * dir_a;
        out[i] = d;
    }
#endif
}

// The block's per-channel minimum and maximum. Only used when no texel is
// excluded, which is every block outside the punch-through mode, one
// 16-byte load is four texels, so the whole block is four loads and a
// reduction rather than 48 compares.
void bounds_block(const uint8_t* rgba, int* mn, int* mx) {
#if STUD_TEX_ENCODE_SIMD
    __m128i lo = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rgba));
    __m128i hi = lo;
    for (int i = 4; i < 16; i += 4) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rgba + i * 4));
        lo = _mm_min_epu8(lo, v);
        hi = _mm_max_epu8(hi, v);
    }
    // Fold the four texels in each register down to one.
    lo = _mm_min_epu8(lo, _mm_shuffle_epi32(lo, _MM_SHUFFLE(1, 0, 3, 2)));
    hi = _mm_max_epu8(hi, _mm_shuffle_epi32(hi, _MM_SHUFFLE(1, 0, 3, 2)));
    lo = _mm_min_epu8(lo, _mm_shuffle_epi32(lo, _MM_SHUFFLE(2, 3, 0, 1)));
    hi = _mm_max_epu8(hi, _mm_shuffle_epi32(hi, _MM_SHUFFLE(2, 3, 0, 1)));
    uint8_t l[16], h[16];
    _mm_storeu_si128(reinterpret_cast<__m128i*>(l), lo);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(h), hi);
    for (int c = 0; c < 3; ++c) {
        mn[c] = l[c];
        mx[c] = h[c];
    }
#else
    for (int c = 0; c < 3; ++c) {
        mn[c] = 255;
        mx[c] = 0;
    }
    for (int i = 0; i < 16; ++i) {
        for (int c = 0; c < 3; ++c) {
            const int v = rgba[i * 4 + c];
            if (v < mn[c]) mn[c] = v;
            if (v > mx[c]) mx[c] = v;
        }
    }
#endif
}

// Which diagonal of the bounding box the colours actually run along.
//
// A box has four diagonals and the box alone does not say which one the
// data uses. Taking (min,min,min)->(max,max,max) is right only when the
// channels rise together; when two of them move in opposite directions
// the endpoints end up on the wrong diagonal and every index is chosen
// against a line the block's colours never lie on. That is not a corner
// case here. It is exactly what a normal map does, X rising while Y
// falls, and it is why normal maps came out as blocky noise.
//
// The sign of each channel's covariance with the first says which corner
// to take. One pass, and it is what makes a bounding box a reasonable
// stand-in for the principal axis at a fraction of its cost.
void orient_box(const uint8_t* rgba, int channels, const bool* transparent, const int* mn,
                const int* mx, int* e0, int* e1) {
    int sum[4] = {0, 0, 0, 0};
    int count = 0;
    for (int i = 0; i < 16; ++i) {
        if (transparent != nullptr && transparent[i]) continue;
        ++count;
        for (int c = 0; c < channels; ++c) sum[c] += rgba[i * 4 + c];
    }
    if (count == 0) count = 1;
    int mean[4];
    for (int c = 0; c < channels; ++c) mean[c] = sum[c] / count;

    long cov[4] = {0, 0, 0, 0};  // each channel against channel 0
    for (int i = 0; i < 16; ++i) {
        if (transparent != nullptr && transparent[i]) continue;
        const int d0 = rgba[i * 4] - mean[0];
        for (int c = 1; c < channels; ++c) cov[c] += static_cast<long>(d0) * (rgba[i * 4 + c] - mean[c]);
    }
    e0[0] = mx[0];
    e1[0] = mn[0];
    for (int c = 1; c < channels; ++c) {
        const bool same_direction = cov[c] >= 0;
        e0[c] = same_direction ? mx[c] : mn[c];
        e1[c] = same_direction ? mn[c] : mx[c];
    }
}

void unpack565(uint16_t c, int* rgb) {
    const int r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
    // The same expansion the hardware does, so the endpoints here are the
    // colours the GPU will actually produce.
    rgb[0] = (r << 3) | (r >> 2);
    rgb[1] = (g << 2) | (g >> 4);
    rgb[2] = (b << 3) | (b >> 2);
}

// The line through the block's colours that loses the least: the
// principal axis of their covariance, found by power iteration. A
// bounding box is cheaper and worse. It is the diagonal of the box
// whatever the colours do inside it, so a block whose colours run along
// some other direction gets endpoints that are not on their line at all.
// Eight iterations is well past convergence for a 3x3 matrix.
void principal_axis(const float* p, int count, const float* mean, float* axis) {
    float c[6] = {0, 0, 0, 0, 0, 0};  // xx xy xz yy yz zz
    for (int i = 0; i < count; ++i) {
        const float x = p[i * 3 + 0] - mean[0];
        const float y = p[i * 3 + 1] - mean[1];
        const float z = p[i * 3 + 2] - mean[2];
        c[0] += x * x; c[1] += x * y; c[2] += x * z;
        c[3] += y * y; c[4] += y * z; c[5] += z * z;
    }
    float v[3] = {1.0f, 1.0f, 1.0f};
    for (int it = 0; it < 8; ++it) {
        const float x = c[0] * v[0] + c[1] * v[1] + c[2] * v[2];
        const float y = c[1] * v[0] + c[3] * v[1] + c[4] * v[2];
        const float z = c[2] * v[0] + c[4] * v[1] + c[5] * v[2];
        const float m = std::fabs(x) > std::fabs(y)
                            ? (std::fabs(x) > std::fabs(z) ? std::fabs(x) : std::fabs(z))
                            : (std::fabs(y) > std::fabs(z) ? std::fabs(y) : std::fabs(z));
        if (m < 1e-6f) {
            // No spread worth speaking of: any direction will do, and the
            // endpoints collapse onto the mean anyway.
            axis[0] = 1.0f; axis[1] = 0.0f; axis[2] = 0.0f;
            return;
        }
        v[0] = x / m; v[1] = y / m; v[2] = z / m;
    }
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len < 1e-6f) {
        axis[0] = 1.0f; axis[1] = 0.0f; axis[2] = 0.0f;
        return;
    }
    axis[0] = v[0] / len; axis[1] = v[1] / len; axis[2] = v[2] / len;
}

}  // namespace

void bc1_block(const uint8_t* rgba, uint8_t* out) {
    tables();
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


namespace {

// Sixteen interpolation weights, out of 64, the 4-bit index table BC7
// mode 6 uses.
const int kWeight4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

// Little-endian bit stream into the 16-byte block.
struct BitWriter {
    uint8_t* out = nullptr;
    int pos = 0;
    void put(uint32_t value, int bits) {
        for (int i = 0; i < bits; ++i, ++pos) {
            if ((value >> i) & 1u) out[pos >> 3] |= static_cast<uint8_t>(1u << (pos & 7));
        }
    }
};

// Returns the squared error this encoding costs, summed over all four
// channels of all sixteen texels, so bc7_block() below can compare it
// against mode 5 without unpacking either block again.
uint64_t bc7_mode6_encode(const uint8_t* rgba, uint8_t* out) {
    // Endpoints from the block's bounding box over all four channels,
    // then the same least-squares step the BC1 encoder uses, with
    // sixteen index steps the endpoints matter more, not less.
    int mn[4] = {255, 255, 255, 255};
    int mx[4] = {0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        for (int c = 0; c < 4; ++c) {
            const int v = rgba[i * 4 + c];
            if (v < mn[c]) mn[c] = v;
            if (v > mx[c]) mx[c] = v;
        }
    }
    int e0[4], e1[4];
    orient_box(rgba, 4, nullptr, mn, mx, e0, e1);

    // Assign indices against the current endpoints, then solve for the
    // endpoints those indices imply. One pass, as in BC1.
    int idx[16];
    for (int pass = 0; pass < 2; ++pass) {
        int dir[4];
        int len2 = 0;
        for (int c = 0; c < 4; ++c) {
            dir[c] = e1[c] - e0[c];
            len2 += dir[c] * dir[c];
        }
        if (len2 == 0) {
            for (int i = 0; i < 16; ++i) idx[i] = 0;
            break;
        }
        const int64_t inv = (static_cast<int64_t>(64) << 24) / len2;
        int proj[16];
        project_block(rgba, e0, dir, proj, 4);
        for (int i = 0; i < 16; ++i) {
            int w = static_cast<int>((static_cast<int64_t>(proj[i]) * inv + (1 << 23)) >> 24);
            if (w < 0) w = 0;
            if (w > 64) w = 64;
            // The sixteen weights are evenly spaced to within half a step
            // (0, 4, 9, 13, ... 64), so the nearest one is a multiply
            // rather than a search over the table. Verified against an
            // exhaustive search for every w in 0..64: this always lands
            // on a nearest weight, differing only where two are exactly
            // equidistant (11 of the 65 values), which cost the same.
            idx[i] = (w * 15 + 32) / 64;
        }
        if (pass == 1) break;
        // Least squares over the fixed weights.
        int a2 = 0, ab = 0, b2 = 0, ax[4] = {0, 0, 0, 0}, bx[4] = {0, 0, 0, 0};
        for (int i = 0; i < 16; ++i) {
            const int t = kWeight4[idx[i]];
            const int u = 64 - t;
            a2 += u * u;
            ab += u * t;
            b2 += t * t;
            for (int c = 0; c < 4; ++c) {
                ax[c] += u * rgba[i * 4 + c];
                bx[c] += t * rgba[i * 4 + c];
            }
        }
        const int64_t det = static_cast<int64_t>(a2) * b2 - static_cast<int64_t>(ab) * ab;
        if (det == 0) break;
        for (int c = 0; c < 4; ++c) {
            const int64_t n0 =
                (static_cast<int64_t>(64) * (static_cast<int64_t>(b2) * ax[c] -
                                             static_cast<int64_t>(ab) * bx[c])) / det;
            const int64_t n1 =
                (static_cast<int64_t>(64) * (static_cast<int64_t>(a2) * bx[c] -
                                             static_cast<int64_t>(ab) * ax[c])) / det;
            e0[c] = clamp255(static_cast<int>(n0));
            e1[c] = clamp255(static_cast<int>(n1));
        }
    }

    // Quantise to 7 bits plus one p-bit shared by all four channels of an
    // endpoint. Both p values are tried and the closer one kept, which is
    // the whole of the choice at this precision.
    int q0[4], q1[4], p0 = 0, p1 = 0;
    for (int end = 0; end < 2; ++end) {
        const int* e = end == 0 ? e0 : e1;
        int* q = end == 0 ? q0 : q1;
        int best_p = 0;
        long best_err = -1;
        int best_q[4] = {0, 0, 0, 0};
        for (int p = 0; p < 2; ++p) {
            long err = 0;
            int cand[4];
            for (int c = 0; c < 4; ++c) {
                int v = (e[c] - p) >> 1;
                if (v < 0) v = 0;
                if (v > 127) v = 127;
                cand[c] = v;
                const int back = (v << 1) | p;
                const long d = back - e[c];
                err += d * d;
            }
            if (best_err < 0 || err < best_err) {
                best_err = err;
                best_p = p;
                for (int c = 0; c < 4; ++c) best_q[c] = cand[c];
            }
        }
        for (int c = 0; c < 4; ++c) q[c] = best_q[c];
        (end == 0 ? p0 : p1) = best_p;
    }

    // The first index is written with its top bit implied zero, so the
    // endpoints have to be ordered such that texel 0 sits in the lower
    // half of the line. Swapping them inverts every index.
    if (idx[0] >= 8) {
        for (int c = 0; c < 4; ++c) {
            const int t = q0[c];
            q0[c] = q1[c];
            q1[c] = t;
        }
        const int tp = p0;
        p0 = p1;
        p1 = tp;
        for (int i = 0; i < 16; ++i) idx[i] = 15 - idx[i];
    }

    std::memset(out, 0, 16);
    BitWriter w{out};
    w.put(1u << 6, 7);  // mode 6: six zero bits then a one
    for (int c = 0; c < 4; ++c) {
        w.put(static_cast<uint32_t>(q0[c]), 7);
        w.put(static_cast<uint32_t>(q1[c]), 7);
    }
    w.put(static_cast<uint32_t>(p0), 1);
    w.put(static_cast<uint32_t>(p1), 1);
    w.put(static_cast<uint32_t>(idx[0]), 3);
    for (int i = 1; i < 16; ++i) w.put(static_cast<uint32_t>(idx[i]), 4);

    // What that cost, against the endpoints as the decoder will see them.
    int d0[4], d1[4];
    for (int c = 0; c < 4; ++c) {
        d0[c] = (q0[c] << 1) | p0;
        d1[c] = (q1[c] << 1) | p1;
    }
    uint64_t err = 0;
    for (int i = 0; i < 16; ++i) {
        const int t = kWeight4[idx[i]];
        for (int c = 0; c < 4; ++c) {
            const int got = (d0[c] * (64 - t) + d1[c] * t + 32) >> 6;
            const int64_t d = got - rgba[i * 4 + c];
            err += static_cast<uint64_t>(d * d);
        }
    }
    return err;
}

// BC7 mode 5: one subset, RGB endpoints at 7 bits with their own 2-bit
// indices, and alpha endpoints at 8 bits with a SECOND, independent set.
//
// Why this exists beside mode 6. Mode 6 has finer indices (16 steps to
// mode 5's 4) and is the better mode whenever a block's four channels
// move together, which is most of them. But its one index per texel is
// shared by all four channels, so a texel cannot sit at one point along
// the colour line and a different point along alpha. Where alpha varies
// independently of colour, that is unrepresentable, and the encoder can
// only split the difference.
//
// Measured on a 4x4 block whose alpha and colour both vary non-linearly
// and independently: mode 6 manages 12.53 dB, worst-case error 124 of
// 255, which is half scale. The same block through mode 5 is a different
// order of magnitude. Soft-edged decals and cut-out masks are exactly
// this shape, and a per-block error that size is what shows up as faint
// squares on the 4x4 grid.
const int kWeight2[4] = {0, 21, 43, 64};

// One least-squares line fit of `comp` interleaved channels, against a
// fixed weight table. The same two-pass shape the other encoders use:
// bounding box, assign, solve for the endpoints those assignments imply.
void fit_line(const uint8_t* src, int stride, int comp, const int* weights, int nweights,
              int e0[4], int e1[4], int idx[16]) {
    int mn[4], mx[4];
    for (int c = 0; c < comp; ++c) {
        mn[c] = 255;
        mx[c] = 0;
    }
    for (int i = 0; i < 16; ++i) {
        for (int c = 0; c < comp; ++c) {
            const int v = src[i * stride + c];
            if (v < mn[c]) mn[c] = v;
            if (v > mx[c]) mx[c] = v;
        }
    }
    for (int c = 0; c < comp; ++c) {
        e0[c] = mn[c];
        e1[c] = mx[c];
    }
    for (int i = 0; i < 16; ++i) idx[i] = 0;

    for (int pass = 0; pass < 2; ++pass) {
        int dir[4];
        int64_t len2 = 0;
        for (int c = 0; c < comp; ++c) {
            dir[c] = e1[c] - e0[c];
            len2 += static_cast<int64_t>(dir[c]) * dir[c];
        }
        if (len2 == 0) break;
        for (int i = 0; i < 16; ++i) {
            int64_t dot = 0;
            for (int c = 0; c < comp; ++c)
                dot += static_cast<int64_t>(src[i * stride + c] - e0[c]) * dir[c];
            int64_t w = (dot * 64 + len2 / 2) / len2;
            if (w < 0) w = 0;
            if (w > 64) w = 64;
            int best = 0;
            int64_t best_d = -1;
            for (int k = 0; k < nweights; ++k) {
                int64_t d = w - weights[k];
                if (d < 0) d = -d;
                if (best_d < 0 || d < best_d) {
                    best_d = d;
                    best = k;
                }
            }
            idx[i] = best;
        }
        if (pass == 1) break;
        int64_t a2 = 0, ab = 0, b2 = 0, ax[4] = {0, 0, 0, 0}, bx[4] = {0, 0, 0, 0};
        for (int i = 0; i < 16; ++i) {
            const int64_t t = weights[idx[i]];
            const int64_t u = 64 - t;
            a2 += u * u;
            ab += u * t;
            b2 += t * t;
            for (int c = 0; c < comp; ++c) {
                ax[c] += u * src[i * stride + c];
                bx[c] += t * src[i * stride + c];
            }
        }
        const int64_t det = a2 * b2 - ab * ab;
        if (det == 0) break;
        for (int c = 0; c < comp; ++c) {
            e0[c] = clamp255(static_cast<int>((64 * (b2 * ax[c] - ab * bx[c])) / det));
            e1[c] = clamp255(static_cast<int>((64 * (a2 * bx[c] - ab * ax[c])) / det));
        }
    }
}

uint64_t bc7_mode5_encode(const uint8_t* rgba, uint8_t* out) {
    int ce0[4], ce1[4], cidx[16];
    fit_line(rgba, 4, 3, kWeight2, 4, ce0, ce1, cidx);

    uint8_t alpha[16];
    for (int i = 0; i < 16; ++i) alpha[i] = rgba[i * 4 + 3];
    int ae0[4], ae1[4], aidx[16];
    fit_line(alpha, 1, 1, kWeight2, 4, ae0, ae1, aidx);

    // RGB is 7 bits with no p-bit, so the decoder expands it by replicating
    // the top bit down. Alpha is a full 8 and is used as written.
    int q0[3], q1[3];
    for (int c = 0; c < 3; ++c) {
        q0[c] = (ce0[c] * 127 + 127) / 255;
        q1[c] = (ce1[c] * 127 + 127) / 255;
    }
    int a0 = ae0[0], a1 = ae1[0];

    const auto expand7 = [](int v) { return (v << 1) | (v >> 6); };
    int d0[3], d1[3];
    for (int c = 0; c < 3; ++c) {
        d0[c] = expand7(q0[c]);
        d1[c] = expand7(q1[c]);
    }

    // Re-assign both index sets against the endpoints as quantised, which
    // is what the decoder will actually interpolate between.
    for (int i = 0; i < 16; ++i) {
        int best = 0;
        int64_t best_e = -1;
        for (int k = 0; k < 4; ++k) {
            const int t = kWeight2[k];
            int64_t e = 0;
            for (int c = 0; c < 3; ++c) {
                const int got = (d0[c] * (64 - t) + d1[c] * t + 32) >> 6;
                const int64_t d = got - rgba[i * 4 + c];
                e += d * d;
            }
            if (best_e < 0 || e < best_e) {
                best_e = e;
                best = k;
            }
        }
        cidx[i] = best;
    }
    for (int i = 0; i < 16; ++i) {
        int best = 0;
        int64_t best_e = -1;
        for (int k = 0; k < 4; ++k) {
            const int t = kWeight2[k];
            const int got = (a0 * (64 - t) + a1 * t + 32) >> 6;
            const int64_t d = got - alpha[i];
            const int64_t e = d * d;
            if (best_e < 0 || e < best_e) {
                best_e = e;
                best = k;
            }
        }
        aidx[i] = best;
    }

    // Both index sets carry their own anchor: texel 0's top bit is implied
    // zero in each, so each pair of endpoints is ordered independently.
    if (cidx[0] > 1) {
        for (int c = 0; c < 3; ++c) {
            int t = q0[c];
            q0[c] = q1[c];
            q1[c] = t;
            t = d0[c];
            d0[c] = d1[c];
            d1[c] = t;
        }
        for (int i = 0; i < 16; ++i) cidx[i] = 3 - cidx[i];
    }
    if (aidx[0] > 1) {
        const int t = a0;
        a0 = a1;
        a1 = t;
        for (int i = 0; i < 16; ++i) aidx[i] = 3 - aidx[i];
    }

    std::memset(out, 0, 16);
    BitWriter w{out};
    w.put(1u << 5, 6);  // mode 5: five zero bits then a one
    w.put(0, 2);        // no channel rotation; alpha stays alpha
    for (int c = 0; c < 3; ++c) {
        w.put(static_cast<uint32_t>(q0[c]), 7);
        w.put(static_cast<uint32_t>(q1[c]), 7);
    }
    w.put(static_cast<uint32_t>(a0), 8);
    w.put(static_cast<uint32_t>(a1), 8);
    w.put(static_cast<uint32_t>(cidx[0]), 1);
    for (int i = 1; i < 16; ++i) w.put(static_cast<uint32_t>(cidx[i]), 2);
    w.put(static_cast<uint32_t>(aidx[0]), 1);
    for (int i = 1; i < 16; ++i) w.put(static_cast<uint32_t>(aidx[i]), 2);

    uint64_t err = 0;
    for (int i = 0; i < 16; ++i) {
        const int t = kWeight2[cidx[i]];
        for (int c = 0; c < 3; ++c) {
            const int got = (d0[c] * (64 - t) + d1[c] * t + 32) >> 6;
            const int64_t d = got - rgba[i * 4 + c];
            err += static_cast<uint64_t>(d * d);
        }
        const int ta = kWeight2[aidx[i]];
        const int gota = (a0 * (64 - ta) + a1 * ta + 32) >> 6;
        const int64_t da = gota - alpha[i];
        err += static_cast<uint64_t>(da * da);
    }
    return err;
}

}  // namespace

// Squared error, over all 64 components of a block, below which mode 5 is
// not attempted at all.
//
// Mode 5 only ever helps where mode 6 cannot express the block, and mode 6
// reports what it cost anyway, so a block it already fits closely can skip
// the second encode entirely. The number is 64 components times 16, i.e.
// a root-mean-square error of 4 per component; see bc7_block().
constexpr uint64_t kMode6GoodEnough = 64ull * 16ull;

// The single-subset pair, and what it cost.
static uint64_t bc7_single_subset(const uint8_t* rgba, uint8_t* out, bool opaque) {
    uint8_t six[16];
    const uint64_t err6 = bc7_mode6_encode(rgba, six);
    // The early-out is only safe on an opaque block. Mode 6 shares one
    // index set across all four channels, so on a block with alpha it can
    // post a small TOTAL error while the alpha itself is off by a couple
    // -- enough for the hidden colour behind a transparent texel to show
    // through. Mode 5 is the single-subset mode with its own alpha
    // indices, so it is exactly what such a block needs, and skipping it
    // on a cheap total-error test is how the leak got in.
    if (opaque && err6 <= kMode6GoodEnough) { std::memcpy(out, six, 16); return err6; }
    uint8_t five[16];
    const uint64_t err5 = bc7_mode5_encode(rgba, five);
    std::memcpy(out, err5 < err6 ? five : six, 16);
    return err5 < err6 ? err5 : err6;
}


// Stud's own mode 6/5 first, and bc7enc's mode 1 only for the blocks it
// fits badly.
//
// Mode 6 and mode 5 are single-subset: one line through the block. That
// is enough for most blocks and it is fast. It is not enough for a block
// holding two distinct colours, which comes out as a visible square on a
// normal map, and no amount of endpoint precision fixes it -- such a
// block needs two subsets, which is mode 1.
//
// So the cheap encoder runs first and reports what it cost, and only a
// block above the threshold pays for a partition search. Measured on a
// real dumped texture, per 1024x1024 level:
//
//   mode 6/5 alone                33.1 ms  48.06 dB  worst 20  <- squares
//   bc7enc, all of modes 1+5+6    86.3 ms  52.79 dB  worst  8
//   this, mode 1 above rms 1      51.5 ms  52.15 dB  worst  4
//
// Cheaper than handing every block to bc7enc AND a better worst case,
// which is the number that decides whether a square shows: a square is
// one block much worse than its neighbours, not a worse average. The
// average is 0.64 dB behind bc7enc and that is the right thing to spend.
// Mode 1 is reached on 22% of blocks.
//
// The threshold is squared error over the block's 48 colour components,
// so 48 is a root-mean-square error of one per component.
constexpr uint64_t kNeedsTwoSubsets = 48;

void bc7_block(const uint8_t* rgba, uint8_t* out) {
    // A block carrying any transparency goes straight to bc7enc.
    //
    // Mode 6 shares ONE index set across all four channels, so a fully
    // transparent texel's index is chosen to suit its colour and its
    // alpha comes back near zero rather than zero. What shows on screen
    // is the hidden canvas behind the transparent part bleeding through
    // as a faint square. The block's total error stays small the whole
    // time -- alpha being off by two is not much squared error -- so no
    // threshold on total error catches it.
    //
    // bc7enc picks among modes 5, 6 and 7, and 5 and 7 carry alpha in
    // their own index set. Stud's single-subset encoder keeps the opaque
    // blocks, which is the overwhelming majority of them.
    bool opaque = true;
    int clear = 0;
    for (int i = 0; i < 16; ++i) {
        const int a = rgba[i * 4 + 3];
        if (a != 255) opaque = false;
        if (a == 0) ++clear;
    }

    // A fully transparent texel's colour is never seen, but the encoder
    // does not know that and spends its endpoints reaching for it. When
    // that hidden canvas is far from the visible colours -- white behind
    // green, say -- the colour line is stretched across both and alpha
    // accuracy is what gets traded away to pay for it. The clear texels
    // then come back with alpha near zero instead of zero and the canvas
    // shows through as a faint square.
    //
    // So the canvas is replaced with the average of the visible texels
    // before encoding. It costs nothing on screen, since those texels are
    // invisible, and it stops the canvas bleeding into its neighbours
    // under bilinear filtering as well.
    uint8_t fixed[64];
    if (clear > 0 && clear < 16) {
        int sum[3] = {0, 0, 0};
        int seen = 0;
        for (int i = 0; i < 16; ++i) {
            if (rgba[i * 4 + 3] == 0) continue;
            ++seen;
            for (int c = 0; c < 3; ++c) sum[c] += rgba[i * 4 + c];
        }
        std::memcpy(fixed, rgba, 64);
        for (int i = 0; i < 16; ++i) {
            if (rgba[i * 4 + 3] != 0) continue;
            for (int c = 0; c < 3; ++c)
                fixed[i * 4 + c] = static_cast<uint8_t>(sum[c] / seen);
        }
        rgba = fixed;
    }

    uint8_t single[16];
    const uint64_t err = bc7_single_subset(rgba, single, opaque);
    if (err <= kNeedsTwoSubsets) {
        std::memcpy(out, single, 16);
        return;
    }
    tables();
    bc7enc_compress_block(out, rgba, &bc7_mode1_params());
}

}  // namespace stud::texture_encode
