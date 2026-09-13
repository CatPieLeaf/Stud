#pragma once

namespace stud::ui {

// Registers Stud's desktop entry and icons in the user's own
// ~/.local/share tree, pointing at however Stud is being run right now.
//
// A package's own entry is installed system-wide and needs none of this.
// An AppImage is a single file that installs nothing, so nothing ties its
// windows back to an icon: Wayland has no window-icon protocol, and a
// compositor finds an application's icon by matching the surface's app_id
// against a .desktop file. Without an entry the taskbar shows a
// placeholder however correct the app_id is -- and `roblox://` links from
// a browser have nothing to open either.
//
// Writes only under $XDG_DATA_HOME, never system-wide, and only when
// asked (`stud --install-desktop-entry`).
//
// Returns a process exit status.
int install_desktop_entry();

}  // namespace stud::ui
