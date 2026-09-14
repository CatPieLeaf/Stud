#include "stud/engine_v2_bridge.h"

#include "stud/bionic_jvm.h"
#include "stud/engine_thread.h"
#include "stud/game_activity_stubs.h"
#include "stud/start_app_params.h"
#include "stud/start_game_params.h"
#include "stud/trap_recovery.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string_view>
#include <thread>

namespace stud::jni_bridge {

namespace {

using V2VoidOneObjFn = void (*)(JNIEnv*, jclass, jobject);
using V2VoidTwoObjFn = void (*)(JNIEnv*, jclass, jobject, jobject);
using V2ResumeGameFn = void (*)(JNIEnv*, jclass, jobject, jobject, jobject);
using V2StartGameFn = jint (*)(JNIEnv*, jclass, jobject);
using V2VoidNoArgFn = void (*)(JNIEnv*, jclass);

struct BoundedCallOutcome {
    bool trapped_abort = false;
    bool still_running = false;
};

// Shared bounded-wait machinery for every V2 call (see
// run_engine_v2_sequence()'s own doc comment: real evidence, Phase 5,
// shows more than just StartAppWithParams genuinely never returns).
// `invoke` runs on a freshly spawned, detached background thread --
// never the calling thread -- and must resolve its own JNI locals on
// its own frame (a jclass/jobject local reference is only valid on the
// thread/frame that created it, so nothing JNI-related may be captured
// from the caller). Returns true iff the call completed without
// trapping a fatal signal; the outer bounded wait treats "still
// running" as an honest, real report, not a guess.
// Same reporting as run_bounded_v2_call, but runs the call on the CALLING
// thread instead of a fresh one -- for the cases where the caller's thread
// identity is itself load-bearing and moving the work off it changes what
// the engine does (see acknowledge_experience_start). There is no bound
// here, by definition: the point is not to hand the work to someone else.
BoundedCallOutcome run_bounded_v2_call_inline(const char* name, std::function<bool()> invoke) {
    std::fprintf(stderr, "stud: v2: calling %s (inline, on the caller's thread) ...\n", name);
    std::fflush(stderr);
    BoundedCallOutcome outcome{};
    const bool ok = invoke();
    outcome.trapped_abort = !ok;
    outcome.still_running = false;
    std::fprintf(stderr, "stud: v2: %s returned (trapped=%d)\n", name, outcome.trapped_abort);
    std::fflush(stderr);
    return outcome;
}

BoundedCallOutcome run_bounded_v2_call(const char* name, std::function<bool()> invoke) {
    auto completed = std::make_shared<std::atomic<bool>>(false);
    auto trapped = std::make_shared<std::atomic<bool>>(false);

    std::fprintf(stderr, "stud: v2: calling %s (background thread, bounded wait) ...\n", name);
    std::fflush(stderr);

    // NOTE (live-tested, deliberately NOT a persistent shared thread):
    // routing every call onto one long-lived Stud thread was tried and
    // reverted. The thread the engine designates as its own internal
    // "main" thread is one libroblox spawns ITSELF (see
    // engine_thread.h's own doc comment for the full trace), never one
    // of Stud's caller threads -- so serialising Stud's calls cannot
    // influence that designation, and it does cause real head-of-line
    // blocking (a call that blocks forever starves every later one;
    // live-observed turning a 50ms `nativeUpdateAdapterInit` into an
    // 8s timeout). A fresh detached thread per call keeps each one
    // independently bounded.
    std::thread([invoke = std::move(invoke), completed, trapped]() mutable {
        bool ok = invoke();
        trapped->store(!ok, std::memory_order_relaxed);
        completed->store(true, std::memory_order_relaxed);
    }).detach();

    constexpr int kPollIntervalMs = 50;
    constexpr int kMaxPolls = 160;  // 8s total
    int polls = 0;
    while (!completed->load(std::memory_order_relaxed) && polls < kMaxPolls) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
        ++polls;
    }

    BoundedCallOutcome outcome;
    if (completed->load(std::memory_order_relaxed)) {
        outcome.trapped_abort = trapped->load(std::memory_order_relaxed);
        std::fprintf(stderr, "stud: v2: %s %s within %dms\n", name,
                      outcome.trapped_abort ? "trapped a fatal signal" : "returned normally",
                      polls * kPollIntervalMs);
    } else {
        outcome.still_running = true;
        std::fprintf(stderr,
                      "stud: v2: %s still running/blocked after %dms -- its background thread "
                      "keeps going, not waiting further\n",
                      name, kMaxPolls * kPollIntervalMs);
    }
    std::fflush(stderr);
    return outcome;
}

// Real, shared JNI plumbing for one UpdateSurfaceApp+UpdateSurfaceGame
// pair -- factored out of the STUD_ENABLE_V2_STARTAPP-gated block below
// so run_engine_v2_sequence()'s own default path can reuse it. Real
// evidence for calling this on the default path at all: a real, working
// Sober session's own FLog capture (see the engineering notes) shows both
// calls firing twice each, right after a successful
// nativeAppBridgeV2StartGameWithParam -- a different real context than
// the StartApp-prerequisite one this block was originally written for,
// but the same real JNI call shape.
//
// Real, live-caught bug this session (not a hypothetical): calling this
// synchronously hung the whole boot sequence forever on the direct-launch
// path -- no crash, no STUD_TRAP, `run_engine_v2_sequence()` just never
// returned. Exactly the documented "any new V2 entry point can block
// forever, assume it does until live-tested otherwise" rule
// (engine_v2_bridge.h's own doc comment) -- this function itself does
// NOT protect against that; every call site MUST wrap it in
// run_bounded_v2_call(), same as every other V2 call in this file.
// `called_app`/`called_game` are written unconditionally (even if the
// bounded wait times out) since "was this symbol found and the call
// attempted" is real and known immediately; the caller decides whether
// to trust `*trapped_app`/`*trapped_game` based on the bounded-wait
// outcome it gets back separately.
bool call_update_surface_pair(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                               const std::shared_ptr<PlatformParams>& platform_params,
                               const std::shared_ptr<SurfaceStub>& surface,
                               const std::shared_ptr<std::atomic<bool>>& called_app,
                               const std::shared_ptr<std::atomic<bool>>& called_game,
                               bool update_game_surface) {
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);
    jclass native_gl_interface_class = env.FindClass("com/roblox/engine/jni/NativeGLInterface");
    bool ok = true;

    if (void* update_app_addr = lib.find_symbol(
            "Java_com_roblox_engine_jni_NativeGLInterface_"
            "nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams");
        update_app_addr != nullptr) {
        called_app->store(true, std::memory_order_relaxed);
        auto* update_app_fn = reinterpret_cast<V2VoidTwoObjFn>(update_app_addr);
        jobject surface_ref = env.createLocalReference(surface);
        jobject platform_params_ref = env.createLocalReference(platform_params);
        ok = call_trapping_abort(update_app_fn, jni_env, native_gl_interface_class, surface_ref,
                                  platform_params_ref) &&
             ok;
        clear_pending_jni_exception(jni_env, "nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams");
    } else {
        std::fprintf(stderr,
                      "stud: engine_v2_bridge: "
                      "nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams not found, skipping\n");
    }

