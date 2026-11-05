#pragma once

#include <fake-jni/fake-jni.h>

#include <string>

#include "stud/linker.h"

// Real orchestration of NativeAppBridgeInterface's real
// nativeAppBridgeAppStart, a real Java-declared native method Roblox's
// own C++ implements, ground-truth-traced against the app's own code
// of `com.roblox.engine.jni.NativeAppBridgeInterface` and its one real
// caller, `InitHelper.startAppBridge()`.
//
// The six arguments are app and device configuration, not launch data:
//
//   nativeAppBridgeAppStart(String baseUrl,
//                            String userAgent,      // build_real_user_agent()
//                            boolean someFlag,      // a compiled-in bool, false
//                            String androidId,      // generated once, then kept
//                            String appUpgradeKey,  // the build's "AppAndroidV"
//                            String /* always "" */)
//
// Called once on every launch, ahead of the MainGameActivity and GameActivity
// bring-up.

namespace stud::jni_bridge {

struct AppBridgeResult {
    bool app_start_called = false;
    bool app_start_trapped_abort = false;
};

// `jvm` supplies the JNIEnv* passed to the call. `lib` must already be
// successfully loaded. `base_url` should be the same real base URL
// (e.g. "https://www.roblox.com") the rest of Stud's own bootstrap
// already uses, an empty string degrades to that same default.
// Missing exported symbol is fatal (throws stud::linker::LoadError
// naming the symbol), same policy as run_bootstrap()/
// run_native_settings_bootstrap().
// The real Roblox Android app's own User-Agent, rebuilt from the app's
// own code (the app's own User-Agent builder's format string, the app's own HTTP layer's arguments).
// Stud hosts the real, unmodified app, so this is what it honestly is,
// and the join API is one of the things that reads it. Everything Stud
// sends over HTTP should use this, never a Stud-specific placeholder.
std::string build_real_user_agent();

// The same agent with the real app's own web-view client token
// ("ROBLOX Android App"), which is what the app's WebView really sends.
// roblox.com serves the in-app version of a page only to this, and the
// login-challenge page's native transport depends on it.
std::string build_web_view_user_agent();

AppBridgeResult run_app_bridge_start(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                      const std::string& base_url = "");

}  // namespace stud::jni_bridge
