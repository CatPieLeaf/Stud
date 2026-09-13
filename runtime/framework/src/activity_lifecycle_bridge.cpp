#include "stud/activity_lifecycle_bridge.h"

#include <string>
#include <utility>

#include "stud/game_activity_stubs.h"
#include "stud/trap_recovery.h"

namespace stud::jni_bridge {

namespace {

using StringVoidFn = void (*)(JNIEnv*, jclass, jstring);

// Calls one real
// Java_com_roblox_universalapp_activitylifecyclecallbacks_
// JNIActivityLifecycleCallbacks_native<Name> entry point with the given
// activity name string, if it exists in this build of libroblox.so.
// Returns {found, trapped_abort}.
std::pair<bool, bool> call_one(const stud::linker::LoadedLibrary& lib, JNIEnv* jni_env,
                                jstring name_ref, const char* symbol_suffix) {
    std::string symbol =
        "Java_com_roblox_universalapp_activitylifecyclecallbacks_"
        "JNIActivityLifecycleCallbacks_native" +
        std::string(symbol_suffix);
    void* addr = lib.find_symbol(symbol.c_str());
    if (addr == nullptr) {
        return {false, false};
    }
    auto* fn = reinterpret_cast<StringVoidFn>(addr);
    bool ok = call_trapping_abort(fn, jni_env, nullptr, name_ref);
    clear_pending_jni_exception(jni_env, symbol.c_str());
    return {true, !ok};
}

}  // namespace

ActivityLifecycleBridgeResult run_activity_lifecycle_bridge(FakeJni::Jvm& jvm,
                                                              const stud::linker::LoadedLibrary& lib,
                                                              const std::string& activity_name) {
    ActivityLifecycleBridgeResult result;

    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);
    jstring name_ref = env.NewStringUTF(activity_name.c_str());

    // Real Android order: each real transition's "pre" native callback
    // fires immediately before the app's own onActivityXxx() Java code
    // would run, the real (unprefixed) one alongside it, and "post"
    // immediately after -- see JNIActivityLifecycleCallbacks (the app's own code).
    // Only the boot-relevant onCreate/onStart/onResume trio is dispatched
    // here; onPause/onStop/onDestroy/onSaveInstanceState are real
    // teardown/backgrounding events this bring-up sequence doesn't reach.
    struct Step {
        const char* suffix;
        bool* called_field;
    };
    Step steps[] = {
        {"OnPreCreated", &result.pre_created_called},
        {"OnCreated", &result.created_called},
        {"OnPostCreated", &result.post_created_called},
        {"OnPreStarted", &result.pre_started_called},
        {"OnStarted", &result.started_called},
        {"OnPostStarted", &result.post_started_called},
        {"OnPreResumed", &result.pre_resumed_called},
        {"OnResumed", &result.resumed_called},
        {"OnPostResumed", &result.post_resumed_called},
    };
    for (const auto& step : steps) {
        auto [found, trapped] = call_one(lib, jni_env, name_ref, step.suffix);
        *step.called_field = found;
        result.any_trapped_abort = result.any_trapped_abort || trapped;
    }

    return result;
}

ActivityPauseStopBridgeResult run_activity_pause_stop_bridge(FakeJni::Jvm& jvm,
                                                               const stud::linker::LoadedLibrary& lib,
                                                               const std::string& activity_name) {
    ActivityPauseStopBridgeResult result;

    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);
    jstring name_ref = env.NewStringUTF(activity_name.c_str());

    struct Step {
        const char* suffix;
        bool* called_field;
    };
    Step steps[] = {
        {"OnPrePaused", &result.pre_paused_called},
        {"OnPaused", &result.paused_called},
        {"OnPostPaused", &result.post_paused_called},
        {"OnPreStopped", &result.pre_stopped_called},
        {"OnStopped", &result.stopped_called},
        {"OnPostStopped", &result.post_stopped_called},
    };
    for (const auto& step : steps) {
        auto [found, trapped] = call_one(lib, jni_env, name_ref, step.suffix);
        *step.called_field = found;
        result.any_trapped_abort = result.any_trapped_abort || trapped;
    }

    return result;
}

