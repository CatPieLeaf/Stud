#pragma once

// Which ASCII characters the live keyboard layout can actually type.
//
// The engine's scan-code table is positional: it indexes straight to a USB
// HID usage code, so a key only means what the US layout puts where it
// sits. key_map.h therefore moves a key to the position that MEANS what it
// types, which is what made "/" open chat on a Brazilian ABNT2 keyboard.
//
// Moving a key vacates the position it came from, and that is only safe
// while some other key still types the character that position stands for.
// When none does, the position becomes unreachable and every binding on it
// is lost: ABNT2 types "'" where US has the grave, nothing on it types "`"
// with an ordinary keypress, and that is what stopped "'" from opening the
// Roblox inventory.
//
// Only the compositor knows the layout, so the set is built here, next to
// the keymap it hands over, and travels to the process that maps keys as
// HostInputEvent::layout_chars.

#include <xkbcommon/xkbcommon.h>

#include <cstdint>

namespace stud::android_glue {

// Sets bit c of out[c / 64] for every ASCII character `keymap` can type.
//
// Every key and every layout it defines, but only the two ordinary shift
// levels of each: a binding is something a player presses, so what counts
// is a plain press or shift and a press. The AltGr levels are real and
// deliberately left out, measured on ABNT2, where "`" exists only as
// AltGr and the acute key while "/" sits at level 0 on two separate keys.
//
// Dead keys resolve to no code point at all, which is one of the ways a
// layout ends up unable to type a character US has a whole key for.
inline void collect_reachable_ascii(xkb_keymap* keymap, std::uint64_t out[2]) {
    out[0] = 0;
    out[1] = 0;
    if (keymap == nullptr) return;
    const xkb_keycode_t min_code = xkb_keymap_min_keycode(keymap);
    const xkb_keycode_t max_code = xkb_keymap_max_keycode(keymap);
    for (xkb_keycode_t code = min_code; code <= max_code; ++code) {
        const xkb_layout_index_t layouts = xkb_keymap_num_layouts_for_key(keymap, code);
        for (xkb_layout_index_t layout = 0; layout < layouts; ++layout) {
            xkb_level_index_t levels = xkb_keymap_num_levels_for_key(keymap, code, layout);
            if (levels > 2) levels = 2;
            for (xkb_level_index_t level = 0; level < levels; ++level) {
                const xkb_keysym_t* syms = nullptr;
                const int count =
                    xkb_keymap_key_get_syms_by_level(keymap, code, layout, level, &syms);
                for (int i = 0; i < count; ++i) {
                    const std::uint32_t cp = xkb_keysym_to_utf32(syms[i]);
                    if (cp < 128) out[cp / 64] |= (1ull << (cp % 64));
                }
            }
        }
    }
}

}  // namespace stud::android_glue
