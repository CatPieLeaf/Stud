#pragma once

#include <cstdint>
#include <string>

// The native text input the engine asks Stud for.
//
// A focused Lua TextBox does NOT draw its own text on Android: the engine
// sets an internal "a native input widget is showing my text" flag on
// focus, and from then on its text-drawing path substitutes an empty
// string until focus is lost. That is why `NativeTextBoxInfo` carries a
// rectangle, a font, a font size, a colour and alignments at all -- on a
// real device the app lays a real Android EditText over the GL view and
// that widget is what the user sees. Stud is the Android layer here, so
// supplying that widget is Stud's job; without it, typing lands in the
// engine (it does -- the text really is delivered) and simply never
// appears until the box is unfocused. See the text-input entry in
// the engineering notes for the full trace.
//
// This draws it as a real wl_subsurface above the game surface, in the
// engine's OWN font -- resolved from the same
// `assets/android/fonts/font-mappings.json` the real app uses -- so it
// looks like the TextBox it is standing in for rather than like a
// foreign widget.
namespace stud::android_glue {

struct TextOverlaySpec {
    bool visible = false;
    // The box, in engine/buffer pixels -- the same space
    // ANativeWindow_getWidth/getHeight report and the engine renders in.
    // Fractional on purpose: a box at y=10 in the engine's own
    // density-independent units is at 12.5 real pixels, and rounding that
    // away is a visible half-pixel of lift next to the engine's own text.
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    // Glyph height in buffer pixels, already scaled by the font's own
    // `fromRbxFontRatio` (see font-mappings.json) exactly as the real
    // app does before calling setTextSize().
    float pixel_size = 0.0f;
    // Real letter spacing as a fraction of the em, from the same source.
    float letter_spacing = 0.0f;
    uint32_t argb = 0xffffffffu;
    // Roblox's own TextXAlignment/TextYAlignment: 0 left/top, 1
    // right/centre, 2 centre/bottom (the order the real NativeTextBoxInfo
    // uses, confirmed against RbxKeyboard's own gravity mapping).
    int32_t x_alignment = 0;
    int32_t y_alignment = 0;
    // Caret position, as a byte offset into `text`.
    int32_t caret = 0;
    // Selected range, as byte offsets. Equal means no selection.
    int32_t selection_begin = 0;
    int32_t selection_end = 0;
    // A password box draws bullets, never the characters.
    bool password = false;
    // Sub-pixel remainder between where the box really is and where the
    // subsurface could be placed (a subsurface sits on whole logical
    // units). Filled in by the overlay itself, not by the caller.
    float residual_x = 0.0f;
    float residual_y = 0.0f;
    // Absolute path to the real font file, resolved by the caller from
    // Roblox's own extracted assets.
    std::string font_path;
    std::string text;
};

// Show, move, restyle or hide the overlay. Safe to call with the same
// spec repeatedly; only a real change redraws.
void set_text_overlay(const TextOverlaySpec& spec);

// Blink the caret. Called from whoever pumps Wayland; does nothing when
// the overlay is hidden.
void tick_text_overlay();

// Byte offset of the character nearest a given x, in buffer pixels and in
// the same space the box rectangle uses. This is where a click puts the
// caret, and it has to be answered here because this is where the font
// and the layout are.
int32_t text_overlay_offset_at_x(float x);

}  // namespace stud::android_glue
