#pragma once

#include <QByteArray>
#include <QString>

#include <functional>

namespace stud::ui::launcher_portal {

// Adds a launcher to the desktop's application menu through the desktop
// portal's DynamicLauncher, the way a sandboxed app can: the desktop asks the
// user to confirm (they may rename it), then writes the entry and its icon
// itself, its Exec= run through `flatpak run`. `exec` is the command inside
// the sandbox; `png` the icon, at most 512 pixels a side. `done` gets true
// and the name the user kept, or false and why; an empty reason is the user
// declining.
void install(const QString& id_suffix, const QString& name, const QString& exec, const QByteArray& png,
             std::function<void(bool, const QString&)> done);

// `image` as the portal takes an icon: PNG, at most 512 pixels a side.
QByteArray icon_png(const QByteArray& image_file);

}  // namespace stud::ui::launcher_portal
