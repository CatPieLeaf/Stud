#pragma once

#include <fake-jni/fake-jni.h>

#include <string>

#include "stud/linker.h"

// Real fix for task #8 (the engineering notes): confirmed via a real, working
// Sober install's own logs (`data/sober/sober_logs/*.log`,
// `[FLog::AndroidGLView] nativeInitClientSettings` firing at t=2.44s
// into a real cold boot, immediately followed by
// `nativePostClientSettingsLoadedInitialization3`) that a real device's
// JAVA side fetches ClientSettings content over HTTP itself (via
// the app's own app-shell manager's real AsyncTask, ground-truth-traced from the app
// itself many sessions ago) and hands the ALREADY-FETCHED content to
// nativeInitClientSettings(). Roblox's own native code never does
// this fetch itself.
//
// Stud's architecture deliberately bypasses all Java/Kotlin (the
// locked "Universal App" decision; see the engineering notes' M1 section), so
// there's no Java AsyncTask to do this fetch.
//
// Real, ground-truth-traced endpoint (the app's own HTTP/cookie layer,
// methods, `BuildConfig.CLIENT_SETTINGS_GROUPNAME = "GoogleAndroidApp"`):
//   https://clientsettingscdn.roblox.com/v2/settings/application/GoogleAndroidApp
//
// The actual HTTP GET no longer happens here: it's done once by the UI
// process (a real glibc process, already doing other network-adjacent
// work for the login flow) before the runtime process even starts, and
// handed over via stud::ipc::LaunchPayload; see that struct's own doc
// comment for why (this used to be a libcurl call directly in this
// module, cross-building curl for bionic being the alternative, ruled
// out as substantial standalone work for a single fixed-URL GET that's
// needed at a fixed, early point in boot anyway). This module just
// calls nativeInitClientSettings() with whatever body/status it's
// given.

namespace stud::jni_bridge {

// The ClientSettings group Stud fetches and identifies as.
//
// The real Android app uses "GoogleAndroidApp" (its own
// BuildConfig.CLIENT_SETTINGS_GROUPNAME), and Stud did too, but that
// group is where Roblox puts its MOBILE policy, and Stud is a desktop
// client. The difference is not cosmetic: DFIntFRMConstantFrameTimeTargetMs
// is 34 on Android and 0 on desktop, and the IXP layers behind those
// values are literally named FRM_ConstantFrametimeTargetsAndDrawDistance
// _Mobile versus _NonMobile. The same split is the obvious suspect for
// mobile-grade textures and shadows.
//
// "PCDesktopClient" is the real group the Windows client uses (22,129
// flags, fetched live). STUD_CLIENT_SETTINGS_GROUP overrides it, so
// comparing groups is a relaunch rather than a rebuild.
const char* client_settings_group();


struct ClientSettingsBridgeResult {
    bool http_fetch_succeeded = false;
    long http_status_code = 0;
    bool init_client_settings_called = false;
    bool init_client_settings_trapped_abort = false;
    int init_client_settings_result = -1;
    bool post_init_called = false;
    bool post_init_trapped_abort = false;
};

// `jvm` supplies the JNIEnv*. `lib` must already be successfully
// loaded. `body`/`http_status` come from the UI process's own pre-boot
// fetch (stud::ipc::LaunchPayload). If `http_status` isn't 200, this is
// a no-op (matches the old fetch-failed path: don't call
// nativeInitClientSettings with no real content). Otherwise calls
// nativeInitClientSettings(body, "{}", "GoogleAndroidApp"), if that
// returns 0 (success, matching the app's own app-shell manager's own real
// `f.onPostExecute()` check), also calls
// nativePostClientSettingsLoadedInitialization3(null), matching the
// exact real sequence observed in Sober's own working logs.
ClientSettingsBridgeResult run_client_settings_bridge(FakeJni::Jvm& jvm,
                                                       const stud::linker::LoadedLibrary& lib,
                                                       const std::string& body, long http_status);

}  // namespace stud::jni_bridge
