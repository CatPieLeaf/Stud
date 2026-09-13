#pragma once

#include <cstddef>
#include <string>

// The editing model behind the text box Stud draws itself.
//
// While a Lua TextBox is focused the engine stops drawing its own text and
// expects the platform's text widget to own the edit (see
// stud/text_overlay.h). On a device that widget is an Android EditText,
// which brings a caret, a selection, and the clipboard with it. This is
// that behaviour, in the one place that already knows the text.
//
// Offsets are byte offsets into UTF-8 text and always land on a character
// boundary.
namespace stud::jni_bridge {

class TextEditor {
public:
    void set_text(std::string text);
    const std::string& text() const { return text_; }

    int caret() const { return caret_; }
    int anchor() const { return anchor_; }
    bool has_selection() const { return caret_ != anchor_; }
    int selection_begin() const { return caret_ < anchor_ ? caret_ : anchor_; }
    int selection_end() const { return caret_ < anchor_ ? anchor_ : caret_; }
    std::string selected_text() const;

    // Every one of these returns whether the text itself changed, so the
    // caller only re-delivers to the engine when it has to.
    bool insert(const std::string& utf8);
    bool backspace();
    bool del();
    bool delete_selection();

    void move_left(bool select);
    void move_right(bool select);
    void move_home(bool select);
    void move_end(bool select);
    void select_all();
    void set_caret(int offset, bool select);

private:
    int step_left(int offset) const;
    int step_right(int offset) const;
    int clamp(int offset) const;

    std::string text_;
    int caret_ = 0;
    int anchor_ = 0;
};

}  // namespace stud::jni_bridge
