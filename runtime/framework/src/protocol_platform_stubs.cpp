#include "stud/protocol_platform_stubs.h"

#include "stud/trap_recovery.h"

#include <cstdio>

namespace stud::jni_bridge {

namespace {
constexpr int kStaticPublicProxy = FakeJni::JMethodID::PUBLIC | FakeJni::JMethodID::STATIC;
}  // namespace

BEGIN_NATIVE_DESCRIPTOR(AppAgeSignalsCoreCppProxyStub)
{ FakeJni::Constructor<AppAgeSignalsCoreCppProxyStub>{} },
{ FakeJni::Constructor<AppAgeSignalsCoreCppProxyStub, FakeJni::JLong>{} },
{ FakeJni::Function<&AppAgeSignalsCoreCppProxyStub::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&AppAgeSignalsCoreCppProxyStub::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(BugReporterCoreCppProxyStub)
{ FakeJni::Constructor<BugReporterCoreCppProxyStub>{} },
{ FakeJni::Constructor<BugReporterCoreCppProxyStub, FakeJni::JLong>{} },
{ FakeJni::Function<&BugReporterCoreCppProxyStub::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&BugReporterCoreCppProxyStub::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(PinShortcutCoreCppProxyStub)
{ FakeJni::Constructor<PinShortcutCoreCppProxyStub>{} },
{ FakeJni::Constructor<PinShortcutCoreCppProxyStub, FakeJni::JLong>{} },
{ FakeJni::Function<&PinShortcutCoreCppProxyStub::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&PinShortcutCoreCppProxyStub::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DeviceDisplayCoreCppProxyStub)
{ FakeJni::Constructor<DeviceDisplayCoreCppProxyStub>{} },
{ FakeJni::Constructor<DeviceDisplayCoreCppProxyStub, FakeJni::JLong>{} },
{ FakeJni::Function<&DeviceDisplayCoreCppProxyStub::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&DeviceDisplayCoreCppProxyStub::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(LocalStorageCoreCppProxyStub)
{ FakeJni::Constructor<LocalStorageCoreCppProxyStub>{} },
{ FakeJni::Constructor<LocalStorageCoreCppProxyStub, FakeJni::JLong>{} },
{ FakeJni::Function<&LocalStorageCoreCppProxyStub::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&LocalStorageCoreCppProxyStub::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DesignFoundationsCoreCppProxyStub)
{ FakeJni::Constructor<DesignFoundationsCoreCppProxyStub>{} },
{ FakeJni::Constructor<DesignFoundationsCoreCppProxyStub, FakeJni::JLong>{} },
{ FakeJni::Function<&DesignFoundationsCoreCppProxyStub::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&DesignFoundationsCoreCppProxyStub::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(AppAgeSignalsCppProxyStub)
{ FakeJni::Constructor<AppAgeSignalsCppProxyStub>{} },
{ FakeJni::Constructor<AppAgeSignalsCppProxyStub, FakeJni::JLong>{} },
{ FakeJni::Function<&AppAgeSignalsCppProxyStub::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&AppAgeSignalsCppProxyStub::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(SystemDialogCppProxyStub)
{ FakeJni::Constructor<SystemDialogCppProxyStub>{} },
{ FakeJni::Constructor<SystemDialogCppProxyStub, FakeJni::JLong>{} },
{ FakeJni::Function<&SystemDialogCppProxyStub::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&SystemDialogCppProxyStub::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(BugReporterCppProxyStub)
{ FakeJni::Constructor<BugReporterCppProxyStub>{} },
{ FakeJni::Constructor<BugReporterCppProxyStub, FakeJni::JLong>{} },
{ FakeJni::Function<&BugReporterCppProxyStub::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&BugReporterCppProxyStub::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DesignFoundationsCppProxyStub)
{ FakeJni::Constructor<DesignFoundationsCppProxyStub>{} },
{ FakeJni::Constructor<DesignFoundationsCppProxyStub, FakeJni::JLong>{} },
{ FakeJni::Function<&DesignFoundationsCppProxyStub::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&DesignFoundationsCppProxyStub::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DeviceDisplayCppProxyStub)
{ FakeJni::Constructor<DeviceDisplayCppProxyStub>{} },
{ FakeJni::Constructor<DeviceDisplayCppProxyStub, FakeJni::JLong>{} },
{ FakeJni::Function<&DeviceDisplayCppProxyStub::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&DeviceDisplayCppProxyStub::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(LocalStorageCppProxyStub)
{ FakeJni::Constructor<LocalStorageCppProxyStub>{} },
{ FakeJni::Constructor<LocalStorageCppProxyStub, FakeJni::JLong>{} },
{ FakeJni::Function<&LocalStorageCppProxyStub::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&LocalStorageCppProxyStub::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(PinShortcutCppProxyStub)
{ FakeJni::Constructor<PinShortcutCppProxyStub>{} },
{ FakeJni::Constructor<PinShortcutCppProxyStub, FakeJni::JLong>{} },
{ FakeJni::Function<&PinShortcutCppProxyStub::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&PinShortcutCppProxyStub::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(AppAgeSignalsPlatformStub)
{ FakeJni::Function<&AppAgeSignalsPlatformStub::getAgeSignal>{}, "getAgeSignal" },
{ FakeJni::Function<&AppAgeSignalsPlatformStub::isAgeSignalAvailable>{}, "isAgeSignalAvailable" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(BugReporterPlatformStub)
{ FakeJni::Function<&BugReporterPlatformStub::isAvailable>{}, "isAvailable" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(PinShortcutPlatformStub)
{ FakeJni::Function<&PinShortcutPlatformStub::isAvailable>{}, "isAvailable" },
{ FakeJni::Function<&PinShortcutPlatformStub::pinExperience>{}, "pinExperience" },
{ FakeJni::Function<&PinShortcutPlatformStub::getDesiredThumbnailFormat>{}, "getDesiredThumbnailFormat" },
{ FakeJni::Function<&PinShortcutPlatformStub::isRevealPinnedExperienceAvailable>{}, "isRevealPinnedExperienceAvailable" },
{ FakeJni::Function<&PinShortcutPlatformStub::revealPinnedExperience>{}, "revealPinnedExperience" },
{ FakeJni::Function<&PinShortcutPlatformStub::isPinExperienceV2Available>{}, "isPinExperienceV2Available" },
{ FakeJni::Function<&PinShortcutPlatformStub::pinExperienceV2>{}, "pinExperienceV2" },
{ FakeJni::Function<&PinShortcutPlatformStub::shouldShowLuaNotificationOnPinExperienceCompleted>{}, "shouldShowLuaNotificationOnPinExperienceCompleted" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DeviceDisplayCapabilityStub)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DeviceDisplayPlatformStub)
{ FakeJni::Function<&DeviceDisplayPlatformStub::getBrightness>{}, "getBrightness" },
{ FakeJni::Function<&DeviceDisplayPlatformStub::hasCapability>{}, "hasCapability" },
{ FakeJni::Function<&DeviceDisplayPlatformStub::isAvailable>{}, "isAvailable" },
{ FakeJni::Function<&DeviceDisplayPlatformStub::setBrightness>{}, "setBrightness" },
{ FakeJni::Function<&DeviceDisplayPlatformStub::setBrightnessToDefault>{}, "setBrightnessToDefault" },
{ FakeJni::Function<&DeviceDisplayPlatformStub::setKeepAwake>{}, "setKeepAwake" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(LocalStoragePlatformStub)
{ FakeJni::Function<&LocalStoragePlatformStub::deleteCurrentUserValues>{}, "deleteCurrentUserValues" },
{ FakeJni::Function<&LocalStoragePlatformStub::deleteSecureValue>{}, "deleteSecureValue" },
{ FakeJni::Function<&LocalStoragePlatformStub::deleteUserValues>{}, "deleteUserValues" },
{ FakeJni::Function<&LocalStoragePlatformStub::getCurrentUser>{}, "getCurrentUser" },
{ FakeJni::Function<&LocalStoragePlatformStub::getSecureValue>{}, "getSecureValue" },
{ FakeJni::Function<&LocalStoragePlatformStub::getSecureValueForCurrentUser>{}, "getSecureValueForCurrentUser" },
{ FakeJni::Function<&LocalStoragePlatformStub::getSecureValueForUser>{}, "getSecureValueForUser" },
{ FakeJni::Function<&LocalStoragePlatformStub::getUsers>{}, "getUsers" },
{ FakeJni::Function<&LocalStoragePlatformStub::setCurrentUser>{}, "setCurrentUser" },
{ FakeJni::Function<&LocalStoragePlatformStub::setSecureValue>{}, "setSecureValue" },
{ FakeJni::Function<&LocalStoragePlatformStub::setSecureValueForCurrentUser>{}, "setSecureValueForCurrentUser" },
{ FakeJni::Function<&LocalStoragePlatformStub::setSecureValueForUser>{}, "setSecureValueForUser" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DesignTokensStub)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DesignFoundationsPlatformStub)
{ FakeJni::Function<&DesignFoundationsPlatformStub::onTokensCleared>{}, "onTokensCleared" },
{ FakeJni::Function<&DesignFoundationsPlatformStub::onTokensUpdated>{}, "onTokensUpdated" },
END_NATIVE_DESCRIPTOR

namespace {

using SetPlatformImplFn = jobject (*)(JNIEnv*, jclass, jobject);

// Shared helper for every `<X>Core.setPlatformImpl(stub)` call below --
// same find-symbol/call/degrade-gracefully shape repeated 6 times
// otherwise. `called`/`trapped_abort` mirror this file's own existing
// per-protocol result-struct field pair.
void call_set_platform_impl(FakeJni::Env& env, JNIEnv* jni_env, const stud::linker::LoadedLibrary& lib,
                             const char* symbol_name, const char* log_name,
                             std::shared_ptr<FakeJni::JObject> stub, bool& called,
                             bool& trapped_abort) {
    void* addr = lib.find_symbol(symbol_name);
    if (addr == nullptr) {
        std::fprintf(stderr, "stud: protocol_platform_stubs: %s.setPlatformImpl not found, skipping\n",
                      log_name);
        return;
    }
    called = true;
    auto* fn = reinterpret_cast<SetPlatformImplFn>(addr);
    jobject stub_ref = env.createLocalReference(std::move(stub));
    jobject core_result = nullptr;
    trapped_abort = !call_trapping_abort_with_result(fn, core_result, jni_env, nullptr, stub_ref);
    clear_pending_jni_exception(jni_env, log_name);
}

}  // namespace

void register_protocol_platform_stubs(FakeJni::Jvm& jvm) {
    // Real Djinni `$CppProxy` inner classes -- see the header. Without
    // these, every setPlatformImpl() below fails with a pending JNI
    // exception and no platform implementation is actually installed.
    jvm.registerClass<AppAgeSignalsCoreCppProxyStub>();
    jvm.registerClass<BugReporterCoreCppProxyStub>();
    jvm.registerClass<PinShortcutCoreCppProxyStub>();
    jvm.registerClass<DeviceDisplayCoreCppProxyStub>();
    jvm.registerClass<LocalStorageCoreCppProxyStub>();
    jvm.registerClass<DesignFoundationsCoreCppProxyStub>();
    jvm.registerClass<AppAgeSignalsCppProxyStub>();
    jvm.registerClass<SystemDialogCppProxyStub>();
    jvm.registerClass<BugReporterCppProxyStub>();
    jvm.registerClass<DesignFoundationsCppProxyStub>();
    jvm.registerClass<DeviceDisplayCppProxyStub>();
    jvm.registerClass<LocalStorageCppProxyStub>();
    jvm.registerClass<PinShortcutCppProxyStub>();

    jvm.registerClass<AppAgeSignalsPlatformStub>();
    jvm.registerClass<BugReporterPlatformStub>();
    jvm.registerClass<PinShortcutPlatformStub>();
    jvm.registerClass<DeviceDisplayCapabilityStub>();
    jvm.registerClass<DeviceDisplayPlatformStub>();
    jvm.registerClass<LocalStoragePlatformStub>();
    jvm.registerClass<DesignTokensStub>();
    jvm.registerClass<DesignFoundationsPlatformStub>();
}

ProtocolPlatformBootstrapResult run_protocol_platform_stubs_bootstrap(
    FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib) {
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    ProtocolPlatformBootstrapResult result;

    call_set_platform_impl(
        env, jni_env, lib,
        "Java_com_roblox_protocols_appagesignalsplatforminterface_generated_"
        "IAppAgeSignalsCore_setPlatformImpl",
        "IAppAgeSignalsCore", std::make_shared<AppAgeSignalsPlatformStub>(),
        result.app_age_signals_set_platform_impl_called,
        result.app_age_signals_set_platform_impl_trapped_abort);

    call_set_platform_impl(
        env, jni_env, lib,
        "Java_com_roblox_protocols_bugreporterplatforminterface_generated_"
        "IBugReporterCore_setPlatformImpl",
        "IBugReporterCore", std::make_shared<BugReporterPlatformStub>(),
        result.bug_reporter_set_platform_impl_called,
        result.bug_reporter_set_platform_impl_trapped_abort);

    call_set_platform_impl(
        env, jni_env, lib,
        "Java_com_roblox_protocols_pinshortcutplatforminterface_generated_"
        "IPinShortcutHandlerCore_setPlatformImpl",
        "IPinShortcutHandlerCore", std::make_shared<PinShortcutPlatformStub>(),
        result.pin_shortcut_set_platform_impl_called,
        result.pin_shortcut_set_platform_impl_trapped_abort);

    call_set_platform_impl(
        env, jni_env, lib,
        "Java_com_roblox_protocols_devicedisplayplatforminterface_generated_"
        "IDeviceDisplayHandlerCore_setPlatformImpl",
        "IDeviceDisplayHandlerCore", std::make_shared<DeviceDisplayPlatformStub>(),
        result.device_display_set_platform_impl_called,
        result.device_display_set_platform_impl_trapped_abort);

    call_set_platform_impl(
        env, jni_env, lib,
        "Java_com_roblox_protocols_localstorageplatforminterface_generated_"
        "ILocalStorageHandlerCore_setPlatformImpl",
        "ILocalStorageHandlerCore", std::make_shared<LocalStoragePlatformStub>(),
        result.local_storage_set_platform_impl_called,
        result.local_storage_set_platform_impl_trapped_abort);

    call_set_platform_impl(
        env, jni_env, lib,
        "Java_com_roblox_protocols_designfoundationsplatforminterface_generated_"
        "IDesignFoundationsCoreListener_setPlatformImpl",
        "IDesignFoundationsCoreListener", std::make_shared<DesignFoundationsPlatformStub>(),
        result.design_foundations_set_platform_impl_called,
        result.design_foundations_set_platform_impl_trapped_abort);

    return result;
}

}  // namespace stud::jni_bridge
