#pragma once

#include <fake-jni/fake-jni.h>

#include <memory>

#include "stud/device_params.h"
#include "stud/app_java_classes.h"
#include "stud/init_params.h"
#include "stud/linker.h"
#include "stud/platform_params.h"
#include "stud/start_game_params.h"

// NativeGLInterface's "V2" app bridge: what starts the app shell, joins a game,
// follows a resize and shuts down. Stud runs no Java, so it makes the calls the
// app's own app-shell classes would, in their order:
//
//   nativeGameGlobalInit, nativeUpdateAdapterInit   one-time setup
//   nativeAppBridgeV2InitWithParams                 before any Surface exists
//   (the Surface arrives through the GameActivity lifecycle, game_engine_boot.h)
//   nativeAppBridgeStartLuaAppDM                    bare launch
//   nativeAppBridgeV2UpdateSurfaceApp/GameWithPlatformParams
//   nativeAppBridgeV2StartAppWithParams             every launch
//   nativeAppBridgeV2StartGameWithParam             a deep link, or each join
//
// The order carries weight in two places:
//   - Init after the Surface resets the engine's stage to Native and destroys
//     the live render view, which is why run_engine_v2_early_init() exists.
//   - A join needs the app at stage LuaApp, which StartApp is what reaches. From
//     InitializedLuaApp the start-game task never produces a DataModel. So
//     StartApp runs on a deep link too, not only on a bare launch.
//
// A game picked inside the app arrives as a MessageBus request
// (subscribe_to_experience_launch()), and the engine then waits for
// acknowledge_experience_start(). Leaving a game from its own menu stays inside
// the engine. Closing is LeaveGame then DestroyApp.
//
// Any of these calls can block, so each runs on a thread of its own with a
// bounded wait: *_still_running below reports one that had not returned. The
// engine carries on regardless, since its work is on its own threads.

