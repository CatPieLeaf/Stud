#include "stud/text_overlay.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "wayland_overlay_deps.h"

#include "stud/android_glue.h"
#include "x11_backend.h"

// See stud/text_overlay.h for why this exists at all. In short: a focused
// TextBox stops drawing its own text on Android, because the platform is
// expected to put a real text widget over it. This is that widget.

namespace stud::android_glue {
namespace {

struct Face {
    FT_Face face = nullptr;
    // FreeType wants the file to stay alive for the life of the face when
    // loaded from memory, and loading once per focus is wasteful anyway.
    std::vector<unsigned char> bytes;
};

FT_Library& library() {
    static FT_Library lib = [] {
        FT_Library l = nullptr;
        if (FT_Init_FreeType(&l) != 0) return static_cast<FT_Library>(nullptr);
        return l;
    }();
    return lib;
}

// Faces are keyed by path: the app shell reuses a handful of Roblox fonts,
// so this settles almost immediately.
Face* face_for(const std::string& path) {
    static std::unordered_map<std::string, Face> cache;
    auto it = cache.find(path);
    if (it != cache.end()) return it->second.face != nullptr ? &it->second : nullptr;

    Face& f = cache[path];
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        std::printf("stud: text overlay: cannot open font %s\n", path.c_str());
        std::fflush(stdout);
        return nullptr;
    }
    std::fseek(fp, 0, SEEK_END);
    long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size > 0) {
        f.bytes.resize(static_cast<size_t>(size));
        if (std::fread(f.bytes.data(), 1, f.bytes.size(), fp) != f.bytes.size()) f.bytes.clear();
    }
    std::fclose(fp);
    if (f.bytes.empty() || library() == nullptr) return nullptr;
    if (FT_New_Memory_Face(library(), f.bytes.data(), static_cast<FT_Long>(f.bytes.size()), 0,
                           &f.face) != 0) {
        f.face = nullptr;
        return nullptr;
    }
    return &f;
}

// One decoded code point and how far to advance after it.
struct Utf8Step {
    uint32_t cp;
    size_t next;
};

Utf8Step decode_utf8(const std::string& s, size_t i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) return {b0, i + 1};
    size_t extra = 0;
    uint32_t cp = 0;
    if ((b0 & 0xe0) == 0xc0) { extra = 1; cp = b0 & 0x1fu; }
    else if ((b0 & 0xf0) == 0xe0) { extra = 2; cp = b0 & 0x0fu; }
    else if ((b0 & 0xf8) == 0xf0) { extra = 3; cp = b0 & 0x07u; }
    else return {0xfffd, i + 1};
    if (i + extra >= s.size()) return {0xfffd, s.size()};
    for (size_t k = 1; k <= extra; ++k) {
        const auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xc0) != 0x80) return {0xfffd, i + 1};
        cp = (cp << 6) | (b & 0x3fu);
    }
    return {cp, i + extra + 1};
}

struct Overlay {
    wl_surface* surface = nullptr;
    wl_subsurface* subsurface = nullptr;
    wp_viewport* viewport = nullptr;
    wl_buffer* buffer = nullptr;
    void* pixels = nullptr;
    int fd = -1;
    size_t mapped = 0;
    int width = 0;
    int height = 0;
    bool mapped_visible = false;
    // The X11 path has no shm buffer: the pixels are ordinary memory
    // that XPutImage reads from. Everything above this line is Wayland's.
    std::vector<uint8_t> cpu_pixels;
    // The last layout drawn, so a click can be turned into a caret
    // position without re-deriving where every glyph landed.
    std::vector<std::pair<size_t, int>> pen;  // byte offset -> pen x, 26.6
    float origin_abs = 0.0f;                  // buffer px of the text's own origin
};

Overlay& overlay() {
    static Overlay o;
    return o;
}

std::mutex& state_mutex() {
    static std::mutex m;
    return m;
}

TextOverlaySpec& current() {
    static TextOverlaySpec s;
    return s;
}

bool& caret_on() {
    static bool on = true;
    return on;
}

