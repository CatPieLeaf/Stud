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

// What Stud installs and runs: the Roblox player, or Roblox Studio.
enum class Product { Player, Studio };

// "Roblox Player" or "Roblox Studio", for the user.
QString display_name(Product product);

// One installed build: Roblox's upload name ("version-8001f8762ae44efe"), its
// version ("0.747.0.7471481") and the bundle to run.
struct Build {
    QString upload;
    QString version;
    QString app;  // .../RobloxPlayer.app
};

// Where each product's builds live, and Stud's Rosemary prefix, which both
// share.
QString clients_dir(Product product = Product::Player);
QString prefix_dir();

// The build installed now, if any.
std::optional<Build> installed(Product product = Product::Player);

// Status for the user while something slow happens: a line of text, and a
// percentage, or -1 when there is none.
using Progress = std::function<void(const QString& text, int percent)>;

// The build to run: the one Roblox serves now, downloaded if it is not the
// installed one. Offline, the installed build is kept. Empty, with `error` set,
// when there is nothing to run.
std::optional<Build> ensure_current(Product product, const Progress& progress, QString* error);

// The Rosemary executable: STUD_ROSEMARY, then the copy installed with Stud,
// then PATH. Empty when there is none.
QString find_rosemary();

// Creates Stud's prefix on first use (`rosemary prefix create`, which
// downloads the Darwin base system). False, with `error` set, on failure.
bool ensure_prefix(const QString& rosemary, const Progress& progress, QString* error);

// Puts Roblox back on the production channel, the one whose build Stud
// installs. Roblox can move a player to a test channel and saves it in
// com.roblox.<product>Channel.plist; seeing the installed build is not of that
// channel, the client would start Roblox's own installer in the prefix. A
// file naming any other channel is emptied, which Roblox reads as production.
// `root` is the prefix's root directory. Returns the channels taken back.
QStringList reset_release_channels(const QString& root);

// The bundle's executable, as its Info.plist names it (CFBundleExecutable).
// Empty when the bundle has none.
QString executable(const Build& build);

// Unpacks a downloaded zip into `dir`, refusing any entry or symbolic link
// that would land outside it. False, with `error` set, on failure.
bool extract_zip(const QString& zip_path, const QString& dir, QString* error);

// The client's code, as Roblox shipped it: one "<sha256>  <path>" line per
// file of the bundle's Contents/MacOS, Contents/_CodeSignature and its
// Info.plist, sorted, paths relative to the bundle. What Stud itself writes
// inside the bundle is left out: ClientSettings (the FFlags), and Resources,
// which customizations change on purpose. Empty when the bundle has no code.
QByteArray code_manifest(const QString& app);

// Writes ClientAppSettings.json inside the build: `flags` as given, an empty
// object removing every override.
bool write_app_settings(const Build& build, const QJsonObject& flags, QString* error);

}  // namespace stud::ui::client
