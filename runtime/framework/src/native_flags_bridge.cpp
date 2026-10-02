#include "stud/native_flags_bridge.h"

#include "stud/trap_recovery.h"

namespace stud::jni_bridge {

namespace {

using StringArrayFn = jobject (*)(JNIEnv*, jclass, jobjectArray);

}  // namespace

NativeFlagsBridgeResult run_native_flags_bridge(FakeJni::Jvm& jvm,
                                                 const stud::linker::LoadedLibrary& lib) {
    NativeFlagsBridgeResult result;

    void* addr = lib.find_symbol(
        "Java_com_roblox_client_flags_FlagJniInterface_nativeInitializeNativeFlags");
    if (addr == nullptr) {
        return result;
    }

    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    // Called in its place in the boot sequence, with no names. The engine
    // only looks each name up and returns the values to the app's Java
    // side, which Stud does not run, so nothing here reads the answer;
    // see native_flags_bridge.h.
    jclass string_class = env.FindClass("java/lang/String");
    jobjectArray flag_names_array = env.NewObjectArray(0, string_class, nullptr);
    auto* fn = reinterpret_cast<StringArrayFn>(addr);

    result.called = true;
    result.trapped_abort = !call_trapping_abort(fn, jni_env, nullptr, flag_names_array);
    clear_pending_jni_exception(jni_env, "nativeInitializeNativeFlags");

    return result;
}

}  // namespace stud::jni_bridge
