#pragma once

#include <functional>

// Real, live-root-caused fix (the engineering notes, "what drains the task
// queue"): Roblox's own engine designates ONE thread as its real "main"
// thread, the first thread to call into the relevant engine entry
// points stores its own `pthread_t` in a real global
// (an
// investigation-only address, never used at runtime by Stud).
//
// Every subsequent engine call compares `pthread_self()` against that
// stored value (confirmed live against the real dispatcher):
//   - SAME thread  -> the task runs INLINE, immediately, via a plain
//                     virtual call. No queue, no waiting.
//   - OTHER thread -> the task is posted to that designated thread's
//                     own work queue, and the caller blocks waiting for
//                     the designated thread to drain it.
//
// Stud used to run each of these calls on its own freshly-spawned,
// detached `std::thread` (one per call). That meant the designated
// thread was whichever short-lived helper thread happened to call in
// first, and once that thread exited, every later call posted work to
// a DEAD thread's queue and waited forever for a drain that could never
// happen. Confirmed live, not theorised: the registered `pthread_t`
// pointed into an `[anon:stack_and_tls:14]` mapping that no live thread
// owned any more, while every blocked call sat in an infinite
// `FUTEX_WAIT`.
//
// This is exactly the invariant a real Android device satisfies for
// free: all of these calls come from the single, long-lived Java
// main/UI thread. Stud replicates that honestly here, one persistent,
// process-lifetime engine thread that every engine call is funnelled
// through, so the designated thread is always alive and every call
// after the first runs inline.
namespace stud::jni_bridge {

struct EngineThreadOutcome {
    // False if the work never finished within the timeout. The engine
    // thread keeps running it; the caller just stops waiting (same
    // honest, non-blocking-Stud discipline the old per-call bounded
    // wait had).
    bool completed = false;
    // Only meaningful when `completed` is true, whatever `work`
    // itself returned.
    bool result = false;
};

// Runs `work` on the one persistent engine thread and waits up to
// `timeout_ms` for it to finish. Work is executed in submission order,
// matching a real device's single-main-thread ordering.
EngineThreadOutcome run_on_engine_thread(std::function<bool()> work, int timeout_ms);

}  // namespace stud::jni_bridge
