# Patches libjnivm's vendored jni.h so `_JavaVM`'s 5 dispatch methods
# (DestroyJavaVM/AttachCurrentThread/DetachCurrentThread/GetEnv/
# AttachCurrentThreadAsDaemon) route through Stud's bionic/glibc module
# classification before calling through `functions`, instead of calling
# it directly and unconditionally.
#
# Real, confirmed bug this fixes (the engineering notes, "Root cause found:
# the conditional JNI-vtable trampoline's core assumption breaks..." and
# its "Plan" follow-up): libroblox.so's own compiled code can overwrite
# `javaVM.functions`, the whole JNIInvokeInterface table pointer,
# with its own function pointers, completely bypassing every earlier
# wrapping layer (which only ever protected the ORIGINAL table, and only
# decided whether to swap %fs based on the CALLER's context, an
# assumption that breaks the moment the real target is Roblox's own
# bionic code reached from Stud's own glibc-side call). This patch fixes
# it at the one place that's still guaranteed to run on every dispatch
# regardless of which table `functions` currently points to: the call
# site itself.
#
# String-replace based, NOT a unified diff/line-number patch,
# deliberately, since libjnivm is fetched against a moving branch
# (GIT_TAG main, not a pinned commit, see jni-bridge/CMakeLists.txt) and
# a line-number-anchored diff would silently stop applying (or apply
# wrong) the moment anything else in the file shifts. Matching against
# the distinctive method bodies survives unrelated upstream changes.
# It only fails loudly (see the checks below) if
# libjnivm changes THESE specific 5 methods, which is the one thing this
# patch actually depends on.

set(jni_h "${jni_h_path}")

if(NOT EXISTS "${jni_h}")
    message(FATAL_ERROR "patch_jni_h.cmake: '${jni_h}' does not exist")
endif()

file(READ "${jni_h}" content)

# Idempotent: if already patched (e.g. a stale build dir re-running this
# script by hand), don't double-patch. Real bug fixed here (this
# session): the original version of this check called return() at
# top-level, which exits THIS ENTIRE SCRIPT the moment jni.h alone is
# already patched, silently skipping every later patch (vm.cpp,
# method.cpp, field.cpp, fake-jni.cpp) on every subsequent invocation
# after the first, since jni.h only ever needs patching once. Wrapped in
# if/else like every other patch in this file instead, so this section
# being a no-op doesn't prevent the rest of the script from running.
string(FIND "${content}" "stud_is_bionic_address" already_patched)
if(NOT already_patched EQUAL -1)
    message(STATUS "patch_jni_h.cmake: already patched, skipping")
else()

set(dispatch_helpers "
#if defined(__cplusplus)
extern \"C\" {
int stud_is_bionic_address(const void* addr);
void stud_enter_bionic_context(void);
void stud_leave_bionic_context(void);
void* stud_canonical_java_vm(void);
}
namespace stud_jni_dispatch_detail {
// Real, confirmed second bug found chasing the first fix's own knock-on
// effects (the engineering notes, \"investigate why vm is wrong\" entry): this
// process only ever constructs one real JavaVM (`BionicAwareJvm jvm`),
// but more than one pointer VALUE can end up referring to it, the
// correct, ABI-safe `jvm.GetBionicSafeJavaVM()` one, and a raw,
// ABI-non-compliant `&jvm`-derived one (multiple inheritance pushes the
// _JavaVM base subobject to a real, non-zero offset, confirmed live,
// this exact object, via a hardware trace resolving straight to `vtable
// for stud::jni_bridge::BionicAwareJvm`). Rather than hunt every
// internal vendored call site that might construct the wrong variant,
// substitute the one, registered, correct pointer for `this`
// unconditionally at every dispatch, correct regardless of which
// value a given caller (Stud's own code, jnivm's own internals, or
// Roblox itself) happened to pass, since there's structurally only one
// real JavaVM either way. Falls back to the passed-in pointer if none
// was ever registered (e.g. a test binary that never calls
// stud::tls_compat::register_canonical_java_vm()).
inline _JavaVM* stud_canonical_or(_JavaVM* fallback) {
    void* canonical = stud_canonical_java_vm();
    return canonical ? reinterpret_cast<_JavaVM*>(canonical) : fallback;
}
// See the engineering notes' \"Root cause found...\"/\"Plan\" entries for the
// real bug this exists to fix. Generic over every _JavaVM method's
// signature, all 5 just forward their own arguments through
// `functions`, so one small template covers all of them.
//
// Real, deliberate degrade-not-crash decision (the engineering notes, the
// \"try running further\" entry): a failure in one JNI vtable call (e.g.
// a daemon-thread attach whose `vm` value jnivm doesn't recognize)
// should not abort the entire process via an uncaught C++ exception,
// return a real JNI error code instead and keep going, uniformly across
// all 5 methods rather than special-casing just the one confirmed to
// fail. %fs is restored on both the success and exception paths before
// returning either way.
// Real, second compile-time bug caught on the first attempt at extending
// this to _JNIEnv (the engineering notes): the exception-path failure value
// `decltype(fn(args...)){-1}` only compiles for integer return types
// (works for _JavaVM's jint methods), FindClass/GetObjectClass return
// jclass (a pointer), and a pointer can't brace-init from an int literal.
// A small type trait picks the right failure value per return type
// instead (nullptr for pointers, -1 for everything else) so the same
// template covers both categories, plain C++14 (no `if constexpr`,
// matching libjnivm's own build flags, gnu++14).
template <typename T>
struct stud_dispatch_failure {
    static T value() { return T{-1}; }
};
template <typename T>
struct stud_dispatch_failure<T*> {
    static T* value() { return nullptr; }
};

template <typename Fn, typename... Args>
inline auto stud_dispatch(Fn fn, Args... args) -> decltype(fn(args...)) {
    bool entered_bionic = false;
    if (stud_is_bionic_address(reinterpret_cast<const void*>(fn))) {
        stud_enter_bionic_context();
        entered_bionic = true;
    }
    try {
        auto result = fn(args...);
        if (entered_bionic) stud_leave_bionic_context();
        return result;
    } catch (...) {
        if (entered_bionic) stud_leave_bionic_context();
        return stud_dispatch_failure<decltype(fn(args...))>::value();
    }
}
}  // namespace stud_jni_dispatch_detail
#endif /*__cplusplus*/
")

