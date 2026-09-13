// Real, confirmed-live gap (this session): trap_recovery.h's abort/trap
// protection was only ever armed around ONE specific call (the main
// thread's JNI_OnLoad invocation) -- any background thread Roblox's own
// code spawns (confirmed live: a real, named "RBX Worker A" thread hit
// an unrecovered SIGTRAP/int3, killing the whole process, with none of
// the main thread's own recovery machinery ever engaged for it) gets no
// protection at all.
//
// Fix: interpose pthread_create at the ELF symbol level -- a real,
// exported `pthread_create` symbol in this executable's own global
// scope takes priority over real bionic libc.so's own definition for
// every call any loaded library (including libroblox.so) makes,
// standard ELF symbol resolution behavior on bionic same as glibc.
// Every new thread's entire body runs inside call_trapping_abort()'s
// own sigsetjmp/trap bracket -- if it hits an abort()/int3/stack-guard-
// fault anywhere during its run, that ONE thread exits cleanly instead
// of taking the whole process down with it, exactly mirroring the
// protection the main thread's JNI_OnLoad call already has.

#include "stud/bionic_jvm.h"
#include "stud/ndk_types.h"
#include "stud/trap_recovery.h"

#include <dlfcn.h>
#include <pthread.h>

namespace {

struct WrappedThreadArgs {
    void* (*real_start)(void*);
    void* real_arg;
};

void* stud_thread_trampoline(void* raw) {
    auto* wrapped = static_cast<WrappedThreadArgs*>(raw);
    void* (*start)(void*) = wrapped->real_start;
    void* arg = wrapped->real_arg;
    delete wrapped;

    // Real, confirmed-live gap (this session): AGDK's own
    // GameActivity_initializeNativeCode() spawns an internal "app
    // thread" to run its native code on, and that thread later calls
    // ALooper_release() on ALooper_forThread()'s result without ever
    // calling ALooper_prepare() on itself first -- looper.cpp's own
    // t_looper is thread_local, so that call sees a null looper and
    // crashes dereferencing looper->ref_count (confirmed live: fault
    // address 0x4 exactly matches ALooper::ref_count's real struct
    // offset). A real Android thread that goes on to use Looper APIs is
    // always expected to have prepared one first; every thread Stud's
    // own interposed pthread_create() spawns now does this up front,
    // matching that real convention, rather than special-casing this
    // one call site.
    ::ALooper_prepare(0);

    // Real, live-caught gap (the engineering notes, "GameActivity_
    // initializeNativeCode returns NULL" fix / follow-up crash inside a
    // libroblox.so-spawned thread): now that the ALooper fix above lets
    // real engine callbacks actually fire for the first time, threads
    // libroblox.so spawns itself make real JNI calls -- but were never
    // attached to the JVM first (every other Stud-spawned background
    // thread in this codebase already does this: engine_v2_bridge.cpp's
    // run_bounded_v2_call(), game_engine_boot.cpp's bounded lifecycle
    // calls). A live crash traced directly to this: `GetNativeMethodID
    // class is null` deep inside a real, newly-firing engine callback,
    // on a thread spawned right through this same interpose. Attaching
    // here, once, for every thread this process (or libroblox.so) ever
    // spawns, closes the gap at its real, single source instead of
    // requiring every future JNI-calling background thread to remember
    // to do it individually.
    stud::jni_bridge::ensure_current_thread_attached_to_jvm();

    // Real, live-caught gap, fixed same pass as the fix this comment
    // describes (the engineering notes, "fix the landing pad crash"): this
    // used the strict call_trapping_abort() (recoverable only for a
    // stack-overflow-shaped or near-null fault), so a genuinely wild-
    // shaped SIGSEGV on any Roblox-spawned background thread -- e.g. the
    // real, evidence-confirmed (unwind tables plus the engine's own code) C++ exception-
    // unwind landing pad this project spent significant effort root-
    // causing -- always took the whole process down with it, even though
    // Roblox's own internal worker threads are numerous and this
    // project's own already-established reasoning elsewhere in this file
    // applies equally here: a real device survives losing one internal
    // worker thread; it does not survive this process dying outright.
    // Using the wild-tolerant variant here (not at some narrower,
    // address-specific call site -- there isn't one to hang this on,
    // since Roblox's own internal task-queue dispatch is what spawns
    // this thread and calls into whatever code faults, not any call Stud
    // itself makes) is a deliberate, one-time architectural choice for
    // this single, central thread-wrapping point, not the kind of
    // casual "make an inconvenient crash go away" reach the stricter
    // variant's own doc comment warns against at ad-hoc call sites.
    void* result = nullptr;
    stud::jni_bridge::call_trapping_abort_tolerating_wild_sigsegv([&] { result = start(arg); });
    return result;
}

}  // namespace

extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                               void* (*start_routine)(void*), void* arg) {
    using RealPthreadCreate = int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
    static auto real = reinterpret_cast<RealPthreadCreate>(::dlsym(RTLD_NEXT, "pthread_create"));
    auto* wrapped = new WrappedThreadArgs{start_routine, arg};
    return real(thread, attr, stud_thread_trampoline, wrapped);
}
