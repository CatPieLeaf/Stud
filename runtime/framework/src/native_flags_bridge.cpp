#include "stud/native_flags_bridge.h"

#include "stud/real_native_flag_names.h"
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

    jclass string_class = env.FindClass("java/lang/String");
    jobjectArray flag_names_array =
        env.NewObjectArray(kRealNativeFlagNamesCount, string_class, nullptr);
    for (int i = 0; i < kRealNativeFlagNamesCount; ++i) {
        jstring name_ref = env.NewStringUTF(kRealNativeFlagNames[i]);
        jni_env->SetObjectArrayElement(flag_names_array, i, name_ref);
    }
    auto* fn = reinterpret_cast<StringArrayFn>(addr);

    result.called = true;
    result.trapped_abort = !call_trapping_abort(fn, jni_env, nullptr, flag_names_array);
    clear_pending_jni_exception(jni_env, "nativeInitializeNativeFlags");

    return result;
}

}  // namespace stud::jni_bridge