bool run_app_lifecycle_native_adapter_set_active(FakeJni::Jvm& jvm,
                                                  const stud::linker::LoadedLibrary& lib) {
    void* addr = lib.find_symbol(
        "Java_com_roblox_universalapp_applifecyclenativeadapter_JNIAppLifecycleNativeAdapter_"
        "setActive");
    if (addr == nullptr) {
        return false;
    }
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);
    using VoidFn = void (*)(JNIEnv*, jclass);
    auto* fn = reinterpret_cast<VoidFn>(addr);
    bool ok = call_trapping_abort(fn, jni_env, nullptr);
    clear_pending_jni_exception(jni_env, "JNIAppLifecycleNativeAdapter.setActive");
    return ok;
}

bool run_asset_manager_setup_bridge(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib) {
    void* addr = lib.find_symbol("Java_com_roblox_client_JNIAAssetManagerSetup_initNative");
    if (addr == nullptr) {
        return false;
    }
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);
    // Any real, non-null jobject works here -- see this function's own
    // doc comment for why the object's actual identity doesn't matter.
    jobject dummy_asset_manager = env.NewStringUTF("stud-dummy-asset-manager");
    using AssetManagerFn = void (*)(JNIEnv*, jclass, jobject);
    auto* fn = reinterpret_cast<AssetManagerFn>(addr);
    bool ok = call_trapping_abort(fn, jni_env, nullptr, dummy_asset_manager);
    clear_pending_jni_exception(jni_env, "JNIAAssetManagerSetup.initNative");
    return ok;
}

bool run_local_storage_manager_bootstrap(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                          const std::string& files_dir,
                                          const std::string& cache_dir) {
    void* addr =
        lib.find_symbol("Java_com_roblox_client_LocalStorageManager_initStorageManagerNativeV3");
    if (addr == nullptr) {
        return false;
    }
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);
    // Real `AssetManagerStub` instance rather than a placeholder string:
    // the real call is `LocalStorageManager.a(context)` ->
    // `initStorageManagerNativeV3(context.getAssets(), filesDir,
    // cacheDir)`, so the first argument really is an
    // `android.content.res.AssetManager`.
    //
    // `this` is a real LocalStorageManagerStub, not null. This is an
    // INSTANCE native method on a Kotlin `object` ( `private final
    // native void initStorageManagerNativeV3(...)`), and the same class
    // carries a real `@Keep public final long getAllocatableBytes()`
    // that exists for exactly one reason -- for the native side to call
    // back on this object to ask how much disk it may use. Passing null
    // left it nothing to call back on.
    //
    // Honest about what this did NOT fix: live-tested, the engine still
    // logs `[DFLog::RbxmFileManager] LocalStorageManager is not
    // available`, and it never calls getAllocatableBytes at all -- so
    // whatever decides "available" is not this object. Kept because it
    // matches the real declaration; do not re-test it as a fix for that
    // warning.
    jobject storage_manager =
        env.createLocalReference(std::make_shared<LocalStorageManagerStub>());
    jobject dummy_asset_manager = env.createLocalReference(std::make_shared<AssetManagerStub>());
    jobject files_dir_jstr = env.NewStringUTF(files_dir.c_str());
    jobject cache_dir_jstr = env.NewStringUTF(cache_dir.c_str());
    using StorageInitFn = void (*)(JNIEnv*, jobject, jobject, jobject, jobject);
    auto* fn = reinterpret_cast<StorageInitFn>(addr);
    bool ok = call_trapping_abort(fn, jni_env, storage_manager, dummy_asset_manager, files_dir_jstr,
                                   cache_dir_jstr);
    clear_pending_jni_exception(jni_env, "LocalStorageManager.initStorageManagerNativeV3");
    return ok;
}

