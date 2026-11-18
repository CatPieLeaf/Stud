#pragma once

#include <QByteArray>
#include <QString>

#include <functional>

namespace stud::ui::launcher_portal {

// Adds a menu launcher through the portal's DynamicLauncher: the desktop asks the
// user (who may rename it) and writes the entry with a `flatpak run` Exec=. `exec`
// runs inside the sandbox. `done` gets the kept name, or false and a reason, empty
// when the user declined.
void install(const QString& id_suffix, const QString& name, const QString& exec, const QByteArray& png,
             std::function<void(bool, const QString&)> done);

// PNG, at most 512 pixels a side.
QByteArray icon_png(const QByteArray& image_file);

}  // namespace stud::ui::launcher_portal
