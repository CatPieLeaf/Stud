// What a key press becomes, checked against REAL compiled keymaps rather
// than against the same assumptions the mapping was written from.
//
// This exists because Stud shipped a purely positional, US-shaped table
// for a long time and it was wrong in both directions on a Brazilian
// ABNT2 keyboard: the "/" the engine's own on-screen hint offers for
// search sits on evdev 89, which no US layout has, so it mapped to
// nothing at all: while evdev 53, which types ";" on ABNT2, was
// reported to the engine as KEYCODE_SLASH.
//
// xkbcommon compiles the same layout data the compositor hands Stud at
// runtime, so these are the real keysyms, not fixtures.

#include "stud/key_map.h"
#include "stud/key_compose.h"

#include "stud/keymap_chars.h"

#include <xkbcommon/xkbcommon.h>

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

bool has_char(const uint64_t set[2], char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return (set[u / 64] & (1ull << (u % 64))) != 0;
}

void check_eq(const std::string& got, const std::string& want, const std::string& what) {
    if (got != want) {
        std::printf("FAIL: %s (got \"%s\", wanted \"%s\")\n", what.c_str(), got.c_str(),
                    want.c_str());
        ++g_failures;
    }
}

// Android key codes, by name, so the expectations below read as intent.
constexpr std::int32_t kKeycodeA = 29;
constexpr std::int32_t kKeycodeW = 51;
constexpr std::int32_t kKeycodeSemicolon = 74;
constexpr std::int32_t kKeycodeSlash = 76;
constexpr std::int32_t kKeycodeEscape = 111;
constexpr std::int32_t kKeycodeDpadLeft = 21;

struct Keymap {
    xkb_context* context = nullptr;
    xkb_keymap* keymap = nullptr;
    xkb_state* state = nullptr;

    bool open(const char* layout, const char* variant) {
        context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (context == nullptr) return false;
        xkb_rule_names names{};
        names.rules = "evdev";
        names.layout = layout;
        names.variant = variant;
        keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (keymap == nullptr) return false;
        state = xkb_state_new(keymap);
        return state != nullptr;
    }
    // Exactly what android-glue's resolve_key_from_keymap() does at
    // runtime, including the evdev-to-XKB offset of 8.
    uint32_t keysym(uint32_t evdev) const {
        return static_cast<uint32_t>(xkb_state_key_get_one_sym(state, evdev + 8));
    }
    uint32_t codepoint(uint32_t evdev) const {
        return xkb_state_key_get_utf32(state, evdev + 8);
    }
};

}  // namespace

