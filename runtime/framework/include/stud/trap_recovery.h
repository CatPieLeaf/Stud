#pragma once

#include <csetjmp>
#include <cstdint>
#include <jni.h>

// Real, independent-of-ABI signal-based recovery: makes one direct call
// into Roblox's own bionic-compiled code, catching two things instead of
// letting them kill the whole process:
//
//   - SIGABRT/SIGTRAP: Roblox's own native bootstrap code calling
//     abort() (or hitting a debug trap) on an unmet precondition during
//     Stud's still-imperfect bring-up (a missing flag, unexpected state,
//     ...) -- lets a caller observe "did this call complete or trigger
//     an abort" as a diagnostic signal instead of losing the whole
//     process to one bootstrap call's internal assertion.
//   - SIGSEGV, narrowly: only a fault landing within ~1MB of the current
//     stack pointer (a real, previously-characterized stack-overflow
//     guard-page hit inside Roblox's own code, not a null/wild-pointer
//     bug -- that shape falls straight through to the normal, fatal
//     default disposition, unaffected by this).
//
// This is orthogonal to the old glibc/bionic ABI-crossing problem this
// project used to have (deleted along with tls-compat) -- it's plain
// POSIX sigsetjmp/siglongjmp plus a per-thread alt signal stack, and
// applies identically to Process B's real, single-ABI bionic execution.
namespace stud::jni_bridge {

// Arms a checkpoint on the calling thread for the sigsetjmp() the caller
// is about to perform. See call_trapping_abort()'s implementation for
// the required call order (sigsetjmp() first, then arm).
void arm_abort_trap(sigjmp_buf& checkpoint);

// Same as arm_abort_trap(), but also disables the ~1MB-of-RSP
// recoverability heuristic for this one armed call: ANY SIGSEGV during
// it is treated as recoverable, not just a stack-overflow-shaped one.
// Only for call sites that are genuinely best-effort probes of
// not-yet-fully-wired Roblox internals on their own background thread
// (see call_trapping_abort_tolerating_wild_sigsegv()'s doc comment) --
// using this elsewhere would hide real, unrelated null/wild-pointer
// bugs that should stay fatal so they get root-caused.
void arm_abort_trap_tolerating_wild_sigsegv(sigjmp_buf& checkpoint);

// Disarms whatever checkpoint is currently armed on this thread (a
// no-op if none is). Always safe to call.
void disarm_abort_trap();

// Makes one real call into Roblox's own code, trapping any int3/abort()
// it hits mid-call instead of terminating the process. Deliberately
// narrow: only the raw function-pointer call itself runs under the trap
// bracket, never the caller's own C++ argument setup.
template <typename Fn, typename... Args>
bool call_trapping_abort(Fn fn, Args... args) {
    sigjmp_buf checkpoint;
    bool completed = true;
    // arm_abort_trap() must run AFTER sigsetjmp() has actually
    // initialized `checkpoint`, not before -- arming first leaves a real
    // window where a signal firing between the arm and the sigsetjmp
    // call itself would siglongjmp into an uninitialized buffer.
    if (sigsetjmp(checkpoint, 1) == 0) {
        arm_abort_trap(checkpoint);
        fn(args...);
    } else {
        completed = false;
    }
    disarm_abort_trap();
    return completed;
}

// Same as above, for a function whose real return value the caller
// needs. `result` is only meaningfully written if this returns true.
template <typename Result, typename Fn, typename... Args>
bool call_trapping_abort_with_result(Fn fn, Result& result, Args... args) {
    sigjmp_buf checkpoint;
    bool completed = true;
    if (sigsetjmp(checkpoint, 1) == 0) {
        arm_abort_trap(checkpoint);
        result = fn(args...);
    } else {
        completed = false;
    }
    disarm_abort_trap();
    return completed;
}

// Same as call_trapping_abort(), but for call sites where a genuinely
// wild/null-pointer SIGSEGV (not just a stack-overflow-shaped one) is
// expected to be survivable and shouldn't take the whole process down --
// e.g. a best-effort background probe of a Roblox code path Stud's
// bring-up doesn't fully support yet, running on its own detached
// thread where losing that one call has no effect on the rest of the
// process. Confirmed, real crash this exists for: nativeAppBridgeV2
// StartAppWithParams's internal telemetry-logging helper dereferences a
// null object a few calls deep (root-caused live,
// not a guess) -- likely a
// Roblox-internal singleton real Android populates before this path
// runs that Stud's bring-up doesn't yet, out of scope to chase further
// right now since this call is already documented as optional/
// best-effort. Do NOT reach for this at other call sites just because a
// crash is inconvenient -- see arm_abort_trap_tolerating_wild_sigsegv()'s
// own doc comment.
template <typename Fn, typename... Args>
bool call_trapping_abort_tolerating_wild_sigsegv(Fn fn, Args... args) {
    sigjmp_buf checkpoint;
    bool completed = true;
    if (sigsetjmp(checkpoint, 1) == 0) {
        arm_abort_trap_tolerating_wild_sigsegv(checkpoint);
        fn(args...);
    } else {
        completed = false;
    }
    disarm_abort_trap();
    return completed;
}


// Real JNI hygiene, previously entirely absent from this codebase
// (confirmed: zero calls anywhere to ExceptionCheck/ExceptionClear/
// ExceptionOccurred before this). Any pending exception left on `env`
// after one bootstrap call is visible to the NEXT, unrelated JNI call's
// own internal ExceptionCheck() -- real, standard JNI/Android code
// commonly guards its own FindClass/GetMethodID calls this way. Live-
// observed cascading exactly this way in this project's own capture: an
// "Invalid Reference, Unexpected Type" exception thrown deep inside a
// classloader-bootstrap lookup left the JNIEnv's exception state set,
// and the real WebRTC helpers_android.cc code's own
// `Check failed: !jni->ExceptionCheck()` fired on an entirely
// unrelated, later `FindClass("org/webrtc/voiceengine/BuildInfo")`
// call, taking the process down via a real, deliberate `abort()` --
// not a bug in the WebRTC lookup itself, just Stud never having
// cleared the earlier exception the way a real, disciplined JNI caller
// would. Call this after any bootstrap phase that might throw, before
// moving on to an unrelated one. Returns true iff a pending exception
// was found and cleared.
bool clear_pending_jni_exception(JNIEnv* env, const char* context);

}  // namespace stud::jni_bridge

extern "C" {

// A SIGSEGV that is an ordinary, expected event rather than a crash.
//
// The render client write-protects its mapped-memory staging pages so
// the MMU reports which ones the engine wrote (see
// render-client/src/mapped_write_barrier.h). Those faults must be
// handled BEFORE any crash classification, or a perfectly normal write
// is read as a wild-shaped fault and the recovery kills the render
// thread -- live-caught exactly that way, as a window that never
// appeared.
//
// Chaining sigaction from the client is not sufficient: this file
// installs its own SIGSEGV handler later in bring-up and would replace
// the chain. So the client registers here instead, resolving this
// symbol with dlsym(RTLD_DEFAULT, ...) because libvulkan.so is a
// separate object that does not link this one. The handler must be
// async-signal-safe and return true only for addresses it owns.
using StudWriteFaultHandler = bool (*)(void* addr);
void stud_set_write_fault_handler(StudWriteFaultHandler handler);

}  // extern "C"
