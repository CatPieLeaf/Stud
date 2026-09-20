import re

src = "/home/catpieleaf/.claude/jobs/76daf66d/tmp/prescalers/ravu-zoom-r2-rgb.hook"
dst = "/home/catpieleaf/Stud-Folder/Stud/.claude/worktrees/perf-latency/render-host/shaders/ravu.comp"

text = open(src).read()
lines = text.splitlines()

begin = next(i for i, l in enumerate(lines) if l.startswith("vec4 hook()"))
end = next(i for i, l in enumerate(lines) if l.startswith("//!TEXTURE"))
body = lines[begin + 1:end]
# drop the trailing "}" of hook()
while body and body[-1].strip() in ("}", ""):
    body.pop()

joined = "\n".join(body)

# mpv's hook vocabulary -> ours. Order matters: the longest first.
joined = joined.replace("HOOKED_pos * HOOKED_size", "uv * src_size")
joined = re.sub(r"HOOKED_tex\((.*?)\)\.xyz",
                lambda m: "textureLod(src, " + m.group(1) + ", 0.0).xyz",
                joined)
joined = joined.replace("HOOKED_pt", "inv_src_size")
joined = joined.replace("texture(ravu_zoom_lut2,", "textureLod(lut,")
# textureLod needs its lod argument; the substitution above left two-arg calls.
joined = re.sub(r"textureLod\(lut, ([^;]*?)\);", r"textureLod(lut, \1, 0.0);", joined)
joined = joined.replace("return vec4(res, 1.0);", "")

# also carry over the two constants declared above hook()
consts = [l for l in lines[:begin]
          if l.startswith("const ") or l.startswith("#define")]

header = '''#version 450

// RAVU-Zoom (r2, RGB), a trained upscaler from mpv's prescaler set.
//
//     Copyright the mpv-prescalers authors.
//     SPDX-License-Identifier: LGPL-3.0-or-later
//
// GENERATED, not written: the body below is the hook() of
// third_party/mpv-prescalers/ravu-zoom-r2-rgb.hook with mpv's texture
// vocabulary mechanically substituted for Stud's, by
// tools/gen_ravu.py. It is not retyped, because the gradient weights
// and the LUT indexing arithmetic are trained constants and there is
// nothing in them a reader could check by eye -- which is exactly the
// case where transcribing from a reference goes wrong quietly.
//
// What it does, unlike every other upscaler here: it builds a structure
// tensor from the 4x4 neighbourhood's luma, reduces it to an edge angle,
// a strength and a coherence, and uses those three to index a trained
// table of filter weights. The other filters decide how to weight a tap
// from arithmetic; this one looks the answer up.
//
// The table is 18x2592 rgba, 746KB, uploaded device-local once at chain
// build. Note that the hook declares it `rgba16f` while the data it
// carries is float32 -- verified by decoding, not assumed: as float16
// the values come out as nonsense like -10136, and as float32 every one
// of the 186,624 falls inside [-4,4]. It is uploaded as
// R32G32B32A32_SFLOAT accordingly.
//
// Note the licence. Unlike FSR1 (MIT) and SGSR (BSD-3-Clause) this is
// LGPL-3.0-or-later, which is a different question for a shipped binary
// and has to be settled before it could be the one Stud keeps.
//
// STUD_UPSCALER=ravu.

precision highp float;
precision highp int;

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D dst;
// The trained weights. Its own binding, which is why a RAVU chain needs
// a descriptor layout the other upscalers do not.
layout(set = 0, binding = 2) uniform sampler2D lut;

layout(push_constant) uniform Params {
    ivec2 src_size_i;
    ivec2 dst_size;
    // Unused: RAVU has no sharpening stage of its own.
    float sharpness;
    int swap_rb;
} p;

'''

footer = '''
    // See upscale.comp: the last pass to write swizzles, so the
    // hand-over to the swapchain is a byte copy rather than a
    // converting blit.
    imageStore(dst, out_pos, vec4(p.swap_rb != 0 ? res.bgr : res, 1.0));
}
'''

main_open = '''void main() {
    ivec2 out_pos = ivec2(gl_GlobalInvocationID.xy);
    if (out_pos.x >= p.dst_size.x || out_pos.y >= p.dst_size.y) return;

    vec2 src_size = vec2(p.src_size_i);
    vec2 inv_src_size = 1.0 / src_size;
    vec2 uv = (vec2(out_pos) + 0.5) / vec2(p.dst_size);

'''

out = header + "\n".join(consts) + "\n\n" + main_open + joined + footer
open(dst, "w").write(out)
print(f"wrote {dst}: {len(out.splitlines())} lines")
print("constants carried over:", consts)
