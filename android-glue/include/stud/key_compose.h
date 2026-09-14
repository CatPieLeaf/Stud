#pragma once

// Dead keys, composed against the system's own Compose file.
//
// On a Brazilian ABNT2 keyboard the acute and tilde are dead keys: they
// type nothing themselves, and the character only exists once the next
// key arrives ("acute" then "a" is "a-acute"). Resolving a key straight
// from the keymap gives neither half a typable character, which leaves a
// Portuguese speaker unable to write their own language.
//
// Split out of native_window.cpp so the state machine can be tested
// against a real compiled Compose table (tests/key_map_test.cpp) rather
// than only by pressing keys.

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

#include <cstdint>
#include <string>

namespace stud::android_glue {

// The longest composed result carried across the input IPC. Sized for the
// multi-character sequences standard Compose files produce; a longer one
// is dropped rather than cut in half, since half a character is worse
// than none.
inline constexpr std::size_t kComposedTextMax = 12;

struct ComposeResult {
    // Non-zero when this press produced exactly one character, which is
    // every ordinary key and nearly every compose sequence.
    std::uint32_t codepoint = 0;
    // Set only for the few sequences whose result is several characters.
    std::string text;
    // The sequence is still in progress, or was abandoned: this press
    // types nothing at all. The key event itself still happens.
    bool types_nothing = false;
};

// Advances the compose state by one key press and says what it typed.
//
// `fallback_codepoint` is what the keymap alone says the key produces,
// used whenever no sequence is involved. Feed presses only -- a release
// would advance a sequence a second time.
inline ComposeResult compose_key_press(xkb_compose_state* state, xkb_keysym_t keysym,
                                       std::uint32_t fallback_codepoint) {
    ComposeResult result;
    result.codepoint = fallback_codepoint;
    if (state == nullptr) return result;
    if (xkb_compose_state_feed(state, keysym) != XKB_COMPOSE_FEED_ACCEPTED) return result;

    switch (xkb_compose_state_get_status(state)) {
        case XKB_COMPOSE_COMPOSING:
            // Mid-sequence: the dead key types nothing, and the character
            // arrives with whatever key completes it.
            result.codepoint = 0;
            result.types_nothing = true;
            break;
        case XKB_COMPOSE_CANCELLED:
            // A combination the Compose file does not define ("acute"
            // then "q"). Producing nothing is what the rest of the
            // desktop does with it.
            xkb_compose_state_reset(state);
            result.codepoint = 0;
            result.types_nothing = true;
            break;
        case XKB_COMPOSE_COMPOSED: {
            const xkb_keysym_t composed = xkb_compose_state_get_one_sym(state);
            if (composed != XKB_KEY_NoSymbol) {
                result.codepoint = xkb_keysym_to_utf32(composed);
            } else {
                // No single keysym stands for it, so it is a
                // multi-character result and travels as text.
                result.codepoint = 0;
                char buffer[kComposedTextMax] = {};
                const int written = xkb_compose_state_get_utf8(state, buffer, sizeof(buffer));
                // A result too long to fit is reported without being
                // written; dropping it beats emitting half a character.
                if (written > 0 && static_cast<std::size_t>(written) < sizeof(buffer)) {
                    result.text.assign(buffer, static_cast<std::size_t>(written));
                } else {
                    result.types_nothing = true;
                }
            }
            xkb_compose_state_reset(state);
            break;
        }
        case XKB_COMPOSE_NOTHING:
        default:
            // An ordinary key, untouched by any sequence.
            break;
    }
    return result;
}

}  // namespace stud::android_glue
