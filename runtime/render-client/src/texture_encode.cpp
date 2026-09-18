#include "texture_encode.h"

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

inline uint16_t pack565(int r, int g, int b) {
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// The colour half of BC1 and BC3. `four_colour` forces the mode with four
// interpolated colours and no transparent index, which is what BC3 needs
// (its alpha lives in a separate block) and what an opaque BC1 block
// wants; punch-through uses the other mode, where c0 <= c1 means index 3
// is transparent black.
// Where each of the 16 texels falls along the line from `base` towards
// `base + dir`, as an unnormalised dot product. Both of this encoder's
// passes over a block are exactly this, and it is where its time goes, so
// it is done four texels at a time: an RGBA texel is four bytes, so one
// 16-byte load is four of them, and the per-channel multiply-accumulate
// is what _mm_madd_epi16 does natively.
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

void encode_colour(const uint8_t* rgba, uint8_t* out, bool four_colour,
                   const bool* transparent) {
    int mn[3] = {255, 255, 255};
    int mx[3] = {0, 0, 0};
    int count = 0;
    if (transparent == nullptr) {
        count = 16;
        bounds_block(rgba, mn, mx);
    } else {
        for (int i = 0; i < 16; ++i) {
            if (transparent[i]) continue;
            ++count;
            for (int c = 0; c < 3; ++c) {
                const int v = rgba[i * 4 + c];
                if (v < mn[c]) mn[c] = v;
                if (v > mx[c]) mx[c] = v;
            }
        }
    }
    if (count == 0) {
        // Every texel transparent. Endpoints do not matter as long as the
        // mode is the punch-through one (c0 <= c1) and every index is 3.
        out[0] = 0; out[1] = 0; out[2] = 1; out[3] = 0;
        std::memset(out + 4, 0xff, 4);
        return;
    }

    // Start from the block's bounding box, on the diagonal the colours
    // actually run along, then let the least-squares step below move the
    // endpoints to where they belong.
    //
    // The principal axis of the colours is the better starting point in
    // principle and was measured here: worth 1.2 dB on its own, at 7.7x
    // the decode against the bounding box's 2.6x, because a power
    // iteration over a 3x3 covariance runs per block. Choosing the right
    // diagonal captures most of what the axis was for; it is the same
    // covariance, reduced to its sign, and the refinement does the
    // rest.
    int e0[3], e1[3];
    orient_box(rgba, 3, transparent, mn, mx, e0, e1);
    for (int c = 0; c < 3; ++c) {
        const int inset = (mx[c] - mn[c]) >> 4;
        // Toward each other, whichever way round this channel runs.
        if (e0[c] >= e1[c]) {
            e0[c] = clamp255(e0[c] - inset);
            e1[c] = clamp255(e1[c] + inset);
            if (e0[c] < e1[c]) e0[c] = e1[c];
        } else {
            e0[c] = clamp255(e0[c] + inset);
            e1[c] = clamp255(e1[c] - inset);
            if (e0[c] > e1[c]) e0[c] = e1[c];
        }
    }

    uint16_t c0 = pack565(e0[0], e0[1], e0[2]);
    uint16_t c1 = pack565(e1[0], e1[1], e1[2]);
    // The mode is selected by the order of the two endpoints, so they are
    // arranged to match the one wanted. Which of them ends up first no
    // longer matters, because the indices below are chosen against the
    // palette the hardware will build from whatever is written.
    if (four_colour ? (c0 <= c1) : (c0 > c1)) {
        const uint16_t t = c0;
        c0 = c1;
        c1 = t;
    }
    out[0] = static_cast<uint8_t>(c0 & 0xff);
    out[1] = static_cast<uint8_t>(c0 >> 8);
    out[2] = static_cast<uint8_t>(c1 & 0xff);
    out[3] = static_cast<uint8_t>(c1 >> 8);

    if (four_colour && c0 == c1) {
        // A flat block cannot keep c0 > c1 and stay in this mode, so it
        // falls into the other one, index 0 is endpoint 0 either way and
        // reproduces the colour exactly.
        std::memset(out + 4, 0, 4);
        return;
    }

    int pal[4][3];
    unpack565(c0, pal[0]);
    unpack565(c1, pal[1]);
    if (four_colour) {
        for (int c = 0; c < 3; ++c) {
            pal[2][c] = (2 * pal[0][c] + pal[1][c] + 1) / 3;
            pal[3][c] = (pal[0][c] + 2 * pal[1][c] + 1) / 3;
        }
    } else {
        for (int c = 0; c < 3; ++c) pal[2][c] = (pal[0][c] + pal[1][c]) / 2;
        pal[3][0] = pal[3][1] = pal[3][2] = 0;  // transparent black
    }
    // One least-squares refinement, in integers.
    //
    // With indices chosen, the endpoints that minimise the error are no
    // longer the two ends of the bounding box, solving for them and
    // re-quantising recovers most of what an endpoint search would find,
    // for one extra pass over the block rather than a search over
    // candidate pairs. Worth 1.3 dB here, which takes this past stb_dxt.
    //
    // The weights are the index positions along the line (0, 1/3, 2/3,
    // 3/3), so scaling them by 3 makes every term an integer: floats cost
    // more than the whole rest of the encoder at this block size.
    if (four_colour) {
        int dir0[3];
        for (int c = 0; c < 3; ++c) dir0[c] = pal[1][c] - pal[0][c];
        const int len2_0 = dir0[0] * dir0[0] + dir0[1] * dir0[1] + dir0[2] * dir0[2];
        if (len2_0 > 0) {
            int a2 = 0, ab = 0, b2 = 0;
            int ax[3] = {0, 0, 0}, bx[3] = {0, 0, 0};
            // One reciprocal for the block instead of a division per
            // texel. Integer division was the whole cost of this encoder:
            // 32 of them per block, at tens of cycles each, dwarfing
            // everything else it does.
            const int64_t inv0 = (static_cast<int64_t>(3) << 24) / len2_0;
            int proj[16];
            project_block(rgba, pal[0], dir0, proj);
            for (int i = 0; i < 16; ++i) {
                const int d = proj[i];
                int step = static_cast<int>((static_cast<int64_t>(d) * inv0 + (1 << 23)) >> 24);
                if (step < 0) step = 0;
                if (step > 3) step = 3;
                const int u = 3 - step;
                const int t = step;
                a2 += u * u;
                ab += u * t;
                b2 += t * t;
                for (int c = 0; c < 3; ++c) {
                    ax[c] += u * rgba[i * 4 + c];
                    bx[c] += t * rgba[i * 4 + c];
                }
            }
            const int det = a2 * b2 - ab * ab;
            if (det != 0) {
                int n0[3], n1[3];
                for (int c = 0; c < 3; ++c) {
                    n0[c] = clamp255((3 * (b2 * ax[c] - ab * bx[c]) + det / 2) / det);
                    n1[c] = clamp255((3 * (a2 * bx[c] - ab * ax[c]) + det / 2) / det);
                }
                const uint16_t r0 = pack565(n0[0], n0[1], n0[2]);
                const uint16_t r1 = pack565(n1[0], n1[1], n1[2]);
                // Only taken when the pair still expresses this mode: one
                // that collapses or inverts would change what the block
                // means rather than improve it.
                if (r0 > r1) {
                    c0 = r0;
                    c1 = r1;
                    out[0] = static_cast<uint8_t>(c0 & 0xff);
                    out[1] = static_cast<uint8_t>(c0 >> 8);
                    out[2] = static_cast<uint8_t>(c1 & 0xff);
                    out[3] = static_cast<uint8_t>(c1 >> 8);
                    unpack565(c0, pal[0]);
                    unpack565(c1, pal[1]);
                    for (int c = 0; c < 3; ++c) {
                        pal[2][c] = (2 * pal[0][c] + pal[1][c] + 1) / 3;
                        pal[3][c] = (pal[0][c] + 2 * pal[1][c] + 1) / 3;
                    }
                }
            }
        }
    }

    // Indices come from projecting onto the line between the two endpoints
    // the hardware will use, rather than from comparing against every
    // palette entry: one dot product per texel instead of four distances,
    // and with the endpoints on the colours' own principal axis the two
    // agree on all but the occasional boundary texel.
    int dir[3];
    for (int c = 0; c < 3; ++c) dir[c] = pal[1][c] - pal[0][c];
    const int len2 = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
    const int steps = four_colour ? 3 : 2;
    const int64_t inv = len2 > 0 ? (static_cast<int64_t>(steps) << 24) / len2 : 0;
    static const uint8_t kOrder4[4] = {0, 2, 3, 1};
    static const uint8_t kOrder3[3] = {0, 2, 1};
    int proj_final[16];
    if (len2 > 0) project_block(rgba, pal[0], dir, proj_final);
    uint32_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        if (transparent != nullptr && transparent[i]) {
            bits |= 3u << (i * 2);
            continue;
        }
        int step = 0;
        if (len2 > 0) {
            step = static_cast<int>((static_cast<int64_t>(proj_final[i]) * inv + (1 << 23)) >> 24);
            if (step < 0) step = 0;
            if (step > steps) step = steps;
        }
        bits |= static_cast<uint32_t>(four_colour ? kOrder4[step] : kOrder3[step]) << (i * 2);
    }
    std::memcpy(out + 4, &bits, 4);
}

