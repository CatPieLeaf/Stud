#include "stud/game_engine_boot.h"

#include "stud/engine_thread.h"
#include "stud/game_activity_stubs.h"
#include "stud/trap_recovery.h"

#include <android/looper.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include <unistd.h>

namespace stud::jni_bridge {

std::function<void(const stud::linker::LoadedLibrary&)> make_post_constructor_hook(
    BionicAwareJvm& jvm, const std::string& so_path, JniOnLoadResult& out_result) {
    // so_path is unused now that unwind-info registration (a Stud-owned
    // glibc-side unwinder's concern, moot now that Process B's own
    // unwinder is real bionic code, exactly like a real device) is gone
    // kept as a parameter for call-site compatibility.
    (void)so_path;
    return [&jvm, &out_result](const stud::linker::LoadedLibrary& lib) {
        using JniOnLoadFn = jint (*)(JavaVM*, void*);
        if (void* on_load_addr = lib.find_symbol("JNI_OnLoad")) {
            out_result.found = true;
            auto* on_load = reinterpret_cast<JniOnLoadFn>(on_load_addr);
            auto* vm = jvm.GetBionicSafeJavaVM();
            out_result.trapped_abort =
                !call_trapping_abort([&] { out_result.result = on_load(vm, nullptr); });
            FakeJni::LocalFrame frame(jvm);
            clear_pending_jni_exception(static_cast<JNIEnv*>(&frame.getJniEnv()), "JNI_OnLoad");
        }
    };
}

GameActivityLifecycleResult drive_game_activity_lifecycle(
    BionicAwareJvm& jvm, const stud::linker::LoadedLibrary& lib,
    const std::string& internal_data_dir, const std::string& obb_dir,
    const std::string& external_data_dir, int surface_width, int surface_height,
    std::function<void()> on_bootstrap_the_app) {
    GameActivityLifecycleResult out;

    // Real signature confirmed from AGDK's own public source
    // (android.googlesource.com/platform/frameworks/opt/gamesdk,
    // game-activity/.../GameActivity.java): (String internalDataDir,
    // String obbDir, String externalDataDir, AssetManager, byte[]
    // savedState, Configuration).
    using InitNativeCodeFn = jlong (*)(JNIEnv*, jobject, jstring, jstring, jstring, jobject,
                                        jbyteArray, jobject);
    void* addr = lib.find_symbol("Java_com_google_androidgamesdk_GameActivity_initializeNativeCode");
    if (addr == nullptr) {
        std::fprintf(stderr, "stud: GameActivity_initializeNativeCode is not exported by this "
                             "libroblox.so, so there is no AGDK lifecycle\n");
        return out;
    }
    out.initialize_native_code_found = true;

    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    jstring internal_data_dir_jstr = env.NewStringUTF(internal_data_dir.c_str());
    jstring obb_dir_jstr = env.NewStringUTF(obb_dir.c_str());
    jstring external_data_dir_jstr = env.NewStringUTF(external_data_dir.c_str());
    // Real MainGameActivity subclass, not bare GameActivityStub (see
    // game_activity_stubs.h's doc comment), the engine's own real
    // bootstrapTheApp() callback needs a real, dispatchable method on
    // whatever jobject it receives as `thiz` here.
    auto main_game_activity = std::make_shared<MainGameActivityStub>();
    main_game_activity->on_bootstrap_the_app = std::move(on_bootstrap_the_app);
    jobject game_activity_instance = env.createLocalReference(main_game_activity);
    jobject configuration_instance = env.createLocalReference(std::make_shared<ConfigurationStub>());

    ALooper* looper_on_this_thread = ALooper_forThread();
    if (looper_on_this_thread == nullptr) {
        // AGDK's own initializeNativeCode returns null on this, silently.
        std::fprintf(stderr, "stud: no ALooper on the thread calling initializeNativeCode\n");
    }
    int probe_pipe_fds[2];
    int probe_pipe_result = ::pipe(probe_pipe_fds);
    if (probe_pipe_result != 0) {
        // The other thing AGDK's own initializeNativeCode returns null on.
        std::fprintf(stderr, "stud: pipe() failed before initializeNativeCode\n");
    }
    if (probe_pipe_result == 0) {
        ::close(probe_pipe_fds[0]);
        ::close(probe_pipe_fds[1]);
    }

    auto* init_native_code = reinterpret_cast<InitNativeCodeFn>(addr);
    out.initialize_native_code_trapped_abort = !call_trapping_abort(
        [&] {
            out.game_activity_ptr = init_native_code(jni_env, game_activity_instance,
                                                       internal_data_dir_jstr, obb_dir_jstr,
                                                       external_data_dir_jstr, nullptr, nullptr,
                                                       configuration_instance);
        });
    clear_pending_jni_exception(jni_env, "GameActivity_initializeNativeCode");
    if (out.initialize_native_code_trapped_abort) {
        return out;
    }
    // Gap found in testing (the engineering notes): the real AGDK
    // initializeNativeCode_native() returns a real, valid NativeCode*
    // on success but returns a plain 0 (no crash, no trap) if
    // ALooper_forThread() is null on the calling thread, a real,
    // silent early-return this code never checked for, meaning every
    // "ok" lifecycle call below could have been dispatched with a null
    // `J` (game_activity_ptr) argument the whole time without Stud ever
    // knowing. This is a diagnostic only, not (yet) a behavior change,
    // never observed to actually be null in testing so far, but no
    // prior test explicitly checked, either.
    if (out.game_activity_ptr == 0) {
        std::fprintf(stderr, "stud: GameActivity_initializeNativeCode returned NULL, so every "
                             "lifecycle call below is a no-op (see this file's doc comment)\n");
    }

    // Permanent: Stud plays the "Java side" role directly, calling
    // AGDK's own real, RegisterNatives()-installed lifecycle methods via
    // ordinary JNI reflection; see game_engine_boot.h's doc comment for
    // the two real jnivm bugs (patched permanently in
    // jni-bridge/patches/patch_libjnivm.cmake) this depends on.
    jclass game_activity_class = env.GetObjectClass(game_activity_instance);

    // Gap found in testing (the engineering notes, "GameActivity_
    // initializeNativeCode returns NULL" fix, immediately-following new
    // symptom): with a real, valid NativeCode* now flowing through (the
    // fix above), these calls invoke libroblox.so's own real
    // onNativeWindowCreated/etc. engine callbacks for the first time in
    // this project's history, previously-unexercised code whose real
    // blocking behavior was unknown. A real, full launch hung here
    // (confirmed live: process alive, near-zero CPU, log stops growing,
    // never reaches run_engine_v2_sequence()) the first time this fix
    // was tested. Same real fix pattern as every other real V2 entry
    // point already known to be able to block forever
    // (engine_v2_bridge.cpp's own run_bounded_v2_call(), "Calls that
    // might block forever" in this file's own Methodology), run each
    // call on its own detached background thread with its own fresh
    // JNI frame (a jmethodID/jclass/jobject local reference is only
    // valid on the thread/frame that resolved it, so nothing JNI-
    // related may cross threads directly, re-resolved fresh inside
    // the lambda, same as engine_v2_bridge.cpp's own established
    // pattern), bounded wait, honest "still running" report instead of
    // blocking the rest of boot forever.
    auto call_lifecycle_method_bounded = [&](const char* name, const char* sig, bool* called_flag,
                                              bool* trapped_flag,
                                              std::function<void(FakeJni::Env&, jmethodID, jobject)>
                                                  invoke_with_fresh_frame) {
        jmethodID method_probe = env.GetMethodID(game_activity_class, name, sig);
        if (method_probe == nullptr) {
            return;
        }
        *called_flag = true;

        auto completed = std::make_shared<std::atomic<bool>>(false);
        auto trapped = std::make_shared<std::atomic<bool>>(false);
        // Fresh detached thread per call, deliberately; see the note
        // in engine_v2_bridge.cpp's run_bounded_v2_call() for why a
        // single shared engine thread was tried here and reverted.
        std::thread([&jvm, main_game_activity, name, sig, invoke_with_fresh_frame, completed,
                     trapped]() mutable {
            static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
            FakeJni::LocalFrame inner_frame(jvm);
            auto& inner_env = inner_frame.getJniEnv();
            auto* inner_jni_env = static_cast<JNIEnv*>(&inner_env);
            jobject inner_activity_ref = inner_env.createLocalReference(main_game_activity);
            jclass inner_class = inner_env.GetObjectClass(inner_activity_ref);
            jmethodID inner_method = inner_env.GetMethodID(inner_class, name, sig);
            bool ok = call_trapping_abort(
                [&] { invoke_with_fresh_frame(inner_env, inner_method, inner_activity_ref); });
            clear_pending_jni_exception(inner_jni_env, name);
            trapped->store(!ok, std::memory_order_relaxed);
            completed->store(true, std::memory_order_relaxed);
        }).detach();

        constexpr int kPollIntervalMs = 50;
        constexpr int kMaxPolls = 60;  // 3s total, simple callbacks, not full engine bring-up
        int polls = 0;
        while (!completed->load(std::memory_order_relaxed) && polls < kMaxPolls) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
            ++polls;
        }
        if (completed->load(std::memory_order_relaxed)) {
            *trapped_flag = trapped->load(std::memory_order_relaxed);
        } else {
            std::fprintf(stderr, "stud: %s still running/blocked after %dms, not waiting further\n",
                         name, kPollIntervalMs * kMaxPolls);
        }
    };

