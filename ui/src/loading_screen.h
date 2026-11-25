#pragma once

#include <QString>

#include <functional>
#include <string>

namespace stud::ui {

// The startup window: spinning stud, a line, a progress bar and Cancel.

// Rotates the loading lines every two seconds; Cancel closes and calls on_cancel.
void show_loading_lines(std::function<void()> on_cancel);

bool loading_cancelled();

// Replaces the random lines (showing the window if needed) until clear_loading_text().
void show_loading_text(const QString& text);

// 0-100, or -1 for an indeterminate bar.
void set_loading_progress(int percent);

// Back to the random lines if they were showing; otherwise closes.
void clear_loading_text();

void close_loading_screen();

}  // namespace stud::ui
