#pragma once

#include <fake-jni/fake-jni.h>

// Real android.app.ActivityThread-equivalent driver, the real
// replacement for the old, scripted "call one native entry point after
// another, then enter a bespoke render-only loop" shape this project
// moved away from (the engineering notes' "nuke stud, recreate sober 1:1"
// entry). Real Android's own ActivityThread.main() does two things
// this project's old main.cpp never did together: it dispatches real
// lifecycle/Handler work AND real per-frame/input work through the
// SAME real main-thread Looper, not two separate, uncoordinated
// mechanisms. Concretely:
//
//   - The real bring-up sequence (dlopen, register real framework
//     classes, JNI_OnLoad, native settings, asset manager, Activity
//     lifecycle dispatch, app-bridge start, engine V2 sequence) is
//     kept as-is. That ordering is real, evidence-grounded (matches
//     real device logcat captures), checked, and rewriting it from
//     scratch would just rediscover the same real constraints through
//     the same trial and error already paid for once.
//
//   - What changes: after bring-up, runtime/src/main.cpp's own real
//     render/input loop calls `LooperStub::getMainLooper()->
//     drain_pending(jvm)` once per iteration (see android_framework_
//     stubs.h), making the real process main thread the SAME thread
//     that services any real `Handler.post()`/`runOnUiThread()` call
//     Roblox's own native code makes, exactly matching real Android's
//     actual main-thread identity contract, instead of leaving posted
//     work to a separate, uncoordinated background thread. This is
//     what "real, continuous, message-driven runtime instead of a
//     scripted one-shot function" concretely means in this codebase:
//     not a new heavyweight class, a real, minimal, testable change to
//     an already-real render loop.
//
// No separate ActivityThread *class* exists here on purpose: real
// Android's own ActivityThread is mostly bookkeeping (the current
// Activity stack, the Application instance, ...) Stud doesn't need a
// parallel model of, since GameActivityStub/MainGameActivityStub
// already hold the real, equivalent state directly. This header exists
// to name and document the real design decision, not to wrap it in
// unnecessary structure.