void release_buffer(Overlay& o) {
    if (o.buffer != nullptr) {
        wl_buffer_destroy(o.buffer);
        o.buffer = nullptr;
    }
    if (o.pixels != nullptr) {
        ::munmap(o.pixels, o.mapped);
        o.pixels = nullptr;
        o.mapped = 0;
    }
    if (o.fd >= 0) {
        ::close(o.fd);
        o.fd = -1;
    }
}

bool ensure_buffer(Overlay& o, wl_shm* shm, int w, int h) {
    if (o.buffer != nullptr && o.width == w && o.height == h) return true;
    release_buffer(o);
    const size_t stride = static_cast<size_t>(w) * 4;
    const size_t size = stride * static_cast<size_t>(h);
    if (size == 0) return false;
    o.fd = ::memfd_create("stud-text-overlay", MFD_CLOEXEC);
    if (o.fd < 0) return false;
    if (::ftruncate(o.fd, static_cast<off_t>(size)) != 0) {
        ::close(o.fd);
        o.fd = -1;
        return false;
    }
    o.pixels = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, o.fd, 0);
    if (o.pixels == MAP_FAILED) {
        o.pixels = nullptr;
        ::close(o.fd);
        o.fd = -1;
        return false;
    }
    o.mapped = size;
    wl_shm_pool* pool = wl_shm_create_pool(shm, o.fd, static_cast<int32_t>(size));
    o.buffer = wl_shm_pool_create_buffer(pool, 0, w, h, static_cast<int32_t>(stride),
                                         WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    o.width = w;
    o.height = h;
    return o.buffer != nullptr;
}

// One glyph's coverage blended over whatever is already there, in
// premultiplied ARGB (what WL_SHM_FORMAT_ARGB8888 means).
void blend_pixel(uint32_t* px, int stride_px, int w, int h, int x, int y, uint8_t coverage,
                 uint32_t argb) {
    if (x < 0 || y < 0 || x >= w || y >= h || coverage == 0) return;
    const uint32_t sa = ((argb >> 24) & 0xffu) * coverage / 255u;
    if (sa == 0) return;
    const uint32_t sr = ((argb >> 16) & 0xffu) * sa / 255u;
    const uint32_t sg = ((argb >> 8) & 0xffu) * sa / 255u;
    const uint32_t sb = (argb & 0xffu) * sa / 255u;
    uint32_t& dst = px[static_cast<size_t>(y) * stride_px + x];
    const uint32_t da = (dst >> 24) & 0xffu;
    const uint32_t dr = (dst >> 16) & 0xffu;
    const uint32_t dg = (dst >> 8) & 0xffu;
    const uint32_t db = dst & 0xffu;
    const uint32_t inv = 255u - sa;
    dst = ((sa + da * inv / 255u) << 24) | ((sr + dr * inv / 255u) << 16) |
          ((sg + dg * inv / 255u) << 8) | (sb + db * inv / 255u);
}

// Lays the string out once so the caret's own x can be measured with the
// exact same advances the drawing pass uses.
struct Layout {
    std::vector<std::pair<size_t, int>> pen;  // byte offset -> pen x, in 26.6
    int total = 0;                            // 26.6
};

Layout measure(FT_Face face, const std::string& text, bool password, int extra_advance_26_6) {
    Layout out;
    int pen = 0;
    size_t i = 0;
    out.pen.emplace_back(0, 0);
    while (i < text.size()) {
        const Utf8Step step = decode_utf8(text, i);
        const uint32_t cp = password ? 0x2022u : step.cp;  // BULLET
        if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT) == 0) {
            pen += static_cast<int>(face->glyph->advance.x) + extra_advance_26_6;
        }
        i = step.next;
        out.pen.emplace_back(i, pen);
    }
    out.total = pen;
    return out;
}

