#pragma once

#include <vector>

#include <fake-jni/fake-jni.h>

#include <string>

#include "stud/linker.h"

// Caught in testing:, structural gap fix (the engineering notes): a real
// Android device calls Application.registerActivityLifecycleCallbacks(),
// and every real Activity transition (onCreate/onStart/onResume, each
// with real "pre"/"post" variants around the actual Java-side handler)
// fires a matching native callback on
// com.roblox.universalapp.activitylifecyclecallbacks.
// JNIActivityLifecycleCallbacks (confirmed against the app's own code, a real,
// `Application.ActivityLifecycleCallbacks`-implementing class, 19 native
// methods total). Stud has no ART/real Activity, so none of these were
// ever called: and real, live evidence (a live backtrace and the engine's own code,
// taken from a genuinely stuck worker thread, StartApp path) shows
// Roblox's own internal engine code blocks *indefinitely* (a real
// absl::Mutex::Await with an infinite timeout) waiting on state only
// these callbacks update, immediately before calling its own
// nativeOnSaveInstanceState implementation. This is a real, structural
// dependency on the full Activity lifecycle being dispatched, not
// something bypassable by skipping straight to the game engine's own
// GameActivity_* entry points (which are a separate, later set of
// native callbacks Stud already drives).
namespace stud::jni_bridge {

struct ActivityLifecycleBridgeResult {
    bool pre_created_called = false;
    bool created_called = false;
    bool post_created_called = false;
    bool pre_started_called = false;
    bool started_called = false;
    bool post_started_called = false;
    bool pre_resumed_called = false;
    bool resumed_called = false;
    bool post_resumed_called = false;
    // True if any of the above calls trapped a fatal signal/abort. Real
    // native code decides its own recovery per-call; this is purely
    // diagnostic for callers.
    bool any_trapped_abort = false;
};

// Dispatches the real onCreate -> onStart -> onResume sequence (pre/post
// variants around each, matching Android's own real
// ActivityLifecycleCallbacks ordering) for a single, real Activity name.
// `jvm` supplies the JNIEnv*; `lib` must already be successfully loaded.
// Missing symbols are skipped individually, not treated as a hard
// failure, same degrade-gracefully convention as every other bridge
// in this directory.
ActivityLifecycleBridgeResult run_activity_lifecycle_bridge(FakeJni::Jvm& jvm,
                                                              const stud::linker::LoadedLibrary& lib,
                                                              const std::string& activity_name);

struct ActivityPauseStopBridgeResult {
    bool pre_paused_called = false;
    bool paused_called = false;
    bool post_paused_called = false;
    bool pre_stopped_called = false;
    bool stopped_called = false;
    bool post_stopped_called = false;
    bool any_trapped_abort = false;
};

// Evidence-based addition (the engineering notes' real logcat
// analysis): a real device's own `InitHelper` log shows
// `unsetView=[ActivitySplash]` firing *before* `setView=[ActivityNativeMain]`
// and `nativeAppBridgeAppStart`, matching Android's own standard,
// documented single-task activity-switch order (old activity onPause,
// then the new activity's own onCreate/onStart/onResume, then the old
// activity's onStop). Dispatches the real onPause -> onStop sequence
// (pre/post variants) for the *outgoing* activity, to be called before
// run_activity_lifecycle_bridge() for the *incoming* one.
ActivityPauseStopBridgeResult run_activity_pause_stop_bridge(FakeJni::Jvm& jvm,
                                                               const stud::linker::LoadedLibrary& lib,
                                                               const std::string& activity_name);

// Separate mechanism found in the app's own code (the engineering notes): completely
// distinct from JNIActivityLifecycleCallbacks above.
// RobloxApplication.onCreate(), the real Application subclass's own
// onCreate, the actual first real Android callback of the whole
// process, even before any Activity's own onCreate, registers a
// JNIAppLifecycleNativeAdapter as an observer on
// ProcessLifecycleOwner (real androidx Jetpack API: process-wide
// foreground/background, not per-Activity). Its real, no-arg native
// methods (confirmed against the library's exported symbols): setActive() on the process's own
// ON_RESUME (first Activity resumes), setInactive() on ON_PAUSE,
// setHidden() on ON_STOP. Stud never called any of these either.
// Dispatches setActive() only, the real boot-relevant one.
bool run_app_lifecycle_native_adapter_set_active(FakeJni::Jvm& jvm,
                                                  const stud::linker::LoadedLibrary& lib);

// Separate finding in the app's own code (the engineering notes):
// RobloxApplication.onCreate() also calls
// `JNIAAssetManagerSetup.a(context)` -> `initNative(context.getAssets())`
// a real native entry point (confirmed against the library's exported symbols) that hands
// libroblox.so a real Java `AssetManager` object, separate from and
// earlier than anything Stud's own `AAssetManager_open()` stub
// (android-glue/src/asset_manager.cpp) intercepts. Stud never called
// this either. `AAssetManager_fromJava()`'s own Stud-side stub already
// ignores its jobject argument entirely (returns a fixed sentinel
// regardless), so any non-null jobject is sufficient here; this call
// exists to let libroblox.so's own internal code register *an* asset
// manager reference at all, not for the specific Java object's
// identity to matter.
bool run_asset_manager_setup_bridge(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib);

// Confirmed (`com.roblox.client.LocalStorageManager`):
// `LocalStorageManager.a(context)` -> `initStorageManagerNativeV3(
// context.getAssets(), context.getFilesDir().getAbsolutePath(),
// context.getCacheDir().getAbsolutePath())`, a real, separate native
// entry point (confirmed against the library's exported symbols:
// `Java_com_roblox_client_LocalStorageManager_initStorageManagerNativeV3`)
// from the AssetManager-only bridge above; this one is real device
// storage-subsystem bring-up (matches this project's own documented
// `RbxStorage::init` discussion in the engineering notes), not asset access.
// Stud never called this either. Real files/cache directory strings
// (already computed for `nativeSetFilesDirectory`/`nativeSetCacheDirectory`)
// are passed through as real JNI strings, not placeholders.
bool run_local_storage_manager_bootstrap(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                          const std::string& files_dir,
                                          const std::string& cache_dir);

// Confirmed: `RobloxApplication.
// onCreate()`'s real, confirmed-taken branch (flag `EnableGameActivity8`
// defaults `false`; see `the engineering notes`, "get Stud rendering the
// real Lua UI" plan) unconditionally calls `JNIBaseUrlProtocol.init(
// context)` and `JNIWebLoginProtocol.init(context)`, two real, separate
// native entry points (confirmed against the library's exported symbols:
// `Java_com_roblox_universalapp_linking_JNIBaseUrlProtocol_init`,
// `Java_com_roblox_universalapp_linking_JNIWebLoginProtocol_init`), both
// real signature `public static native void init(Context)`. Stud never
// called either. Same "real, functional Context object, identity mostly
// doesn't matter but real methods like getFilesDir/getResources must
// actually work if this native code calls them" reasoning as the asset
// manager/storage manager bridges above, unlike those two (which pass
// an identity-agnostic dummy jobject), this uses a real `ActivityStub`
// instance so any real Context method the native implementation might
// call succeeds instead of hitting the same "class is null" bug class
// already fixed twice this session.
bool run_base_url_protocol_init(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib);
bool run_web_login_protocol_init(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib);

// Confirmed gap, found via a full, systematic diff of every
// exported `Java_*` symbol in the real libroblox.so (518 total) against
// every symbol Stud's own source actually resolves, the first time
// this project has done that comparison exhaustively rather than
// piecemeal. `ActivitySplash.onCreate()`
// calls `NativeReportingInterface.initAppShellReporter()`
// unconditionally. No flag gate, no branch, early in the real first
// activity Stud already replicates the lifecycle of. Stud never called
// it. No-arg static native (`Java_com_roblox_engine_jni_
// NativeReportingInterface_initAppShellReporter`, confirmed against the library's exported symbols).
bool run_app_shell_reporter_init(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib);

// Confirmed against the app's own code, same systematic diff: InitHelper's app-start step
// (`startAppBridge`,)
// calls `NativeAppBridgeInterface.setIsFirstInstall(boolean)` right
// before the real `nativeAppBridgeAppStart` call Stud already
// replicates. Real semantics of the boolean come from the real
// InitHelper's first-run check: a real `SharedPreferences("FirstRunPrefs")`
// `"isFreshInstall"` entry, defaulting true and flipped to false on
// first read, i.e. genuinely "is this the very first launch". Stud
// replicates that honestly with a real marker file under its own real
// files directory (see main.cpp), not a hardcoded guess.
// Tells the engine the app is in the foreground and not suspended
// (real ActivityNativeMain.onStart() pair). Its HTTP client will not
// send requests while it believes the app is suspended; see the
// implementation's own comment for the live join symptom that caused.
bool run_app_foreground_bridge(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib);

// Tells the engine the display's real refresh rate, and every rate it
// supports, so it paces frames to the panel that is actually attached.
// Without this the engine cannot retrieve screen info at all and falls
// back to a default, a 144Hz display was being driven at 60.
bool run_pass_display_refresh_rates(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                    float current_hz, const std::vector<float>& supported_hz);

bool run_set_is_first_install(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                               bool is_first_install);

// Confirmed against the app's own code, from the same systematic exported-symbol diff:
// `NativeGLInterface.setTaskSchedulerBackgroundMode(boolean, String)`
// (the real app
// shell manager's own "ASMA.start") puts the engine's own real
// internal task scheduler into FOREGROUND mode (`false`) as part of
// real app-shell startup; the real code only ever passes `true`
// (background) on real pause/stop/end-game paths
// (`ES.endGameInBackground`, `ASMA.pause`, `ASMA.stop`). Stud never
// called it in either direction, so the scheduler has only ever run in
// whatever mode it defaults to at construction. This matters directly:
// every still-blocked V2 call in this project (`InitWithParams`,
// `StartLuaAppDM`, the `UpdateSurface` pair) posts its real work to
// that same internal scheduler and waits for a completion signal that
// never arrives; see the engineering notes' own hardware-watchpoint
// closure on that futex. A scheduler parked in background mode not
// running foreground work is a real, concrete, testable explanation
// for all of them at once. Real second arg is a plain reason/tag
// string used for the engine's own logging.
bool run_set_task_scheduler_foreground(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib);

}  // namespace stud::jni_bridge