    jlong game_activity_ptr = out.game_activity_ptr;
    call_lifecycle_method_bounded(
        "onStartNative", "(J)V", &out.on_start_called, &out.on_start_trapped_abort,
        [game_activity_ptr](FakeJni::Env& e, jmethodID m, jobject activity) {
            e.CallVoidMethod(activity, m, game_activity_ptr);
        });
    call_lifecycle_method_bounded(
        "onResumeNative", "(J)V", &out.on_resume_called, &out.on_resume_trapped_abort,
        [game_activity_ptr](FakeJni::Env& e, jmethodID m, jobject activity) {
            e.CallVoidMethod(activity, m, game_activity_ptr);
        });
    out.activity = main_game_activity;
    out.surface = std::make_shared<SurfaceStub>();
    auto surface_stub = out.surface;
    call_lifecycle_method_bounded(
        "onSurfaceCreatedNative", "(JLandroid/view/Surface;)V", &out.on_surface_created_called,
        &out.on_surface_created_trapped_abort,
        [game_activity_ptr, surface_stub](FakeJni::Env& e, jmethodID m, jobject activity) {
            jobject surface_ref = e.createLocalReference(surface_stub);
            e.CallVoidMethod(activity, m, game_activity_ptr, surface_ref);
        });
    // Real Android always follows onSurfaceCreated with
    // onSurfaceChanged(width, height, format) before an app's engine
    // actually starts using the surface. format=1 is
    // android.graphics.PixelFormat.RGBA_8888, the real, documented
    // constant value.
    call_lifecycle_method_bounded(
        "onSurfaceChangedNative", "(JLandroid/view/Surface;III)V", &out.on_surface_changed_called,
        &out.on_surface_changed_trapped_abort,
        [game_activity_ptr, surface_stub, surface_width, surface_height](FakeJni::Env& e,
                                                                          jmethodID m,
                                                                          jobject activity) {
            jobject surface_ref = e.createLocalReference(surface_stub);
            e.CallVoidMethod(activity, m, game_activity_ptr, surface_ref,
                              static_cast<jint>(surface_width), static_cast<jint>(surface_height),
                              static_cast<jint>(1));
        });

