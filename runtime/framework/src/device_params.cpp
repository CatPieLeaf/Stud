#include "stud/device_params.h"

namespace stud::jni_bridge {

BEGIN_NATIVE_DESCRIPTOR(DeviceParams)
{ FakeJni::Field<&DeviceParams::osVersion>{}, "osVersion" },
{ FakeJni::Field<&DeviceParams::deviceName>{}, "deviceName" },
{ FakeJni::Field<&DeviceParams::appVersion>{}, "appVersion" },
{ FakeJni::Field<&DeviceParams::country>{}, "country" },
{ FakeJni::Field<&DeviceParams::manufacturer>{}, "manufacturer" },
{ FakeJni::Field<&DeviceParams::deviceTotalMemoryMB>{}, "deviceTotalMemoryMB" },
{ FakeJni::Field<&DeviceParams::displayResolution>{}, "displayResolution" },
{ FakeJni::Field<&DeviceParams::displayPhysicalWidthPixels>{}, "displayPhysicalWidthPixels" },
{ FakeJni::Field<&DeviceParams::displayPhysicalHeightPixels>{}, "displayPhysicalHeightPixels" },
{ FakeJni::Field<&DeviceParams::networkType>{}, "networkType" },
{ FakeJni::Field<&DeviceParams::isChrome>{}, "isChrome" },
{ FakeJni::Field<&DeviceParams::appBuildVariant>{}, "appBuildVariant" },
{ FakeJni::Field<&DeviceParams::cpu64Bit>{}, "cpu64Bit" },
{ FakeJni::Field<&DeviceParams::memoryClass>{}, "memoryClass" },
{ FakeJni::Field<&DeviceParams::largeMemoryClass>{}, "largeMemoryClass" },
{ FakeJni::Field<&DeviceParams::isLowRamDevice>{}, "isLowRamDevice" },
{ FakeJni::Field<&DeviceParams::lowMemoryKillerBackgroundAppThreshold>{}, "lowMemoryKillerBackgroundAppThreshold" },
{ FakeJni::Field<&DeviceParams::lowMemoryKillerForegroundAppThreshold>{}, "lowMemoryKillerForegroundAppThreshold" },
{ FakeJni::Field<&DeviceParams::deviceSku>{}, "deviceSku" },
{ FakeJni::Field<&DeviceParams::socModel>{}, "socModel" },
{ FakeJni::Field<&DeviceParams::testDeviceName>{}, "testDeviceName" },
END_NATIVE_DESCRIPTOR

std::shared_ptr<DeviceParams> build_desktop_device_params(const std::string& os_version,
                                                            const std::string& device_name,
                                                            const std::string& app_version,
                                                            const std::string& display_resolution,
                                                            int display_width_px, int display_height_px,
                                                            int total_memory_mb) {
    auto params = std::make_shared<DeviceParams>();
    params->osVersion = std::make_shared<FakeJni::JString>(os_version);
    params->deviceName = std::make_shared<FakeJni::JString>(device_name);
    params->appVersion = std::make_shared<FakeJni::JString>(app_version);
    params->country = std::make_shared<FakeJni::JString>("");
    params->manufacturer = std::make_shared<FakeJni::JString>("Stud");
    params->deviceTotalMemoryMB = total_memory_mb;
    params->displayResolution = std::make_shared<FakeJni::JString>(display_resolution);
    params->displayPhysicalWidthPixels = display_width_px;
    params->displayPhysicalHeightPixels = display_height_px;
    params->networkType = std::make_shared<FakeJni::JString>("WIFI");
    // Matches the ChromeOS the User-Agent already reports. On a device
    // this comes from hasSystemFeature("org.chromium.arc.device_management")
    // the same real ARC check, and it was left false here, so the
    // agent and the params described different machines.
    params->isChrome = true;
    params->appBuildVariant = std::make_shared<FakeJni::JString>("release");
    params->cpu64Bit = true;
    params->memoryClass = 512;
    params->largeMemoryClass = 512;
    params->isLowRamDevice = false;
    params->lowMemoryKillerBackgroundAppThreshold = 0;
    params->lowMemoryKillerForegroundAppThreshold = 0;
    params->deviceSku = std::make_shared<FakeJni::JString>("unknown");
    params->socModel = std::make_shared<FakeJni::JString>("unknown");
    return params;
}

BEGIN_NATIVE_DESCRIPTOR(DeviceStaticParamsStub)
{ FakeJni::Field<&DeviceStaticParamsStub::appBuildVariant>{}, "appBuildVariant" },
{ FakeJni::Field<&DeviceStaticParamsStub::appVersion>{}, "appVersion" },
{ FakeJni::Field<&DeviceStaticParamsStub::cpu64Bit>{}, "cpu64Bit" },
{ FakeJni::Field<&DeviceStaticParamsStub::deviceName>{}, "deviceName" },
{ FakeJni::Field<&DeviceStaticParamsStub::deviceSku>{}, "deviceSku" },
{ FakeJni::Field<&DeviceStaticParamsStub::manufacturer>{}, "manufacturer" },
{ FakeJni::Field<&DeviceStaticParamsStub::osVersion>{}, "osVersion" },
{ FakeJni::Field<&DeviceStaticParamsStub::socModel>{}, "socModel" },
END_NATIVE_DESCRIPTOR

std::shared_ptr<DeviceStaticParamsStub> build_desktop_device_static_params(
    const std::shared_ptr<DeviceParams>& device_params) {
    auto params = std::make_shared<DeviceStaticParamsStub>();
    params->appBuildVariant = device_params->appBuildVariant;
    params->appVersion = device_params->appVersion;
    params->cpu64Bit = device_params->cpu64Bit;
    params->deviceName = device_params->deviceName;
    params->deviceSku = device_params->deviceSku;
    params->manufacturer = device_params->manufacturer;
    params->osVersion = device_params->osVersion;
    params->socModel = device_params->socModel;
    return params;
}

}  // namespace stud::jni_bridge