void draw(const TextOverlaySpec& spec, Overlay& o, const WaylandOverlayDeps& deps) {
    auto* px = static_cast<uint32_t*>(o.pixels);
    std::memset(px, 0, o.mapped);

    Face* f = spec.font_path.empty() ? nullptr : face_for(spec.font_path);
    if (f == nullptr) return;
    FT_Face face = f->face;
    const float size_px = spec.pixel_size > 1.0f ? spec.pixel_size : 1.0f;
    // FT_Set_Pixel_Sizes takes whole pixels, and these sizes are not
    // whole: Roblox's own ratio makes a TextSize of 20 at density 1.25
    // come out at 20.68px, which truncates to 20 and draws visibly small.
    // FT_Set_Char_Size takes 26.6 fixed point, so at 72dpi one point is
    // one pixel and the fraction survives.
    //
    // The ratio itself is now understood exactly rather than taken on
    // faith: `fromRbxFontRatio` is `unitsPerEm / (hhea.ascender -
    // hhea.descender)` for every font in the table, checked directly
    // against the font files. So a Roblox TextSize IS the font's line
    // height in pixels, and the em to ask for is that times the ratio.
    if (FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(std::lround(size_px * 64.0f)), 72, 72) !=
        0) {
        return;
    }

    const int extra = static_cast<int>(spec.letter_spacing * size_px * 64.0f);
    const Layout layout = measure(face, spec.text, spec.password, extra);

    // Caret x, in 26.6, from the same advances the glyphs use.
    int caret_26_6 = layout.total;
    for (const auto& entry : layout.pen) {
        if (static_cast<int32_t>(entry.first) == spec.caret) {
            caret_26_6 = entry.second;
            break;
        }
    }

    // Roblox's own alignment: 0 left, 1 right, 2 centre for X; 0 top,
    // 1 centre, 2 bottom for Y.
    const float text_w = static_cast<float>(layout.total) / 64.0f;
    const float box_w = spec.width;
    float origin_xf = spec.residual_x;
    if (spec.x_alignment == 1) origin_xf = spec.residual_x + box_w - text_w;
    else if (spec.x_alignment == 2) origin_xf = spec.residual_x + (box_w - text_w) / 2.0f;

    // A box narrower than its text scrolls to keep the caret visible,
    // which is what any real text field does.
    if (text_w > box_w) {
        const float caret_x = static_cast<float>(caret_26_6) / 64.0f;
        float shift = 0.0f;
        if (caret_x + origin_xf > box_w - 2.0f) shift = caret_x + origin_xf - (box_w - 2.0f);
        else if (caret_x + origin_xf < 0.0f) shift = caret_x + origin_xf;
        origin_xf -= shift;
    }

    // Vertical placement comes from the font's DESIGN metrics, scaled
    // exactly, not from face->size->metrics, which FreeType grid-fits
    // (the ascender is rounded up to a whole pixel). That rounding makes
    // the line box taller than it really is and lifts centred text by
    // about a pixel, which is visible next to the engine's own.
    //
    // The line box is the font's ascender-to-descender span, which at
    // this em is exactly Roblox's own TextSize; see the ratio note
    // above.
    const auto upem = static_cast<float>(face->units_per_EM);
    float asc_px = size_px * static_cast<float>(face->ascender) / upem;
    float desc_px = size_px * static_cast<float>(face->descender) / upem;  // negative
    float line_px = asc_px - desc_px;
    // Roblox's line box, when it is known, rather than the font's span at
    // this em. The two are different once the em is TextSize (see
    // TextOverlaySpec::line_height): the font's own span is then taller
    // than TextSize, so centring against it dropped the baseline a pixel
    // or two below the engine's, visible as text sitting low against a
    // caret that was in the right place. The box is split by the font's
    // own ascent fraction, which is what puts the baseline back where it
    // was before the em changed.
    if (spec.line_height > 1.0f && line_px > 0.0f) {
        const float ascent_fraction = asc_px / line_px;
        line_px = spec.line_height;
        asc_px = line_px * ascent_fraction;
        desc_px = asc_px - line_px;
    }
    float top = spec.residual_y;
    if (spec.y_alignment == 1) top = spec.residual_y + (spec.height - line_px) / 2.0f;
    else if (spec.y_alignment == 2) top = spec.residual_y + spec.height - line_px;
    // Kept fractional: a baseline of 29.48 snapped to a whole row is a
    // visible pixel of lift next to the engine's own text, which places
    // glyphs at exact float positions. The fraction is handed to
    // FreeType as an outline delta below, so the glyph is rasterised at
    // its true position instead of being nudged to the grid.
    const float baseline_f = top + asc_px;
    const int baseline = static_cast<int>(std::floor(baseline_f));
    const auto baseline_frac_26_6 =
        static_cast<FT_Pos>(std::lround((baseline_f - static_cast<float>(baseline)) * 64.0f));
    const int line_h = static_cast<int>(std::lround(line_px));
    const int ascender = static_cast<int>(std::lround(asc_px));

    o.pen = layout.pen;
    o.origin_abs = spec.x - spec.residual_x + origin_xf;

    // Selection first, so the glyphs sit on top of it. Colour is the
    // text's own, at a low alpha: the engine does not report a selection
    // colour, and tinting the text's colour keeps it legible on whatever
    // background the box has.
    if (spec.selection_end > spec.selection_begin) {
        auto pen_at = [&](int32_t offset) {
            for (const auto& e : layout.pen) {
                if (static_cast<int32_t>(e.first) == offset) return e.second;
            }
            return layout.total;
        };
        const float sel_x0 = origin_xf + static_cast<float>(pen_at(spec.selection_begin)) / 64.0f;
        const float sel_x1 = origin_xf + static_cast<float>(pen_at(spec.selection_end)) / 64.0f;
        const int y0 = static_cast<int>(std::lround(top));
        const int y1 = static_cast<int>(std::lround(top + line_px));
        const uint32_t tint = (spec.argb & 0x00ffffffu) | 0x66000000u;
        for (int y = y0; y < y1; ++y) {
            for (int x = static_cast<int>(std::floor(sel_x0));
                 x < static_cast<int>(std::ceil(sel_x1)); ++x) {
                blend_pixel(px, o.width, o.width, o.height, x, y, 255, tint);
            }
        }
    }

    size_t i = 0;
    int pen = 0;
    while (i < spec.text.size()) {
        const Utf8Step step = decode_utf8(spec.text, i);
        const uint32_t cp = spec.password ? 0x2022u : step.cp;
        // Sub-pixel placement, both axes: the outline is shifted by the
        // fraction of a pixel this glyph really sits at before it is
        // rasterised. Down is negative in FreeType's own space, so the
        // baseline fraction is subtracted.
        const float glyph_xf = origin_xf + static_cast<float>(pen) / 64.0f;
        const int glyph_x = static_cast<int>(std::floor(glyph_xf));
        FT_Vector delta;
        delta.x = static_cast<FT_Pos>(
            std::lround((glyph_xf - static_cast<float>(glyph_x)) * 64.0f));
        delta.y = -baseline_frac_26_6;
        FT_Set_Transform(face, nullptr, &delta);
        if (FT_Load_Char(face, cp, FT_LOAD_RENDER) == 0) {
            const FT_GlyphSlot g = face->glyph;
            const int gx = glyph_x + g->bitmap_left;
            const int gy = baseline - g->bitmap_top;
            for (unsigned row = 0; row < g->bitmap.rows; ++row) {
                const unsigned char* src = g->bitmap.buffer + row * g->bitmap.pitch;
                for (unsigned col = 0; col < g->bitmap.width; ++col) {
                    blend_pixel(px, o.width, o.width, o.height, gx + static_cast<int>(col),
                                gy + static_cast<int>(row), src[col], spec.argb);
                }
            }
            pen += static_cast<int>(g->advance.x) + extra;
        }
        i = step.next;
    }

    // The face is cached across draws, so the per-glyph delta must not
    // outlive this one.
    FT_Set_Transform(face, nullptr, nullptr);

    if (caret_on() && spec.selection_end <= spec.selection_begin) {
        const int cx = static_cast<int>(std::lround(origin_xf +
                                                    static_cast<float>(caret_26_6) / 64.0f));
        const int top = baseline - ascender;
        const int caret_w = size_px >= 24.0f ? 2 : 1;
        for (int y = top; y < top + line_h; ++y) {
            for (int k = 0; k < caret_w; ++k) {
                blend_pixel(px, o.width, o.width, o.height, cx + k, y, 255, spec.argb);
            }
        }
    }
    (void)deps;
}

