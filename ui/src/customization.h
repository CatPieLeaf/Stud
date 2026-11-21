#pragma once

// Files that replace the client's content (textures, cursors, sounds, fonts), as
// Bloxstrap and Froststrap apply them. The folder mirrors the bundle's
// Contents/Resources; replaced files are kept and restored on removal.

#include "mac_client.h"

#include <QColor>
#include <QImage>
#include <QList>
#include <QPair>
#include <QString>
#include <QUrl>

#include <functional>

namespace stud::ui::customization {

QString folder(client::Product product);

// Restores the build, then copies the customization folder over it. A custom font
// becomes every text font family and a custom code font every monospace one; script
// fallbacks (Noto) and emoji keep Roblox's. On failure the build stays restored.
bool apply(const client::Build& build, client::Product product, QString* error);

// Generated customizations offered in Settings.
enum class Part { Cursor, ShiftLock, EmoteWheel, DeathSound, Font, MonoFont, AvatarBackground, CharacterSounds, Emoji };

// An image for cursor or shift lock, .ogg for the death sound, .ttf/.otf for the fonts
// and .ttf for the emoji.
bool use_file(Part part, const QString& file, QString* error);

// Recolours the installed client's own images, keeping their shapes.
bool recolour(Part part, const QColor& colour, QString* error);

void remove(Part part);

// A look another bootstrapper offers for a part, downloaded only once chosen.
struct Offer {
    QString name;
    QString source;
    QUrl preview;  // a picture of it, when there is one
    // Its files with their place in the content tree, or an archive laid out as one.
    QList<QPair<QString, QUrl>> files;
    QUrl archive;
};

// What Bloxstrap, Froststrap and Voidstrap offer for `part`, looked up in their
// repositories and catalogues; empty, with `error` set, when none could be reached.
void fetch_offers(Part part, std::function<void(const QList<Offer>& offers, const QString& error)> done);

// Downloads `offer` into the customizations folder; nothing is written unless every
// file arrives. `done` gets an empty string or what failed.
void install(Part part, const Offer& offer, std::function<void(const QString& error)> done);

// A picture from the network, such as an offer's preview.
void fetch_image(const QUrl& url, std::function<void(const QImage& image)> done);

// Froststrap's Old Login Screen: the pre-2026 login background and logo.
bool old_login_screen();
void set_old_login_screen(bool on, std::function<void(const QString& error)> done);

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

// The font file a font part uses: the custom one, and for the emoji also the installed
// client's own; empty otherwise.
QString font_file(Part part);

// Crash guard: a run with customizations that differ from the last good set is a
// trial. If it crashes, the new or changed files move to disabled_folder() and the
// last good set returns; if it ends cleanly, it becomes the last good set.
// begin_run() after apply(); end_run() when the client exits returns what was set aside.
void begin_run(client::Product product);
QStringList end_run(client::Product product, bool crashed);

// e.g. ~/.config/stud/customizations-disabled
QString disabled_folder(client::Product product);

}  // namespace stud::ui::customization
