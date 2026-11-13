#pragma once

// Roblox's macOS client, as Stud keeps and runs it.
//
// The client comes from Roblox's own CDN, the build Roblox currently serves
// for macOS, and is never modified: the only file Stud writes inside it is
// Contents/MacOS/ClientSettings/ClientAppSettings.json, the one Roblox reads
// local settings from. It runs on Rosemary (github.com/microlift/rosemary), in
// a prefix of Stud's own.

#include <QJsonObject>
#include <QString>

#include <functional>
#include <optional>

namespace stud::ui::client {

// One installed build: Roblox's upload name ("version-8001f8762ae44efe"), its
// version ("0.747.0.7471481") and the bundle to run.
struct Build {
    QString upload;
    QString version;
    QString app;  // .../RobloxPlayer.app
};

// Where the builds live, and Stud's Rosemary prefix.
QString clients_dir();
QString prefix_dir();

// The build installed now, if any.
std::optional<Build> installed();

// Status for the user while something slow happens: a line of text, and a
// percentage, or -1 when there is none.
using Progress = std::function<void(const QString& text, int percent)>;

// The build to run: the one Roblox serves now, downloaded if it is not the
// installed one. Offline, the installed build is kept. Empty, with `error` set,
// when there is nothing to run.
std::optional<Build> ensure_current(const Progress& progress, QString* error);

// The Rosemary executable: STUD_ROSEMARY, then the copy installed with Stud,
// then PATH. Empty when there is none.
QString find_rosemary();

// Creates Stud's prefix on first use (`rosemary prefix create`, which
// downloads the Darwin base system). False, with `error` set, on failure.
bool ensure_prefix(const QString& rosemary, const Progress& progress, QString* error);

// The bundle's executable, as its Info.plist names it (CFBundleExecutable).
// Empty when the bundle has none.
QString executable(const Build& build);

// Writes ClientAppSettings.json inside the build: `flags` as given, an empty
// object removing every override.
bool write_app_settings(const Build& build, const QJsonObject& flags, QString* error);

}  // namespace stud::ui::client
