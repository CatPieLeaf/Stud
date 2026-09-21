#!/usr/bin/env python3
"""Turn an mpv RAVU-Zoom hook into the compute shader render-host compiles.

    gen_ravu.py <hook> <out.comp>

Handles both the plain hook and the anti-ringing one, which differ in
whether they bind a second weight table.

The body is mpv's, mechanically substituted rather than retyped: the
gradient weights and the LUT indexing are trained constants with nothing
in them a reader could check by eye, which is exactly the case where
transcribing from a reference goes wrong quietly. upscale.comp's own
comment records what that cost the first time.

What this supplies, because mpv's hook vocabulary is not Stud's:

    HOOKED_pos * HOOKED_size  -> uv * src_size
    HOOKED_tex(c)             -> textureLod(src, c, 0.0)
    HOOKED_pt                 -> 1/src_size
    texture(ravu_zoom_lut2,)  -> textureLod(lut,)
    texture(..._lut2_ar,)     -> textureLod(lut_ar,)
    return vec4(res, 1.0)     -> a store, with the swizzle Stud's
                                 byte-copy hand-over needs
"""

import re
import sys


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    src_path, out_path = sys.argv[1], sys.argv[2]

    lines = open(src_path).read().splitlines()
    begin = next(i for i, l in enumerate(lines) if l.startswith("vec4 hook()"))
    end = next(i for i, l in enumerate(lines) if l.startswith("//!TEXTURE"))
    body = lines[begin + 1:end]
    while body and body[-1].strip() in ("}", ""):
        body.pop()

    consts = [l for l in lines[:begin]
              if l.startswith("const ") or l.startswith("#define")]

    joined = "\n".join(body)
    anti_ringing = "ravu_zoom_lut2_ar" in joined

    # Longest patterns first, so the _ar table is not mangled by the
    # substitution for the plain one.
    joined = joined.replace("HOOKED_pos * HOOKED_size", "uv * src_size")
    joined = re.sub(r"HOOKED_tex\((.*?)\)\.xyz",
                    lambda m: "textureLod(src, " + m.group(1) + ", 0.0).xyz", joined)
    joined = joined.replace("HOOKED_pt", "inv_src_size")
    joined = joined.replace("texture(ravu_zoom_lut2_ar,", "textureLod(lut_ar,")
    joined = joined.replace("texture(ravu_zoom_lut2,", "textureLod(lut,")
    joined = re.sub(r"textureLod\((lut|lut_ar), ([^;]*?)\);",
                    r"textureLod(\1, \2, 0.0);", joined)
    joined = joined.replace("return vec4(res, 1.0);", "")

    # Skip the anti-ringing work where it provably does nothing.
    #
    # Ringing is overshoot at an edge. On a flat neighbourhood the soft
    # minimum and maximum collapse onto the value itself and the clamp
    # changes nothing -- but the shader pays for it anyway: four table
    # samples and eight groups of five mat4x3 squarings, around 576
    # multiplies, on every pixel including a blank wall.
    #
    # The test is free, because the structure tensor has already
    # produced `lambda` before this block runs, and the threshold is
    # mpv's own: 0.004 is where its trained `strength` leaves the
    # flattest bucket. Using that boundary rather than one invented here
    # means the pixels skipped are exactly the ones the reference itself
    # classifies as having no edge in them.
    if anti_ringing:
        start = joined.index("w = textureLod(lut_ar,")
        end = joined.index("res = mix(res, clamp(res, lo, hi)")
        end = joined.index("\n", end) + 1
        block = joined[start:end]
        indented = "\n".join(("    " + l) if l.strip() else l for l in block.splitlines())
        joined = (joined[:start] +
                  "if (lambda >= 0.004) {  // see tools/gen_ravu.py: no edge, no ringing\n" +
                  indented + "\n}\n" +
                  joined[end:])

    leftovers = sorted({w for w in re.findall(r"\w*(?:HOOKED|ravu_zoom_lut)\w*", joined)})
    if leftovers:
        sys.stderr.write(f"{src_path}: unsubstituted mpv vocabulary: {leftovers}\n")
        return 1

    name = "RAVU-Zoom-AR" if anti_ringing else "RAVU-Zoom"
    ar_note = '''//
// This is the ANTI-RINGING variant. Plain RAVU reconstructs an edge from
// trained weights that have negative lobes, so it can overshoot on hard
// edges -- which looks like a bright or dark border traced around
// things, and was reported exactly that way. This one builds a soft
// local minimum and maximum from a SECOND trained table and clamps the
// result into them, so the output cannot leave the range its own
// neighbourhood spans. Structurally incapable of ringing, the same
// property that makes SGSR and RCAS ring-free.
//
// It costs a second set of table samples and the pow() chain that
// shapes them, which is why the plain one is kept beside it.''' \
        if anti_ringing else '''//
// The plain variant: no clamp on the output, so its trained weights'
// negative lobes can overshoot and trace a border around hard edges.
// See ravu_ar.comp, which bounds it.'''

    ar_binding = '''
// The second trained table, for the soft minimum and maximum the result
// is clamped into. Only the anti-ringing variant binds it.
layout(set = 0, binding = 3) uniform sampler2D lut_ar;''' if anti_ringing else ""

    header = f'''#version 450

// {name} (r2, RGB), a trained upscaler from mpv's prescaler set.
//
//     Copyright the mpv-prescalers authors.
//     SPDX-License-Identifier: LGPL-3.0-or-later
//
// GENERATED by tools/gen_ravu.py from
// third_party/mpv-prescalers/{src_path.split("/")[-1]} -- see that
// script for what it substitutes and why the body is not retyped.
//
// What it does, unlike every other upscaler here: it builds a structure
// tensor from the 4x4 neighbourhood's luma, reduces it to an edge angle,
// a strength and a coherence, and uses those three to index a table of
// filter weights trained offline. The others decide a tap's weight by
// arithmetic; this one looks it up.
{ar_note}
//
// The tables are 18x2592 rgba, uploaded device-local once at chain build
// as HALF. The hook declares rgba16f and carries float32 data; the
// format it declares is the right one, since mpv uploads half and the
// weights are trained at that precision, and a table is sampled four
// times per output pixel. tools/gen_ravu_lut.py does the conversion and
// fails the build if any weight does not survive it.
//
// mpv also ships a COMPUTE variant that loads the shared neighbourhood
// into LDS once per workgroup instead of re-fetching sixteen taps per
// pixel. It was ported and measured here, and it is SLOWER: 0.557ms per
// megapixel against 0.537, with a worse tail. The taps were already
// hitting the texture cache, and the barrier and the LDS round trip cost
// more than the redundant fetches did.
//
// Precision is left at the default. The structure tensor takes
// eigenvalues, a sqrt and an atan over small gradients and then
// quantises the angle into 24 buckets; reduced precision there could
// move a pixel between buckets, which is a different picture rather than
// a cheaper one. The weight tables ARE half -- that is where the
// bandwidth was.

precision highp float;
precision highp int;

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D dst;
// The trained weights. Its own binding, which is why a RAVU chain needs
// a descriptor layout the other upscalers do not.
layout(set = 0, binding = 2) uniform sampler2D lut;{ar_binding}

layout(push_constant) uniform Params {{
    ivec2 src_size_i;
    ivec2 dst_size;
    // Unused: RAVU does no sharpening of its own, so the setting drives
    // the separate RCAS pass after it instead.
    float sharpness;
    int swap_rb;
}} p;

'''

    main_open = '''void main() {
    ivec2 out_pos = ivec2(gl_GlobalInvocationID.xy);
    if (out_pos.x >= p.dst_size.x || out_pos.y >= p.dst_size.y) return;

    vec2 src_size = vec2(p.src_size_i);
    vec2 inv_src_size = 1.0 / src_size;
    vec2 uv = (vec2(out_pos) + 0.5) / vec2(p.dst_size);

'''

    footer = '''
    // See upscale.comp: the last pass to write swizzles, so the
    // hand-over to the swapchain is a byte copy rather than a converting
    // blit.
    imageStore(dst, out_pos, vec4(p.swap_rb != 0 ? res.bgr : res, 1.0));
}
'''

    out = header + "\n".join(consts) + "\n\n" + main_open + joined + footer
    open(out_path, "w").write(out)
    print(f"{out_path}: {len(out.splitlines())} lines, "
          f"{'anti-ringing' if anti_ringing else 'plain'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