    // CORRECTED (this comment used to claim the Game variant TEARS DOWN
    // rendering, citing a SurfaceController/RenderView teardown in the
    // engine's own FLog immediately after this pair runs). That was a real
    // log excerpt but a wrong causal reading: the teardown is triggered by
    // `nativeAppBridgeV2Init` arriving AFTER the surface had already been
    // handed over, and it precedes this call rather than following from it.
    // Fixed separately by splitting the V2 sequence so app setup and
    // InitWithParams run BEFORE the surface (the real `ASMA.E -> j -> F`
    // order) -- see run_engine_v2_early_init(). Geometry now draws.
    //
    // The Game variant is still correctly skipped on a bare launch, for the
    // reason below, which was always the sound one.
    //
    // It matches the real device: this file's own from-scratch
    // logcat re-read found a real bare launch calls
    // `UpdateSurfaceAppWithPlatformParams` exactly ONCE and never the
    // Game variant -- the Game surface belongs to an actual experience,
    // which a bare/home-screen launch does not have. Gated on a real
    // deep link accordingly.
    if (!update_game_surface) {
        std::fprintf(stderr,
                      "stud: engine_v2_bridge: skipping "
                      "nativeAppBridgeV2UpdateSurfaceGameWithPlatformParams -- no real "
                      "experience on a bare launch\n");
    } else if (void* update_game_addr = lib.find_symbol(
            "Java_com_roblox_engine_jni_NativeGLInterface_"
            "nativeAppBridgeV2UpdateSurfaceGameWithPlatformParams");
        update_game_addr != nullptr) {
        called_game->store(true, std::memory_order_relaxed);
        auto* update_game_fn = reinterpret_cast<V2ResumeGameFn>(update_game_addr);
        auto activity = std::make_shared<ActivityStub>();
        jobject surface_ref = env.createLocalReference(surface);
        jobject platform_params_ref = env.createLocalReference(platform_params);
        jobject activity_ref = env.createLocalReference(std::move(activity));
        ok = call_trapping_abort(update_game_fn, jni_env, native_gl_interface_class, surface_ref,
                                  platform_params_ref, activity_ref) &&
             ok;
        clear_pending_jni_exception(jni_env, "nativeAppBridgeV2UpdateSurfaceGameWithPlatformParams");
    } else {
        std::fprintf(stderr,
                      "stud: engine_v2_bridge: "
                      "nativeAppBridgeV2UpdateSurfaceGameWithPlatformParams not found, skipping\n");
    }
    return ok;
}

}  // namespace

// Real app-shell order (the app's own app-shell manager, the "ASMA" class): `E(context)`
// does nativeGameGlobalInit/nativeUpdateAdapterInit, `j(d)` does
// nativeAppBridgeV2InitWithParams, and only then does `F(surface)`
// hand over a real Surface. Extracted here so both the normal sequence
// and the early, pre-surface path can run exactly the same calls --
// live evidence for why that matters is in run_engine_v2_early_init()'s
// own doc comment.
void perform_early_init(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                         const std::shared_ptr<InitParams>& init_params,
                         EngineV2BridgeResult& result) {
    {
        bool ok = true;
        bool any_still_running = false;
        // The real app-shell manager calls FMOD.init(context) immediately
        // before nativeGameGlobalInit (the app's own app-shell manager: `FMOD.init(context);`
        // then `NativeGLInterface.nativeGameGlobalInit();`), and Stud
        // never did -- so org.fmod.FMOD.checkInit() answered false and
        // getAssetManager() returned null for the engine's own audio
        // system, which is the Java state native FMOD reads before it will
        // initialise its output.
        FMODStub::init(std::make_shared<ContextStub>());

        if (void* addr = lib.find_symbol(
                "Java_com_roblox_engine_jni_NativeGLInterface_nativeGameGlobalInit");
            addr != nullptr) {
            result.app_setup_called = true;
            auto* fn = reinterpret_cast<V2VoidNoArgFn>(addr);
            auto outcome = run_bounded_v2_call("nativeGameGlobalInit", [&jvm, fn] {
                static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
                FakeJni::LocalFrame inner_frame(jvm);
                auto& inner_env = inner_frame.getJniEnv();
                auto* inner_jni_env = static_cast<JNIEnv*>(&inner_env);
                jclass inner_class = inner_env.FindClass("com/roblox/engine/jni/NativeGLInterface");
                bool inner_ok = call_trapping_abort(fn, inner_jni_env, inner_class);
                clear_pending_jni_exception(inner_jni_env, "nativeGameGlobalInit");
                return inner_ok;
            });
            ok = !outcome.trapped_abort && ok;
            any_still_running = any_still_running || outcome.still_running;
        } else {
            std::fprintf(stderr,
                          "stud: engine_v2_bridge: nativeGameGlobalInit not found, skipping\n");
        }
        if (void* addr = lib.find_symbol(
                "Java_com_roblox_engine_jni_NativeGLInterface_nativeUpdateAdapterInit");
            addr != nullptr) {
            result.app_setup_called = true;
            auto* fn = reinterpret_cast<V2VoidNoArgFn>(addr);
            auto outcome = run_bounded_v2_call("nativeUpdateAdapterInit", [&jvm, fn] {
                static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
                FakeJni::LocalFrame inner_frame(jvm);
                auto& inner_env = inner_frame.getJniEnv();
                auto* inner_jni_env = static_cast<JNIEnv*>(&inner_env);
                jclass inner_class = inner_env.FindClass("com/roblox/engine/jni/NativeGLInterface");
                bool inner_ok = call_trapping_abort(fn, inner_jni_env, inner_class);
                clear_pending_jni_exception(inner_jni_env, "nativeUpdateAdapterInit");
                return inner_ok;
            });
            ok = !outcome.trapped_abort && ok;
            any_still_running = any_still_running || outcome.still_running;
        } else {
            std::fprintf(stderr,
                          "stud: engine_v2_bridge: nativeUpdateAdapterInit not found, skipping\n");
        }
        result.app_setup_trapped_abort = !ok;
        std::fprintf(stderr,
                      "stud: v2: app setup (nativeGameGlobalInit/nativeUpdateAdapterInit) %s%s\n",
                      ok ? "ok" : "trapped a fatal signal",
                      any_still_running ? " (at least one call still running/blocked)" : "");
        std::fflush(stderr);
    }

    // nativeAppBridgeV2InitWithParams(InitParams)
    if (void* addr = lib.find_symbol(
            "Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2InitWithParams");
        addr != nullptr) {
        result.init_with_params_called = true;
        auto* fn = reinterpret_cast<V2VoidOneObjFn>(addr);
        auto outcome =
            run_bounded_v2_call("nativeAppBridgeV2InitWithParams", [&jvm, fn, init_params] {
                static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
                FakeJni::LocalFrame inner_frame(jvm);
                auto& inner_env = inner_frame.getJniEnv();
                auto* inner_jni_env = static_cast<JNIEnv*>(&inner_env);
                jclass inner_class = inner_env.FindClass("com/roblox/engine/jni/NativeGLInterface");
                jobject params_ref = inner_env.createLocalReference(init_params);
                bool ok = call_trapping_abort(fn, inner_jni_env, inner_class, params_ref);
                clear_pending_jni_exception(inner_jni_env, "engine_v2_bridge");
                return ok;
            });
        result.init_with_params_trapped_abort = outcome.trapped_abort;
        result.init_with_params_still_running = outcome.still_running;
    } else {
        std::fprintf(
            stderr,
            "stud: engine_v2_bridge: nativeAppBridgeV2InitWithParams not found, skipping\n");
    }
}

