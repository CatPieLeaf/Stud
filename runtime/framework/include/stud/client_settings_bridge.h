#pragma once

#include <fake-jni/fake-jni.h>

#include <string>

#include "stud/linker.h"

// On a device the Java side fetches ClientSettings over HTTP and hands the
// content to nativeInitClientSettings(); the engine never fetches it itself.
// Stud runs no Java, so Process A fetches it before the runtime starts and hands
// it over through stud::ipc::LaunchPayload. This module only calls the engine
// with the body and status it's given.

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
// a no-op. Otherwise calls nativeInitClientSettings(body, "{}",
// client_settings_group()), and if that returns 0, as the app itself checks,
// nativePostClientSettingsLoadedInitialization3 with an empty list.
ClientSettingsBridgeResult run_client_settings_bridge(FakeJni::Jvm& jvm,
                                                       const stud::linker::LoadedLibrary& lib,
                                                       const std::string& body, long http_status);

}  // namespace stud::jni_bridge
