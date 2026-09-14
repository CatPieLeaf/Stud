#pragma once

#include <string>

// The user agent a Roblox page sees inside Stud's web view.
//
// Shared deliberately. The engine asks for this string
// (NativeGLJavaInterface.getWebViewUserAgent) and sends it to Roblox when
// it creates a login challenge; the page that answers the challenge then
// runs in the viewer under whatever the viewer actually sends. If the two
// differ, or if the engine is told nothing, which is what Stud used to
// do, the challenge is created against one client and answered by
// another, and the page fails with "something went wrong".
//
// So both halves build it here, from the same inputs.
namespace stud::webview {

// QtWebEngine's own Chrome version, pinned rather than read, because the
// engine has to be told this string before any viewer exists to ask. It
// only has to be true of the viewer, not of the machine: both sides use
// this one function, so they cannot disagree with each other.
inline constexpr const char* kChromeVersion = "Chrome/140.0.0.0";

// `app_token` is the app's own identifier, e.g. "RobloxApp/2.737.1584",
// what makes roblox.com serve its app layout rather than the full site.
inline std::string user_agent(const std::string& app_token) {
    std::string agent = "Mozilla/5.0 (Linux; Android 14) AppleWebKit/537.36 (KHTML, like Gecko) ";
    agent += kChromeVersion;
    agent += " Mobile Safari/537.36";
    if (!app_token.empty()) {
        agent += ' ';
        agent += app_token;
    }
    return agent;
}

}  // namespace stud::webview
