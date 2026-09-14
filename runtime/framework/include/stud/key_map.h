#pragma once

// Turning a real key press into what the engine expects.
//
// Two questions, and they have different answers: which Android key code
// this is (KEYCODE_SLASH), and which character it types ("/"). Wayland
// reports only an evdev scan code -- the same thing real Android reports
// as KeyEvent.getScanCode() -- plus, from the compositor's own keymap, the
// keysym and code point that scan code actually produces on the layout the
// user really has.
//
// Header-only so it can be tested directly against a real compiled keymap
// (tests/key_map_test.cpp), which is how the ABNT2 mappings below were
// established rather than assumed.

#include <cstdint>
#include <string>

namespace stud::jni_bridge {

// Real evdev scan code -> real Android KeyEvent key code. Wayland only
// ever reports the evdev code (which real Android reports identically as
// KeyEvent.getScanCode()), so the virtual key code has to be derived
// here. Covers the real printable/navigation/modifier set a login screen
// and ordinary gameplay use; anything not listed is still delivered with
// its real scan code and key code 0 rather than dropped.
inline std::int32_t android_key_code_for_scan_code(uint32_t scan) {
    switch (scan) {
        case 1: return 111;    // ESC -> KEYCODE_ESCAPE
        case 2: return 8;      // 1 -> KEYCODE_1
        case 3: return 9;
        case 4: return 10;
        case 5: return 11;
        case 6: return 12;
        case 7: return 13;
        case 8: return 14;
        case 9: return 15;
        case 10: return 16;    // 9
        case 11: return 7;     // 0 -> KEYCODE_0
        case 12: return 69;    // MINUS
        case 13: return 70;    // EQUAL
        case 14: return 67;    // BACKSPACE -> KEYCODE_DEL
        case 15: return 61;    // TAB
        case 16: return 45;    // Q -> KEYCODE_Q
        case 17: return 51;    // W
        case 18: return 33;    // E
        case 19: return 46;    // R
        case 20: return 48;    // T
        case 21: return 53;    // Y
        case 22: return 49;    // U
        case 23: return 37;    // I
        case 24: return 43;    // O
        case 25: return 44;    // P
        case 26: return 71;    // LEFTBRACE
        case 27: return 72;    // RIGHTBRACE
        case 28: return 66;    // ENTER
        case 29: return 113;   // LEFTCTRL
        case 30: return 29;    // A
        case 31: return 47;    // S
        case 32: return 32;    // D
        case 33: return 34;    // F
        case 34: return 35;    // G
        case 35: return 36;    // H
        case 36: return 38;    // J
        case 37: return 39;    // K
        case 38: return 40;    // L
        case 39: return 74;    // SEMICOLON
        case 40: return 75;    // APOSTROPHE
        case 41: return 68;    // GRAVE
        case 42: return 59;    // LEFTSHIFT
        case 43: return 73;    // BACKSLASH
        case 44: return 54;    // Z
        case 45: return 52;    // X
        case 46: return 31;    // C
        case 47: return 50;    // V
        case 48: return 30;    // B
        case 49: return 42;    // N
        case 50: return 41;    // M
        case 51: return 55;    // COMMA
        case 52: return 56;    // DOT
        case 53: return 76;    // SLASH
        case 54: return 60;    // RIGHTSHIFT
        case 56: return 57;    // LEFTALT
        case 57: return 62;    // SPACE
        case 58: return 115;   // CAPSLOCK
        case 97: return 114;   // RIGHTCTRL
        case 100: return 58;   // RIGHTALT
        case 102: return 122;  // HOME -> KEYCODE_MOVE_HOME
        case 103: return 19;   // UP
        case 104: return 92;   // PAGEUP
        case 105: return 21;   // LEFT
        case 106: return 22;   // RIGHT
        case 107: return 123;  // END -> KEYCODE_MOVE_END
        case 108: return 20;   // DOWN
        case 109: return 93;   // PAGEDOWN
        case 110: return 124;  // INSERT
        case 111: return 112;  // DELETE -> KEYCODE_FORWARD_DEL
        // KEYCODE_SLASH rather than KEYCODE_NUMPAD_DIVIDE, deliberately,
        // and this has now been confirmed against real hardware rather
        // than reasoned about twice.
        //
        // A Brazilian ABNT2 keyboard has a dedicated "/" key that is not
        // on the numpad, and the kernel reports it as KEY_KPSLASH. The
        // xkb layout data says ABNT2's slash is <AB11> (evdev 89), which
        // describes a DIFFERENT physical key -- so a previous pass
        // "corrected" this to KEYCODE_NUMPAD_DIVIDE on the strength of
        // that data and broke the key it was trying to fix. A real run on
        // a real ABNT2 keyboard settles it:
        //
        //   input bridge: nativePassKeyEvent path active
        //       (scan=98 keycode=154 unicode=47)
        //
        // -- evdev 98, typing "/" (U+002F). Evdev 89 is handled too, via
        // the keymap, for the keyboards that do report it.
        //
        // The cost is that a real numpad divide also reports as a slash.
        // That is the deliberate trade: the key people press to open chat
        // or search must work, and both keys genuinely type "/".
        case 98: return 76;    // KPSLASH -> KEYCODE_SLASH
        case 55: return 155;   // KPASTERISK -> KEYCODE_NUMPAD_MULTIPLY
        case 74: return 156;   // KPMINUS -> KEYCODE_NUMPAD_SUBTRACT
        case 78: return 157;   // KPPLUS -> KEYCODE_NUMPAD_ADD
        case 83: return 158;   // KPDOT -> KEYCODE_NUMPAD_DOT
        case 96: return 160;   // KPENTER -> KEYCODE_NUMPAD_ENTER
        case 79: return 145;   // KP1 -> KEYCODE_NUMPAD_1
        case 80: return 146;
        case 81: return 147;
        case 75: return 148;
        case 76: return 149;
        case 77: return 150;
        case 71: return 151;
        case 72: return 152;
        case 73: return 153;
        case 82: return 144;   // KP0 -> KEYCODE_NUMPAD_0
        // Deliberately absent: evdev 89 (<AB11>), the extra key some
        // layouts have. It types "/" on one and "\\" on another, so there
        // is no correct positional answer -- it has to come from the
        // keymap, which is what android_key_code_for_event() is for.
        default: return 0;
    }
}

// What the key PRODUCES, for the keys a scan code alone cannot answer.
//
// The table above is positional, so it can only ever describe one layout,
// and it describes a US one. A Brazilian ABNT2 keyboard puts "/" on evdev
// 89 -- a key US layouts do not have -- so the table returns 0, the engine
// receives keycode 0, and the "/" the engine's own on-screen hint offers
// for search does nothing at all. The same is true of every key any
// non-US layout moves.
//
// A keysym is what the compositor's real keymap says the key produces, so
// it is layout-independent by construction, and this is the mapping a real
// Android device performs too: its key layout files translate a scan code
// through the attached keyboard's layout to a keycode, rather than
// assuming a position. Only used when the positional table has no answer,
// so nothing that already worked changes.
inline std::int32_t android_key_code_for_keysym(uint32_t keysym) {
    // Latin-1 keysyms are their own code points, which covers everything
    // an ordinary printable key produces.
    if (keysym >= 'a' && keysym <= 'z') return static_cast<std::int32_t>(29 + (keysym - 'a'));
    if (keysym >= 'A' && keysym <= 'Z') return static_cast<std::int32_t>(29 + (keysym - 'A'));
    if (keysym >= '0' && keysym <= '9') return static_cast<std::int32_t>(7 + (keysym - '0'));
    switch (keysym) {
        case '/': return 76;    // KEYCODE_SLASH -- the one this exists for
        case '?': return 76;    // its shifted level, same physical key
        case ',': return 55;    // KEYCODE_COMMA
        case '.': return 56;    // KEYCODE_PERIOD
        case ';': return 74;    // KEYCODE_SEMICOLON
        case '\'': return 75;   // KEYCODE_APOSTROPHE
        case '`': return 68;    // KEYCODE_GRAVE
        case '-': return 69;    // KEYCODE_MINUS
        case '=': return 70;    // KEYCODE_EQUALS
        case '[': return 71;    // KEYCODE_LEFT_BRACKET
        case ']': return 72;    // KEYCODE_RIGHT_BRACKET
        case '\\': return 73;   // KEYCODE_BACKSLASH
        case ' ': return 62;    // KEYCODE_SPACE
        case '@': return 77;    // KEYCODE_AT
        case '+': return 81;    // KEYCODE_PLUS
        case '*': return 17;    // KEYCODE_STAR
        case '#': return 18;    // KEYCODE_POUND
        default: return 0;
    }
}

// The Android keycode for a real key event: what the key PRODUCES where
// the keymap can say, and where it SITS otherwise.
//
// That order is deliberate, and it is the one place Stud knowingly
// differs from a real Android device. Android is positional here: its key
// layout files map a scan code straight to a keycode (Generic.kl calls
// evdev 89 "RO"), and the layout only decides the character afterwards.
// Follow that and a Brazilian ABNT2 keyboard reports KEYCODE_RO for the
// key its own keycap prints "/" on, and the engine's own offer of "/" to
// search can never be taken -- which is exactly the behaviour this
// replaces. Worse, the positional table would keep insisting that ABNT2's
// evdev 53 is a slash when that key really types ";".
//
// Stud is a desktop client and its users press the key their keycap
// names, which is also how Roblox behaves on Windows. So the keymap wins
// wherever it has an answer.
//
// Everything the keysym table does not cover -- function and arrow keys,
// modifiers, the numpad (KP_Divide is not "/"), dead keys, AltGr output --
// falls through to the positional table untouched, so no mapping that
// already worked changes. Letters and digits resolve identically either
// way on any layout that keeps them where US does.
inline std::int32_t android_key_code_for_event(uint32_t scan_code, uint32_t keysym) {
    if (keysym != 0) {
        const std::int32_t from_layout = android_key_code_for_keysym(keysym);
        if (from_layout != 0) return from_layout;
    }
    return android_key_code_for_scan_code(scan_code);
}

// One code point as UTF-8. The engine takes a real string, and a
// Portuguese speaker's own keyboard produces plenty that does not fit in
// a byte -- "ç", every accented vowel.
inline std::string utf8_from_codepoint(uint32_t cp) {
    std::string out;
    if (cp == 0) return out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

}  // namespace stud::jni_bridge
