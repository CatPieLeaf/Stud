#pragma once

#include <QString>

// The client's web views, shown with QtWebEngine.
//
// Rosemary implements Apple's WKWebView for the client; started by Stud, it
// hands every page to this host over a local socket instead of showing it
// itself. Each WKWebView becomes a window here, and everything the client
// expects back - navigation decisions, load events, script messages,
// JavaScript results, dialogs, cookies - goes back over the same socket.
// The protocol is described in Rosemary's crates/webkit/src/remote.rs, which
// packaging/rosemary/webview-host.patch adds.

namespace stud::ui::webview {

// Start listening, once; the socket's path, for STUD_WEBVIEW_SOCKET in the
// client's environment, or empty if no socket could be opened.
QString listen();

}  // namespace stud::ui::webview