namespace stud::jni_bridge {

struct EngineV2BridgeResult {
    // nativeGameGlobalInit and nativeUpdateAdapterInit.
    bool app_setup_called = false;
    bool app_setup_trapped_abort = false;
    // What StartAppWithParams needs first; see the order above.
    bool lua_app_dm_called = false;
    bool lua_app_dm_trapped_abort = false;
    bool update_surface_app_called = false;
    bool update_surface_app_trapped_abort = false;
    bool update_surface_game_called = false;
    bool update_surface_game_trapped_abort = false;
    bool start_app_with_params_called = false;
    bool start_app_with_params_trapped_abort = false;
    // True if the real, bounded wait for this call elapsed before it
    // completed (see run_engine_v2_sequence()'s own doc comment), an
    // honest report that it's still running/blocked, checked about
    // whether it ever will finish. The background thread it runs on
    // keeps going regardless (detached, same precedent as
    // start_app_with_params_background()).
    bool start_app_with_params_still_running = false;
    bool init_with_params_called = false;
    bool init_with_params_trapped_abort = false;
    // See start_app_with_params_still_running above.
    bool init_with_params_still_running = false;
    bool resume_game_called = false;
    bool resume_game_trapped_abort = false;
    bool resume_game_still_running = false;
    bool start_game_called = false;
    bool start_game_trapped_abort = false;
    bool start_game_still_running = false;
    // Live-traced correction (of the actual
    // Java_..._nativeAppBridgeV2StartGameWithParam entry point and its
    // real return-value global): this is NOT a join
    // success/failure status code. It's a native-window-creation
    // counter, incremented by an unrelated internal helper
    // (ANativeWindow_fromSurface-based dedup logic, "Created
    // ANativeWindow {} (ID:{})") every time a genuinely new window is
    // seen, the value this function happens to return is whatever
    // that counter's current value is at the time, not anything this
    // call itself computes. Every real test this session observed `1`
    // regardless of whether a real join happened, because it was always
    // the first native window created in that process. Do not use this
    // field as a success signal, the real signal is the engine's own
    // "Joining game" FLog line (only visible via a real journalctl/log
    // capture, not this return value).
    int start_game_result = 0;
};

// The steps before the Surface exists: setup and InitWithParams. Pass
// `skip_early_init=true` to run_engine_v2_sequence() afterwards.
EngineV2BridgeResult run_engine_v2_early_init(FakeJni::Jvm& jvm,
                                                const stud::linker::LoadedLibrary& lib,
                                                std::shared_ptr<InitParams> init_params);

// The rest of the order at the top of this file. `jvm` supplies the JNIEnv*,
// and `lib` must already be loaded. One call trapping doesn't stop the next
// from being tried. `surface` must be the Surface the GameActivity lifecycle
// already created (GameActivityLifecycleResult::surface): a new one would be a
// second window.
EngineV2BridgeResult run_engine_v2_sequence(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                             std::shared_ptr<PlatformParams> platform_params,
                                             std::shared_ptr<DeviceParams> device_params,
                                             std::shared_ptr<InitParams> init_params,
                                             std::shared_ptr<SurfaceJava> surface,
                                             const DeepLinkJoinInfo& deep_link = {},
                                             bool skip_early_init = false);

// Join a place handed over by a second stud-ui, because a game link was
// clicked while this session was already playing.
//
// The payload is the parsed link as key=value lines (placeId, gameInfo,
// joinAttemptId, ...), produced by Process A, which owns the URI parser.
// This runs the same nativeAppBridgeV2StartGameWithParam path a deep-link
// launch already uses, the difference is only when it happens.
//
// Returns false when the payload carries no place id. NEVER logs the
// payload: a deep link carries a one-time join ticket.
bool join_experience_from_deep_link(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                    const std::string& payload,
                                    const std::shared_ptr<PlatformParams>& platform_params,
                                    const std::shared_ptr<DeviceParams>& device_params,
                                    const std::shared_ptr<SurfaceJava>& surface);

// Acknowledges a Lua-initiated experience launch by running
// nativeAppBridgeV2StartGameWithParam.
//
// The engine reaches setStage:UGCGame, reports "No DM yet", and waits for the
// platform to acknowledge. Only then does it create its NetworkClient and log
// "! Joining game". Without this, a game picked from Home sits on the loading
// screen for ever.
//
// Called from NativeHelperJava::gameActivity_onExperienceStart's hook on the
// engine's own callback thread, so the work goes to its own bounded thread and
// this returns at once.
void acknowledge_experience_start(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                  const std::shared_ptr<PlatformParams>& platform_params,
                                  const std::shared_ptr<DeviceParams>& device_params,
                                  const std::shared_ptr<SurfaceJava>& surface);

// Re-runs UpdateSurfaceApp for a surface that changed size.
//
// The engine ignores AGDK's onSurfaceChangedNative for its geometry: its
// surface handling lives in this bridge. Without this it keeps its render
// targets at the size it was told at boot, which on screen is the old image in
// one corner and uncleared garbage filling the rest.
bool notify_surface_resized(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                            const std::shared_ptr<PlatformParams>& platform_params,
                            const std::shared_ptr<SurfaceJava>& surface);

// StartApp again after leaving a game, as the real app's own view does when
// its surface comes back; see the definition for the capture.
bool resume_lua_app_after_game(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                               const std::shared_ptr<PlatformParams>& platform_params,
                               const std::shared_ptr<SurfaceJava>& surface);

// StartAppWithParams alone, fire and forget on a detached thread. An older path,
// only used with STUD_ENABLE_V2=0 and STUD_ENABLE_STARTAPP=1; the sequence above
// is what normally calls it. `surface` as above.
void start_app_with_params_background(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                       std::shared_ptr<PlatformParams> platform_params,
                                       std::shared_ptr<SurfaceJava> surface);

struct EngineV2TeardownResult {
    bool leave_game_called = false;
    bool leave_game_trapped_abort = false;
    // Both can hang their calling thread, as a live SIGTERM test showed.
    bool leave_game_still_running = false;
    bool destroy_app_called = false;
    bool destroy_app_trapped_abort = false;
    bool destroy_app_still_running = false;
};

// nativeAppBridgeV2LeaveGame() then nativeAppBridgeV2DestroyApp(), the shutdown
// order on a real close. LeaveGame is a no-op when not in a game, so both are
// always safe to call. Bounded, because either can hang: without the bound a
// close never finishes.
EngineV2TeardownResult run_engine_v2_teardown(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib);

}  // namespace stud::jni_bridge

namespace stud::jni_bridge {

// Subscribes to the experience-launch request the Lua app publishes on the
// engine's own MessageBus, so Stud learns WHICH experience is being
// launched.
//
// This is the real mechanism, traced through the app's own Java rather
// than guessed: the app's own message-bus subscriber subscribes with
// a MessageBus subscription on `JNIExperienceProtocol.getLaunchId()`, and
// the app's own launch-request parser parses the resulting JSON, placeId, userId, gameInstanceId,
// accessCode, linkCode, launchData, referredByPlayerId, joinAttemptId,
// joinAttemptOrigin, into the params that eventually reach
// nativeAppBridgeV2StartGameWithParam. The topic is not a literal
// anywhere: it comes from a native method, which is why searching the
// binary and the Lua bundle for it never found one.
//
// Returns true when the subscription was established.
bool subscribe_to_experience_launch(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib);

}  // namespace stud::jni_bridge
