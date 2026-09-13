#pragma once

#include <fake-jni/fake-jni.h>

#include <memory>
#include <string>

// com.roblox.engine.jni.model.DeviceParams, reimplemented as a real
// FakeJni object -- same approach as PlatformParams (see platform_params.h).
//
// Field layout is ground-truth, traced directly from the real Roblox APK's
// app's own device-params builder and the two helper methods it calls
// into (and the two helpers it calls) that mutate the same
// DeviceParams instance by reference. Not copied from any of the app's
// source -- this is a clean-room reimplementation of the observed *shape*.
//
// 16 fields are unconditionally set on a real device; 5 more
// (lowMemoryKiller*Threshold, deviceSku, socModel, testDeviceName) are only
// set under real-device conditions (API level gates, a dev-flag, and a
// Parcel-size check for the memory-killer thresholds that doesn't
// meaningfully apply here). All 21 are included as plain members with
// desktop-appropriate defaults -- Stud's own construction isn't bound by
// the real app's conditional logic, so there's no reason to omit fields
// libroblox.so's native getters might still probe for.
//
// See the engineering notes, milestone M4.

namespace stud::jni_bridge {

class DeviceParams : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/model/DeviceParams")

    std::shared_ptr<FakeJni::JString> osVersion;
    std::shared_ptr<FakeJni::JString> deviceName;
    std::shared_ptr<FakeJni::JString> appVersion;
    std::shared_ptr<FakeJni::JString> country;
    std::shared_ptr<FakeJni::JString> manufacturer;
    FakeJni::JInt deviceTotalMemoryMB = 0;
    std::shared_ptr<FakeJni::JString> displayResolution;
    FakeJni::JInt displayPhysicalWidthPixels = 0;
    FakeJni::JInt displayPhysicalHeightPixels = 0;
    std::shared_ptr<FakeJni::JString> networkType;
    FakeJni::JBoolean isChrome = false;
    std::shared_ptr<FakeJni::JString> appBuildVariant;
    FakeJni::JBoolean cpu64Bit = true;

    // Set by the app's own helper on a real device (ActivityManager.getMemoryClass()/
    // getLargeMemoryClass()/isLowRamDevice()) -- no equivalent concept on
    // desktop Linux, given plausible non-constrained-device defaults.
    FakeJni::JInt memoryClass = 512;
    FakeJni::JInt largeMemoryClass = 512;
    FakeJni::JBoolean isLowRamDevice = false;

    // Set by the app's own helper on a real device, gated behind an SDK-version
    // threshold and an ActivityManager.MemoryInfo Parcel-size check --
    // Android's low-memory-killer thresholds have no desktop Linux
    // equivalent, left at 0 (matches what happens on a real device when
    // the gate doesn't pass).
    FakeJni::JLong lowMemoryKillerBackgroundAppThreshold = 0;
    FakeJni::JLong lowMemoryKillerForegroundAppThreshold = 0;

    // Only set on a real device when Build.VERSION.SDK_INT >= 31
    // (Android 12+). Populated unconditionally here with plausible values.
    std::shared_ptr<FakeJni::JString> deviceSku;
    std::shared_ptr<FakeJni::JString> socModel;

    // Only set on a real device behind a remote dev-flag
    // (a device-test flag). Left null -- Stud isn't a test device.
    std::shared_ptr<FakeJni::JString> testDeviceName;
};

// Builds a DeviceParams reporting plausible desktop Linux values.
// `os_version` should be whatever Android API level Stud reports elsewhere
// (see libc-shim's __system_property_get "ro.build.version.sdk" table --
// keep these consistent if either changes).
std::shared_ptr<DeviceParams> build_desktop_device_params(
    const std::string& os_version, const std::string& device_name, const std::string& app_version,
    const std::string& display_resolution, int display_width_px, int display_height_px,
    int total_memory_mb);

// com.roblox.engine.jni.model.DeviceStaticParams -- real, confirmed against the app's own code
// (the app's own device-params builder,): a real,
// plain `@Keep` POJO, all 8 real fields already covered by DeviceParams
// above (same real Build.* fields, just a smaller, static-only subset
// `NativeGLJavaInterface.setDeviceStaticParams()` receives once at real
// app startup). Separate class (not reusing DeviceParams directly)
// because it's a genuinely different real Java class name/JNI type --
// native code doing `GetObjectClass`/`FindClass` on a `DeviceStaticParams`
// instance needs the real class name to match.
class DeviceStaticParamsStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/model/DeviceStaticParams")

    std::shared_ptr<FakeJni::JString> appBuildVariant;
    std::shared_ptr<FakeJni::JString> appVersion;
    FakeJni::JBoolean cpu64Bit = true;
    std::shared_ptr<FakeJni::JString> deviceName;
    std::shared_ptr<FakeJni::JString> deviceSku;
    std::shared_ptr<FakeJni::JString> manufacturer;
    std::shared_ptr<FakeJni::JString> osVersion;
    std::shared_ptr<FakeJni::JString> socModel;
};

// Builds a DeviceStaticParamsStub from an already-built DeviceParams --
// same real, honest desktop values (see build_desktop_device_params
// above), just the smaller field subset this real class needs. Keeps
// the two objects consistent with each other rather than re-deriving
// separately.
std::shared_ptr<DeviceStaticParamsStub> build_desktop_device_static_params(
    const std::shared_ptr<DeviceParams>& device_params);

}  // namespace stud::jni_bridge