int main() {
    using namespace stud::jni_bridge;

    Keymap br;
    if (!br.open("br", "abnt2")) {
        // Not a pass: the layout data this test exists to check is absent.
        std::printf("SKIP: no br(abnt2) layout data on this system\n");
        return 77;  // ctest's own "skipped" convention
    }

    // The bug this whole mapping exists for. ABNT2 puts "/" on evdev 89
    // (<AB11>), a key US layouts do not have at all.
    check(br.keysym(89) == '/', "br: evdev 89 really is the slash key");
    check(android_key_code_for_event(89, br.keysym(89)) == kKeycodeSlash,
          "br: the key that types / reports KEYCODE_SLASH");
    // The positional table alone is what used to answer, and it has
    // nothing here; this is the regression guard.
    check(android_key_code_for_scan_code(89) == 0,
          "br: the positional table genuinely cannot answer for evdev 89");

    // The other direction: evdev 53 is the slash key on US and the
    // semicolon key on ABNT2. It must not keep claiming to be a slash.
    check(br.codepoint(53) == ';', "br: evdev 53 really types ;");
    check(android_key_code_for_event(53, br.keysym(53)) == kKeycodeSemicolon,
          "br: the key that types ; reports KEYCODE_SEMICOLON, not SLASH");

    // The numpad divide also types "/", but it is a different key and
    // must stay a numpad key: its keysym is KP_Divide, not "/", so it
    // falls through to the positional table.
    // Evdev 98 is KP_Divide by keysym, but on a real ABNT2 keyboard it is
    // the dedicated "/" key rather than a numpad one, confirmed from a
    // live run (scan=98, unicode=47). It reports KEYCODE_SLASH so that the
    // key people actually press opens chat and search; see key_map.h.
    check(br.keysym(98) == XKB_KEY_KP_Divide, "br: evdev 98 is KP_Divide by keysym");
    check(android_key_code_for_event(98, br.keysym(98)) == kKeycodeSlash,
          "br: the key the kernel calls KPSLASH reports KEYCODE_SLASH");

    // Gameplay must not move. W and A sit where US puts them on ABNT2 and
    // resolve identically whichever way round the lookup goes.
    check(android_key_code_for_event(17, br.keysym(17)) == kKeycodeW, "br: W is still W");
    check(android_key_code_for_event(30, br.keysym(30)) == kKeycodeA, "br: A is still A");

    // Non-printing keys have no character to map, so they come from the
    // positional table exactly as before.
    check(android_key_code_for_event(1, br.keysym(1)) == kKeycodeEscape, "br: escape unchanged");
    check(android_key_code_for_event(105, br.keysym(105)) == kKeycodeDpadLeft,
          "br: left arrow unchanged");

    Keymap us;
    if (us.open("us", "")) {
        // Nothing about a US layout may change: it was already right.
        check(android_key_code_for_event(53, us.keysym(53)) == kKeycodeSlash,
              "us: evdev 53 is still the slash");
        check(android_key_code_for_event(39, us.keysym(39)) == kKeycodeSemicolon,
              "us: evdev 39 is still the semicolon");
        check(android_key_code_for_event(17, us.keysym(17)) == kKeycodeW, "us: W is still W");
        check(android_key_code_for_event(1, us.keysym(1)) == kKeycodeEscape,
              "us: escape unchanged");
    }

    // With no keymap at all (the X11 backend, or before wl_keyboard.keymap
    // arrives) the positional table has to remain the whole answer.
    check(android_key_code_for_event(53, 0) == kKeycodeSlash, "no keymap: falls back positionally");
    check(android_key_code_for_event(17, 0) == kKeycodeW, "no keymap: W still works");

    // A character that does not fit in a byte. ABNT2 has a dedicated
    // c-cedilla key, and typing it must not truncate.
    check(utf8_from_codepoint(0x00E7) == "\xC3\xA7", "c-cedilla encodes as two UTF-8 bytes");
    check(utf8_from_codepoint('/') == "/", "ASCII encodes as itself");
    check(utf8_from_codepoint(0).empty(), "no character produces no text");

    // Dead keys. The acute and tilde on ABNT2 type nothing by themselves
    // and only produce a character once the next key arrives. Composed
    // against the system's own Compose file, so these are the same
    // sequences every other application on the desktop performs.
    {
        using stud::android_glue::compose_key_press;
        using stud::android_glue::ComposeResult;
        xkb_compose_table* table = xkb_compose_table_new_from_locale(
            br.context, "en_US.UTF-8", XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (table == nullptr) {
            std::printf("note: no compose table for en_US.UTF-8; dead keys not checked\n");
        } else {
            xkb_compose_state* cs = xkb_compose_state_new(table, XKB_COMPOSE_STATE_NO_FLAGS);

            // "acute" then "a" is "a-acute": the first press types
            // nothing, the second carries the whole character.
            ComposeResult dead = compose_key_press(cs, XKB_KEY_dead_acute, 0);
            check(dead.types_nothing && dead.codepoint == 0,
                  "the acute dead key itself types nothing");
            ComposeResult done = compose_key_press(cs, XKB_KEY_a, 'a');
            check(done.codepoint == 0x00E1 && !done.types_nothing,
                  "acute then a composes to a-acute");

            // The tilde, which Portuguese needs at least as much.
            compose_key_press(cs, XKB_KEY_dead_tilde, 0);
            ComposeResult atilde = compose_key_press(cs, XKB_KEY_a, 'a');
            check(atilde.codepoint == 0x00E3, "tilde then a composes to a-tilde");

            // Cedilla, the other character an ABNT2 keyboard is shaped for
            // (it also has a dedicated key, which needs no composing).
            compose_key_press(cs, XKB_KEY_dead_cedilla, 0);
            ComposeResult ccedilla = compose_key_press(cs, XKB_KEY_c, 'c');
            check(ccedilla.codepoint == 0x00E7, "cedilla then c composes to c-cedilla");

            // A sequence the Compose file does not define produces
            // nothing rather than a stray character.
            compose_key_press(cs, XKB_KEY_dead_acute, 0);
            ComposeResult bad = compose_key_press(cs, XKB_KEY_q, 'q');
            check(bad.types_nothing && bad.codepoint == 0 && bad.text.empty(),
                  "an undefined sequence types nothing");

            // ...and the state recovers: an ordinary key straight after
            // is completely unaffected.
            ComposeResult plain = compose_key_press(cs, XKB_KEY_w, 'w');
            check(plain.codepoint == 'w' && !plain.types_nothing,
                  "an ordinary key after a cancelled sequence is untouched");

            // The whole point of the fallback argument: with no sequence
            // in play the keymap's own answer passes straight through.
            ComposeResult slash = compose_key_press(cs, '/', '/');
            check(slash.codepoint == '/' && !slash.types_nothing, "slash is unaffected by compose");

            // No compose table at all (a locale with no Compose file) must
            // behave exactly as before it existed.
            ComposeResult none = compose_key_press(nullptr, XKB_KEY_a, 'a');
            check(none.codepoint == 'a' && !none.types_nothing,
                  "with no compose table a key still types what the keymap says");

            xkb_compose_state_unref(cs);
            xkb_compose_table_unref(table);
        }
    }

    // What a key types, and the difference between "nothing" and
    // "don't know", which is what put a spurious "[" in front of every
    // accented character.
    {
        using stud::jni_bridge::text_from_keymap;
        std::string text;
        char none[12] = {};

        // A dead key mid-sequence: the keymap answered, and the answer is
        // that this key types nothing yet. The caller must NOT fall back
        // to its own layout table, on ABNT2 the dead acute sits at the
        // US "[" position and the dead tilde at the US "'" position, so
        // falling back types exactly the character the user reported
        // seeing: "[e-acute" instead of "e-acute".
        check(text_from_keymap(XKB_KEY_dead_acute, 0, none, sizeof(none), &text),
              "a dead key IS answered by the keymap");
        check(text.empty(), "a dead key types nothing at all");

        // The key that completes it carries the whole character.
        check(text_from_keymap(XKB_KEY_e, 0x00E9, none, sizeof(none), &text),
              "the completing key is answered");
        check_eq(text, "\xC3\xA9", "the completing key types the composed character");

        // An ordinary key.
        check(text_from_keymap(XKB_KEY_a, 'a', none, sizeof(none), &text), "an ordinary key");
        check_eq(text, "a", "an ordinary key types itself");

        // A multi-character composed result travels as text.
        char multi[12] = {'n', 'o', '\0'};
        check(text_from_keymap(XKB_KEY_a, 0, multi, sizeof(multi), &text), "a multi-char result");
        check_eq(text, "no", "a multi-character result is not truncated");

        // Only with NO keysym at all is the layout table the right answer,
        // which is the X11 backend and the moment before the keymap lands.
        check(!text_from_keymap(0, 0, none, sizeof(none), &text),
              "no keysym means no keymap, so the caller falls back");

        // A key that types nothing and is not a dead key, a function or
        // arrow key, is still answered, and still types nothing.
        check(text_from_keymap(XKB_KEY_Left, 0, none, sizeof(none), &text), "an arrow key");
        check(text.empty(), "an arrow key types nothing");
    }

    // The scan code the engine is handed. It indexes a table straight to a
    // USB HID usage code and the Android key code is never read, so the
    // scan code has to carry what the key TYPES. The expectations below
    // are the real table's own entries, read out of the binary:
    //
    //   evdev 17 -> 26 (HID W)          evdev 53 -> 56 (HID Slash)
    //   evdev 39 -> 51 (HID Semicolon)  evdev 98 -> 84 (HID Keypad Divide)
    {
        using stud::jni_bridge::engine_scan_code_for_event;

        // The bug this exists for: on ABNT2 "/" is evdev 98, which the
        // engine reads as a keypad divide and nothing is bound to.
        check(engine_scan_code_for_event(98, "/") == 53,
              "the key that types / is handed the slash position");
        check(engine_scan_code_for_event(98, "?") == 53,
              "its shifted level is the same physical key");

        // ...and the mirror: ABNT2's evdev 53 types ";", so it must not
        // keep claiming the slash position.
        check(engine_scan_code_for_event(53, ";") == 39,
              "the key that types ; is handed the semicolon position");

        // A US keyboard cannot move: every key already sits where its own
        // character says it should.
        check(engine_scan_code_for_event(53, "/") == 53, "us: slash stays put");
        check(engine_scan_code_for_event(39, ";") == 39, "us: semicolon stays put");
        check(engine_scan_code_for_event(17, "w") == 17, "us: W stays put");
        check(engine_scan_code_for_event(17, "W") == 17, "a shifted letter stays put");

        // Gameplay must not move on any layout.
        check(engine_scan_code_for_event(17, "w") == 17, "W is still W");
        check(engine_scan_code_for_event(30, "a") == 30, "A is still A");
        check(engine_scan_code_for_event(31, "s") == 31, "S is still S");
        check(engine_scan_code_for_event(32, "d") == 32, "D is still D");

        // The keypad keeps its own identity, a numpad digit is not the
        // digit row.
        check(engine_scan_code_for_event(82, "0") == 82, "numpad 0 stays on the numpad");
        check(engine_scan_code_for_event(79, "1") == 79, "numpad 1 stays on the numpad");
        check(engine_scan_code_for_event(78, "+") == 78, "numpad plus stays on the numpad");

        // A position whose character the layout cannot type with an
        // ordinary press must keep its key, or every binding on it is
        // lost. ABNT2 types "'" where US has the grave and reaches "`"
        // only through AltGr and the acute key, which is why "'" stopped
        // opening the Roblox inventory once keys began moving by
        // character. The sets below come from the real compiled keymaps.
        {
            Keymap abnt2_map;
            Keymap us_map;
            uint64_t abnt2[2] = {0, 0};
            uint64_t us[2] = {0, 0};
            const bool opened = abnt2_map.open("br", "abnt2") && us_map.open("us", "");
            check(opened, "both real keymaps compiled");
            if (opened) {
                stud::android_glue::collect_reachable_ascii(abnt2_map.keymap, abnt2);
                stud::android_glue::collect_reachable_ascii(us_map.keymap, us);

                check(!has_char(abnt2, '`'), "abnt2 cannot type ` with an ordinary press");
                check(has_char(abnt2, '/'), "abnt2 can type /");
                check(has_char(us, '`') && has_char(us, '\''), "us types both ` and '");

                check(engine_scan_code_for_event(41, "'", abnt2) == 41,
                      "abnt2: the grave position keeps its key");
                // The slash pair is unaffected: both layouts type "/".
                check(engine_scan_code_for_event(98, "/", abnt2) == 53,
                      "abnt2: / still reaches the slash position");
                check(engine_scan_code_for_event(53, ";", abnt2) == 39,
                      "abnt2: ; still reaches the semicolon position");

                check(engine_scan_code_for_event(41, "`", us) == 41, "us: grave stays put");
                check(engine_scan_code_for_event(40, "'", us) == 40, "us: apostrophe stays put");
                check(engine_scan_code_for_event(53, "/", us) == 53, "us: slash still stays put");
            }
        }

        // Nothing typed means nothing to place: dead keys, modifiers,
        // function and arrow keys all keep the position they came from.
        check(engine_scan_code_for_event(26, "") == 26, "a dead key keeps its position");
        check(engine_scan_code_for_event(1, "") == 1, "escape keeps its position");
        check(engine_scan_code_for_event(105, "") == 105, "the left arrow keeps its position");
        check(engine_scan_code_for_event(42, "") == 42, "shift keeps its position");

        // A character no US position types (ABNT2 has a dedicated cedilla
        // key) has nowhere to move to, so it stays where it is.
        check(engine_scan_code_for_event(39, "\xC3\xA7") == 39,
              "a character with no US position stays put");
    }

    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("key_map_test: all checks passed\n");
    return 0;
}
