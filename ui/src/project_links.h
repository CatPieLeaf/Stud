#pragma once

#include <QPushButton>

namespace stud::ui {

// Stud's GitHub repository and Discord server, as buttons with their logos:
// one colour, the palette's, like every other icon (see mono_icon.h). With
// `labelled` false the button is the logo alone, its name in the tooltip.
QPushButton* github_button(QWidget* parent, bool labelled = true);
QPushButton* discord_button(QWidget* parent, bool labelled = true);

}  // namespace stud::ui
