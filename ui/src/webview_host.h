#pragma once

#include <QString>

// The client's web views, shown with QtWebEngine. Rosemary's WKWebView hands each
// page to this host over a local socket, and navigation, load events, script
// messages, dialogs and cookies go back the same way (crates/webkit/src/remote.rs
// in the Rosemary fork).

namespace stud::ui::webview {

// Listens once; returns the path for STUD_WEBVIEW_SOCKET, or empty on failure.
QString listen();

// `stud --webview-helper`, started by Rosemary: the same host for one client, on
// STUD_WEBVIEW_FD, drawing into Rosemary's compositor (WAYLAND_DISPLAY). Returns the exit code.
int run_helper(int argc, char** argv);

}  // namespace stud::ui::webview