// Runs ONLY the pre-surface half of the real app-shell sequence.
//
// Real, live-caught ordering bug this fixes (engine FLog, via Stud's
// logd sink): Stud drove `onSurfaceCreatedNative` first, and the engine
// responded by bootstrapping its own Lua app to completion
// (`setStage: LuaApp`, `userDidLogin`) -- and only THEN did Stud's
// `nativeAppBridgeV2Init` arrive, which reset `setStage: (stage:Native)`
// and destroyed the live SurfaceController/RenderView underneath it.
// A real device does the reverse: init first, surface last
// (`ASMA.E` -> `ASMA.j` -> `ASMA.F(surface)`).
EngineV2BridgeResult run_engine_v2_early_init(FakeJni::Jvm& jvm,
                                                const stud::linker::LoadedLibrary& lib,
                                                std::shared_ptr<InitParams> init_params) {
    EngineV2BridgeResult result;
    perform_early_init(jvm, lib, init_params, result);
    return result;
}

EngineV2BridgeResult run_engine_v2_sequence(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                             std::shared_ptr<PlatformParams> platform_params,
                                             std::shared_ptr<DeviceParams> device_params,
                                             std::shared_ptr<InitParams> init_params,
                                             std::shared_ptr<SurfaceStub> surface,
                                             const DeepLinkJoinInfo& deep_link,
                                             bool skip_early_init) {
    EngineV2BridgeResult result;

    // Real order (ground-truth, read out of the app's own code this session against the
    // app itself -- see engine_v2_bridge.h's own doc
    // comment): InitWithParams MUST run before StartAppWithParams, not
    // the reverse. The real Java caller of both, the app-shell singleton ((an "AppShell"
    // singleton) -- the app shell's init step sets its own internal
    // isInitialized flag (an internal isInitialized flag) as a side effect;
    // the app shell's start step (-> StartAppWithParams) is itself gated behind
    // that isInitialized flag and is a real, silent no-op otherwise. Every
    // real caller of the start step (its fragment's own lifecycle)
    // only runs after the init step already ran, in AppShellFragment's own
    // onCreate(). Previously ordered the other way here, based on a
    // best-effort guess from the method names alone -- confirmed wrong.

    // Real AppShell setup step's own two calls -- see this function's
    // own doc comment (engine_v2_bridge.h) for why these are needed
    // before Init/StartApp at all: root-caused via a real, reproducible
    // near-null SIGSEGV whose backtrace pc traced back to a lazily-
    // populated subsystem pointer these two calls are the real,
    // confirmed (the library's exported symbols) way to populate.
    // Real, live-caught bug (the engineering notes, "keep going. until now,
    // there's no kde window at all" -- the crash-recovery fixes landed
    // earlier this session let real execution reach this call for the
    // first time, and it hung the main thread forever, blocking
    // everything downstream including render-context setup, which is
    // why no window ever appeared). This ran synchronously on the
    // calling (main) thread via plain call_trapping_abort(), unlike
    // every other V2 entry point in this file, which already goes
    // through run_bounded_v2_call() specifically because this project's
    // own established rule is "assume any new V2 call can block forever
    // until live-tested otherwise." Confirmed live via a live syscall trace on the
    // genuinely stuck main thread: blocked in a real futex wait deep
    // inside libroblox.so, directly under this exact call --
    // nativeGameGlobalInit had simply never been exercised long enough
    // before today's other fixes for this to be noticed. Fixed by
    // routing both calls through the same bounded-wait machinery.
    if (skip_early_init) {
        std::fprintf(stderr,
                     "stud: v2: app setup + InitWithParams already ran before the surface was "
                     "handed over (real ASMA order), not repeating them\n");
        result.init_with_params_called = true;
    } else {
        perform_early_init(jvm, lib, init_params, result);
    }

    // nativeAppBridgeV2StartAppWithParams(StartAppParams) -- UPDATE,
    // 2026-09-07, and this one is settled by a real, successful join
    // rather than by an absence of evidence.
    //
    // The previous comment here concluded this call is "not part of a
    // real bare launch at all", from grepping two real logcat captures
    // and finding zero matches. That was an absence-of-evidence error:
    // the logcat lines are tagged by the engine's own FLog, which
    // prints this entry point as `nativeAppBridgeV2StartApp:` (no
    // `WithParams` suffix), so the grep could never have matched.
    //
    // A real Sober session captured on this machine that joins a real
    // game successfully end to end
    // (`~/.var/app/org.vinegarhq.Sober/data/sober/appData/logs/
    // 2.737.0.1584_20260907T234811Z_Player_4d14f_last.log`) shows it
    // firing on a plain bare launch, and shows exactly what it is for:
    //
    //   5.828  setStage: (stage:InitializedLuaApp)
    //   5.829  nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams:
    //   5.830  nativeAppBridgeV2UpdateSurfaceGameWithPlatformParams:
    //   6.296  nativeAppBridgeV2StartApp:
    //   6.296  startLuaApp: (stage:InitializedLuaApp).
    //   6.296  returnToLuaApp / returnToLuaAppInternal
    //   6.296  replaceDataModel: (stage:0) / RenderView created[1]
    //   6.669  SurfaceController[_:1]::run -> setStage: (stage:LuaApp)
    //
    // StartApp is what advances the stage from `InitializedLuaApp` to
    // `LuaApp`. That matters far beyond a cosmetic state name: when the
    // user later picks a game, `launchUGCGame` branches on it. From
    // `LuaApp` it runs `pauseLuaAppAndDestroyIfNeeded` -> `pauseLuaApp`
    // -> `SurfaceController::pause destroyView:true` ->
    // `RenderView destroyed[1]` before building the experience's own
    // DataModel, and the join completes (`NetworkClient:Create`,
    // `! Joining game ...`). Stud, stuck at `InitializedLuaApp`, took
    // the other branch, skipped that teardown entirely, and the
    // submitted start-game task never produced a DataModel --
    // `stepDataModelJob: No DM yet. Continue...` forever.
    //
    // So this is unconditional on a bare launch now, in the real ASMA
    // order (after the UpdateSurface pair, not before it).
    // `STUD_DISABLE_V2_STARTAPP=1` skips it, for bisecting only.
    bool bare_launch = deep_link.place_id == 0;
    const char* disable_startapp_env = std::getenv("STUD_DISABLE_V2_STARTAPP");
    bool startapp_disabled =
        disable_startapp_env != nullptr && std::string_view(disable_startapp_env) == "1";
    // A deep link needs this every bit as much as a bare launch does.
    //
    // It used to be bare-launch only, so a browser click went straight to
    // StartGameWithParam with the app still at `InitializedLuaApp` --
    // which is precisely the stage `launchUGCGame` branches on (see the
    // comment above): from `LuaApp` it tears the app view down and builds
    // the experience's DataModel, and from `InitializedLuaApp` it does
    // neither, so the submitted start-game task never produces one. The
    // in-app path (pick a game on Home) works today exactly because
    // StartApp has already run by then.
    bool want_startapp = !startapp_disabled;

    // nativeAppBridgeStartLuaAppDM + the single UpdateSurfaceApp/Game
    // pair that follows it -- real, load-bearing, unconditional part of
    // a bare launch (confirmed identically in BOTH real captures below),
    // pulled out from under the StartAppWithParams conditional they used
    // to live inside: that nesting made both of these calls wrongly
    // depend on `force_startapp`, when real evidence shows they're
    // needed regardless of whether StartAppWithParams is ever called at
    // all (it isn't, on this real path -- see that block's own doc
    // comment right below).
    if (bare_launch) {
        if (void* dm_addr = lib.find_symbol(
                "Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeStartLuaAppDM");
            dm_addr != nullptr) {
            result.lua_app_dm_called = true;
            auto* dm_fn = reinterpret_cast<V2VoidNoArgFn>(dm_addr);
            // Real, live-caught bug (the engineering notes): this call used to
            // run synchronously via a bare call_trapping_abort() on the
            // calling thread -- but its real implementation dispatches
            // the actual work (initializeLuaAppWithLoggedInUser) to a
            // separate internal worker thread and blocks the calling
            // thread waiting for a completion signal. Wrapping it in
            // run_bounded_v2_call(), like every other V2 entry point
            // that might block, lets real bring-up continue after a
            // bound instead of hanging the whole sequence forever.
            FakeJni::Jvm* jvm_ptr = &jvm;
            auto outcome = run_bounded_v2_call("nativeAppBridgeStartLuaAppDM", [jvm_ptr, dm_fn] {
                static_cast<BionicAwareJvm&>(*jvm_ptr).ensure_env_for_current_thread();
                FakeJni::LocalFrame prereq_frame(*jvm_ptr);
                auto& prereq_env = prereq_frame.getJniEnv();
                auto* prereq_jni_env = static_cast<JNIEnv*>(&prereq_env);
                jclass prereq_class = prereq_env.FindClass("com/roblox/engine/jni/NativeGLInterface");
                bool ok = call_trapping_abort(dm_fn, prereq_jni_env, prereq_class);
                clear_pending_jni_exception(prereq_jni_env, "nativeAppBridgeStartLuaAppDM");
                return ok;
            });
            result.lua_app_dm_trapped_abort = outcome.trapped_abort;
        } else {
            std::fprintf(stderr,
                          "stud: engine_v2_bridge: nativeAppBridgeStartLuaAppDM not found, "
                          "skipping\n");
        }
        {
            auto called_app = std::make_shared<std::atomic<bool>>(false);
            auto called_game = std::make_shared<std::atomic<bool>>(false);
            // Both halves, App then Game. The real successful-join Sober
            // capture (see the StartApp doc comment above) calls
            // UpdateSurfaceApp and UpdateSurfaceGame back to back here,
            // ~150us apart, both at (stage:InitializedLuaApp), before
            // StartApp. Stud used to send only the App half on the
            // reasoning that a bare launch has no experience yet -- but
            // the engine wants the game surface registered up front, not
            // at join time.
            auto pair_outcome = run_bounded_v2_call(
                "UpdateSurfaceApp/Game pair (bare-launch path)",
                [&jvm, &lib, platform_params, surface, called_app, called_game] {
                    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
                    return call_update_surface_pair(jvm, lib, platform_params, surface, called_app,
                                                     called_game, /*update_game_surface=*/true);
                });
            result.update_surface_app_trapped_abort = pair_outcome.trapped_abort;
            result.update_surface_game_trapped_abort = pair_outcome.trapped_abort;
            result.update_surface_app_called = called_app->load(std::memory_order_relaxed);
            result.update_surface_game_called = called_game->load(std::memory_order_relaxed);
        }
    }

    if (void* addr = lib.find_symbol(
            "Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2StartAppWithParams");
        addr != nullptr && want_startapp) {
        result.start_app_with_params_called = true;
        auto* fn = reinterpret_cast<V2VoidOneObjFn>(addr);
        // Real correctness point (JNI local refs are only valid on the
        // thread/frame that created them): everything JNI-related here
        // is resolved fresh inside the background thread's own frame,
        // not reused from any outer scope. Only real, thread-safe
        // values (shared_ptr copies, the raw jvm reference) cross the
        // thread boundary.
        auto outcome =
            run_bounded_v2_call("nativeAppBridgeV2StartAppWithParams", [&jvm, fn, platform_params, surface] {
                static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
                FakeJni::LocalFrame inner_frame(jvm);
                auto& inner_env = inner_frame.getJniEnv();
                auto* inner_jni_env = static_cast<JNIEnv*>(&inner_env);
                jclass inner_class = inner_env.FindClass("com/roblox/engine/jni/NativeGLInterface");
                auto start_app_params = build_desktop_start_app_params(platform_params, surface);
                jobject params_ref = inner_env.createLocalReference(std::move(start_app_params));
                bool ok = call_trapping_abort(fn, inner_jni_env, inner_class, params_ref);
                clear_pending_jni_exception(inner_jni_env, "engine_v2_bridge");
                return ok;
            });
        result.start_app_with_params_trapped_abort = outcome.trapped_abort;
        result.start_app_with_params_still_running = outcome.still_running;
    } else if (lib.find_symbol(
                   "Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2StartAppWithParams") ==
               nullptr) {
        std::fprintf(
            stderr,
            "stud: engine_v2_bridge: nativeAppBridgeV2StartAppWithParams not found, skipping\n");
    } else {
        std::fprintf(stderr,
                      "stud: engine_v2_bridge: nativeAppBridgeV2StartAppWithParams skipped (%s)\n",
                      startapp_disabled ? "STUD_DISABLE_V2_STARTAPP=1"
                                        : "not a bare launch -- a deep link goes straight to "
                                          "StartGameWithParam");
    }

    // Real order (ground-truth, this session's read of the app's own code against the app's own app-shell helper
    // -- a real, separate "game join" class, distinct from AppShell/
    // AppShellFragment): StartGameWithParam runs on the FIRST real
    // updateSurface() call (surfaceState==0), ResumeGameWithPlatformParams
    // on the SECOND (surfaceState==2 && !graphicsStarted) -- the reverse
    // of the order this file used before. See engine_v2_bridge.h's own
    // doc comment.

    // Real, live-caught bug (the engineering notes, the "stop trying to force
    // StartGameWithParam" user correction): this call used to fire
    // unconditionally for EVERY launch, bare or not. On a real device,
    // StartGameWithParam only ever runs either (a) immediately, for a
    // deep link that already names a place (matches the real Sober
    // capture this file's own doc comments cite), or (b) after a real
    // human picks a game from the home/Lua UI -- something a bare,
    // no-deep-link launch has no way to synthesize headlessly. Calling
    // it anyway, seconds after boot, races the real home-UI/Lua-side
    // state machine before it has ever settled. Gated: only for a real
    // deep link, or with the explicit force-enable env var below, since
    // Stud has no real home-screen UI a human could click through.
    bool force_startgame = std::getenv("STUD_ENABLE_V2_STARTGAME") != nullptr &&
                            std::string_view(std::getenv("STUD_ENABLE_V2_STARTGAME")) == "1";
    if (void* addr = lib.find_symbol(
            "Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2StartGameWithParam");
        addr != nullptr && (!bare_launch || force_startgame)) {
        result.start_game_called = true;
        auto* fn = reinterpret_cast<V2StartGameFn>(addr);
        auto start_game_result = std::make_shared<jint>(0);
        auto outcome = run_bounded_v2_call(
            "nativeAppBridgeV2StartGameWithParam",
            [&jvm, fn, platform_params, device_params, surface, start_game_result, deep_link] {
                static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
                FakeJni::LocalFrame inner_frame(jvm);
                auto& inner_env = inner_frame.getJniEnv();
                auto* inner_jni_env = static_cast<JNIEnv*>(&inner_env);
                jclass inner_class = inner_env.FindClass("com/roblox/engine/jni/NativeGLInterface");
                auto start_game_params =
                    build_desktop_start_game_params(platform_params, device_params, surface, deep_link);
                jobject params_ref = inner_env.createLocalReference(std::move(start_game_params));
                bool ok = call_trapping_abort_with_result(fn, *start_game_result, inner_jni_env,
                                                           inner_class, params_ref);
                clear_pending_jni_exception(inner_jni_env, "nativeAppBridgeV2StartGameWithParam");
                return ok;
            });
        result.start_game_trapped_abort = outcome.trapped_abort;
        result.start_game_still_running = outcome.still_running;
        result.start_game_result = *start_game_result;

        // Real, evidence-backed addition (a real, successful Sober FLog
        // capture, the engineering notes): on the SAME direct-launch path this
        // function already drives, both UpdateSurfaceApp and
        // UpdateSurfaceGame fire twice each, right after
        // StartGameWithParam completes -- previously only ever called
        // from the STUD_ENABLE_V2_STARTAPP-gated block below, a
        // different real context (a StartApp prerequisite), so the
        // default path never made these calls at all. Gated on the call
        // actually completing normally (not trapped, not still running)
        // -- calling further JNI into a process that just trapped a
        // fatal signal isn't a real scenario worth reproducing.
        if (!outcome.trapped_abort && !outcome.still_running) {
            auto called_app = std::make_shared<std::atomic<bool>>(false);
            auto called_game = std::make_shared<std::atomic<bool>>(false);
            for (int i = 0; i < 2; ++i) {
                auto pair_outcome = run_bounded_v2_call(
                    "UpdateSurfaceApp/Game pair (direct-launch path)",
                    [&jvm, &lib, platform_params, surface, called_app, called_game] {
                        static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
                        return call_update_surface_pair(jvm, lib, platform_params, surface,
                                                         called_app, called_game,
                                                         /*update_game_surface=*/true);
                    });
                result.update_surface_app_trapped_abort =
                    result.update_surface_app_trapped_abort || pair_outcome.trapped_abort;
                result.update_surface_game_trapped_abort =
                    result.update_surface_game_trapped_abort || pair_outcome.trapped_abort;
                if (pair_outcome.still_running) {
                    // Real, honest degrade: don't attempt the second pair
                    // if the first one is still blocked in the
                    // background -- a second concurrent call into the
                    // same native entry points while the first hasn't
                    // returned isn't a real scenario worth reproducing.
                    break;
                }
            }
            result.update_surface_app_called = called_app->load(std::memory_order_relaxed);
            result.update_surface_game_called = called_game->load(std::memory_order_relaxed);
        }
    } else if (lib.find_symbol(
                   "Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2StartGameWithParam") ==
               nullptr) {
        std::fprintf(
            stderr,
            "stud: engine_v2_bridge: nativeAppBridgeV2StartGameWithParam not found, skipping\n");
    } else {
        std::fprintf(stderr,
                      "stud: engine_v2_bridge: nativeAppBridgeV2StartGameWithParam skipped -- no "
                      "real deep link (a bare/home-screen launch needs real user interaction to "
                      "pick a game, which this can't synthesize; set STUD_ENABLE_V2_STARTGAME=1 "
                      "to force it anyway)\n");
    }

    // nativeAppBridgeV2ResumeGameWithPlatformParams(Surface, PlatformParams, Activity)
    // -- real evidence this session (same real Sober journalctl capture
    // that showed StartApp is never called): NOTHING V2-related happens
    // after StartGameWithParam in that whole real ~12s capture, from
    // process start through the game actually loading -- no
    // ResumeGameWithPlatformParams either. Matches this file's own
    // earlier doc comment (engine_v2_bridge.h): ResumeGame's real
    // trigger is the *second* real updateSurface() call
    // (surfaceState==2 && !graphicsStarted), which calling it here,
    // immediately after StartGame, doesn't actually correspond to.
    // Real, live-confirmed symptom this session with StartApp fixed:
    // this call now reliably crashes on a null controller pointer
    // (inside SingleSurfaceApp's own real
    // `pauseBeforeResume` branch) -- consistent with calling it at a
    // point in the real lifecycle it isn't meant to run yet. Default
    // OFF now, opt-in via STUD_ENABLE_V2_RESUMEGAME=1, same pattern as
    // STUD_ENABLE_V2_STARTAPP above.
    if (void* addr = lib.find_symbol(
            "Java_com_roblox_engine_jni_NativeGLInterface_"
            "nativeAppBridgeV2ResumeGameWithPlatformParams");
        addr != nullptr && std::getenv("STUD_ENABLE_V2_RESUMEGAME") != nullptr &&
        std::string_view(std::getenv("STUD_ENABLE_V2_RESUMEGAME")) == "1") {
        result.resume_game_called = true;
        auto* fn = reinterpret_cast<V2ResumeGameFn>(addr);
        auto outcome = run_bounded_v2_call(
            "nativeAppBridgeV2ResumeGameWithPlatformParams", [&jvm, fn, platform_params, surface] {
                static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
                FakeJni::LocalFrame inner_frame(jvm);
                auto& inner_env = inner_frame.getJniEnv();
                auto* inner_jni_env = static_cast<JNIEnv*>(&inner_env);
                jclass inner_class = inner_env.FindClass("com/roblox/engine/jni/NativeGLInterface");
                auto activity = std::make_shared<ActivityStub>();
                jobject surface_ref = inner_env.createLocalReference(surface);
                jobject platform_params_ref = inner_env.createLocalReference(platform_params);
                jobject activity_ref = inner_env.createLocalReference(std::move(activity));
                bool ok = call_trapping_abort(fn, inner_jni_env, inner_class, surface_ref,
                                               platform_params_ref, activity_ref);
                clear_pending_jni_exception(inner_jni_env,
                                             "nativeAppBridgeV2ResumeGameWithPlatformParams");
                return ok;
            });
        result.resume_game_trapped_abort = outcome.trapped_abort;
        result.resume_game_still_running = outcome.still_running;
    } else {
        std::fprintf(stderr,
                      "stud: engine_v2_bridge: nativeAppBridgeV2ResumeGameWithPlatformParams not "
                      "found, skipping\n");
    }

    return result;
}

