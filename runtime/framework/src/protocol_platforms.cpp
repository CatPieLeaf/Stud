#include "stud/protocol_platforms.h"

#include "stud/trap_recovery.h"

#include <cstdio>

namespace stud::jni_bridge {

namespace {
constexpr int kStaticPublicProxy = FakeJni::JMethodID::PUBLIC | FakeJni::JMethodID::STATIC;
}  // namespace

BEGIN_NATIVE_DESCRIPTOR(AppAgeSignalsCoreCppProxyJava)
{ FakeJni::Constructor<AppAgeSignalsCoreCppProxyJava>{} },
{ FakeJni::Constructor<AppAgeSignalsCoreCppProxyJava, FakeJni::JLong>{} },
{ FakeJni::Function<&AppAgeSignalsCoreCppProxyJava::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&AppAgeSignalsCoreCppProxyJava::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(BugReporterCoreCppProxyJava)
{ FakeJni::Constructor<BugReporterCoreCppProxyJava>{} },
{ FakeJni::Constructor<BugReporterCoreCppProxyJava, FakeJni::JLong>{} },
{ FakeJni::Function<&BugReporterCoreCppProxyJava::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&BugReporterCoreCppProxyJava::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(PinShortcutCoreCppProxyJava)
{ FakeJni::Constructor<PinShortcutCoreCppProxyJava>{} },
{ FakeJni::Constructor<PinShortcutCoreCppProxyJava, FakeJni::JLong>{} },
{ FakeJni::Function<&PinShortcutCoreCppProxyJava::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&PinShortcutCoreCppProxyJava::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DeviceDisplayCoreCppProxyJava)
{ FakeJni::Constructor<DeviceDisplayCoreCppProxyJava>{} },
{ FakeJni::Constructor<DeviceDisplayCoreCppProxyJava, FakeJni::JLong>{} },
{ FakeJni::Function<&DeviceDisplayCoreCppProxyJava::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&DeviceDisplayCoreCppProxyJava::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(LocalStorageCoreCppProxyJava)
{ FakeJni::Constructor<LocalStorageCoreCppProxyJava>{} },
{ FakeJni::Constructor<LocalStorageCoreCppProxyJava, FakeJni::JLong>{} },
{ FakeJni::Function<&LocalStorageCoreCppProxyJava::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&LocalStorageCoreCppProxyJava::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DesignFoundationsCoreCppProxyJava)
{ FakeJni::Constructor<DesignFoundationsCoreCppProxyJava>{} },
{ FakeJni::Constructor<DesignFoundationsCoreCppProxyJava, FakeJni::JLong>{} },
{ FakeJni::Function<&DesignFoundationsCoreCppProxyJava::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&DesignFoundationsCoreCppProxyJava::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(AppAgeSignalsCppProxyJava)
{ FakeJni::Constructor<AppAgeSignalsCppProxyJava>{} },
{ FakeJni::Constructor<AppAgeSignalsCppProxyJava, FakeJni::JLong>{} },
{ FakeJni::Function<&AppAgeSignalsCppProxyJava::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&AppAgeSignalsCppProxyJava::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(SystemDialogCppProxyJava)
{ FakeJni::Constructor<SystemDialogCppProxyJava>{} },
{ FakeJni::Constructor<SystemDialogCppProxyJava, FakeJni::JLong>{} },
{ FakeJni::Function<&SystemDialogCppProxyJava::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&SystemDialogCppProxyJava::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(BugReporterCppProxyJava)
{ FakeJni::Constructor<BugReporterCppProxyJava>{} },
{ FakeJni::Constructor<BugReporterCppProxyJava, FakeJni::JLong>{} },
{ FakeJni::Function<&BugReporterCppProxyJava::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&BugReporterCppProxyJava::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DesignFoundationsCppProxyJava)
{ FakeJni::Constructor<DesignFoundationsCppProxyJava>{} },
{ FakeJni::Constructor<DesignFoundationsCppProxyJava, FakeJni::JLong>{} },
{ FakeJni::Function<&DesignFoundationsCppProxyJava::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&DesignFoundationsCppProxyJava::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DeviceDisplayCppProxyJava)
{ FakeJni::Constructor<DeviceDisplayCppProxyJava>{} },
{ FakeJni::Constructor<DeviceDisplayCppProxyJava, FakeJni::JLong>{} },
{ FakeJni::Function<&DeviceDisplayCppProxyJava::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&DeviceDisplayCppProxyJava::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(LocalStorageCppProxyJava)
{ FakeJni::Constructor<LocalStorageCppProxyJava>{} },
{ FakeJni::Constructor<LocalStorageCppProxyJava, FakeJni::JLong>{} },
{ FakeJni::Function<&LocalStorageCppProxyJava::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&LocalStorageCppProxyJava::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(PinShortcutCppProxyJava)
{ FakeJni::Constructor<PinShortcutCppProxyJava>{} },
{ FakeJni::Constructor<PinShortcutCppProxyJava, FakeJni::JLong>{} },
{ FakeJni::Function<&PinShortcutCppProxyJava::nativeDestroy>{}, "nativeDestroy", kStaticPublicProxy },
{ FakeJni::Field<&PinShortcutCppProxyJava::nativeRef>{}, "nativeRef" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(AppAgeSignalsPlatformJava)
{ FakeJni::Function<&AppAgeSignalsPlatformJava::getAgeSignal>{}, "getAgeSignal" },
{ FakeJni::Function<&AppAgeSignalsPlatformJava::isAgeSignalAvailable>{}, "isAgeSignalAvailable" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(BugReporterPlatformJava)
{ FakeJni::Function<&BugReporterPlatformJava::isAvailable>{}, "isAvailable" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(PinShortcutPlatformJava)
{ FakeJni::Function<&PinShortcutPlatformJava::isAvailable>{}, "isAvailable" },
{ FakeJni::Function<&PinShortcutPlatformJava::pinExperience>{}, "pinExperience" },
{ FakeJni::Function<&PinShortcutPlatformJava::getDesiredThumbnailFormat>{}, "getDesiredThumbnailFormat" },
{ FakeJni::Function<&PinShortcutPlatformJava::isRevealPinnedExperienceAvailable>{}, "isRevealPinnedExperienceAvailable" },
{ FakeJni::Function<&PinShortcutPlatformJava::revealPinnedExperience>{}, "revealPinnedExperience" },
{ FakeJni::Function<&PinShortcutPlatformJava::isPinExperienceV2Available>{}, "isPinExperienceV2Available" },
{ FakeJni::Function<&PinShortcutPlatformJava::pinExperienceV2>{}, "pinExperienceV2" },
{ FakeJni::Function<&PinShortcutPlatformJava::shouldShowLuaNotificationOnPinExperienceCompleted>{}, "shouldShowLuaNotificationOnPinExperienceCompleted" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DeviceDisplayCapabilityJava)
{ FakeJni::Function<&DeviceDisplayCapabilityJava::ordinal>{}, "ordinal" },
{ FakeJni::Function<&DeviceDisplayCapabilityJava::values>{}, "values" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DeviceDisplayPlatformJava)
{ FakeJni::Function<&DeviceDisplayPlatformJava::getBrightness>{}, "getBrightness" },
{ FakeJni::Function<&DeviceDisplayPlatformJava::hasCapability>{}, "hasCapability" },
{ FakeJni::Function<&DeviceDisplayPlatformJava::isAvailable>{}, "isAvailable" },
{ FakeJni::Function<&DeviceDisplayPlatformJava::setBrightness>{}, "setBrightness" },
{ FakeJni::Function<&DeviceDisplayPlatformJava::setBrightnessToDefault>{}, "setBrightnessToDefault" },
{ FakeJni::Function<&DeviceDisplayPlatformJava::setKeepAwake>{}, "setKeepAwake" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(LocalStoragePlatformJava)
{ FakeJni::Function<&LocalStoragePlatformJava::deleteCurrentUserValues>{}, "deleteCurrentUserValues" },
{ FakeJni::Function<&LocalStoragePlatformJava::deleteSecureValue>{}, "deleteSecureValue" },
{ FakeJni::Function<&LocalStoragePlatformJava::deleteUserValues>{}, "deleteUserValues" },
{ FakeJni::Function<&LocalStoragePlatformJava::getCurrentUser>{}, "getCurrentUser" },
{ FakeJni::Function<&LocalStoragePlatformJava::getSecureValue>{}, "getSecureValue" },
{ FakeJni::Function<&LocalStoragePlatformJava::getSecureValueForCurrentUser>{}, "getSecureValueForCurrentUser" },
{ FakeJni::Function<&LocalStoragePlatformJava::getSecureValueForUser>{}, "getSecureValueForUser" },
{ FakeJni::Function<&LocalStoragePlatformJava::getUsers>{}, "getUsers" },
{ FakeJni::Function<&LocalStoragePlatformJava::setCurrentUser>{}, "setCurrentUser" },
{ FakeJni::Function<&LocalStoragePlatformJava::setSecureValue>{}, "setSecureValue" },
{ FakeJni::Function<&LocalStoragePlatformJava::setSecureValueForCurrentUser>{}, "setSecureValueForCurrentUser" },
{ FakeJni::Function<&LocalStoragePlatformJava::setSecureValueForUser>{}, "setSecureValueForUser" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DesignTokensJava)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DesignFoundationsPlatformJava)
{ FakeJni::Function<&DesignFoundationsPlatformJava::onTokensCleared>{}, "onTokensCleared" },
{ FakeJni::Function<&DesignFoundationsPlatformJava::onTokensUpdated>{}, "onTokensUpdated" },
END_NATIVE_DESCRIPTOR

namespace {

using SetPlatformImplFn = jobject (*)(JNIEnv*, jclass, jobject);

// Shared helper for every `<X>Core.setPlatformImpl(platform)` call below,
// same find-symbol/call/degrade-gracefully shape repeated 6 times
// otherwise. `called`/`trapped_abort` mirror this file's own existing
// per-protocol result-struct field pair.
void call_set_platform_impl(FakeJni::Env& env, JNIEnv* jni_env, const stud::linker::LoadedLibrary& lib,
                             const char* symbol_name, const char* log_name,
                             std::shared_ptr<FakeJni::JObject> platform, bool& called,
                             bool& trapped_abort) {
    void* addr = lib.find_symbol(symbol_name);
    if (addr == nullptr) {
        std::fprintf(stderr, "stud: protocol_platforms: %s.setPlatformImpl not found, skipping\n",
                      log_name);
        return;
    }
    called = true;
    auto* fn = reinterpret_cast<SetPlatformImplFn>(addr);
    jobject platform_ref = env.createLocalReference(std::move(platform));
    jobject core_result = nullptr;
    trapped_abort = !call_trapping_abort_with_result(fn, core_result, jni_env, nullptr, platform_ref);
    clear_pending_jni_exception(jni_env, log_name);
}

}  // namespace

void register_protocol_platforms(FakeJni::Jvm& jvm) {
    // Real Djinni `$CppProxy` inner classes; see the header. Without
    // these, every setPlatformImpl() below fails with a pending JNI
    // exception and no platform implementation is actually installed.
    jvm.registerClass<AppAgeSignalsCoreCppProxyJava>();
    jvm.registerClass<BugReporterCoreCppProxyJava>();
    jvm.registerClass<PinShortcutCoreCppProxyJava>();
    jvm.registerClass<DeviceDisplayCoreCppProxyJava>();
    jvm.registerClass<LocalStorageCoreCppProxyJava>();
    jvm.registerClass<DesignFoundationsCoreCppProxyJava>();
    jvm.registerClass<AppAgeSignalsCppProxyJava>();
    jvm.registerClass<SystemDialogCppProxyJava>();
    jvm.registerClass<BugReporterCppProxyJava>();
    jvm.registerClass<DesignFoundationsCppProxyJava>();
    jvm.registerClass<DeviceDisplayCppProxyJava>();
    jvm.registerClass<LocalStorageCppProxyJava>();
    jvm.registerClass<PinShortcutCppProxyJava>();

    jvm.registerClass<AppAgeSignalsPlatformJava>();
    jvm.registerClass<BugReporterPlatformJava>();
    jvm.registerClass<PinShortcutPlatformJava>();
    jvm.registerClass<DeviceDisplayCapabilityJava>();
    jvm.registerClass<DeviceDisplayPlatformJava>();
    jvm.registerClass<LocalStoragePlatformJava>();
    jvm.registerClass<DesignTokensJava>();
    jvm.registerClass<DesignFoundationsPlatformJava>();
}

ProtocolPlatformBootstrapResult run_protocol_platforms_bootstrap(
    FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib) {
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    ProtocolPlatformBootstrapResult result;

    call_set_platform_impl(
        env, jni_env, lib,
        "Java_com_roblox_protocols_appagesignalsplatforminterface_generated_"
        "IAppAgeSignalsCore_setPlatformImpl",
        "IAppAgeSignalsCore", std::make_shared<AppAgeSignalsPlatformJava>(),
        result.app_age_signals_set_platform_impl_called,
        result.app_age_signals_set_platform_impl_trapped_abort);

    call_set_platform_impl(
        env, jni_env, lib,
        "Java_com_roblox_protocols_bugreporterplatforminterface_generated_"
        "IBugReporterCore_setPlatformImpl",
        "IBugReporterCore", std::make_shared<BugReporterPlatformJava>(),
        result.bug_reporter_set_platform_impl_called,
        result.bug_reporter_set_platform_impl_trapped_abort);

    call_set_platform_impl(
        env, jni_env, lib,
        "Java_com_roblox_protocols_pinshortcutplatforminterface_generated_"
        "IPinShortcutHandlerCore_setPlatformImpl",
        "IPinShortcutHandlerCore", std::make_shared<PinShortcutPlatformJava>(),
        result.pin_shortcut_set_platform_impl_called,
        result.pin_shortcut_set_platform_impl_trapped_abort);

    call_set_platform_impl(
        env, jni_env, lib,
        "Java_com_roblox_protocols_devicedisplayplatforminterface_generated_"
        "IDeviceDisplayHandlerCore_setPlatformImpl",
        "IDeviceDisplayHandlerCore", std::make_shared<DeviceDisplayPlatformJava>(),
        result.device_display_set_platform_impl_called,
        result.device_display_set_platform_impl_trapped_abort);

    call_set_platform_impl(
        env, jni_env, lib,
        "Java_com_roblox_protocols_localstorageplatforminterface_generated_"
        "ILocalStorageHandlerCore_setPlatformImpl",
        "ILocalStorageHandlerCore", std::make_shared<LocalStoragePlatformJava>(),
        result.local_storage_set_platform_impl_called,
        result.local_storage_set_platform_impl_trapped_abort);

    call_set_platform_impl(
        env, jni_env, lib,
        "Java_com_roblox_protocols_designfoundationsplatforminterface_generated_"
        "IDesignFoundationsCoreListener_setPlatformImpl",
        "IDesignFoundationsCoreListener", std::make_shared<DesignFoundationsPlatformJava>(),
        result.design_foundations_set_platform_impl_called,
        result.design_foundations_set_platform_impl_trapped_abort);

    return result;
}

}  // namespace stud::jni_bridge
