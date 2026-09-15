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
// UPDATE (this session): the real param names below were a first-pass
// guess from an earlier session with no the app's own code available, corrected now
// against `InitHelper.startAppBridge()`'s own real call
// site, `NativeAppBridgeInterface`'s own five-argument call. None of these five are launch-intent
// data at all. They're real, mundane app/device config values:
//
//   nativeAppBridgeAppStart(String baseUrl,        // base URL: "https://" + real configured host
//                            String userAgent,      // user agent: a real, built Android User-Agent string
//                            boolean someFlag,       // flag: a real compiled-in resources.arsc bool (rarely true)
//                            String androidId,       // android id: real Settings.Secure.ANDROID_ID
//                            String appUpgradeKey,   // upgrade key: BuildConfig.APP_UPGRADE_KEY, a real per-package build constant
//                            String /* unused, always "" */)
//
// Called once, unconditionally, on every real launch regardless of
// whether it's a deep-link launch (per startAppBridge()'s own real call
// site). Real Application-level engine bring-up, ahead of
// MainGameActivity's own onCreate-style flow (run_bootstrap()) and
// GameActivity's lifecycle. See the engineering notes, the entry connecting
// this to the "Create a new NativeEngine" real engine log line.
//
// Previously called with ALL FIVE real params as empty/false
// placeholders, an honest-at-the-time choice (real values weren't
// traced yet), but a real, confirmed-wrong input regardless: a real
// device never calls this with an empty base URL, empty Android ID, or
// empty app-upgrade-key. Now supplies real values for baseUrl, the
// flag's real compiled-in default, androidId (see
// stud::config::real_persistent_android_id()'s own doc comment for how
// this is honestly sourced, not fabricated), and appUpgradeKey (the
// real, confirmed against the app's own code constant "AppAndroidV"; see
// com/roblox/client/personasdk/BuildConfig.java, checked). userAgent
// is the one remaining honest empty placeholder: its real construction
// (built several real calls deep in the app's own HTTP layer) wasn't traced this
// session, left honestly empty rather than fabricated.

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
