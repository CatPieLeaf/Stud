#pragma once

#include <QString>

class QFrame;
class QWidget;

namespace stud::ui {

// A yellow warning: `text`, then `command` to run in a terminal, selectable and
// with a button that copies it.
QFrame* command_warning(QWidget* parent, const QString& text, const QString& command);

}  // namespace stud::ui