// Logical (surface-local) units from buffer pixels, a subsurface is
// positioned in the parent's coordinate space, which is logical, while
// everything the engine hands over is in buffer pixels.
double from_logical(int32_t logical, int32_t scale_120) {
    if (scale_120 <= 0) scale_120 = 120;
    return static_cast<double>(logical) * static_cast<double>(scale_120) / 120.0;
}

int32_t to_logical(int32_t buffer_px, int32_t scale_120) {
    if (scale_120 <= 0) scale_120 = 120;
    return static_cast<int32_t>((static_cast<int64_t>(buffer_px) * 120 + scale_120 / 2) /
                                scale_120);
}

// X11's half. The drawing below is shared, only where the pixels end
// up differs, so this is the same sequence with the Wayland surface
// work replaced by a child window, and without the logical-unit rounding
// (an X11 window is placed in real pixels, so there is no residual).
void apply_locked_x11(const TextOverlaySpec& spec) {
    Overlay& o = overlay();
    if (!spec.visible || spec.width <= 0.0f || spec.height <= 0.0f) {
        if (o.mapped_visible) {
            x11::hide_text_overlay();
            o.mapped_visible = false;
        }
        return;
    }
    // The spec is in the engine's buffer pixels; an X window is placed and
    // sized in the server's device pixels. They are the same number only
    // while the engine renders at the window's full resolution, which is
    // no longer true with HiDPI off or the upscaler running -- the box
    // then landed short of where the engine drew its own text and was
    // drawn smaller than it, by exactly the display's scale.
    //
    // Everything is scaled, geometry and type together, so the overlay is
    // rendered at the resolution it will actually be shown at rather than
    // drawn small and stretched. Wayland needs none of this: its
    // subsurface carries a buffer scale and the compositor maps it.
    const float measured = native_window_device_px_from_pointer(1.0f);
    const float to_device = measured > 0.0f ? measured : 1.0f;
    TextOverlaySpec drawn = spec;
    drawn.x *= to_device;
    drawn.y *= to_device;
    drawn.width *= to_device;
    drawn.height *= to_device;
    drawn.pixel_size *= to_device;
    drawn.line_height *= to_device;
    drawn.residual_x = 0.0f;
    drawn.residual_y = 0.0f;

    const int buf_w = static_cast<int>(std::ceil(drawn.width));
    const int buf_h = static_cast<int>(std::ceil(drawn.height));
    if (buf_w <= 0 || buf_h <= 0) return;
    o.cpu_pixels.assign(static_cast<size_t>(buf_w) * static_cast<size_t>(buf_h) * 4, 0);
    o.pixels = o.cpu_pixels.data();
    o.width = buf_w;
    o.height = buf_h;
    draw(drawn, o, WaylandOverlayDeps{});
    x11::present_text_overlay(o.pixels, o.width, o.height, static_cast<int>(drawn.x),
                              static_cast<int>(drawn.y));
    o.mapped_visible = true;
}

