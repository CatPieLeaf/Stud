#pragma once

#include <string>

// The real system clipboard. Stud owns text editing while a Lua TextBox
// is focused (see stud/text_overlay.h), so copy and paste are Stud's job
// too, the same way they belong to the Android EditText on a device.
namespace stud::android_glue {

// Starts listening for what is on the clipboard. The compositor only tells a
// client that has a data device, so one made at the first paste has heard
// nothing yet and that paste comes back empty. Called once the seat is known.
void clipboard_start();

// Takes ownership of the selection and serves it to whoever pastes.
void clipboard_set_text(const std::string& text);

// Whatever the current selection holds as text, or empty if there is none
// (or nobody answers in time; this never blocks indefinitely). On X11 the
// answer arrives as an event, and `pump_x11` is how this drains them: it has
// to take whatever keeps the event pump to one thread at a time.
std::string clipboard_get_text(void (*pump_x11)());

}  // namespace stud::android_glue
