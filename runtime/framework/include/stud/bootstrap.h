#pragma once

#include <fake-jni/fake-jni.h>

#include <memory>
#include <string>

#include "stud/flag_overrides.h"
#include "stud/init_params.h"
#include "stud/linker.h"

// Real orchestration of MainGameActivity's real bootstrap sequence, via
// real JNI-convention exported symbols
// (`Java_com_roblox_client_startup_MainGameActivity_native*`) resolved
// through stud::linker::LoadedLibrary::find_symbol; no real JNI dispatch,
// direct C function calls, matching the "these are plain exported ELF
// symbols" reasoning already established for the whole JNI bridge (see
// the engineering notes' "libroblox.so native symbol survey").
//
// Real, ground-truth call order (the engineering notes, "Sober does not
// patch libroblox.so either" entry, corrects the earlier, guessed
// order this file previously documented), traced from the actual
// the real MainGameActivity.onCreate()/J2()/A2()/D2():
//
//   1. nativeSetAssetPath(String)            , BEFORE GameActivity's
//      own native init (J2(), called before super.onCreate() in
//      onCreate()). See run_preload_bootstrap().
//   2. nativePreloadFlagOverrides(String)    , also from J2(), also
//      before super.onCreate(), and on a real device only called
//      conditionally (a real gate this project has no equivalent for.
//      Stud always calls it, matching the locked "FFlags are always
//      user-JSON-driven" product decision). See run_preload_bootstrap().
//   3. nativeAppBridgeSetInitParams(InitParams), NOT called eagerly by
//      Java at a fixed point at all. Reached via D2() <- A2() <- d2() <-
//      bootstrapTheApp(), a real @Keep native-to-Java callback (confirmed:
//      "bootstrapTheApp"/"[FLog::NativeDM] bootstrapTheApp_:" strings in
//      libroblox.so) the engine invokes when IT is ready, not something
//      Java calls proactively. See run_init_params_bootstrap() and
//      game_activity_stubs.h's MainGameActivityStub::bootstrapTheApp().
//
// run_bootstrap() below is kept as a combined convenience wrapper (used by
// tests/tools that don't need the split) with the SAME corrected order;
// production code (runtime/main.cpp) uses the two split functions directly
// so nativeAppBridgeSetInitParams genuinely only fires from inside the
// real callback, not eagerly.
//
// tests/bootstrap_test.cpp tests the orchestration logic (argument
// shapes, call order, JNIEnv* validity) against a synthetic fixture;
// runtime/main.cpp is what actually calls this against the real,
// patched libroblox.so.
//
// See the engineering notes, milestones M4 and the "Sober does not patch
// libroblox.so either" / boot-sequence-rework entries.

namespace stud::jni_bridge {

struct BootstrapResult {
    bool preload_flag_overrides_called = false;
    bool set_asset_path_called = false;
    bool app_bridge_set_init_params_called = false;

    // True if the corresponding call above hit a real, in-flight int3/
    // abort() inside libroblox.so's own bootstrap logic, trapped via
    // stud::jni_bridge::arm_abort_trap() (trap_recovery.h) rather than
    // letting it terminate the process. *_called stays true either way
    // (the call was made); this flags that it didn't run to completion.
    bool preload_flag_overrides_trapped_abort = false;
    bool set_asset_path_trapped_abort = false;
    bool app_bridge_set_init_params_trapped_abort = false;
};

// `jvm` supplies the JNIEnv* passed to each native call (FakeJni::Env
// publicly inherits from the real JNIEnv, so no conversion is needed).
// `lib` must already be successfully loaded. Missing exported symbols are
// treated as fatal (throws stud::linker::LoadError naming the symbol),
// if MainGameActivity's real bootstrap entry points aren't present, that's
// a real problem worth failing loudly on, not silently skipping.
BootstrapResult run_bootstrap(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                               const std::string& asset_path, const FlagOverrides& flag_overrides,
                               std::shared_ptr<InitParams> init_params);

struct PreloadBootstrapResult {
    bool set_asset_path_called = false;
    bool set_asset_path_trapped_abort = false;
    bool preload_flag_overrides_called = false;
    bool preload_flag_overrides_trapped_abort = false;
};

// nativeSetAssetPath then nativePreloadFlagOverrides; real order, see
// this header's own doc comment above. Call BEFORE
// GameActivity_initializeNativeCode() (real Android calls J2() before
// super.onCreate()).
PreloadBootstrapResult run_preload_bootstrap(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                              const std::string& asset_path,
                                              const FlagOverrides& flag_overrides);

struct InitParamsBootstrapResult {
    bool app_bridge_set_init_params_called = false;
    bool app_bridge_set_init_params_trapped_abort = false;
};

// nativeAppBridgeSetInitParams only. Call from inside
// MainGameActivityStub::bootstrapTheApp()'s real callback (see
// game_activity_stubs.h), not eagerly, matches D2()'s real trigger.
InitParamsBootstrapResult run_init_params_bootstrap(FakeJni::Jvm& jvm,
                                                     const stud::linker::LoadedLibrary& lib,
                                                     std::shared_ptr<InitParams> init_params);

// Real, confirmed against the app's own code, previously-missing entry point (found while
// investigating why Roblox's Lua-driven app UI never renders anything,
// the engineering notes' "instantiate controllers" investigation): the real
// D2() (setInitParamsForEngine) calls `NativeSettingsInterface.
// nativeSetDeviceInfo(DeviceParams)`, confirmed against the library's exported symbols as a real,
// exported symbol (`Java_com_roblox_engine_jni_NativeSettingsInterface_
// nativeSetDeviceInfo`), as its very first action, BEFORE building or
// setting InitParams at all. Stud has never called this at any point in
// this project's history (confirmed via a grep across the whole
// codebase before adding this). Real, plausible significance: this is
// the engine's one dedicated entry point for its own internal
// "device info" global state, and D2()'s own real ordering (this call
// strictly precedes InitParams) suggests later initialization code may
// read that state, a real, concrete candidate for what's missing
// upstream of the never-triggered "instantiate controllers" vtable
// chain, not confirmed yet, but real and previously untested.
struct SetDeviceInfoResult {
    bool called = false;
    bool trapped_abort = false;
};
SetDeviceInfoResult run_native_set_device_info(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                                std::shared_ptr<DeviceParams> device_params);

}  // namespace stud::jni_bridge