void apply_locked(const TextOverlaySpec& spec) {
    if (display_backend() == DisplayBackend::X11) {
        apply_locked_x11(spec);
        return;
    }
    Overlay& o = overlay();
    const WaylandOverlayDeps deps = overlay_deps();
    if (deps.compositor == nullptr || deps.subcompositor == nullptr || deps.shm == nullptr ||
        deps.parent == nullptr) {
        return;
    }

    if (!spec.visible || spec.width <= 0.0f || spec.height <= 0.0f) {
        if (o.surface != nullptr && o.mapped_visible) {
            // An attached null buffer is how a subsurface is hidden;
            // destroying it would lose the placement work.
            wl_surface_attach(o.surface, nullptr, 0, 0);
            wl_surface_commit(o.surface);
            wl_surface_commit(deps.parent);
            o.mapped_visible = false;
        }
        return;
    }

    if (o.surface == nullptr) {
        o.surface = wl_compositor_create_surface(deps.compositor);
        o.subsurface = wl_subcompositor_get_subsurface(deps.subcompositor, o.surface, deps.parent);
        // Desynchronised: the overlay redraws on its own keystrokes and
        // must not wait for the engine's next frame to appear.
        wl_subsurface_set_desync(o.subsurface);
        wl_subsurface_place_above(o.subsurface, deps.parent);
        if (deps.viewporter != nullptr) {
            o.viewport = wp_viewporter_get_viewport(deps.viewporter, o.surface);
        }
        // The overlay never wants pointer or touch events: clicks belong
        // to the engine underneath it.
        wl_region* empty = wl_compositor_create_region(deps.compositor);
        wl_surface_set_input_region(o.surface, empty);
        wl_region_destroy(empty);
    }

    // A subsurface can only be positioned on whole LOGICAL units, and one
    // logical unit is 1.25 real pixels here, so the surface is placed at
    // the largest logical position that does not overshoot, and whatever
    // fraction of a pixel is left over is carried into the drawing. That
    // is what makes the overlay land on the engine's own baseline instead
    // of near it.
    const int32_t logical_x = static_cast<int32_t>(std::floor(
        static_cast<double>(spec.x) * 120.0 / static_cast<double>(deps.scale_120 > 0 ? deps.scale_120 : 120)));
    const int32_t logical_y = static_cast<int32_t>(std::floor(
        static_cast<double>(spec.y) * 120.0 / static_cast<double>(deps.scale_120 > 0 ? deps.scale_120 : 120)));
    const float placed_x = static_cast<float>(from_logical(logical_x, deps.scale_120));
    const float placed_y = static_cast<float>(from_logical(logical_y, deps.scale_120));
    TextOverlaySpec drawn = spec;
    drawn.residual_x = spec.x - placed_x;
    drawn.residual_y = spec.y - placed_y;

    const int buf_w = static_cast<int>(std::ceil(spec.width + drawn.residual_x));
    const int buf_h = static_cast<int>(std::ceil(spec.height + drawn.residual_y));
    if (!ensure_buffer(o, deps.shm, buf_w, buf_h)) return;
    draw(drawn, o, deps);

    wl_subsurface_set_position(o.subsurface, logical_x, logical_y);
    if (o.viewport != nullptr) {
        wp_viewport_set_destination(o.viewport, to_logical(static_cast<int32_t>(o.width),
                                                           deps.scale_120),
                                    to_logical(static_cast<int32_t>(o.height), deps.scale_120));
    }
    wl_surface_attach(o.surface, o.buffer, 0, 0);
    wl_surface_damage_buffer(o.surface, 0, 0, o.width, o.height);
    wl_surface_commit(o.surface);
    // A subsurface's placement only takes effect on the PARENT's commit,
    // even a desynchronised one.
    wl_surface_commit(deps.parent);
    if (deps.display != nullptr) wl_display_flush(deps.display);
    o.mapped_visible = true;
}

}  // namespace

void set_text_overlay(const TextOverlaySpec& spec) {
    std::lock_guard<std::mutex> lock(state_mutex());
    current() = spec;
    caret_on() = true;
    apply_locked(current());
}

int32_t text_overlay_offset_at_x(float x) {
    std::lock_guard<std::mutex> lock(state_mutex());
    const Overlay& o = overlay();
    const TextOverlaySpec& spec = current();
    if (!spec.visible || o.pen.empty()) return 0;
    const float local = x - o.origin_abs;
    // Nearest boundary, not the character under the cursor: clicking the
    // right half of a character puts the caret after it, which is what a
    // real text field does.
    int32_t best = 0;
    float best_distance = 1e9f;
    for (const auto& e : o.pen) {
        const float px_pos = static_cast<float>(e.second) / 64.0f;
        const float distance = std::fabs(px_pos - local);
        if (distance < best_distance) {
            best_distance = distance;
            best = static_cast<int32_t>(e.first);
        }
    }
    return best;
}

void tick_text_overlay() {
    std::lock_guard<std::mutex> lock(state_mutex());
    if (!current().visible) return;
    caret_on() = !caret_on();
    apply_locked(current());
}

}  // namespace stud::android_glue