namespace {
bool run_context_init_bridge(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                              const char* symbol, const char* log_context) {
    void* addr = lib.find_symbol(symbol);
    if (addr == nullptr) {
        return false;
    }
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);
    // Real ActivityStub instance (not an identity-agnostic dummy) -- its
    // real Context methods (getFilesDir/getCacheDir/getResources/
    // getPackageName/getSharedPreferences) are all already implemented,
    // so this works whether or not the real native init() call reaches
    // for any of them internally.
    jobject context = env.createLocalReference(std::make_shared<ActivityStub>());
    using ContextInitFn = void (*)(JNIEnv*, jclass, jobject);
    auto* fn = reinterpret_cast<ContextInitFn>(addr);
    bool ok = call_trapping_abort(fn, jni_env, nullptr, context);
    clear_pending_jni_exception(jni_env, log_context);
    return ok;
}
}  // namespace

bool run_base_url_protocol_init(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib) {
    return run_context_init_bridge(
        jvm, lib, "Java_com_roblox_universalapp_linking_JNIBaseUrlProtocol_init",
        "JNIBaseUrlProtocol.init");
}

bool run_web_login_protocol_init(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib) {
    return run_context_init_bridge(
        jvm, lib, "Java_com_roblox_universalapp_linking_JNIWebLoginProtocol_init",
        "JNIWebLoginProtocol.init");
}

bool run_app_shell_reporter_init(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib) {
    void* addr =
        lib.find_symbol("Java_com_roblox_engine_jni_NativeReportingInterface_initAppShellReporter");
    if (addr == nullptr) {
        return false;
    }
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);
    using VoidFn = void (*)(JNIEnv*, jclass);
    auto* fn = reinterpret_cast<VoidFn>(addr);
    bool ok = call_trapping_abort(fn, jni_env, nullptr);
    clear_pending_jni_exception(jni_env, "NativeReportingInterface.initAppShellReporter");
    return ok;
}

bool run_set_task_scheduler_foreground(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib) {
    void* addr = lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeGLInterface_setTaskSchedulerBackgroundMode");
    if (addr == nullptr) {
        return false;
    }
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);
    // Real reason-string argument, matching the real call sites' own
    // convention ("ASMA.start", "ES.onSurfaceCreated") -- honest about
    // being Stud's own bring-up, not impersonating a real Java caller.
    jstring reason = env.NewStringUTF("stud.start");
    using SchedulerModeFn = void (*)(JNIEnv*, jclass, jboolean, jstring);
    auto* fn = reinterpret_cast<SchedulerModeFn>(addr);
    bool ok = call_trapping_abort(fn, jni_env, nullptr, static_cast<jboolean>(JNI_FALSE), reason);
    clear_pending_jni_exception(jni_env, "NativeGLInterface.setTaskSchedulerBackgroundMode");
    return ok;
}

