#pragma once

#include <QString>

namespace stud::ui {

// `stud --install-desktop-entry`: registers the entry and icons under $XDG_DATA_HOME
// for the current launcher. An AppImage installs nothing, and without an entry the
// compositor can't match the app_id to an icon or open roblox:// links.
// Returns a process exit status.
int install_desktop_entry();

// The command that starts Stud: the AppImage file, or this binary.
QString launcher_command();

// `program` as one quoted Exec= argument, per the desktop entry spec.
QString desktop_exec_quote(const QString& program);

}  // namespace stud::ui
