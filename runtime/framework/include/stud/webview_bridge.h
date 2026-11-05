#pragma once

#include <functional>
#include <string>

#include <fake-jni/fake-jni.h>

#include "stud/linker.h"

namespace stud::jni_bridge {

// The real WebView surface is a MessageBus protocol, not a native call.
//
// On a real device `com.roblox.protocols.webview.WebViewProtocol` is
// constructed by the app's own DEX: it answers an "is this platform able
// to show a web view" request, subscribes to open/mutate/close messages,
// and opens `RobloxWebActivity` for the URL the Lua app sends. When the
// user closes that activity it publishes the protocol's own
// "window closed" message back, which is what makes the Lua app navigate
// away from the placeholder screen it was showing behind it.
//
// Stud runs no DEX, so none of that has ever happened: nothing answered
// the availability request and nothing was subscribed, which is exactly
// the reported symptom, opening Messages leaves a grey Roblox panel
// with no web view over it and no way back.
//
// Every string in the protocol (its name, each message id, each JSON
// key) comes from a real exported getter on that same class, so nothing
// here is a hardcoded literal that a future Roblox build could move.
struct WebViewProtocolIds {
    std::string protocol;
    std::string open_window_id;
    std::string mutate_window_id;
    std::string close_window_id;
    std::string is_available_id;
    std::string handle_window_close_id;
    std::string url_key;
    std::string title_key;
    // Which KIND of window the app is asking for. This is how it tells an
    // in-app panel apart from a link meant for the system browser,
    // the domain cannot, since blog.roblox.com is external and still ends
    // in .roblox.com.
    std::string window_type_key;
    std::string available_key;
    bool valid = false;
};

// Reads the protocol's own identifiers out of the engine.
WebViewProtocolIds read_webview_protocol_ids(FakeJni::Jvm& jvm,
                                             const stud::linker::LoadedLibrary& lib);

// Answers the availability request and subscribes to the open/close
// messages. `on_open` is handed the requested URL and title; `on_close`
// is the engine asking Stud to close whatever it opened.
bool run_webview_protocol_bootstrap(
    FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
    std::function<void(const std::string& url, const std::string& title)> on_open,
    std::function<void()> on_close);

// Tells the Lua app the web view the user was looking at has closed, so
// it stops waiting behind it. This is the half that makes the back
// navigation work rather than leaving a dead screen.
void publish_webview_closed(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib);

// The engine's OPEN_CAPTCHA_VIEW app-bridge notification: the security check
// it wants shown, opened like any other panel. When that panel closes,
// publish_webview_closed() also tells the engine it has focus back.
void open_security_check(const std::string& payload);

// The app reached its login or sign-up screen: use Roblox's own site instead.
// The app keeps switching accounts and logging out; see stud/web_origin.h.
// Does nothing with the webLogin setting off.
void open_login_page(const char* url, const char* title);
// Starts the login window hidden, for when one is about to be wanted: no
// session, or the account switcher open. open_login_page() then only shows it.
void prepare_login_page();
// Whether the panel being opened on this thread is that hidden window.
bool preparing_login();
// The viewer could not be started, so no closed event will ever come.
void login_window_failed();
// A panel other than the login window is opening. A hidden window would take
// its place, and none is prepared while it is open.
void note_panel_opening();
// How a hidden login window is told to show, given the page it is to show;
// false if there turned out to be no such window.
void set_login_page_shower(std::function<bool(const std::string&)> shower);
void set_web_login_enabled(bool enabled);

// Hands the engine one message a web view's page sent through its
// JavaScript bridge (`__globalRobloxAndroidBridge__.executeRoblox`).
// This is how a login challenge reports that it is finished: without it
// the user completes an OTP or a captcha and the app is never told, so
// the login never completes.
void signal_webview_javascript(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                const std::string& message);

// Answers NativeGLJavaInterface.getWebViewUserAgent() by calling the
// engine's own NativeGLInterface.setWebviewUserAgent with the agent
// Stud's viewer really sends (stud/webview_user_agent.h).
void report_webview_user_agent(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                               const std::string& agent);

}  // namespace stud::jni_bridge