void start_app_with_params_background(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                       std::shared_ptr<PlatformParams> platform_params,
                                       std::shared_ptr<SurfaceStub> surface) {
    void* addr = lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2StartAppWithParams");
    if (addr == nullptr) {
        std::fprintf(
            stderr,
            "stud: engine_v2_bridge: nativeAppBridgeV2StartAppWithParams not found, skipping\n");
        return;
    }

    std::thread([&jvm, addr, platform_params = std::move(platform_params),
                 surface = std::move(surface)]() mutable {
        // Real fix (the engineering notes, "now proceed" entry, root-caused via
        // a live bisection print): FakeJni::LocalFrame(jvm)'s own ctor,
        // on a thread's FIRST-ever attach, falls through to
        // vm.AttachCurrentThread(nullptr, nullptr) -- which dispatches
        // through the JNI invoke-interface's AttachCurrentThread slot.
        // Roblox's own compiled code overwrites that exact slot with its
        // own function (the already-documented "JNI vtable swap" -- real,
        // legitimate Android JNI hook-install behavior, not corruption),
        // and THAT function does real internal work that recurses into
        // the already-known, externally-gated mimalloc/protobuf bootstrap
        // bug (github.com/microsoft/mimalloc/issues/1362) on this
        // thread's first-ever allocation. The main thread never hits this
        // because it's already attached (via this exact call, made once
        // at startup) before LocalFrame is ever constructed on it.
        // ensure_env_for_current_thread() attaches THIS thread the same
        // way, through a real patched-in jnivm method
        // (EnsureEnvForCurrentThread(), see patch_libjnivm.cmake) that
        // does the exact lock+CreateEnv()+store the default
        // AttachCurrentThread lambda uses -- a direct C++ call, never
        // touching the (Roblox-overwritten) JNI vtable slot at all -- so
        // the subsequent LocalFrame construction below finds an
        // already-populated env and never calls AttachCurrentThread,
        // sidestepping the crash entirely. Confirmed working end-to-end:
        // real Roblox FLog output now shows nativeAppBridgeV2StartApp ->
        // startLuaApp -> setStage(LuaApp), the first time this project
        // has ever reached the real 2D LuaApp shell trigger.
        static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
        FakeJni::LocalFrame frame(jvm);
        auto& env = frame.getJniEnv();
        auto* jni_env = static_cast<JNIEnv*>(&env);

        // Real jclass, not nullptr -- see NativeGLInterfaceStub's own doc
        // comment (game_activity_stubs.h): a real, reproducible SIGSEGV
        // a null jclass dereferenced a few
        // instructions into this exact function.
        jclass native_gl_interface_class = env.FindClass("com/roblox/engine/jni/NativeGLInterface");

        auto start_app_params =
            build_desktop_start_app_params(std::move(platform_params), std::move(surface));
        jobject params_ref = env.createLocalReference(std::move(start_app_params));
        auto* fn = reinterpret_cast<V2VoidOneObjFn>(addr);

        // Root-caused live:
        // this call's internal
        // telemetry-logging helper dereferences a null object a few
        // calls deep -- not our jclass/params (both confirmed real and
        // non-null at the crash), most likely a Roblox-internal
        // singleton real Android populates before this path runs that
        // Stud's bring-up doesn't yet. Out of scope to chase further
        // right now (this call is already documented as optional/
        // best-effort, on its own detached thread), so tolerate a wild
        // SIGSEGV here specifically rather than losing the whole
        // process to it -- see call_trapping_abort_tolerating_wild_
        // sigsegv()'s own doc comment for why this is scoped to just
        // this one call site.
        std::fprintf(stderr, "stud: v2: calling nativeAppBridgeV2StartAppWithParams "
                              "(background thread, may never return) ...\n");
        std::fflush(stderr);
        bool completed =
            call_trapping_abort_tolerating_wild_sigsegv(fn, jni_env, native_gl_interface_class, params_ref);
        clear_pending_jni_exception(jni_env, "nativeAppBridgeV2StartAppWithParams");
        std::fprintf(stderr,
                      "stud: v2: nativeAppBridgeV2StartAppWithParams %s (background thread)\n",
                      completed ? "returned" : "trapped a fatal signal mid-call");
        std::fflush(stderr);
    }).detach();
}

