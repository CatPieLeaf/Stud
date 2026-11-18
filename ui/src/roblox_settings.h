#pragma once

#include <QString>

#include <optional>
#include <utility>

// Roblox's GlobalBasicSettings_<n>.xml in the prefix. Roblox rewrites it on exit,
// so changes made while it runs are lost.
namespace stud::ui::roblox_settings {

// The newest Player file; empty before Roblox writes one.
QString player_file();

// The element's type (bool, float, ...) and text.
std::optional<std::pair<QString, QString>> read(const QString& xml, const QString& name);

// Only replaces an existing element's text.
QString write(const QString& xml, const QString& name, const QString& text);

}  // namespace stud::ui::roblox_settings
