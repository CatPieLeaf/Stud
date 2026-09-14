#pragma once

#include <fake-jni/fake-jni.h>

#include <memory>

#include "stud/device_params.h"
#include "stud/game_activity_stubs.h"
#include "stud/init_params.h"
#include "stud/linker.h"
#include "stud/platform_params.h"
#include "stud/start_game_params.h"

// UPDATE, real and conclusive (supersedes the StartApp-ordering
// investigation below, doesn't invalidate it, Init-before-StartApp
// is still the right order for whenever StartApp is used): a real
// Sober session's own journalctl log, a real logged-in user launching
// a real UGC game via a real deep link, captured completely from
// process start through the game actually loading, shows
// nativeAppBridgeV2StartAppWithParams is NEVER called at all for a
// direct game launch. The real, verified sequence is just
// nativeAppBridgeAppStart -> nativeAppBridgeV2Init -> (real internal
// SingleSurfaceApp setup, logged as "initializeSingleton"/"instantiate
// controllers"/"instantiate experience coordinator") ->
// nativeAppBridgeV2StartGameWithParam. This is also the real
// explanation for this session's own extensively investigated
// StartAppWithParams crash (a null UserController singleton, static analysis-
// traced to SingleSurfaceApp::userDidLogin/UserController::didLogin):
// that whole call chain simply isn't exercised by a real, working
// client for this launch mode. run_engine_v2_sequence() now defaults
// StartApp OFF (STUD_ENABLE_V2_STARTAPP=1 to opt back in, for future
// work on the "no specific game" home-screen scenario this real
// capture doesn't cover).
//
// UPDATE 2, real, four-way-tested (a real Sober install, four separate
// real captures via journalctl: (1) URI/deep-link Play click, (2) KDE
// menu launch with cached login picking a game manually, (3) a
// completely fresh install, data/cache wiped, through real login
// and game pick, (4) exiting a game via its own in-game menu instead
// of the window titlebar). This is the real, now-covered ground
// truth for the "no specific game" home-screen scenario UPDATE 1 above
// left open:
//
//   - There is NO separate bare "nativeAppBridgeV2StartApp" JNI export.
//     the library's exported symbols against the real libroblox.so shows only
//     nativeAppBridgeV2StartAppWithParams; the shorter
//     "nativeAppBridgeV2StartApp:" text seen in real FLog output is
//     just that same function's own internal, shortened log tag. Any
//     earlier read of this as a second, distinct symbol was wrong.
//   - The real reason StartAppWithParams crashed (null UserController)
//     is that three real, load-bearing prerequisite calls were never
//     made before it: nativeAppBridgeStartLuaAppDM() (no args; real,
//     confirmed against the library's exported symbols and NativeGLInterface itself,
//     triggers `initializeLuaAppWithLoggedInUser`/setStage(InitializedLuaApp)
//     internally), then nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams
//     (Surface, PlatformParams) and nativeAppBridgeV2UpdateSurfaceGameWithPlatformParams
//     (Surface, PlatformParams, Activity), both real, exported,
//     confirmed by the library's exported symbols, and both observed firing (twice each, back to
//     back) immediately before StartAppWithParams in every real
//     non-URI-launch capture. Real, verified home-screen order:
//       nativeAppBridgeAppStart -> nativeAppBridgeV2Init ->
//       (instantiate controllers/coordinator) ->
//       nativeAppBridgeStartLuaAppDM ->
//       nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams ->
//       nativeAppBridgeV2UpdateSurfaceGameWithPlatformParams ->
//       nativeAppBridgeV2StartAppWithParams -> (setStage(LuaApp), home
//       screen) -> [user picks a game] -> nativeAppBridgeV2StartGameWithParam
//   - Real login (test 3, fresh install, no cached cookie) happens
//     entirely on the Lua side, as ordinary HTTP calls logged under
//     `LuaAppStarterScript` (401s/403s while unauthenticated, then a
//     real `auth.roblox.com/v2/login` challenge, then quiet success),
//     there is no separate native/JNI login path to implement. The
//     `userDidLogin` FLog line fires right after StartAppWithParams
//     returns, well before real auth completes; it's a LuaApp stage
//     transition, not proof of a real session.
//   - Real in-game "exit to menu" (test 4, clicked from inside the
//     game, not the window titlebar) never crosses the JNI bridge at
//     all. It's a pure internal SingleSurfaceApp transition
//     (returnToLuaApp -> leaveUGCGameInternal -> returnToLuaAppInternal
//     -> setStage(LuaApp)), entirely inside libroblox.so. Process B
//     needs to do nothing special to support it beyond staying alive.
//   - Real graceful shutdown (all four captures, titlebar close) is
//     nativeAppBridgeV2LeaveGame() (a real, harmless no-op if already
//     out of a game, confirmed by test 4's own
//     "leaveUGCGame: ... no-op, not in-game" log line) followed by
//     nativeAppBridgeV2DestroyApp() -> destroyLuaApp -> setStage(None).
//     Both real, no-arg, exported symbols Stud never called before now
//     (see run_engine_v2_teardown() below); Process B previously just
//     exited raw on window close.
//
// UPDATE 3, real, live-tested (STUD_ENABLE_V2_STARTAPP=1, bare launch,
// this build with UPDATE 2's prerequisite chain wired in): the fix
// moves the crash, doesn't clear it. StartLuaAppDM's queued work
// reaches the real SingleSurfaceApp::initializeLuaAppWithLoggedInUser
// (confirmed live via its own FLog format string,
// "initializeLuaAppWithLoggedInUser: (stage:{})") but then SIGSEGVs on
// `MOV RAX,[RDI]` where RDI is `*(context+0x400)`, null, a vtable
// call on a second, different lazily-populated subsystem object,
// adjacent to but distinct from the `+0x430` UserController field this
// project already spent a full session on (breakpoints that never
// fired, zero live xrefs to the relevant FLog strings) before dropping
// it once the direct-game-launch path turned out not to need it at
// all. Same shape, likely same class of dead end. STUD_ENABLE_V2_STARTAPP
// stays real, opt-in, and honestly experimental/broken for the
// home-screen (no specific game) scenario, not pursued further right
// now. The direct-game-launch path (default config, StartApp OFF) is
// unaffected and remains the one real, working path.
//
// UPDATE 4, real, live-tested and negative: the crashing branch inside
// initializeLuaAppWithLoggedInUser (UPDATE 3 above) is gated behind
// a compiled-in gate byte, traced in the engine (real, live) to be the
// cached value of a real Roblox FVariable, registered in a real
// .init_array constructor (a static initialiser) alongside the literal string
// "DoNotInitializeLuaAppIfAlreadyInitialized". A real, clean, no-code-
// change fix was tried: forcing that flag false via Stud's own already-
// built --flag-overrides mechanism (~/.config/stud/flags.json), tried
// under both its bare name and the real "FFlag"-prefixed convention.
// Confirmed live that the override IS loaded and delivered
// (`nativePreloadFlagOverrides` called, no trap), but the crash is
// byte-for-byte identical both times. No engine-side log line confirms
// or denies whether Roblox's own internal flag system actually matched
// either name; this project has no way to observe that from outside.
// Same wall as UPDATE 3 and the original UserController investigation:
// clean, reasoned attempts, zero observable effect. Not pursued
// further for now; see this project's own memory/session notes for
// the standing decision to stop here.
//
// Real orchestration of NativeGLInterface's "V2" app-bridge API,
// distinct from MainGameActivity's own bootstrap()/nativeAppBridgeSetInitParams
// (already driven by run_bootstrap()) and from NativeAppBridgeInterface's
// nativeAppBridgeAppStart() (already driven by run_app_bridge_start()).
//
// Ground-truth-traced against the library's exported symbols (real, single, non-overloaded exported
// symbols) + the app's own code of `NativeGLInterface.java`/
// `StartAppParams.java`/`StartGameParams.java`
// (`com.roblox.engine.jni.NativeGLInterface`):
//
//   nativeAppBridgeV2StartAppWithParams(StartAppParams)
//   nativeAppBridgeV2InitWithParams(InitParams)
//   nativeAppBridgeV2ResumeGameWithPlatformParams(Surface, PlatformParams, Activity)
//   nativeAppBridgeV2StartGameWithParam(StartGameParams) -> int
//
// Real ordering was Stud's own best-effort guess for a long time ("app
// start, then init, then resume, then start game", the natural reading
// of the method names), confirmed WRONG this session via a full,
// the app's own code of the app itself (user correction:
// Stud was bruteforcing raw JNI calls instead of housing Roblox as a
// real Android app, i.e. going through its own real Activity/Fragment
// lifecycle. This read of the app's own code is the direct result of taking that
// correction seriously). Ground truth, from the app's own code:
//
// Both StartAppWithParams and InitWithParams's one real Java caller is
// the app-shell singleton (an internal "AppShell" singleton, not Stud-named, obfuscated
// in the real APK), called from a real Android Fragment,
// its own fragment ("AppShellFragment"), that Stud does not itself implement:
//   - AppShellFragment.onCreate() (`D0()`) calls
//     `the app shell's setup step` (one-time setup: FMOD init, nativeGameGlobalInit,
//     nativeUpdateAdapterInit) then the app shell's init step
//     (-> nativeAppBridgeV2InitWithParams). the init step sets its own
//     internal `isInitialized` flag (an internal isInitialized flag) as a real,
//     load-bearing side effect.
//   - AppShellFragment.onCreateView() (`H0()`) builds the real
//     SurfaceView; if a real "surface ready" check
//     (`this.S0.c()`/`this.Q0`) is already true, calls `K2()` ->
//     the app shell's start step (-> nativeAppBridgeV2StartAppWithParams).
//   - the start step's own real body is gated behind
//     that isInitialized flag, i.e. it is a real, silent no-op
//     unless the init step (Init) already ran. Every real call site of
//     the start step (its fragment's own lifecycle) only exists after
//     the fragment's own the init step call already happened.
//
// So the real, load-bearing order is Init before StartApp, the
// reverse of what this file called for a long time. Fixed this session.
//
// ResumeGameWithPlatformParams and StartGameWithParam's real trigger
// was also found this session (superseding the old "unconfirmed"
// framing below): a completely separate real class, a separate app-shell class (real
// package `vi`, not AppShell-related; its own SurfaceView/
// ExperienceSession, "rbx.game" log tag), drives the actual game-join
// flow. Its real `G()` ("updateSurface") is called on every real
// surface event and branches on its own `surfaceState` field:
//   - First call (`surfaceState == 0`): builds real StartGameParams
//     (real placeId/userId/accessCode/... from a real join-info object)
//     and calls `NativeGLInterface.nativeAppBridgeV2StartGameWithParam()`.
//   - A later call (`surfaceState == 2 && !graphicsStarted`): calls
//     `NativeGLInterface.nativeAppBridgeV2ResumeGameWithPlatformParams()`
//     instead (a *different* branch calls
//     `nativeAppBridgeV2UpdateSurfaceGameWithPlatformParams()` if
//     graphics had already started, not currently called by this file
//     at all, real gap, not yet needed for what this file drives).
// So StartGame really does run before ResumeGame, the reverse of
// what this file called for a long time, and separately confirmed by
// the real Sober boot log already on record (`nativeInitClientSettings
// -> nativeAppBridgeAppStart -> nativeAppBridgeV2Init ->
// nativeAppBridgeV2StartGameWithParam`, no ResumeGameWithPlatformParams
// logged in between, consistent with ResumeGame only firing on a
// *second* updateSurface() call this file's own single, one-shot
// sequence never reaches). Real, honest caveat: StartGameWithParam's
// real parameters need a real join/matchmaking response (place ID,
// access code, etc.) this project has no source for yet; Stud's own
// build_desktop_start_game_params() still passes honest placeholders,
// so a real 3D game world isn't expected to render from this alone;
// this fix is about matching the real call order, not about having a
// real game to join yet.
//
// A real, earlier setup step was also missing: the app-shell singleton's own real
// `E(Context)`, called by AppShellFragment.onCreate() BEFORE `j()`
// (Init), calls `NativeGLInterface.nativeGameGlobalInit()` then
// `nativeUpdateAdapterInit()` (both real, confirmed-present, no-arg
// void static natives, the library's exported symbols verified). Root-caused via this
// session's own new near-null-fault pc+backtrace logging
// (trap_recovery.cpp): with the Init-before-StartApp fix alone,
// StartAppWithParams still hit a real, reproducible SIGSEGV,
// `cmpl $0x0,0x140(%r14)` where `%r14` traces back to
// `*(some_internal_context+0x430)`, a lazily-populated subsystem
// pointer still null. `nativeGameGlobalInit()`/`nativeUpdateAdapterInit()`
// are the two real calls AppShell.E() makes that Stud never called at
// all, run before Init now, matching the real E() -> j() -> F() order.