EngineV2TeardownResult run_engine_v2_teardown(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib) {
    EngineV2TeardownResult result;

    // Real, live-tested correction: LeaveGame/DestroyApp are NOT safe to
    // call synchronously, same as Init/StartApp/ResumeGame above -- a
    // real SIGTERM-triggered test this session hung the whole process
    // forever right here (no "shutting down" ever printed, STUD_TRAP
    // fired on a real background worker thread these calls spawn
    // internally, and the calling thread's own call_trapping_abort()
    // never returned because it was blocked joining that thread). Routed
    // through the same run_bounded_v2_call() machinery as every other
    // V2 call now -- "still running" after the bound is an honest report
    // Process B's own shutdown path doesn't wait further on, not a
    // guess.

    // Real order (see engine_v2_bridge.h's own UPDATE 2 doc comment):
    // LeaveGame always first, even if no game was ever joined -- real,
    // confirmed harmless no-op in that case ("leaveUGCGame: ... no-op,
    // not in-game").
    if (void* addr =
            lib.find_symbol("Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2LeaveGame");
        addr != nullptr) {
        result.leave_game_called = true;
        auto* fn = reinterpret_cast<V2VoidNoArgFn>(addr);
        auto outcome = run_bounded_v2_call("nativeAppBridgeV2LeaveGame", [&jvm, fn] {
            static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
            FakeJni::LocalFrame inner_frame(jvm);
            auto& inner_env = inner_frame.getJniEnv();
            auto* inner_jni_env = static_cast<JNIEnv*>(&inner_env);
            jclass inner_class = inner_env.FindClass("com/roblox/engine/jni/NativeGLInterface");
            bool ok = call_trapping_abort(fn, inner_jni_env, inner_class);
            clear_pending_jni_exception(inner_jni_env, "engine_v2_bridge");
            return ok;
        });
        result.leave_game_trapped_abort = outcome.trapped_abort;
        result.leave_game_still_running = outcome.still_running;
    } else {
        std::fprintf(stderr, "stud: engine_v2_bridge: nativeAppBridgeV2LeaveGame not found, skipping\n");
    }

    if (void* addr =
            lib.find_symbol("Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2DestroyApp");
        addr != nullptr) {
        result.destroy_app_called = true;
        auto* fn = reinterpret_cast<V2VoidNoArgFn>(addr);
        auto outcome = run_bounded_v2_call("nativeAppBridgeV2DestroyApp", [&jvm, fn] {
            static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
            FakeJni::LocalFrame inner_frame(jvm);
            auto& inner_env = inner_frame.getJniEnv();
            auto* inner_jni_env = static_cast<JNIEnv*>(&inner_env);
            jclass inner_class = inner_env.FindClass("com/roblox/engine/jni/NativeGLInterface");
            bool ok = call_trapping_abort(fn, inner_jni_env, inner_class);
            clear_pending_jni_exception(inner_jni_env, "engine_v2_bridge");
            return ok;
        });
        result.destroy_app_trapped_abort = outcome.trapped_abort;
        result.destroy_app_still_running = outcome.still_running;
    } else {
        std::fprintf(stderr, "stud: engine_v2_bridge: nativeAppBridgeV2DestroyApp not found, skipping\n");
    }

    std::fprintf(stderr,
                 "stud: v2: teardown complete: leave_game=%d/abort=%d/still_running=%d "
                 "destroy_app=%d/abort=%d/still_running=%d\n",
                 result.leave_game_called, result.leave_game_trapped_abort,
                 result.leave_game_still_running, result.destroy_app_called,
                 result.destroy_app_trapped_abort, result.destroy_app_still_running);
    std::fflush(stderr);
    return result;
}