    // Real Android delivers window focus AFTER the surface exists, the
    // window is not focusable until it has been added and its surface
    // created. This used to run before onSurfaceCreated, which meant the
    // engine evaluated focus against a window it did not yet have, and
    // real keyboard input and the engine's own in-frame mouse cursor
    // (both gated on a focused window) never activated.
    call_lifecycle_method_bounded(
        "onWindowFocusChangedNative", "(JZ)V", &out.on_window_focus_changed_called,
        &out.on_window_focus_changed_trapped_abort,
        [game_activity_ptr](FakeJni::Env& e, jmethodID m, jobject activity) {
            e.CallVoidMethod(activity, m, game_activity_ptr, static_cast<jboolean>(JNI_TRUE));
        });

    return out;
}

bool dispatch_surface_changed(FakeJni::Jvm& jvm, const GameActivityLifecycleResult& lifecycle,
                              int32_t width, int32_t height) {
    if (lifecycle.activity == nullptr || lifecycle.surface == nullptr) return false;
    // Deliberately inline on the caller's thread, unlike the boot-time
    // lifecycle calls. Those hand the work to a fresh detached thread, and a
    // first attempt here did the same, which failed: GetObjectClass on a
    // reference created inside that thread's own frame resolved to
    // java/lang/Object, so the method lookup missed every time
    // (`STUD_DIAG GetMethodID MISS class=\`java/lang/Object\`
    // method=\`onSurfaceChangedNative\``, once per resize). Resolving and
    // calling on one thread avoids that entirely, and this callback is a plain
    // geometry notification. It has never been observed to block, unlike the
    // engine bring-up calls the bounded-thread pattern exists for.
    FakeJni::LocalFrame frame(jvm);
    auto& e = frame.getJniEnv();
    jobject activity_ref = e.createLocalReference(lifecycle.activity);
    jclass cls = e.GetObjectClass(activity_ref);
    if (cls == nullptr) return false;
    jmethodID m = e.GetMethodID(cls, "onSurfaceChangedNative", "(JLandroid/view/Surface;III)V");
    if (m == nullptr) return false;
    jobject surface_ref = e.createLocalReference(lifecycle.surface);
    const jlong ptr = lifecycle.game_activity_ptr;
    // format 1 == android.graphics.PixelFormat.RGBA_8888.
    bool ok = call_trapping_abort([&] {
        e.CallVoidMethod(activity_ref, m, ptr, surface_ref, static_cast<jint>(width),
                         static_cast<jint>(height), static_cast<jint>(1));
    });
    clear_pending_jni_exception(static_cast<JNIEnv*>(&e), "onSurfaceChangedNative");
    std::printf("stud: onSurfaceChangedNative(%dx%d) -> %s\n", width, height, ok ? "ok" : "trapped");
    std::fflush(stdout);
    return ok;
}

}  // namespace stud::jni_bridge
