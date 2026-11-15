#pragma once

#include <QString>
#include <QStringList>

namespace stud::ui {

// The permissions Stud needs that a Flatpak sandbox was started without,
// read from its /.flatpak-info, whose [Context] already has every override
// (flatpak override, Flatseal) applied. Each is the `flatpak override` option
// that grants it back; empty when nothing is missing.
QStringList missing_flatpak_permissions(const QString& flatpak_info);

}  // namespace stud::ui
