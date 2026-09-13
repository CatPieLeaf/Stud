#pragma once

#include <fake-jni/fake-jni.h>

#include <cstdint>
#include <string>
#include <vector>

#include "stud/linker.h"

// Real orchestration of NativeSettingsInterface's networking-adjacent
// native methods -- a distinct real Java class (com.roblox.engine.jni,
// not MainGameActivity's com.roblox.client.startup, see bootstrap.h) with
// its own real call site, ground-truth-traced from the app's own code
// (the app's own HTTP/cookie layer's T0()/P0()/R0() methods, class already familiar from the
// M4 desktop-spoof investigation -- same class, different methods this
// time):
//
//   nativeSetBaseUrl(String baseUrl, String apiBaseUrl)
//   nativeSetHttpClientProxy(String proxyHost, long proxyPort)
//   nativeSetMultipleCookies(String baseUrl, String cookieHeaderString)
//   nativeSetFilesDirectory(String filesDir)
//
// Same mechanism as run_bootstrap() (bootstrap.h): these are Java-declared
// native methods Roblox's own C++ implements (real symbols confirmed via
// nm against the real libroblox.so, e.g.
// Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetBaseUrl) --
// Stud's job is to CALL them, replacing the Java bootstrap code that
// normally would, not to implement them.
//
// Deliberately NOT included: nativeGetCookiesForDomain. Confirmed (grepped
// the whole app) to have zero Java-side callers anywhere in
// this build -- same dead-code pattern already documented for
// nativeIsLuaLoginEnabled in the engineering notes' M4 section. Real,
// implemented-in-libroblox.so function Stud would only ever call, never
// needs to for the real boot/play path.
//
// See the engineering notes, milestone M7.

namespace stud::jni_bridge {

struct NativeSettingsConfig {
    // Real defaults match the app's own HTTP layer ( "https://api." +
    // domain) -- Stud-controlled, not fabricated to impersonate anything;
    // these are just Roblox's own real, public API endpoints.
    std::string base_url = "https://www.roblox.com";
    std::string api_base_url = "https://apis.roblox.com";
    // Empty/0 matches Java's own System.getProperty(key, "") default when
    // no proxy is configured -- Stud has no proxy-config UI yet (M8).
    std::string http_proxy_host;
    int64_t http_proxy_port = 0;
    // Empty: honest "not logged in" state. Stud has no real WebView/
    // CookieManager yet (M8's QtWebEngine + QtKeychain work) -- once that
    // exists, the real post-login cookie string goes here instead of a
    // fabricated one.
    std::string cookies;
    // Real, persistent per-app data directory (matches Android's
    // Context.getFilesDir()) -- Roblox's own engine caches its fetched
    // client settings here (confirmed empirically: a real device/Stud run
    // writes <filesDir>/appData/ClientSettings/{IxpSettings,FlagsSet}.json
    // itself, via its own real networking, once given a real base URL and
    // a writable directory -- Stud doesn't need to fabricate this content).
    // Empty skips the call entirely (see run_native_settings_bootstrap's
    // own doc comment) -- callers that haven't set up a real persistent
    // directory yet get the previous, unchanged behavior.
    std::string files_directory;

    // Real, previously-missing gap closed (the engineering notes, "verify the
    // 2D shell renders" entry): real device FLog output
    // (`RbxStorage::getStorageInterface failed to initialize... Path
    // does not exist: ""`) showed Roblox's own local-storage subsystem
    // failing to init because nothing had ever called
    // nativeSetCacheDirectory -- confirmed via the app's own code
    // (the app's own HTTP/cookie layer's Q0(), NativeSettingsInterface.java) that this is a
    // real, distinct native method (matches Android's
    // Context.getCacheDir()), never wired alongside files_directory.
    // Same empty-skips-the-call convention as files_directory above.
    std::string cache_directory;

    // nativeSetBaseDataDirectories(internalDir, externalDir) -- real,
    // ground-truth-traced call site (InitHelper: internalDir =
    // Context.getDataDir(), externalDir = getExternalFilesDir(null)'s
    // PARENT directory). Both empty skips the call.
    std::string base_internal_directory;
    std::string base_external_directory;