bool notify_surface_resized(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                            const std::shared_ptr<PlatformParams>& platform_params,
                            const std::shared_ptr<SurfaceStub>& surface) {
    if (platform_params == nullptr || surface == nullptr) return false;
    auto called_app = std::make_shared<std::atomic<bool>>(false);
    auto called_game = std::make_shared<std::atomic<bool>>(false);
    auto outcome = run_bounded_v2_call(
        "nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams (resize)",
        [&jvm, &lib, platform_params, surface, called_app, called_game] {
            // Only the App surface: a resize is not an experience transition,
            // and the Game variant is correctly skipped off the game path for
            // the same reason run_engine_v2_sequence() skips it.
            return call_update_surface_pair(jvm, lib, platform_params, surface, called_app,
                                            called_game, /*update_game_surface=*/false);
        });
    return !outcome.still_running && !outcome.trapped_abort;
}

bool join_experience_from_deep_link(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                    const std::string& payload,
                                    const std::shared_ptr<PlatformParams>& platform_params,
                                    const std::shared_ptr<DeviceParams>& device_params,
                                    const std::shared_ptr<SurfaceStub>& surface) {
    // key=value per line, produced by Process A from its own parsed link.
    NativeHelperStub::LaunchRequest request{};
    size_t at = 0;
    while (at < payload.size()) {
        size_t end = payload.find('\n', at);
        if (end == std::string::npos) end = payload.size();
        const std::string line = payload.substr(at, end - at);
        at = end + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "placeId") {
            request.place_id = std::strtoll(value.c_str(), nullptr, 10);
        } else if (key == "referredBy") {
            request.referred_by_player_id = std::strtoll(value.c_str(), nullptr, 10);
        } else if (key == "joinAttemptId") {
            request.join_attempt_id = value;
        } else if (key == "joinOrigin") {
            request.join_attempt_origin = value;
        } else if (key == "gameInfo") {
            request.launch_data = value;
        } else if (key == "gameInstanceId") {
            request.game_instance_id = value;
        }
    }
    if (request.place_id == 0) {
        std::fprintf(stderr, "stud: the handed-over link named no place id -- ignoring it\n");
        std::fflush(stderr);
        return false;
    }
    // Everything past here is what a Lua-initiated launch already does:
    // the request becomes the one acknowledge_experience_start reads, and
    // that runs the real nativeAppBridgeV2StartGameWithParam. Reusing it
    // rather than repeating it means one join path, not two that can
    // drift.
    std::fprintf(stderr, "stud: joining placeId=%lld from a second launch\n", request.place_id);
    std::fflush(stderr);
    NativeHelperStub::set_last_launch_request(std::move(request));
    acknowledge_experience_start(jvm, lib, platform_params, device_params, surface);
    return true;
}