set(original_block "struct _JavaVM {
    const struct JNIInvokeInterface* functions;
#if defined(__cplusplus)
    jint DestroyJavaVM()
    { return functions->DestroyJavaVM(this); }
    jint AttachCurrentThread(JNIEnv** p_env, void* thr_args)
    { return functions->AttachCurrentThread(this, p_env, thr_args); }
    jint DetachCurrentThread()
    { return functions->DetachCurrentThread(this); }
    jint GetEnv(void** env, jint version)
    { return functions->GetEnv(this, env, version); }
    jint AttachCurrentThreadAsDaemon(JNIEnv** p_env, void* thr_args)
    { return functions->AttachCurrentThreadAsDaemon(this, p_env, thr_args); }
#endif /*__cplusplus*/")

set(patched_block "struct _JavaVM {
    const struct JNIInvokeInterface* functions;
#if defined(__cplusplus)
    jint DestroyJavaVM()
    { auto* self = stud_jni_dispatch_detail::stud_canonical_or(this); return stud_jni_dispatch_detail::stud_dispatch(self->functions->DestroyJavaVM, self); }
    jint AttachCurrentThread(JNIEnv** p_env, void* thr_args)
    { auto* self = stud_jni_dispatch_detail::stud_canonical_or(this); return stud_jni_dispatch_detail::stud_dispatch(self->functions->AttachCurrentThread, self, p_env, thr_args); }
    jint DetachCurrentThread()
    { auto* self = stud_jni_dispatch_detail::stud_canonical_or(this); return stud_jni_dispatch_detail::stud_dispatch(self->functions->DetachCurrentThread, self); }
    jint GetEnv(void** env, jint version)
    { auto* self = stud_jni_dispatch_detail::stud_canonical_or(this); return stud_jni_dispatch_detail::stud_dispatch(self->functions->GetEnv, self, env, version); }
    jint AttachCurrentThreadAsDaemon(JNIEnv** p_env, void* thr_args)
    { auto* self = stud_jni_dispatch_detail::stud_canonical_or(this); return stud_jni_dispatch_detail::stud_dispatch(self->functions->AttachCurrentThreadAsDaemon, self, p_env, thr_args); }
#endif /*__cplusplus*/")

string(FIND "${content}" "${original_block}" match_pos)
if(match_pos EQUAL -1)
    message(FATAL_ERROR
        "patch_jni_h.cmake: expected _JavaVM method bodies not found in "
        "'${jni_h}', libjnivm's jni.h changed in a way this patch doesn't "
        "recognize. Update this script's original_block/patched_block to "
        "match the new source before proceeding; do NOT skip this patch "
        "silently, see the file header comment for why it's load-bearing.")
endif()

# Same underlying bug, the _JNIEnv side (the engineering notes, "Real finding:
# the recurring 0x250e0c0 crash..." investigation's follow-on SIGTRAP,
# reached via _JNIEnv::FindClass -> GetObjectClass): _JNIEnv's own
# ~230 inline methods call `functions->X(this, ...)` directly and
# unconditionally too, same as _JavaVM's did before the patch above,
# and libroblox.so's own JNINativeInterface table (the JNIEnv-side
# table, wrapped once at construction by
# wrap_native_interface_for_bionic_callers()) can end up holding a real
# bionic function pointer for a NEWLY created env this process's own
# wrapping hook hasn't reached yet, same collision shape as the
# JavaVM-side bug. Only the two methods with live, confirmed crash
# evidence (FindClass, GetObjectClass) are patched here, not the whole
# struct, grown against further real evidence the same way every
# other fix in this project has been, not guessed at wholesale.
#
# _JNIEnv is defined BEFORE _JavaVM in this file, so the dispatch
# helpers (stud_dispatch et al, shared with the _JavaVM patch above)
# have to be inserted here instead, ahead of first use, at true
# top-level, immediately before "struct _JNIEnv {" itself, NOT inside
# the struct body (inserting extern "C"/namespace text inside an
# already-open struct is invalid C++, caught by a real build failure
# on the first attempt at this patch, fixed by anchoring here instead).
# _JavaVM's own patch below then just replaces its method bodies
# without re-inserting the helpers a second time.
set(original_jnienv_struct_open "struct _JNIEnv {
    /* do not rename this; it does not seem to be entirely opaque */
    const struct JNINativeInterface* functions;")

string(FIND "${content}" "${original_jnienv_struct_open}" jnienv_open_match_pos)
if(jnienv_open_match_pos EQUAL -1)
    message(FATAL_ERROR
        "patch_libjnivm.cmake: expected 'struct _JNIEnv {' opening not "
        "found in '${jni_h}', libjnivm's jni.h changed in a way this "
        "patch doesn't recognize. Update this script before proceeding.")
endif()

string(REPLACE "${original_jnienv_struct_open}" "${dispatch_helpers}\n${original_jnienv_struct_open}" content "${content}")

set(original_jnienv_methods "    jclass FindClass(const char* name)
    { return functions->FindClass(this, name); }")

set(patched_jnienv_methods "    jclass FindClass(const char* name)
    { return stud_jni_dispatch_detail::stud_dispatch(functions->FindClass, this, name); }")

string(FIND "${content}" "${original_jnienv_methods}" jnienv_match_pos)
if(jnienv_match_pos EQUAL -1)
    message(FATAL_ERROR
        "patch_libjnivm.cmake: expected _JNIEnv::FindClass body not found "
        "in '${jni_h}', libjnivm's jni.h changed in a way this patch "
        "doesn't recognize. Update this script before proceeding.")
endif()

string(REPLACE "${original_jnienv_methods}" "${patched_jnienv_methods}" content "${content}")

set(original_getobjectclass "    jclass GetObjectClass(jobject obj)
    { return functions->GetObjectClass(this, obj); }")
set(patched_getobjectclass "    jclass GetObjectClass(jobject obj)
    { return stud_jni_dispatch_detail::stud_dispatch(functions->GetObjectClass, this, obj); }")
string(FIND "${content}" "${original_getobjectclass}" getobjectclass_match_pos)
if(getobjectclass_match_pos EQUAL -1)
    message(FATAL_ERROR
        "patch_libjnivm.cmake: expected _JNIEnv::GetObjectClass body not "
        "found in '${jni_h}', libjnivm's jni.h changed in a way this "
        "patch doesn't recognize. Update this script before proceeding.")
endif()
string(REPLACE "${original_getobjectclass}" "${patched_getobjectclass}" content "${content}")

# Real, live-tested extension of the same _JNIEnv dispatch-wrapping
# pattern to GetMethodID/GetStaticMethodID, tried specifically to test
# a real hypothesis about the still-open `getClassLoader`/`loadClass`
# null-jclass bug (the engineering notes' own long-documented investigation):
# maybe the same JNI-vtable-swap bug class this whole file exists to fix
# also affects these two slots. Live-tested, real, honest NEGATIVE
# result: zero effect on the "class is null" diagnostic (still fires,
# byte-identical), but also zero regression, same milestone reached
# just as fast.
#
# Root cause of the negative result, traced afterward, and a real,
# separate architectural finding worth recording: libroblox.so's own
# pre-compiled JNI calls go through the raw C function-pointer vtable
# (`env->functions->GetMethodID(...)`) directly, never through these
# inline C++ convenience methods (those only get inlined into whichever
# TU is compiled against *this* jni.h. Stud's own code and jnivm's own
# internal code, not libroblox.so, which was compiled against the real
# NDK's jni.h). Separately, `stud_dispatch`'s own bionic-context-switch
# behavior (`canonical_vm_registry.cpp`) is itself now a permanently
# no-op stub in this codebase's CURRENT architecture;
# `stud_is_bionic_address()` unconditionally `return 0`, and
# `stud_wrap_native_for_bionic_caller()` is a bare passthrough. That
# machinery was real and load-bearing for an EARLIER, since-replaced
# architecture (one glibc process manually swapping %fs around bionic
# calls on the same OS thread; see the engineering notes' "Why three
# processes" entry), the current architecture runs Process B as a
# real, standalone bionic process with no glibc/bionic mixing to guard
# against at all, so every `stud_dispatch` call today reduces to a
# plain try/catch (still real: it prevents an uncaught C++ exception
# from propagating out of a JNI call and crashing the process). This
# patch is kept for that real, if modest, exception-safety value and
# for consistency with FindClass/GetObjectClass above, not because it
# fixes the bug that motivated trying it. Do not re-attempt this same
# hypothesis on a different vtable slot expecting a different result,
# the mechanism genuinely cannot reach libroblox.so's own calls, for
# either of the two independent reasons above.
set(original_getmethodid "    jmethodID GetMethodID(jclass clazz, const char* name, const char* sig)
    { return functions->GetMethodID(this, clazz, name, sig); }")
set(patched_getmethodid "    jmethodID GetMethodID(jclass clazz, const char* name, const char* sig)
    { return stud_jni_dispatch_detail::stud_dispatch(functions->GetMethodID, this, clazz, name, sig); }")
string(FIND "${content}" "${original_getmethodid}" getmethodid_h_match_pos)
if(getmethodid_h_match_pos EQUAL -1)
    message(FATAL_ERROR
        "patch_libjnivm.cmake: expected _JNIEnv::GetMethodID body not "
        "found in '${jni_h}', libjnivm's jni.h changed in a way this "
        "patch doesn't recognize. Update this script before proceeding.")
endif()
string(REPLACE "${original_getmethodid}" "${patched_getmethodid}" content "${content}")

set(original_getstaticmethodid "    jmethodID GetStaticMethodID(jclass clazz, const char* name, const char* sig)
    { return functions->GetStaticMethodID(this, clazz, name, sig); }")
set(patched_getstaticmethodid "    jmethodID GetStaticMethodID(jclass clazz, const char* name, const char* sig)
    { return stud_jni_dispatch_detail::stud_dispatch(functions->GetStaticMethodID, this, clazz, name, sig); }")
string(FIND "${content}" "${original_getstaticmethodid}" getstaticmethodid_h_match_pos)
if(getstaticmethodid_h_match_pos EQUAL -1)
    message(FATAL_ERROR
        "patch_libjnivm.cmake: expected _JNIEnv::GetStaticMethodID body "
        "not found in '${jni_h}', libjnivm's jni.h changed in a way "
        "this patch doesn't recognize. Update this script before "
        "proceeding.")
endif()
string(REPLACE "${original_getstaticmethodid}" "${patched_getstaticmethodid}" content "${content}")

# Now replace the _JavaVM block, the dispatch helpers it needs are
# already present (inserted above, before _JNIEnv), so this is just the
# method-body swap, no second helper insertion.
string(REPLACE "${original_block}" "${patched_block}" content "${content}")

file(WRITE "${jni_h}" "${content}")
message(STATUS "patch_libjnivm.cmake: patched '${jni_h}' successfully")
endif()

# Second patch, same file, same underlying bug class: jnivm::VM's own
# constructor (vm.cpp) directly implements AttachCurrentThread's and
# DetachCurrentThread's real logic as two lambdas that call
# `VM::FromJavaVM(vm)` DIRECTLY, not through any _JavaVM C++ method,
# so the jni.h patch above (which only intercepts _JavaVM's own method
# dispatch) never runs for these two specific call paths. Real, confirmed
# bug this closes (the engineering notes, "investigate why vm is wrong"
# entry): real Roblox code calling `functions->AttachCurrentThread(vm,
# ...)` via raw C dispatch (bypassing _JavaVM's C++ methods entirely)
# reaches this lambda directly with whatever `vm` value the caller
# supplied, if that's the ABI-non-compliant `&jvm`-derived pointer
# rather than `jvm.GetBionicSafeJavaVM()`, `FromJavaVM(vm)` throws
# `"Failed to get reference to jnivm::VM"` and aborts the whole process.
# Fix: same `stud_canonical_or` substitution already used in the jni.h
# patch, applied at the top of these same two lambda bodies before
# `vm` is used for anything.
set(vm_cpp "${vm_cpp_path}")
if(NOT EXISTS "${vm_cpp}")
    message(FATAL_ERROR "patch_libjnivm.cmake: '${vm_cpp}' does not exist")
endif()

file(READ "${vm_cpp}" vm_content)

string(FIND "${vm_content}" "stud_canonical_or" vm_already_patched)
if(NOT vm_already_patched EQUAL -1)
    message(STATUS "patch_libjnivm.cmake: '${vm_cpp}' already patched, skipping")
else()
    set(fromjavavm_original "auto&& nvm = *VM::FromJavaVM(vm);")
    string(FIND "${vm_content}" "${fromjavavm_original}" vm_match_pos)
    if(vm_match_pos EQUAL -1)
        message(FATAL_ERROR
            "patch_libjnivm.cmake: expected 'auto&& nvm = *VM::FromJavaVM(vm);' "
            "not found in '${vm_cpp}', libjnivm's vm.cpp changed in a way "
            "this patch doesn't recognize. Update fromjavavm_original/"
            "fromjavavm_patched to match the new source before proceeding; "
            "do NOT skip this patch silently, see this file's own header "
            "comment for why it's load-bearing.")
    endif()
    # Plain string(REPLACE) replaces every occurrence, deliberately,
    # since this exact line appears identically in both the
    # AttachCurrentThread and DetachCurrentThread lambdas (confirmed:
    # exactly 2 occurrences), and both need the identical fix.
    set(fromjavavm_patched "vm = stud_jni_dispatch_detail::stud_canonical_or(vm); auto&& nvm = *VM::FromJavaVM(vm);")
    string(REPLACE "${fromjavavm_original}" "${fromjavavm_patched}" vm_content "${vm_content}")

    # Third patch, same file: FromJavaVM() itself derives the real
    # jnivm::VM* from `vm->functions->reserved0`, jnivm's own
    # constructor sets `iinterface.reserved0 = this`, but that breaks
    # the instant `vm->functions` points at a table Roblox itself
    # constructed (confirmed live: individual slots like
    # AttachCurrentThread still correctly forward to jnivm's real code,
    # but Roblox's own table never sets reserved0, since it has no
    # reason to know jnivm's internal convention, reserved0 reads back
    # null and this throws, aborting the whole process). Since there's
    # only one real jnivm::VM in this process, register it once (right
    # here, in its own constructor, the earliest point with a
    # legitimate jnivm::VM* to hand over) and have FromJavaVM() use it
    # directly, bypassing the reserved0 lookup, and therefore any
    # table-pointer swap, entirely.
    set(ctor_original "	javaVM.functions = &iinterface;
	if(skipInit == false)")
    string(FIND "${vm_content}" "${ctor_original}" ctor_match_pos)
    if(ctor_match_pos EQUAL -1)
        message(FATAL_ERROR
            "patch_libjnivm.cmake: expected VM constructor tail not found in "
            "'${vm_cpp}', update ctor_original/ctor_patched to match the "
            "new source before proceeding.")
    endif()
    set(ctor_patched "	javaVM.functions = &iinterface;
	stud_register_canonical_jnivm_instance(this);
	if(skipInit == false)")
    string(REPLACE "${ctor_original}" "${ctor_patched}" vm_content "${vm_content}")

    set(fromjavavm_fn_original "jnivm::VM *jnivm::VM::FromJavaVM(JavaVM *vm) {
	if(vm == nullptr || vm->functions->reserved0 == nullptr) throw std::runtime_error(\"Failed to get reference to jnivm::VM\");
    return static_cast<jnivm::VM*>(vm->functions->reserved0);
}")
    string(FIND "${vm_content}" "${fromjavavm_fn_original}" fromjavavm_fn_match_pos)
    if(fromjavavm_fn_match_pos EQUAL -1)
        message(FATAL_ERROR
            "patch_libjnivm.cmake: expected FromJavaVM() body not found in "
            "'${vm_cpp}', update fromjavavm_fn_original/fromjavavm_fn_patched "
            "to match the new source before proceeding.")
    endif()
    set(fromjavavm_fn_patched "jnivm::VM *jnivm::VM::FromJavaVM(JavaVM *vm) {
	if (void* canonical = stud_canonical_jnivm_instance()) {
		return static_cast<jnivm::VM*>(canonical);
	}
	if(vm == nullptr || vm->functions->reserved0 == nullptr) throw std::runtime_error(\"Failed to get reference to jnivm::VM\");
    return static_cast<jnivm::VM*>(vm->functions->reserved0);
}")
    string(REPLACE "${fromjavavm_fn_original}" "${fromjavavm_fn_patched}" vm_content "${vm_content}")

    # Fourth patch, same file: RegisterNatives() stores whatever raw
    # function pointer it's handed straight into Method::native, later
    # invoked by method.h's j2invoke() via a plain cast-and-call with
    # whatever %fs context happens to be active at the call site,
    # correct for every native method Stud's own code has ever
    # registered (glibc code, always reached from a bionic caller
    # through the already-%fs-aware JNI vtable dispatch, which swaps to
    # glibc before getting this far), but wrong the moment code running
    # AS the "Java side" (i.e. Stud's own C++, in glibc context) calls a
    # native method Roblox itself registered (AGDK's
    # onStartNative/onResumeNative/onWindowFocusChangedNative/
    # onSurfaceCreatedNative etc, installed via GameActivity_register())
    # see module_range.h's stud_wrap_native_for_bionic_caller() doc
    # comment for the full real reasoning. Fixed once, here, at
    # registration time: wrap the incoming fnPtr with a conditional
    # trampoline if (and only if) it's a bionic address, so j2invoke's
    # existing, unmodified call code always invokes something already
    # safe to call from any context, a pure no-op for every native
    # method this project has already registered and tested (never a
    # bionic address, so the wrapper hands the pointer back unchanged).
    set(regnatives_original "		while(i--) {
			clazz->natives[method->name] = method->fnPtr;")
    string(FIND "${vm_content}" "${regnatives_original}" regnatives_match_pos)
    if(regnatives_match_pos EQUAL -1)
        message(FATAL_ERROR
            "patch_libjnivm.cmake: expected RegisterNatives() loop head not "
            "found in '${vm_cpp}', libjnivm's vm.cpp changed in a way this "
            "patch doesn't recognize. Update regnatives_original/"
            "regnatives_patched to match the new source before proceeding.")
    endif()
    set(regnatives_patched "		while(i--) {
			void* stud_wrapped_fn_ptr = stud_wrap_native_for_bionic_caller(method->fnPtr);
			clazz->natives[method->name] = stud_wrapped_fn_ptr;")
    string(REPLACE "${regnatives_original}" "${regnatives_patched}" vm_content "${vm_content}")

    set(regnatives_m_native_original "			m->native = method->fnPtr;")
    string(FIND "${vm_content}" "${regnatives_m_native_original}" regnatives_m_native_pos)
    if(regnatives_m_native_pos EQUAL -1)
        message(FATAL_ERROR
            "patch_libjnivm.cmake: expected 'm->native = method->fnPtr;' not "
            "found in '${vm_cpp}', update regnatives_m_native_original/"
            "regnatives_m_native_patched to match the new source before "
            "proceeding.")
    endif()
    set(regnatives_m_native_patched "			m->native = stud_wrapped_fn_ptr;")
    string(REPLACE "${regnatives_m_native_original}" "${regnatives_m_native_patched}" vm_content "${vm_content}")

    # Declarations for the new symbols used above, vm.cpp doesn't
    # include jni.h's dispatch_helpers block (that's only inserted into
    # jni.h itself), so these need their own forward declarations here.
    string(PREPEND vm_content "extern \"C\" {
void stud_register_canonical_jnivm_instance(void* vm);
void* stud_canonical_jnivm_instance(void);
void* stud_wrap_native_for_bionic_caller(void* fn);
}
")

    # Sixth patch, same file: real, confirmed gap found live-bisecting a
    # thread's very first LocalFrame construction (the engineering notes, "now
    # proceed" entry): `VM::GetEnv()` (`return jnienvs[pthread_t]`) does
    # NOT lazily create an entry, `std::unordered_map::operator[]`
    # default-constructs an EMPTY shared_ptr for a never-seen key, and
    # `GetJNIEnv()`'s subsequent `->GetJNIEnv()` on that null shared_ptr
    # crashes. On a real device (and on Stud's own main thread, whose
    # `jnienvs` entry is populated once, during the VM's own
    # constructor) this is masked by the JNIInvokeInterface's
    # AttachCurrentThread slot lazily creating the entry on first use,
    # but that slot is exactly the one real Roblox code can (and does,
    # confirmed live) overwrite with its own function (the same
    # already-documented "JNI vtable swap" class this whole file
    # exists to work around), and calling THAT via `_JavaVM::
    # AttachCurrentThread(nullptr, nullptr)` does real, Roblox-internal
    # work with no glibc-side escape hatch. `EnsureEnvForCurrentThread()`
    # is a real, public, directly-callable alternative: the exact same
    # lock+create+store logic the default (never-overwritten)
    # AttachCurrentThread lambda already uses, exposed so Stud's own
    # code can attach a brand-new thread without ever touching the
    # (foreign-code-writable) JNI vtable at all.
    set(getenv_original "const std::shared_ptr<ENV>& VM::GetEnv() {
#ifdef EnableJNIVMGC
	return jnienvs[pthread_self()];
#else
	return jnienvs.begin()->second;
#endif
}")
    string(FIND "${vm_content}" "${getenv_original}" getenv_match_pos)
    if(getenv_match_pos EQUAL -1)
        message(FATAL_ERROR
            "patch_libjnivm.cmake: expected VM::GetEnv() body not found in "
            "'${vm_cpp}', update getenv_original/getenv_patched to match "
            "the new source before proceeding.")
    endif()
    set(getenv_patched "${getenv_original}

void VM::EnsureEnvForCurrentThread() {
	std::lock_guard<std::mutex> lock(mtx);
	auto& slot = jnienvs[pthread_self()];
	if (!slot) {
		slot = CreateEnv();
	}
}")
    string(REPLACE "${getenv_original}" "${getenv_patched}" vm_content "${vm_content}")

    file(WRITE "${vm_cpp}" "${vm_content}")
    message(STATUS "patch_libjnivm.cmake: patched '${vm_cpp}' successfully")
endif()

# Seventh patch, a second header: vm.h needs EnsureEnvForCurrentThread()
# (implemented above) declared as a real, public member, otherwise
# vm.cpp's own definition has no matching declaration.
set(vm_h "${vm_h_path}")
if(NOT EXISTS "${vm_h}")
    message(FATAL_ERROR "patch_libjnivm.cmake: '${vm_h}' does not exist")
endif()

file(READ "${vm_h}" vmh_content)

string(FIND "${vmh_content}" "EnsureEnvForCurrentThread" vmh_already_patched)
if(NOT vmh_already_patched EQUAL -1)
    message(STATUS "patch_libjnivm.cmake: '${vm_h}' already patched, skipping")
else()
    set(getenv_decl_original "        const std::shared_ptr<ENV>& GetEnv();")
    string(FIND "${vmh_content}" "${getenv_decl_original}" vmh_match_pos)
    if(vmh_match_pos EQUAL -1)
        message(FATAL_ERROR
            "patch_libjnivm.cmake: expected 'GetEnv();' declaration not "
            "found in '${vm_h}', update getenv_decl_original/"
            "getenv_decl_patched to match the new source before proceeding.")
    endif()
    set(getenv_decl_patched "${getenv_decl_original}
        // Real, Stud-added (see vm.cpp's own patch, same file group):
        // ensures the calling thread has an ENV, creating and storing
        // one directly via the same lock+map logic the default
        // AttachCurrentThread lambda uses, bypasses the
        // JNIInvokeInterface's AttachCurrentThread slot (and therefore
        // any foreign overwrite of it) entirely.
        void EnsureEnvForCurrentThread();")
    string(REPLACE "${getenv_decl_original}" "${getenv_decl_patched}" vmh_content "${vmh_content}")

    file(WRITE "${vm_h}" "${vmh_content}")
    message(STATUS "patch_libjnivm.cmake: patched '${vm_h}' successfully")
endif()

# Fifth patch, a third file: internal/method.cpp's GetMethodID() template
# has a real, confirmed asymmetry that makes the public env->GetMethodID
# unable to find RegisterNatives()-registered methods at all, found
# chasing why GameActivity's own real lifecycle methods (onStartNative,
# onResumeNative, onWindowFocusChangedNative, onSurfaceCreatedNative,
# confirmed via a temporary diagnostic to genuinely register successfully
# during GameActivity_register()) could never be looked up afterward with
# ordinary JNI reflection, even though a compile-time FakeJni::Function<>-
# declared method (e.g. GameActivityStub::finish) looked up the exact
# same way worked fine.
#
# Root cause: GetMethodID<isStatic, ReturnNull, AllowNative, trace>'s
# search predicate requires `AllowNative == (bool)namesp->native`,
# exact equality, not "native is acceptable too". The public vtable slot
# (vm.cpp's `GetMethodID<false, ReturnNull>`) always leaves AllowNative
# at its default (false), so it only ever matches methods whose
# Method::native is null, true for FakeJni::Function<>-declared
# methods (dispatched via `nativehandle` instead), false for anything
# RegisterNatives() ever installed. Real device JNI makes no such
# distinction between "how a native method got installed" when looking
# it up by name+signature; this is a real jnivm-specific gap, not
# anything Roblox-side.
#
# Fixed generically, not narrowly for GameActivity specifically: if the
# normal (AllowNative-matching-the-template-parameter) search and the
# base-class search both come up empty, and the CALLER didn't
# specifically ask for AllowNative=true already, retry once with
# AllowNative=true before giving up. Strictly additive, can only make
# GetMethodID find MORE real methods than before, never fewer, so every
# already-registered/tested lookup in this project is unaffected.
set(method_cpp "${method_cpp_path}")
if(NOT EXISTS "${method_cpp}")
    message(FATAL_ERROR "patch_libjnivm.cmake: '${method_cpp}' does not exist")
endif()

file(READ "${method_cpp}" method_content)

string(FIND "${method_content}" "Real Stud fix" method_already_patched)
if(NOT method_already_patched EQUAL -1)
    message(STATUS "patch_libjnivm.cmake: '${method_cpp}' already patched, skipping")
else()
    set(getmethodid_original "        if(ReturnNull) {
#ifdef JNI_TRACE
            if(trace) {
                LOG(\"JNIVM\", \"Unresolved symbol, Class=`%s`, %sMethod=`%s`, Signature=`%s`\", cur ? cur->nativeprefix.data() : nullptr, AllowNative ? \"Native\" : isStatic ? \"Static\" : \"\", str0, str1);
            }
#endif
            return nullptr;
        }")
    string(FIND "${method_content}" "${getmethodid_original}" getmethodid_match_pos)
    if(getmethodid_match_pos EQUAL -1)
        message(FATAL_ERROR
            "patch_libjnivm.cmake: expected GetMethodID() ReturnNull branch not "
            "found in '${method_cpp}', libjnivm's method.cpp changed in a way "
            "this patch doesn't recognize. Update getmethodid_original/"
            "getmethodid_patched to match the new source before proceeding.")
    endif()
    set(getmethodid_patched "        if (!AllowNative) {
            // Real Stud fix: the public env->GetMethodID always
            // instantiates AllowNative=false, so a method registered at
            // RUNTIME via RegisterNatives (Method::native set to a real
            // fn pointer) is invisible to it, only compile-time
            // FakeJni::Function<>-declared methods (Method::native left
            // null, dispatched via `nativehandle` instead) are found.
            // Real device JNI makes no such distinction. Retry once,
            // allowing native-registered methods too, before giving up.
            // trace=false on the retry so a genuine miss is reported once,
            // by the outermost call, rather than twice.
            jmethodID stud_native_fallback_id = GetMethodID<isStatic, true, true, false>(env, cl, str0, str1);
            if (stud_native_fallback_id) {
                return stud_native_fallback_id;
            }
        }
        if(ReturnNull) {
#ifdef JNI_TRACE
            if(trace) {
                LOG(\"JNIVM\", \"Unresolved symbol, Class=`%s`, %sMethod=`%s`, Signature=`%s`\", cur ? cur->nativeprefix.data() : nullptr, AllowNative ? \"Native\" : isStatic ? \"Static\" : \"\", str0, str1);
            }
#endif
            return nullptr;
        }")
    string(REPLACE "${getmethodid_original}" "${getmethodid_patched}" method_content "${method_content}")

    # Seventh patch, same file: MDispatchBase2<T>::CallMethod's instance
    # dispatch (the real function every env->CallVoidMethod/CallObjectMethod/
    # etc call funnels through) only ever checks `mid->nativehandle`,
    # the compile-time FakeJni::Function<>-registered path. A method
    # RegisterNatives() installed at runtime (Method::native set,
    # nativehandle left null, exactly what the GetMethodID fix above
    # makes findable) silently falls into the "Call Unknown Member
    # Function" branch and returns a default value, WITHOUT EVER CALLING
    # THE REAL NATIVE FUNCTION. Confirmed live: GameActivity's own
    # onSurfaceCreatedNative (and every other AGDK lifecycle method Stud
    # drives via drive_game_activity_lifecycle()/the try_bootstrap.cpp
    # probe) reports "returned" with zero exception/trap, because
    # CallMethod never actually invoked it, not because the real,
    # confirmed-present ANativeWindow_fromSurface() call inside it (see
    # AGDK's own public source, GameActivity.cpp) somehow declined to run.
    #
    # Fixed generically: if nativehandle is unset but native IS set,
    # invoke the raw native fn pointer directly, mirroring exactly how
    # Method::j2invoke (method.h) already calls it elsewhere in this
    # library, just driven by a runtime jvalue* array (already correctly
    # boxed by the caller, matching CallVoidMethodA's own convention)
    # instead of a compile-time argument pack. Every JNI value type this
    # project's own real call sites use (J/Z/B/S/C/I/L) is bit-identical,
    # 8 bytes, to how jvalue stores it, SysV classifies all of them as
    # a single INTEGER-class eightbyte, so passing a `jvalue` by value in
    # each argument slot reproduces the exact register content a
    # correctly-typed (jlong/jobject/jint/...) parameter would receive,
    # with zero knowledge of the callee's real declared parameter types
    # needed. F/D (float/double) would need SSE-class register passing
    # instead, not silently mishandled, explicitly detected and
    # rejected (falls through to the same "Unknown Member Function"
    # degrade-not-crash default already used for every other unresolved
    # case), since no real call site in this project has needed it yet;
    # grow this the same iterative, evidence-driven way as every other
    # gap in libc_shim/android_glue rather than guessing float-register
    # handling upfront.
    set(callmethod_original "template<class T> T jnivm::MDispatchBase2<T>::CallMethod(JNIEnv *env, jobject obj, jmethodID id, jvalue *param) {
    auto mid = ((Method *)id);
#ifdef JNI_DEBUG
    if(!obj)
        LOG(\"JNIVM\", \"CallMethod object is null\");
    if(!id)
        LOG(\"JNIVM\", \"CallMethod field is null\");
#endif
    if (mid && mid->nativehandle) {
        auto cl = JNITypes<std::shared_ptr<Class>>::JNICast(ENV::FromJNIEnv(env), env->GetObjectClass(obj));
        mid = findVirtualOverload(ENV::FromJNIEnv(env), cl.get(), mid);
#ifdef JNI_TRACE
        LOG(\"JNIVM\", \"Call Member Function Class=`%s` Method=`%s` Signature=`%s`\", cl ? cl->nativeprefix.data() : \"???\", mid->name.data(), mid->signature.data());
#endif
        try {
            return static_cast<jnivm::impl::MethodHandleBase<T>*>(mid->nativehandle.get())->InstanceInvoke(ENV::FromJNIEnv(env), obj, param, jnivm::impl::MethodHandleBase<T>{});
        } catch (...) {
            auto cur = std::make_shared<Throwable>();
            cur->except = std::current_exception();
            (ENV::FromJNIEnv(env))->current_exception = cur;
#ifdef JNI_TRACE
            env->ExceptionDescribe();
#endif
            return defaultVal<T>(ENV::FromJNIEnv(env), mid ? mid->signature : \"\");
        }
    } else {
#ifdef JNI_TRACE
        auto cl = JNITypes<std::shared_ptr<Class>>::JNICast(ENV::FromJNIEnv(env), env->GetObjectClass(obj));
        LOG(\"JNIVM\", \"Call Unknown Member Function Class=`%s` Method=`%s` Signature=`%s`\", cl ? cl->nativeprefix.data() : \"???\", mid ? mid->name.data() : \"???\", mid ? mid->signature.data() : \"???\");
#endif
        return defaultVal<T>(ENV::FromJNIEnv(env), mid ? mid->signature : \"\");")

    string(FIND "${method_content}" "${callmethod_original}" callmethod_match_pos)
    if(callmethod_match_pos EQUAL -1)
        message(FATAL_ERROR
            "patch_libjnivm.cmake: expected MDispatchBase2<T>::CallMethod(jobject) "
            "body not found in '${method_cpp}', libjnivm's method.cpp changed in "
            "a way this patch doesn't recognize. Update callmethod_original/"
            "callmethod_patched to match the new source before proceeding.")
    endif()

    set(callmethod_helpers "namespace stud_native_dispatch_detail {
template<class T> struct StudZero { static T value() { return T{}; } };
template<> struct StudZero<void> { static void value() {} };
struct StudNativeArgInfo { size_t count = 0; bool unsupported_fp = false; };
inline StudNativeArgInfo stud_parse_native_argc(const std::string& signature) {
    StudNativeArgInfo info;
    size_t i = 1; // skip leading '('
    while (i < signature.size() && signature[i] != ')') {
        char c = signature[i];
        if (c == 'L') {
            i = signature.find(';', i);
            if (i == std::string::npos) break;
        } else if (c == '[') {
            i++;
            continue;
        } else if (c == 'F' || c == 'D') {
            info.unsupported_fp = true;
        }
        info.count++;
        i++;
    }
    return info;
}
template<class T>
T stud_invoke_registered_native(JNIEnv* env, jobject obj, void* native, jvalue* param, size_t argc) {
    switch (argc) {
        case 0: return ((T(*)(JNIEnv*, jobject))native)(env, obj);
        case 1: return ((T(*)(JNIEnv*, jobject, jvalue))native)(env, obj, param[0]);
        case 2: return ((T(*)(JNIEnv*, jobject, jvalue, jvalue))native)(env, obj, param[0], param[1]);
        case 3: return ((T(*)(JNIEnv*, jobject, jvalue, jvalue, jvalue))native)(env, obj, param[0], param[1], param[2]);
        case 4: return ((T(*)(JNIEnv*, jobject, jvalue, jvalue, jvalue, jvalue))native)(env, obj, param[0], param[1], param[2], param[3]);
        case 5: return ((T(*)(JNIEnv*, jobject, jvalue, jvalue, jvalue, jvalue, jvalue))native)(env, obj, param[0], param[1], param[2], param[3], param[4]);
        case 6: return ((T(*)(JNIEnv*, jobject, jvalue, jvalue, jvalue, jvalue, jvalue, jvalue))native)(env, obj, param[0], param[1], param[2], param[3], param[4], param[5]);
        default:
            LOG(\"JNIVM\", \"stud_invoke_registered_native: %zu args unsupported, skipping\", argc);
            return StudZero<T>::value();
    }
}
}  // namespace stud_native_dispatch_detail

")

    set(callmethod_patched "${callmethod_helpers}template<class T> T jnivm::MDispatchBase2<T>::CallMethod(JNIEnv *env, jobject obj, jmethodID id, jvalue *param) {
    auto mid = ((Method *)id);
#ifdef JNI_DEBUG
    if(!obj)
        LOG(\"JNIVM\", \"CallMethod object is null\");
    if(!id)
        LOG(\"JNIVM\", \"CallMethod field is null\");
#endif
    if (mid && mid->nativehandle) {
        auto cl = JNITypes<std::shared_ptr<Class>>::JNICast(ENV::FromJNIEnv(env), env->GetObjectClass(obj));
        mid = findVirtualOverload(ENV::FromJNIEnv(env), cl.get(), mid);
#ifdef JNI_TRACE
        LOG(\"JNIVM\", \"Call Member Function Class=`%s` Method=`%s` Signature=`%s`\", cl ? cl->nativeprefix.data() : \"???\", mid->name.data(), mid->signature.data());
#endif
        try {
            return static_cast<jnivm::impl::MethodHandleBase<T>*>(mid->nativehandle.get())->InstanceInvoke(ENV::FromJNIEnv(env), obj, param, jnivm::impl::MethodHandleBase<T>{});
        } catch (...) {
            auto cur = std::make_shared<Throwable>();
            cur->except = std::current_exception();
            (ENV::FromJNIEnv(env))->current_exception = cur;
#ifdef JNI_TRACE
            env->ExceptionDescribe();
#endif
            return defaultVal<T>(ENV::FromJNIEnv(env), mid ? mid->signature : \"\");
        }
    } else if (mid && mid->native && !stud_native_dispatch_detail::stud_parse_native_argc(mid->signature).unsupported_fp) {
        auto stud_argc = stud_native_dispatch_detail::stud_parse_native_argc(mid->signature);
#ifdef JNI_TRACE
        LOG(\"JNIVM\", \"Call RegisterNatives-installed Member Function Method=`%s` Signature=`%s`\", mid->name.data(), mid->signature.data());
#endif
        return stud_native_dispatch_detail::stud_invoke_registered_native<T>(env, obj, mid->native, param, stud_argc.count);
    } else {
#ifdef JNI_TRACE
        auto cl = JNITypes<std::shared_ptr<Class>>::JNICast(ENV::FromJNIEnv(env), env->GetObjectClass(obj));
        LOG(\"JNIVM\", \"Call Unknown Member Function Class=`%s` Method=`%s` Signature=`%s`\", cl ? cl->nativeprefix.data() : \"???\", mid ? mid->name.data() : \"???\", mid ? mid->signature.data() : \"???\");
#endif
        return defaultVal<T>(ENV::FromJNIEnv(env), mid ? mid->signature : \"\");")

    string(REPLACE "${callmethod_original}" "${callmethod_patched}" method_content "${method_content}")

    # Eighth patch, same file: GetMethodID<>'s "class is null" diagnostic
    # (the engineering notes, "A real, previously-undiscovered class-resolution
    # failure cluster" entry) proves the CALLER passed a literal null
    # jclass, but says nothing about which real code produced that null
    # and a JNI_TRACE pass on FindClass() came back with zero hits,
    # ruling out a plain by-name lookup as the source. Live tracing
    # is separately, already documented as unreliable in this sandbox
    # for anything requiring `continue`. This adds a real, no-debugger-
    # needed native backtrace (manual rbp-chain walk + dladdr(), the
    # same class of technique this project's own trap_recovery.cpp and
    # this session's own raw-stack-scan work already validated in
    # this exact environment) printed right at the log site, so a single
    # live run's stdout directly answers "what real code called this"
    # instead of requiring another investigation pass.
    set(getmethodid_diag_original "    } else {
#ifdef JNI_DEBUG
        LOG(\"JNIVM\", \"Get%sMethodID class is null %s, %s\", AllowNative ? \"Native\" : isStatic ? \"Static\" : \"\", str0, str1);
#endif
    }")
    string(FIND "${method_content}" "${getmethodid_diag_original}" getmethodid_diag_match_pos)
    if(getmethodid_diag_match_pos EQUAL -1)
        message(FATAL_ERROR
            "patch_libjnivm.cmake: expected GetMethodID() 'class is null' branch "
            "not found in '${method_cpp}', libjnivm's method.cpp changed in a "
            "way this patch doesn't recognize. Update getmethodid_diag_original/"
            "getmethodid_diag_patched to match the new source before proceeding.")
    endif()
    set(getmethodid_diag_helpers "namespace stud_getmethodid_diag_detail {
inline void stud_print_native_backtrace() {
    void **frame = static_cast<void **>(__builtin_frame_address(0));
    void *prev = nullptr;
    for (int i = 0; i < 16 && frame; ++i) {
        if (prev && (void *)frame <= prev) break;
        prev = (void *)frame;
        void *ret_addr = frame[1];
        if (!ret_addr) break;
        Dl_info info;
        if (dladdr(ret_addr, &info) && info.dli_fname) {
            unsigned long offset = reinterpret_cast<unsigned long>(ret_addr) -
                                    reinterpret_cast<unsigned long>(info.dli_fbase);
            printf(\"[JNIVM]:   frame[%d]=%p -> %s + 0x%lx (%s)\\n\", i, ret_addr,
                   info.dli_fname, offset, info.dli_sname ? info.dli_sname : \"?\");
        } else {
            printf(\"[JNIVM]:   frame[%d]=%p (unresolved)\\n\", i, ret_addr);
        }
        frame = static_cast<void **>(frame[0]);
    }
}
}  // namespace stud_getmethodid_diag_detail
")
    set(getmethodid_diag_patched "    } else {
#ifdef JNI_DEBUG
        LOG(\"JNIVM\", \"Get%sMethodID class is null %s, %s\", AllowNative ? \"Native\" : isStatic ? \"Static\" : \"\", str0, str1);
        stud_getmethodid_diag_detail::stud_print_native_backtrace();
#endif
    }")
    string(REPLACE "${getmethodid_diag_original}" "${getmethodid_diag_patched}" method_content "${method_content}")
    string(REPLACE "using namespace jnivm;" "using namespace jnivm;\n#include <dlfcn.h>\n#include <cstdio>\n${getmethodid_diag_helpers}" method_content "${method_content}")

    file(WRITE "${method_cpp}" "${method_content}")
    message(STATUS "patch_libjnivm.cmake: patched '${method_cpp}' successfully")
endif()

# Patches libjnivm's vendored GetFieldID<> (field.cpp) to null-check the
# resolved Class pointer before dereferencing it.
#
# Real, confirmed bug this fixes (the engineering notes, the "V2InitWithParams
# crashes: jnivm::GetFieldID on a null class" entry): real Roblox code
# (reached via NativeGLInterface's V2 app-bridge API) calls
# env->GetFieldID(cl_, "dpiScale", "F") with a null cl_ argument (the
# prior GetObjectClass/FindClass call that should have produced a real
# class evidently failed or was never made the way MainGameActivity's
# own V1 bootstrap path does it), jnivm's real GetFieldID<> never
# null-checks `cl` before `std::lock_guard<std::mutex> lock(cl->mtx)`,
# so this crashes the WHOLE PROCESS with a genuine null-pointer
# dereference (fault_addr near 0x60, `cl->mtx`'s real field offset),
# not a recoverable stack-overflow-shaped fault the existing SIGSEGV
# recovery mechanism can safely catch.
#
# Real JNI semantics: a null class argument to GetFieldID is a caller
# error real JNI implementations handle safely (undefined per spec, but
# no real JVM segfaults the whole process over it); Roblox's own code
# is already well-defended elsewhere in this project against JNI lookup
# failures (logs and continues rather than assuming success), so the
# safe fix is making this lookup FAIL CLEANLY (return a null jfieldID,
# the same value this function's own "not found" path already returns)
# instead of crashing.

set(field_cpp "${field_cpp_path}")

if(NOT EXISTS "${field_cpp}")
    message(FATAL_ERROR "patch_libjnivm.cmake: '${field_cpp}' does not exist")
endif()

file(READ "${field_cpp}" field_content)

string(FIND "${field_content}" "stud_null_class_guard" field_already_patched)
if(NOT field_already_patched EQUAL -1)
    message(STATUS "patch_libjnivm.cmake: '${field_cpp}' already patched, skipping")
else()
    set(getfieldid_original "jfieldID jnivm::GetFieldID(JNIEnv *env, jclass cl_, const char *name, const char *type) {
    auto cl = JNITypes<std::shared_ptr<Class>>::JNICast(ENV::FromJNIEnv(env), cl_);
    std::lock_guard<std::mutex> lock(cl->mtx);")

    set(getfieldid_patched "jfieldID jnivm::GetFieldID(JNIEnv *env, jclass cl_, const char *name, const char *type) {
    auto cl = JNITypes<std::shared_ptr<Class>>::JNICast(ENV::FromJNIEnv(env), cl_);
    if (!cl) {  // stud_null_class_guard
#ifdef JNI_TRACE
        LOG(\"JNIVM\", \"GetFieldID: null class, Field=`%s`, Signature=`%s`\", name, type);
#endif
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(cl->mtx);")

    string(FIND "${field_content}" "${getfieldid_original}" field_match_pos)
    if(field_match_pos EQUAL -1)
        message(FATAL_ERROR "patch_libjnivm.cmake: GetFieldID's real body not found in '${field_cpp}', libjnivm upstream changed, needs re-verifying")
    endif()

    string(REPLACE "${getfieldid_original}" "${getfieldid_patched}" field_content "${field_content}")
    file(WRITE "${field_cpp}" "${field_content}")
    message(STATUS "patch_libjnivm.cmake: patched '${field_cpp}' successfully")
endif()

# Sixth patch, internal/method.cpp and internal/field.cpp: the two "MISS"
# diagnostics that found nearly every JNI gap this project has fixed.
# They had been edited straight into the build tree and never mirrored
# here, so a fresh fetch silently dropped both, confirmed by deleting
# the checkout and watching them disappear. A lookup that fails in
# silence is exactly the failure mode they exist to end: libroblox's own
# Djinni glue reports only "GetMethodID returned null" before refusing to
# install a platform implementation, and jnivm's own callers then hit an
# anonymous "GetField field is null".
#
# The method one is guarded on `trace`. GetMethodID recurses over base
# classes with ReturnNull=true, so without the guard every level of the
# walk reported a miss, a lookup that then succeeded on the derived
# class still printed misses for Activity, Context and Object, which
# trains a reader to ignore the very line that matters. The recursion
# already passes trace=false to mean "interior step, not the answer", so
# a genuine miss prints exactly once, from the outermost call.
set(method_miss_cpp "${method_cpp_path}")
file(READ "${method_miss_cpp}" method_miss_content)
string(FIND "${method_miss_content}" "STUD_DIAG GetMethodID MISS" method_miss_present)
if(NOT method_miss_present EQUAL -1)
    message(STATUS "patch_libjnivm.cmake: the GetMethodID MISS diagnostic is already in '${method_miss_cpp}'")
else()
    set(method_miss_original "        if(ReturnNull) {
#ifdef JNI_TRACE
            if(trace) {
                LOG(\"JNIVM\", \"Unresolved symbol, Class=`%s`, %sMethod=`%s`, Signature=`%s`\", cur ? cur->nativeprefix.data() : nullptr, AllowNative ? \"Native\" : isStatic ? \"Static\" : \"\", str0, str1);
            }
#endif
            return nullptr;
        }
        next = std::make_shared<Method>();")
    set(method_miss_patched "        if(ReturnNull) {
#ifdef JNI_DEBUG
            // Only the outermost lookup reports: the base-class recursion
            // passes trace=false, so a method found on a derived class no
            // longer prints a miss for every base walked past on the way.
            if(trace) {
                LOG(\"JNIVM\", \"STUD_DIAG GetMethodID MISS class=`%s` %smethod=`%s` sig=`%s`\",
                    cur ? cur->nativeprefix.data() : \"(null)\",
                    AllowNative ? \"native \" : isStatic ? \"static \" : \"\",
                    str0 ? str0 : \"(null)\", str1 ? str1 : \"(null)\");
            }
#endif
#ifdef JNI_TRACE
            if(trace) {
                LOG(\"JNIVM\", \"Unresolved symbol, Class=`%s`, %sMethod=`%s`, Signature=`%s`\", cur ? cur->nativeprefix.data() : nullptr, AllowNative ? \"Native\" : isStatic ? \"Static\" : \"\", str0, str1);
            }
#endif
            return nullptr;
        }
        next = std::make_shared<Method>();")
    string(FIND "${method_miss_content}" "${method_miss_original}" method_miss_pos)
    if(method_miss_pos EQUAL -1)
        message(FATAL_ERROR
            "patch_libjnivm.cmake: GetMethodID's ReturnNull branch not found in "
            "'${method_miss_cpp}', libjnivm changed, re-verify this patch")
    endif()
    string(REPLACE "${method_miss_original}" "${method_miss_patched}" method_miss_content "${method_miss_content}")
    file(WRITE "${method_miss_cpp}" "${method_miss_content}")
    message(STATUS "patch_libjnivm.cmake: added the GetMethodID MISS diagnostic to '${method_miss_cpp}'")
endif()

set(field_miss_cpp "${field_cpp_path}")
file(READ "${field_miss_cpp}" field_miss_content)
string(FIND "${field_miss_content}" "STUD_DIAG GetFieldID MISS" field_miss_present)
if(NOT field_miss_present EQUAL -1)
    message(STATUS "patch_libjnivm.cmake: the GetFieldID MISS diagnostic is already in '${field_miss_cpp}'")
else()
    set(field_miss_original "        if(ReturnNull) {
#ifdef JNI_TRACE
            if(trace) {
                LOG(\"JNIVM\", \"Unresolved symbol, Class=`%s`, %sField=`%s`, Signature=`%s`\", cl ? cl->nativeprefix.data() : nullptr, isStatic ? \"Static\" : \"\", name, type);
            }
#endif
            return nullptr;
        }
        next = std::make_shared<Field>();")
    set(field_miss_patched "        if(ReturnNull) {
#ifdef JNI_DEBUG
            // Same reasoning as the method one above: a field that is not
            // found returns null in silence, and the caller then hits the
            // anonymous \"GetField field is null\". Name it instead.
            if(trace) {
                LOG(\"JNIVM\", \"STUD_DIAG GetFieldID MISS class=`%s` %sfield=`%s` sig=`%s`\",
                    cl ? cl->nativeprefix.data() : \"(null)\", isStatic ? \"static \" : \"\",
                    name ? name : \"(null)\", type ? type : \"(null)\");
            }
#endif
#ifdef JNI_TRACE
            if(trace) {
                LOG(\"JNIVM\", \"Unresolved symbol, Class=`%s`, %sField=`%s`, Signature=`%s`\", cl ? cl->nativeprefix.data() : nullptr, isStatic ? \"Static\" : \"\", name, type);
            }
#endif
            return nullptr;
        }
        next = std::make_shared<Field>();")
    string(FIND "${field_miss_content}" "${field_miss_original}" field_miss_pos)
    if(field_miss_pos EQUAL -1)
        message(FATAL_ERROR
            "patch_libjnivm.cmake: GetFieldID's ReturnNull branch not found in "
            "'${field_miss_cpp}', libjnivm changed, re-verify this patch")
    endif()
    string(REPLACE "${field_miss_original}" "${field_miss_patched}" field_miss_content "${field_miss_content}")
    file(WRITE "${field_miss_cpp}" "${field_miss_content}")
    message(STATUS "patch_libjnivm.cmake: added the GetFieldID MISS diagnostic to '${field_miss_cpp}'")
endif()

# Patches fake-jni.cpp's ThreadContext::~ThreadContext() to not abort()
# the whole process over a lingering env reference at thread exit.
#
# Real, confirmed-live bug this fixes: process-b's start_app_with_params_
# background() (see engine_v2_bridge.cpp) calls Roblox's own
# nativeAppBridgeV2StartAppWithParams on a detached background thread,
# wrapped in trap_recovery.cpp's siglongjmp-based crash recovery (a real,
# separately root-caused null-pointer SIGSEGV deep inside Roblox's own
# non-essential telemetry-logging code, see engine_v2_bridge.cpp's own
# doc comment). Recovering via siglongjmp is deliberate and correct for
# getting this ONE call to stop crashing the whole process, but has a
# real, now-confirmed cost: it jumps straight out of Roblox's own
# (crashed, third-party, opaque) call stack, skipping whatever cleanup
# that code would have done on its way out normally, including,
# apparently, releasing whatever reference it took on this thread's JNI
# env. That leaked reference is invisible until this thread actually
# exits and ThreadContext's own thread_local destructor runs its
# consistency check (env.lock() still succeeding after
# DetachCurrentThread should have released the last reference), which
# jnivm's own code treats as a hard, unrecoverable bug and calls abort().
# Confirmed via this project's own in-process crash diagnostics
# (trap_recovery.cpp's describe_address()/print_backtrace(), added this
# session specifically because a debugger cannot attach usefully to a binary
# bionic's own linker64 loads via its internal ELF-mapping logic rather
# than a real execve(); see that file's own doc comment): the real
# abort() call's PC resolves into bionic's own libc.so, reached from
# exactly this destructor.
#
# Real JNI semantics point (same reasoning already applied to the
# GetFieldID null-class fix above): a real device's JNI implementation
# doesn't crash the whole VM over one thread's env bookkeeping being in
# an unexpected state at exit, and here, the thread is already in the
# process of exiting regardless. The resource genuinely leaks (a real,
# acknowledged cost. This is a workaround for the recovered crash's
# side effect, not a fix for the leak's root cause, which lives inside
# Roblox's own opaque compiled code), but leaking one already-abandoned
# thread's own env reference is far safer than aborting the entire
# multi-threaded process over it.
set(fake_jni_cpp "${fake_jni_cpp_path}")

if(NOT EXISTS "${fake_jni_cpp}")
    message(FATAL_ERROR "patch_libjnivm.cmake: '${fake_jni_cpp}' does not exist")
endif()

file(READ "${fake_jni_cpp}" fake_jni_content)

string(FIND "${fake_jni_content}" "stud_thread_context_leak_guard" fake_jni_already_patched)
if(NOT fake_jni_already_patched EQUAL -1)
    message(STATUS "patch_libjnivm.cmake: '${fake_jni_cpp}' already patched, skipping")
else()
    set(threadcontext_original "FakeJni::ThreadContext::~ThreadContext() {
	auto _env = env.lock();
	if(_env) {
		_env->getVM().DetachCurrentThread();
		_env = nullptr;
		if(env.lock()) {
			abort();
		}
	}
}")
    string(FIND "${fake_jni_content}" "${threadcontext_original}" threadcontext_match_pos)
    if(threadcontext_match_pos EQUAL -1)
        message(FATAL_ERROR
            "patch_libjnivm.cmake: expected ThreadContext::~ThreadContext() body "
            "not found in '${fake_jni_cpp}', fake-jni's source changed in a way "
            "this patch doesn't recognize. Update threadcontext_original/"
            "threadcontext_patched to match the new source before proceeding; do "
            "NOT skip this patch silently, see this block's own doc comment for "
            "why it's load-bearing.")
    endif()
    set(threadcontext_patched "FakeJni::ThreadContext::~ThreadContext() {
	auto _env = env.lock();
	if(_env) {
		_env->getVM().DetachCurrentThread();
		_env = nullptr;
		if(env.lock()) {
			// stud_thread_context_leak_guard: see patch_libjnivm.cmake's own
			// doc comment for the full, real reasoning, degrade (leak,
			// log, keep exiting normally) instead of aborting the whole
			// process over one already-recovered-from-crash thread's own
			// env bookkeeping.
			std::fprintf(stderr, \"[JNIVM]: ThreadContext leaked an env reference at thread exit (stud_thread_context_leak_guard)\\n\");
		}
	}
}")
    string(REPLACE "${threadcontext_original}" "${threadcontext_patched}" fake_jni_content "${fake_jni_content}")
    string(PREPEND fake_jni_content "#include <cstdio>\n")

    file(WRITE "${fake_jni_cpp}" "${fake_jni_content}")
    message(STATUS "patch_libjnivm.cmake: patched '${fake_jni_cpp}' successfully")
endif()

# ---------------------------------------------------------------------
# jnitypes.h: name the real type mismatch behind
# `Invalid Reference, Unexpected Type`.
#
# UnpackJObject<T>() throws that when its dynamic_cast fails. That throw
# is fatal in practice: libroblox checks for a pending JNI exception,
# logs `RBXCRASH: JNI: Crashing due to unhandled Java exception` and
# traps, which kills whichever thread it happened on, including, in
# two real live-caught cases, the thread the engine had designated as
# its own internal "main" thread, after which nothing ever drained its
# task queue again (see the engineering notes, "what drains the engine's task
# queue"). The bare message names neither the type that arrived nor the
# one expected, which is exactly what is needed to find the bug; both
# real instances (ByteBufferStub, ClassMetaStub; Stud stub classes
# colliding with a jnivm built-in over the same real Java class name)
# were identified immediately once this printed the two typeids.
set(jnitypes_h "${jnitypes_h_path}")
if(NOT EXISTS "${jnitypes_h}")
    message(FATAL_ERROR "patch_libjnivm.cmake: '${jnitypes_h}' does not exist")
endif()

file(READ "${jnitypes_h}" jnitypes_content)

string(FIND "${jnitypes_content}" "STUD_DIAG UnpackJObject mismatch" jnitypes_already_patched)
if(NOT jnitypes_already_patched EQUAL -1)
    message(STATUS "patch_libjnivm.cmake: '${jnitypes_h}' already patched, skipping")
else()
    set(unpack_original "    throw std::runtime_error(\"Invalid Reference, Unexpected Type\")")
    string(FIND "${jnitypes_content}" "${unpack_original}" jnitypes_match_pos)
    if(jnitypes_match_pos EQUAL -1)
        message(FATAL_ERROR "patch_libjnivm.cmake: could not find the UnpackJObject throw in '${jnitypes_h}'")
    endif()

    set(unpack_patched "    std::fprintf(stderr, \"[JNIVM]: STUD_DIAG UnpackJObject mismatch: have=%s want=%s\\n\", typeid(*obj).name(), typeid(T).name());\n    std::fflush(stderr);\n    throw std::runtime_error(\"Invalid Reference, Unexpected Type\")")

    string(REPLACE "${unpack_original}" "${unpack_patched}" jnitypes_content "${jnitypes_content}")
    string(PREPEND jnitypes_content "#include <cstdio>\n#include <typeinfo>\n")

    file(WRITE "${jnitypes_h}" "${jnitypes_content}")
    message(STATUS "patch_libjnivm.cmake: patched '${jnitypes_h}' successfully")
endif()