namespace stud::jni_bridge {

struct EngineV2BridgeResult {
    // Real AppShell.E()'s own two calls (nativeGameGlobalInit,
    // nativeUpdateAdapterInit); see this file's own doc comment.
    // Synchronous, not bounded-wait: real, one-time global setup calls,
    // not calls known to block.
    bool app_setup_called = false;
    bool app_setup_trapped_abort = false;
    // Real, load-bearing prerequisites for StartAppWithParams, only
    // attempted when it is (see this file's own UPDATE 2 doc comment):
    // synchronous, not bounded-wait, same as app_setup above: real
    // captures show all three return near-instantly.
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
    // honest report that it's still running/blocked, not a guess about
    // whether it ever will finish. The background thread it runs on
    // keeps going regardless (detached, same precedent as
    // start_app_with_params_background()).
    bool start_app_with_params_still_running = false;
    bool init_with_params_called = false;
    bool init_with_params_trapped_abort = false;
    // See start_app_with_params_still_running above, same bounded-wait
    // treatment applies to this call now too (real evidence, Phase 5:
    // it also genuinely never returned within an 8s bounded wait during
    // a real end-to-end render-host + session test, the same class of
    // blocking behavior StartApp already had).
    bool init_with_params_still_running = false;
    bool resume_game_called = false;
    bool resume_game_trapped_abort = false;
    bool resume_game_still_running = false;
    bool start_game_called = false;
    bool start_game_trapped_abort = false;
    bool start_game_still_running = false;
    // Real, live-traced correction (static analysis of the actual
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

// `jvm` supplies the JNIEnv*. `lib` must already be successfully loaded.
// Runs all four real V2 calls in sequence (best-effort order, see above),
// each independently guarded by the same abort-trap mechanism every
// other real call in this project uses, one call trapping doesn't stop
// the rest from being attempted. `surface` must be the SAME Surface
// object already handed to GameActivity's own lifecycle (see
// GameActivityLifecycleResult::surface's doc comment), reused here,
// not freshly constructed, to avoid a second, real, independently-
// mapped window (the engineering notes, "two windows" entry).
//
// Every one of the four V2 calls below runs on its own detached
// background thread with a real, bounded wait (see each
// EngineV2BridgeResult::*_still_running field's own doc comment) rather
// than being called synchronously on the caller's thread. None of them
// are skipped outright.
//
// Real, a live syscall trace-confirmed finding (this session, superseding the old
// docs' "hangs forever on a network-wait futex" characterization with
// more precise evidence): nativeAppBridgeV2StartAppWithParams spawns a
// real worker-thread pool early (all its clone() calls land in one
// tight ~200ms burst) and then genuinely never returns, one spawned
// thread sits in a permanent, real ~500ms FUTEX_WAIT-with-timeout retry
// loop (not a raw infinite block: it wakes, times out, re-arms,
// forever). Across a full 20s a live syscall trace capture with real network access,
// real /etc (DNS+CA certs), and Binder ruled out (libroblox.so has zero
// Binder symbols), there is not one real outbound connect() anywhere,
// only local sockets (logdw, render-host.sock).
//
// Real, Phase-5-confirmed follow-up: run end-to-end against a real,
// live stud-render-host (real ANGLE + real Wayland window) with a real
// LaunchPayload session (real fetched ClientSettings, delivered over a
// real --ipc-connect socket), StartAppWithParams still never returns
// within the 8s bound, but this time render-client's connection to
// stud-render-host genuinely succeeds (confirmed both by the absence of
// the "failed to connect" log line and by stud-render-host's own log
// showing a real "client connected"). So the earlier hypothesis (this
// being purely a missing-render-host/missing-window problem) is now
// ruled out by direct evidence, not assumption. Also newly confirmed:
// nativeAppBridgeV2InitWithParams, called synchronously at the time,
// blocked for the entire remainder of a 45s run with no "returned" log
// and no trapped signal, i.e. the exact same permanent-block pattern as
// StartAppWithParams. Whatever real login-session/asset-load state
// these calls are waiting on, a real render-host connection alone
// doesn't supply it. Genuinely untested: whether a real session cookie
// (this project has none yet, LaunchPayload::session_cookie is empty
// in every test so far) changes this. Don't assume either way without
// that real test.
EngineV2BridgeResult run_engine_v2_sequence(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                             std::shared_ptr<PlatformParams> platform_params,
                                             std::shared_ptr<DeviceParams> device_params,
                                             std::shared_ptr<InitParams> init_params,
                                             std::shared_ptr<SurfaceStub> surface,
                                             const DeepLinkJoinInfo& deep_link = {},
                                             bool skip_early_init = false);

// Pre-surface half of the real app-shell sequence (nativeGameGlobalInit,
// nativeUpdateAdapterInit, nativeAppBridgeV2InitWithParams). Call this
// BEFORE handing the engine a Surface, then pass
// `skip_early_init=true` to run_engine_v2_sequence() so it is not
// repeated; see the .cpp for the live-caught ordering bug this fixes.
// Re-runs the real UpdateSurfaceApp step for a surface that changed size.
//
// Roblox does not take its surface geometry from AGDK's own
// onSurfaceChangedNative. That fires, and the engine ignores it. Its surface
// handling lives entirely in this V2 app bridge, so a real device re-enters
// the app shell manager's own surface path (ASMA.F) on a rotation or resize.
// Without this the engine keeps its render targets at the size it was told at
// boot: live-measured, the EGL surface really does follow the window
// (1536x792 confirmed via eglQuerySurface) while "SceneManager: resizing main
// targets" stays at 1280x720, which on screen is the old image in one corner
// and uncleared garbage filling the rest.
//
// Bounded on its own thread like every other V2 entry point here, assume it
// can block until proven otherwise.
// Acknowledges a Lua-initiated experience launch by running the real
// nativeAppBridgeV2StartGameWithParam step.
//
// Ground truth from a real, successful Sober join: the engine reaches
// setStage:UGCGame, reports "No DM yet", and then WAITS, the platform side
// acknowledges the start-game request (Sober logs its own
// `app_interface$json: {"type":"did_handle_start_game"}`), and only after that
// does the engine log "[FLog::Network] NetworkClient:Create" and
// "! Joining game '<id>' place <n> at <ip>". Stud never acknowledged, so a
// game picked from the real Home screen sat on the loading screen forever with
// its DataModel never created.
//
// Called from NativeHelperStub::gameActivity_onExperienceStart's hook, which
// runs on the engine's own callback thread, so this dispatches the real work
// to its own bounded background thread and returns immediately.
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
                                    const std::shared_ptr<SurfaceStub>& surface);

void acknowledge_experience_start(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                  const std::shared_ptr<PlatformParams>& platform_params,
                                  const std::shared_ptr<DeviceParams>& device_params,
                                  const std::shared_ptr<SurfaceStub>& surface);

bool notify_surface_resized(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                            const std::shared_ptr<PlatformParams>& platform_params,
                            const std::shared_ptr<SurfaceStub>& surface);

EngineV2BridgeResult run_engine_v2_early_init(FakeJni::Jvm& jvm,
                                                const stud::linker::LoadedLibrary& lib,
                                                std::shared_ptr<InitParams> init_params);

// Real, deliberate departure from run_engine_v2_sequence() above
// (the engineering notes, "make Roblox appear on the window" entry):
// nativeAppBridgeV2StartAppWithParams itself is confirmed (via a real,
// working Sober log's own FLog output) to be what triggers Roblox's own
// 2D "Universal App" LuaApp shell (startLuaApp/setStage(LuaApp)) to
// begin loading, but the call also blocks its calling thread forever
// on a real network-wait futex (confirmed via a real, controlled
// 100-second test), so it can never be called synchronously from the
// main thread without freezing the whole process, event loop and render
// loop included.
//
// Runs it on a real, detached background thread instead, fire-and-
// forget, no result to wait on (there would be nothing meaningful to
// return even if it did complete, since the call is permanently
// blocking). Roblox's own async engine machinery already runs on its
// own real bionic threads regardless of whether this specific call
// ever returns, so a hung caller thread doesn't block the rest of the
// engine (or Stud's own render loop) from making progress.
// `surface` MUST be the same real Surface GameActivity's own lifecycle
// already created: reused here, not freshly constructed, to avoid a
// second, real, independently-mapped window (the engineering notes, "two
// windows" entry).
void start_app_with_params_background(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                       std::shared_ptr<PlatformParams> platform_params,
                                       std::shared_ptr<SurfaceStub> surface);

struct EngineV2TeardownResult {
    bool leave_game_called = false;
    bool leave_game_trapped_abort = false;
    // See run_engine_v2_sequence()'s own *_still_running fields, a
    // real, live SIGTERM test showed these two calls also spawn a
    // background worker and can hang the calling thread forever
    // (UPDATE 3 in this file's own doc comment); routed through the
    // same bounded-wait machinery now.
    bool leave_game_still_running = false;
    bool destroy_app_called = false;
    bool destroy_app_trapped_abort = false;
    bool destroy_app_still_running = false;
};

// Real, graceful shutdown pair, nativeAppBridgeV2LeaveGame() then
// nativeAppBridgeV2DestroyApp(), both real, no-arg, exported symbols
// (the library's exported symbols confirmed); see this file's own UPDATE 2 doc comment for the
// real capture (four-for-four across URI launch, menu launch, fresh
// install, and in-game exit) that ground-truths this as the real
// titlebar-close sequence. LeaveGame is a real, harmless no-op if
// called while not in a game (confirmed live: test 4's own
// "leaveUGCGame: ... no-op, not in-game" log line), so it's always
// safe to call both unconditionally on shutdown regardless of whether
// a game was ever joined. Bounded-wait, not synchronous (see UPDATE 3):
// a real SIGTERM test showed both calls can spawn a background worker
// and never return on the calling thread, hanging Process B's shutdown
// forever with no "shutting down" ever printed.
EngineV2TeardownResult run_engine_v2_teardown(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib);

}  // namespace stud::jni_bridge

namespace stud::jni_bridge {

// Subscribes to the experience-launch request the Lua app publishes on the
// engine's own MessageBus, so Stud learns WHICH experience is being
// launched.
//
// This is the real mechanism, traced through the app's own Java rather
// than guessed: the app's own message-bus subscriber subscribes with
// `MessageBus.f().u(JNIExperienceProtocol.getLaunchId(), callback)`, and
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
