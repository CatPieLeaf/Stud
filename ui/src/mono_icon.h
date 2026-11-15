#pragma once

#include <QIcon>
#include <QStyle>

namespace stud::ui {

// Every icon on Stud's buttons comes from here, so all of them look alike:
// one colour, the palette's button text, and greyed out on a disabled
// button. The colour is read each time the icon is drawn, so a switch
// between a light and a dark scheme repaints them with no help.
//
// The glyph is the icon theme's monochrome one, `<name>-symbolic`, then its
// plain `name`, then Qt's own `fallback`. A theme's monochrome glyph is a
// fixed colour (Breeze draws them dark, and only KDE's own libraries
// recolour them), and some of its plain icons are in full colour, so
// whichever is found is repainted in one colour, keeping only its shape.
QIcon mono_icon(const char* name, QStyle::StandardPixmap fallback);

// Stud's own picture at `path` (a resource), drawn the same way: only its
// shape is kept, in the palette's button-text colour. For logos no icon
// theme has, such as GitHub's and Discord's.
QIcon mono_resource_icon(const QString& path);

}  // namespace stud::ui
