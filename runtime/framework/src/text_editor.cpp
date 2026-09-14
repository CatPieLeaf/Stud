#include "stud/text_editor.h"

namespace stud::jni_bridge {

namespace {
bool is_continuation(unsigned char c) { return (c & 0xc0) == 0x80; }
}  // namespace

void TextEditor::set_text(std::string text) {
    text_ = std::move(text);
    caret_ = static_cast<int>(text_.size());
    anchor_ = caret_;
}

int TextEditor::clamp(int offset) const {
    if (offset < 0) return 0;
    if (offset > static_cast<int>(text_.size())) return static_cast<int>(text_.size());
    // Never leave an offset in the middle of a multi-byte character.
    while (offset > 0 && is_continuation(static_cast<unsigned char>(text_[offset]))) --offset;
    return offset;
}

int TextEditor::step_left(int offset) const {
    if (offset <= 0) return 0;
    --offset;
    while (offset > 0 && is_continuation(static_cast<unsigned char>(text_[offset]))) --offset;
    return offset;
}

int TextEditor::step_right(int offset) const {
    const int size = static_cast<int>(text_.size());
    if (offset >= size) return size;
    ++offset;
    while (offset < size && is_continuation(static_cast<unsigned char>(text_[offset]))) ++offset;
    return offset;
}

std::string TextEditor::selected_text() const {
    if (!has_selection()) return {};
    return text_.substr(static_cast<size_t>(selection_begin()),
                        static_cast<size_t>(selection_end() - selection_begin()));
}

bool TextEditor::delete_selection() {
    if (!has_selection()) return false;
    const int begin = selection_begin();
    text_.erase(static_cast<size_t>(begin), static_cast<size_t>(selection_end() - begin));
    caret_ = begin;
    anchor_ = begin;
    return true;
}

bool TextEditor::insert(const std::string& utf8) {
    if (utf8.empty() && !has_selection()) return false;
    delete_selection();
    text_.insert(static_cast<size_t>(caret_), utf8);
    caret_ += static_cast<int>(utf8.size());
    anchor_ = caret_;
    return true;
}

bool TextEditor::backspace() {
    if (delete_selection()) return true;
    if (caret_ <= 0) return false;
    const int from = step_left(caret_);
    text_.erase(static_cast<size_t>(from), static_cast<size_t>(caret_ - from));
    caret_ = from;
    anchor_ = from;
    return true;
}

bool TextEditor::del() {
    if (delete_selection()) return true;
    if (caret_ >= static_cast<int>(text_.size())) return false;
    const int to = step_right(caret_);
    text_.erase(static_cast<size_t>(caret_), static_cast<size_t>(to - caret_));
    anchor_ = caret_;
    return true;
}

void TextEditor::move_left(bool select) {
    // Without shift, a selection collapses to its near edge rather than
    // moving the caret, what every real text field does.
    if (!select && has_selection()) {
        caret_ = selection_begin();
    } else {
        caret_ = step_left(caret_);
    }
    if (!select) anchor_ = caret_;
}

void TextEditor::move_right(bool select) {
    if (!select && has_selection()) {
        caret_ = selection_end();
    } else {
        caret_ = step_right(caret_);
    }
    if (!select) anchor_ = caret_;
}

void TextEditor::move_home(bool select) {
    caret_ = 0;
    if (!select) anchor_ = caret_;
}

void TextEditor::move_end(bool select) {
    caret_ = static_cast<int>(text_.size());
    if (!select) anchor_ = caret_;
}

void TextEditor::select_all() {
    anchor_ = 0;
    caret_ = static_cast<int>(text_.size());
}

void TextEditor::set_caret(int offset, bool select) {
    caret_ = clamp(offset);
    if (!select) anchor_ = caret_;
}

}  // namespace stud::jni_bridge
