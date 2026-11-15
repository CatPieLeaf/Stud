#pragma once

#include <QString>

#include <optional>
#include <utility>

// Roblox's own settings file in the prefix, GlobalBasicSettings_<n>.xml:
// the volume, sensitivity and the rest its in-game menu saves. Roblox writes
// it when it closes, so a change made while it runs is lost.
namespace stud::ui::roblox_settings {

// The Player's file, the most recently written one; empty when Roblox has not
// written one yet.
QString player_file();

// The type (bool, float, int, ...) and text of the element named `name`.
std::optional<std::pair<QString, QString>> read(const QString& xml, const QString& name);

// `xml` with that element's text replaced by `text`. Only an element already
// there is changed.
QString write(const QString& xml, const QString& name, const QString& text);

}  // namespace stud::ui::roblox_settings
