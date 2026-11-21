#pragma once

// Files that replace the client's content (textures, cursors, sounds, fonts), as
// Bloxstrap and Froststrap apply them. The folder mirrors the bundle's
// Contents/Resources; replaced files are kept and restored on removal.

#include "mac_client.h"

#include <QColor>
#include <QImage>
#include <QString>

#include <functional>

namespace stud::ui::customization {

QString folder(client::Product product);

// Restores the build, then copies the customization folder over it. A custom font
// becomes every text font family and a custom code font every monospace one; script
// fallbacks (Noto) and emoji keep Roblox's. On failure the build stays restored.
bool apply(const client::Build& build, client::Product product, QString* error);

// Generated customizations offered in Settings.
enum class Part { Cursor, ShiftLock, EmoteWheel, DeathSound, Font, MonoFont, AvatarBackground, CharacterSounds };

// An image for cursor or shift lock, .ogg for the death sound, .ttf/.otf for the fonts.
bool use_file(Part part, const QString& file, QString* error);

// Recolours the installed client's own images, keeping their shapes.
bool recolour(Part part, const QColor& colour, QString* error);

void remove(Part part);

// Bloxstrap's classic files, downloaded from its repository into the customizations
// folder: the old avatar editor background, the old character sounds, or the 2006 or
// 2013 cursor (`variant`). Nothing is written unless every file arrives; `done` gets
// an empty string or what failed.
void download_classic(Part part, const QString& variant, std::function<void(const QString& error)> done);

bool present(Part part);

// What a part has now, as Settings shows it: "Roblox's default", the chosen file's
// name, a recolour (with `colour` set) or a Bloxstrap classic.
struct Applied {
    QString text;
    QColor colour;
};
Applied applied(Part part);

// A picture part as the client will show it: the custom image, else the installed
// client's own. Null for parts without one, or before Roblox is installed.
QImage image(Part part);

// The custom font file a font part uses; empty while it is Roblox's own.
QString font_file(Part part);

// Bloxstrap's classic cursor `variant`, fetched to show before it is chosen.
void fetch_classic_cursor(const QString& variant, std::function<void(const QImage& image)> done);

// Crash guard: a run with customizations that differ from the last good set is a
// trial. If it crashes, the new or changed files move to disabled_folder() and the
// last good set returns; if it ends cleanly, it becomes the last good set.
// begin_run() after apply(); end_run() when the client exits returns what was set aside.
void begin_run(client::Product product);
QStringList end_run(client::Product product, bool crashed);

// e.g. ~/.config/stud/customizations-disabled
QString disabled_folder(client::Product product);

}  // namespace stud::ui::customization
