#include "stud/bootstrap.h"

#include "stud/trap_recovery.h"

namespace stud::jni_bridge {

namespace {

using PreloadFlagOverridesFn = void (*)(JNIEnv*, jclass, jstring);
using SetAssetPathFn = void (*)(JNIEnv*, jclass, jstring);
using AppBridgeSetInitParamsFn = void (*)(JNIEnv*, jclass, jobject);
using SetDeviceInfoFn = void (*)(JNIEnv*, jclass, jobject);

template <typename Fn>
Fn find_required_symbol(const stud::linker::LoadedLibrary& lib, const char* name) {
    void* addr = lib.find_symbol(name);
    if (addr == nullptr) {
        throw stud::linker::LoadError(std::string("bootstrap: required symbol not found: ") + name);
    }
    return reinterpret_cast<Fn>(addr);
}

}  // namespace

PreloadBootstrapResult run_preload_bootstrap(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                              const std::string& asset_path,
                                              const FlagOverrides& flag_overrides) {
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    PreloadBootstrapResult result;

    // Real order (the engineering notes, "Sober does not patch libroblox.so
    // either" entry): nativeSetAssetPath BEFORE nativePreloadFlagOverrides
    // reversed from this project's earlier, guessed order. Each call is
    // a direct, same-ABI call into Roblox's own bionic-compiled native
    // code, wrapped only in call_trapping_abort()'s abort/trap recovery
    // (see trap_recovery.h), not any ABI-crossing bracket, since
    // Process B and libroblox.so share the same ABI.
    auto* set_asset_path = find_required_symbol<SetAssetPathFn>(
        lib, "Java_com_roblox_client_startup_MainGameActivity_nativeSetAssetPath");
    jstring path_jstring = env.NewStringUTF(asset_path.c_str());
    result.set_asset_path_called = true;
    result.set_asset_path_trapped_abort =
        !call_trapping_abort(set_asset_path, jni_env, nullptr, path_jstring);
    clear_pending_jni_exception(jni_env, "nativeSetAssetPath");

    auto* preload_flags = find_required_symbol<PreloadFlagOverridesFn>(
        lib, "Java_com_roblox_client_startup_MainGameActivity_nativePreloadFlagOverrides");
    jstring flags_json = env.NewStringUTF(flag_overrides.to_wire_format().c_str());
    result.preload_flag_overrides_called = true;
    result.preload_flag_overrides_trapped_abort =
        !call_trapping_abort(preload_flags, jni_env, nullptr, flags_json);
    clear_pending_jni_exception(jni_env, "nativePreloadFlagOverrides");

    return result;
}

InitParamsBootstrapResult run_init_params_bootstrap(FakeJni::Jvm& jvm,
                                                     const stud::linker::LoadedLibrary& lib,
                                                     std::shared_ptr<InitParams> init_params) {
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    InitParamsBootstrapResult result;

    auto* app_bridge_set_init_params = find_required_symbol<AppBridgeSetInitParamsFn>(
        lib, "Java_com_roblox_client_startup_MainGameActivity_nativeAppBridgeSetInitParams");
    jobject init_params_jobject = env.createLocalReference(std::move(init_params));
    result.app_bridge_set_init_params_called = true;
    result.app_bridge_set_init_params_trapped_abort =
        !call_trapping_abort(app_bridge_set_init_params, jni_env, nullptr, init_params_jobject);
    clear_pending_jni_exception(jni_env, "nativeAppBridgeSetInitParams");

    return result;
}

SetDeviceInfoResult run_native_set_device_info(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                                std::shared_ptr<DeviceParams> device_params) {
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    SetDeviceInfoResult result;

    void* addr = lib.find_symbol("Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetDeviceInfo");
    if (addr == nullptr) {
        return result;
    }
    auto* set_device_info = reinterpret_cast<SetDeviceInfoFn>(addr);
    jobject device_params_jobject = env.createLocalReference(std::move(device_params));
    result.called = true;
    result.trapped_abort = !call_trapping_abort(set_device_info, jni_env, nullptr, device_params_jobject);
    clear_pending_jni_exception(jni_env, "nativeSetDeviceInfo");

    return result;
}

BootstrapResult run_bootstrap(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                               const std::string& asset_path, const FlagOverrides& flag_overrides,
                               std::shared_ptr<InitParams> init_params) {
    // Combined convenience wrapper (tests/tools that don't need the split
    // native-callback flow), same real order as the split functions.
    BootstrapResult result;
    auto preload = run_preload_bootstrap(jvm, lib, asset_path, flag_overrides);
    result.set_asset_path_called = preload.set_asset_path_called;
    result.set_asset_path_trapped_abort = preload.set_asset_path_trapped_abort;
    result.preload_flag_overrides_called = preload.preload_flag_overrides_called;
    result.preload_flag_overrides_trapped_abort = preload.preload_flag_overrides_trapped_abort;

    auto init = run_init_params_bootstrap(jvm, lib, std::move(init_params));
    result.app_bridge_set_init_params_called = init.app_bridge_set_init_params_called;
    result.app_bridge_set_init_params_trapped_abort = init.app_bridge_set_init_params_trapped_abort;

    return result;
}

}  // namespace stud::jni_bridge