// Real ActivityNativeMain.onStart() pair (the app's own code, ActivityNativeMain):
// applicationForegrounded() and setAppSuspended(false).
//
// This is not bookkeeping. The engine's own HTTP client refuses to send
// while it believes the app is suspended -- libroblox carries the
// matching messages verbatim:
//   [DFLog::HttpTraceError] Network not available while the app is suspended!
//   [DFLog::HttpTraceError] App successfully foregrounded! Resuming HTTP requests.
// Stud never made either call, so the engine was never told the app is
// in the foreground at all. Live symptom: a game launch reached
// submitStartGameTask and then sat forever with the in-game network
// diagnostics reading "Placelauncher (Load Time, Total Time, Retries):
// Waiting..." and "JoinStart (loading stage) Waiting..." -- the join
// request queued, never sent, so no NetworkClient and no "Joining game".
bool run_app_foreground_bridge(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib) {
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);
    bool any = false;

    if (void* addr = lib.find_symbol(
            "Java_com_roblox_engine_jni_NativeReportingInterface_setAppSuspended");
        addr != nullptr) {
        using BoolFn = void (*)(JNIEnv*, jclass, jboolean);
        auto* fn = reinterpret_cast<BoolFn>(addr);
        any = call_trapping_abort(fn, jni_env, nullptr, static_cast<jboolean>(JNI_FALSE)) || any;
        clear_pending_jni_exception(jni_env, "NativeReportingInterface.setAppSuspended");
    }

    if (void* addr = lib.find_symbol(
            "Java_com_roblox_engine_jni_NativeReportingInterface_applicationForegrounded");
        addr != nullptr) {
        using VoidFn = void (*)(JNIEnv*, jclass);
        auto* fn = reinterpret_cast<VoidFn>(addr);
        any = call_trapping_abort(fn, jni_env, nullptr) || any;
        clear_pending_jni_exception(jni_env, "NativeReportingInterface.applicationForegrounded");
    }
    return any;
}

bool run_set_is_first_install(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                               bool is_first_install) {
    void* addr =
        lib.find_symbol("Java_com_roblox_engine_jni_NativeAppBridgeInterface_setIsFirstInstall");
    if (addr == nullptr) {
        return false;
    }
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);
    using BoolFn = void (*)(JNIEnv*, jclass, jboolean);
    auto* fn = reinterpret_cast<BoolFn>(addr);
    bool ok = call_trapping_abort(fn, jni_env, nullptr,
                                   static_cast<jboolean>(is_first_install ? JNI_TRUE : JNI_FALSE));
    clear_pending_jni_exception(jni_env, "NativeAppBridgeInterface.setIsFirstInstall");
    return ok;
}

// The engine paces frames to what it believes the display can do. It
// asks Android for that (the app's own app-shell helper calls
// nativePassSupportedRefreshRates(supported rates) then
// nativePassCurrentDisplayRefreshRate(current rate)), and with
// nothing answering it logs
// `getPrimaryDisplayRefreshRate FAILED: Could not retrieve screen info`
// and settles for a default -- which is why a 144Hz panel was being
// driven at 60. Both rates come from the compositor at runtime, so this
// reports whatever display is actually attached rather than a number
// baked in here.
bool run_pass_display_refresh_rates(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                    float current_hz, const std::vector<float>& supported_hz) {
    void* current_addr = lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeGLInterface_nativePassCurrentDisplayRefreshRate");
    void* supported_addr = lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeGLInterface_nativePassSupportedRefreshRates");
    if (current_addr == nullptr || current_hz <= 0.0f) return false;

    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    // Supported set first, matching the real call order.
    if (supported_addr != nullptr && !supported_hz.empty()) {
        auto rates = std::make_shared<FakeJni::JFloatArray>(
            static_cast<FakeJni::JInt>(supported_hz.size()));
        for (std::size_t i = 0; i < supported_hz.size(); ++i) {
            (*rates)[static_cast<FakeJni::JInt>(i)] = supported_hz[i];
        }
        using ArrayFn = void (*)(JNIEnv*, jclass, jobject);
        auto* supported_fn = reinterpret_cast<ArrayFn>(supported_addr);
        call_trapping_abort(supported_fn, jni_env, nullptr, env.createLocalReference(rates));
        clear_pending_jni_exception(jni_env, "nativePassSupportedRefreshRates");
    }

    using FloatFn = void (*)(JNIEnv*, jclass, jfloat);
    auto* current_fn = reinterpret_cast<FloatFn>(current_addr);
    const bool ok =
        call_trapping_abort(current_fn, jni_env, nullptr, static_cast<jfloat>(current_hz));
    clear_pending_jni_exception(jni_env, "nativePassCurrentDisplayRefreshRate");
    return ok;
}

}  // namespace stud::jni_bridge