    // nativeSetExternalDirectory(externalFilesDir) -- real, ground-
    // truth-traced call site (NativeHelper.java: Context.
    // getExternalFilesDir(null), the same real Android directory
    // base_external_directory above is derived from the PARENT of).
    // Empty skips the call.
    std::string external_directory;

    // Real, previously-missing cluster, all traced to a single real
    // function this session (the engineering notes, "instantiate controllers"
    // investigation): the app's own HTTP/cookie layer's T0(boolean) is the real, full
    // NativeSettingsInterface bootstrap sequence -- Stud had only ever
    // replicated 4 of its real calls (base URL, proxy, cookies, files/
    // cache directory). Real default values below match T0()'s own real
    // argument-computing helper methods exactly (w()/T()/J()/a1()), not
    // guessed. Unlike the directory fields above, these all have a real,
    // sensible, always-valid default (a literal filename, an empty
    // channel string, a real BuildConfig constant, a literal version
    // string already used elsewhere in this codebase) -- so they're
    // always called, not gated behind an emptiness check.
    std::string exception_reason_filename = "exception_reason.txt";
    std::string roblox_channel;
    std::string platform_name = "GoogleAndroidApp";
    // Filled at startup from the configured APK's real versionName
    // (main.cpp). Empty by default rather than seeded with a literal: a
    // manifest that could not be read should be visibly missing, not
    // silently reported as some build this project once saw.
    std::string roblox_version;
};

struct NativeSettingsResult {
    bool set_base_url_called = false;
    bool set_base_url_trapped_abort = false;
    bool set_http_client_proxy_called = false;
    bool set_http_client_proxy_trapped_abort = false;
    bool set_multiple_cookies_called = false;
    bool set_multiple_cookies_trapped_abort = false;
    bool set_files_directory_called = false;
    bool set_files_directory_trapped_abort = false;
    bool set_cache_directory_called = false;
    bool set_cache_directory_trapped_abort = false;
    bool set_base_data_directories_called = false;
    // Real InitHelper-order call (see native_settings.cpp): the engine
    // persists its cached user id here, which is what lets the Lua
    // app's account router pick Home over the logged-out Landing.
    bool set_preferences_file_called = false;
    bool set_preferences_file_trapped_abort = false;
    bool set_base_data_directories_trapped_abort = false;
    bool set_external_directory_called = false;
    bool set_external_directory_trapped_abort = false;
    bool set_exception_reason_filename_called = false;
    bool set_exception_reason_filename_trapped_abort = false;
    bool set_roblox_channel_called = false;
    bool set_roblox_channel_trapped_abort = false;
    bool override_channel_platform_name_called = false;
    bool override_channel_platform_name_trapped_abort = false;
    bool init_fast_log_called = false;
    bool init_fast_log_trapped_abort = false;
    bool set_roblox_version_called = false;
    bool set_roblox_version_trapped_abort = false;
};

// `jvm` supplies the JNIEnv* passed to each native call. `lib` must
// already be successfully loaded. Missing exported symbols are fatal
// (throws stud::linker::LoadError naming the symbol), same policy as
// run_bootstrap().
NativeSettingsResult run_native_settings_bootstrap(FakeJni::Jvm& jvm,
                                                    const stud::linker::LoadedLibrary& lib,
                                                    const NativeSettingsConfig& config = {});

// The engine's own live cookie jar for `url`, one Set-Cookie style
// header per cookie ("name=value; Domain=...; Path=/; Secure").
//
// This asks the engine, at the moment the answer is needed, rather than
// replaying anything Stud kept from earlier. That distinction is the
// whole point: Roblox supports several signed-in accounts and lets the
// user switch between them inside the app, and a switch replaces the
// session in this jar. Anything captured once at login is the account
// that was current *then*, so a panel opened after a switch would show
// the wrong account -- or, worse, act on it. Reading here always yields
// whichever account the app is signed in as right now.
//
// Returns empty if the engine has no jar for that URL, or if the real
// exported entry point is missing from this build. Cookie values are
// real credentials: nothing in the implementation logs one, and callers
// must not either.
std::vector<std::string> engine_cookies_for_url(FakeJni::Jvm& jvm,
                                                 const stud::linker::LoadedLibrary& lib,
                                                 const std::string& url);

}  // namespace stud::jni_bridge
