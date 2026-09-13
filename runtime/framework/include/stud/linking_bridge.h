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

}  // namespace stud::jni_bridge