// BC4, and the alpha half of BC3: eight values interpolated between a
// minimum and a maximum, three bits per texel.
void encode_alpha8(const uint8_t* values, int stride, uint8_t* out) {
    int mn = 255, mx = 0;
    for (int i = 0; i < 16; ++i) {
        const int v = values[i * stride];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    out[0] = static_cast<uint8_t>(mx);
    out[1] = static_cast<uint8_t>(mn);
    uint64_t bits = 0;
    const int range = mx - mn;
    static const uint8_t kOrder[8] = {1, 7, 6, 5, 4, 3, 2, 0};
    for (int i = 0; i < 16; ++i) {
        int step = 0;
        if (range > 0) {
            step = ((values[i * stride] - mn) * 7 + range / 2) / range;
            if (step < 0) step = 0;
            if (step > 7) step = 7;
        }
        bits |= static_cast<uint64_t>(kOrder[step]) << (i * 3);
    }
    for (int i = 0; i < 6; ++i) out[2 + i] = static_cast<uint8_t>((bits >> (i * 8)) & 0xff);
}

}  // namespace

void bc1_block(const uint8_t* rgba, uint8_t* out) { encode_colour(rgba, out, true, nullptr); }

void bc1_block_punchthrough(const uint8_t* rgba, uint8_t* out) {
    bool transparent[16];
    bool any = false;
    for (int i = 0; i < 16; ++i) {
        transparent[i] = rgba[i * 4 + 3] < 128;
        any = any || transparent[i];
    }
    // A block with no transparent texel is better served by the mode with
    // four colours, which is the whole difference between the two.
    if (!any) {
        encode_colour(rgba, out, true, nullptr);
        return;
    }
    encode_colour(rgba, out, false, transparent);
}

void bc3_block(const uint8_t* rgba, uint8_t* out) {
    encode_alpha8(rgba + 3, 4, out);
    encode_colour(rgba, out + 8, true, nullptr);
}

void bc4_block_from_r16(const uint16_t* r16, uint8_t* out) {
    uint8_t v[16];
    for (int i = 0; i < 16; ++i) v[i] = static_cast<uint8_t>(r16[i] >> 8);
    encode_alpha8(v, 1, out);
}

void bc5_block_from_rg16(const uint16_t* rg16, uint8_t* out) {
    uint8_t r[16], g[16];
    for (int i = 0; i < 16; ++i) {
        r[i] = static_cast<uint8_t>(rg16[i * 2] >> 8);
        g[i] = static_cast<uint8_t>(rg16[i * 2 + 1] >> 8);
    }
    encode_alpha8(r, 1, out);
    encode_alpha8(g, 1, out + 8);
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

}  // namespace

void bc7_mode6_block(const uint8_t* rgba, uint8_t* out) {
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
}

}  // namespace stud::texture_encode
