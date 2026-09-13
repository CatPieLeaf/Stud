#pragma once

#include <fake-jni/fake-jni.h>

#include <functional>
#include <string>

#include "stud/linker.h"

// The protocol behind "open this in a browser".
//
// Settings' footer links -- About Us, Careers, Parents and friends --
// are not web-view panels. The Lua app calls
// `GuiService:OpenBrowserWindow(url)`, which the engine routes to
// `LinkingProtocolCore::openUrl`, which publishes on the real
// `com.roblox.universalapp.linking` protocol and waits for the platform
// to say whether it handled it.
//
// Stud registered nothing for it, so those requests went nowhere: no
// window, no error, no log line -- the button simply did nothing.
// (Confirmed live: clicking one produces no WebView open request and no
// JNI miss, because the Lua side never publishes to a protocol nobody
// answers.)
//
// Every identifier is read from the engine's own exported getters
// (`getProtocolName`, `getOpenURLId`, `getUrlKey`, `getSuccessKey`),
// same as webview_bridge.h -- no wire name is written down here, so a
// future Roblox build moving one does not silently break this.

namespace stud::jni_bridge {

struct LinkingIds {
    std::string protocol;
    std::string open_url_id;
    std::string url_key;
    std::string success_key;
    bool resolved = false;
};

LinkingIds read_linking_ids(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib);

// Subscribes to the open-URL request. `on_open_url` is handed the real
// URL and returns whether it was handled, which is reported back to the
// engine so the Lua side can fall back rather than believe a lie.
bool run_linking_protocol_bootstrap(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                     std::function<bool(const std::string&)> on_open_url);

// The other half of this protocol: a URL the web view is about to
// navigate to, handed to the engine to see whether IT wants it.
//
// This is how a private server is joined on a real device, and it is
// not the hybrid JS bridge. The server list's own code
// (`ServerList.js`) only calls `Roblox.GameLauncher.joinPrivateGame` on
// a device whose `deviceType` is "computer" -- on a tablet or a phone it
// sets `window.location.href` to
// `/games/start?placeId=...&accessCode=...` instead. The real app never
// loads that page: its WebView's `shouldOverrideUrlLoading` returns true
// for every URL, publishes this protocol's isURLRegistered REQUEST, and
// on a "yes" publishes detectURL, which is what makes the engine launch
// the experience. On a "no" it loads the URL in the WebView as usual.
//
// Stud had neither half, so the navigation went to a page that tries to
// hand off to a desktop Roblox install through a protocol handler --
// nothing happened, and nothing was logged.
struct LinkingUrlIds {
    std::string protocol;
    std::string url_key;
    std::string detect_url_id;
    std::string is_url_registered_request_id;
    std::string is_url_registered_response_id;
    std::string is_registered_key;
    std::string matched_url_key;
    bool resolved = false;
};

// Subscribes to the isURLRegistered RESPONSE. The callback is handed the
// URL the answer is about and whether the engine claims it.
bool run_linking_url_detection_bootstrap(
    FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
    std::function<void(const std::string& url, bool registered)> on_answer);

// "Do you handle this URL?" -- answered asynchronously through the
// callback registered above.
void ask_engine_about_url(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                          const std::string& url);

// "Then handle it." Publishes detectURL, the real message that makes the
// engine act on a URL it recognised.
void hand_url_to_engine(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                        const std::string& url);

}  // namespace stud::jni_bridge