void acknowledge_experience_start(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                  const std::shared_ptr<PlatformParams>& platform_params,
                                  const std::shared_ptr<DeviceParams>& device_params,
                                  const std::shared_ptr<SurfaceStub>& surface) {
    void* addr = lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2StartGameWithParam");
    if (addr == nullptr || platform_params == nullptr || surface == nullptr) return;
    // One at a time: the engine can re-signal, and two concurrent start-game
    // calls would hand it two conflicting launches.
    static std::atomic<bool> in_flight{false};
    bool expected = false;
    if (!in_flight.compare_exchange_strong(expected, true)) return;

    auto* fn = reinterpret_cast<V2StartGameFn>(addr);
    // Detached, because this is called from the engine's own callback
    // thread and must not block it -- the bounded helper waits internally.
    //
    // Running it INLINE on the callback thread was tried, on the theory
    // that `[FLog::ExperienceContextHolder] Unsafe call with no engine
    // context.` (logged at exactly this moment) meant the caller's thread
    // identity mattered. Live-tested and reverted: inline, the call never
    // returns at all and the whole app hangs the instant Play is pressed.
    // Detached it returns in ~900ms. So that log line is not about this
    // call's thread, and the join is blocked by something else.
    std::thread([&jvm, &lib, fn, platform_params, device_params, surface]() {
        auto start_game_result = std::make_shared<jint>(0);
        auto outcome = run_bounded_v2_call(
            "nativeAppBridgeV2StartGameWithParam (experience start)",
            [&jvm, fn, platform_params, device_params, surface, start_game_result] {
                static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
                FakeJni::LocalFrame frame(jvm);
                auto& env = frame.getJniEnv();
                auto* jni_env = static_cast<JNIEnv*>(&env);
                jclass cls = env.FindClass("com/roblox/engine/jni/NativeGLInterface");
                // Which experience: the engine names it itself, through
                // gameActivity_onGameLoaded(placeId), and that is what is
                // replayed here. Sending 0 -- which is all Stud had before,
                // with no deep link to read -- makes the server answer "not
                // authorized to join this experience", so an acknowledgement
                // without a real id is worse than none.
                // The whole request the Lua app published, not just the
                // place id -- the real client passes all of it.
                const auto request = NativeHelperStub::last_launch_request();
                DeepLinkJoinInfo join{};
                join.place_id = request.place_id;
                join.join_attempt_id = request.join_attempt_id;
                join.join_attempt_origin = request.join_attempt_origin;
                join.referred_by_player_id = request.referred_by_player_id;
                join.launch_data = request.launch_data;
                join.game_join_context = request.game_join_context;
                join.event_id = request.event_id;
                join.access_code = request.access_code;
                join.game_instance_id = request.game_instance_id;
                if (join.place_id == 0) {
                    std::fprintf(stderr,
                                 "stud: experience start NOT acknowledged: the engine has not "
                                 "named a place id yet (acknowledging with 0 is rejected as "
                                 "\"not authorized to join this experience\")\n");
                    std::fflush(stderr);
                    return false;
                }
                std::fprintf(stderr, "stud: acknowledging experience start for placeId=%lld\n",
                             join.place_id);
                std::fflush(stderr);
                auto params = build_desktop_start_game_params(platform_params, device_params,
                                                              surface, join);
                jobject params_ref = env.createLocalReference(std::move(params));
                bool ok = call_trapping_abort_with_result(fn, *start_game_result, jni_env, cls,
                                                          params_ref);
                clear_pending_jni_exception(jni_env, "nativeAppBridgeV2StartGameWithParam");
                return ok;
            });
        std::fprintf(stderr,
                     "stud: experience start acknowledged: StartGameWithParam trapped=%d "
                     "still_running=%d result=%d\n",
                     outcome.trapped_abort, outcome.still_running, *start_game_result);
        std::fflush(stderr);

        // No UpdateSurface call belongs here. An earlier version of this
        // sent the App/Game pair again right after the launch, on the
        // theory that the game DataModel needed rebinding to the
        // renderer. The real successful-join Sober capture does nothing
        // of the kind: between `nativeAppBridgeV2StartGameWithParam` and
        // `! Joining game ...` there is not a single UpdateSurface call.
        // The game surface is registered once, up front, on the
        // bare-launch path (see run_engine_v2_sequence).

        // What a real client does immediately after the launch call: its
        // ExperienceSession brings the game fragment up
        // (ExperienceSession: NativeReportingInterface.gameForegrounded()
        // then either nativeOnExperienceSessionResume() or
        // nativeOnFragmentStart()). Stud called NONE of these -- the engine
        // had been handed a launch and then never told the experience was
        // actually on screen, which is where a bare-launch join stops.
        //
        // Both start entry points are sent, in the real order: a first
        // launch is the fragment-start case, and resume is the returning
        // case, and the engine's own handlers are the right place for that
        // distinction rather than a guess made here.
        for (const char* name :
             {"Java_com_roblox_engine_jni_NativeReportingInterface_gameForegrounded",
              "Java_com_roblox_engine_jni_NativeGLInterface_nativeOnFragmentStart",
              "Java_com_roblox_engine_jni_NativeGLInterface_nativeOnExperienceSessionResume"}) {
            void* session_addr = lib.find_symbol(name);
            if (session_addr == nullptr) {
                std::fprintf(stderr, "stud: experience session: %s not found\n", name);
                continue;
            }
            auto* session_fn = reinterpret_cast<V2VoidNoArgFn>(session_addr);
            run_bounded_v2_call(name, [&jvm, session_fn] {
                static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
                FakeJni::LocalFrame session_frame(jvm);
                auto& session_env = session_frame.getJniEnv();
                auto* session_jni = static_cast<JNIEnv*>(&session_env);
                jclass session_cls =
                    session_env.FindClass("com/roblox/engine/jni/NativeGLInterface");
                bool session_ok = call_trapping_abort(session_fn, session_jni, session_cls);
                clear_pending_jni_exception(session_jni, "experience session start");
                return session_ok;
            });
        }

        in_flight.store(false);
    }).detach();
}

}  // namespace stud::jni_bridge

