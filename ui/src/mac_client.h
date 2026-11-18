#pragma once

// Roblox's macOS client, downloaded from Roblox's CDN and run on Rosemary in
// Stud's own prefix. Stud only writes ClientSettings/ClientAppSettings.json in it.

#include <QJsonObject>
#include <QString>

#include <functional>
#include <optional>

namespace stud::ui::client {

enum class Product { Player, Studio };

QString display_name(Product product);

// An installed build, e.g. "version-8001f8762ae44efe", "0.747.0.7471481".
struct Build {
    QString upload;
    QString version;
    QString app;  // .../RobloxPlayer.app
};

QString clients_dir(Product product = Product::Player);
QString prefix_dir();
// The client's home in the prefix: this user's folder, else the first besides Shared.
QString prefix_home();

std::optional<Build> installed(Product product = Product::Player);

// Percent is -1 when unknown.
using Progress = std::function<void(const QString& text, int percent)>;

// The build Roblox serves now, downloaded if needed; the installed one when
// offline. nullopt with `error` set when there's nothing to run.
std::optional<Build> ensure_current(Product product, const Progress& progress, QString* error);

// STUD_ROSEMARY, then the copy installed with Stud, then PATH.
QString find_rosemary();

// Creates the prefix on first use, which downloads the Darwin base system.
bool ensure_prefix(const QString& rosemary, const Progress& progress, QString* error);

// The release channel Roblox assigned this account, from the client's plist;
// empty for production. Stud installs that channel so Roblox's installer never runs.
QString assigned_channel(const QString& root, Product product);

// While the installer's downloads are in the prefix, the client hands every
// start to the installer and quits. Returns what was removed.
QStringList remove_installer_leftovers(const QString& root);

// CFBundleExecutable from Info.plist; empty when missing.
QString executable(const Build& build);

// Refuses any entry or symlink that would land outside `dir`. False when cancelled.
bool extract_zip(const QString& zip_path, const QString& dir, QString* error,
                 const std::function<void(int percent)>& step = {});

// Stops ensure_current() or ensure_prefix() and removes what they left half-done,
// for the rest of the process.
void cancel();
bool cancelled();

// One "<sha256>  <path>" line per file of Contents/MacOS, _CodeSignature and
// Info.plist, sorted. Excludes ClientSettings and Resources, which Stud changes.
QByteArray code_manifest(const QString& app);

// An empty object removes every override.
bool write_app_settings(const Build& build, const QJsonObject& flags, QString* error);

}  // namespace stud::ui::client
