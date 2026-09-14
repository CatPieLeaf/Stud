#pragma once

#include <fake-jni/fake-jni.h>

// Process B (this file, once ported to the bionic toolchain) is real
// bionic code calling into fake-jni, ALSO compiled as real bionic code,
// in the same process: same ABI, same TLS layout, ordinary same-process
// function calls. The old version of this file wrapped every JNIEnv/
// JavaVM vtable slot in a %fs-swapping trampoline because Roblox's
// bionic-compiled code and jnivm's glibc-compiled implementation used to
// run interleaved on the same OS thread; that precondition no longer
// exists, so all of that wrapping is gone.
//
// Two of the old file's fixes are NOT %fs-related bugs and are kept
// as-is: a genuine C++ Itanium-ABI object-layout bug in multiple
// inheritance (GetBionicSafeJavaVM()), and a genuine behavioral gap
// around Roblox's own JNI hook installation (ensure_env_for_current_
// thread()), both would still be real bugs on a genuine Android
// device running genuine bionic-vs-bionic JNI calls, so both stay.

namespace stud::jni_bridge {

// A FakeJni::Jvm exposing a correctly-laid-out JavaVM* for real
// bionic-compiled callers.
class BionicAwareJvm : public FakeJni::Jvm {
public:
    // Real, confirmed C++ ABI bug (unrelated to %fs/glibc-vs-bionic):
    // `Jvm : public JavaVM, protected jnivm::VM`, jnivm::VM is
    // polymorphic (CreateEnv is virtual), plain `_JavaVM` is not, so
    // under the Itanium C++ ABI jnivm::VM becomes the *primary* base
    // (vtable ptr at offset 0) and the `_JavaVM` base subobject,
    // therefore `this->functions` too, gets pushed to a large,
    // non-zero offset (measured: 2280 bytes) instead of offset 0. Any
    // real caller built against plain C `JavaVM` (`typedef const struct
    // JNIInvokeInterface** JavaVM`) hard-requires `functions` at offset
    // 0, `static_cast<JavaVM*>(&jvm)` silently violates that and hands
    // out a pointer into the middle of a 2344-byte object, live memory
    // unrelated code can and does overwrite later.
    //
    // jnivm::VM already owns a second, completely standalone `JavaVM
    // javaVM` data member (not a base-class subobject, no inheritance
    // involved) with `functions` genuinely at offset 0, `GetJavaVM()`
    // returns its address. Use *this* pointer for any real
    // bionic-compiled caller, never `&jvm`/`static_cast<JavaVM*>(&jvm)`.
    JavaVM* GetBionicSafeJavaVM() { return this->GetJavaVM(); }

    // Real, confirmed behavioral gap (unrelated to %fs): a brand-new
    // thread's FIRST-ever FakeJni::LocalFrame construction falls through
    // to `vm.AttachCurrentThread(nullptr, nullptr)`, dispatched through
    // the JNIInvokeInterface's AttachCurrentThread slot, which real
    // Roblox JNI_OnLoad code may overwrite with its own function (real,
    // legitimate Android JNI hook-install behavior). That function can
    // do real, substantial internal work Stud doesn't want triggered
    // just to attach one of Stud's own internal utility threads.
    // EnsureEnvForCurrentThread() attaches THIS thread the same way
    // jnivm's own default (never-overwritten) AttachCurrentThread lambda
    // would: direct lock+CreateEnv()+store, zero JNI-vtable dispatch,
    // immune to whatever Roblox has put in that slot. Call once, before
    // constructing any LocalFrame on a freshly-spawned Stud-owned
    // thread.
    void ensure_env_for_current_thread() {
        this->jnivm::VM::EnsureEnvForCurrentThread();
    }
};

// Real, live-caught gap (the engineering notes, "GameActivity_
// initializeNativeCode returns NULL" fix / "new crash inside a
// libroblox.so-spawned thread" follow-up): `pthread_create_interpose.
// cpp` wraps *every* thread any code in this process spawns, including
// libroblox.so's own real internal worker threads, now that the
// ALooper fix above lets real engine callbacks actually fire, those
// threads make real JNI calls for the first time, and none of them
// were ever attached to the JVM first (this class's own
// `ensure_env_for_current_thread()`, already required by every other
// Stud-spawned background thread in this codebase, engine_v2_bridge.
// cpp's run_bounded_v2_call(), game_engine_boot.cpp's bounded lifecycle
// calls, was never called for threads libroblox.so spawns itself).
// A real, process-wide singleton pointer, set once right after `jvm` is
// constructed in main(), lets the standalone, global-scope pthread_
// create interpose reach it without needing a direct reference to a
// function-local variable.
inline BionicAwareJvm* g_process_wide_jvm_for_thread_attach = nullptr;

inline void set_process_wide_jvm_for_thread_attach(BionicAwareJvm* jvm) {
    g_process_wide_jvm_for_thread_attach = jvm;
}

// Safe to call from any thread, any number of times; a no-op before
// `set_process_wide_jvm_for_thread_attach()` has run (e.g. Stud's own
// very earliest bring-up threads, none of which make JNI calls).
inline void ensure_current_thread_attached_to_jvm() {
    if (g_process_wide_jvm_for_thread_attach != nullptr) {
        g_process_wide_jvm_for_thread_attach->ensure_env_for_current_thread();
    }
}

}  // namespace stud::jni_bridge
