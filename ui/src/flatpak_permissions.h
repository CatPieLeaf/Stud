#pragma once

#include <QString>
#include <QStringList>

namespace stud::ui {

// Read from /.flatpak-info, whose [Context] has every override (flatpak override,
// Flatseal) applied. Each result is the `flatpak override` option that grants it.

// What Roblox can't run without; empty when nothing is missing.
QStringList missing_flatpak_permissions(const QString& flatpak_info);

// Opt-in features the manifest leaves out; empty once granted.
QStringList missing_discord_permissions(const QString& flatpak_info);
QStringList missing_controller_permissions(const QString& flatpak_info);

// The host command that grants `options` to this Flatpak.
QString flatpak_override_command(const QStringList& options);

}  // namespace stud::ui
