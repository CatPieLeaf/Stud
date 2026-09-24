#pragma once

#include "stud/bionic_jvm.h"
#include "stud/app_java_classes.h"
#include "stud/linker.h"

#include <functional>
#include <string>

// General infrastructure for driving a loaded libroblox.so through
// its actual post-load boot sequence, promoted out of
// tools/try_bootstrap.cpp once proven against the real binary, shared by
// the diagnostic tool and the real stud-runtime binary so both stay in
// sync. See the engineering notes, "run_bootstrap() completes end-to-end" and
// "GameActivity's real lifecycle driven end-to-end" entries, for the full
// history and the real bugs (bionic %fs-context handling, two jnivm
// GetMethodID/RegisterNatives bugs) this depends on.
//
// Deliberately NOT included here: any Roblox-build-specific byte offset
// or address. Every function in this header works the same way regardless
// of which Roblox APK version produced the loaded library, matching the
// project's locked "works with any given Roblox APK" requirement. (One
// real, known limitation of the *diagnostic tool*, not promoted here: an
// exploratory direct call to one specific build's internal flag-registry
// constructor at a hardcoded address, kept local to
// tools/try_bootstrap.cpp and never wired into this shared module.)
namespace stud::jni_bridge {

struct JniOnLoadResult {
    bool found = false;
    bool trapped_abort = false;
    jint result = 0;
};

// Returns a callable suitable as stud::linker::load_library()'s
// after_constructors argument: calls JNI_OnLoad(vm, nullptr) if the
// library exports one, with the same abort-trap protection as every
// other native call in this module (see trap_recovery.h's
// arm_abort_trap()), a trapped abort leaves *out_result.trapped_abort
// true and the process running, not a crash. Real bionic's own linker64
// (not Stud's own former in-process loader) handles module registration
// and unwind-info for real C++ exception unwinding across this load
// natively, exactly like a real device; no separate registration step
// needed here anymore. `out_result` is only meaningful after
// load_library() itself has returned.
std::function<void(const stud::linker::LoadedLibrary&)> make_post_constructor_hook(
    BionicAwareJvm& jvm, const std::string& so_path, JniOnLoadResult& out_result);

struct GameActivityLifecycleResult {
    bool initialize_native_code_found = false;
    bool initialize_native_code_trapped_abort = false;
    jlong game_activity_ptr = 0;

    // The real Surface object passed to onSurfaceCreatedNative/
    // onSurfaceChangedNative. Real bug found and fixed
    // (the engineering notes, "two windows" entry): any other caller that
    // needs to hand Roblox a Surface (e.g. the V2 app-bridge's
    // ResumeGameWithPlatformParams/StartGameWithParam) must reuse THIS
    // SAME object, not construct a fresh SurfaceJava; android-glue's
    // ANativeWindow dedup cache (added for the exact same reason) is
    // keyed by jobject identity, so a different object means a second,
    // real, independently-mapped Wayland window.
    std::shared_ptr<SurfaceJava> surface;

    // The real GameActivity instance every lifecycle call above was made
    // on. The input bridge needs it to deliver real MotionEvent/KeyEvent
    // objects through AGDK's own onTouchEventNative/onKeyDownNative, the
    // only input path that carries a real InputDevice source and tool type.
    std::shared_ptr<MainGameActivityJava> activity;

    bool on_start_called = false;
    bool on_start_trapped_abort = false;
    bool on_resume_called = false;
    bool on_resume_trapped_abort = false;
    bool on_window_focus_changed_called = false;
    bool on_window_focus_changed_trapped_abort = false;
    bool on_surface_created_called = false;
    bool on_surface_created_trapped_abort = false;
    bool on_surface_changed_called = false;
    bool on_surface_changed_trapped_abort = false;
};

// Re-delivers onSurfaceChangedNative for a window that really changed size.
//
// Real Android sends this on every surface geometry change, not just once at
// startup; without it the engine keeps rendering at the size it was told at
// boot while the compositor has already resized the window underneath it,
// live-observed as the window growing on a drag with the old, smaller image
// still in it and the rest of the frame showing straight through to the
// desktop.
//
// Runs on its own detached thread with a bounded wait, for the same reason
// every other lifecycle call here does (a real one can block), and refuses to
// overlap itself, a drag delivers a configure per pixel.
bool dispatch_surface_changed(FakeJni::Jvm& jvm, const GameActivityLifecycleResult& lifecycle,
                              int32_t width, int32_t height);

// Drives AGDK's real GameActivity native lifecycle end-to-end against a
// loaded libroblox.so, playing the "Java framework side" role a real
// Android device's own GameActivity.java would: GameActivity_
// initializeNativeCode() -> onStartNative -> onResumeNative ->
// onWindowFocusChangedNative(true) -> onSurfaceCreatedNative ->
// onSurfaceChangedNative(surface_width, surface_height, RGBA_8888).
//
// Requires: `jvm` must already have register_app_java_classes()
// (app_java_classes.h) called on it and jvm.attachLibrary("") already
// called (both one-time FakeJni::Jvm setup, unrelated to any specific
// call here), and ALooper_prepare(0) already called on the calling thread
// (android-glue's ALooper; real device precondition GameActivity's own
// init code depends on; without it, real init code crashes
// dereferencing a null ALooper). internal_data_dir/
// obb_dir/external_data_dir are plain scratch directory path strings
// (unrelated to AAssetManager's own asset base, set separately via
// android_glue::set_asset_base_directory()).
//
// `on_bootstrap_the_app`, if given, is set on the MainGameActivityJava
// instance (app_java_classes.h) BEFORE calling
// GameActivity_initializeNativeCode(); real Android's bootstrapTheApp()
// is a native-to-Java callback the engine invokes when it's ready for
// setInitParamsForEngine (see bootstrap.h's run_init_params_bootstrap()
// doc comment), not something called at a fixed point from here. Default
// empty for callers that don't need it (e.g. diagnostic probes that drive
// nativeAppBridgeSetInitParams some other way).
GameActivityLifecycleResult drive_game_activity_lifecycle(
    BionicAwareJvm& jvm, const stud::linker::LoadedLibrary& lib,
    const std::string& internal_data_dir, const std::string& obb_dir,
    const std::string& external_data_dir, int surface_width, int surface_height,
    std::function<void()> on_bootstrap_the_app = {});

}  // namespace stud::jni_bridge