namespace stud::jni_bridge {

bool subscribe_to_experience_launch(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib) {
    using GetLaunchIdFn = jstring (*)(JNIEnv*, jclass);
    using DoSubscribeRawFn = jobject (*)(JNIEnv*, jobject, jstring, jobject, jboolean);

    auto* get_launch_id = reinterpret_cast<GetLaunchIdFn>(lib.find_symbol(
        "Java_com_roblox_universalapp_experience_JNIExperienceProtocol_getLaunchId"));
    auto* do_subscribe = reinterpret_cast<DoSubscribeRawFn>(
        lib.find_symbol("Java_com_roblox_universalapp_messagebus_MessageBus_doSubscribeRaw"));
    if (get_launch_id == nullptr || do_subscribe == nullptr) {
        std::fprintf(stderr,
                     "stud: experience-launch subscription unavailable (getLaunchId=%s "
                     "doSubscribeRaw=%s)\n",
                     get_launch_id != nullptr ? "found" : "MISSING",
                     do_subscribe != nullptr ? "found" : "MISSING");
        return false;
    }

    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    // The topic is whatever the engine says it is -- no literal to guess,
    // and nothing version-specific baked in.
    jstring topic = nullptr;
    jclass protocol_class = env.FindClass("com/roblox/universalapp/experience/JNIExperienceProtocol");
    if (!call_trapping_abort_with_result(get_launch_id, topic, jni_env, protocol_class)) {
        std::fprintf(stderr, "stud: JNIExperienceProtocol.getLaunchId() trapped\n");
        return false;
    }
    if (topic == nullptr) {
        std::fprintf(stderr, "stud: JNIExperienceProtocol.getLaunchId() returned null\n");
        return false;
    }

    // The engine keeps this subscription for the rest of the process, so
    // the objects behind it have to live that long too.
    //
    // They used to be plain locals: a shared_ptr destroyed when this
    // function returned, wrapped in a reference belonging to the
    // LocalFrame above, which is destroyed at the same moment. The engine
    // was then holding a subscription whose callback had been freed --
    // and the symptom was exactly what that predicts, an experience
    // launch that worked on one run, did nothing on the next, and
    // occasionally faulted. (webview_bridge.cpp subscribes the same way
    // and already owns its callbacks for this reason.)
    //
    // Owned here, deliberately, because process lifetime IS the correct
    // lifetime for them: nothing ever unsubscribes.
    struct ExperienceLaunchSubscription {
        std::shared_ptr<MessageBusStub> bus;
        std::shared_ptr<MessageBusRawCallbackStub> callback;
    };
    static ExperienceLaunchSubscription subscription;
    subscription.bus = std::make_shared<MessageBusStub>();
    subscription.callback = std::make_shared<MessageBusRawCallbackStub>();
    jobject bus_ref = env.createLocalReference(subscription.bus);
    jobject callback_ref = env.createLocalReference(subscription.callback);

    jobject connection = nullptr;
    const bool ok = call_trapping_abort_with_result(do_subscribe, connection, jni_env, bus_ref,
                                                    topic, callback_ref,
                                                    static_cast<jboolean>(JNI_FALSE));
    clear_pending_jni_exception(jni_env, "MessageBus.doSubscribeRaw");
    if (!ok) {
        std::fprintf(stderr, "stud: MessageBus.doSubscribeRaw trapped\n");
        return false;
    }
    std::printf("stud: subscribed to the experience-launch request (connection=%s)\n",
                connection != nullptr ? "live" : "null");
    std::fflush(stdout);
    return true;
}

}  // namespace stud::jni_bridge
