// What a key press becomes, checked against REAL compiled keymaps rather
// than against the same assumptions the mapping was written from.
//
// This exists because Stud shipped a purely positional, US-shaped table
// for a long time and it was wrong in both directions on a Brazilian
// ABNT2 keyboard: the "/" the engine's own on-screen hint offers for
// search sits on evdev 89, which no US layout has, so it mapped to
// nothing at all -- while evdev 53, which types ";" on ABNT2, was
// reported to the engine as KEYCODE_SLASH.
//
// xkbcommon compiles the same layout data the compositor hands Stud at
// runtime, so these are the real keysyms, not fixtures.

#include "stud/key_map.h"

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

// Android key codes, by name, so the expectations below read as intent.
constexpr std::int32_t kKeycodeA = 29;
constexpr std::int32_t kKeycodeW = 51;
constexpr std::int32_t kKeycodeSemicolon = 74;
constexpr std::int32_t kKeycodeSlash = 76;
constexpr std::int32_t kKeycodeNumpadDivide = 154;
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
    // nothing here -- this is the regression guard.
    check(android_key_code_for_scan_code(89) == 0,
          "br: the positional table genuinely cannot answer for evdev 89");

    // The other direction: evdev 53 is the slash key on US and the
    // semicolon key on ABNT2. It must not keep claiming to be a slash.
    check(br.codepoint(53) == ';', "br: evdev 53 really types ;");
    check(android_key_code_for_event(53, br.keysym(53)) == kKeycodeSemicolon,
          "br: the key that types ; reports KEYCODE_SEMICOLON, not SLASH");

    // The numpad divide also types "/", but it is a different key and
    // must stay a numpad key -- its keysym is KP_Divide, not "/", so it
    // falls through to the positional table.
    check(br.keysym(98) == XKB_KEY_KP_Divide, "br: evdev 98 is KP_Divide");
    check(android_key_code_for_event(98, br.keysym(98)) == kKeycodeNumpadDivide,
          "br: the numpad divide stays a numpad key");

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

    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("key_map_test: all checks passed\n");
    return 0;
}
