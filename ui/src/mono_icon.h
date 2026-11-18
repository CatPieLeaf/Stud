#pragma once

#include <QIcon>
#include <QStyle>

namespace stud::ui {

// Every button icon: the theme's `<name>-symbolic`, then `name`, then Qt's `fallback`,
// repainted in the palette's button-text colour (greyed when disabled) and re-read
// on each draw, so scheme switches repaint them. Themes' own glyphs have fixed colours.
QIcon mono_icon(const char* name, QStyle::StandardPixmap fallback);

// The same for a resource image, for logos no theme has (GitHub, Discord).
QIcon mono_resource_icon(const QString& path);

}  // namespace stud::ui
