#pragma once

// Customizations: files that replace the client's own content (textures, cursors,
// sounds, fonts), as Bloxstrap and Froststrap apply them. A customization folder mirrors
// the client's content tree (content/textures/..., content/sounds/...), which
// on macOS is the bundle's Contents/Resources. Only those content files change;
// the client's executable is never touched, and every replaced file is kept and
// put back when its customization is removed.

#include "mac_client.h"

#include <QColor>
#include <QString>

namespace stud::ui::customization {

// ~/.config/stud/customizations for the player, ~/.config/stud/studio-customizations for Studio.
QString folder(client::Product product);

// Puts the build back as it came, then copies the product's customization folder over
// it. A custom font (content/fonts/CustomFont.ttf or .otf) is pointed at by
// every font family, as Froststrap does. False, with `error` set, when a file
// could not be written; the build is then left as it came.
bool apply(const client::Build& build, client::Product product, QString* error);

// The generated customizations Settings offers, written into the player's customization folder.
enum class Part { Cursor, ShiftLock, EmoteWheel, DeathSound, Font };

// Uses `file` for `part`: an image for the cursor or shift lock, an .ogg for
// the death sound, a .ttf or .otf for the font.
bool use_file(Part part, const QString& file, QString* error);

// Recolours `part`'s images from the installed client's own, keeping their
// shapes: the cursor, the shift lock and the emote wheel.
bool recolour(Part part, const QColor& colour, QString* error);

// Removes what use_file() or recolour() made for `part`.
void remove(Part part);

// Whether the customization folder holds `part` now.
bool present(Part part);

}  // namespace stud::ui::customization
