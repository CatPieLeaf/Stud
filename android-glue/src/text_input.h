#pragma once

#include <cstdint>
#include <string>

struct wl_display;
struct wl_registry;
struct wl_seat;

// Input-method support: what lets a Chinese, Japanese or Korean input
// method compose text into a focused Roblox TextBox.
//
// A phone does this through the soft keyboard the engine asks for with
// NativeHelper.showKeyboard; a desktop does it through the compositor's
// text-input protocol (Wayland) or XIM (X11). Either way the input method
// is only engaged while Stud's text overlay is up -- a TextBox has focus --
// so an input method left in, say, pinyin mode never eats the keys a game
// is played with.
//
// Committed text goes to Process B as HostInputEvent::kTextCommit and is
// inserted by the same editor typed keys use. Text still being composed
// (the preedit) never reaches the engine: the overlay draws it, exactly as
// a composing region on Android belongs to the IME, not the app.
namespace stud::android_glue {

// Wayland. Called from the registry listener; the text input itself is
// created once both the manager and the seat are known, in either order.
void text_input_bind_manager(wl_display* display, wl_registry* registry, uint32_t name,
                             uint32_t version);
void text_input_attach_seat(wl_seat* seat);

// Where input should go, from the text overlay after every redraw: whether
// a TextBox has focus, whether it is a password box (input methods are
// told to stay out of those), and the caret, in the window surface's
// logical coordinates, which is where the candidate window is placed.
void text_input_set_target(bool active, bool sensitive, int32_t x, int32_t y, int32_t width,
                           int32_t height);

// Shared by both backends: text an input method committed, and a request
// to delete text around the caret before it, handed to Process B.
void text_input_push_commit(const std::string& utf8);
void text_input_push_delete_surrounding(uint32_t before_bytes, uint32_t after_bytes);

}  // namespace stud::android_glue
