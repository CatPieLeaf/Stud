#pragma once

#include <string>

// The real system clipboard. Stud owns text editing while a Lua TextBox
// is focused (see stud/text_overlay.h), so copy and paste are Stud's job
// too, the same way they belong to the Android EditText on a device.
namespace stud::android_glue {

// Takes ownership of the selection and serves it to whoever pastes.
void clipboard_set_text(const std::string& text);

// Whatever the current selection holds as text, or empty if there is none
// (or nobody answers in time; this never blocks indefinitely).
std::string clipboard_get_text();

}  // namespace stud::android_glue
