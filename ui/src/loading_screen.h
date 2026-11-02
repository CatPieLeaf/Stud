#pragma once

#include <QString>

#include <functional>

namespace stud::ui {

// The window shown while Stud starts: the spinning stud, one line under
// it, a loading bar and a Cancel button. One per process.

// Shows it with a line from resources/loading_lines.txt, a different one
// every two seconds, until close_loading_screen(). Cancel closes it and
// calls on_cancel.
void show_loading_lines(std::function<void()> on_cancel);

// Whether Cancel was pressed.
bool loading_cancelled();

// A line that says what is actually happening, shown instead of the random
// ones (and showing the window if it is not up) until clear_loading_text().
void show_loading_text(const QString& text);

// Back to the random lines if they were showing; otherwise the window goes.
void clear_loading_text();

void close_loading_screen();

}  // namespace stud::ui
