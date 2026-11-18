#pragma once

// Files that replace the client's content (textures, cursors, sounds, fonts), as
// Bloxstrap and Froststrap apply them. The folder mirrors the bundle's
// Contents/Resources; replaced files are kept and restored on removal.

#include "mac_client.h"

#include <QColor>
#include <QString>

namespace stud::ui::customization {

QString folder(client::Product product);

// Restores the build, then copies the customization folder over it. A custom font
// becomes every font family, as in Froststrap. On failure the build stays restored.
bool apply(const client::Build& build, client::Product product, QString* error);

// Generated customizations offered in Settings.
enum class Part { Cursor, ShiftLock, EmoteWheel, DeathSound, Font };

// An image for cursor or shift lock, .ogg for the death sound, .ttf/.otf for the font.
bool use_file(Part part, const QString& file, QString* error);

// Recolours the installed client's own images, keeping their shapes.
bool recolour(Part part, const QColor& colour, QString* error);

void remove(Part part);

bool present(Part part);

// Crash guard: a run with customizations that differ from the last good set is a
// trial. If it crashes, the new or changed files move to disabled_folder() and the
// last good set returns; if it ends cleanly, it becomes the last good set.
// begin_run() after apply(); end_run() when the client exits returns what was set aside.
void begin_run(client::Product product);
QStringList end_run(client::Product product, bool crashed);

// e.g. ~/.config/stud/customizations-disabled
QString disabled_folder(client::Product product);

}  // namespace stud::ui::customization
